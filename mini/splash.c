/* ===========================================================================
 *  BoltOS Mini  -  mini/splash.c
 *
 *  The five seconds between the firmware handing over and the desktop: the
 *  BoltOS mark, the one on boltos.dev, drawing itself on a flat dark field
 *  (kernel/boltmark.c). The two arcs of the ring sweep round into place, the
 *  bolt drops through the gap between them with a front in the accent colour,
 *  a single thin ripple goes out from the ring where it lands, and the name
 *  and a hairline progress bar come up underneath. Everything else in the
 *  kernel carries on bringing the machine up behind it; any key or click
 *  skips the wait.
 *
 *  Flat, like the rest of Mini: no blur, no glow, no gradients.
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
#include "boltmark.h"
#include "buildid.h"                   /* BOLT_VERSION                      */

#define SPLASH_MS 5000

/* The splash is always dark, whatever the theme: it is the one screen that
 * is the OS rather than the user's desktop. Only the accent follows the
 * theme, so the touch of colour carries through. */
#define SP_BG     0x0F1013
#define SP_TEXT   0xE8E9EC
#define SP_FAINT  0x6A6E78
#define SP_TRACK  0x26272B

static uint64_t splash_t0;
static int      splash_started, splash_skipped, splash_freed;

void splash_start(void) { splash_t0 = now_ms(); splash_started = 1; splash_skipped = 0; splash_freed = 0; }

int splash_active(void) {
    if (!splash_started || splash_skipped || now_ms() - splash_t0 >= SPLASH_MS) {
        if (splash_started && !splash_freed) { boltmark_release(); splash_freed = 1; }
        return 0;
    }
    return 1;
}

void splash_dismiss(void) { splash_skipped = 1; }

void splash_draw(void) {
    int cx = scr_w / 2;
    float t = (float)(now_ms() - splash_t0);
    gfx_bind(&g_back, 0, 0, 0, 0, scr_w, scr_h);
    gfx_fill(0, 0, scr_w, scr_h, SP_BG);

    /* the mark: its ring centred a little above the middle */
    int size = S(200), max = scr_w - S(64);
    if (size > max) size = max;
    int cy = S(238), mx, my;
    boltmark_center(cx, cy, size, &mx, &my);
    bm_pose_t p = BM_STILL;
    float ring = bm_ease_out(bm_span(t, 150.0f, 1050.0f));
    p.ring = ring;
    p.spin = -90.0f * (1.0f - ring);
    float b = bm_span(t, 850.0f, 1100.0f);
    p.bolt = b * b;
    p.stroke = bm_stroke_for(size);
    int striking = p.bolt > 0.0f && p.bolt < 1.0f;
    boltmark_draw_pose(mx, my, size, &p, SP_TEXT, 255, th.accent, striking ? 255 : 0, 0, 0);

    /* where the bolt lands: one thin ripple out from the ring, in the accent */
    float rp = bm_span(t, 1100.0f, 1750.0f);
    if (rp > 0.0f && rp < 1.0f) {
        float r0 = 25.4f * (float)size / 76.0f;
        float r = r0 + (float)S(32) * bm_ease_out(rp);
        gfx_ring((float)cx, (float)cy, r, (float)S(2) > 1.5f ? (float)S(2) : 1.5f, th.accent, (int)(200.0f * (1.0f - rp)));
    }

    /* the name, and under it the hairline bar */
    float w = bm_ease_out(bm_span(t, 1150.0f, 1600.0f));
    int ny = my + size * 66 / 76 + S(40);
    if (w > 0.0f) {
        const char *name = "BoltOS Mini";
        int px = S(22), nw = font_width(FONT_MEDIUM, px, name, -1);
        font_draw(FONT_MEDIUM, px, cx - nw / 2, ny + (font_capheight(FONT_MEDIUM, px) + 1) / 2 + (int)((float)S(8) * (1.0f - w)),
                  name, -1, SP_TEXT, (int)(255.0f * w));
    }
    float pr = t / (float)SPLASH_MS;
    if (pr > 1.0f) pr = 1.0f;
    float eased = pr * pr * (3.0f - 2.0f * pr);
    int bw = S(180), bh = S(2) > 1 ? S(2) : 1;
    int bx = cx - bw / 2, by = ny + S(36);
    gfx_fill(bx, by, bw, bh, SP_TRACK);
    int fw = (int)((float)bw * eased + 0.5f);
    if (fw > 0) gfx_fill(bx, by, fw, bh, th.accent);

    char v[64];
    snprintf(v, sizeof v, "BoltOS Mini " BOLT_VERSION);
    text_l(S(28), scr_h - S(22), v, FONT_MONO, S(11), SP_FAINT);
    text_r(scr_w - S(28), scr_h - S(22), "press any key to skip", FONT_REGULAR, S(11), SP_FAINT);
}
