/* ===========================================================================
 *  BoltOS Mini  -  mini/splash.c
 *
 *  The five seconds between the firmware handing over and the desktop:
 *  the BoltOS Mini logo (assets/mini/splash-logo.png, built in by
 *  build-mini.sh through mini/tools/mkimg.py) on a dark field, a loading
 *  bar under it. Everything else in the kernel carries on bringing the machine up
 *  behind the curtain; any key or click skips the wait.
 *
 *  The bar is time, not work: boot itself is done in a few hundred
 *  milliseconds long before the five are up, so the fill is a smoothstep
 *  across the five seconds rather than a mirror of some fraction of init.
 *  It reaches the end exactly as the splash stops, never before and never
 *  after.
 * =========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "ui.h"                        /* S(), th (for the accent only)     */
#include "string.h"
#include "stdio.h"
#include "kheap.h"
#include "buildid.h"                   /* BOLT_VERSION                      */

#define SPLASH_MS 5000

/* The splash is always dark, whatever the theme: it is the one screen that
 * is the OS rather than the user's desktop. Only the accent follows the
 * theme, so the touch of colour carries through. */
#define SP_BG_TOP 0x141419
#define SP_BG_BOT 0x0A0A0D
#define SP_TEXT   0xE8E9EC
#define SP_SUB    0xA2A5AD
#define SP_FAINT  0x6A6E78
#define SP_TRACK  0x26272B

static uint64_t splash_t0;
static int      splash_started, splash_skipped;

void splash_start(void) { splash_t0 = now_ms(); splash_started = 1; splash_skipped = 0; }

static uint32_t *logo_px;
static int logo_w, logo_h;
static void logo_free(void) { if (logo_px) { kfree(logo_px); logo_px = 0; } }

int splash_active(void) {
    if (!splash_started || splash_skipped || now_ms() - splash_t0 >= SPLASH_MS) { logo_free(); return 0; }
    return 1;
}

void splash_dismiss(void) { splash_skipped = 1; }

/* The logo: "BIMG", width, height, then premultiplied ARGB (mkimg.py).
 * The linked-in bytes needn't be 4-byte aligned, so the pixels are copied
 * out once, for the five seconds, and let go when the splash ends. */
extern const uint8_t _binary_splash_logo_img_start[], _binary_splash_logo_img_end[];
static int logo_load(void) {
    if (logo_px) return 1;
    const uint8_t *d = _binary_splash_logo_img_start;
    size_t n = (size_t)(_binary_splash_logo_img_end - d);
    uint32_t w, h;
    if (n < 12 || memcmp(d, "BIMG", 4)) return 0;
    memcpy(&w, d + 4, 4); memcpy(&h, d + 8, 4);
    if (!w || !h || 12 + (size_t)w * h * 4 > n) return 0;
    logo_px = (uint32_t *)kmalloc((size_t)w * h * 4);
    if (!logo_px) return 0;
    memcpy(logo_px, d + 12, (size_t)w * h * 4);
    logo_w = (int)w; logo_h = (int)h;
    return 1;
}

void splash_draw(void) {
    int cx = scr_w / 2;
    gfx_bind(&g_back, 0, 0, 0, 0, scr_w, scr_h);
    gfx_vgrad(0, 0, scr_w, scr_h, SP_BG_TOP, SP_BG_BOT, 255);

    /* The logo, as wide as 440 of the 600-high design, centred over the bar.
     * Narrow screens get it narrower, never cut. */
    if (logo_load()) {
        int lw = S(440), max_w = scr_w - S(48);
        if (lw > max_w) lw = max_w;
        int lh = (int)((long)lw * logo_h / logo_w);
        gfx_image(cx - lw / 2, S(250) - lh / 2, lw, lh, logo_px, logo_w, logo_h, 255);
    } else {
        text_c(cx, S(286), "BoltOS Mini", FONT_DISPLAY, S(60), SP_TEXT);
    }

    /* the loading bar: smoothstepped across the five seconds */
    uint64_t el = now_ms() - splash_t0;
    float p = (float)el / (float)SPLASH_MS;
    if (p > 1.0f) p = 1.0f;
    float eased = p * p * (3.0f - 2.0f * p);
    int bw = S(240), bh = S(5);
    int bx = cx - bw / 2, by = S(380);
    gfx_rrect(bx, by, bw, bh, bh / 2, SP_TRACK, 255);
    int fw = (int)((float)bw * eased + 0.5f);
    if (fw > 0) gfx_rrect(bx, by, fw < bh ? bh : fw, bh, bh / 2, th.accent, 255);

    char v[64];
    snprintf(v, sizeof v, "BoltOS Mini " BOLT_VERSION);
    text_l(S(28), scr_h - S(22), v, FONT_REGULAR, S(11), SP_FAINT);
    text_r(scr_w - S(28), scr_h - S(22), "press any key to skip", FONT_REGULAR, S(11), SP_FAINT);
}
