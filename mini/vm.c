/* ===========================================================================
 *  BoltOS Mini  -  mini/vm.c
 *
 *  A machine for Firefox, inside the Pi. Firefox needs Linux under it -- a
 *  kernel, a file system, a compositor -- so BoltOS runs one, as a guest:
 *  the A53 has the virtualisation extensions, U-Boot starts Mini at EL2,
 *  and start.S leaves EL2's vectors pointing at arch/hyp.S before it drops
 *  to EL1. BoltOS stays what it was, an EL1 kernel with one main loop on
 *  core 0; cores 1-3 each run one of the guest's CPUs.
 *
 *  The guest's machine is a small cousin of QEMU's `virt` board, so a
 *  stock arm64 kernel knows all of it:
 *
 *      0x08000000  GIC distributor     made up here (gicd_access)
 *      0x08010000  GIC CPU interface   the real GIC-400's virtual one
 *      0x09000000  PL011 UART          the console, into BoltOS's log
 *      0x09010000  PL031 RTC           BoltOS's clock
 *      0x0A000000  virtio-mmio x 8     disks, screen, input, network (vdev.c)
 *      0x40000000  RAM                 BoltOS's two big windows, 894 MiB
 *
 *  Stage-2 translation maps the RAM and the virtual CPU interface; every
 *  other address the guest touches faults to EL2, comes back here as an
 *  exit, and is answered as the device would.
 *
 *  Interrupts: a physical one taken while the guest runs stops it (HCR
 *  IMO). Its virtual timer fires on its own core as PPI 27, and goes back
 *  in through a list register linked to the physical interrupt, so the
 *  guest's own EOI ends both -- the GIC-400's virtualisation, as Linux's
 *  KVM uses it. Everything else -- the devices' SPIs, the guest's IPIs
 *  between its CPUs -- is a bit set here and a physical SGI to the core
 *  that should look at it.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "vm.h"
#include "vmctx.h"
#include "store.h"
#include "pkg.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

uint64_t hyp_call(uint64_t fn, uint64_t arg);
extern uint32_t hyp_present;
extern uint64_t hyp_fault[3];

int  vdev_setup(const vm_config *c);
void vdev_stop(void);
void vdev_poll(void);
int  vdev_count(void);
int  vdev_mmio(int dev, uint32_t off, int write, int size, uint64_t *val);
int  vdev_dt(int dev, const char **compatible);

#define G_GICD    0x08000000u
#define G_GICC    0x08010000u
#define G_UART    0x09000000u
#define G_RTC     0x09010000u
#define G_RAM     0x40000000ull
#define G_RAM0    MINI_HIGH_SIZE               /* IPA 0x40000000.. -> the high window */
#define G_RAM1    MINI_LOW_SIZE                /* then the low one                    */
#define G_KERNEL  (G_RAM + 0x200000)
#define G_DTB     (G_RAM + G_RAM0 - 0x200000)
#define NSPI      64
#define KICK_SGI  7
#define MAXV      3

/* GICv2 */
#define GICD_ICPENDR  0x280
#define GICD_ICACTIVER 0x380
#define GICD_SGIR     0xF00
#define GICC_CTLR     0x00
#define GICC_PMR      0x04
#define GICC_BPR      0x08
#define GICC_IAR      0x0C
#define GICC_EOIR     0x10
#define GICC_DIR      0x1000
#define GICH_HCR      0x00
#define GICH_VTR      0x04
#define GICH_VMCR     0x08
#define GICH_ELRSR0   0x30
#define GICH_APR      0xF0
#define GICH_LR       0x100
#define LR_HW         (1u << 31)
#define LR_PEND       (1u << 28)
#define LR_ACT        (2u << 28)
#define LR_STATE      (3u << 28)

/* HCR_EL2 for the guest: stage 2, interrupts and SErrors to EL2, SMC and
 * set/way cache operations trapped, broadcast TLB/cache maintenance, AArch64 */
#define HCR_GUEST ((1ull << 0) | (1ull << 1) | (1ull << 3) | (1ull << 4) | (1ull << 5) | (1ull << 9) | \
                   (1ull << 10) | (1ull << 19) | (1ull << 22) | (1ull << 31))
/* VTCR_EL2: 32-bit IPA, start at level 1, 4K, write-back, inner shareable */
#define VTCR_GUEST 0x80003560ull

#define S2_BLOCK  1ull
#define S2_TABLE  3ull
#define S2_PAGE   3ull
#define S2_NORMAL (0xFull << 2)
#define S2_DEVICE (0x1ull << 2)
#define S2_RW     (3ull << 6)
#define S2_ISH    (3ull << 8)
#define S2_AF     (1ull << 10)
#define S2_XN     (1ull << 54)

enum { V_OFF, V_START, V_ON };

typedef struct {
    uint64_t *x;                        /* the context (vmctx.h)                     */
    void     *mem;
    int       id, pcpu;
    volatile int state;
    volatile uint64_t entry, arg;
    volatile uint32_t ppi_en;           /* the guest's view: SGIs and PPIs enabled   */
    uint8_t   prio[32];
    volatile uint16_t sgi[16];          /* SGIs waiting: a bit per sending vCPU      */
    volatile int timer;                 /* its virtual timer fired: put it in an LR  */
    volatile uint64_t exits, irqs, mmios, calls;
} vcpu_t;

static struct {
    int       state;
    char      msg[200];
    vm_config cfg;
    char      kname[48], rname[48], hname[48], cmd[400];
    int       nv;
    vcpu_t    v[MAXV];
    volatile int stop;
    volatile int done[MAXV];
    uint64_t  kick_us;                  /* stopping: the last kick to the CPUs still in */
    uint64_t *s2;                       /* level 1, then the tables under it         */
    uint64_t  vttbr;
    uintptr_t gicd, gicc, gich, gicv;
    int       nlr;
    /* the guest's distributor */
    volatile uint32_t dctlr;
    volatile uint32_t spi_en[NSPI / 32], spi_pend[NSPI / 32];
    uint8_t   spi_prio[NSPI], spi_tgt[NSPI];
    uint32_t  spi_cfg[NSPI / 16];
    /* the console */
    volatile uint32_t uart_imsc;
    char      con[16384];
    volatile uint32_t con_wr;
    uint32_t  con_rd;
    char      line[240]; int linen;
    char      tail[3072]; int tailn;
    char      rx[1024];                 /* typed in (development): /vm?sh=          */
    volatile uint32_t rx_wr, rx_rd;
    int       debug;                    /* boltos.debug=1: a shell on the console   */
    int       up;                       /* its screen has shown something           */
    /* loading */
    const store_entry_t *ke;
    uint64_t  koff, flushed;
    uint64_t  t0;
} VM;

/* ---- memory ---------------------------------------------------------------------------- */
static uint64_t ipa_pa(uint64_t ipa) {
    if (ipa >= G_RAM && ipa < G_RAM + G_RAM0) return MINI_HIGH_BASE + (ipa - G_RAM);
    if (ipa >= G_RAM + G_RAM0 && ipa < G_RAM + G_RAM0 + G_RAM1) return MINI_LOW_BASE + (ipa - G_RAM - G_RAM0);
    return 0;
}

void *vm_ptr(uint64_t ipa, uint64_t len) {
    uint64_t pa = ipa_pa(ipa);
    if (!pa || !len || ipa_pa(ipa + len - 1) != pa + len - 1) return 0;   /* not across the seam */
    return (void *)(uintptr_t)pa;
}

static void cache_clean(uint64_t pa, uint64_t len) {
    for (uint64_t a = pa & ~63ull; a < pa + len; a += 64) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
}
static void cache_flush(uint64_t pa, uint64_t len) {
    for (uint64_t a = pa & ~63ull; a < pa + len; a += 64) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
}

/* EL2's own identity map (hyp.S HYP_INIT): RAM write-back as EL1 sees it,
 * minus TF-A's first 2 MiB, so EL2 and BoltOS share the caches. */
static uint64_t hyp_l1[512] __attribute__((aligned(4096)));
static uint64_t hyp_l2[512] __attribute__((aligned(4096)));

static void hyp_tables(void) {
    const uint64_t dev = 1 | 0x40 | 0x400 | (1ull << 54);
    hyp_l1[0] = 0x00000000ull | dev;
    hyp_l1[1] = (uint64_t)(uintptr_t)hyp_l2 | 3;
    hyp_l1[2] = 0x80000000ull | dev;
    hyp_l1[3] = 0xC0000000ull | dev;
    for (int i = 0; i < 512; i++) {
        uint64_t pa = 0x40000000ull + (uint64_t)i * 0x200000ull;
        uint64_t attr = 1;
        if ((pa >= MINI_DMA_BASE && pa < MINI_DMA_BASE + MINI_DMA_SIZE) || (pa >= MINI_LOG_BASE && pa < MINI_LOG_BASE + MINI_LOG_SIZE)) attr = 2;
        hyp_l2[i] = i == 0 ? 0 : pa | 1 | (attr << 2) | 0x40 | 0x300 | 0x400;
    }
    __asm__ volatile("dsb ish" ::: "memory");
}

/* Stage 2: the guest's addresses to the Pi's. */
static int build_s2(void) {
    uint64_t *t = kmalloc(5 * 4096 + 4096);
    if (!t) return -1;
    t = (uint64_t *)(((uintptr_t)t + 4095) & ~(uintptr_t)4095);
    memset(t, 0, 4 * 4096);
    uint64_t *l1 = t, *l2lo = t + 512, *l2ram = t + 1024, *l3gic = t + 1536;
    l1[0] = (uint64_t)(uintptr_t)l2lo | S2_TABLE;
    l1[1] = (uint64_t)(uintptr_t)l2ram | S2_TABLE;
    l2lo[G_GICD >> 21] = (uint64_t)(uintptr_t)l3gic | S2_TABLE;
    for (int i = 0; i < 2; i++)                                       /* GICC = the real GICV */
        l3gic[((G_GICC - G_GICD) >> 12) + i] = (VM.gicv + (uint64_t)i * 4096) | S2_PAGE | S2_DEVICE | S2_RW | S2_AF | S2_XN;
    for (int i = 0; i < 512; i++) {
        uint64_t pa = ipa_pa(G_RAM + (uint64_t)i * 0x200000ull);
        if (pa) l2ram[i] = pa | S2_BLOCK | S2_NORMAL | S2_RW | S2_ISH | S2_AF;
    }
    __asm__ volatile("dsb ish" ::: "memory");
    VM.s2 = l1;
    VM.vttbr = (uint64_t)(uintptr_t)l1 | (1ull << 48);                /* VMID 1 */
    return 0;
}

/* ---- a device tree for the guest --------------------------------------------------------- */
typedef struct { uint8_t *st; int sn; char *str; int strn; } fdtw;

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void f_u32(fdtw *f, uint32_t v) { be32(f->st + f->sn, v); f->sn += 4; }
static void f_pad(fdtw *f) { while (f->sn & 3) f->st[f->sn++] = 0; }
static void f_begin(fdtw *f, const char *name) {
    f_u32(f, 1);
    int n = (int)strlen(name) + 1;
    memcpy(f->st + f->sn, name, (size_t)n); f->sn += n; f_pad(f);
}
static void f_end(fdtw *f) { f_u32(f, 2); }
static int f_strofs(fdtw *f, const char *name) {
    for (int o = 0; o < f->strn; o += (int)strlen(f->str + o) + 1) if (!strcmp(f->str + o, name)) return o;
    int o = f->strn, n = (int)strlen(name) + 1;
    memcpy(f->str + o, name, (size_t)n); f->strn += n;
    return o;
}
static void f_prop(fdtw *f, const char *name, const void *data, int len) {
    f_u32(f, 3); f_u32(f, (uint32_t)len); f_u32(f, (uint32_t)f_strofs(f, name));
    if (len) memcpy(f->st + f->sn, data, (size_t)len);
    f->sn += len; f_pad(f);
}
static void f_cells(fdtw *f, const char *name, const uint32_t *c, int n) {
    uint8_t b[64];
    for (int i = 0; i < n; i++) be32(b + 4 * i, c[i]);
    f_prop(f, name, b, 4 * n);
}
static void f_u(fdtw *f, const char *name, uint32_t v) { f_cells(f, name, &v, 1); }
static void f_s(fdtw *f, const char *name, const char *s) { f_prop(f, name, s, (int)strlen(s) + 1); }
static void f_reg(fdtw *f, uint64_t a, uint64_t s) {
    uint32_t c[4] = { (uint32_t)(a >> 32), (uint32_t)a, (uint32_t)(s >> 32), (uint32_t)s };
    f_cells(f, "reg", c, 4);
}

static int build_dtb(uint8_t *out, int cap) {
    fdtw f = { kmalloc(16384), 0, kmalloc(2048), 0 };
    if (!f.st || !f.str) { if (f.st) kfree(f.st); if (f.str) kfree(f.str); return -1; }
    char node[64];
    f_begin(&f, "");
    f_u(&f, "#address-cells", 2); f_u(&f, "#size-cells", 2);
    f_s(&f, "compatible", "linux,dummy-virt");
    f_s(&f, "model", "BoltOS Mini: Firefox");
    f_u(&f, "interrupt-parent", 1);

    f_begin(&f, "chosen");
    f_s(&f, "bootargs", VM.cmd);
    f_s(&f, "stdout-path", "/pl011@9000000");
    f_end(&f);

    snprintf(node, sizeof node, "memory@%llx", (unsigned long long)G_RAM);
    f_begin(&f, node);
    f_s(&f, "device_type", "memory");
    f_reg(&f, G_RAM, G_RAM0 + G_RAM1);
    f_end(&f);

    f_begin(&f, "cpus");
    f_u(&f, "#address-cells", 1); f_u(&f, "#size-cells", 0);
    for (int i = 0; i < VM.nv; i++) {
        snprintf(node, sizeof node, "cpu@%d", i);
        f_begin(&f, node);
        f_s(&f, "device_type", "cpu");
        f_s(&f, "compatible", "arm,cortex-a53");
        f_u(&f, "reg", (uint32_t)i);
        f_s(&f, "enable-method", "psci");
        f_end(&f);
    }
    f_end(&f);

    f_begin(&f, "psci");
    f_prop(&f, "compatible", "arm,psci-1.0\0arm,psci-0.2", 26);
    f_s(&f, "method", "hvc");
    f_end(&f);

    uint32_t ppi = (((1u << VM.nv) - 1) << 8) | 4;                  /* CPUs, level high */
    f_begin(&f, "timer");
    f_s(&f, "compatible", "arm,armv8-timer");
    uint32_t ti[12] = { 1, 13, ppi, 1, 14, ppi, 1, 11, ppi, 1, 10, ppi };
    f_cells(&f, "interrupts", ti, 12);
    f_prop(&f, "always-on", 0, 0);
    f_end(&f);

    f_begin(&f, "intc@8000000");
    f_s(&f, "compatible", "arm,cortex-a15-gic");
    f_u(&f, "#interrupt-cells", 3);
    f_prop(&f, "interrupt-controller", 0, 0);
    uint32_t gr[8] = { 0, G_GICD, 0, 0x1000, 0, G_GICC, 0, 0x2000 };
    f_cells(&f, "reg", gr, 8);
    f_u(&f, "phandle", 1);
    f_end(&f);

    f_begin(&f, "apb-pclk");
    f_s(&f, "compatible", "fixed-clock");
    f_u(&f, "#clock-cells", 0);
    f_u(&f, "clock-frequency", 24000000);
    f_s(&f, "clock-output-names", "clk24mhz");
    f_u(&f, "phandle", 2);
    f_end(&f);

    f_begin(&f, "pl011@9000000");
    f_prop(&f, "compatible", "arm,pl011\0arm,primecell", 24);
    f_reg(&f, G_UART, 0x1000);
    uint32_t ui[3] = { 0, VM_SPI_UART, 4 };
    f_cells(&f, "interrupts", ui, 3);
    uint32_t ck[2] = { 2, 2 };
    f_cells(&f, "clocks", ck, 2);
    f_prop(&f, "clock-names", "uartclk\0apb_pclk", 17);
    f_end(&f);

    f_begin(&f, "pl031@9010000");
    f_prop(&f, "compatible", "arm,pl031\0arm,primecell", 24);
    f_reg(&f, G_RTC, 0x1000);
    uint32_t ri[3] = { 0, VM_SPI_RTC, 4 };
    f_cells(&f, "interrupts", ri, 3);
    f_u(&f, "clocks", 2);
    f_s(&f, "clock-names", "apb_pclk");
    f_end(&f);

    for (int i = 0; i < vdev_count(); i++) {
        uint64_t a = VM_VIRTIO_BASE + (uint64_t)i * VM_VIRTIO_SIZE;
        snprintf(node, sizeof node, "virtio_mmio@%llx", (unsigned long long)a);
        f_begin(&f, node);
        f_s(&f, "compatible", "virtio,mmio");
        f_reg(&f, a, VM_VIRTIO_SIZE);
        uint32_t vi[3] = { 0, (uint32_t)(VM_SPI_VIRTIO + i), 1 };
        f_cells(&f, "interrupts", vi, 3);
        f_prop(&f, "dma-coherent", 0, 0);
        f_end(&f);
    }
    f_end(&f);
    f_u32(&f, 9);

    int off_rsv = 40, off_st = off_rsv + 16, off_str = off_st + f.sn, total = off_str + f.strn;
    int r = -1;
    if (total <= cap) {
        memset(out, 0, (size_t)total);
        be32(out + 0, 0xD00DFEED); be32(out + 4, (uint32_t)total); be32(out + 8, (uint32_t)off_st);
        be32(out + 12, (uint32_t)off_str); be32(out + 16, (uint32_t)off_rsv); be32(out + 20, 17); be32(out + 24, 16);
        be32(out + 28, 0); be32(out + 32, (uint32_t)f.strn); be32(out + 36, (uint32_t)f.sn);
        memcpy(out + off_st, f.st, (size_t)f.sn);
        memcpy(out + off_str, f.str, (size_t)f.strn);
        r = total;
    }
    kfree(f.st); kfree(f.str);
    return r;
}

/* ---- the virtual GIC ------------------------------------------------------------------------ */
static void kick(vcpu_t *t, int from_pcpu) {
    if (t->pcpu == from_pcpu) return;                  /* it looks before it goes back in */
    __asm__ volatile("dsb ish" ::: "memory");
    w32(VM.gicd + GICD_SGIR, (1u << (16 + t->pcpu)) | KICK_SGI);
}

static int this_pcpu(void) { uint64_t m; __asm__ volatile("mrs %0, mpidr_el1" : "=r"(m)); return (int)(m & 0xFF); }

static vcpu_t *spi_target(int spi) {
    uint8_t t = VM.spi_tgt[spi];
    for (int i = 0; i < VM.nv; i++) if (t >> i & 1) return &VM.v[i];
    return &VM.v[0];
}

void vm_irq(int spi) {
    if (VM.state != VM_RUNNING || spi < 0 || spi >= NSPI) return;
    __atomic_fetch_or(&VM.spi_pend[spi / 32], 1u << (spi % 32), __ATOMIC_RELEASE);
    kick(spi_target(spi), this_pcpu());
}

static uint32_t lr_prio(uint8_t p) { return (uint32_t)(p >> 3) << 23; }

/* Before going into the guest: what's waiting for it, into the list registers. */
static void vgic_flush(vcpu_t *v) {
    int n = VM.nlr;
    uint32_t free = r32(VM.gich + GICH_ELRSR0) & ((1u << n) - 1);
    uint32_t lr[16];
    for (int i = 0; i < n; i++) lr[i] = (free >> i & 1) ? 0 : r32(VM.gich + GICH_LR + 4 * (uintptr_t)i);
    int more = 0;
#define PUT(val) ({ int ok_ = 0; for (int i_ = 0; i_ < n; i_++) if (free >> i_ & 1) { free &= ~(1u << i_); lr[i_] = (val); \
                    w32(VM.gich + GICH_LR + 4 * (uintptr_t)i_, lr[i_]); ok_ = 1; break; } ok_; })
    if (v->timer) {
        if (PUT(LR_HW | (27u << 10) | 27u | LR_PEND | lr_prio(v->prio[27]))) v->timer = 0; else more = 1;
    }
    for (int id = 0; id < 16; id++) {
        uint32_t src = __atomic_load_n(&v->sgi[id], __ATOMIC_ACQUIRE);
        while (src) {
            int s = __builtin_ctz(src);
            int there = 0;
            for (int i = 0; i < n; i++) if (lr[i] && (lr[i] & 0x3FF) == (uint32_t)id && ((lr[i] >> 10) & 7) == (uint32_t)s) there = 1;
            if (there) { src &= src - 1; continue; }             /* the same one is still in: later */
            if (!PUT(((uint32_t)s << 10) | (uint32_t)id | LR_PEND | lr_prio(v->prio[id]))) { more = 1; break; }
            __atomic_fetch_and(&v->sgi[id], (uint16_t)~(1u << s), __ATOMIC_RELAXED);
            src &= src - 1;
        }
    }
    if (VM.dctlr & 1)
        for (int w = 0; w < NSPI / 32; w++) {
            uint32_t p = __atomic_load_n(&VM.spi_pend[w], __ATOMIC_ACQUIRE) & VM.spi_en[w];
            while (p) {
                int b = __builtin_ctz(p); p &= p - 1;
                int spi = w * 32 + b;
                if (spi_target(spi) != v) continue;
                uint32_t id = (uint32_t)(32 + spi);
                int k = -1;
                for (int i = 0; i < n; i++) if (lr[i] && (lr[i] & 0x3FF) == id) k = i;
                if (k >= 0) {                                        /* still in: one more edge merges */
                    if ((lr[k] & LR_STATE) == LR_ACT) { lr[k] |= LR_PEND; w32(VM.gich + GICH_LR + 4 * (uintptr_t)k, lr[k]); }
                } else if (!PUT(id | LR_PEND | lr_prio(VM.spi_prio[spi]))) { more = 1; break; }
                __atomic_fetch_and(&VM.spi_pend[w], ~(1u << b), __ATOMIC_RELAXED);
            }
        }
#undef PUT
    w32(VM.gich + GICH_HCR, 1u | (more ? 2u : 0u));             /* En, and UIE if some wait */
}

/* A physical interrupt stopped the guest. */
static void host_irq(vcpu_t *v) {
    for (;;) {
        uint32_t iar = r32(VM.gicc + GICC_IAR), id = iar & 0x3FF;
        if (id >= 1020) break;
        /* The maintenance interrupt: the list registers ran low with more
         * waiting (UIE). It stays asserted while UIE is set, so it would be
         * taken again forever; vgic_flush refills them and sets it again. */
        if (id == 25) w32(VM.gich + GICH_HCR, 1);
        w32(VM.gicc + GICC_EOIR, iar);                          /* priority drop only    */
        if (id == 27) v->timer = 1;                              /* the guest's EOI ends it */
        else w32(VM.gicc + GICC_DIR, iar);                       /* a kick, maintenance   */
    }
}

static void sgi_send(vcpu_t *from, uint32_t val) {
    int id = (int)(val & 15), filt = (int)(val >> 24 & 3);
    uint32_t list = val >> 16 & 0xFF;
    for (int t = 0; t < VM.nv; t++) {
        int to = filt == 0 ? (int)(list >> t & 1) : filt == 1 ? t != from->id : t == from->id;
        if (!to) continue;
        __atomic_fetch_or(&VM.v[t].sgi[id], (uint16_t)(1u << from->id), __ATOMIC_RELEASE);
        kick(&VM.v[t], from->pcpu);
    }
}

static void gicd_access(vcpu_t *v, uint32_t off, int wr, int size, uint64_t *val) {
    uint64_t r = 0;
    if (off >= 0x400 && off < 0x400 + 32 + NSPI) {                         /* IPRIORITYR: bytes */
        for (int i = 0; i < size; i++) {
            int id = (int)(off - 0x400) + i;
            uint8_t *p = id < 32 ? &v->prio[id] : id < 32 + NSPI ? &VM.spi_prio[id - 32] : 0;
            if (!p) continue;
            if (wr) *p = (uint8_t)(*val >> (8 * i)); else r |= (uint64_t)*p << (8 * i);
        }
        if (!wr) *val = r;
        return;
    }
    if (off >= 0x800 && off < 0x800 + 32 + NSPI) {                         /* ITARGETSR: bytes */
        for (int i = 0; i < size; i++) {
            int id = (int)(off - 0x800) + i;
            if (id < 32) r |= (uint64_t)(1u << v->id) << (8 * i);
            else if (id < 32 + NSPI) {
                if (wr) { uint8_t t = (uint8_t)(*val >> (8 * i)); VM.spi_tgt[id - 32] = t ? t : 1; }
                else r |= (uint64_t)VM.spi_tgt[id - 32] << (8 * i);
            }
        }
        if (!wr) *val = r;
        if (wr) for (int t = 0; t < VM.nv; t++) kick(&VM.v[t], v->pcpu);
        return;
    }
    uint32_t w = (uint32_t)*val;
    int n;
    if (off == 0x000) { if (wr) VM.dctlr = w & 1; else r = VM.dctlr; }
    else if (off == 0x004) r = (uint32_t)((32 + NSPI) / 32 - 1) | (uint32_t)(VM.nv - 1) << 5;
    else if (off == 0x008) r = 0x0200043B;
    else if (off >= 0x100 && off < 0x200) {                                  /* I[SC]ENABLER */
        int set = off < 0x180;
        n = (int)((off & 0x7F) / 4);
        if (n == 0) {
            if (wr) { if (set) v->ppi_en |= w; else v->ppi_en &= ~w; }
            r = v->ppi_en | 0xFFFF;
        } else if (n <= NSPI / 32) {
            if (wr) {
                if (set) __atomic_fetch_or(&VM.spi_en[n - 1], w, __ATOMIC_RELEASE);
                else __atomic_fetch_and(&VM.spi_en[n - 1], ~w, __ATOMIC_RELEASE);
                for (int t = 0; t < VM.nv; t++) kick(&VM.v[t], v->pcpu);
            }
            r = VM.spi_en[n - 1];
        }
    } else if (off >= 0x200 && off < 0x300) {                                /* I[SC]PENDR */
        int set = off < 0x280;
        n = (int)((off & 0x7F) / 4);
        if (n >= 1 && n <= NSPI / 32) {
            if (wr) {
                if (set) { __atomic_fetch_or(&VM.spi_pend[n - 1], w, __ATOMIC_RELEASE); for (int t = 0; t < VM.nv; t++) kick(&VM.v[t], v->pcpu); }
                else __atomic_fetch_and(&VM.spi_pend[n - 1], ~w, __ATOMIC_RELEASE);
            }
            r = VM.spi_pend[n - 1];
        }
    } else if (off >= 0xC00 && off < 0xD00) {                                /* ICFGR */
        n = (int)((off - 0xC00) / 4);
        if (n == 0) r = 0xAAAAAAAA;
        else if (n >= 2 && n - 2 < NSPI / 16) { if (wr) VM.spi_cfg[n - 2] = w; r = VM.spi_cfg[n - 2]; }
    } else if (off == GICD_SGIR) { if (wr) sgi_send(v, w); }
    else if (off == 0xFE8) r = 0x20;
    if (!wr) *val = r;
}

/* ---- the console and the clock ------------------------------------------------------------------- */
/* What the UART's interrupt lines say: TX always (its FIFO is always
 * empty), RX and RX-timeout while something typed in is waiting. */
static uint32_t uart_ris(void) {
    return 0x20 | (__atomic_load_n(&VM.rx_wr, __ATOMIC_ACQUIRE) != VM.rx_rd ? 0x50 : 0);
}

static void uart_access(uint32_t off, int wr, uint64_t *val) {
    static const uint8_t id[8] = { 0x11, 0x10, 0x14, 0x00, 0x0D, 0xF0, 0x05, 0xB1 };
    if (wr) {
        if (off == 0x00) {
            uint32_t i = __atomic_fetch_add(&VM.con_wr, 1, __ATOMIC_RELAXED);
            VM.con[i % sizeof VM.con] = (char)*val;
            if (VM.uart_imsc & 0x20) vm_irq(VM_SPI_UART);
        } else if (off == 0x38) {
            VM.uart_imsc = (uint32_t)*val;
            if (uart_ris() & VM.uart_imsc) vm_irq(VM_SPI_UART);
        }
        return;
    }
    uint64_t r = 0;
    if (off == 0x00) {                                              /* DR: the next typed byte */
        uint32_t rd = VM.rx_rd;
        if (rd != __atomic_load_n(&VM.rx_wr, __ATOMIC_ACQUIRE)) {
            r = (uint8_t)VM.rx[rd % sizeof VM.rx];
            __atomic_store_n(&VM.rx_rd, rd + 1, __ATOMIC_RELEASE);
        }
    }
    else if (off == 0x18) r = 0x80 | (uart_ris() & 0x10 ? 0 : 0x10); /* TXFE; RXFE when nothing waits */
    else if (off == 0x38) r = VM.uart_imsc;
    else if (off == 0x3C) r = uart_ris();
    else if (off == 0x40) r = uart_ris() & VM.uart_imsc;
    else if (off == 0x30) r = 0x301;
    else if (off >= 0xFE0) r = id[(off - 0xFE0) / 4];
    *val = r;
}

static void rtc_access(uint32_t off, int wr, uint64_t *val) {
    static const uint8_t id[8] = { 0x31, 0x10, 0x04, 0x00, 0x0D, 0xF0, 0x05, 0xB1 };
    if (wr) return;
    uint64_t r = 0;
    if (off == 0x00) r = clock_unix();
    else if (off == 0x0C) r = 1;
    else if (off >= 0xFE0) r = id[(off - 0xFE0) / 4];
    *val = r;
}

/* ---- the guest's exits ---------------------------------------------------------------------------- */
static void halt(vcpu_t *v, const char *why) {
    if (!VM.stop) {
        snprintf(VM.msg, sizeof VM.msg, "%s (CPU %d at %llx)", why, v->id, (unsigned long long)v->x[CTX_PC]);
        VM.stop = 2;
        __asm__ volatile("dsb ish\nsev" ::: "memory");
    }
}

static void mmio(vcpu_t *v, uint64_t ipa, int wr, int size, uint64_t *val) {
    if (ipa >= G_GICD && ipa < G_GICD + 0x1000) gicd_access(v, (uint32_t)(ipa - G_GICD), wr, size, val);
    else if (ipa >= G_UART && ipa < G_UART + 0x1000) uart_access((uint32_t)(ipa - G_UART), wr, val);
    else if (ipa >= G_RTC && ipa < G_RTC + 0x1000) rtc_access((uint32_t)(ipa - G_RTC), wr, val);
    else if (ipa >= VM_VIRTIO_BASE && ipa < VM_VIRTIO_BASE + VM_VIRTIO_N * VM_VIRTIO_SIZE) {
        int d = (int)((ipa - VM_VIRTIO_BASE) / VM_VIRTIO_SIZE);
        if (d >= vdev_count() || vdev_mmio(d, (uint32_t)((ipa - VM_VIRTIO_BASE) % VM_VIRTIO_SIZE), wr, size, val) < 0)
            if (!wr) *val = 0;
    } else if (!wr) *val = 0;                                        /* nothing there: reads 0 */
}

static void data_abort(vcpu_t *v, uint64_t esr) {
    uint64_t *x = v->x;
    if (!(esr >> 24 & 1) || (esr >> 7 & 1)) { halt(v, "The guest touched memory BoltOS doesn't give it"); return; }
    int size = 1 << (esr >> 22 & 3), srt = (int)(esr >> 16 & 31), wr = (int)(esr >> 6 & 1);
    uint64_t ipa = ((x[CTX_HPFAR] >> 4) << 12) | (x[CTX_FAR] & 0xFFF);
    uint64_t mask = size == 8 ? ~0ull : (1ull << (8 * size)) - 1, val = 0;
    if (wr) val = (srt == 31 ? 0 : x[srt]) & mask;
    mmio(v, ipa, wr, size, &val);
    if (!wr && srt != 31) {
        val &= mask;
        if ((esr >> 21 & 1) && size < 8 && (val >> (8 * size - 1) & 1)) val |= ~mask;
        if (!(esr >> 15 & 1)) val &= 0xFFFFFFFFull;
        x[srt] = val;
    }
    x[CTX_PC] += 4;
    v->mmios++;
}

static int64_t psci(vcpu_t *v) {
    uint64_t *x = v->x;
    uint32_t fn = (uint32_t)x[0];
    switch (fn) {
    case 0x84000000: return 0x10000;                                 /* PSCI 1.0           */
    case 0x84000001: case 0xC4000001: return 0;                      /* CPU_SUSPEND: a WFI */
    case 0x84000002:                                                 /* CPU_OFF            */
        __atomic_store_n(&v->state, V_OFF, __ATOMIC_RELEASE);
        return 0;
    case 0x84000003: case 0xC4000003: {                              /* CPU_ON             */
        int t = (int)(x[1] & 0xFF);
        if (t >= VM.nv) return -2;
        vcpu_t *c = &VM.v[t];
        if (c->state != V_OFF) return -4;
        c->entry = x[2]; c->arg = x[3];
        __atomic_store_n(&c->state, V_START, __ATOMIC_RELEASE);
        __asm__ volatile("dsb ish\nsev" ::: "memory");
        return 0;
    }
    case 0x84000004: case 0xC4000004: {                              /* AFFINITY_INFO      */
        int t = (int)(x[1] & 0xFF);
        return t < VM.nv && VM.v[t].state != V_OFF ? 0 : 1;
    }
    case 0x84000006: return 2;                                       /* MIGRATE_INFO_TYPE  */
    case 0x84000008: case 0x84000009:                                /* SYSTEM_OFF, _RESET */
        snprintf(VM.msg, sizeof VM.msg, "%s", fn == 0x84000008 ? "Firefox's machine turned itself off" : "Firefox's machine restarted");
        VM.stop = 1;
        __asm__ volatile("dsb ish\nsev" ::: "memory");
        return 0;
    case 0x8400000A: {                                               /* PSCI_FEATURES      */
        uint32_t q = (uint32_t)x[1];
        return q == 0x84000000 || q == 0x84000002 || q == 0x84000003 || q == 0xC4000003 || q == 0x84000004 ||
               q == 0xC4000004 || q == 0x84000008 || q == 0x84000009 || q == 0x8400000A || q == 0x80000000 ? 0 : -1;
    }
    case 0x80000000: return 0x10001;                                 /* SMCCC 1.1          */
    default: return -1;
    }
}

static void guest_trap(vcpu_t *v) {
    uint64_t *x = v->x, esr = x[CTX_ESR];
    switch (esr >> 26 & 0x3F) {
    case 0x16: v->calls++; x[0] = (uint64_t)psci(v); break;           /* HVC: PC is past it */
    case 0x17: v->calls++; x[0] = (uint64_t)psci(v); x[CTX_PC] += 4; break;   /* SMC     */
    case 0x24: data_abort(v, esr); break;
    case 0x18: case 0x01: x[CTX_PC] += 4; break;                      /* set/way ops, WFx   */
    default: {
        char w[80]; snprintf(w, sizeof w, "The guest stopped on something unexpected (ESR %llx)", (unsigned long long)esr);
        halt(v, w);
    }
    }
}

/* ---- one virtual CPU, on its own core ------------------------------------------------------------ */
static void reset_regs(vcpu_t *v) {
    uint64_t *x = v->x;
    memset(x, 0, CTX_WORDS * 8);
    x[0] = v->arg;
    x[CTX_PC] = v->entry;
    x[CTX_PSTATE] = 0x3C5;                                     /* EL1h, DAIF masked        */
    x[CTX_SYS + 0] = 0x30D00800;                               /* SCTLR_EL1: MMU off       */
    x[CTX_HCR] = HCR_GUEST;
    x[CTX_VTTBR] = VM.vttbr;
    x[CTX_VTCR] = VTCR_GUEST;
    x[CTX_VMPIDR] = 0x80000000ull | (uint64_t)v->id;
    x[CTX_CNTHCTL] = 3;
}

/* Each core's GIC state is its own (the PPI registers are banked), and a
 * guest started again lands on the same cores: anything the last run left
 * behind has to go. Its virtual timer may still be ACTIVE -- it went in
 * hardware-linked, and a tick taken as the machine stopped never reached a
 * list register, so no guest EOI ever ends it -- and that core then gets no
 * timer at all. And Linux parks its other CPUs inside the stop IPI's
 * handler, so that SGI's priority stays set in GICH_APR into the next run. */
static void pcpu_gic_clean(void) {
    w32(VM.gicd + GICD_ICACTIVER, (1u << 25) | (1u << 27));   /* maintenance, vtimer      */
    w32(VM.gicd + GICD_ICPENDR, (1u << 25) | (1u << 27));
    w32(VM.gich + GICH_APR, 0);
}

static void pcpu_gic_on(void) {
    __asm__ volatile("msr cntv_ctl_el0, xzr\nisb" ::: "memory");
    pcpu_gic_clean();
    w32(VM.gich + GICH_VMCR, 0);                                /* the guest sets its own   */
    for (int i = 0; i < 32; i += 4) w32(VM.gicd + 0x400 + (uintptr_t)i, 0xA0A0A0A0u);
    w32(VM.gicd + 0x100, (1u << 25) | (1u << 27) | 0xFFFFu);   /* maintenance, vtimer, SGIs */
    w32(VM.gicc + GICC_PMR, 0xF0);
    w32(VM.gicc + GICC_BPR, 0);
    w32(VM.gicc + GICC_CTLR, 1u | (1u << 9));                  /* EOI drops; DIR ends      */
    for (int i = 0; i < VM.nlr; i++) w32(VM.gich + GICH_LR + 4 * (uintptr_t)i, 0);
    w32(VM.gich + GICH_HCR, 1);
}

static void pcpu_gic_off(void) {
    __asm__ volatile("msr cntv_ctl_el0, xzr\nisb" ::: "memory");
    for (int i = 0; i < VM.nlr; i++) {
        uint32_t lr = r32(VM.gich + GICH_LR + 4 * (uintptr_t)i);
        w32(VM.gich + GICH_LR + 4 * (uintptr_t)i, 0);
        if ((lr & LR_HW) && (lr & LR_STATE)) w32(VM.gicc + GICC_DIR, (lr >> 10) & 0x3FF);   /* still active */
    }
    w32(VM.gich + GICH_HCR, 0);
    for (;;) {                                                   /* anything left waiting */
        uint32_t iar = r32(VM.gicc + GICC_IAR);
        if ((iar & 0x3FF) >= 1020) break;
        w32(VM.gicc + GICC_EOIR, iar); w32(VM.gicc + GICC_DIR, iar);
    }
    w32(VM.gicd + 0x180, (1u << 25) | (1u << 27));
    pcpu_gic_clean();
    w32(VM.gicc + GICC_CTLR, 0);
}

static void vcpu_thread(void *arg) {
    vcpu_t *v = arg;
    v->pcpu = this_pcpu();
    hyp_call(HYP_INIT, (uint64_t)(uintptr_t)hyp_l1);
    pcpu_gic_on();
    while (!VM.stop) {
        int st = __atomic_load_n(&v->state, __ATOMIC_ACQUIRE);
        if (st == V_START) { reset_regs(v); __atomic_store_n(&v->state, V_ON, __ATOMIC_RELEASE); continue; }
        if (st != V_ON) { __asm__ volatile("wfe"); continue; }
        vgic_flush(v);
        uint64_t kind = hyp_call(HYP_RUN, (uint64_t)(uintptr_t)v->x);
        v->exits++;
        if (kind == EXIT_IRQ) { v->irqs++; host_irq(v); }
        else if (kind == EXIT_SYNC) guest_trap(v);
        else halt(v, kind == EXIT_SERROR ? "The guest's machine hit a hardware error" : "An FIQ reached the guest");
    }
    pcpu_gic_off();
    __atomic_store_n(&VM.done[v->id], 1, __ATOMIC_RELEASE);
}

/* ---- starting and stopping -------------------------------------------------------------------------- */
static void release_cb(void) {
    snprintf(VM.msg, sizeof VM.msg, "Something else needed Firefox's memory");
    vm_stop();
}

int vm_supported(void) {
    static int known = -1;
    if (known < 0) known = hyp_present && hyp_call(HYP_PING, 0) == 0x4B56;
    return known;
}

static int fail(const char *fmt, const char *a) {
    snprintf(VM.msg, sizeof VM.msg, fmt, a ? a : "");
    kprintf("[vm] %s\n", VM.msg);
    if (bigmem_owner(0) && !strcmp(bigmem_owner(0), "Firefox")) bigmem_free(0);
    if (bigmem_owner(1) && !strcmp(bigmem_owner(1), "Firefox")) bigmem_free(1);
    VM.state = VM_FAILED;
    return -1;
}

int vm_start(const vm_config *c) {
    if (VM.state == VM_LOADING || VM.state == VM_RUNNING) return 0;
    if (!vm_supported()) return fail("This machine started BoltOS without its hypervisor level (EL2)", 0);
    VM.cfg = *c;
    snprintf(VM.kname, sizeof VM.kname, "%s", c->kernel);
    snprintf(VM.rname, sizeof VM.rname, "%s", c->root ? c->root : "");
    snprintf(VM.hname, sizeof VM.hname, "%s", c->home ? c->home : "");
    VM.cfg.kernel = VM.kname; VM.cfg.root = VM.rname; VM.cfg.home = VM.hname;
    VM.nv = c->vcpus < 1 ? 1 : c->vcpus > MAXV ? MAXV : c->vcpus;
    if (store_open(0) != 0 || !(VM.ke = store_find(VM.kname)) || !(VM.ke->flags & STORE_DONE))
        return fail("Firefox's kernel (%s) isn't on the card", VM.kname);
    const char *o0 = bigmem_owner(0), *o1 = bigmem_owner(1);
    if (o0 && strcmp(o0, "Firefox")) return fail("%s is using the memory Firefox needs", o0);
    if (o1 && strcmp(o1, "Firefox")) return fail("%s is using the memory Firefox needs", o1);
    if (!o0 && !bigmem_claim(0, "Firefox", release_cb)) return fail("The memory Firefox needs is in use", 0);
    if (!o1 && !bigmem_claim(1, "Firefox", release_cb)) return fail("The memory Firefox needs is in use", 0);
    if (g_plat == PLAT_H618) { VM.gicd = 0x03021000; VM.gicc = 0x03022000; VM.gich = 0x03024000; VM.gicv = 0x03026000; }
    else                     { VM.gicd = 0x08000000; VM.gicc = 0x08010000; VM.gich = 0x08030000; VM.gicv = 0x08040000; }
    VM.nlr = (int)(r32(VM.gich + GICH_VTR) & 0x3F) + 1;
    if (VM.nlr > 16) VM.nlr = 16;
    if (!VM.s2 && build_s2() < 0) return fail("Out of memory", 0);
    hyp_tables();
    if (vdev_setup(&VM.cfg) < 0) return fail("Firefox's disks aren't ready", 0);
    VM.dctlr = 0;
    memset((void *)VM.spi_en, 0, sizeof VM.spi_en); memset((void *)VM.spi_pend, 0, sizeof VM.spi_pend);
    memset(VM.spi_prio, 0xA0, sizeof VM.spi_prio); memset(VM.spi_tgt, 1, sizeof VM.spi_tgt);
    VM.uart_imsc = 0; VM.con_wr = VM.con_rd = 0; VM.linen = 0; VM.tailn = 0; VM.rx_wr = VM.rx_rd = 0;
    char dns[16] = "1.1.1.1";
    if (inet_dns_server()) net_fmt_ip(inet_dns_server(), dns);
    snprintf(VM.cmd, sizeof VM.cmd, "console=ttyAMA0 earlycon=pl011,mmio32,0x%x root=/dev/vda rootfstype=squashfs ro "
             "init=/sbin/boltinit boltos.dns=%s boltos.tz=%d loglevel=4%s %s", G_UART, dns, clock_tz_minutes(),
             VM.debug ? " boltos.debug=1" : "", c->cmdline ? c->cmdline : "");
    VM.koff = 0; VM.flushed = 0; VM.stop = 0; VM.up = 0;
    VM.t0 = now_ms();
    VM.state = VM_LOADING;
    snprintf(VM.msg, sizeof VM.msg, "Starting Firefox's machine\x85");
    kprintf("[vm] starting: %d CPUs, %lu MiB, kernel %s (%lu KiB)\n", VM.nv, (unsigned long)((G_RAM0 + G_RAM1) >> 20),
            VM.kname, (unsigned long)(VM.ke->size >> 10));
    return 0;
}

static void launch(void) {
    uint8_t *dtb = vm_ptr(G_DTB, 65536);
    int n = build_dtb(dtb, 65536);
    if (n < 0) { fail("The guest's device tree didn't fit", 0); return; }
    cache_clean((uint64_t)(uintptr_t)dtb, (uint64_t)n);
    cache_clean(ipa_pa(G_KERNEL), VM.ke->size);
    hyp_call(HYP_TLBI, VM.vttbr);
    __asm__ volatile("ic ialluis\ndsb ish\nisb" ::: "memory");
    for (int i = 0; i < VM.nv; i++) {
        vcpu_t *v = &VM.v[i];
        if (!v->mem) v->mem = kmalloc(CTX_WORDS * 8 + 64);
        if (!v->mem) { fail("Out of memory", 0); return; }
        uint64_t *x = (uint64_t *)(((uintptr_t)v->mem + 63) & ~(uintptr_t)63);
        uint8_t keep_mem = 0; (void)keep_mem;
        void *m = v->mem;
        memset(v, 0, sizeof *v);
        v->mem = m; v->x = x; v->id = i;
        memset(v->prio, 0xA0, sizeof v->prio);
        v->state = V_OFF;
        VM.done[i] = 0;
    }
    VM.v[0].entry = G_KERNEL; VM.v[0].arg = G_DTB;
    VM.v[0].state = V_START;
    VM.state = VM_RUNNING;
    for (int i = 0; i < VM.nv; i++)
        if (cpu_start(i + 1, vcpu_thread, &VM.v[i]) != 0) {
            VM.stop = 2;
            snprintf(VM.msg, sizeof VM.msg, "Core %d is busy with something else", i + 1);
            VM.done[i] = 1;
        }
    snprintf(VM.msg, sizeof VM.msg, "Firefox's machine is starting");
    kprintf("[vm] running: kernel at %llx, device tree at %llx (%d bytes), %d list registers\n",
            (unsigned long long)G_KERNEL, (unsigned long long)G_DTB, n, VM.nlr);
}

void vm_stop(void) {
    if (VM.state == VM_RUNNING) {
        if (!VM.stop) VM.stop = 1;
        __asm__ volatile("dsb ish\nsev" ::: "memory");
        uint64_t end = now_ms() + 3000;
        for (int i = 0; i < VM.nv; i++) {
            /* a vCPU in the guest comes out at the next interrupt: kick it */
            while (!VM.done[i] && now_ms() < end) {
                w32(VM.gicd + GICD_SGIR, (1u << (16 + i + 1)) | KICK_SGI);
                delay_us(200);
            }
        }
        for (int i = 0; i < VM.nv; i++) while (core_busy(i + 1) && now_ms() < end) __asm__ volatile("yield");
    }
    vdev_stop();
    if (VM.state == VM_LOADING || VM.state == VM_RUNNING) VM.state = VM_OFF;
    if (bigmem_owner(0) && !strcmp(bigmem_owner(0), "Firefox")) bigmem_free(0);
    if (bigmem_owner(1) && !strcmp(bigmem_owner(1), "Firefox")) bigmem_free(1);
}

int vm_state(void) { return VM.state; }
const char *vm_message(void) { return VM.msg; }

/* The guest's console, a line at a time into BoltOS's log. */
static void drain_console(void) {
    uint32_t wr = __atomic_load_n(&VM.con_wr, __ATOMIC_ACQUIRE);
    if (wr - VM.con_rd > sizeof VM.con) VM.con_rd = wr - (uint32_t)sizeof VM.con;
    while (VM.con_rd != wr) {
        char c = VM.con[VM.con_rd++ % sizeof VM.con];
        if (VM.tailn == (int)sizeof VM.tail) { memmove(VM.tail, VM.tail + 1024, sizeof VM.tail - 1024); VM.tailn -= 1024; }
        if (c != '\r') VM.tail[VM.tailn++] = c;
        if (c == '\n' || VM.linen == (int)sizeof VM.line - 1) {
            VM.line[VM.linen] = 0;
            if (VM.linen) kprintf("[guest] %s\n", VM.line);
            VM.linen = 0;
        } else if (c != '\r') VM.line[VM.linen++] = c;
    }
}

void vm_poll(void) {
    if (fxget_poll()) shell_request_frame();
    if (VM.state == VM_LOADING) {
        /* First every cache line of the guest's RAM out to memory: the guest
         * starts with its MMU off, and a dirty line of whatever used this RAM
         * before (a model, a video) mustn't land on top of what it writes. */
        uint64_t total = G_RAM0 + G_RAM1;
        if (VM.flushed < total) {
            uint64_t n = 64ull << 20;
            if (n > total - VM.flushed) n = total - VM.flushed;
            cache_flush(ipa_pa(G_RAM + VM.flushed), n);
            VM.flushed += n;
            return;
        }
        if (VM.koff < VM.ke->size) {                                 /* then the kernel, a MiB a turn */
            uint32_t n = VM.ke->size - VM.koff > (1u << 20) ? (1u << 20) : (uint32_t)(VM.ke->size - VM.koff);
            n = (n + 511) & ~511u;                                   /* the card reads sectors */
            if (store_read(VM.ke, VM.koff, vm_ptr(G_KERNEL + VM.koff, n), n) < 0) { fail("Firefox's kernel couldn't be read from the card", 0); return; }
            VM.koff += n;
            return;
        }
        launch();
        return;
    }
    if (VM.state != VM_RUNNING) return;
    drain_console();
    vdev_poll();
    if (!VM.up && vgpu_size(0, 0)) {
        VM.up = 1;
        snprintf(VM.msg, sizeof VM.msg, "Firefox's machine is up");
        kprintf("[vm] its screen is up, %lu ms after the start\n", (unsigned long)(now_ms() - VM.t0));
    }
    if (VM.stop) {
        int all = 1;
        for (int i = 0; i < VM.nv; i++) all &= VM.done[i];
        if (!all && now_us() - VM.kick_us >= 500) {
            /* A CPU Linux parked with its interrupts masked only leaves the
             * guest at its core's next physical interrupt: send it one. */
            VM.kick_us = now_us();
            __asm__ volatile("dsb ish" ::: "memory");
            for (int i = 0; i < VM.nv; i++)
                if (!VM.done[i]) w32(VM.gicd + GICD_SGIR, (1u << (16 + i + 1)) | KICK_SGI);
        }
        if (all) {
            int bad = VM.stop == 2;
            drain_console();
            vm_stop();
            if (bad) VM.state = VM_FAILED;
            kprintf("[vm] stopped: %s\n", VM.msg);
            if (hyp_fault[0]) kprintf("[vm] EL2 fault: esr %llx elr %llx far %llx\n", (unsigned long long)hyp_fault[0],
                                      (unsigned long long)hyp_fault[1], (unsigned long long)hyp_fault[2]);
        }
    }
}

/* ---- the dev page ---------------------------------------------------------------------------------- */
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : 0; }

int vm_dev(const char *q, char *out, int cap) {
    const char *sh = strstr(q, "sh=");
    if (sh) {                                   /* a line for the guest's shell (boltos.debug=1) */
        for (sh += 3;; sh++) {
            char ch = *sh;
            if (!ch || ch == '&') ch = '\n';
            else if (ch == '+') ch = ' ';
            else if (ch == '%' && sh[1] && sh[2]) { ch = (char)(hexv(sh[1]) << 4 | hexv(sh[2])); sh += 2; }
            uint32_t w = VM.rx_wr;
            if (w - __atomic_load_n(&VM.rx_rd, __ATOMIC_ACQUIRE) < sizeof VM.rx) {
                VM.rx[w % sizeof VM.rx] = ch;
                __atomic_store_n(&VM.rx_wr, w + 1, __ATOMIC_RELEASE);
            }
            if (ch == '\n') break;
        }
        if (VM.state == VM_RUNNING && (uart_ris() & VM.uart_imsc)) vm_irq(VM_SPI_UART);
    } else if (strstr(q, "debug")) VM.debug = !strstr(q, "debug=0");
    else if (strstr(q, "start")) {
        int h = scr_h - (28 * scr_h + 300) / 600 - 1;           /* the Browser's area (shell.c) */
        vm_config c = { "fx/Image", "fx/root.sqfs", "fx/home.img", 256ull << 20, 3, scr_w, h, 0 };
        vm_start(&c);
    } else if (strstr(q, "stop")) vm_stop();
    static const char *const st[] = { "failed", "off", "loading", "running" };
    int o = snprintf(out, (size_t)cap, "vm: %s, EL2 %s%s; %s\n", st[VM.state + 1], vm_supported() ? "yes" : "no",
                     VM.debug ? ", debug shell" : "", VM.msg);
    for (int i = 0; i < VM.nv; i++) {
        vcpu_t *v = &VM.v[i];
        o += snprintf(out + o, (size_t)(cap - o), "cpu %d (core %d): %s, %llu exits, %llu irqs, %llu mmio, %llu calls, pc %llx\n",
                      i, v->pcpu, v->state == V_ON ? "on" : v->state == V_START ? "starting" : "off",
                      (unsigned long long)v->exits, (unsigned long long)v->irqs, (unsigned long long)v->mmios,
                      (unsigned long long)v->calls, v->x ? (unsigned long long)v->x[CTX_PC] : 0ull);
    }
    o += vdev_debug(out + o, cap - o);
    if (strstr(q, "con")) {
        int n = VM.tailn < cap - o - 1 ? VM.tailn : cap - o - 1;
        memcpy(out + o, VM.tail + VM.tailn - n, (size_t)n); o += n; out[o] = 0;
    }
    return o;
}
