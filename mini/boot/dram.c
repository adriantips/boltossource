/* ===========================================================================
 *  BoltBoot  -  mini/boot/dram.c
 *
 *  The H616/H618's DRAM: a Synopsys uMCTL2-style controller (CTL), Allwinner's
 *  own bus glue in front of it (COM) and a PHY nobody has documented. The
 *  controller's registers follow the uMCTL2 layout; the PHY's are known only
 *  by what Allwinner's own boot0 writes, as worked out by the people who
 *  reverse-engineered it for U-Boot (arch/arm/mach-sunxi/dram_sun50i_h616.c,
 *  in build/mini-fw/, read as documentation). The sequence and the numbers
 *  below are that chip's; the code is ours, for one board only:
 *
 *    the Orange Pi Zero3: LPDDR4 at 792 MHz, Orange Pi's tuning values
 *    (DX_ODT, DX_DRI, CA_DRI, ODT_EN, TPR6/10/11/12), PHY address map 0.
 *
 *  dram_init() finds the bus width and ranks (most demanding first: a
 *  smaller setting also "works" but shows only part of the RAM), then the
 *  column and row counts by aliasing, then sets it all up for real and
 *  returns the size. Every wait has a timeout; a failure is reported, never
 *  a hang, because over FEL the Mac is waiting for the report.
 * ===========================================================================*/
#include "hw.h"
#include "boot0.h"

#define COM 0x047FA000u
#define CTL 0x047FB000u
#define PHY 0x04800000u
#define DRAM_BASE 0x40000000u

/* COM (the glue) */
#define COM_CR      (COM + 0x000)
#define COM_008     (COM + 0x008)
#define COM_TMR     (COM + 0x00C)
#define COM_014     (COM + 0x014)
#define COM_MAER0   (COM + 0x020)
#define COM_MAER1   (COM + 0x024)
#define COM_MAER2   (COM + 0x028)
#define COM_050     (COM + 0x050)
#define COM_BWCR    (COM + 0x200)
#define COM_MCFG0(p) (COM + 0x210 + (p) * 0x10)
#define COM_MCFG1(p) (COM + 0x214 + (p) * 0x10)
#define COM_500     (COM + 0x500)

/* CTL (uMCTL2) */
#define MSTR       (CTL + 0x000)
#define STAT       (CTL + 0x004)
#define CLKEN      (CTL + 0x00C)
#define MRCTRL0    (CTL + 0x010)
#define MRCTRL1    (CTL + 0x014)
#define PWRCTL     (CTL + 0x030)
#define HWLPCTL    (CTL + 0x038)
#define RFSHCTL3   (CTL + 0x060)
#define RFSHTMG    (CTL + 0x064)
#define INIT(n)    (CTL + 0x0D0 + (n) * 4)
#define RANKCTL    (CTL + 0x0F4)
#define DRAMTMG(n) (CTL + 0x100 + (n) * 4)
#define ZQCTL0     (CTL + 0x180)
#define DFITMG0    (CTL + 0x190)
#define DFITMG1    (CTL + 0x194)
#define DFIUPD0    (CTL + 0x1A0)
#define DFIMISC    (CTL + 0x1B0)
#define DFISTAT    (CTL + 0x1BC)
#define ADDRMAP(n) (CTL + 0x200 + (n) * 4)
#define ODTCFG     (CTL + 0x240)
#define ODTMAP     (CTL + 0x244)
#define SCHED0     (CTL + 0x250)
#define SWCTL      (CTL + 0x320)
#define SWSTAT     (CTL + 0x324)

/* CCU */
#define PLL_DDR0   (CCU + 0x010)
#define MBUS_CFG   (CCU + 0x540)
#define DRAM_CLK   (CCU + 0x800)
#define DRAM_BGR   (CCU + 0x80C)
#define R_PRCM_RES_CAL  0x07010310u
#define R_PRCM_OHMS240  0x07010318u

/* The Zero3's numbers (Orange Pi's, as in its U-Boot defconfig). */
#define CLK_MHZ  792
#define DX_ODT   0x07070707u
#define DX_DRI   0x0E0E0E0Eu
#define CA_DRI   0x0E0Eu
#define ODT_EN   0xAAAAEEEEu
#define TPR6     0x44000000u
#define TPR10    0x402F6663u
#define TPR11    0x24242624u
#define TPR12    0x0F0F100Fu

#define TPR10_CA_BIT_DELAY   (1u << 16)
#define TPR10_DX_BIT_DELAY0  (1u << 17)
#define TPR10_DX_BIT_DELAY1  (1u << 18)
#define TPR10_WRITE_LEVELING (1u << 20)
#define TPR10_READ_CAL       (1u << 21)
#define TPR10_READ_TRAINING  (1u << 22)
#define TPR10_WRITE_TRAINING (1u << 23)

typedef struct { int cols, rows, ranks, full; } geom_t;

static int g_timeout;                    /* a wait ran out: this attempt failed */

static void wait(uintptr_t reg, uint32_t mask, uint32_t val) {
    if (g_timeout) return;
    uint32_t t0 = now_us();
    while ((r32(reg) & mask) != val)
        if (now_us() - t0 > 100000) {
            logf("  dram: timeout at %08x (%08x & %08x != %08x)\n", (unsigned)reg, r32(reg), mask, val);
            g_timeout = 1;
            return;
        }
}

/* Controller cycles (half the DRAM clock) for ns nanoseconds, rounded up. */
static uint32_t ns(uint32_t n) { return ((CLK_MHZ / 2) * n + 999) / 1000; }
static uint32_t max2(uint32_t a, uint32_t b) { return a > b ? a : b; }

/* ---- clocks and resets ----------------------------------------------------- */
static void sys_init(void) {
    clr32(MBUS_CFG, 1u << 31);                    /* MBUS off, held in reset     */
    clr32(MBUS_CFG, 1u << 30);
    clr32(DRAM_BGR, 1u << 0);                     /* DRAM bus gate, reset        */
    udelay(5);
    clr32(DRAM_BGR, 1u << 16);
    clr32(PLL_DDR0, 1u << 31);
    clr32(DRAM_CLK, 1u << 30);                    /* DRAM module reset           */
    udelay(5);

    /* PLL_DDR0 at twice the DRAM clock: 24 MHz * 66 = 1584 MHz. */
    w32(PLL_DDR0, (1u << 31) | (1u << 29) | (1u << 27) | ((CLK_MHZ * 2 / 24 - 1) << 8));
    wait(PLL_DDR0, 1u << 28, 1u << 28);

    w32(DRAM_CLK, 0);                             /* from PLL_DDR0, /1           */
    w32(DRAM_BGR, 1u << 16);
    udelay(5);
    set32(DRAM_BGR, 1u << 0);

    w32(COM_MAER0, 0);                            /* no bus masters yet          */
    w32(COM_MAER1, 0);
    w32(COM_MAER2, 0);

    set32(MBUS_CFG, 1u << 30);
    set32(MBUS_CFG, 1u << 31);
    clr32(COM_500, 1u << 25);
    set32(DRAM_CLK, 1u << 30);
    udelay(5);
    w32(CLKEN, 0x8000);                           /* lets the CTL registers be written */
}

/* ---- where rows, columns, banks and ranks sit in an address -------------- */
static void set_addrmap(const geom_t *g) {
    int cols = g->full ? g->cols : g->cols - 1, rows = g->rows;
    uint32_t c3 = (uint32_t)(cols - 3);

    w32(ADDRMAP(0), g->ranks == 2 ? (uint32_t)(rows + cols - 3) : 0x1F);
    w32(ADDRMAP(1), (uint32_t)(cols - 2) * 0x010101u);       /* 8 banks */
    w32(ADDRMAP(2), 0);
    static const uint32_t a3[] = { 0x1F1F1F00, 0x1F1F0000, 0x1F000000, 0, 0, 0 };
    static const uint32_t a4[] = { 0x1F1F, 0x1F1F, 0x1F1F, 0x1F1F, 0x1F00, 0 };
    w32(ADDRMAP(3), a3[cols - 7]);
    w32(ADDRMAP(4), a4[cols - 7]);
    w32(ADDRMAP(5), c3 * 0x01010101u);
    uint32_t a6, a7 = 0x0F0F;
    switch (rows) {
    case 13: a6 = c3 | 0x0F0F0F00; break;
    case 14: a6 = c3 * 0x0101u | 0x0F0F0000; break;
    case 15: a6 = c3 * 0x010101u | 0x0F000000; break;
    case 16: a6 = c3 * 0x01010101u; break;
    case 17: a6 = c3 * 0x01010101u; a7 = c3 | 0x0F00; break;
    default: a6 = c3 * 0x01010101u; a7 = c3 * 0x0101u; break;     /* 18 */
    }
    w32(ADDRMAP(6), a6);
    w32(ADDRMAP(7), a7);
    w32(ADDRMAP(8), 0x3F3F);                       /* bank groups: DDR4 only      */
}

/* ---- timings, for LPDDR4 at 792 MHz ---------------------------------------- */
static void set_timing(void) {
    uint32_t tccd = 4, tfaw = ns(40), trrd = max2(ns(10), 2), trcd = max2(ns(18), 2);
    uint32_t trc = ns(65), txp = max2(ns(8), 2), trtp = 4, trp = ns(21), tras = ns(42);
    uint32_t trefi = ns(3904) / 32, trfc = ns(280), txsr = ns(190);
    uint32_t tmrw = max2(ns(14), 5), tmrd = tmrw, tmod = 12;
    uint32_t tcke = max2(ns(15), 2), tcksrx = max2(ns(2), 2), tcksre = max2(ns(5), 2), tckesr = tcke;
    uint32_t trasmax = (trefi * 9) / 32, txs = 4, txsdll = 16, txsabort = 4, txsfast = 4;
    uint32_t tcl = 10, tcwl = 5, t_rdata_en = 17, tphy_wrlat = 5, twtp = 24;
    uint32_t twr2rd = max2(trrd, 4) + 14, trd2wr = ns(4) + 17 - ns(1);

    w32(DRAMTMG(0), twtp << 24 | tfaw << 16 | trasmax << 8 | tras);
    w32(DRAMTMG(1), txp << 16 | trtp << 8 | trc);
    w32(DRAMTMG(2), tcwl << 24 | tcl << 16 | trd2wr << 8 | twr2rd);
    w32(DRAMTMG(3), tmrw << 20 | tmrd << 12 | tmod);
    w32(DRAMTMG(4), trcd << 24 | tccd << 16 | trrd << 8 | trp);
    w32(DRAMTMG(5), tcksrx << 24 | tcksre << 16 | tckesr << 8 | tcke);
    w32(DRAMTMG(6), (txp + 2) | 0x02020000);
    w32(DRAMTMG(8), txsfast << 24 | txsabort << 16 | txsdll << 8 | txs);
    w32(DRAMTMG(9), 0x00020208);
    w32(DRAMTMG(10), 0x000E0C05);
    w32(DRAMTMG(11), 0x440C021C);
    w32(DRAMTMG(12), 8);
    w32(DRAMTMG(13), 0x0A100002);
    w32(DRAMTMG(14), txsr);

    clrset32(INIT(0), 0xC0000FFF, 0x3F0);
    w32(INIT(1), 0x01F20000);
    w32(INIT(2), 0x00000D05);
    w32(DFIMISC, 0);
    w32(INIT(3), 0x0034001B);                     /* MR1, MR2                    */
    w32(INIT(4), 0x00330000);                     /* MR3                         */
    w32(INIT(6), 0x00040072);                     /* MR11, MR12                  */
    w32(INIT(7), 0x00240009);                     /* MR22, MR14                  */
    clrset32(RANKCTL, 0xFF0, 0x660);

    w32(DFITMG0, tphy_wrlat | 0x2000000 | t_rdata_en << 16 | 0x808000);
    w32(DFITMG1, 0x100202);
    w32(RFSHTMG, trefi << 16 | trfc);
}

/* ---- the PHY --------------------------------------------------------------- */
/* Which controller address line goes to which PHY pin (map 0, LPDDR4). */
static const uint8_t phy_map[27] = {
    0x02, 0x00, 0x17, 0x05, 0x04, 0x19, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
    0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x01, 0x18, 0x03, 0x1A,
};

#define B(v, n) (((v) >> ((n) * 8)) & 0x1F)

static void phy_odt(void) {
    /* per byte lane (0x40 apart): drive strength, then ODT */
    static const uint32_t lane[4] = { 0x380, 0x3C0, 0x400, 0x440 };
    for (int i = 0; i < 4; i++) clrset32(PHY + lane[i] + 0x10, 1u << 5, 1u << 4);
    for (int i = 0; i < 4; i++) {
        w32(PHY + lane[i] + 0x08, B(DX_DRI, i));
        w32(PHY + lane[i] + 0x0C, B(0x04040404u, i));
    }
    w32(PHY + 0x340, B(CA_DRI, 0)); w32(PHY + 0x344, B(CA_DRI, 0));
    w32(PHY + 0x348, B(CA_DRI, 1)); w32(PHY + 0x34C, B(CA_DRI, 1));
    for (int i = 0; i < 4; i++) {
        w32(PHY + lane[i] + 0x00, B(DX_ODT, i));
        w32(PHY + lane[i] + 0x04, 0);
    }
    dsb();
}

static int phy_write_leveling(const geom_t *g) {
    int ok = 1;
    uint32_t done = g->full ? 0xF : 0x3;
    clrset32(PHY + 8, 0xC0, 0x80);
    w32(PHY + 0x0C, 0x1B);                        /* MR2 for leveling            */
    w32(PHY + 0x10, 0);
    set32(PHY + 8, 4);
    wait(PHY + 0x188, done, done);
    clr32(PHY + 8, 4);
    static const uint32_t res[] = { 0x258, 0x25C, 0x318, 0x31C };
    for (int i = 0; i < 4; i++) { uint32_t v = r32(PHY + res[i]); if (v == 0 || v == 0x3F) ok = 0; }
    clr32(PHY + 8, 0xC0);
    if (g->ranks == 2) {
        clrset32(PHY + 8, 0xC0, 0x40);
        set32(PHY + 8, 4);
        wait(PHY + 0x188, done, done);
        clr32(PHY + 8, 4);
    }
    clr32(PHY + 8, 0xC0);
    return ok && !g_timeout;
}

static int read_cal_once(uint32_t done) {
    uint32_t t0 = now_us();
    set32(PHY + 8, 1);
    int ok = 1;
    while ((r32(PHY + 0x184) & done) != done) {
        if (r32(PHY + 0x184) & 0x20) { ok = 0; break; }
        if (now_us() - t0 > 100000) { ok = 0; g_timeout = 1; logf("  dram: read calibration timed out\n"); break; }
    }
    clr32(PHY + 8, 1);
    return ok;
}

static int phy_read_calibration(const geom_t *g) {
    uint32_t done = g->full ? 0xF : 0x3;
    clrset32(PHY + 8, 0x30, 0x20);
    int ok = read_cal_once(done);
    clr32(PHY + 8, 0x30);
    if (g->ranks == 2) {
        clrset32(PHY + 8, 0x30, 0x10);
        ok &= read_cal_once(done);
    }
    clr32(PHY + 8, 0x30);
    uint32_t v = r32(PHY + 0x274) & 7, t;
    if ((t = r32(PHY + 0x26C) & 7) > v) v = t;
    if ((t = r32(PHY + 0x32C) & 7) > v) v = t;
    if ((t = r32(PHY + 0x334) & 7) > v) v = t;
    clrset32(PHY + 0x38, 7, (v + 2) & 7);
    set32(PHY + 4, 0x20);
    return ok;
}

/* A training result is a window per bit: the right edge must be more than
 * 6 steps past the left one, for all 9 bits (8 data + mask) of a lane. */
static int windows_ok(uint32_t right, uint32_t left) {
    for (int i = 0; i < 9; i++)
        if (r32(PHY + right + i * 4) - r32(PHY + left + i * 4) <= 6) return 0;
    return 1;
}

static int phy_read_training(const geom_t *g) {
    int ok = 1;
    w32(PHY + 0x800, 0);
    w32(PHY + 0x81C, 0);
    clrset32(PHY + 0x198, 3, 2);
    clrset32(PHY + 0x804, 0x3F, 0xF);
    clrset32(PHY + 0x808, 0x3F, 0xF);
    clrset32(PHY + 0xA04, 0x3F, 0xF);
    clrset32(PHY + 0xA08, 0x3F, 0xF);
    set32(PHY + 0x190, 6);
    set32(PHY + 0x190, 1);
    wait(PHY + 0x840, 0xC, 0xC);
    if (r32(PHY + 0x840) & 3) ok = 0;
    if (g->full) { wait(PHY + 0xA40, 0xC, 0xC); if (r32(PHY + 0xA40) & 3) ok = 0; }
    ok &= windows_ok(0x898, 0x850) & windows_ok(0x8BC, 0x874);
    if (g->full) ok &= windows_ok(0xA98, 0xA50) & windows_ok(0xABC, 0xA74);
    clr32(PHY + 0x190, 3);
    if (g->ranks == 2) {
        clrset32(PHY + 0x198, 3, 2);
        set32(PHY + 0x190, 6);
        set32(PHY + 0x190, 1);
        wait(PHY + 0x840, 0xC, 0xC);
        if (r32(PHY + 0x840) & 3) ok = 0;
        if (g->full) { wait(PHY + 0xA40, 0xC, 0xC); if (r32(PHY + 0xA40) & 3) ok = 0; }
        clr32(PHY + 0x190, 3);
    }
    clr32(PHY + 0x198, 3);
    return ok && !g_timeout;
}

static int phy_write_training(const geom_t *g) {
    int ok = 1;
    w32(PHY + 0x134, 0); w32(PHY + 0x138, 0);
    w32(PHY + 0x19C, 0); w32(PHY + 0x1A0, 0);
    clrset32(PHY + 0x198, 0xC, 8);
    set32(PHY + 0x190, 0x10);
    set32(PHY + 0x190, 0x20);
    wait(PHY + 0x8E0, 3, 3);
    if (r32(PHY + 0x8E0) & 0xC) ok = 0;
    if (g->full) { wait(PHY + 0xAE0, 3, 3); if (r32(PHY + 0xAE0) & 0xC) ok = 0; }
    ok &= windows_ok(0x938, 0x8F0) & windows_ok(0x95C, 0x914);
    if (g->full) ok &= windows_ok(0xB38, 0xAF0) & windows_ok(0xB5C, 0xB14);
    clr32(PHY + 0x190, 0x60);
    if (g->ranks == 2) {
        clrset32(PHY + 0x198, 0xC, 4);
        set32(PHY + 0x190, 0x10);
        set32(PHY + 0x190, 0x20);
        wait(PHY + 0x8E0, 3, 3);
        if (r32(PHY + 0x8E0) & 0xC) ok = 0;
        if (g->full) { wait(PHY + 0xAE0, 3, 3); if (r32(PHY + 0xAE0) & 0xC) ok = 0; }
        clr32(PHY + 0x190, 0x60);
    }
    clr32(PHY + 0x198, 0xC);
    return ok && !g_timeout;
}

/* Nine values to every other word from base (and the same 0xC0 further on:
 * the other half of the lane pair). */
static void put9(uint32_t base, uint32_t v) {
    for (int i = 0; i < 9; i++) { w32(PHY + base + i * 8, v); w32(PHY + base + 0xC0 + i * 8, v); }
}
static void put4(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t v) {
    w32(PHY + a, v); w32(PHY + b, v); w32(PHY + c, v); w32(PHY + d, v);
}

/* The tuning values' per-bit delays (TPR10 bit 30: TPR11/12 hold 6-bit
 * values and ODT_EN the DQS ones). */
static void phy_bit_delays(void) {
    if (TPR10 & TPR10_DX_BIT_DELAY1) {             /* write side, from TPR11     */
        clr32(PHY + 0x60, 1);
        set32(PHY + 8, 8);
        clr32(PHY + 0x190, 0x10);
        clr32(PHY + 0x4, 0x80);
        put9(0x484, TPR11 & 0x3F);
        put4(0x4D0, 0x590, 0x4CC, 0x58C, (ODT_EN >> 15) & 0x1E);
        put9(0x4D8, (TPR11 >> 8) & 0x3F);
        put4(0x524, 0x5E4, 0x520, 0x5E0, (ODT_EN >> 19) & 0x1E);
        put9(0x604, (TPR11 >> 16) & 0x3F);
        put4(0x650, 0x710, 0x64C, 0x70C, (ODT_EN >> 23) & 0x1E);
        put9(0x658, (TPR11 >> 24) & 0x3F);
        put4(0x6A4, 0x764, 0x6A0, 0x760, (ODT_EN >> 27) & 0x1E);
        dsb();
        set32(PHY + 0x60, 1);
    }
    if (TPR10 & TPR10_DX_BIT_DELAY0) {             /* read side, from TPR12      */
        clr32(PHY + 0x54, 0x80);
        clr32(PHY + 0x190, 4);
        put9(0x480, TPR12 & 0x3F);
        put4(0x528, 0x5E8, 0x4C8, 0x588, (ODT_EN << 1) & 0x1E);
        put9(0x4D4, (TPR12 >> 8) & 0x3F);
        put4(0x52C, 0x5EC, 0x51C, 0x5DC, (ODT_EN >> 3) & 0x1E);
        put9(0x600, (TPR12 >> 16) & 0x3F);
        put4(0x6A8, 0x768, 0x648, 0x708, (ODT_EN >> 7) & 0x1E);
        put9(0x654, (TPR12 >> 24) & 0x3F);
        put4(0x6AC, 0x76C, 0x69C, 0x75C, (ODT_EN >> 11) & 0x1E);
        dsb();
        set32(PHY + 0x54, 0x80);
    }
}

static void phy_ca_delays(const geom_t *g) {
    uint32_t v = (TPR10 >> 3) & 0x1E;              /* TPR0 is 0 on the Zero3      */
    for (int i = 0; i < 32; i++) w32(PHY + 0x780 + i * 4, v);
    put4(0x7D8, 0x7DC, 0x7E0, 0x7F4, (TPR10 << 1) & 0x1E);
    w32(PHY + 0x788, (TPR10 >> 7) & 0x1E);
    if (g->ranks == 2) w32(PHY + 0x794, (TPR10 >> 11) & 0x1E);
}

/* An LPDDR4 mode register write through the controller: MR number and value. */
static void mrw(uint32_t mr_val, int slow) {
    w32(MRCTRL1, mr_val);
    if (slow) udelay(10);
    w32(MRCTRL0, 0x80000030);
    if (slow) udelay(10);
    wait(MRCTRL0, 1u << 31, 0);
}

static int phy_init(const geom_t *g) {
    clr32(PHY + 0x4, 0x80);
    clrset32(PHY + 0x3C, 0xF, g->full ? 0xF : 0x3);

    /* read and write latencies, LPDDR4: 20 and 10 */
    put4(0x14, 0x35C, 0x368, 0x374, 20);
    put4(0x18, 0x360, 0x36C, 0x378, 0);
    put4(0x1C, 0x364, 0x370, 0x37C, 10);

    for (int i = 0; i < 27; i++) w32(PHY + 0xC0 + i * 4, phy_map[i]);
    if (TPR10 & TPR10_CA_BIT_DELAY) phy_ca_delays(g);

    w32(PHY + 0x3DC, (TPR6 >> 24) & 0xFF);
    w32(PHY + 0x45C, (TPR6 >> 24) & 0xFF);
    phy_odt();
    clrset32(PHY + 4, 7, 0x0D);

    /* 792 MHz: above 672 and 500 */
    clr32(PHY + 0x144, 1u << 7);
    clr32(PHY + 0x14C, 0xE0);

    clr32(COM_500, 0x200);
    udelay(1);
    clr32(PHY + 0x14C, 8);
    wait(PHY + 0x180, 4, 4);
    udelay(1000);
    w32(PHY + 0x58, 0x37);

    /* DFI init, through the controller's quasi-dynamic register gate */
    w32(SWCTL, 0);
    set32(DFIMISC, 1);
    set32(DFIMISC, 0x20);
    w32(SWCTL, 1);
    wait(SWSTAT, 1, 1);
    wait(DFISTAT, 1, 1);
    w32(SWCTL, 0);
    clr32(DFIMISC, 0x20);
    clr32(PWRCTL, 0x20);
    w32(SWCTL, 1);
    wait(SWSTAT, 1, 1);
    wait(STAT, 3, 1);                              /* normal operating mode       */
    udelay(200);
    w32(SWCTL, 0);
    clr32(DFIMISC, 1);
    w32(SWCTL, 1);
    wait(SWSTAT, 1, 1);
    if (g_timeout) return 0;

    /* the mode registers: MR0, 1, 2, 3, 4, 11, 12, 14, 22 */
    mrw(0x0000, 0); mrw(0x0134, 0); mrw(0x021B, 0); mrw(0x0333, 0); mrw(0x0403, 0);
    mrw(0x0B04, 1); mrw(0x0C72, 1); mrw(0x0E09, 1); mrw(0x1624, 1);
    if (g_timeout) return 0;

    w32(PHY + 0x54, 0);
    w32(SWCTL, 0);
    clr32(RFSHCTL3, 1);                            /* refresh on                  */
    w32(SWCTL, 1);

    int i;
    if (TPR10 & TPR10_WRITE_LEVELING) {
        for (i = 0; i < 5 && !phy_write_leveling(g); i++) { }
        if (i == 5) { logf("    write leveling failed\n"); return 0; }
    }
    if (TPR10 & TPR10_READ_CAL) {
        for (i = 0; i < 5 && !phy_read_calibration(g); i++) { }
        if (i == 5) { logf("    read calibration failed\n"); return 0; }
    }
    if (TPR10 & TPR10_READ_TRAINING) {
        for (i = 0; i < 5 && !phy_read_training(g); i++) { }
        if (i == 5) { logf("    read training failed\n"); return 0; }
    }
    if (TPR10 & TPR10_WRITE_TRAINING) {
        for (i = 0; i < 5 && !phy_write_training(g); i++) { }
        if (i == 5) { logf("    write training failed\n"); return 0; }
    }
    phy_bit_delays();
    clr32(PHY + 0x60, 4);
    return !g_timeout;
}

/* ---- one whole bring-up with one geometry ---------------------------------- */
static int core_init(const geom_t *g) {
    g_timeout = 0;
    sys_init();

    clrset32(COM_500, 1u << 24, 0x200);
    w32(CLKEN, 0x8000);
    set32(COM_008, 0xFF00);
    w32(COM_050, 1);                               /* LPDDR4                      */
    clrset32(SCHED0, 0xFF00, 0x3000);
    w32(HWLPCTL, 0);
    set32(COM_008, 0xFF00);

    /* LPDDR4, burst 16, active ranks, bus width */
    w32(MSTR, (1u << 31) | (1u << 30) | ((g->ranks == 2 ? 3u : 1u) << 24) | (8u << 16) | (1u << 5) |
              (g->full ? 0 : 1u << 12));
    w32(ODTMAP, g->ranks == 2 ? 0x0303 : 0x0201);
    w32(ODTCFG, 0x04000400);
    w32(CTL + 0x2240, 0x04000400);
    w32(CTL + 0x3240, 0x04000400);
    w32(CTL + 0x4240, 0x04000400);
    w32(COM_CR, 1u << 31);

    set_addrmap(g);
    set_timing();

    w32(PWRCTL, 0);
    set32(DFIUPD0, 3u << 30);
    set32(ZQCTL0, 3u << 30);
    set32(CTL + 0x2180, 3u << 30);
    set32(CTL + 0x3180, 3u << 30);
    set32(CTL + 0x4180, 3u << 30);
    set32(RFSHCTL3, 1);
    clr32(DFIMISC, 1);
    w32(COM_MAER0, 0); w32(COM_MAER1, 0); w32(COM_MAER2, 0);
    w32(PWRCTL, 0x20);
    set32(CLKEN, 1u << 8);
    clrset32(COM_500, 1u << 24, 0x300);
    udelay(1);
    set32(COM_500, 1u << 24);                      /* opens the PHY's registers   */
    udelay(1);

    if (!phy_init(g)) return 0;

    w32(SWCTL, 0);
    clr32(RFSHCTL3, 1);
    set32(COM_014, 1u << 31);
    w32(COM_MAER0, 0xFFFFFFFF);                    /* every bus master on         */
    w32(COM_MAER1, 0x7FF);
    w32(COM_MAER2, 0xFFFF);
    w32(SWCTL, 1);
    wait(SWSTAT, 1, 1);
    return !g_timeout;
}

/* ---- finding the geometry --------------------------------------------------- */
static volatile uint32_t *const ram = (volatile uint32_t *)(uintptr_t)DRAM_BASE;

static void pattern_write(void) {
    for (int i = 0; i < 16; i++) {
        uint32_t a = DRAM_BASE + (uint32_t)i * 4;
        ram[i] = (i & 1) ? ~a : a;
    }
}

/* 1 if the 16 words at DRAM_BASE + off read back as the pattern: that
 * address bit doesn't exist, the access wrapped round to the start. */
static int pattern_at(uint64_t off) {
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(DRAM_BASE + off);
    for (int i = 0; i < 16; i++) {
        uint32_t a = DRAM_BASE + (uint32_t)i * 4;
        if (p[i] != ((i & 1) ? ~a : a)) return 0;
    }
    return 1;
}

static void save16(uint32_t *b)          { for (int i = 0; i < 16; i++) b[i] = ram[i]; }
static void restore16(const uint32_t *b) { for (int i = 0; i < 16; i++) ram[i] = b[i]; }

static int check_word(uint64_t addr) {
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)addr;
    uint32_t o = *p;
    *p = ~o;
    uint32_t v = *p;
    *p = o;
    return v == ~o;
}

uint64_t dram_init(void) {
    w32(R_PRCM_RES_CAL, r32(R_PRCM_RES_CAL) | (1u << 8));
    clr32(R_PRCM_OHMS240, 0x3F);

    static const geom_t tries[4] = { { 8, 13, 2, 1 }, { 8, 13, 1, 1 }, { 8, 13, 2, 0 }, { 8, 13, 1, 0 } };
    geom_t g = { 0 };
    int t;
    for (t = 0; t < 4; t++) {
        g = tries[t];
        logf("  dram: trying %d-bit, %d rank%s\n", g.full ? 32 : 16, g.ranks, g.ranks == 2 ? "s" : "");
        if (core_init(&g)) break;
    }
    if (t == 4) { logf("  dram: no setting trains\n"); return 0; }

    /* columns: the most there could be, then see where the pattern repeats */
    uint32_t keep[16];
    g.cols = 11; g.rows = 13;
    if (!core_init(&g)) { logf("  dram: no longer trains (columns)\n"); return 0; }
    save16(keep);
    pattern_write();
    int shift = g.full + 1, cols;
    for (cols = 8; cols < 11; cols++)
        if (pattern_at(1ull << (cols + shift))) break;
    restore16(keep);

    /* rows: the fewest columns and the most rows, the same way */
    g.cols = 8; g.rows = 17;
    if (!core_init(&g)) { logf("  dram: no longer trains (rows)\n"); return 0; }
    save16(keep);
    pattern_write();
    shift = g.full + 4 + g.cols;
    int rows;
    for (rows = 13; rows < 17; rows++)
        if (pattern_at(1ull << (rows + shift))) break;
    restore16(keep);

    g.cols = cols; g.rows = rows;
    if (!core_init(&g)) { logf("  dram: the final setting did not train\n"); return 0; }

    uint64_t size = (1ull << (g.cols + g.rows + 3)) * (g.full ? 4 : 2) * (uint64_t)g.ranks;
    /* 1.5 / 3 GiB boards show up as 2 / 4: the last quarter isn't there */
    if (!check_word(DRAM_BASE + size / 4 * 3)) {
        if (check_word(DRAM_BASE + size / 4 * 3 - 64)) size = size / 4 * 3;
        else { logf("  dram: no RAM at %lx\n", (uint64_t)DRAM_BASE + size / 4 * 3 - 64); return 0; }
    }
    logf("  dram: LPDDR4 %u MHz, %d-bit, %d rank%s, %d columns, %d rows: %lu MiB\n",
         CLK_MHZ, g.full ? 32 : 16, g.ranks, g.ranks == 2 ? "s" : "", g.cols, g.rows, size >> 20);

    /* bus masters' priorities and bandwidth limits, as Allwinner sets them */
    w32(COM_TMR, 399);                             /* 1 us windows                */
    w32(COM_BWCR, 1u << 16);
    static const struct { uint8_t port, prio, qos, acs; uint16_t b0, b1, b2; } mb[] = {
        { 0, 0, 3, 0, 256, 128, 100 },   { 1, 0, 2, 0, 1536, 1400, 256 }, { 2, 0, 3, 0, 512, 256, 96 },
        { 3, 0, 2, 0, 256, 100, 80 },    { 4, 0, 2, 2, 8192, 5500, 5000 }, { 5, 0, 2, 2, 100, 64, 32 },
        { 6, 0, 2, 2, 100, 64, 32 },     { 8, 0, 2, 0, 256, 128, 64 },    { 11, 0, 2, 0, 256, 128, 100 },
        { 14, 0, 2, 0, 1024, 256, 64 },  { 16, 0, 3, 6, 8192, 2800, 2400 }, { 21, 0, 3, 6, 2048, 768, 512 },
        { 22, 0, 2, 0, 256, 128, 100 },  { 25, 1, 3, 0, 100, 64, 32 },    { 26, 0, 2, 2, 8192, 5500, 5000 },
        { 37, 0, 2, 0, 256, 128, 64 },   { 38, 0, 2, 2, 100, 64, 32 },    { 39, 0, 2, 2, 8192, 5500, 5000 },
        { 40, 0, 2, 2, 100, 64, 32 },
    };
    for (unsigned i = 0; i < sizeof mb / sizeof mb[0]; i++) {
        w32(COM_MCFG0(mb[i].port), 1u | (uint32_t)mb[i].prio << 1 | (uint32_t)mb[i].qos << 2 |
                                   (uint32_t)mb[i].acs << 8 | (uint32_t)mb[i].b0 << 16);
        w32(COM_MCFG1(mb[i].port), (uint32_t)mb[i].b2 << 16 | mb[i].b1);
    }
    dsb();
    return size;
}
