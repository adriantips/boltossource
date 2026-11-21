/* ===========================================================================
 *  BoltOS Mini  -  mini/hdmi.c
 *
 *  The Orange Pi's micro-HDMI port. A monitor on it shows the same screen
 *  the Mac sees over Screen Sharing, at a size the monitor asked for.
 *
 *  The picture's path through the H618:
 *
 *      front (RAM) -> DE 3.3 mixer 0, one UI layer -> TCON TOP -> TCON TV0
 *        (the timings) -> DesignWare HDMI TX -> its PHY -> the cable
 *
 *  Our U-Boot has no video, so every clock, reset and register is set here
 *  from nothing. The register maps and the order of things follow Linux's
 *  sun4i DRM driver (sun8i_mixer, sun4i_tcon, sun8i_tcon_top, dw-hdmi) and
 *  Orange Pi's 6.1 tree for what is H616/H618's own: DE33's three register
 *  windows, the UI channel map and the PHY tables.
 *
 *  The monitor says what it can show in its EDID, read over the cable's DDC
 *  wires by the controller's own I2C master. Its detailed timings, and the
 *  established, standard and CEA modes it lists that are in the table
 *  below, are what Settings offers. The first detailed timing is the one it
 *  prefers; Automatic picks that. The cable is looked at twice a second, so
 *  a monitor plugged in later gets the screen too.
 *
 *  Small HDMI LCDs (the 5" and 7" touchscreens sold for the Raspberry Pi)
 *  are monitors too, with a few habits of their own: a 1024 x 600 or
 *  800 x 480 panel whose timing only its EDID knows, a board that may not
 *  raise hot-plug detect (so the DDC wires are asked as well), and touch on
 *  a separate USB cable (usbhost.c). With no EDID at all, the LCD sizes are
 *  offered with the usual ones, at the CVT timings their makers give for
 *  the Pi.
 *
 *  The second half decides the screen's size: Automatic, or a size chosen in
 *  Settings (kept across restarts), on the monitor or, with none, for VNC.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

#define SYSCON      0x03000000u
#define CCU         0x03001000u
#define DE          0x01000000u
#define DE_CLK      (DE + 0x8000)                     /* DE33's own gates and resets   */
#define DE_TOP      (DE + 0x8100)                     /* mixer 0: enable, size, commit */
#define DE_UI       (DE + 0x100000 + 6 * 0x20000 + 0x1000)  /* UI channel 1 (map {0,6,7,8}) */
#define DE_BLD      (DE + 0x280000 + 0x1000)          /* mixer 0's blender             */
#define HDMI        0x06000000u                       /* DesignWare HDMI TX, byte regs */
#define HDMI_PHY    0x06010000u
#define TCON_TOP    0x06510000u
#define TCON_TV     0x06515000u

/* ---- the modes we know by name ------------------------------------------------- */
#define M(khz, w, hfp, hs, hbp, h, vfp, vs, vbp, hp, vp) \
    { w, h, 0, khz, hfp, hs, hbp, vfp, vs, vbp, hp, vp }
static const video_mode_t known[] = {
    M( 25175,  640, 16,  96,  48,  480, 10, 2, 33, 0, 0),   /* DMT, VIC 1     */
    M( 27000,  720, 16,  62,  60,  480,  9, 6, 30, 0, 0),   /* VIC 2/3        */
    M( 27000,  720, 12,  64,  68,  576,  5, 5, 39, 0, 0),   /* VIC 17/18      */
    M( 29500,  800, 24,  72,  96,  480,  3, 7, 10, 0, 1),   /* CVT: 5" LCDs   */
    M( 40000,  800, 40, 128,  88,  600,  1, 4, 23, 1, 1),
    M( 27000,  480, 100,  62,  108,  320,  50,  6,  220, 0, 0),  /* 480 x 320 LCDs @ 60 Hz */
    M( 48960, 1024, 40, 104, 144,  600,  3, 10, 11, 0, 1),  /* CVT: 7" LCDs   */
    M( 65000, 1024, 24, 136, 160,  768,  3, 6, 29, 0, 0),
    M( 74250, 1280, 110, 40, 220,  720,  5, 5, 20, 1, 1),   /* VIC 4          */
    M( 74250, 1280, 440, 40, 220,  720,  5, 5, 20, 1, 1),   /* VIC 19, 50 Hz  */
    M( 83500, 1280, 72, 128, 200,  800,  3, 6, 22, 0, 1),
    M(108000, 1280, 48, 112, 248, 1024,  1, 3, 38, 1, 1),
    M( 85500, 1366, 70, 143, 213,  768,  3, 3, 24, 1, 1),
    M(106500, 1440, 80, 152, 232,  900,  3, 6, 25, 0, 1),
    M(108000, 1600, 24,  80,  96,  900,  1, 3, 96, 1, 1),
    M(146250, 1680, 104, 176, 280, 1050, 3, 6, 30, 0, 1),
    M(148500, 1920, 88,  44, 148, 1080,  4, 5, 36, 1, 1),   /* VIC 16         */
    M(148500, 1920, 528, 44, 148, 1080,  4, 5, 36, 1, 1),   /* VIC 31, 50 Hz  */
    M( 74250, 1920, 88,  44, 148, 1080,  4, 5, 36, 1, 1),   /* VIC 34, 30 Hz  */
    M(154000, 1920, 48,  32,  80, 1200,  3, 6, 26, 1, 0),   /* reduced blanking */
};
#define NKNOWN ((int)(sizeof known / sizeof known[0]))
static const struct { uint8_t vic; int16_t w, h, hz; } vics[] = {
    { 1, 640, 480, 60 }, { 2, 720, 480, 60 }, { 3, 720, 480, 60 }, { 4, 1280, 720, 60 },
    { 16, 1920, 1080, 60 }, { 17, 720, 576, 50 }, { 18, 720, 576, 50 }, { 19, 1280, 720, 50 },
    { 31, 1920, 1080, 50 }, { 34, 1920, 1080, 30 },
};

static int mode_hz(const video_mode_t *m) {
    uint32_t ht = m->w + m->hfp + m->hs + m->hbp, vt = m->h + m->vfp + m->vs + m->vbp;
    return (int)(((uint64_t)m->khz * 1000 + ht * vt / 2) / ((uint64_t)ht * vt));
}

static const video_mode_t *known_mode(int w, int h, int hz) {
    for (int i = 0; i < NKNOWN; i++) {
        int k = mode_hz(&known[i]);
        if (known[i].w == w && known[i].h == h && (k == hz || k == hz - 1 || k == hz + 1)) return &known[i];
    }
    return 0;
}

/* What fits: the screen lives in two buffers of the 96 MiB heap, the PHY
 * tables stop being trustworthy past 1920 x 1200's clock, and TMDS doesn't
 * go below 25 MHz (a 480 x 320 panel's own timing would). */
static int mode_ok(const video_mode_t *m) {
    return m && m->w >= 320 && m->h >= 240 && m->w <= 1920 && m->h <= 1200 &&
           m->khz >= 27000 && m->khz <= 165000;
}

/* ---- clocks ---------------------------------------------------------------------- *
 *  The pixel clock comes from PLL_VIDEO0. Its VCO runs at 24 MHz * N / M
 *  (N 12..255, M 1 or 2), between 288 MHz and 2.4 GHz (Linux's H616 CCU),
 *  and its 1x output is the VCO / 4. TCON TV0 and the HDMI TMDS clock both
 *  divide that 1x output by div (1..16). The usual rates come out exact:
 *  74.25 and 148.5 MHz are VCO 1188 MHz / 4 / 4 and / 4 / 2.              */
static int pll3_pick(uint32_t khz, int *m_out, int *n_out, int *div_out) {
    int best_m = 1, best_n = 12, best_div = 1;
    uint32_t best_diff = 0xFFFFFFFFu;
    int found = 0;
    for (int div = 1; div <= 16; div++) {
        for (int m = 1; m <= 2; m++) {                      /* M is 1 or 2  */
            int n = (int)(((uint64_t)khz * (uint64_t)m * (uint64_t)div + 3000) / 6000);
            if (n < 12 || n > 255) continue;
            uint32_t vco = 24000u * (uint32_t)n / (uint32_t)m;
            if (vco < 288000 || vco > 2400000) continue;
            uint32_t pixel = vco / (4u * (uint32_t)div);
            uint32_t diff = pixel > khz ? pixel - khz : khz - pixel;
            if (diff < best_diff) { best_diff = diff; best_m = m; best_n = n; best_div = div; found = 1; }
        }
    }
    if (!found) return 0;
    *m_out = best_m; *n_out = best_n; *div_out = best_div;
    return best_div;
}

/* PLL_VIDEO0 runs at 24 MHz * N / 4, for the DE mixer. */
static void clocks_on(void) {
    clr32(SYSCON + 0x04, 1u << 24);                           /* SRAM C belongs to the DE  */
    w32(CCU + 0x600, (1u << 31) | (1u << 24) | 1u);           /* DE: periph0-2x / 2 = 600 MHz */
    set32(CCU + 0x60C, 1u | (1u << 16));                      /* DE bus gate, reset off    */
    set32(DE_CLK + 0x08, 1u);                                 /* mixer 0 reset off         */
    set32(DE_CLK + 0x04, 1u);                                 /* mixer 0 bus gate          */
    set32(DE_CLK + 0x00, 1u);                                 /* mixer 0 clock gate        */
    clr32(DE_CLK + 0x0C, 0xFu);                               /* mixer 0 = DE / 1          */
    w32(DE_CLK + 0x24, 0);                                    /* DE33 wants these two set  */
    w32(DE_CLK + 0x28, 0x0000A980);                           /* (Linux, unexplained)      */

    w32(CCU + 0xB04, 1u << 31);                               /* HDMI slow: 24 MHz (DDC)   */
    w32(CCU + 0xB10, (1u << 31) | (1u << 30));                /* HDMI CEC: 32 kHz          */
    w32(CCU + 0xC40, (1u << 31) | 1u);                        /* HDCP: periph0 / 2         */
    set32(CCU + 0xC4C, 1u | (1u << 16));
    set32(CCU + 0xB1C, 1u | (1u << 16) | (1u << 17));         /* HDMI bus, HDMI + PHY resets */
    set32(CCU + 0xB5C, 1u | (1u << 16));                      /* TCON TOP                  */
    set32(CCU + 0xB9C, 1u | (1u << 16));                      /* TCON TV0                  */
}

static uint32_t pll_khz;                                      /* what PLL_VIDEO0 / div gives */

/* PLL_VIDEO0 for the pixel clock, then TCON TV0 and the HDMI TMDS clock
 * from its 1x output, both / div. The PLL's other bits are left as found. */
static void pixel_clock(uint32_t khz) {
    int m, n, div;
    div = pll3_pick(khz, &m, &n, &div);
    if (div == 0) { kprintf("[hdmi] no PLL config for %u kHz\n", khz); return; }
    uint32_t v = r32(CCU + 0x040) & ~((0xFFu << 8) | (1u << 1) | 1u);
    w32(CCU + 0x040, v | (1u << 31) | (1u << 29) | (1u << 27)
        | ((uint32_t)(n - 1) << 8) | ((uint32_t)(m - 1) << 1));
    for (uint64_t end = now_us() + 20000; !(r32(CCU + 0x040) & (1u << 28)) && now_us() < end; ) { }
    if (!(r32(CCU + 0x040) & (1u << 28))) kprintf("[hdmi] PLL_VIDEO0 didn't lock\n");
    w32(CCU + 0xB80, (1u << 31) | (uint32_t)(div - 1));          /* TCON TV0: pll / div  */
    w32(CCU + 0xB00, (1u << 31) | (uint32_t)(div - 1));          /* HDMI TMDS: the same   */
    pll_khz = (uint32_t)((uint64_t)24000 * (uint32_t)n / (uint32_t)m / 4 / (uint32_t)div);
}

/* ---- HDMI controller registers (byte access) ------------------------------------- */
static inline void hw(uint32_t r, uint8_t v) { w8(HDMI + r, v); }
static inline uint8_t hr(uint32_t r)         { return r8(HDMI + r); }
static void hmod(uint32_t r, uint8_t mask, uint8_t v) { hw(r, (uint8_t)((hr(r) & ~mask) | (v & mask))); }

/* ---- the PHY ---------------------------------------------------------------------- *
 *  The H616/H618's HDMI PHY is a Synopsys DesignWare HDMI 2.0 TX PHY (the
 *  controller's CONFIG2_ID says 0xF3), the H6's kind, not the H3's Allwinner
 *  PHY. It is configured through the controller's own PHY I2C master (slave
 *  0x69), with PHY_CONF0 for power. The Allwinner glue at 0x06010000 only
 *  has REXT (+0x04), set in hdmi_init. Sequence: Linux's dw-hdmi.c
 *  (hdmi_phy_configure, the DWC 3D TX configure), tables: the Orange Pi 6.1
 *  tree's sun8i_hdmi_phy.c, sun50i_h616_*. Hot-plug is PHY_STAT0 bit 1.     */
#define PHY_CONF0    0x3000
#define PHY_TST0     0x3001
#define PHY_STAT0    0x3004
#define C0_SVSRET    0x20
#define C0_PDDQ      0x10
#define C0_TXPWRON   0x08
#define C0_ENHPDRX   0x04
#define C0_DATAENPOL 0x02
#define C0_SELDIPIF  0x01
#define ST0_HPD      0x02
#define ST0_LOCK     0x01

static const struct { uint32_t khz; uint16_t cpce, gmp; } h616_mpll[] = {
    {  27000, 0x00b3, 0x0003 }, {  74250, 0x0072, 0x0003 }, { 148500, 0x0051, 0x0003 },
    { 297000, 0x0040, 0x0003 }, { 594000, 0x1a40, 0x0003 },
};
static const struct { uint32_t khz; uint16_t curr; } h616_curr[] = {
    { 27000, 0x0012 }, { 74250, 0x0013 }, { 148500, 0x0019 }, { 297000, 0x0019 }, { 594000, 0x0010 },
};
static const struct { uint32_t khz; uint16_t sym, term, vlev; } h616_cfg[] = {
    {  27000, 0x8009, 0x0007, 0x02b0 }, {  74250, 0x8019, 0x0004, 0x0290 },
    { 148500, 0x8019, 0x0004, 0x0290 }, { 297000, 0x8039, 0x0004, 0x022b },
    { 594000, 0x8029, 0x0000, 0x008a },
};
#define PICK(t, khz) ({ unsigned _i = 0; \
    while (_i + 1 < sizeof t / sizeof t[0] && (khz) > t[_i].khz) _i++; &t[_i]; })

static int phy_i2c_errors;

static void phy_i2c_write(uint8_t reg, uint16_t v) {
    hw(0x0108, 0xFF);                                         /* IH_I2CMPHY_STAT0: clear   */
    hw(0x3021, reg);                                          /* PHY_I2CM_ADDRESS          */
    hw(0x3022, (uint8_t)(v >> 8));                            /* DATAO_1                   */
    hw(0x3023, (uint8_t)v);                                   /* DATAO_0                   */
    hw(0x3026, 0x10);                                         /* OPERATION: write          */
    uint8_t st = 0;
    for (uint64_t end = now_us() + 2000; !(st = hr(0x0108) & 3) && now_us() < end; ) { }
    hw(0x0108, st);
    if (st != 2) phy_i2c_errors++;                            /* 1 = error, 0 = no answer  */
}

static void phy_power_off(void) {
    hmod(PHY_CONF0, C0_TXPWRON, 0);
    for (uint64_t end = now_us() + 5000; (hr(PHY_STAT0) & ST0_LOCK) && now_us() < end; ) { }
    hmod(PHY_CONF0, C0_PDDQ, C0_PDDQ);
}

/* Configure the PHY for a pixel clock and power it on; 0 once its PLL locks. */
static int phy_configure(uint32_t khz) {
    hmod(PHY_CONF0, C0_DATAENPOL | C0_SELDIPIF, C0_DATAENPOL);
    phy_power_off();
    hmod(PHY_CONF0, C0_SVSRET, C0_SVSRET);                    /* leave low-power mode      */
    hw(0x4005, 1); hw(0x4005, 0);                             /* MC_PHYRSTZ: gen2 reset    */
    hw(0x4007, 1);                                            /* MC_HEACPHY_RST            */
    hmod(PHY_TST0, 0x20, 0x20);                               /* TSTCLR                    */
    hw(0x3020, 0x69);                                         /* PHY_I2CM_SLAVE_ADDR       */
    hmod(PHY_TST0, 0x20, 0);
    phy_i2c_errors = 0;
    const __typeof__(h616_mpll[0]) *mp = PICK(h616_mpll, khz);
    const __typeof__(h616_curr[0]) *cu = PICK(h616_curr, khz);
    const __typeof__(h616_cfg[0])  *cf = PICK(h616_cfg, khz);
    phy_i2c_write(0x06, mp->cpce);                            /* CPCE_CTRL                 */
    phy_i2c_write(0x15, mp->gmp);                             /* GMPCTRL                   */
    phy_i2c_write(0x10, cu->curr);                            /* CURRCTRL                  */
    phy_i2c_write(0x13, 0);                                   /* PLLPHBYCTRL               */
    phy_i2c_write(0x17, 3u << 1);                             /* MSM_CTRL: CKO = FB clock  */
    phy_i2c_write(0x19, cf->term);                            /* TXTERM                    */
    phy_i2c_write(0x09, cf->sym);                             /* CKSYMTXCTRL               */
    phy_i2c_write(0x0E, cf->vlev);                            /* VLEVCTRL                  */
    phy_i2c_write(0x05, 0x8000);                              /* CKCALCTRL: override       */
    hmod(PHY_CONF0, C0_TXPWRON, C0_TXPWRON);
    hmod(PHY_CONF0, C0_PDDQ, 0);
    for (uint64_t end = now_us() + 10000; now_us() < end; )
        if (hr(PHY_STAT0) & ST0_LOCK) return 0;
    return -1;
}

/* ---- the monitor ----------------------------------------------------------------------------- */
static uint8_t edid[256];
static int edid_len;
static char monitor[16];
static video_mode_t modes[24];
static int nmodes;
static int hw_up, showing, plugged;
static int slept;                      /* screen sleep turned the output off (display_sleep) */
static video_mode_t shown;

static int hpd_less;                                          /* found by DDC, not by HPD  */

static void ddc_setup(void) {
    hw(0x7E09, 0);                                            /* I2CM_SOFTRSTZ             */
    hw(0x7E07, 0);                                            /* standard mode             */
    hw(0x7E0C, 0xd8); hw(0x7E0E, 0xfe);                       /* SCL high, low (U-Boot's)  */
    hw(0x7E00, 0x50);                                         /* the EDID's address        */
}

/* Does an EDID answer on the cable? Its first byte is always 0. With nothing
 * there the address isn't acknowledged, which takes a tenth of a millisecond. */
static int ddc_probe(void) {
    ddc_setup();
    hw(0x0105, 0x03);
    hw(0x7E01, 0);
    hw(0x7E04, 0x01);
    uint8_t st = 0;
    for (uint64_t end = now_us() + 5000; !(st & 3) && now_us() < end; ) st = hr(0x0105);
    hw(0x0105, 0x03);
    return (st & 3) == 2 && hr(0x7E03) == 0x00;
}

/* PHY_STAT0's HPD bit, or -- for an LCD board that doesn't drive HPD -- an
 * EDID that answers anyway. */
int hdmi_connected(void) {
    if (!hw_up) return 0;
    if (hr(PHY_STAT0) & ST0_HPD) { hpd_less = 0; return 1; }
    hpd_less = ddc_probe();
    return hpd_less;
}

static int ddc_read(int block, uint8_t *out) {
    ddc_setup();
    hw(0x7E08, 0x30);                                         /* segment pointer's address */
    hw(0x7E0A, (uint8_t)(block >> 1));
    for (int i = 0; i < 128; i++) {
        hw(0x0105, 0x03);                                     /* IH_I2CM_STAT0             */
        hw(0x7E01, (uint8_t)((block & 1) * 128 + i));
        hw(0x7E04, block < 2 ? 0x01 : 0x02);                  /* read 1 byte (+ segment)   */
        uint8_t st = 0;
        for (uint64_t end = now_us() + 10000; !(st & 3) && now_us() < end; ) st = hr(0x0105);
        if ((st & 3) != 2) return -1;
        out[i] = hr(0x7E03);
    }
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++) sum += out[i];
    return sum == 0 ? 0 : -1;
}

static int add_mode(const video_mode_t *m) {
    if (!mode_ok(m) || nmodes >= (int)(sizeof modes / sizeof modes[0])) return -1;
    video_mode_t c = *m;
    c.hz = (uint16_t)mode_hz(&c);
    for (int i = 0; i < nmodes; i++)
        if (modes[i].w == c.w && modes[i].h == c.h && modes[i].hz == c.hz) return i;
    modes[nmodes] = c;
    return nmodes++;
}

/* An 18-byte detailed timing descriptor, or a monitor-name descriptor. */
static int dtd(const uint8_t *d) {
    uint32_t clk = (uint32_t)(d[0] | d[1] << 8) * 10;
    if (!clk) {
        if (d[3] == 0xFC) {
            int n = 0;
            for (int i = 5; i < 18 && d[i] != 0x0A && n < 15; i++) monitor[n++] = (char)d[i];
            while (n && monitor[n - 1] == ' ') n--;
            monitor[n] = 0;
        }
        return -1;
    }
    if (d[17] & 0x80) return -1;                              /* interlaced                */
    video_mode_t m = { 0 };
    m.khz = clk;
    m.w = (uint16_t)(d[2] | (d[4] & 0xF0) << 4);
    int hbl = d[3] | (d[4] & 0x0F) << 8;
    m.h = (uint16_t)(d[5] | (d[7] & 0xF0) << 4);
    int vbl = d[6] | (d[7] & 0x0F) << 8;
    m.hfp = (uint16_t)(d[8] | (d[11] & 0xC0) << 2);
    m.hs  = (uint16_t)(d[9] | (d[11] & 0x30) << 4);
    m.vfp = (uint16_t)((d[10] >> 4) | (d[11] & 0x0C) << 2);
    m.vs  = (uint16_t)((d[10] & 0x0F) | (d[11] & 0x03) << 4);
    if (hbl <= m.hfp + m.hs || vbl <= m.vfp + m.vs) return -1;
    m.hbp = (uint16_t)(hbl - m.hfp - m.hs);
    m.vbp = (uint16_t)(vbl - m.vfp - m.vs);
    if ((d[17] & 0x18) == 0x18) { m.vpos = (d[17] >> 2) & 1; m.hpos = (d[17] >> 1) & 1; }
    return add_mode(&m);
}

static int cmp_mode(const video_mode_t *a, const video_mode_t *b) {
    long pa = (long)a->w * a->h, pb = (long)b->w * b->h;
    if (pa != pb) return pa > pb ? -1 : 1;
    int da = a->hz > 60 ? a->hz - 60 : 60 - a->hz, db = b->hz > 60 ? b->hz - 60 : 60 - b->hz;
    return da < db ? -1 : da > db ? 1 : 0;
}

/* Fill modes[] from the EDID; [0] is what the monitor prefers. */
static void read_monitor(void) {
    nmodes = 0; monitor[0] = 0; edid_len = 0;
    int ok = -1;
    for (int t = 0; t < 3 && ok; t++) ok = ddc_read(0, edid);
    static const uint8_t hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (ok == 0 && !memcmp(edid, hdr, 8)) {
        edid_len = 128;
        if (edid[126] && ddc_read(1, edid + 128) == 0) edid_len = 256;
        int pref = -1;
        for (int i = 0; i < 4; i++) {                         /* the four descriptors      */
            int k = dtd(edid + 54 + i * 18);
            if (pref < 0 && k >= 0) pref = k;
        }
        if (edid[35] & 0x20) add_mode(known_mode(640, 480, 60));
        if (edid[35] & 0x01) add_mode(known_mode(800, 600, 60));
        if (edid[36] & 0x08) add_mode(known_mode(1024, 768, 60));
        for (int i = 0; i < 8; i++) {                         /* standard timings          */
            const uint8_t *s = edid + 38 + i * 2;
            if (s[0] <= 1) continue;
            int w = (s[0] + 31) * 8, hz = (s[1] & 0x3F) + 60, a = s[1] >> 6;
            int h = a == 0 ? w * 10 / 16 : a == 1 ? w * 3 / 4 : a == 2 ? w * 4 / 5 : w * 9 / 16;
            const video_mode_t *m = known_mode(w, h, hz);
            if (m) add_mode(m);
        }
        if (edid_len == 256 && edid[128] == 0x02) {           /* the CEA extension         */
            const uint8_t *c = edid + 128;
            int end = c[2] >= 4 && c[2] <= 127 ? c[2] : 4;
            for (int p = 4; p < end; p += 1 + (c[p] & 31)) {
                if ((c[p] >> 5) != 2) continue;               /* video data block          */
                for (int i = 1; i <= (c[p] & 31) && p + i < end; i++) {
                    int vic = c[p + i] & 0x7F;
                    for (unsigned j = 0; j < sizeof vics / sizeof vics[0]; j++)
                        if (vics[j].vic == vic) add_mode(known_mode(vics[j].w, vics[j].h, vics[j].hz));
                }
            }
            for (int p = end; p + 18 <= 127; p += 18) dtd(c + p);
        }
        /* The preferred one first, the rest biggest first; a monitor that
         * prefers more than we show (4K) gets the biggest that fits. */
        video_mode_t first = pref >= 0 ? modes[pref] : (video_mode_t){ 0 };
        for (int i = 1; i < nmodes; i++)
            for (int j = i; j > 0 && cmp_mode(&modes[j], &modes[j - 1]) < 0; j--) {
                video_mode_t t = modes[j]; modes[j] = modes[j - 1]; modes[j - 1] = t;
            }
        if (pref >= 0) {
            int k = 0;
            while (k < nmodes && !(modes[k].w == first.w && modes[k].h == first.h && modes[k].hz == first.hz)) k++;
            for (; k > 0; k--) modes[k] = modes[k - 1];
            modes[0] = first;
        }
    }
    if (!nmodes) {
        /* No EDID: try 720p first (standard HDMI), then smaller LCD sizes. */
        add_mode(known_mode(1280, 720, 60));
        add_mode(known_mode(480, 320, 60));
        add_mode(known_mode(800, 480, 59));
        add_mode(known_mode(640, 480, 60));
        add_mode(known_mode(1024, 600, 60));
        add_mode(known_mode(1024, 768, 60));
        add_mode(known_mode(1920, 1080, 60));
        kprintf("[hdmi] no EDID from the monitor: offering the usual sizes and the LCD ones\n");
    }
    kprintf("[hdmi] monitor \"%s\": %d modes, prefers %dx%d at %d Hz%s\n", monitor[0] ? monitor : "?",
            nmodes, modes[0].w, modes[0].h, modes[0].hz, hpd_less ? " (no hot-plug signal: found over DDC)" : "");
}

/* ---- on and off ------------------------------------------------------------------------------------ */
#define R_PIO_BASE  0x07022000u

static void pmic_sda_high(void) { w32(R_PIO_BASE + 0x00, r32(R_PIO_BASE + 0x00) & ~(0xFu << 0)); }
static void pmic_sda_low(void)  { uint32_t v = r32(R_PIO_BASE + 0x00) & ~(0xFu << 0); w32(R_PIO_BASE + 0x00, v | (1u << 0)); w32(R_PIO_BASE + 0x10, r32(R_PIO_BASE + 0x10) & ~(1u << 0)); }
static void pmic_scl_high(void) { w32(R_PIO_BASE + 0x00, r32(R_PIO_BASE + 0x00) & ~(0xFu << 4)); }
static void pmic_scl_low(void)  { uint32_t v = r32(R_PIO_BASE + 0x00) & ~(0xFu << 4); w32(R_PIO_BASE + 0x00, v | (1u << 4)); w32(R_PIO_BASE + 0x10, r32(R_PIO_BASE + 0x10) & ~(1u << 1)); }
static int  pmic_sda_read(void) { return (r32(R_PIO_BASE + 0x10) >> 0) & 1; }
static int  pmic_scl_read(void) { return (r32(R_PIO_BASE + 0x10) >> 1) & 1; }

static void pmic_i2c_delay(void) { delay_us(5); }

static void pmic_i2c_start(void) {
    pmic_sda_high(); pmic_scl_high(); pmic_i2c_delay();
    pmic_sda_low(); pmic_i2c_delay();
    pmic_scl_low(); pmic_i2c_delay();
}
static void pmic_i2c_stop(void) {
    pmic_sda_low(); pmic_i2c_delay();
    pmic_scl_high(); pmic_i2c_delay();
    pmic_sda_high(); pmic_i2c_delay();
}
static int pmic_i2c_write_byte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        if (b & (1u << i)) pmic_sda_high(); else pmic_sda_low();
        pmic_i2c_delay();
        pmic_scl_high(); pmic_i2c_delay();
        pmic_scl_low(); pmic_i2c_delay();
    }
    pmic_sda_high(); pmic_i2c_delay();
    pmic_scl_high(); pmic_i2c_delay();
    int ack = !pmic_sda_read();
    pmic_scl_low(); pmic_i2c_delay();
    return ack;
}

static void pmic_enable_dldo1(void) {
    /* Enable R_PIO and R_TWI clocks/resets (PRCM). */
    set32(0x07010028u, (1u << 0) | (1u << 6));
    set32(0x070100B0u, (1u << 0) | (1u << 6));
    delay_ms(1);
    /* AXP313A at I2C address 0x36 on R_I2C (PL0/PL1).
     * DLDO1 = 3.3 V (500 + 100*28 mV). */
    pmic_i2c_start();
    pmic_i2c_write_byte(0x36 << 1);
    pmic_i2c_write_byte(0x17);          /* AXP313A_DLDO1_CONTROL */
    pmic_i2c_write_byte(0x1C);
    pmic_i2c_stop();
    /* Enable all regulators (DLDO1 = bit 4 of OUTPUT_CONTROL). */
    pmic_i2c_start();
    pmic_i2c_write_byte(0x36 << 1);
    pmic_i2c_write_byte(0x10);          /* AXP313A_OUTPUT_CONTROL */
    pmic_i2c_write_byte(0x1F);
    pmic_i2c_stop();
    kprintf("[hdmi] pmic: DLDO1 on at 3.3 V (HDMI PHY rail)\n");
}

int hdmi_init(void) {
    if (g_plat != PLAT_H618) return -1;
    /* DLDO1 powers the HDMI PHY; the rail is off at reset, so the PHY
     * never drives TMDS and the monitor sees nothing. */
    pmic_enable_dldo1();
    clocks_on();
    pixel_clock(148500);
    set32(HDMI_PHY + 0x04, 1u << 31);                         /* REXT on (H6 PHY init)     */
    w32(HDMI_PHY + 0x04, (r32(HDMI_PHY + 0x04) & 0xFFFF) | 0x80C00000u);
    if (hr(0x0000) == 0 && hr(0x0002) == 0) { kprintf("[hdmi] the controller doesn't answer\n"); return -1; }
    hw(0x01FF, 0x03);                                         /* IH_MUTE: no interrupts    */
    hw(0x7E05, 0x08); hw(0x7E06, 0x88);                       /* I2CM interrupts (polled)  */
    hw(0x3027, 0x08); hw(0x3028, 0x88);                       /* PHY I2CM the same         */
    hw(0x3006, (uint8_t)~0x02);                               /* PHY_MASK0: HPD            */
    hw(0x3007, 0x12);                                         /* PHY_POL0: HPD, RX sense   */
    hw(0x0104, 0x01);
    hmod(PHY_CONF0, C0_ENHPDRX, C0_ENHPDRX);                  /* hot-plug and RX sense on  */
    hw_up = 1;
    kprintf("[hdmi] DesignWare HDMI %02x%02x, PHY type %02x\n", hr(0x0001), hr(0x0000), hr(0x0006));
    delay_ms(20);
    kprintf("[hdmi] PHY_CONF0 %02x PHY_STAT0 %02x REXT %08x\n",
            hr(PHY_CONF0), hr(PHY_STAT0), r32(HDMI_PHY + 0x04));
    plugged = hdmi_connected();
    if (plugged) read_monitor();
    else kprintf("[hdmi] nothing on the HDMI port\n");
    return 0;
}

int hdmi_modes(const video_mode_t **m) { *m = modes; return plugged ? nmodes : 0; }
const char *hdmi_monitor(void) { return plugged ? (monitor[0] ? monitor : "A monitor") : 0; }
const video_mode_t *hdmi_shown(void) { return showing ? &shown : 0; }

void hdmi_off(void) {
    if (!hw_up) return;
    w32(TCON_TV + 0x90, 0);
    w32(TCON_TV + 0x00, 0);
    if (showing) phy_power_off();
    showing = 0;
    screen_scanout(0);
}

int hdmi_show(const video_mode_t *m, const uint32_t *fb) {
    if (!hw_up || !m) return -1;
    hdmi_off();
    pixel_clock(m->khz);

    uint32_t ht = m->w + m->hfp + m->hs + m->hbp, vt = m->h + m->vfp + m->vs + m->vbp;
    uint32_t size = ((uint32_t)(m->h - 1) << 16) | (uint32_t)(m->w - 1);

    /* TCON TOP: mixer 0 -> TCON TV0 -> HDMI, with TV0's clock let through. */
    w32(TCON_TOP + 0x1C, (3u << 4) | 2u);
    w32(TCON_TOP + 0x20, (1u << 28) | (1u << 20));

    /* The mixer: one full-screen UI layer, XRGB, opaque, into pipe 0. */
    w32(DE_TOP + 0x00, 1);                                    /* run                       */
    w32(DE_TOP + 0x0C, 1);
    w32(DE_TOP + 0x08, size);
    w32(DE_UI + 0x00, (0xFFu << 24) | (4u << 8) | (1u << 1) | 1u);   /* alpha 255, XRGB8888, layer alpha, on */
    w32(DE_UI + 0x04, size);
    w32(DE_UI + 0x08, 0);
    w32(DE_UI + 0x0C, (uint32_t)m->w * 4);
    w32(DE_UI + 0x10, (uint32_t)(uintptr_t)fb);
    w32(DE_UI + 0x80, 0);
    w32(DE_UI + 0x88, size);
    w32(DE_BLD + 0x00, (1u << 8) | 1u);                       /* pipe 0 on, fill colour    */
    w32(DE_BLD + 0x04, 0xFF000000u);
    w32(DE_BLD + 0x08, size);
    w32(DE_BLD + 0x0C, 0);
    w32(DE_BLD + 0x80, 1);                                    /* pipe 0 <- channel 1       */
    w32(DE_BLD + 0x84, 0);
    w32(DE_BLD + 0x88, 0xFF000000u);                          /* black behind it           */
    w32(DE_BLD + 0x8C, size);
    for (int i = 0; i < 4; i++) w32(DE_BLD + 0x90 + 4 * i, 0x03010301u);
    w32(DE_BLD + 0xFC, 0);
    w32(DE_TOP + 0x10, 1);                                    /* commit                    */

    /* TCON TV0: the timings. */
    int delay = (int)(vt - m->h) - 2;
    if (delay > 30) delay = 30;
    w32(TCON_TV + 0x90, (uint32_t)delay << 4);
    w32(TCON_TV + 0x94, ((uint32_t)(m->w - 1) << 16) | (uint32_t)(m->h - 1));
    w32(TCON_TV + 0x98, ((uint32_t)(m->w - 1) << 16) | (uint32_t)(m->h - 1));
    w32(TCON_TV + 0x9C, ((uint32_t)(m->w - 1) << 16) | (uint32_t)(m->h - 1));
    w32(TCON_TV + 0xA0, ((ht - 1) << 16) | (uint32_t)(m->hs + m->hbp - 1));
    w32(TCON_TV + 0xA4, ((vt * 2) << 16) | (uint32_t)(m->vs + m->vbp - 1));
    w32(TCON_TV + 0xA8, ((uint32_t)(m->hs - 1) << 16) | (uint32_t)(m->vs - 1));
    w32(TCON_TV + 0xF0, (1u << 26) | (m->hpos ? 1u << 25 : 0) | (m->vpos ? 1u << 24 : 0));
    w32(TCON_TV + 0x00, (1u << 31) | 1u);                     /* on, pins to channel 1     */
    set32(TCON_TV + 0x90, 1u << 31);

    /* The controller: DVI signalling (no audio to carry), RGB in and out. */
    hw(0x1000, (uint8_t)((m->vpos ? 0x40 : 0) | (m->hpos ? 0x20 : 0) | 0x10));   /* FC_INVIDCONF */
    hw(0x1001, (uint8_t)m->w); hw(0x1002, (uint8_t)(m->w >> 8));
    hw(0x1003, (uint8_t)(ht - m->w)); hw(0x1004, (uint8_t)((ht - m->w) >> 8));
    hw(0x1005, (uint8_t)m->h); hw(0x1006, (uint8_t)(m->h >> 8));
    hw(0x1007, (uint8_t)(vt - m->h));
    hw(0x1008, (uint8_t)m->hfp); hw(0x1009, (uint8_t)(m->hfp >> 8));
    hw(0x100A, (uint8_t)m->hs); hw(0x100B, (uint8_t)(m->hs >> 8));
    hw(0x100C, (uint8_t)m->vfp);
    hw(0x100D, (uint8_t)m->vs);
    /* The PHY, for the clock the PLL really gives. The databook says to do
     * it twice (Linux's dw_hdmi_phy_init does). */
    uint32_t pk = pll_khz ? pll_khz : m->khz;
    int locked = -1;
    for (int i = 0; i < 2; i++) locked = phy_configure(pk);
    kprintf("[hdmi] PHY at %u kHz: %s, %d I2C errors, PHY_STAT0 %02x CONF0 %02x\n", pk,
            locked == 0 ? "locked" : "PLL NOT LOCKED", phy_i2c_errors, hr(PHY_STAT0), hr(PHY_CONF0));
    hw(0x4001, 0x7E);                                         /* MC_CLKDIS: pixel clock on */
    hw(0x4001, 0x7C);                                         /* and TMDS                 */
    hw(0x1011, 12); hw(0x1012, 32); hw(0x1013, 1);            /* control periods           */
    hw(0x1014, 0x0b); hw(0x1015, 0x16); hw(0x1016, 0x21);     /* preambles                 */
    hw(0x4004, 0);                                            /* MC_FLOWCTRL: CSC bypassed */
    hw(0x0801, 4 << 4);                                       /* VP_PR_CD: 24 bits         */
    hw(0x0802, 0x27);                                         /* VP_STUFF                  */
    hw(0x0803, 0);                                            /* VP_REMAP                  */
    hw(0x0804, 0x47);                                         /* VP_CONF: bypass           */
    hw(0x0200, 0x01);                                         /* TX_INVID0: RGB 8 bit      */
    hw(0x0201, 0x07);
    for (uint32_t r = 0x0202; r <= 0x0207; r++) hw(r, 0);
    hmod(0x5000, 0x04, 0x00);                                 /* HDCP: no rx detect        */
    hmod(0x5009, 0x10, 0x10);                                 /* DE active high            */
    hmod(0x5001, 0x02, 0x02);                                 /* no encryption             */
    hw(0x4002, (uint8_t)~0x02);                               /* clear the overflow        */
    uint8_t v = hr(0x1000);
    for (int i = 0; i < 4; i++) hw(0x1000, v);

    shown = *m;
    shown.hz = (uint16_t)mode_hz(m);
    showing = 1;
    screen_scanout(1);
    kprintf("[hdmi] showing %dx%d at %d Hz (%u.%02u MHz)\n", m->w, m->h, shown.hz,
            (unsigned)(m->khz / 1000), (unsigned)(m->khz % 1000 / 10));
    return 0;
}

/* Is the display reading rows y0..y1 of front, or about to? TCON TV0's
 * debug register counts the frame's lines in bits 11:0 (0 .. vtotal - 1,
 * checked on the Pi), blanking included. Where the active rows sit in that
 * count, and how far ahead the mixer fetches, isn't documented, so the band
 * takes the whole blanking and 48 lines either side. 0 with no monitor. */
int hdmi_beam_near(int y0, int y1) {
    if (!showing) return 0;
    int vt = shown.h + shown.vfp + shown.vs + shown.vbp;
    int line = (int)(r32(TCON_TV + 0xFC) & 0xFFF);
    int a = y0 - 48, b = y1 + (vt - shown.h) + 48;
    for (int k = -1; k <= 1; k++) {
        int l = line + k * vt;
        if (l >= a && l <= b) return 1;
    }
    return 0;
}

/* The front buffer moved (a new size): point the layer at it. */
void hdmi_retarget(const uint32_t *fb) {
    if (!showing) return;
    w32(DE_UI + 0x10, (uint32_t)(uintptr_t)fb);
    w32(DE_TOP + 0x10, 1);
    screen_scanout(1);
}

/* ---- the screen's size --------------------------------------------------------------------------------
 *  choice 0 is Automatic: the monitor's preferred mode, or with no monitor
 *  whatever the screen is (1024 x 600 at boot, for VNC). Any other choice is
 *  a size; on a monitor it must be one of its modes, or Automatic stands in
 *  for it until a monitor that has it comes along.                          */
typedef struct { uint16_t w, h, hz; } disp_pick_t;
static disp_pick_t pick;                                       /* 0 x 0: Automatic          */
static const disp_pick_t vnc_sizes[] = { { 1024, 600, 0 }, { 1280, 720, 0 }, { 1280, 800, 0 }, { 1600, 900, 0 }, { 1920, 1080, 0 } };
#define NVNC ((int)(sizeof vnc_sizes / sizeof vnc_sizes[0]))
static uint64_t last_poll;
static int hpd_seen;

int display_count(void) { return 1 + (plugged ? nmodes : NVNC); }

static disp_pick_t option(int i) {
    if (i <= 0) return (disp_pick_t){ 0, 0, 0 };
    if (plugged) return (disp_pick_t){ modes[i - 1].w, modes[i - 1].h, modes[i - 1].hz };
    return vnc_sizes[i - 1];
}

void display_label(int i, char *out, int cap) {
    if (i <= 0) {
        if (plugged) snprintf(out, (size_t)cap, "Automatic  " "\xB7" "  %d x %d", modes[0].w, modes[0].h);
        else snprintf(out, (size_t)cap, "Automatic");
        return;
    }
    disp_pick_t p = option(i);
    if (p.hz) snprintf(out, (size_t)cap, "%d x %d  " "\xB7" "  %d Hz", p.w, p.h, p.hz);
    else snprintf(out, (size_t)cap, "%d x %d", p.w, p.h);
}

int display_current(void) {
    if (!pick.w) return 0;
    for (int i = 1; i < display_count(); i++) {
        disp_pick_t p = option(i);
        if (p.w == pick.w && p.h == pick.h && (!plugged || p.hz == pick.hz)) return i;
    }
    return 0;
}

static int same_timing(const video_mode_t *a, const video_mode_t *b) {
    return a->w == b->w && a->h == b->h && a->khz == b->khz && a->hfp == b->hfp && a->hs == b->hs &&
           a->hbp == b->hbp && a->vfp == b->vfp && a->vs == b->vs && a->vbp == b->vbp;
}

/* Make the screen what the choice and the cable say. */
static void display_apply(void) {
    const video_mode_t *m = 0;
    int w = scr_w, h = scr_h;
    if (plugged && nmodes) {
        m = &modes[0];
        for (int i = 0; pick.w && i < nmodes; i++)
            if (modes[i].w == pick.w && modes[i].h == pick.h && modes[i].hz == pick.hz) m = &modes[i];
        w = m->w; h = m->h;
    } else if (pick.w) {
        /* No monitor answered EDID/HPD (some cheap LCD panels). Use a
         * known mode that matches the saved choice so the panel still
         * gets a signal. */
        for (int i = 0; i < NKNOWN; i++)
            if (known[i].w == pick.w && known[i].h == pick.h) { m = &known[i]; break; }
        if (m) { w = m->w; h = m->h; }
        else { w = pick.w; h = pick.h; }
    } else {
        /* Nothing detected and no saved choice: still output a default
         * signal so an LCD panel that doesn't answer EDID gets one. 720p
         * first (standard HDMI, the panel's scaler takes it). */
        w = 1280; h = 720;
        m = 0;
        for (int i = 0; i < NKNOWN; i++)
            if (known[i].w == 1280 && known[i].h == 720) { m = &known[i]; break; }
    }
    if (showing && (!m || w != scr_w || h != scr_h)) hdmi_off();
    if ((w != scr_w || h != scr_h) && screen_resize(w, h) != 0) {
        kprintf("[display] no memory for %dx%d: staying at %dx%d\n", w, h, scr_w, scr_h);
        if (m && (m->w != scr_w || m->h != scr_h)) m = 0;
    }
    if (m && showing && same_timing(&shown, m)) hdmi_retarget(g_front);
    else if (m) hdmi_show(m, g_front);
    shell_request_frame();
}

void display_choose(int i) {
    pick = option(i);
    keep_put("disp", &pick, (int)sizeof pick);
    display_apply();
}

/* After the card's files are back: a choice saved before a power cut. */
void display_restore(void) {
    disp_pick_t p;
    if (keep_get("disp", &p, (int)sizeof p) != (int)sizeof p || p.w > 1920 || p.h > 1200) return;
    if (p.w == pick.w && p.h == pick.h && p.hz == pick.hz) return;
    pick = p;
    display_apply();
}

/* Before screen_init: the size to start at. */
void display_start(int *w, int *h) {
    disp_pick_t p;
    if (keep_get("disp", &p, (int)sizeof p) == (int)sizeof p && p.w <= 1920 && p.h <= 1200) pick = p;
    hdmi_init();
    hpd_seen = plugged;
    if (plugged && nmodes) {
        const video_mode_t *m = &modes[0];
        for (int i = 0; pick.w && i < nmodes; i++)
            if (modes[i].w == pick.w && modes[i].h == pick.h && modes[i].hz == pick.hz) m = &modes[i];
        *w = m->w; *h = m->h;
    } else if (pick.w) { *w = pick.w; *h = pick.h; }
}

/* After screen_init: start showing it. */
void display_show(void) {
    /* Always try to output once the HDMI hardware is up. A monitor that
     * doesn't assert HPD or answer EDID (some cheap LCD panels) still
     * needs a signal, so don't wait for a plug event. */
    if (hw_up) display_apply();
}

/* Once a frame: the cable, twice a second. A change has to hold for two
 * looks before it counts (a plug going in bounces). */
void display_poll(void) {
    if (!hw_up || now_ms() - last_poll < 500) return;
    last_poll = now_ms();
    int now = hdmi_connected();
    if (now != hpd_seen) { hpd_seen = now; return; }
    if (now == plugged) return;
    plugged = now;
    slept = 0;                                                /* a plug in or out is news: not asleep */
    if (plugged) {
        kprintf("[hdmi] a monitor was plugged in\n");
        delay_ms(50);
        read_monitor();
        display_apply();
    } else {
        kprintf("[hdmi] the monitor was unplugged\n");
        hdmi_off();                                           /* the screen keeps its size */
        shell_request_frame();
    }
}

/* ---- screen sleep (shell.c decides when) ------------------------------------------------------ */
int  display_lit(void) { return !hw_up || showing; }
int  display_plugged(void) { return plugged; }
int display_sleep(void) {
    if (!hw_up || !showing) return 0;
    hdmi_off();                                               /* no signal: the monitor goes to standby */
    slept = 1;
    return 1;
}
void display_wake(void) {
    slept = 0;
    if (hw_up && !showing) display_apply();
}

/* For Settings and About: one line on what's showing where. */
void display_status(char *out, int cap) {
    if (g_plat == PLAT_QEMU) snprintf(out, (size_t)cap, "%d x %d in QEMU's window and over Screen Sharing.", scr_w, scr_h);
    else if (showing) snprintf(out, (size_t)cap, "%s on the HDMI port, at %d x %d and %d Hz.", hdmi_monitor(), shown.w, shown.h, shown.hz);
    else if (slept) snprintf(out, (size_t)cap, "The screen is asleep. Any key, click or touch wakes it.");
    else if (plugged) snprintf(out, (size_t)cap, "%s is plugged in, but isn't showing anything.", hdmi_monitor());
    else snprintf(out, (size_t)cap, "Nothing on the HDMI port. The screen is %d x %d, for Screen Sharing.", scr_w, scr_h);
}

/* ---- /hdmi (developer): what the hardware says, and pokes for bring-up -----------------
 *    /hdmi                 the registers that matter
 *    /hdmi?ddc             look at the cable again, read the EDID, apply
 *    /hdmi?show            set the mode up again
 *    /hdmi?off             stop the output
 *    /hdmi?r=ADDR          read a 32-bit register (hex), ?w=ADDR,VAL writes one
 *    /hdmi?rb=OFF          read a controller byte register, ?wb=OFF,VAL writes one   */
static const char *hexarg(const char *s, const char *key, uint32_t *v) {
    const char *p = strstr(s, key);
    if (!p) return 0;
    p += strlen(key);
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    uint32_t x = 0; int n = 0;
    for (;; p++, n++) {
        char c = *p;
        if (c >= '0' && c <= '9') x = x * 16 + (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') x = x * 16 + (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') x = x * 16 + (uint32_t)(c - 'A' + 10);
        else break;
    }
    if (!n) return 0;
    *v = x;
    return p;
}

static int mmio_ok(uint32_t a) {
    return !(a & 3) && ((a >= 0x01000000u && a < 0x01400000u) || (a >= 0x03000000u && a < 0x03002000u) ||
                        (a >= 0x06000000u && a < 0x06520000u) || (a >= 0x07010000u && a < 0x07030000u));
}

int hdmi_debug(const char *path, char *out, int cap) {
    const char *q = strchr(path, '?');
    q = q ? q + 1 : "";
    uint32_t a, v;
    const char *p;
    if (!hw_up) return snprintf(out, (size_t)cap, "HDMI isn't up (platform %d)\n", g_plat);
    if ((p = hexarg(q, "wb=", &a)) && *p == ',' && hexarg(p, ",", &v) && a < 0x10000) {
        hw(a, (uint8_t)v);
        return snprintf(out, (size_t)cap, "HDMI+%04x <- %02x, now %02x\n", a, v & 0xFF, hr(a));
    }
    if (hexarg(q, "rb=", &a) && a < 0x10000) return snprintf(out, (size_t)cap, "HDMI+%04x = %02x\n", a, hr(a));
    if ((p = hexarg(q, "w=", &a)) && *p == ',' && hexarg(p, ",", &v)) {
        if (!mmio_ok(a)) return snprintf(out, (size_t)cap, "%08x: not a display register\n", a);
        w32(a, v);
        return snprintf(out, (size_t)cap, "%08x <- %08x, now %08x\n", a, v, r32(a));
    }
    if (hexarg(q, "r=", &a)) {
        if (!mmio_ok(a)) return snprintf(out, (size_t)cap, "%08x: not a display register\n", a);
        return snprintf(out, (size_t)cap, "%08x = %08x\n", a, r32(a));
    }
    int n = 0;
    if (!strcmp(q, "ddc")) {
        plugged = hdmi_connected();
        n += snprintf(out + n, (size_t)(cap - n), "connected: %d (HPD %d, by DDC %d)\n", plugged,
                      !!(hr(PHY_STAT0) & ST0_HPD), hpd_less);
        if (plugged) { read_monitor(); hpd_seen = plugged; }
        display_apply();
    } else if (!strcmp(q, "show")) {
        if (showing) { video_mode_t m = shown; hdmi_off(); hdmi_show(&m, g_front); }
        else display_apply();
    } else if (!strcmp(q, "off")) hdmi_off();
    n += snprintf(out + n, (size_t)(cap - n),
        "controller %02x%02x  PHY type %02x  plugged %d (by DDC %d)  showing %d",
        hr(0x0001), hr(0x0000), hr(0x0006), plugged, hpd_less, showing);
    if (showing) n += snprintf(out + n, (size_t)(cap - n), " %dx%d@%d (%u kHz, PLL gives %u kHz)",
                               shown.w, shown.h, shown.hz, shown.khz, pll_khz);
    n += snprintf(out + n, (size_t)(cap - n),
        "\nmonitor \"%s\", %d modes, EDID %d bytes\n"
        "PHY_CONF0 %02x  PHY_STAT0 %02x  PHY_POL0 %02x  IH_PHY_STAT0 %02x  IH_I2CM_STAT0 %02x  IH_I2CMPHY_STAT0 %02x\n"
        "MC_CLKDIS %02x  MC_PHYRSTZ %02x  FC_INVIDCONF %02x  FC_INHACTV %d  FC_INVACTV %d  VP_CONF %02x  TX_INVID0 %02x\n"
        "glue: DBG %08x  REXT %08x\n"
        "CCU: VIDEO0 %08x  HDMI %08x  SLOW %08x  HDMI_BGR %08x  TCON_TV0 %08x  TCON_TV_BGR %08x  TCON_TOP_BGR %08x\n"
        "TCON_TOP: PORT_SEL %08x  GATE %08x\n"
        "TCON_TV0: GCTL %08x  CTL %08x  B0 %08x  B3 %08x  B4 %08x  B5 %08x  POL %08x\n"
        "DE: TOP %08x  SIZE %08x  UI ATTR %08x  UI ADDR %08x\n",
        monitor, nmodes, edid_len,
        hr(PHY_CONF0), hr(PHY_STAT0), hr(0x3007), hr(0x0104), hr(0x0105), hr(0x0108),
        hr(0x4001), hr(0x4005), hr(0x1000), hr(0x1001) | hr(0x1002) << 8, hr(0x1005) | hr(0x1006) << 8,
        hr(0x0804), hr(0x0200),
        r32(HDMI_PHY + 0x00), r32(HDMI_PHY + 0x04),
        r32(CCU + 0x040), r32(CCU + 0xB00), r32(CCU + 0xB04), r32(CCU + 0xB1C), r32(CCU + 0xB80),
        r32(CCU + 0xB9C), r32(CCU + 0xB5C),
        r32(TCON_TOP + 0x1C), r32(TCON_TOP + 0x20),
        r32(TCON_TV + 0x00), r32(TCON_TV + 0x90), r32(TCON_TV + 0x94), r32(TCON_TV + 0xA0),
        r32(TCON_TV + 0xA4), r32(TCON_TV + 0xA8), r32(TCON_TV + 0xF0),
        r32(DE_TOP + 0x00), r32(DE_TOP + 0x08), r32(DE_UI + 0x00), r32(DE_UI + 0x10));
    return n;
}
