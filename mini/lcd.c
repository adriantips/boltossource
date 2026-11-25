/* ===========================================================================
 *  BoltOS Mini  -  mini/lcd.c
 *
 *  A small SPI screen on the Zero3's 26-pin header: the 3.5" 480 x 320
 *  modules with a Sitronix ST7796S sold for Arduino and STM32 boards, and
 *  the XPT2046 resistive touch some of them carry on the same SPI bus.
 *
 *      module    Zero3 header             module    Zero3 header
 *      3V3   ->  17  3.3 V                GND   ->  20  ground
 *      SCK   ->  23  PH6  SPI1 clock      MOSI  ->  19  PH7  SPI1 out
 *      MISO  ->  21  PH8  SPI1 in         CS    ->  24  PH9
 *      RS    ->  22  PC7  data/command    RST   ->  18  PC14 (optional)
 *      BL    ->  16  PC15 backlight       T_CS  ->  26  PC10 (touch only)
 *
 *  The picture: what changed in front (screen.c) since the last look, by
 *  g_tile_gen as a VNC viewer does it, goes across as rectangles: a window
 *  (CASET, RASET), RAMWR, then the pixels as big-endian RGB565. The pixels
 *  are converted into a buffer and the H618's DMA controller feeds them to
 *  SPI1's FIFO, so a turn of the main loop only starts a rectangle and a
 *  later one notices it ended. A screen of another size (a monitor's, or one
 *  chosen for Screen Sharing) is shown scaled to fit, with black bars.
 *
 *  Is one there? After a reset the controller says it is asleep (RDDPM bit
 *  4 clear) and after SLPOUT that it is awake. MISO has a pull-up, so with
 *  nothing on the header both reads are 0xFF. Settings can force it on (a
 *  module wired without MISO) or off (leave the header's pins alone).
 *
 *  Touch: an XPT2046 answers every conversion as 0, twelve bits, then three
 *  0s; nothing on T_CS reads all ones. It is polled every 15 ms between
 *  rectangles, at 2 MHz, and its finger goes to input_touch() like a USB
 *  touchscreen's. Settings has a three-cross calibration.
 *
 *  /lcd (developer) shows the state and the touch's raw readings, and sets
 *  what a particular module may need: ?inv=0 (a panel that shows a negative
 *  with inversion on, which is the default), ?rgb=1 (red and blue swapped),
 *  ?madctl=HEX (mirrored), ?mhz=N.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kprintf.h"
#include "kheap.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define CCU      0x03001000u
#define PIO       0x0300B000u
#define DMAC      0x03002000u
#define SPI       0x05011000u                  /* SPI1                         */

/* SPI1 (the H3/H6 layout Linux's sun6i-spi knows) */
#define SPI_GCR   (SPI + 0x04)
#define SPI_TCR   (SPI + 0x08)
#define SPI_IER   (SPI + 0x10)
#define SPI_ISR   (SPI + 0x14)
#define SPI_FCR   (SPI + 0x18)
#define SPI_FSR   (SPI + 0x1C)
#define SPI_CCR   (SPI + 0x24)
#define SPI_MBC   (SPI + 0x30)
#define SPI_MTC   (SPI + 0x34)
#define SPI_BCC   (SPI + 0x38)
#define SPI_TXD   (SPI + 0x200)
#define SPI_RXD   (SPI + 0x300)
#define GCR_EN     (1u << 0)
#define GCR_MASTER (1u << 1)
#define GCR_TP     (1u << 7)
#define GCR_SRST   (1u << 31)
#define TCR_SS_OWNER (1u << 6)                 /* SS is ours (it's a GPIO anyway) */
#define TCR_SS_LEVEL (1u << 7)
#define TCR_DHB    (1u << 8)                   /* send only: nothing into the RX FIFO */
#define TCR_SDM    (1u << 13)                  /* sample on the edge (slow reads)  */
#define TCR_XCH    (1u << 31)
#define ISR_TC     (1u << 12)
#define FCR_RF_RST (1u << 15)
#define FCR_TF_DRQ (1u << 24)
#define FCR_TF_RST (1u << 31)
#define FCR_LEVELS ((32u << 16) | 32u)        /* DMA asks when 32 bytes are free  */

/* The DMA controller (H6/H616, Linux's sun6i-dma) */
#define DMA_CH     0
#define DMA_PEND0  (DMAC + 0x10)
#define DMA_GATE   (DMAC + 0x28)
#define DMA_STA    (DMAC + 0x30)
#define DMA_EN     (DMAC + 0x100 + DMA_CH * 0x40)
#define DMA_DESC   (DMA_EN + 0x08)
#define DMA_LEFT   (DMA_EN + 0x18)
#define DRQ_DRAM   1
#define DRQ_SPI1   23
#define DMA_LAST   0xFFFFF800u

/* The pins: bank, number. SCK, MOSI and MISO are PH6..PH8, function 4. */
typedef struct { uint8_t bank, pin; } gpio_t;
enum { BANK_C = 2, BANK_H = 7 };
static const gpio_t P_SCK = { BANK_H, 6 }, P_MOSI = { BANK_H, 7 }, P_MISO = { BANK_H, 8 };
static const gpio_t P_CS = { BANK_H, 9 }, P_DC = { BANK_C, 7 }, P_RST = { BANK_C, 14 };
static const gpio_t P_BL = { BANK_C, 15 }, P_TCS = { BANK_C, 10 };
#define BANK(b) (PIO + (uint32_t)(b) * 0x24u)

static void pin_func(gpio_t g, int f) {
    uintptr_t a = BANK(g.bank) + (g.pin / 8u) * 4u;
    int sh = (g.pin % 8) * 4;
    w32(a, (r32(a) & ~(0xFu << sh)) | ((uint32_t)f << sh));
}
static void pin_bits2(gpio_t g, uint32_t off, int v) {       /* drive (0x14) or pull (0x1C) */
    uintptr_t a = BANK(g.bank) + off + (g.pin / 16u) * 4u;
    int sh = (g.pin % 16) * 2;
    w32(a, (r32(a) & ~(3u << sh)) | ((uint32_t)v << sh));
}
static void pin_set(gpio_t g, int v) {
    uintptr_t a = BANK(g.bank) + 0x10;
    if (v) set32(a, 1u << g.pin); else clr32(a, 1u << g.pin);
    (void)r32(a);                                            /* on the pin before the SPI moves */
}
static void pin_out(gpio_t g, int v) { pin_set(g, v); pin_func(g, 1); }

/* ---- what's kept on the card ------------------------------------------------------------ */
typedef struct {
    uint8_t ver, mode;                 /* mode: 0 Automatic, 1 On, 2 Off              */
    uint8_t flip, inv, rgb, mhz;       /* upside down, inverted, RGB order, SPI clock */
    uint8_t swap, madctl;              /* touch: x from the other reading; MADCTL override */
    int16_t cal[4];                    /* touch: raw x at left, right; raw y at top, bottom */
} lcd_cfg_t;
/* Inverted by default: the TK035F5501 panel on these modules shows a
 * negative without INVON (seen on the Pi, 2026-11-24). */
static lcd_cfg_t cfg = { 1, 0, 0, 1, 0, 30, 0, 0, { 200, 3900, 200, 3900 } };

static void cfg_save(void) { keep_put("lcd", &cfg, (int)sizeof cfg); }
static int cfg_load(lcd_cfg_t *c) {
    if (keep_get("lcd", c, (int)sizeof *c) != (int)sizeof *c || c->ver != 1 || c->mode > 2) return 0;
    if (c->mhz < 10 || c->mhz > 50) c->mhz = 30;
    return 1;
}

/* ---- the state ---------------------------------------------------------------------------- */
static int hw;                         /* clocks, pins and SPI1 set up                */
static int up;                         /* the panel is ours: being drawn on            */
static int found;                      /* it answered the probe                        */
static int lit;                        /* DISPON and the backlight on                  */
static int asleep;                     /* SLPIN for screen sleep                       */
static int need_on;                    /* light it once the batch that draws everything is out */
static int touch;                      /* an XPT2046 answered                          */
static int use_pio;                    /* DMA didn't finish once: the FIFO by hand from then on */
static uint8_t probe_a[2], probe_b[2]; /* RDDPM after reset and after SLPOUT, both ways */
static uint32_t mod_hz, fast_hz;       /* SPI1's module clock; the pixel clock          */
static uint64_t wake_at, hold_until;

static uint32_t sent_gen;
static int full;                       /* the next look takes everything               */
static int src_w, src_h;               /* the screen size the maps are for             */
static int ox, oy, dw, dh;             /* where the screen sits on the panel           */
static int16_t xmap[LCD_W], ymap[LCD_H];

typedef struct { int16_t x, y, w, h; } lrect_t;
#define QMAX 48
static lrect_t q[QMAX];
static int qn, qi, batch_full;

static uint16_t *buf;                  /* RGB565 big-endian: a whole panel and one more pixel */
static uint32_t *desc;                 /* the DMA descriptor, a cache line of its own  */
enum { X_IDLE, X_DMA, X_PIO };
static int xfer, xfer_bytes, xfer_off;
static uint64_t xfer_t0, xfer_moved;   /* started; PIO last got bytes into the FIFO */
static struct { uint32_t rects, frames, errors; uint64_t bytes, busy_us; } st;

/* ---- SPI1 ------------------------------------------------------------------------------------ */
static uint32_t peri1x_hz(void) {                          /* PLL_PERI0(1X), 600 MHz       */
    uint32_t v = r32(CCU + 0x020);
    uint32_t n = ((v >> 8) & 0xFF) + 1, m = ((v >> 1) & 1) + 1, p0 = ((v >> 16) & 7) + 1;
    return 24000000u / m * n / p0 / 2;
}

/* The module clock is set for the pixels: SPI clock = module / 2. Slower
 * transfers (reads, the touch) divide it further in CCR. */
static void spi_clock_setup(void) {
    uint32_t src = peri1x_hz(), want = (uint32_t)cfg.mhz * 1000000u;
    uint32_t div = (src + 2 * want - 1) / (2 * want);       /* module = src / div, at most 100 MHz */
    if (div < 6) div = 6;
    if (div > 16) div = 16;
    w32(CCU + 0x944, (1u << 31) | (1u << 24) | (div - 1));   /* SPI1_CLK: on, PLL_PERI0(1X), M */
    mod_hz = src / div;
    fast_hz = mod_hz / 2;
}

static void spi_speed(uint32_t hz) {                       /* hz 0: the pixel clock         */
    uint32_t n = 1;
    if (hz) { n = (mod_hz + 2 * hz - 1) / (2 * hz); if (n < 1) n = 1; if (n > 256) n = 256; }
    w32(SPI_CCR, (1u << 12) | (n - 1));                     /* DRS: module / (2 * (CDR2 + 1)) */
}

static int spi_done(void) { return (r32(SPI_ISR) & ISR_TC) || !(r32(SPI_TCR) & TCR_XCH); }

static void spi_counts(int n) { w32(SPI_MBC, (uint32_t)n); w32(SPI_MTC, (uint32_t)n); w32(SPI_BCC, (uint32_t)n); }

/* Up to 64 bytes out, and as many in when rx isn't 0. */
static int spi_pio(const uint8_t *tx, uint8_t *rx, int n, int sdm) {
    w32(SPI_FCR, FCR_TF_RST | FCR_RF_RST | FCR_LEVELS);
    w32(SPI_ISR, ~0u);
    uint32_t t = TCR_SS_OWNER | TCR_SS_LEVEL | (rx ? 0 : TCR_DHB) | (sdm ? TCR_SDM : 0);
    w32(SPI_TCR, t);
    spi_counts(n);
    for (int i = 0; i < n; i++) w8(SPI_TXD, tx ? tx[i] : 0);
    w32(SPI_TCR, t | TCR_XCH);
    uint64_t t0 = now_us();
    while (!spi_done())
        if (now_us() - t0 > 5000) { st.errors++; return -1; }
    if (rx) for (int i = 0; i < n; i++) rx[i] = r8(SPI_RXD);
    return 0;
}

/* ---- the panel's commands ------------------------------------------------------------------- */
static void cmd(uint8_t c, const uint8_t *p, int n) {
    pin_set(P_CS, 0);
    pin_set(P_DC, 0);
    spi_pio(&c, 0, 1, 0);
    pin_set(P_DC, 1);
    while (n > 0) { int k = n > 64 ? 64 : n; spi_pio(p, 0, k, 0); p += k; n -= k; }
    pin_set(P_CS, 1);
}
static void cmd0(uint8_t c) { cmd(c, 0, 0); }
static void cmd1(uint8_t c, uint8_t v) { cmd(c, &v, 1); }

/* RDDPM (0Ah): the power mode. Read slowly, and both as it comes and one bit
 * later (some controllers put a dummy clock before a read's data). */
static void rddpm(uint8_t out[2]) {
    uint8_t tx[3] = { 0x0A, 0, 0 }, rx[3] = { 0 };
    spi_speed(6000000);
    pin_set(P_CS, 0);
    pin_set(P_DC, 0);
    spi_pio(tx, rx, 3, 1);
    pin_set(P_CS, 1);
    pin_set(P_DC, 1);
    spi_speed(0);
    out[0] = rx[1];
    out[1] = (uint8_t)((((uint32_t)rx[1] << 8 | rx[2]) >> 7) & 0xFF);
}

static uint8_t madctl(void) {
    if (cfg.madctl) return cfg.madctl;
    return (uint8_t)((cfg.flip ? 0xE0 : 0x20) | (cfg.rgb ? 0 : 0x08));   /* MV (+MX MY), BGR */
}

static void window(int x, int y, int w, int h) {
    uint8_t c[4] = { (uint8_t)(x >> 8), (uint8_t)x, (uint8_t)((x + w - 1) >> 8), (uint8_t)(x + w - 1) };
    uint8_t r[4] = { (uint8_t)(y >> 8), (uint8_t)y, (uint8_t)((y + h - 1) >> 8), (uint8_t)(y + h - 1) };
    cmd(0x2A, c, 4);
    cmd(0x2B, r, 4);
}

/* The ST7796S's own power-up settings (Sitronix's, as TFT_eSPI sends them):
 * command, count, parameters. The command set is unlocked around them. */
static const uint8_t init_seq[] = {
    0xF0, 1, 0xC3,
    0xF0, 1, 0x96,
    0x3A, 1, 0x55,                                        /* 16 bits a pixel             */
    0xB4, 1, 0x01,                                        /* 1-dot inversion             */
    0xB6, 3, 0x80, 0x02, 0x3B,
    0xE8, 8, 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33,
    0xC1, 1, 0x06,
    0xC2, 1, 0xA7,
    0xC5, 1, 0x18,                                        /* VCOM                        */
    0xE0, 14, 0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B,
    0xE1, 14, 0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B,
    0xF0, 1, 0x3C,
    0xF0, 1, 0x69,
};

static void panel_settings(void) {
    for (unsigned i = 0; i < sizeof init_seq; i += 2u + init_seq[i + 1])
        cmd(init_seq[i], &init_seq[i + 2], init_seq[i + 1]);
    cmd1(0x36, madctl());
    cmd0(cfg.inv ? 0x21 : 0x20);
}

static void light(int on) {
    if (on) { cmd0(0x29); pin_set(P_BL, 1); }
    else { pin_set(P_BL, 0); cmd0(0x28); }
    lit = on;
}

/* ---- bring-up ------------------------------------------------------------------------------- */
static void hw_setup(void) {
    if (hw) return;
    set32(CCU + 0x70C, 1u << 16); set32(CCU + 0x70C, 1u);   /* DMA: out of reset, clock on */
    set32(CCU + 0x804, 1u);                                  /* and its way to DRAM (MBUS)  */
    w32(DMA_GATE, 4);                                        /* as Linux does for the H3/H6 */
    clr32(CCU + 0x96C, (1u << 17) | (1u << 1));              /* SPI1: reset, clock off      */
    spi_clock_setup();
    set32(CCU + 0x96C, 1u << 17); set32(CCU + 0x96C, 1u << 1);
    delay_us(10);

    pin_func(P_SCK, 4); pin_func(P_MOSI, 4); pin_func(P_MISO, 4);
    pin_bits2(P_MISO, 0x1C, 1);                              /* pull-up: nothing there reads 1s */
    pin_bits2(P_SCK, 0x14, 2); pin_bits2(P_MOSI, 0x14, 2);   /* a little more drive for 30 MHz  */
    pin_bits2(P_CS, 0x14, 2); pin_bits2(P_DC, 0x14, 2);
    pin_out(P_CS, 1); pin_out(P_TCS, 1); pin_out(P_DC, 1);
    pin_out(P_RST, 1); pin_out(P_BL, 0);

    w32(SPI_GCR, GCR_SRST);
    for (int i = 0; i < 1000 && (r32(SPI_GCR) & GCR_SRST); i++) delay_us(1);
    w32(SPI_GCR, GCR_EN | GCR_MASTER | GCR_TP);
    w32(SPI_IER, ISR_TC);                                    /* latched; the GIC never hears it */
    w32(SPI_TCR, TCR_SS_OWNER | TCR_SS_LEVEL);
    w32(SPI_FCR, FCR_TF_RST | FCR_RF_RST | FCR_LEVELS);
    spi_speed(0);
    hw = 1;
}

/* Nothing on the header after all: its pins back to how U-Boot left them. */
static void hw_release(void) {
    if (!hw) return;
    const gpio_t pins[] = { P_SCK, P_MOSI, P_MISO, P_CS, P_DC, P_RST, P_BL, P_TCS };
    for (unsigned i = 0; i < sizeof pins / sizeof pins[0]; i++) pin_func(pins[i], 7);
    pin_bits2(P_MISO, 0x1C, 0);
    clr32(CCU + 0x96C, 1u << 1);
    hw = 0;
}

/* A reset, then is a controller there? Leaves it awake (SLPOUT). With
 * force, carries on whatever the reads say. */
static int probe(int force) {
    pin_set(P_CS, 1);
    pin_set(P_RST, 0); delay_us(50); pin_set(P_RST, 1);      /* if RST is wired             */
    delay_ms(5);
    cmd0(0x01);                                              /* SWRESET                     */
    uint64_t t = now_ms();
    delay_ms(8);
    rddpm(probe_a);
    int nothing = (probe_a[0] == 0xFF && probe_a[1] == 0xFF) || (probe_a[0] == 0 && probe_a[1] == 0);
    if (nothing && !force) return 0;
    while (now_ms() - t < 125) delay_ms(1);                  /* after a reset from awake     */
    cmd0(0x11);                                              /* SLPOUT                      */
    delay_ms(8);
    rddpm(probe_b);
    for (int k = 0; k < 2; k++)
        if (!(probe_a[k] & 0x10) && (probe_b[k] & 0x10) && probe_b[k] != 0xFF) return 1;
    return 0;
}

/* ---- touch: XPT2046 --------------------------------------------------------------------------- *
 *  One transaction: Z1, Z2, then the two positions three times each, the
 *  next command going out in the last eight clocks of each answer. Answers
 *  are 16 bits: 0, twelve bits, 000.                                       */
static struct { int down, on, off, x, y, u, v, z, ok; uint64_t last; } tp;
static int touch_read(int *u, int *v, int *z) {
    static const uint8_t tx[19] = { 0xB1, 0, 0xC1, 0, 0x91, 0, 0x91, 0, 0xD1, 0, 0x91, 0, 0xD1, 0, 0x91, 0, 0xD0, 0, 0 };
    uint8_t rx[19];
    spi_speed(2000000);
    pin_set(P_TCS, 0);
    int r = spi_pio(tx, rx, 19, 1);
    pin_set(P_TCS, 1);
    spi_speed(0);
    if (r) return 0;
    int w[9], any = 0;
    for (int i = 0; i < 9; i++) {
        uint16_t x = (uint16_t)(rx[1 + 2 * i] << 8 | rx[2 + 2 * i]);
        if (x & 0x8007) return 0;                           /* not an XPT2046's answer      */
        w[i] = x >> 3;
        any |= w[i];
    }
    if (!any) return 0;
    int a = w[3], b = w[5], c = w[7];                        /* w[2]: the first, noisy one   */
    int d0 = abs(a - b), d1 = abs(a - c), d2 = abs(b - c);
    *u = d0 <= d1 && d0 <= d2 ? (a + b) / 2 : d1 <= d2 ? (a + c) / 2 : (b + c) / 2;
    a = w[4]; b = w[6]; c = w[8];
    d0 = abs(a - b); d1 = abs(a - c); d2 = abs(b - c);
    *v = d0 <= d1 && d0 <= d2 ? (a + b) / 2 : d1 <= d2 ? (a + c) / 2 : (b + c) / 2;
    *z = w[0] + 4095 - w[1];
    return 1;
}

static int touch_probe(void) {
    int u, v, z;
    for (int i = 0; i < 4; i++) { if (!touch_read(&u, &v, &z)) return 0; delay_us(200); }
    return 1;
}

/* Where the screen sits on the panel: as large as fits, centred. */
static void fit(int sw, int shh, int *x, int *y, int *w, int *h) {
    if (sw <= 0 || shh <= 0) { *x = *y = 0; *w = LCD_W; *h = LCD_H; return; }
    if ((long)sw * LCD_H >= (long)shh * LCD_W) { *w = LCD_W; *h = (int)((long)shh * LCD_W / sw); }
    else { *h = LCD_H; *w = (int)((long)sw * LCD_H / shh); }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    *x = (LCD_W - *w) / 2; *y = (LCD_H - *h) / 2;
}

/* A raw touch to the screen: calibrated onto the panel, then undo the fit. */
static void touch_map(int u, int v, int *sx, int *sy) {
    int rx = cfg.swap ? v : u, ry = cfg.swap ? u : v;
    int c0 = cfg.cal[0], c1 = cfg.cal[1], c2 = cfg.cal[2], c3 = cfg.cal[3];
    if (c0 == c1 || c2 == c3) { c0 = c2 = 200; c1 = c3 = 3900; }
    int px = (rx - c0) * (LCD_W - 1) / (c1 - c0), py = (ry - c2) * (LCD_H - 1) / (c3 - c2);
    if (px < 0) px = 0;
    if (px > LCD_W - 1) px = LCD_W - 1;
    if (py < 0) py = 0;
    if (py > LCD_H - 1) py = LCD_H - 1;
    if (cfg.flip) { px = LCD_W - 1 - px; py = LCD_H - 1 - py; }
    int fx, fy, fw, fh;
    fit(scr_w, scr_h, &fx, &fy, &fw, &fh);
    int x = (px - fx) * scr_w / fw, y = (py - fy) * scr_h / fh;
    *sx = x < 0 ? 0 : x >= scr_w ? scr_w - 1 : x;
    *sy = y < 0 ? 0 : y >= scr_h ? scr_h - 1 : y;
}

/* ---- calibration: three crosses, drawn on the panel itself ------------------------------------- */
static const int16_t cal_px[3] = { 40, LCD_W - 41, 40 }, cal_py[3] = { 40, 40, LCD_H - 41 };
static struct { int active, step, armed, n; long su, sv; int ru[3], rv[3]; uint64_t t0; } cal;
static void send_blocking(int x, int y, int w, int h);

static void panel_fill(int x, int y, int w, int h, uint16_t c565) {
    uint16_t be = (uint16_t)(c565 >> 8 | c565 << 8);
    for (int i = 0; i < w * h; i++) buf[i] = be;
    send_blocking(x, y, w, h);
}

static void cal_draw(void) {
    panel_fill(0, 0, LCD_W, LCD_H, 0x0000);
    int x = cal_px[cal.step], y = cal_py[cal.step];
    panel_fill(x - 12, y - 1, 25, 3, 0xFFFF);
    panel_fill(x - 1, y - 12, 3, 25, 0xFFFF);
    cal.armed = 0; cal.n = 0; cal.su = cal.sv = 0;
    cal.t0 = now_ms();
}

static void cal_end(const char *msg) {
    cal.active = 0;
    full = 1; qn = qi = 0;
    if (msg) shell_toast(msg, "the SPI screen's touch");
}

static void cal_finish(void) {
    int tx[3], ty[3];
    for (int i = 0; i < 3; i++) {                            /* the crosses, upright          */
        tx[i] = cfg.flip ? LCD_W - 1 - cal_px[i] : cal_px[i];
        ty[i] = cfg.flip ? LCD_H - 1 - cal_py[i] : cal_py[i];
    }
    /* From the first cross to the second only x changes: the reading that
     * moved more is x's. */
    int swap = abs(cal.rv[1] - cal.rv[0]) > abs(cal.ru[1] - cal.ru[0]);
    int x0 = swap ? cal.rv[0] : cal.ru[0], x1 = swap ? cal.rv[1] : cal.ru[1];
    int y0 = swap ? cal.ru[0] : cal.rv[0], y2 = swap ? cal.ru[2] : cal.rv[2];
    if (abs(x1 - x0) < 300 || abs(y2 - y0) < 200) { cal_end("Calibration didn't take"); return; }
    cfg.swap = (uint8_t)swap;
    cfg.cal[0] = (int16_t)(x0 + (0 - tx[0]) * (x1 - x0) / (tx[1] - tx[0]));
    cfg.cal[1] = (int16_t)(x0 + (LCD_W - 1 - tx[0]) * (x1 - x0) / (tx[1] - tx[0]));
    cfg.cal[2] = (int16_t)(y0 + (0 - ty[0]) * (y2 - y0) / (ty[2] - ty[0]));
    cfg.cal[3] = (int16_t)(y0 + (LCD_H - 1 - ty[0]) * (y2 - y0) / (ty[2] - ty[0]));
    cfg_save();
    kprintf("[lcd] touch calibrated: swap %d, x %d..%d, y %d..%d\n", cfg.swap, cfg.cal[0], cfg.cal[1], cfg.cal[2], cfg.cal[3]);
    cal_end("Touch calibrated");
}

/* A sample while the crosses are up: the finger's average, once it lifts. */
static void cal_feed(int pressed, int u, int v) {
    if (now_ms() - cal.t0 > 30000) { cal_end("Calibration stopped"); return; }
    if (!pressed) {
        if (!cal.armed) { cal.armed = 1; return; }
        if (cal.n < 3) { cal.n = 0; cal.su = cal.sv = 0; return; }
        cal.ru[cal.step] = (int)(cal.su / cal.n);
        cal.rv[cal.step] = (int)(cal.sv / cal.n);
        if (++cal.step == 3) cal_finish();
        else cal_draw();
        return;
    }
    if (!cal.armed) return;                                  /* the finger that started it   */
    cal.su += u; cal.sv += v; cal.n++;
}

static void touch_poll(void) {
    if (!touch || now_ms() - tp.last < 15) return;
    tp.last = now_ms();
    int u = 0, v = 0, z = 0;
    tp.ok = touch_read(&u, &v, &z);
    if (tp.ok) { tp.u = u; tp.v = v; tp.z = z; }
    int pressed = tp.ok && z >= (tp.down ? 220 : 350);
    if (cal.active) { cal_feed(pressed, u, v); return; }
    if (pressed) {
        int x, y;
        touch_map(u, v, &x, &y);
        tp.off = 0;
        if (!tp.down) {
            if (++tp.on < 2) return;                         /* two in a row before it counts */
            tp.down = 1; tp.x = x; tp.y = y;
        } else {                                             /* a resistive panel jitters     */
            tp.x = (tp.x * 3 + x + 2) / 4; tp.y = (tp.y * 3 + y + 2) / 4;
        }
        input_touch(tp.x, tp.y, 1, SRC_TOUCH);
        input_touch_tick();
        return;
    }
    tp.on = 0;
    if (!tp.down) return;
    if (++tp.off >= 2) { tp.down = 0; input_touch(tp.x, tp.y, 0, SRC_TOUCH); }
    else input_touch_tick();
}

/* ---- the pixels ------------------------------------------------------------------------------ */
static void remap(void) {
    src_w = scr_w; src_h = scr_h;
    fit(src_w, src_h, &ox, &oy, &dw, &dh);
    for (int x = 0; x < LCD_W; x++) xmap[x] = (int16_t)(x >= ox && x < ox + dw ? (x - ox) * src_w / dw : -1);
    for (int y = 0; y < LCD_H; y++) ymap[y] = (int16_t)(y >= oy && y < oy + dh ? (y - oy) * src_h / dh : -1);
}

/* The panel's pixels showing screen pixels sx..sx+sw, sy..sy+sh: the ones
 * whose map lands in there. 0 if a shrunk screen gives it none. */
static int map_rect(int sx, int sy, int sw, int shh, lrect_t *r) {
    int x0 = ox + (sx * dw + src_w - 1) / src_w, x1 = ox + ((sx + sw) * dw + src_w - 1) / src_w;
    int y0 = oy + (sy * dh + src_h - 1) / src_h, y1 = oy + ((sy + shh) * dh + src_h - 1) / src_h;
    if (x1 > ox + dw) x1 = ox + dw;
    if (y1 > oy + dh) y1 = oy + dh;
    if (x1 <= x0 || y1 <= y0) return 0;
    r->x = (int16_t)x0; r->y = (int16_t)y0; r->w = (int16_t)(x1 - x0); r->h = (int16_t)(y1 - y0);
    return 1;
}

/* What changed since the last look, as panel rectangles: per row of tiles
 * the span of changed ones, and a row joins the one above it when that
 * wastes little. A new size (or the first look) is the whole panel. */
static void collect(void) {
    qn = qi = 0;
    batch_full = 0;
    if (!g_front || !g_tile_gen || !scr_w || !scr_h) return;
    if (scr_w != src_w || scr_h != src_h) { remap(); full = 1; }
    if (!full && sent_gen == g_front_gen) return;
    uint32_t gen = g_front_gen;
    if (full) {
        q[qn++] = (lrect_t){ 0, 0, LCD_W, LCD_H };            /* the black bars too            */
        batch_full = 1;
    } else for (int ty = 0; ty < scr_th; ty++) {
        const uint32_t *tg = g_tile_gen + ty * scr_tw;
        int lo = -1, hi = -1;
        for (int tx = 0; tx < scr_tw; tx++) if ((int32_t)(tg[tx] - sent_gen) > 0) { if (lo < 0) lo = tx; hi = tx; }
        if (lo < 0) continue;
        int sx = lo * SCR_TILE, sy = ty * SCR_TILE;
        int sx1 = (hi + 1) * SCR_TILE < scr_w ? (hi + 1) * SCR_TILE : scr_w;
        int sy1 = sy + SCR_TILE < scr_h ? sy + SCR_TILE : scr_h;
        lrect_t r;
        if (!map_rect(sx, sy, sx1 - sx, sy1 - sy, &r)) continue;
        if (qn) {
            lrect_t *p = &q[qn - 1];
            if (p->y + p->h == r.y) {
                int ux = p->x < r.x ? p->x : r.x;
                int ux1 = p->x + p->w > r.x + r.w ? p->x + p->w : r.x + r.w;
                long ua = (long)(ux1 - ux) * (p->h + r.h), sa = (long)p->w * p->h + (long)r.w * r.h;
                if (ua * 4 <= sa * 5 || qn == QMAX) {
                    p->x = (int16_t)ux; p->w = (int16_t)(ux1 - ux); p->h = (int16_t)(p->h + r.h);
                    continue;
                }
            }
            if (qn == QMAX) {                                 /* not next to it: grow to cover */
                int ux = p->x < r.x ? p->x : r.x, uy = p->y;
                int ux1 = p->x + p->w > r.x + r.w ? p->x + p->w : r.x + r.w;
                p->x = (int16_t)ux; p->w = (int16_t)(ux1 - ux); p->h = (int16_t)(r.y + r.h - uy);
                continue;
            }
        }
        q[qn++] = r;
    }
    sent_gen = gen;
    full = 0;
}

static inline uint16_t px565(uint32_t p) {
    uint32_t v = ((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F);
    return (uint16_t)(v >> 8 | v << 8);                      /* big-endian on the wire        */
}

static void convert(const lrect_t *r) {
    uint16_t *o = buf;
    int ident = src_w == LCD_W && src_h == LCD_H;
    for (int y = r->y; y < r->y + r->h; y++) {
        int sy = ymap[y];
        if (sy < 0) { memset(o, 0, (size_t)r->w * 2); o += r->w; continue; }
        const uint32_t *s = g_front + (size_t)sy * scr_w;
        if (ident) {
            s += r->x;
            for (int i = 0; i < r->w; i++) o[i] = px565(s[i]);
            o += r->w;
        } else for (int x = r->x; x < r->x + r->w; x++) {
            int sx = xmap[x];
            *o++ = sx < 0 ? 0 : px565(s[sx]);
        }
    }
}

static void clean(const void *p, size_t n) {
    for (uintptr_t a = (uintptr_t)p & ~(uintptr_t)63; a < (uintptr_t)p + n; a += 64)
        __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* buf holds w x h pixels for the window x, y: send them. The window wraps,
 * so an odd count gets one more of its first pixel (DMA moves 4 bytes). */
static void start(int x, int y, int w, int h) {
    int n = w * h;
    if (n & 1) buf[n++] = buf[0];
    xfer_bytes = n * 2;
    window(x, y, w, h);
    pin_set(P_CS, 0);
    pin_set(P_DC, 0);
    uint8_t c = 0x2C;                                         /* RAMWR, CS held for the pixels */
    spi_pio(&c, 0, 1, 0);
    pin_set(P_DC, 1);
    w32(SPI_ISR, ~0u);
    w32(SPI_TCR, TCR_SS_OWNER | TCR_SS_LEVEL | TCR_DHB);
    spi_counts(xfer_bytes);
    xfer_t0 = xfer_moved = now_us();
    st.rects++;
    st.bytes += (uint64_t)xfer_bytes;
    if (!use_pio) {
        clean(buf, (size_t)xfer_bytes);
        desc[0] = DRQ_DRAM | (2u << 6) | (0u << 8) | (2u << 9)            /* DRAM, burst 8, linear, 32-bit */
                | ((uint32_t)DRQ_SPI1 << 16) | (2u << 22) | (1u << 24) | (2u << 25);   /* SPI1, burst 8, fixed, 32-bit */
        desc[1] = (uint32_t)(uintptr_t)buf;
        desc[2] = SPI_TXD;
        desc[3] = (uint32_t)xfer_bytes;
        desc[4] = 8;                                          /* a few cycles between bursts   */
        desc[5] = DMA_LAST;
        clean(desc, 24);
        w32(DMA_EN, 0);
        w32(DMA_PEND0, 0xFu << (DMA_CH * 4));
        w32(SPI_FCR, FCR_TF_RST | FCR_RF_RST | FCR_LEVELS);
        w32(SPI_FCR, FCR_TF_DRQ | FCR_LEVELS);
        w32(DMA_DESC, (uint32_t)(uintptr_t)desc);
        w32(DMA_EN, 1);
        xfer = X_DMA;
    } else {
        w32(SPI_FCR, FCR_TF_RST | FCR_RF_RST | FCR_LEVELS);
        const uint8_t *b = (const uint8_t *)buf;
        for (xfer_off = 0; xfer_off < 64 && xfer_off < xfer_bytes; xfer_off++) w8(SPI_TXD, b[xfer_off]);
        xfer = X_PIO;
    }
    w32(SPI_TCR, TCR_SS_OWNER | TCR_SS_LEVEL | TCR_DHB | TCR_XCH);
}

static void xfer_end(void) {
    w32(DMA_EN, 0);
    w32(SPI_FCR, FCR_LEVELS);
    pin_set(P_CS, 1);
    st.busy_us += now_us() - xfer_t0;
    xfer = X_IDLE;
}

/* Is the rectangle out? Moves it along by hand in PIO mode (2 ms a turn). */
static int xfer_step(void) {
    if (xfer == X_IDLE) return 1;
    if (xfer == X_PIO) {
        const uint8_t *b = (const uint8_t *)buf;
        uint64_t t = now_us();
        while (xfer_off < xfer_bytes && now_us() - t < 2000) {
            int room = 64 - (int)((r32(SPI_FSR) >> 16) & 0xFF);
            if (room > 0) xfer_moved = now_us();
            while (room-- > 0 && xfer_off < xfer_bytes) w8(SPI_TXD, b[xfer_off++]);
        }
        if (xfer_off >= xfer_bytes && spi_done()) { xfer_end(); return 1; }
        /* By hand it only moves while we're here: stuck is the FIFO not draining. */
        if (now_us() - xfer_moved < 200000) return 0;
    } else {
        if (spi_done() && !(r32(DMA_STA) & (1u << DMA_CH))) { xfer_end(); return 1; }
        /* Three times as long as it should take, and 50 ms more: give up on it. */
        uint64_t limit = (uint64_t)xfer_bytes * 8 * 3 / (fast_hz / 1000000u) + 50000;
        if (now_us() - xfer_t0 < limit) return 0;
    }
    st.errors++;
    kprintf("[lcd] a %d-byte %s transfer didn't finish (ISR %08x FSR %08x DMA %08x, %u left)%s\n", xfer_bytes,
            xfer == X_DMA ? "DMA" : "PIO", r32(SPI_ISR), r32(SPI_FSR), r32(DMA_STA), r32(DMA_LEFT),
            xfer == X_DMA ? ": PIO from now on" : "");
    if (xfer == X_DMA) use_pio = 1;
    w32(DMA_EN, 0);
    w32(SPI_GCR, GCR_EN | GCR_MASTER | GCR_TP | GCR_SRST);
    for (int i = 0; i < 1000 && (r32(SPI_GCR) & GCR_SRST); i++) delay_us(1);
    w32(SPI_GCR, GCR_EN | GCR_MASTER | GCR_TP);
    spi_speed(0);
    xfer_end();
    full = 1; qn = qi = 0;
    return 1;
}

static void quiesce(void) { while (!xfer_step()) { } }

static void send_blocking(int x, int y, int w, int h) {
    quiesce();
    start(x, y, w, h);
    quiesce();
}

/* ---- the rest of Mini asks ------------------------------------------------------------------- */
static int bring_up(int force) {
    if (g_plat != PLAT_H618) return 0;
    if (!buf) {
        buf = (uint16_t *)kmalloc((uint64_t)LCD_W * LCD_H * 2 + 64);
        uint8_t *d = (uint8_t *)kmalloc(192);
        if (!buf || !d) { kprintf("[lcd] no memory\n"); return 0; }
        desc = (uint32_t *)(((uintptr_t)d + 63) & ~(uintptr_t)63);
    }
    hw_setup();
    found = probe(force);
    if (!found && !force) {
        hw_release();
        kprintf("[lcd] no SPI screen on the header (RDDPM %02x %02x)\n", probe_a[0], probe_a[1]);
        return 0;
    }
    panel_settings();
    touch = touch_probe();
    up = 1; lit = 0; asleep = 0; need_on = 1; full = 1; qn = qi = 0; src_w = src_h = 0;
    memset(&tp, 0, sizeof tp);
    kprintf("[lcd] ST7796S %d x %d on SPI1 at %u MHz%s (RDDPM %02x %02x -> %02x %02x)%s\n", LCD_W, LCD_H,
            fast_hz / 1000000u, found ? "" : ", turned on in Settings", probe_a[0], probe_a[1], probe_b[0], probe_b[1],
            touch ? ", XPT2046 touch" : "");
    return 1;
}

static void shut_down(void) {
    if (!up) return;
    quiesce();
    cal.active = 0;
    if (tp.down) input_touch(tp.x, tp.y, 0, SRC_TOUCH);
    pin_set(P_BL, 0);
    cmd0(0x28); cmd0(0x10);
    up = 0; lit = 0; touch = 0;
    hw_release();
}

/* Before screen_init: 1 if the screen is the panel's from now on. */
int lcd_start(void) {
    lcd_cfg_t c;
    if (cfg_load(&c)) cfg = c;
    if (cfg.mode == 2) return 0;
    return bring_up(cfg.mode == 1);
}

/* The card's copy of the settings, once it's mounted (a power cut loses the
 * other one). 1 if the panel came or went. */
int lcd_restore(void) {
    lcd_cfg_t c;
    if (!cfg_load(&c) || !memcmp(&c, &cfg, sizeof c)) return 0;
    int was = up;
    int redo = c.mode != cfg.mode || c.mhz != cfg.mhz;
    cfg = c;
    if (redo) {
        shut_down();
        if (cfg.mode != 2) bring_up(cfg.mode == 1);
    } else if (up) {
        quiesce();
        cmd1(0x36, madctl()); cmd0(cfg.inv ? 0x21 : 0x20);
        full = 1; qn = qi = 0;
    }
    return was != up;
}

int lcd_on(void) { return up; }
int lcd_lit(void) { return up && !asleep; }
int lcd_touch(void) { return up && touch; }
int lcd_mode(void) { return cfg.mode; }
int lcd_found(void) { return found; }
int lcd_flipped(void) { return cfg.flip; }
int lcd_scaled(void) { return up && (scr_w != LCD_W || scr_h != LCD_H); }

/* Settings: Automatic, On, Off. The caller fits the screen to the result. */
void lcd_set_mode(int m) {
    if (m < 0 || m > 2 || m == cfg.mode) return;
    cfg.mode = (uint8_t)m;
    cfg_save();
    if (m == 2) { shut_down(); return; }
    if (!up) bring_up(m == 1);
}

void lcd_set_flip(int on) {
    cfg.flip = (uint8_t)!!on;
    cfg_save();
    if (!up) return;
    quiesce();
    cmd1(0x36, madctl());
    full = 1; qn = qi = 0;
}

void lcd_calibrate(void) {
    if (!up || !touch || asleep) return;
    quiesce();
    memset(&cal, 0, sizeof cal);
    cal.active = 1;
    if (tp.down) { tp.down = 0; input_touch(tp.x, tp.y, 0, SRC_TOUCH); }
    cal_draw();
}

/* Screen sleep: the backlight off and the panel asleep; back on with its
 * picture sent again before it lights. */
int lcd_sleep(void) {
    if (!up || asleep) return 0;
    quiesce();
    cal.active = 0;
    light(0);
    cmd0(0x10);                                              /* SLPIN                        */
    asleep = 1; wake_at = 0;
    return 1;
}

void lcd_wake(void) {
    if (!up || !asleep) return;
    cmd0(0x11);                                              /* SLPOUT; 5 ms before the next */
    asleep = 0;
    wake_at = now_ms() + 10;
    need_on = 1; full = 1; qn = qi = 0;
}

/* Every turn of the main loop: finish or start a rectangle, read the touch. */
void lcd_poll(void) {
    if (!up) return;
    if (!xfer_step()) return;
    touch_poll();                                            /* asleep too: a touch wakes it */
    if (asleep || cal.active) return;
    if (wake_at) { if (now_ms() < wake_at) return; wake_at = 0; }
    if (hold_until) { if (now_ms() < hold_until) return; hold_until = 0; full = 1; qn = qi = 0; }
    if (qi < qn && (scr_w != src_w || scr_h != src_h)) qn = qi = 0;   /* resized under it */
    if (qi >= qn) {
        if (need_on && batch_full && qn) { light(1); need_on = 0; st.frames++; }
        else if (qn) st.frames++;
        collect();
        if (qi >= qn) return;
    }
    const lrect_t *r = &q[qi++];
    convert(r);
    start(r->x, r->y, r->w, r->h);
}

/* ---- /lcd (developer) --------------------------------------------------------------------------- */
static const char *qarg(const char *q, const char *key) {
    size_t k = strlen(key);
    for (const char *p = q; p && *p; ) {
        if (!strncmp(p, key, k)) return p + k;
        p = strchr(p, '&');
        if (p) p++;
    }
    return 0;
}

int lcd_debug(const char *path, char *out, int cap) {
    const char *q = strchr(path, '?');
    q = q ? q + 1 : "";
    const char *a;
    int changed = 0;
    if ((a = qarg(q, "mode=")))   { lcd_set_mode(atoi(a)); changed = 2; }
    if ((a = qarg(q, "flip=")))   { lcd_set_flip(atoi(a)); }
    if ((a = qarg(q, "inv=")))    { cfg.inv = (uint8_t)!!atoi(a); changed = 1; }
    if ((a = qarg(q, "rgb=")))    { cfg.rgb = (uint8_t)!!atoi(a); changed = 1; }
    if ((a = qarg(q, "madctl="))) { cfg.madctl = (uint8_t)strtoul(a, 0, 16); changed = 1; }
    if ((a = qarg(q, "pio=")))    { quiesce(); use_pio = !!atoi(a); }
    if ((a = qarg(q, "mhz=")) && atoi(a) >= 10 && atoi(a) <= 50) {
        cfg.mhz = (uint8_t)atoi(a);
        cfg_save();
        if (hw) { quiesce(); spi_clock_setup(); spi_speed(0); }
    }
    if ((a = qarg(q, "cal="))) {                             /* x-left,x-right,y-top,y-bottom,swap */
        int v[5] = { 0 }, n = 0;
        for (const char *p = a; n < 5 && p; n++) { v[n] = atoi(p); p = strchr(p, ','); if (p) p++; }
        if (n >= 4) { for (int i = 0; i < 4; i++) cfg.cal[i] = (int16_t)v[i]; cfg.swap = (uint8_t)(n == 5 && v[4]); cfg_save(); }
    } else if (!strcmp(q, "cal")) lcd_calibrate();
    if (changed == 1) {
        cfg_save();
        if (up) { quiesce(); cmd1(0x36, madctl()); cmd0(cfg.inv ? 0x21 : 0x20); full = 1; qn = qi = 0; }
    }
    if (changed == 2) { void display_lcd_changed(void); display_lcd_changed(); }
    if ((a = qarg(q, "fill=")) && up && !asleep) {           /* RRGGBB, held for 5 s          */
        uint32_t c = (uint32_t)strtoul(a, 0, 16);
        uint16_t v = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x1F));
        panel_fill(0, 0, LCD_W, LCD_H, v);
        if (!lit) light(1);
        hold_until = now_ms() + 5000;
    }
    int n = snprintf(out, (size_t)cap,
        "SPI screen: %s, mode %s%s%s%s\n"
        "probe: RDDPM after reset %02x %02x, after SLPOUT %02x %02x (bit 4 = awake)\n"
        "SPI1: module %u Hz, pixels at %u Hz, %s; MADCTL %02x%s, inversion %s\n"
        "sent: %u rects, %u frames, %lu KiB, %lu ms busy, %u errors\n"
        "screen %d x %d shown at %d,%d %d x %d on the panel\n",
        up ? "on" : "off", cfg.mode == 0 ? "Automatic" : cfg.mode == 1 ? "On" : "Off",
        found ? ", found" : "", asleep ? ", asleep" : lit ? ", lit" : "", cal.active ? ", calibrating" : "",
        probe_a[0], probe_a[1], probe_b[0], probe_b[1],
        mod_hz, fast_hz, use_pio ? "PIO" : "DMA", madctl(), cfg.madctl ? " (set by hand)" : "", cfg.inv ? "on" : "off",
        st.rects, st.frames, (unsigned long)(st.bytes >> 10), (unsigned long)(st.busy_us / 1000), st.errors,
        scr_w, scr_h, ox, oy, dw, dh);
    if (n < cap)
        n += snprintf(out + n, (size_t)(cap - n),
            "touch: %s; raw u %d v %d z %d (%s), %s; cal x %d..%d y %d..%d swap %d\n"
            "set: ?mode=0|1|2 ?flip=0|1 ?inv=0|1 ?rgb=0|1 ?madctl=HEX ?mhz=10..50 ?pio=0|1 ?fill=RRGGBB ?cal ?cal=xl,xr,yt,yb,swap\n",
            touch ? "XPT2046" : "none", tp.u, tp.v, tp.z, tp.ok ? "answered" : "no answer", tp.down ? "down" : "up",
            cfg.cal[0], cfg.cal[1], cfg.cal[2], cfg.cal[3], cfg.swap);
    return n;
}
