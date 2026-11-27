/* ===========================================================================
 *  BoltBoot  -  mini/boot/clock.c
 *
 *  The H618's clocks before the DRAM: after the boot ROM the CPU runs from
 *  the 24 MHz crystal and almost nothing else is on. In order:
 *
 *    - R_PRCM: the "system power-off" gating and the resistor calibration
 *      the DRAM PHY's drivers are trimmed against
 *    - PLL_CPUX at a safe 408 MHz (1008 once the PMIC has raised VDD-CPU)
 *    - PLL_PERIPH0 at 600 MHz (its 2x output 1.2 GHz) for the buses, MMC, USB
 *    - CPU AXI /2 and APB /4, PSI/AHB1/AHB2 and AHB3 at 200 MHz, APB1 100 MHz
 *    - MBUS from PERIPH0 x2 / 3 = 400 MHz (switched on by dram.c)
 *
 *  Register values as the H616's CCU wants them (the CCU's PLL layout:
 *  bit 31 enable, 29 lock-enable, 28 locked, 27 output gate, N in 15:8).
 * ===========================================================================*/
#include "hw.h"
#include "boot0.h"

#define R_PRCM            0x07010000u
#define PRCM_PWROFF_GATE  (R_PRCM + 0x250)
#define PRCM_RES_CAL      (R_PRCM + 0x310)

#define PLL_CPUX          (CCU + 0x000)
#define PLL_PERIPH0       (CCU + 0x020)
#define CPU_AXI_CFG       (CCU + 0x500)
#define PSI_AHB_CFG       (CCU + 0x510)
#define AHB3_CFG          (CCU + 0x51C)
#define APB1_CFG          (CCU + 0x520)
#define APB2_CFG          (CCU + 0x524)
#define MBUS_CFG          (CCU + 0x540)

#define PLL_EN      (1u << 31)
#define PLL_LOCK_EN (1u << 29)
#define PLL_LOCKED  (1u << 28)
#define PLL_OUT_EN  (1u << 27)

static int wait_lock(uintptr_t reg) {
    uint32_t t0 = now_us();
    while (!(r32(reg) & PLL_LOCKED))
        if (now_us() - t0 > 10000) return -1;
    return 0;
}

/* PLL_CPUX = 24 MHz * n, the CPU on the crystal while it changes. */
int clock_cpu_mhz(uint32_t mhz) {
    uint32_t n = mhz / 24;
    if (n < 12) n = 12;
    clrset32(CPU_AXI_CFG, 3u << 24, 0u << 24);           /* CPU from OSC24M       */
    uint32_t v = r32(PLL_CPUX) & ~PLL_LOCK_EN;
    w32(PLL_CPUX, v);
    v &= ~PLL_OUT_EN;
    w32(PLL_CPUX, v);
    v &= ~((0xFFu << 8) | 0xFu | (0x3Fu << 16));         /* N, M, P              */
    v |= (n - 1) << 8;
    w32(PLL_CPUX, v);
    v |= PLL_EN;
    w32(PLL_CPUX, v);
    v |= PLL_LOCK_EN;
    w32(PLL_CPUX, v);
    int bad = wait_lock(PLL_CPUX);
    udelay(20);
    v |= PLL_OUT_EN;
    w32(PLL_CPUX, v);
    clrset32(CPU_AXI_CFG, 3u << 24, 3u << 24);           /* CPU from PLL_CPUX     */
    return bad;
}

int clock_init(void) {
    set32(PRCM_PWROFF_GATE, 0x10);
    udelay(1);
    set32(PRCM_RES_CAL, 2);
    udelay(1);
    clr32(PRCM_RES_CAL, 1);
    udelay(1);
    set32(PRCM_RES_CAL, 1);

    int bad = clock_cpu_mhz(408);

    w32(PLL_PERIPH0, 0xA8003100u);                       /* en, lock-en, out, N=50 (1.2 GHz /2 ) */
    bad |= wait_lock(PLL_PERIPH0);

    clrset32(CPU_AXI_CFG, 0x303, 0x301);                 /* AXI = CPU/2, APB = CPU/4 */
    w32(PSI_AHB_CFG, 0x03000002u);                       /* PERIPH0 / 3 = 200 MHz    */
    w32(AHB3_CFG,    0x03000002u);
    w32(APB1_CFG,    0x03000102u);                       /* PERIPH0 / 3 / 2 = 100    */
    w32(APB2_CFG,    0x00000000u);                       /* OSC24M (UART's 13 divisor) */
    w32(MBUS_CFG,    (1u << 24) | (3u - 1));             /* PERIPH0x2 / 3, still off */
    return bad;
}
