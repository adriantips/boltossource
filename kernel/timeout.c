#include <stdint.h>
#include "timeout.h"
#include "io.h"
#include "kprintf.h"

/* ---------------------------------------------------------------------------
 *  TSC calibration against PIT channel 2.
 *
 *  Channel 2 is the one channel whose gate software controls (port 0x61 bit 0)
 *  and whose output software can read (bit 5), which makes it a one-shot timer
 *  that needs no interrupt handler. Program a 10 ms count in mode 0, sample the
 *  TSC, wait for OUT2 to rise, sample again: the difference is 10 ms of TSC.
 *
 *  This runs before interrupts are enabled, before ACPI is parsed and before
 *  the HPET is found, which is the point -- the drivers that need a clock most
 *  are the ones that probe earliest.
 * ---------------------------------------------------------------------------*/

#define PIT_HZ        1193182u
#define CAL_US        10000u                       /* 10 ms calibration window */
#define CAL_LATCH     ((PIT_HZ * CAL_US) / 1000000u)

/* If the PIT never raises OUT2 -- no channel 2, a chipset that does not wire
 * the speaker port, a hypervisor that traps it away -- the wait must still end.
 * The TSC frequency is unknown at this point but it is certainly under 100 GHz,
 * so 5e9 cycles is an upper bound on "much longer than 10 ms" no matter what
 * the number turns out to be. */
#define CAL_ABORT_CYCLES 5000000000ull

/* Assumed when calibration fails. The TSC itself always exists on x86-64 and
 * always counts; only its rate is in question, so a guess keeps every wait
 * bounded and monotonic, which a stopped clock would not. 3 GHz is picked to
 * sit in the middle of the plausible range: on a 1 GHz part waits end about 3x
 * early, on a 5 GHz part they run about 1.7x long. Both are survivable; a
 * `now_us()` that never advances is not. */
#define ASSUMED_TSC_PER_US 3000ull

static uint64_t g_tsc_per_us = ASSUMED_TSC_PER_US;
static int      g_calibrated;
static uint64_t g_base;             /* TSC at timeout_init()                */

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t calibrate(void) {
    uint8_t saved = inb(0x61);

    /* Gate channel 2 on, speaker off (bit 1 clear) so this makes no noise. */
    outb(0x61, (uint8_t)((saved & ~0x02u) | 0x01u));
    outb(0x43, 0xB0);                              /* ch2, lo/hi, mode 0 */
    outb(0x42, (uint8_t)(CAL_LATCH & 0xFF));
    outb(0x42, (uint8_t)(CAL_LATCH >> 8));

    uint64_t t0 = rdtsc();
    while (!(inb(0x61) & 0x20)) {                  /* OUT2 low while counting */
        if (rdtsc() - t0 > CAL_ABORT_CYCLES) { outb(0x61, saved); return 0; }
    }
    uint64_t t1 = rdtsc();

    outb(0x61, saved);

    uint64_t per_us = (t1 - t0) / CAL_US;
    /* Anything outside 50 MHz .. 20 GHz is a misread, not a CPU. Rejecting it
     * costs a coarser clock; believing it would make every timeout in the
     * kernel wrong by the same factor. */
    if (per_us < 50 || per_us > 20000) return 0;
    return per_us;
}

void timeout_init(void) {
    uint64_t per_us = calibrate();
    g_calibrated = per_us != 0;
    if (g_calibrated) g_tsc_per_us = per_us;
    g_base = rdtsc();
    if (g_calibrated)
        kprintf("[ok] TSC %lu MHz; driver timeouts are wall-clock\n", g_tsc_per_us);
    else
        kprintf("[--] TSC would not calibrate against the PIT; assuming %lu MHz\n",
                g_tsc_per_us);
}

uint64_t tsc_hz(void) { return g_calibrated ? g_tsc_per_us * 1000000ull : 0; }

uint64_t now_us(void) { return (rdtsc() - g_base) / g_tsc_per_us; }

void delay_us(uint64_t us) {
    uint64_t target = now_us() + us;
    while (now_us() < target) __asm__ volatile("pause");
}
