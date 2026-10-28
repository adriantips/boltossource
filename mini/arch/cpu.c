/* ===========================================================================
 *  BoltOS Mini  -  mini/arch/cpu.c
 *
 *  The AArch64 machine: the identity map, exceptions, the GIC and the timer.
 *
 *  mmu_init runs before .data has been touched by anything but the loader
 *  and before the MMU is on, so this file is built with -mstrict-align: with
 *  the MMU off all memory is Device memory, and an unaligned access faults.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kprintf.h"

/* ---- the identity map ------------------------------------------------------
 *  4 KiB granule, a 32-bit address space (T0SZ = 32), so a walk starts at
 *  level 1 and needs four entries:
 *      0x00000000  1 GiB  Device-nGnRE   every peripheral on both machines
 *      0x40000000  1 GiB  -> level 2     RAM, in 2 MiB blocks
 *  The first 2 MiB of RAM is TF-A's on the Orange Pi and stays unmapped, so
 *  nothing -- not even a speculative fetch -- touches it.                    */
#define ATTR_DEV   0                      /* MAIR index                       */
#define ATTR_WB    1
#define ATTR_NC    2
#define PTE_BLOCK  1ull
#define PTE_TABLE  3ull
#define PTE_AF     (1ull << 10)
#define PTE_ISH    (3ull << 8)
#define PTE_PXN    (1ull << 53)
#define PTE_UXN    (1ull << 54)
#define PTE_ATTR(i) ((uint64_t)(i) << 2)

static uint64_t l1[512] __attribute__((aligned(4096)));
static uint64_t l2_ram[512] __attribute__((aligned(4096)));

void mmu_init(void) {
    l1[0] = 0x00000000ull | PTE_BLOCK | PTE_AF | PTE_ATTR(ATTR_DEV) | PTE_PXN | PTE_UXN;
    l1[1] = (uint64_t)(uintptr_t)l2_ram | PTE_TABLE;
    for (int i = 0; i < 512; i++) {
        uint64_t pa = 0x40000000ull + (uint64_t)i * 0x200000ull;
        int attr = ATTR_WB;
        if (pa >= MINI_DMA_BASE && pa < MINI_DMA_BASE + MINI_DMA_SIZE) attr = ATTR_NC;
        if (pa >= MINI_LOG_BASE && pa < MINI_LOG_BASE + MINI_LOG_SIZE) attr = ATTR_NC;
        l2_ram[i] = (i == 0) ? 0 : (pa | PTE_BLOCK | PTE_AF | PTE_ISH | PTE_ATTR(attr));
    }

    uint64_t mair = (0x04ull << (8 * ATTR_DEV)) |        /* Device-nGnRE     */
                    (0xFFull << (8 * ATTR_WB))  |        /* Normal WB RWA    */
                    (0x44ull << (8 * ATTR_NC));          /* Normal NC        */
    uint64_t tcr  = 32ull                 /* T0SZ: 4 GiB                     */
                  | (1ull << 8)           /* IRGN0 WBWA                      */
                  | (1ull << 10)          /* ORGN0 WBWA                      */
                  | (3ull << 12)          /* SH0 inner                       */
                  | (0ull << 14)          /* TG0 4K                          */
                  | (1ull << 23);         /* EPD1: no TTBR1 walks            */
    __asm__ volatile(
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "dsb ish\n"
        "tlbi vmalle1\n"
        "dsb ish\n"
        "ic iallu\n"
        "isb\n"
        :: "r"(mair), "r"(tcr), "r"((uint64_t)(uintptr_t)l1) : "memory");
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ull << 0) | (1ull << 2) | (1ull << 12);   /* M, C, I          */
    sctlr &= ~((1ull << 1) | (1ull << 19));              /* no A, no WXN     */
    __asm__ volatile("msr sctlr_el1, %0\nisb" :: "r"(sctlr) : "memory");
}

/* ---- the uncached pool ------------------------------------------------------ */
static uint32_t dma_used;
void *dma_alloc(uint32_t size, uint32_t align) {
    if (align < 64) align = 64;
    uint32_t at = (dma_used + align - 1) & ~(align - 1);
    if (at + size > MINI_DMA_SIZE) panic("dma pool exhausted");
    dma_used = at + size;
    uint8_t *p = (uint8_t *)(uintptr_t)(MINI_DMA_BASE + at);
    for (uint32_t i = 0; i < size; i++) p[i] = 0;
    return p;
}

/* ---- exceptions --------------------------------------------------------------- */
struct frame {
    uint64_t x[31];
    uint64_t elr, spsr, fpsr, fpcr, pad;
};

void irq_dispatch(void);

static const char *ec_name(uint32_t ec) {
    switch (ec) {
    case 0x00: return "unknown instruction";
    case 0x07: return "FP/SIMD trapped";
    case 0x15: return "SVC";
    case 0x21: return "instruction abort";
    case 0x22: return "PC alignment";
    case 0x25: return "data abort";
    case 0x26: return "SP alignment";
    case 0x2F: return "SError";
    case 0x3C: return "BRK";
    default:   return "exception";
    }
}

/* The main loop says it came round (main.c); if it hasn't for six seconds,
 * the timer interrupt says where the CPU is, every six seconds after. */
volatile uint64_t g_loop_ms;

void trap(struct frame *f, uint64_t kind) {
    if (kind == 1) {
        irq_dispatch();
        static uint64_t told;
        uint64_t t = now_ms();
        if (g_loop_ms && t - g_loop_ms > 6000 && t - told > 6000) {
            told = t;
            kprintf("[stall] the main loop hasn't run for %lu ms: pc %lx lr %lx\n",
                    (unsigned long)(t - g_loop_ms), f->elr, f->x[30]);
        }
        return;
    }
    uint64_t esr, far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    uint32_t ec = (uint32_t)(esr >> 26) & 0x3F;
    kprintf("\n*** %s (kind %lu, ESR %lx: %s)\n", kind == 3 ? "SError" : "CPU fault",
            kind, esr, ec_name(ec));
    kprintf("    pc  %lx   far %lx   spsr %lx\n", f->elr, far, f->spsr);
    kprintf("    lr  %lx   sp  %lx\n", f->x[30], (uint64_t)(uintptr_t)(f + 1));
    for (int i = 0; i < 30; i += 3)
        kprintf("    x%-2d %016lx  x%-2d %016lx  x%-2d %016lx\n",
                i, f->x[i], i + 1, f->x[i + 1], i + 2, f->x[i + 2]);
    panic("unrecoverable exception");
}

/* ---- the GIC ---------------------------------------------------------------------
 *  The Orange Pi has a GIC-400 (GICv2). QEMU has whichever it is asked for:
 *  v2 under TCG, where it matches the Pi, and v3 under -accel hvf, because
 *  Apple's hypervisor only offers v3. Which one is the device tree's word
 *  (board.c), not a probe: a v3 ID register sits 64 KiB into the
 *  distributor, where a v2 has nothing and the read faults. v3 then uses
 *  its system-register CPU interface and the redistributor QEMU's virt
 *  board puts at 0x080A0000.
 *  Everything is routed to CPU 0, level-triggered, one priority.             */
static uintptr_t gicd, gicc;
static int gic3;
#define GICR_V3  0x080A0000u
#define GICR_SGI (GICR_V3 + 0x10000u)
#define GICD_CTLR        0x000
#define GICD_TYPER       0x004
#define GICD_ISENABLER   0x100
#define GICD_ICENABLER   0x180
#define GICD_IPRIORITYR  0x400
#define GICD_ITARGETSR   0x800
#define GICC_CTLR        0x00
#define GICC_PMR         0x04
#define GICC_BPR         0x08
#define GICC_IAR         0x0C
#define GICC_EOIR        0x10

static void gic3_init(void) {
    w32(gicd + GICD_CTLR, 0);
    while (r32(gicd + GICD_CTLR) & (1u << 31)) { }
    int lines = ((r32(gicd + GICD_TYPER) & 0x1F) + 1) * 32;
    for (int i = 32; i < lines; i += 32) {
        w32(gicd + GICD_ICENABLER + i / 8, 0xFFFFFFFFu);
        w32(gicd + 0x080 + i / 8, 0xFFFFFFFFu);                 /* group 1       */
    }
    for (int i = 32; i < lines; i += 4) w32(gicd + GICD_IPRIORITYR + i, 0xA0A0A0A0u);
    for (int i = 32; i < lines; i++) { w32(gicd + 0x6000 + 8 * (uintptr_t)i, 0); w32(gicd + 0x6004 + 8 * (uintptr_t)i, 0); }
    w32(gicd + GICD_CTLR, (1u << 4) | (1u << 1) | 1u);           /* ARE, groups on */
    while (r32(gicd + GICD_CTLR) & (1u << 31)) { }
    w32(GICR_V3 + 0x14, r32(GICR_V3 + 0x14) & ~(1u << 1));        /* wake up       */
    while (r32(GICR_V3 + 0x14) & (1u << 2)) { }
    w32(GICR_SGI + 0x080, 0xFFFFFFFFu);                           /* SGI/PPI group 1 */
    for (int i = 0; i < 32; i += 4) w32(GICR_SGI + 0x400 + (uintptr_t)i, 0xA0A0A0A0u);
    uint64_t sre;
    __asm__ volatile("mrs %0, S3_0_C12_C12_5" : "=r"(sre));      /* ICC_SRE_EL1   */
    __asm__ volatile("msr S3_0_C12_C12_5, %0\nisb" :: "r"(sre | 1));
    __asm__ volatile("msr S3_0_C4_C6_0, %0" :: "r"(0xF0ull));     /* ICC_PMR_EL1   */
    __asm__ volatile("msr S3_0_C12_C12_3, %0" :: "r"(0ull));      /* ICC_BPR1_EL1  */
    __asm__ volatile("msr S3_0_C12_C12_7, %0\nisb" :: "r"(1ull)); /* ICC_IGRPEN1_EL1 */
}

void gic_init(uintptr_t dist, uintptr_t cpu, int v3) {
    gicd = dist; gicc = cpu;
    gic3 = v3;
    if (gic3) { gic3_init(); return; }
    w32(gicd + GICD_CTLR, 0);
    int lines = ((r32(gicd + GICD_TYPER) & 0x1F) + 1) * 32;
    for (int i = 32; i < lines; i += 32) w32(gicd + GICD_ICENABLER + i / 8, 0xFFFFFFFFu);
    for (int i = 0; i < lines; i += 4) w32(gicd + GICD_IPRIORITYR + i, 0xA0A0A0A0u);
    for (int i = 32; i < lines; i += 4) w32(gicd + GICD_ITARGETSR + i, 0x01010101u);
    w32(gicd + GICD_CTLR, 1);
    w32(gicc + GICC_PMR, 0xF0);
    w32(gicc + GICC_BPR, 0);
    w32(gicc + GICC_CTLR, 1);
}

void gic_enable(int irq) {
    if (gic3 && irq < 32) w32(GICR_SGI + 0x100, 1u << irq);
    else w32(gicd + GICD_ISENABLER + (irq / 32) * 4, 1u << (irq % 32));
}

/* ---- the timer -----------------------------------------------------------------
 *  The virtual timer, PPI 27: the one an EL1 guest owns under every
 *  hypervisor, including the Mac's when QEMU runs with -accel hvf. 1 kHz.   */
#define TIMER_PPI 27
volatile uint64_t g_ticks;
static uint64_t cnt_freq, cnt_per_tick;

static inline uint64_t cntvct(void) {
    uint64_t v; __asm__ volatile("isb\nmrs %0, cntvct_el0" : "=r"(v)); return v;
}

static void timer_arm(void) {
    __asm__ volatile("msr cntv_tval_el0, %0\nmsr cntv_ctl_el0, %1\nisb"
                     :: "r"(cnt_per_tick), "r"(1ull) : "memory");
}

void timer_init(void) {
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cnt_freq));
    if (!cnt_freq) cnt_freq = 24000000;
    cnt_per_tick = cnt_freq / 1000;
    gic_enable(TIMER_PPI);
    timer_arm();
}

/* Read the frequency on first use, not in timer_init: kmain asks the time
 * before the timer is set up, and an AArch64 divide by zero quietly gives 0. */
static uint64_t freq(void) {
    if (!cnt_freq) __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cnt_freq));
    return cnt_freq ? cnt_freq : 24000000;
}
uint64_t now_us(void) { return cntvct() * 1000000ull / freq(); }
uint64_t now_ms(void) { return cntvct() * 1000ull / freq(); }
void delay_us(uint64_t us) { uint64_t t = now_us() + us; while (now_us() < t) __asm__ volatile("yield"); }
void delay_ms(uint64_t ms) { delay_us(ms * 1000); }

typedef void (*irq_fn)(void);
static irq_fn handlers[256];
void irq_register(int irq, irq_fn fn) { if (irq >= 0 && irq < 256) { handlers[irq] = fn; gic_enable(irq); } }

void irq_dispatch(void) {
    for (;;) {
        uint64_t iar64 = 0;
        if (gic3) __asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(iar64));   /* ICC_IAR1_EL1 */
        uint32_t iar = gic3 ? (uint32_t)iar64 : r32(gicc + GICC_IAR);
        uint32_t id = iar & 0x3FF;
        if (id >= 1020) return;
        if (id == TIMER_PPI) {
            g_ticks++;
            timer_arm();
        } else if (id < 256 && handlers[id]) {
            handlers[id]();
        }
        if (gic3) __asm__ volatile("msr S3_0_C12_C12_1, %0" :: "r"((uint64_t)iar));  /* ICC_EOIR1_EL1 */
        else w32(gicc + GICC_EOIR, iar);
    }
}

/* ---- the wall clock ------------------------------------------------------------- */
static uint64_t wall_base_secs, wall_base_ms;
static int wall_known, tz_min;

int clock_known(void) { return wall_known; }
uint64_t clock_unix(void) { return wall_known ? wall_base_secs + (now_ms() - wall_base_ms) / 1000 : now_ms() / 1000; }
void clock_set_unix(uint64_t secs) { wall_base_secs = secs; wall_base_ms = now_ms(); wall_known = 1; }
int  clock_tz_minutes(void) { return tz_min; }
void clock_set_tz_minutes(int m) { tz_min = m; }

void clock_local(int *y, int *mo, int *d, int *h, int *mi, int *s, int *wday) {
    int64_t t = (int64_t)clock_unix() + (int64_t)tz_min * 60;
    int64_t days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    if (h) *h = (int)(rem / 3600);
    if (mi) *mi = (int)(rem / 60 % 60);
    if (s) *s = (int)(rem % 60);
    if (wday) *wday = (int)((days + 4) % 7 + 7) % 7;        /* 1970-01-01: Thursday */
    /* days -> civil date (Howard Hinnant's algorithm) */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t dd = doy - (153 * mp + 2) / 5 + 1;
    int64_t mm = mp < 10 ? mp + 3 : mp - 9;
    if (mm <= 2) yy++;
    if (y) *y = (int)yy;
    if (mo) *mo = (int)mm;
    if (d) *d = (int)dd;
}
