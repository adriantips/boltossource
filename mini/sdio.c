/* ===========================================================================
 *  BoltOS Mini  -  mini/sdio.c
 *
 *  The H618's second SD/MMC controller (MMC1) as an SDIO host, for the
 *  Orange Pi Zero3's Wi-Fi and Bluetooth module (a Unisoc UWE5622, wcn.c).
 *
 *  The module sits on PG0-PG5 at 1.8 V, wants the RTC's 32 kHz clock as its
 *  sleep clock, and comes out of reset when PG18 goes high. The controller
 *  is Allwinner's (a DesignWare MMC relative). Every transfer goes through
 *  its FIFO by the CPU, as U-Boot does it: no DMA descriptors, and the
 *  module's packets are a few kilobytes at most.
 *
 *  The card: CMD5 until it's ready, CMD3 for its address, CMD7 to select
 *  it, then 4-bit bus, function 1 on, function 1's block size. After that
 *  everything is CMD52 (a byte) and CMD53 (bytes or blocks).
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "sdio.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

#define MMC1        0x04021000u
#define CCU         0x03001000u
#define PIO         0x0300B000u
#define RTC         0x07000000u

#define SMHC1_CLK   (CCU + 0x834)
#define SMHC_BGR    (CCU + 0x84C)
#define PLL_PERI0   (CCU + 0x020)

/* Controller registers */
#define GCTRL   0x00
#define CLKCR   0x04
#define TMOUT   0x08
#define CTYPE   0x0C
#define BLKSZ   0x10
#define BCNT    0x14
#define CMDR    0x18
#define CARG    0x1C
#define RESP0   0x20
#define IMASK   0x30
#define RINT    0x38
#define STAS    0x3C
#define FTRGL   0x40
#define FUNS    0x44
#define DBGC    0x50
#define NTSR    0x5C
#define SAMPDL  0x144
#define FIFO    0x200

#define G_RESET       0x7u               /* soft, FIFO and DMA resets */
#define G_FIFO_RESET  (1u << 1)
#define G_AHB         (1u << 31)         /* the CPU reads and writes the FIFO */
#define C_ON          (1u << 16)
#define C_LOWPOWER    (1u << 17)

#define CMD_RESP      (1u << 6)
#define CMD_LONG      (1u << 7)
#define CMD_CRC       (1u << 8)
#define CMD_DATA      (1u << 9)
#define CMD_WRITE     (1u << 10)
#define CMD_WAITPRE   (1u << 13)
#define CMD_INIT      (1u << 15)
#define CMD_UPCLK     (1u << 21)
#define CMD_START     (1u << 31)

#define I_RESP_ERR    (1u << 1)
#define I_CMD_DONE    (1u << 2)
#define I_DATA_OVER   (1u << 3)
#define I_RESP_CRC    (1u << 6)
#define I_DATA_CRC    (1u << 7)
#define I_RESP_TO     (1u << 8)
#define I_DATA_TO     (1u << 9)
#define I_FIFO_ERR    (1u << 11)
#define I_START_ERR   (1u << 13)
#define I_END_ERR     (1u << 15)
#define I_SDIO        (1u << 16)
#define I_ERRORS      (I_RESP_ERR | I_RESP_CRC | I_DATA_CRC | I_RESP_TO | I_DATA_TO | I_FIFO_ERR | I_START_ERR | I_END_ERR)

#define S_FIFO_EMPTY  (1u << 2)
#define S_FIFO_FULL   (1u << 3)
#define S_BUSY        (1u << 9)

static inline uint32_t mr(uint32_t r) { return r32(MMC1 + r); }
static inline void mw(uint32_t r, uint32_t v) { w32(MMC1 + r, v); }

static struct {
    int up;
    uint16_t rca;
    uint32_t ocr, hz;
    uint16_t blksz[2];
    uint32_t errors, last_err;
    int last_cmd;
} H;

/* ---- pins, power and the 32 kHz clock -------------------------------------------------- */
#define BANK_G 6
static void pio_func(int bank, int pin, int f) {
    uintptr_t r = PIO + bank * 0x24u + (pin / 8) * 4u;
    int sh = (pin % 8) * 4;
    w32(r, (r32(r) & ~(0xFu << sh)) | ((uint32_t)f << sh));
}
static void pio_pull(int bank, int pin, int p) {            /* 1 up, 2 down */
    uintptr_t r = PIO + bank * 0x24u + 0x1C + (pin / 16) * 4u;
    int sh = (pin % 16) * 2;
    w32(r, (r32(r) & ~(3u << sh)) | ((uint32_t)p << sh));
}
static void pio_drive(int bank, int pin, int lvl) {
    uintptr_t r = PIO + bank * 0x24u + 0x14 + (pin / 16) * 4u;
    int sh = (pin % 16) * 2;
    w32(r, (r32(r) & ~(3u << sh)) | ((uint32_t)lvl << sh));
}
void h6_gpio_out(int bank, int pin, int val);

/* The module's reset line: low holds it in reset. */
void sdio_module_reset(int hold) { h6_gpio_out(BANK_G, 18, !hold); }

static void power_up(void) {
    sdio_module_reset(1);
    /* The 32 kHz clock: the RTC's fanout, from the 24 MHz crystal / 750 (accurate,
     * unlike the RC oscillator), out on PG10 (X32KFOUT). */
    w32(RTC + 0x60, (1u << 16) | (2u << 1) | 1u);
    pio_func(BANK_G, 10, 3);
    /* Bank G runs at 1.8 V on this board (vcc-wifi-io): tell the pads. */
    set32(PIO + 0x340, 1u << BANK_G);
    clr32(PIO + 0x344, 1u << BANK_G);
    for (int p = 0; p <= 5; p++) { pio_func(BANK_G, p, 2); pio_pull(BANK_G, p, 1); pio_drive(BANK_G, p, 2); }
    delay_ms(10);
    sdio_module_reset(0);
    delay_ms(200);                                          /* the device tree's post-power-on delay */
}

/* ---- the controller ------------------------------------------------------------------------ */
static int update_clock(void) {
    mw(CMDR, CMD_START | CMD_UPCLK | CMD_WAITPRE);
    for (int i = 0; i < 2000; i++) {
        if (!(mr(CMDR) & CMD_START)) { mw(RINT, mr(RINT)); return 0; }
        delay_us(100);
    }
    kprintf("[sdio] the clock didn't change\n");
    return -1;
}

/* The card's clock: the module clock / M / 2^N, then / 2 inside the controller
 * (its "new timing" mode, the only one this controller has). */
static int set_clock(uint32_t hz) {
    mw(CLKCR, mr(CLKCR) & ~C_ON);
    if (update_clock()) return -1;
    uint32_t src, sel;
    if (hz <= 400000) { src = 24000000; sel = 0; }
    else {
        uint32_t v = r32(PLL_PERI0);
        uint32_t n = ((v >> 8) & 0xFF) + 1, m = ((v >> 1) & 1) + 1, p0 = ((v >> 16) & 7) + 1;
        src = 24000000u / m * n / p0;                       /* PLL_PERI0(2X), 1200 MHz */
        sel = 1;
    }
    uint32_t div = (src / 2 + hz - 1) / hz, n = 0;
    while (div > 16) { n++; div = (div + 1) / 2; }
    if (n > 3) return -1;
    w32(SMHC1_CLK, (1u << 31) | (sel << 24) | (n << 8) | (div - 1));
    H.hz = src / 2 / div >> n;
    set32(MMC1 + NTSR, 1u << 31);
    mw(SAMPDL, 1u << 7);                                    /* delays by software, at 0: as Linux does */
    mw(CLKCR, (mr(CLKCR) & ~0xFFu & ~C_LOWPOWER) | C_ON);   /* always on: the module signals on DAT1 */
    return update_clock();
}

static void host_reset(void) {
    mw(GCTRL, G_RESET);
    for (int i = 0; i < 1000 && (mr(GCTRL) & G_RESET); i++) delay_us(10);
    mw(FTRGL, 0x20070008u);
    mw(TMOUT, 0xFFFFFFFFu);
    mw(IMASK, 0);
    mw(RINT, 0xFFFFFFFFu);
    mw(DBGC, 0xDEB);
    mw(FUNS, 0xCEAAu << 16);
    mw(GCTRL, mr(GCTRL) | G_AHB);
}

static int wait_rint(uint32_t bits, uint32_t ms) {
    uint64_t end = now_ms() + ms;
    for (;;) {
        uint32_t r = mr(RINT);
        if (r & I_ERRORS) return -(int)(r & I_ERRORS);
        if ((r & bits) == bits) return 0;
        if (now_ms() > end) return -0x10000;
    }
}

/* A command, with an optional data stage through the FIFO. 0, or why not. */
static int command(int idx, uint32_t arg, uint32_t flags, uint32_t *resp,
                   void *data, uint32_t blksz, uint32_t len, int write) {
    for (int i = 0; i < 10000 && (mr(STAS) & S_BUSY); i++) delay_us(10);
    mw(RINT, 0xFFFFFFFFu);
    uint32_t c = CMD_START | flags | (uint32_t)idx;
    if (idx == 0) c |= CMD_INIT;
    if (data) {
        c |= CMD_DATA | CMD_WAITPRE | (write ? CMD_WRITE : 0);
        mw(BLKSZ, blksz);
        mw(BCNT, len);
    }
    mw(CARG, arg);
    mw(CMDR, c);
    int err = 0;
    if (data) {
        uint32_t *w = data, words = (len + 3) / 4;
        uint64_t end = now_ms() + 1000;
        for (uint32_t i = 0; i < words && !err; ) {
            uint32_t r = mr(RINT);
            if (r & I_ERRORS) { err = -(int)(r & I_ERRORS); break; }
            uint32_t s = mr(STAS);
            if (write) {
                if (s & S_FIFO_FULL) { if (now_ms() > end) err = -0x20000; continue; }
                mw(FIFO, w[i++]);
            } else {
                uint32_t lvl = (s >> 17) & 0x3FFF;
                if (!lvl) { if (now_ms() > end) err = -0x20000; continue; }
                while (lvl-- && i < words) w[i++] = mr(FIFO);
            }
        }
    }
    if (!err) err = wait_rint(I_CMD_DONE, 200);
    if (!err && data) err = wait_rint(I_DATA_OVER, 500);
    if (!err && resp) *resp = mr(RESP0);
    if (err) {
        H.errors++; H.last_err = (uint32_t)-err; H.last_cmd = idx;
        mw(GCTRL, mr(GCTRL) | G_RESET);
        for (int i = 0; i < 1000 && (mr(GCTRL) & G_RESET); i++) delay_us(10);
        mw(GCTRL, mr(GCTRL) | G_AHB);
        update_clock();
    }
    mw(RINT, 0xFFFFFFFFu & ~I_SDIO);
    mw(GCTRL, mr(GCTRL) | G_FIFO_RESET);
    return err;
}

/* ---- SDIO ---------------------------------------------------------------------------------- */
int sdio_cmd52(int write, int fn, uint32_t addr, uint8_t in, uint8_t *out) {
    uint32_t arg = (write ? 1u << 31 : 0) | ((uint32_t)fn << 28) | ((addr & 0x1FFFF) << 9) | in;
    uint32_t r = 0;
    int err = command(52, arg, CMD_RESP | CMD_CRC, &r, 0, 0, 0, 0);
    if (err) return err;
    if (r & 0xCB00) { H.errors++; H.last_err = r; H.last_cmd = 52; return -0x40000; }   /* CRC, illegal, error, function, range */
    if (out) *out = (uint8_t)r;
    return 0;
}

uint8_t sdio_readb(int fn, uint32_t addr, int *err) {
    uint8_t v = 0;
    int e = sdio_cmd52(0, fn, addr, 0, &v);
    if (err && e) *err = e;
    return v;
}
int sdio_writeb(int fn, uint32_t addr, uint8_t v) { return sdio_cmd52(1, fn, addr, v, 0); }

/* CMD53: whole blocks while there are, then the rest (up to 512 bytes at a time) in
 * byte mode. `incr` steps the card's address along; 0 keeps it (a FIFO port). */
int sdio_rw(int write, int fn, uint32_t addr, int incr, void *buf, uint32_t len) {
    uint8_t *p = buf;
    uint32_t bs = H.blksz[fn & 1] ? H.blksz[fn & 1] : 512;
    while (len) {
        uint32_t nb = len / bs, n, arg;
        if (nb) {
            if (nb > 511) nb = 511;
            n = nb * bs;
            arg = (1u << 27) | nb;
        } else {
            n = len > 512 ? 512 : len;
            arg = n == 512 ? 0 : n;
        }
        arg |= (write ? 1u << 31 : 0) | ((uint32_t)fn << 28) | (incr ? 1u << 26 : 0) | ((addr & 0x1FFFF) << 9);
        int err = command(53, arg, CMD_RESP | CMD_CRC, 0, p, nb ? bs : n, n, write);
        if (err) return err;
        p += n; len -= n;
        if (incr) addr += n;
    }
    return 0;
}

int sdio_set_blksz(int fn, uint16_t bs) {
    int e = sdio_writeb(0, (uint32_t)fn * 0x100 + 0x10, (uint8_t)bs);
    if (!e) e = sdio_writeb(0, (uint32_t)fn * 0x100 + 0x11, (uint8_t)(bs >> 8));
    if (!e) H.blksz[fn & 1] = bs;
    return e;
}

/* The module pulled DAT1 low: it has something for us. */
int sdio_irq_pending(void) {
    if (!H.up) return 0;
    if (!(mr(RINT) & I_SDIO)) return 0;
    mw(RINT, I_SDIO);
    return 1;
}

int sdio_init(void) {
    if (g_plat != PLAT_H618) return -1;
    memset(&H, 0, sizeof H);
    /* Clocks and reset for MMC1: bus gate bit 1, reset bit 17. */
    clr32(SMHC_BGR, 1u << 17);
    clr32(SMHC_BGR, 1u << 1);
    power_up();
    w32(SMHC1_CLK, (1u << 31) | (1u << 8) | 14u);          /* 24 MHz / 15 / 2: the controller needs a clock to reset */
    set32(SMHC_BGR, 1u << 1);
    set32(SMHC_BGR, 1u << 17);
    delay_us(100);
    host_reset();
    mw(CTYPE, 0);
    if (set_clock(400000)) return -1;

    command(0, 0, 0, 0, 0, 0, 0, 0);                        /* 74+ clocks, then idle */
    delay_ms(2);
    uint32_t r = 0;
    int err = command(5, 0, CMD_RESP, &r, 0, 0, 0, 0);
    if (err) { kprintf("[sdio] no card answers CMD5 (%x)\n", (unsigned)-err); return -1; }
    H.ocr = r & 0x00FFFFFFu;
    int nfn = (int)(r >> 28) & 7;
    uint64_t end = now_ms() + 1000;
    do {
        err = command(5, H.ocr & 0x00FF8000u, CMD_RESP, &r, 0, 0, 0, 0);
        if (!err && (r >> 31)) break;
        delay_ms(10);
    } while (now_ms() < end);
    if (err || !(r >> 31)) { kprintf("[sdio] the card never became ready (ocr %x)\n", (unsigned)r); return -1; }
    if (command(3, 0, CMD_RESP | CMD_CRC, &r, 0, 0, 0, 0)) { kprintf("[sdio] CMD3 failed\n"); return -1; }
    H.rca = (uint16_t)(r >> 16);
    if (command(7, (uint32_t)H.rca << 16, CMD_RESP | CMD_CRC, &r, 0, 0, 0, 0)) { kprintf("[sdio] CMD7 failed\n"); return -1; }
    H.up = 1;

    uint8_t cccr = sdio_readb(0, 0x00, &err), sdrev = sdio_readb(0, 0x01, &err), cap = sdio_readb(0, 0x08, &err);
    uint8_t hs = sdio_readb(0, 0x13, &err);
    if (err) { kprintf("[sdio] reading the card's registers failed\n"); H.up = 0; return -1; }
    /* 4-bit bus, then the faster clock (high speed if the card has it). */
    sdio_writeb(0, 0x07, (uint8_t)((sdio_readb(0, 0x07, 0) & ~3) | 2));
    mw(CTYPE, 1);
    uint32_t speed = 25000000;
    if (hs & 1) { sdio_writeb(0, 0x13, (uint8_t)(hs | 2)); speed = 50000000; }
    set_clock(speed);
    kprintf("[sdio] card at %u kHz, 4-bit: rca %04x, ocr %06x, %d functions, CCCR %x SD %x cap %02x%s\n",
            (unsigned)(H.hz / 1000), H.rca, (unsigned)H.ocr, nfn, cccr, sdrev, cap, (hs & 1) ? ", high speed" : "");
    return 0;
}

int sdio_enable_fn(int fn) {
    uint8_t v = sdio_readb(0, 0x02, 0);
    if (sdio_writeb(0, 0x02, (uint8_t)(v | (1 << fn)))) return -1;
    uint64_t end = now_ms() + 500;
    while (now_ms() < end) {
        if (sdio_readb(0, 0x03, 0) & (1 << fn)) return 0;
        delay_ms(1);
    }
    return -1;
}

/* Interrupts from the card on DAT1: master and function `fn`. */
int sdio_enable_irq(int fn) {
    return sdio_writeb(0, 0x04, (uint8_t)(sdio_readb(0, 0x04, 0) | 1 | (1 << fn)));
}

int sdio_debug(char *out, int cap) {
    return snprintf(out, (size_t)cap, "sdio: %s, %u kHz, rca %04x, errors %u (last cmd %d: %x), rint %08x stas %08x clk %08x\n",
                    H.up ? "up" : "down", (unsigned)(H.hz / 1000), H.rca, (unsigned)H.errors, H.last_cmd,
                    (unsigned)H.last_err, (unsigned)mr(RINT), (unsigned)mr(STAS), (unsigned)r32(SMHC1_CLK));
}
