/* ===========================================================================
 *  BoltBoot  -  mini/boot/pmic.c
 *
 *  The Zero3's power chip, an X-Powers AXP313A at I2C address 0x36 on the
 *  R_I2C pins PL0 (SCL) and PL1 (SDA). Bit-banged, as mini/hdmi.c does it:
 *  a line is "high" by letting it float (input, the board pulls it up) and
 *  "low" by driving 0. Slow -- a few hundred kHz at best -- and that's fine
 *  for a handful of bytes.
 *
 *  What boot0 needs from it: VDD-CPU (DCDC2) at 1.0 V before the CPU goes to
 *  1008 MHz, and VDD-DRAM (DCDC3) at 1.1 V for the LPDDR4. Its registers:
 *    0x03  chip ID (& 0xC8 = 0x48 on an AXP313A)
 *    0x10  output enables: bit 0..2 DCDC1..3
 *    0x13..0x15  DCDC1..3 voltage: 0.50 V + 10 mV steps up to 1.20 V
 *                (codes 0..70), then 20 mV steps (71..)
 *    0x1A  bit 7: power off
 * ===========================================================================*/
#include "hw.h"
#include "boot0.h"

#define R_PIO      0x07022000u
#define R_PIO_CFG0 (R_PIO + 0x00)
#define R_PIO_DAT  (R_PIO + 0x10)
#define AXP        0x36

static void line(int pin, int high) {
    int sh = pin * 4;
    if (high) clr32(R_PIO_CFG0, 0xFu << sh);                     /* input: floats up */
    else { clr32(R_PIO_DAT, 1u << pin); clrset32(R_PIO_CFG0, 0xFu << sh, 1u << sh); }
}
#define SCL(h) line(0, h)
#define SDA(h) line(1, h)
static int sda(void) { return (r32(R_PIO_DAT) >> 1) & 1; }
static void dly(void) { udelay(4); }

static void start(void) { SDA(1); SCL(1); dly(); SDA(0); dly(); SCL(0); dly(); }
static void stop(void)  { SDA(0); dly(); SCL(1); dly(); SDA(1); dly(); }

static int put(uint8_t b) {
    for (int i = 7; i >= 0; i--) { SDA((b >> i) & 1); dly(); SCL(1); dly(); SCL(0); }
    SDA(1); dly(); SCL(1); dly();
    int ack = !sda();
    SCL(0); dly();
    return ack;
}

static uint8_t get(int ack) {
    uint8_t b = 0;
    SDA(1);
    for (int i = 7; i >= 0; i--) { dly(); SCL(1); dly(); b = (uint8_t)(b << 1 | sda()); SCL(0); }
    SDA(!ack); dly(); SCL(1); dly(); SCL(0); SDA(1);
    return b;
}

int pmic_read(uint8_t reg, uint8_t *v) {
    start();
    if (!put(AXP << 1) || !put(reg)) { stop(); return -1; }
    start();
    if (!put(AXP << 1 | 1)) { stop(); return -1; }
    *v = get(0);
    stop();
    return 0;
}

int pmic_write(uint8_t reg, uint8_t v) {
    start();
    int ok = put(AXP << 1) && put(reg) && put(v);
    stop();
    return ok ? 0 : -1;
}

static uint8_t mv_code(unsigned mv) {
    if (mv < 500) mv = 500;
    if (mv <= 1200) return (uint8_t)((mv - 500) / 10);
    if (mv > 1540) mv = 1540;
    return (uint8_t)(70 + (mv - 1200) / 20);
}

/* DCDC n (1..3) at mv millivolts, and on. */
int pmic_dcdc(int n, unsigned mv) {
    uint8_t en;
    if (pmic_write((uint8_t)(0x12 + n), mv_code(mv)) || pmic_read(0x10, &en)) return -1;
    return pmic_write(0x10, en | (uint8_t)(1u << (n - 1)));
}

int pmic_init(void) {
    uint8_t id;
    if (pmic_read(0x03, &id)) { logf("  pmic: no answer at 0x36\n"); return -1; }
    if ((id & 0xC8) != 0x48) { logf("  pmic: chip ID %02x, not an AXP313A\n", id); return -1; }
    int bad = pmic_dcdc(2, 1000) | pmic_dcdc(3, 1100);
    uint8_t d2 = 0, d3 = 0, en = 0;
    pmic_read(0x14, &d2); pmic_read(0x15, &d3); pmic_read(0x10, &en);
    logf("  pmic: AXP313A (id %02x), VDD-CPU %u mV, VDD-DRAM %u mV, enables %02x%s\n",
         id, 500 + d2 * 10u, 500 + d3 * 10u, en, bad ? " -- a write failed" : "");
    return bad;
}

void pmic_power_off(void) { uint8_t v = 0; pmic_read(0x1A, &v); pmic_write(0x1A, v | 0x80); }
