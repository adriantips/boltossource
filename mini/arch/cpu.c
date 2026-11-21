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

void mmu_enable(void);

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
    mmu_enable();
}

/* The registers half: what every core does with the boot core's tables. */
void mmu_enable(void) {

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

/* ---- a map for a Linux program (lx.c) ------------------------------------------
 *  The same identity map, with the given windows of RAM (2 MiB aligned) open
 *  to EL0 as well: the core running a Linux program at EL0 uses it, so the
 *  program reaches its own memory and nothing else -- not the kernel's heap,
 *  not a device. Memory EL0 may write is never executable at EL1.          */
static uint64_t l1_user[512] __attribute__((aligned(4096)));
static uint64_t l2_user[512] __attribute__((aligned(4096)));

uint64_t mmu_user_table(const uint64_t *base, const uint64_t *size, int n) {
    for (int i = 0; i < 512; i++) { l1_user[i] = l1[i]; l2_user[i] = l2_ram[i]; }
    l1_user[1] = (uint64_t)(uintptr_t)l2_user | PTE_TABLE;
    for (int w = 0; w < n; w++)
        for (uint64_t a = base[w]; a < base[w] + size[w]; a += 0x200000ull) {
            uint64_t i = (a - 0x40000000ull) >> 21;
            if (a >= 0x40000000ull && i > 0 && i < 512 && l2_user[i]) l2_user[i] |= 1ull << 6;   /* AP[1] */
        }
    __asm__ volatile("dsb ish" ::: "memory");
    return (uint64_t)(uintptr_t)l1_user;
}

/* This core's map (0: everyone's). Only this core's TLB is touched. */
void mmu_use(uint64_t ttbr0) {
    if (!ttbr0) ttbr0 = (uint64_t)(uintptr_t)l1;
    __asm__ volatile("dsb ish\nmsr ttbr0_el1, %0\nisb\ntlbi vmalle1\ndsb nsh\nic iallu\ndsb nsh\nisb"
                     :: "r"(ttbr0) : "memory");
}

/* ---- the other cores ------------------------------------------------------------ *
 *  Cores 1-3 start at boot into pool_loop and wait there (WFE) for work.
 *  core_run() hands an idle one a job; cpu_start() waits for the core to
 *  come free first (asking a pool job there to finish) -- the video decoder
 *  on core 1, which returns when the video closes. Either way the core
 *  waits in the pool again afterwards. No interrupts, no timer of their
 *  own, no shell.
 *
 *  What the boot core writes (the job) and what the worker writes (that it
 *  finished) live on cache lines of their own: a core spinning on a line
 *  the other one keeps writing never gets to finish its own access.        */
volatile uint32_t mini_heap_lock[16] __attribute__((aligned(64)));   /* word 0; a cache line of its own */
extern uint8_t secondary_stacks[];
uint8_t secondary_stacks[4 * 0x40000] __attribute__((aligned(16)));
void secondary_start(void);
int board_cpu_on(int cpu, uint64_t entry, uint64_t ctx);

typedef struct {
    void (*volatile fn)(void *);
    void *volatile arg;
    volatile uint32_t seq;                  /* bumped for every job posted          */
    volatile int wanted;                    /* cpu_start is waiting for the core    */
} __attribute__((aligned(64))) post_t;
typedef struct {
    volatile uint32_t done;                 /* the seq of the last job finished     */
    volatile int up;                        /* the core reached pool_loop           */
} __attribute__((aligned(64))) ack_t;
static post_t post[4];
static ack_t  ack[4];
static int    pooled[4];                    /* started into the pool, still in it   */

void secondary_main(uint64_t cpu) {
    __asm__ volatile("dsb sy\nisb" ::: "memory");
    if (cpu >= 4) for (;;) __asm__ volatile("wfe");
    __atomic_store_n(&ack[cpu].up, 1, __ATOMIC_RELEASE);
    __asm__ volatile("sev");
    uint32_t seen = 0;
    for (;;) {
        uint32_t q;
        while ((q = __atomic_load_n(&post[cpu].seq, __ATOMIC_ACQUIRE)) == seen) __asm__ volatile("wfe");
        seen = q;
        __asm__ volatile("isb" ::: "memory");             /* the job may be code loaded since (pkg.c) */
        post[cpu].fn(post[cpu].arg);
        __atomic_store_n(&ack[cpu].done, seen, __ATOMIC_RELEASE);
        __asm__ volatile("dsb ish\nsev" ::: "memory");
    }
}

void cores_init(void) {
    for (int c = 1; c < 4; c++) {
        if (board_cpu_on(c, (uint64_t)(uintptr_t)secondary_start, (uint64_t)c) != 0) continue;
        uint64_t end = now_ms() + 200;
        while (!__atomic_load_n(&ack[c].up, __ATOMIC_ACQUIRE) && now_ms() < end) __asm__ volatile("yield");
        pooled[c] = ack[c].up;
    }
    int n = 0;
    for (int c = 1; c < 4; c++) n += pooled[c];
    kprintf("[ok] %d more core%s waiting for work\n", n, n == 1 ? "" : "s");
}

static int post_job(int cpu, void (*fn)(void *), void *arg) {
    if (cpu < 1 || cpu > 3 || !pooled[cpu] || core_busy(cpu)) return -1;
    post[cpu].fn = fn; post[cpu].arg = arg;
    __atomic_store_n(&post[cpu].seq, post[cpu].seq + 1, __ATOMIC_RELEASE);
    __asm__ volatile("dsb ish\nsev" ::: "memory");
    return 0;
}

int core_pooled(int cpu) { return cpu >= 1 && cpu <= 3 && pooled[cpu]; }
int core_busy(int cpu) {
    if (!core_pooled(cpu)) return 1;
    return __atomic_load_n(&ack[cpu].done, __ATOMIC_ACQUIRE) != post[cpu].seq;
}
int core_wanted(int cpu) { return cpu >= 1 && cpu <= 3 && post[cpu].wanted; }
int core_run(int cpu, void (*fn)(void *), void *arg) {
    if (core_wanted(cpu)) return -1;
    return post_job(cpu, fn, arg);
}

int cpu_start(int cpu, void (*fn)(void *), void *arg) {
    if (!core_pooled(cpu)) return -1;
    /* A job of the pool's may be running there: ask it to finish (pool users
     * check core_wanted between steps) and give it a moment. */
    post[cpu].wanted = 1;
    uint64_t end = now_ms() + 3000;
    while (core_busy(cpu) && now_ms() < end) __asm__ volatile("yield");
    post[cpu].wanted = 0;
    /* The core stays the job's until it returns, then waits in the pool again. */
    return post_job(cpu, fn, arg);
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
    /* A Linux program's core (lx.c): its syscalls, and its faults, which end
     * the program and not the machine. */
    int lx_trap(void *frame, uint64_t kind);
    if (lx_trap(f, kind)) return;
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
