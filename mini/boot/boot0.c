/* ===========================================================================
 *  BoltBoot  -  mini/boot/boot0.c
 *
 *  boot0's main, at EL3 in SRAM:
 *
 *    clocks at safe speeds -> the PMIC's CPU and DRAM rails -> CPU to
 *    1008 MHz -> DRAM trained and sized -> a quick, non-destructive check
 *
 *  Over FEL (the flag pi.py sets in the header) it then goes back to the
 *  boot ROM, and the Mac reads the report. On the card it will load boot1
 *  (step 4 of the plan); for now it says so with the LEDs.
 * ===========================================================================*/
#include "hw.h"
#include "boot0.h"
#include "buildid.h"

#define DRAM_BASE 0x40000000ul

static uint64_t sysreg_el(void)    { uint64_t v; __asm__ volatile("mrs %0, CurrentEL" : "=r"(v)); return (v >> 2) & 3; }
static uint64_t sysreg_midr(void)  { uint64_t v; __asm__ volatile("mrs %0, midr_el1" : "=r"(v)); return v; }

/* ---- the watchdog (H6-style, the same one the kernel feeds) ---------------- */
void wdt_start(void) {
    w32(WDT + 0x00, 0);                            /* no interrupt                 */
    w32(WDT + 0x14, 0x16AA0000u | 1);              /* on expiry: reset the system  */
    w32(WDT + 0x18, 0x16AA0000u | (0xBu << 4));    /* 16 s                         */
    w32(WDT + 0x18, 0x16AA0000u | (0xBu << 4) | 1);
    w32(WDT + 0x10, (0x0A57u << 1) | 1);           /* restart the count            */
}
void wdt_stop(void) { w32(WDT + 0x18, 0x16AA0000u); }

/* ---- a quick look at the DRAM ---------------------------------------------- *
 *  Non-destructive (each word is put back), because the last run's log and
 *  what the kernel keeps across a restart are in DRAM: one word every 4 KiB
 *  over all of it, then 1 MiB word by word, timed.                           */
static int dram_check(uint64_t size) {
    int bad = 0;
    for (uint64_t off = 0; off < size; off += 4096) {
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(DRAM_BASE + off);
        uint32_t o = *p, want = (uint32_t)(DRAM_BASE + off) ^ 0xA5A5A5A5u;
        *p = want;
        if (*p != want && bad++ < 4) logf("  check: %lx reads %08x, wrote %08x\n", DRAM_BASE + off, *p, want);
        *p = o;
    }
    volatile uint32_t *q = (volatile uint32_t *)(uintptr_t)(DRAM_BASE + 0x08000000);   /* 0x48000000 */
    uint32_t t0 = now_us();
    for (uint32_t i = 0; i < (1u << 18); i++) q[i] = i * 0x9E3779B9u;
    for (uint32_t i = 0; i < (1u << 18); i++)
        if (q[i] != i * 0x9E3779B9u && bad++ < 8) logf("  check: %p reads %08x\n", (void *)&q[i], q[i]);
    uint32_t us = now_us() - t0;
    logf("  check: %lu MiB sampled every 4 KiB, 1 MiB written and read in %u us: %s\n",
         size >> 20, us, bad ? "ERRORS" : "all good");
    return bad;
}

void boot0_main(void) {
    uint32_t flags = r32(SRAM_BASE + HDR_FLAGS);
    int fel = flags & BOOT_FLAG_FEL;
    log_init();
    logf("BoltBoot boot0 %s\n", BOLT_BUILD_ID);
    logf("  EL%lu, MIDR %08lx, SRAM_VER %08x, from %s, %u ms since power-on\n",
         sysreg_el(), sysreg_midr(), r32(SRAM_VER_REG),
         fel ? "FEL (the Mac)" : "the card", (unsigned)(now_us() / 1000));
    led_red(1);
    led_green(1);

    uint32_t t = now_us();
    if (clock_init()) logf("  clocks: a PLL did not lock\n");
    else logf("  clocks: CPU 408 MHz, PERIPH0 1.2 GHz, buses 200/100 MHz\n");

    int pmic_ok = pmic_init() == 0;
    if (pmic_ok && clock_cpu_mhz(1008) == 0) logf("  CPU at 1008 MHz\n");
    else logf("  CPU stays at 408 MHz (the PMIC or the PLL said no)\n");

    uint64_t size = dram_init();
    w32(SRAM_BASE + 0x24, (uint32_t)(size >> 20));
    logf("  DRAM up in %u ms\n", (now_us() - t) / 1000);
    if (size) dram_check(size);
    led_green(0);

    if (fel) {
        logf("  back to FEL\n");
        return_to_fel();
    }
    /* Next: boot1 from the card (step 4). Until then: red on, green blinking
     * fast if the DRAM is up, slowly if it is not. */
    for (;;) {
        unsigned ms = size ? 150 : 600;
        led_green(1); mdelay(ms);
        led_green(0); mdelay(ms);
    }
}
