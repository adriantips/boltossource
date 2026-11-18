/* ===========================================================================
 *  BoltOS Mini  -  mini/splash.c
 *
 *  The five seconds between the firmware handing over and the desktop:
 *  the BoltOS bolt over the wordmark on a dark field, a loading bar under
 *  it. Everything else in the kernel carries on bringing the machine up
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

int splash_active(void) {
    if (!splash_started || splash_skipped) return 0;
    return now_ms() - splash_t0 < SPLASH_MS;
}

void splash_dismiss(void) { splash_skipped = 1; }

/* The bolt: two slashes meeting at the kink, drawn with the same thick
 * anti-aliased lines the glyphs use, over a faint halo of the accent. */
static void draw_bolt(float cx, float cy, float s, uint32_t c) {
    gfx_circle(cx, cy, s * 1.5f, c, 16);
    float w = s * 0.30f;
    /* upper slash: top-right tip down to the kink */
    gfx_line(cx + s * 0.34f, cy - s * 0.92f, cx - s * 0.08f, cy + s * 0.06f, w, c, 255);
    /* lower slash: kink down to the bottom-left tip */
    gfx_line(cx + s * 0.20f, cy - s * 0.02f, cx - s * 0.34f, cy + s * 0.92f, w, c, 255);
    /* bridge across the kink so the two meet cleanly */
    gfx_line(cx - s * 0.08f, cy + s * 0.06f, cx + s * 0.20f, cy - s * 0.02f, w * 0.8f, c, 255);
}

void splash_draw(void) {
    int cx = scr_w / 2;
    gfx_bind(&g_back, 0, 0, 0, 0, scr_w, scr_h);
    gfx_vgrad(0, 0, scr_w, scr_h, SP_BG_TOP, SP_BG_BOT, 255);

    float bolt_cy = (float)S(176);
    draw_bolt((float)cx, bolt_cy, (float)S(56), th.accent);

    text_c(cx, S(286), "BoltOS", FONT_DISPLAY, S(60), SP_TEXT);
    text_c(cx, S(336), "M I N I", FONT_SEMIBOLD, S(12), SP_SUB);

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
