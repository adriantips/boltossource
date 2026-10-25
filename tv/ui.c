/* ===========================================================================
 *  BoltOS TV  -  tv/ui.c
 *
 *  The few things every TV screen is built from: liquid glass, the focus
 *  highlight, a slider, a switch, a spinner. The glass is the desktop's own
 *  material (kernel/gfx.c) -- a blurred, tinted copy of what is behind it,
 *  with light caught along the top rim -- re-captured every frame, because
 *  what is behind it here is a film.
 * ===========================================================================*/
#include <stdint.h>
#include "tv.h"
#include "string.h"
#include "stdio.h"
#include "kheap.h"
#include "math.h"

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

void ui_glass(glasscache_t *gc, int x, int y, int w, int h, int r, int strength, int alpha) {
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    glass_t g = {
        .tint = 0x0E1016, .tint_a = strength, .sat = 165, .blur = S(30), .lift = 6,
    };
    if (g.blur < 8) g.blur = 8;
    gfx_backdrop_capture(&gc->bd, x, y, w, h, &g);
    gc->x = x; gc->y = y; gc->w = w; gc->h = h;
    gfx_shadow(x, y, w, h, r, S(40), S(14), 0, 90 * alpha / 255);
    gfx_backdrop_draw(&gc->bd, x, y, w, h, r, alpha);
    /* a faint wash of light down from the top, then the rim */
    gfx_rrect_vgrad(x, y, w, imin(h, S(160)), r, 0xFFFFFF, 0x000000, 10 * alpha / 255);
    ui_rim(x, y, w, h, r, alpha);
}

void ui_rim(int x, int y, int w, int h, int r, int alpha) {
    gfx_rrect_stroke2(x, y, w, h, r, 0xFFFFFF, 92 * alpha / 255, 18 * alpha / 255);
    gfx_rrect_stroke(x - 1, y - 1, w + 2, h + 2, r + 1, 1, 0x000000, 60 * alpha / 255);
}

/* Near-opaque, deliberately: gfx_shadow leaves the area under an object
 * unpainted except in its rounded corners, so anything see-through drawn on
 * a shadow shows four darker squares. The light pill with dark type is the
 * television convention anyway -- it reads from across the room. */
void ui_focus(int x, int y, int w, int h, int r, int alpha) {
    if (alpha <= 0) return;
    gfx_shadow(x, y, w, h, r, S(30), S(10), 0, 80 * alpha / 255);
    gfx_rrect_vgrad(x, y, w, h, r, 0xFFFFFF, 0xE4E8F0, 246 * alpha / 255);
    gfx_rrect_stroke2(x, y, w, h, r, 0xFFFFFF, 255 * alpha / 255, 40 * alpha / 255);
}

void ui_spinner(int cx, int cy, int r, uint64_t now, uint32_t color) {
    float a0 = (float)((now * 360 / 1100) % 360);
    gfx_ring((float)cx, (float)cy, (float)r, (float)imax(2, r / 5), color, 40);
    gfx_arc((float)cx, (float)cy, (float)r, (float)imax(2, r / 5), a0, a0 + 110.0f, color, 255);
}

void ui_progress(int x, int y, int w, int h, int pct, uint32_t color) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    gfx_rrect(x, y, w, h, h / 2, 0xFFFFFF, 50);
    int fw = w * pct / 100;
    if (fw > 0) gfx_rrect(x, y, imax(fw, h), h, h / 2, color, 255);
}

void ui_slider(int x, int y, int w, int pct, int focused) {
    int h = S(10);
    ui_progress(x, y - h / 2, w, h, pct, focused ? 0xFFFFFF : 0xC8CDD8);
    int kx = x + w * pct / 100;
    int kr = focused ? S(15) : S(11);
    gfx_shadow(kx - kr, y - kr, kr * 2, kr * 2, kr, S(10), S(3), 0, 120);
    gfx_circle((float)kx, (float)y, (float)kr, 0xFFFFFF, 255);
}

void ui_switch(int x, int cy, int on, int focused) {
    int w = S(76), h = S(44);
    int y = cy - h / 2;
    gfx_rrect(x, y, w, h, h / 2, on ? 0x34C759 : 0xFFFFFF, on ? 255 : 60);
    if (focused) gfx_rrect_stroke(x - S(3), y - S(3), w + S(6), h + S(6), h / 2 + S(3), S(2), 0xFFFFFF, 220);
    int kr = h / 2 - S(4);
    int kx = on ? x + w - h / 2 : x + h / 2;
    gfx_shadow(kx - kr, cy - kr, kr * 2, kr * 2, kr, S(6), S(2), 0, 110);
    gfx_circle((float)kx, (float)cy, (float)kr, 0xFFFFFF, 255);
}

int ui_hit(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

float ui_ease(float cur, float target, uint64_t dt, int ms) {
    if (ms <= 0 || dt >= (uint64_t)ms * 3) return target;
    /* exp(-2.3 * dt / ms): 90% of the way in `ms`. A rational approximation
     * is plenty for an animation. */
    float t = 2.3f * (float)dt / (float)ms;
    float k = 1.0f / (1.0f + t + 0.5f * t * t + t * t * t / 6.0f);
    float v = target + (cur - target) * k;
    float d = v - target;
    if (d < 0.0005f && d > -0.0005f) v = target;
    return v;
}

/* Scrims: black fading to nothing, so text over a bright film stays legible.
 * Smoothstep falloff -- a linear ramp shows its edge. */
static int smooth(int i, int n, int a) {
    int t = 256 - i * 256 / (n ? n : 1);            /* 256 at the edge, 0 inside */
    return a * ((t * t * (768 - 2 * t)) >> 16) / 256;
}
void ui_scrim_top(int h, int alpha) {
    for (int i = 0; i < h; i += 2) gfx_fill_a(0, i, tv_w, 2, 0x000000, smooth(i, h, alpha));
}
void ui_scrim_bottom(int h, int alpha) {
    for (int i = 0; i < h; i += 2) gfx_fill_a(0, tv_h - i - 2, tv_w, 2, 0x000000, smooth(i, h, alpha));
}
void ui_scrim_left(int w, int alpha) {
    for (int i = 0; i < w; i += 4) gfx_fill_a(i, 0, 4, tv_h, 0x000000, smooth(i, w, alpha));
}

void fmt_int(char *out, int cap, long v) {
    char tmp[32]; int n = 0, neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    do {
        if (n && n % 4 == 3) tmp[n++] = ',';
        tmp[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u && n < 30);
    int o = 0;
    if (neg && o < cap - 1) out[o++] = '-';
    while (n && o < cap - 1) out[o++] = tmp[--n];
    out[o] = 0;
}

void fmt_time(char *out, int cap, int h, int m) {
    if (tvs.clock24) snprintf(out, (size_t)cap, "%d:%02d", h, m);
    else snprintf(out, (size_t)cap, "%d:%02d %s", (h % 12) ? h % 12 : 12, m, h < 12 ? "AM" : "PM");
}

/* Text with its caps centred on cy, aligned left/centre/right of x, with an
 * alpha -- font.h's text_l/c/r have no alpha, and everything on a TV fades. */
int ui_text(int face, int px, int x, int cy, const char *s, uint32_t color, int alpha, int align) {
    if (!s || !s[0] || alpha <= 0) return 0;
    int w = font_width(face, px, s, -1);
    int bx = align == 1 ? x - w / 2 : align == 2 ? x - w : x;
    int base = cy + font_capheight(face, px) / 2;
    font_draw(face, px, bx, base, s, -1, color, alpha > 255 ? 255 : alpha);
    return w;
}

/* The same over a film: a soft dark copy underneath, so white text stays
 * readable on snow and on surf. */
int ui_text_shadow(int face, int px, int x, int cy, const char *s, uint32_t color, int alpha, int align) {
    if (!s || !s[0] || alpha <= 0) return 0;
    int d = px / 18 + 1;
    ui_text(face, px, x, cy + d, s, 0x000000, alpha * 70 / 255, align);
    ui_text(face, px, x + 1, cy + d + 1, s, 0x000000, alpha * 40 / 255, align);
    return ui_text(face, px, x, cy, s, color, alpha, align);
}

/* Word-wrapped text with an alpha. Returns lines drawn. */
int ui_wrap(int face, int px, int x, int top, int w, int line_h, int max_lines,
            const char *s, uint32_t color, int alpha) {
    int lines = 0;
    const char *p = s;
    while (*p && lines < max_lines) {
        while (*p == ' ') p++;
        int len = (int)strlen(p);
        int n = font_fit(face, px, p, len, w);
        if (n < len) {                      /* back up to a space */
            int b = n;
            while (b > 0 && p[b] != ' ') b--;
            if (b > 0) n = b;
        }
        if (n <= 0) n = len < 1 ? 0 : 1;
        int last = lines == max_lines - 1 && n < len;
        if (last) font_draw_fit(face, px, x, top + font_ascent(face, px), p, w, color, alpha);
        else font_draw(face, px, x, top + font_ascent(face, px), p, n, color, alpha);
        p += n;
        top += line_h;
        lines++;
    }
    return lines;
}

/* A soft round light (or shade): a small radial sprite, built once per
 * colour and stretched bilinearly -- a gradient survives any amount of
 * stretching, and a stack of full-size translucent circles would cost
 * millions of blends a frame. */
#define GS 96
static struct { uint32_t c; uint32_t *px; } sprites[6];

static const uint32_t *glow_sprite(uint32_t c) {
    for (int i = 0; i < 6; i++) if (sprites[i].px && sprites[i].c == c) return sprites[i].px;
    int slot = 0;
    while (slot < 5 && sprites[slot].px) slot++;
    if (!sprites[slot].px) sprites[slot].px = (uint32_t *)kmalloc(GS * GS * 4);
    if (!sprites[slot].px) return 0;
    sprites[slot].c = c;
    for (int y = 0; y < GS; y++)
        for (int x = 0; x < GS; x++) {
            float dx = (x + 0.5f - GS / 2.0f) / (GS / 2.0f), dy = (y + 0.5f - GS / 2.0f) / (GS / 2.0f);
            float d = sqrtf(dx * dx + dy * dy);
            float k = d >= 1.0f ? 0.0f : (1.0f - d) * (1.0f - d);
            uint32_t a = (uint32_t)(k * 255.0f);
            uint32_t r = ((c >> 16) & 0xFF) * a / 255, g = ((c >> 8) & 0xFF) * a / 255, b = (c & 0xFF) * a / 255;
            sprites[slot].px[y * GS + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    return sprites[slot].px;
}

void ui_glow(int cx, int cy, int rx, int ry, uint32_t color, int alpha) {
    const uint32_t *px = glow_sprite(color);
    if (px && alpha > 0) gfx_image(cx - rx, cy - ry, rx * 2, ry * 2, px, GS, GS, alpha);
}
