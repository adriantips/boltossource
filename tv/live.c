/* ===========================================================================
 *  BoltOS TV  -  tv/live.c
 *
 *  Live TV: each film is a channel, on all the time. Up and Down (or
 *  Channel +/-) flip channels with the banner a television shows -- number,
 *  name, what is on, the time -- the number keys tune directly, and OK opens
 *  the guide: every channel with a still of what it is showing, over the
 *  picture, which keeps playing behind it.
 * ===========================================================================*/
#include <stdint.h>
#include "tv.h"
#include "string.h"
#include "stdio.h"

static int chan;                       /* current channel */
static int guide, gfocus;
static uint64_t banner_t0, digits_t0, last_tick;
static char digits[4];
static float gscroll;
static glasscache_t gc_banner, gc_guide;

#define BANNER_MS 5200

int live_channel(void) { return chan; }

void live_tune(int c) {
    if (c < 0) c = tv_nfilms - 1;
    if (c >= tv_nfilms) c = 0;
    chan = c;
    if (tv_top() == &view_live) shell_film_lock(chan);
    banner_t0 = tv_now_ms();
    tv_redraw();
}

static void live_enter(void) {
    shell_film_lock(chan);
    banner_t0 = tv_now_ms();
    guide = 0;
}

static void live_key(tvkey_t k) {
    uint64_t now = tv_now_ms();
    if (guide) {
        switch (k.k) {
        case K_UP: case K_CHUP:   if (gfocus > 0) { gfocus--; sfx_move(); } break;
        case K_DOWN: case K_CHDN: if (gfocus < tv_nfilms - 1) { gfocus++; sfx_move(); } break;
        case K_OK: case K_RIGHT:  sfx_select(); guide = 0; live_tune(gfocus); break;
        case K_BACK: case K_LEFT: case K_GUIDE: sfx_back(); guide = 0; break;
        }
        return;
    }
    switch (k.k) {
    case K_UP: case K_CHUP:   sfx_move(); live_tune(chan + 1); break;
    case K_DOWN: case K_CHDN: sfx_move(); live_tune(chan - 1); break;
    case K_OK: case K_GUIDE: case K_LEFT:
        sfx_select(); guide = 1; gfocus = chan; gscroll = (float)gfocus; break;
    case K_INFO: case K_RIGHT: banner_t0 = now; break;
    case K_BACK: sfx_back(); tv_pop(); break;
    case K_CHAR:
        if (k.ch >= '0' && k.ch <= '9') {
            int n = (int)strlen(digits);
            if (n >= 2 || now - digits_t0 > 1800) n = 0;
            digits[n] = (char)k.ch;
            digits[n + 1] = 0;
            digits_t0 = now;
        }
        break;
    }
}

static void live_pointer(int x, int y, int click, int wheel) {
    if (wheel) { live_tune(chan + (wheel > 0 ? 1 : -1)); return; }
    if (!guide) { if (click) { guide = 1; gfocus = chan; } return; }
    int gx = S(56), gy = S(56) + S(150);
    for (int i = 0; i < tv_nfilms; i++) {
        int ry = gy + i * S(132);
        if (ui_hit(x, y, gx + S(16), ry, S(744), S(124))) {
            gfocus = i;
            if (click) { guide = 0; live_tune(i); }
        }
    }
}

static void live_tick(uint64_t now) {
    uint64_t dt = last_tick ? now - last_tick : 16;
    last_tick = now;
    if (digits[0] && now - digits_t0 > 1500) {
        int n = 0;
        for (char *p = digits; *p; p++) n = n * 10 + (*p - '0');
        digits[0] = 0;
        if (n >= 1 && n <= tv_nfilms) live_tune(n - 1);
        tv_redraw();
    }
    float ns = ui_ease(gscroll, (float)gfocus, dt, 160);
    if (ns != gscroll) { gscroll = ns; tv_redraw(); }
    if (now - banner_t0 < BANNER_MS + 400 || digits[0]) tv_redraw();
}

static void chip(int x, int cy, const char *s, uint32_t bg, uint32_t fg, int alpha) {
    int w = font_width(FONT_BOLD, S(18), s, -1) + S(22);
    gfx_rrect(x, cy - S(15), w, S(30), S(8), bg, alpha);
    ui_text(FONT_BOLD, S(18), x + w / 2, cy, s, fg, alpha, 1);
}

static void draw_banner(uint64_t now) {
    uint64_t dt = now - banner_t0;
    if (dt > BANNER_MS) return;
    float k = dt < 260 ? (float)dt / 260.0f : dt > BANNER_MS - 500 ? (float)(BANNER_MS - dt) / 500.0f : 1.0f;
    k = 1.0f - (1.0f - k) * (1.0f - k);
    int a = (int)(k * 255);
    const film_t *f = &tv_films[chan];
    int x = S(96), w = tv_w - S(192), h = S(196);
    int y = tv_h - S(72) - h + (int)((1.0f - k) * S(60));
    ui_glass(&gc_banner, x, y, w, h, S(40), 165, a);

    /* the number, on a tile in the film's own colour */
    int nx = x + S(36), ny = y + S(34), ns = S(128);
    gfx_rrect_vgrad(nx, ny, ns, ns, S(28), f->tint, 0x101418, a);
    gfx_rrect_stroke2(nx, ny, ns, ns, S(28), 0xFFFFFF, a * 120 / 255, a * 20 / 255);
    char num[8];
    snprintf(num, sizeof num, "%d", chan + 1);
    ui_text(FONT_SEMIBOLD, S(64), nx + ns / 2, ny + ns / 2, num, 0xFFFFFF, a, 1);

    int tx = nx + ns + S(36);
    int cw = ui_text(FONT_SEMIBOLD, S(42), tx, y + S(66), f->channel, 0xFFFFFF, a, 0);
    chip(tx + cw + S(18), y + S(66), "LIVE", 0xFF3B30, 0xFFFFFF, a);
    chip(tx + cw + S(18) + font_width(FONT_BOLD, S(18), "LIVE", -1) + S(32), y + S(66), "1080p", 0xFFFFFF,
         0x101318, a * 220 / 255);
    ui_text(FONT_MEDIUM, S(28), tx, y + S(116), f->title, 0xF2F4F8, a, 0);
    char sub[160];
    snprintf(sub, sizeof sub, "%s  \x95  Filmed by %s", f->place, f->credit);
    ui_text(FONT_REGULAR, S(22), tx, y + S(156), sub, 0xB9C0CC, a, 0);

    int h24, m;
    tv_clock(&h24, &m, 0, 0, 0, 0, 0);
    char t[16];
    fmt_time(t, sizeof t, h24, m);
    int right = x + w - S(40);
    ui_text(FONT_SEMIBOLD, S(40), right, y + S(66), t, 0xFFFFFF, a, 2);
    const film_t *nf = &tv_films[(chan + 1) % tv_nfilms];
    char next[96];
    snprintf(next, sizeof next, "Up next on %d: %s", (chan + 1) % tv_nfilms + 1, nf->channel);
    ui_text(FONT_REGULAR, S(22), right, y + S(116), next, 0xB9C0CC, a, 2);
    ui_text(FONT_REGULAR, S(20), right, y + S(156), "OK  Guide      \x8F\x90  Channels", 0x8F97A5, a, 2);
}

static void draw_guide(void) {
    float t = 1.0f;
    int a = (int)(255 * t);
    int x = S(56), y = S(56), w = S(776), h = tv_h - S(112);
    ui_glass(&gc_guide, x, y, w, h, S(40), 185, a);
    ui_text(FONT_SEMIBOLD, S(40), x + S(40), y + S(64), "Guide", 0xFFFFFF, a, 0);
    int h24, m;
    tv_clock(&h24, &m, 0, 0, 0, 0, 0);
    char tm[16];
    fmt_time(tm, sizeof tm, h24, m);
    ui_text(FONT_MEDIUM, S(28), x + w - S(40), y + S(64), tm, 0xC9CED8, a, 2);
    ui_text(FONT_REGULAR, S(22), x + S(40), y + S(108), "Every channel is live. Pick one and press OK.",
            0xAEB5C2, a, 0);

    int ly = y + S(150), rh = S(132);
    gfx_clip_push(x, ly - S(8), w, h - (ly - y) - S(24));
    int fyy = ly + (int)(gscroll * rh);
    ui_focus(x + S(16), fyy, w - S(32), rh - S(8), S(26), a);
    for (int i = 0; i < tv_nfilms; i++) {
        const film_t *f = &tv_films[i];
        int ry = ly + i * rh, on = i == gfocus;
        uint32_t ink = on ? 0x101318 : 0xFFFFFF, dim = on ? 0x3C4250 : 0xB4BBC7;
        int sw, sh;
        const uint32_t *px = film_still(i, &sw, &sh);
        int tw = S(176), th = S(99);
        int sx = x + S(36), sy = ry + (rh - S(8) - th) / 2;
        if (px) gfx_image_rr(sx, sy, tw, th, px, sw, sh, S(14), a);
        else gfx_rrect(sx, sy, tw, th, S(14), f->tint, a);
        if (i == chan) {                         /* what is on now */
            gfx_rrect(sx + S(8), sy + S(8), S(56), S(26), S(7), 0xFF3B30, a);
            ui_text(FONT_BOLD, S(15), sx + S(36), sy + S(21), "LIVE", 0xFFFFFF, a, 1);
        }
        char num[8];
        snprintf(num, sizeof num, "%d", i + 1);
        int cx = sx + tw + S(30);
        ui_text(FONT_SEMIBOLD, S(30), cx, ry + S(36), num, on ? 0x101318 : 0x8E96A4, a, 0);
        ui_text(FONT_SEMIBOLD, S(30), cx + S(42), ry + S(36), f->channel, ink, a, 0);
        ui_text(FONT_MEDIUM, S(22), cx + S(42), ry + S(72), f->title, ink, a * 230 / 255, 0);
        font_draw_fit(FONT_REGULAR, S(19), cx + S(42), ry + S(108), f->blurb, x + w - S(48) - (cx + S(42)),
                      dim, a);
    }
    gfx_clip_pop();
}

static void draw_digits(uint64_t now) {
    if (!digits[0] || now - digits_t0 > 1500) return;
    int w = S(150), h = S(110), x = tv_w - S(64) - w, y = S(64);
    gfx_rrect(x, y, w, h, S(28), 0x000000, 150);
    ui_rim(x, y, w, h, S(28), 255);
    char s[8];
    snprintf(s, sizeof s, "%s%s", digits, (now / 400) % 2 ? "_" : " ");
    ui_text(FONT_SEMIBOLD, S(60), x + w / 2, y + h / 2, s, 0xFFFFFF, 255, 1);
}

static void live_draw(uint64_t now) {
    if (guide) draw_guide();
    else draw_banner(now);
    draw_digits(now);
}

const view_t view_live = {
    .name = "live", .backdrop = BG_FILM,
    .enter = live_enter, .key = live_key, .pointer = live_pointer,
    .draw = live_draw, .tick = live_tick,
};
