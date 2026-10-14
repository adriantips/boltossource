#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS  -  include/gfx.h
 *
 *  The drawing layer under the desktop. The compositor, the shell and every
 *  app draw through this: a target surface, an origin, a clip, and a set of
 *  primitives that are anti-aliased wherever an edge is not axis-aligned.
 *
 *  Coordinates are always desktop coordinates. A window's draw callback is
 *  handed screen positions and draws at screen positions, but since the
 *  compositor started rendering each window into its own surface the pixels
 *  land in that surface instead: the target's origin is the window's corner,
 *  and gfx subtracts it. No app had to change for that, which was the point.
 *
 *  Colours are 0x00RRGGBB. Where a pixel buffer carries alpha (icons,
 *  decoded images) it is 0xAARRGGBB, straight (not premultiplied).
 * ===========================================================================*/

typedef struct {
    uint32_t *px;
    int       w, h;
    int       stride;          /* in pixels */
} surface_t;

typedef struct { int x, y, w, h; } rect_t;

/* ---- target, origin and clip ------------------------------------------- *
 * gfx_bind points every draw call at `s`, with desktop (ox, oy) landing on
 * its pixel (0, 0). The base clip (bx..bw) is the most anything may touch
 * until the next bind -- the compositor sets it to the damage rectangle it is
 * repainting, or to a window's client area while that window draws. The
 * legacy g_set_clip() intersects with it and g_clear_clip() returns to it,
 * so an app that clears its clip can no longer paint outside its window. */
void gfx_bind(surface_t *s, int ox, int oy, int bx, int by, int bw, int bh);
surface_t *gfx_target(int *ox, int *oy);
void gfx_clip_push(int x, int y, int w, int h);     /* intersect, and remember */
void gfx_clip_pop(void);
void gfx_clip_get(int *x, int *y, int *w, int *h);
int  gfx_clip_empty(void);
int  gfx_clip_hits(int x, int y, int w, int h);     /* does the rect touch the clip */

/* ---- colour ----------------------------------------------------------- */
static inline uint32_t gfx_mix(uint32_t d, uint32_t s, uint32_t a256) {
    uint32_t rb = ((s & 0xFF00FFu) * a256 + (d & 0xFF00FFu) * (256u - a256)) >> 8;
    uint32_t g  = ((s & 0x00FF00u) * a256 + (d & 0x00FF00u) * (256u - a256)) >> 8;
    return (rb & 0xFF00FFu) | (g & 0x00FF00u);
}
static inline uint32_t gfx_a256(int a) { return (uint32_t)(a + (a >> 7)); }  /* 0..255 -> 0..256 */
uint32_t gfx_lerp(uint32_t a, uint32_t b, int t, int tmax);
uint32_t gfx_shade(uint32_t c, int pct);                /* 100 = same, <100 darker, >100 lighter */
int      gfx_luma(uint32_t c);                          /* 0..255 */

/* ---- rectangles ------------------------------------------------------- */
void gfx_fill(int x, int y, int w, int h, uint32_t c);
void gfx_fill_a(int x, int y, int w, int h, uint32_t c, int a);
void gfx_vgrad(int x, int y, int w, int h, uint32_t top, uint32_t bot, int a);
void gfx_hgrad(int x, int y, int w, int h, uint32_t left, uint32_t right, int a);

/* Anti-aliased rounded rectangle. Radii are clamped to half the short side.
 * The four-radius form exists for title bars (rounded top only) and for
 * maximised windows, which keep none. */
void gfx_rrect(int x, int y, int w, int h, int r, uint32_t c, int a);
void gfx_rrect4(int x, int y, int w, int h, int tl, int tr, int br, int bl, uint32_t c, int a);
void gfx_rrect_vgrad(int x, int y, int w, int h, int r, uint32_t top, uint32_t bot, int a);
/* A ring `t` px wide just inside the rounded rect's edge. The two-alpha form
 * fades from a_top at the top edge to a_bot at the bottom -- which is how a
 * glass panel catches the light along its upper rim and not its lower one. */
void gfx_rrect_stroke(int x, int y, int w, int h, int r, int t, uint32_t c, int a);
void gfx_rrect_stroke2(int x, int y, int w, int h, int r, uint32_t c, int a_top, int a_bot);

/* ---- round things ------------------------------------------------------ */
void gfx_circle(float cx, float cy, float r, uint32_t c, int a);
void gfx_ring(float cx, float cy, float r, float t, uint32_t c, int a);
void gfx_line(float x0, float y0, float x1, float y1, float width, uint32_t c, int a);
/* An arc of a ring, angles in degrees clockwise from 12 o'clock. Spinners and
 * progress rings. */
void gfx_arc(float cx, float cy, float r, float t, float a0, float a1, uint32_t c, int a);

/* ---- shadows ----------------------------------------------------------- *
 * The soft shadow of a rounded rectangle: (x, y, w, h, r) is the object, the
 * shadow is that shape grown by `spread`, moved down `dy`, blurred by `blur`
 * (roughly CSS box-shadow's blur radius) at `opacity`. Nothing is painted
 * under the object itself -- it is assumed opaque, and painting 300,000
 * pixels of shadow that the window then covers was half the cost of a frame. */
void gfx_shadow(int x, int y, int w, int h, int r, int blur, int dy, int spread, int opacity);

/* ---- images and surfaces ------------------------------------------------ */
/* Premultiplied ARGB image, scaled bilinearly into (x, y, w, h). Decoded
 * images are straight alpha: icons.c premultiplies its atlases once. */
void gfx_image(int x, int y, int w, int h, const uint32_t *src, int sw, int sh, int alpha);
/* A w*h window into a bigger image (an atlas): sw*sh source, `stride` wide. */
void gfx_image_sub(int x, int y, int w, int h, const uint32_t *src, int sw, int sh, int stride, int alpha);
/* The same, rounding the corners of what it draws. */
void gfx_image_rr(int x, int y, int w, int h, const uint32_t *src, int sw, int sh, int r, int alpha);
/* 8-bit coverage mask tinted with `c` (text glyphs, monochrome icons). */
void gfx_mask(int x, int y, const uint8_t *m, int mw, int mh, int mstride, uint32_t c, int a);
/* Copy an opaque surface region; the four-radius form trims the corners. */
void gfx_copy(int dx, int dy, const surface_t *s, int sx, int sy, int w, int h);
void gfx_copy_rr(int dx, int dy, const surface_t *s, int r, int a);
/* A surface drawn into (x, y, w, h): bilinear, faded by `a`, corners at `r`
 * (in destination pixels). Window open/close/minimise animations. */
void gfx_surface_scaled(int x, int y, int w, int h, const surface_t *s, int r, int a);

/* Bulk pixel moves, exported for the framebuffer driver and the compositor.
 * rep movs / rep stos: QEMU's TCG has a fast path for both and none for a
 * dword loop, so this is the difference between 12 fps and 40 in a VM. */
void gfx_memcpy32(uint32_t *d, const uint32_t *s, int n);
void gfx_memset32(uint32_t *d, uint32_t v, int n);

/* ---- glass ------------------------------------------------------------- *
 * The material the shell is made of. A backdrop is a blurred, tinted copy of
 * whatever is under a panel, captured at a quarter of the resolution (blur
 * throws the detail away anyway) and stretched back up bilinearly when the
 * panel draws. Capturing is the expensive half, so it is cached: the panel
 * redraws from the cache for as long as nothing underneath it changes.     */
typedef struct {
    uint32_t tint;          /* colour mixed over the blurred backdrop        */
    int      tint_a;        /* 0..255: how much of it                        */
    int      sat;           /* saturation, percent (100 = untouched)         */
    int      blur;          /* blur radius in desktop pixels                 */
    int      lift;          /* brightness added after tinting, -64..64       */
} glass_t;

typedef struct {
    int       x, y, w, h;   /* captured region (desktop coords)              */
    int       sw, sh, f;    /* small image size and downsample factor        */
    uint32_t *small;
    int       cap;          /* pixels allocated                              */
    int       valid;
} backdrop_t;

void gfx_backdrop_capture(backdrop_t *b, int x, int y, int w, int h, const glass_t *g);
void gfx_backdrop_draw(const backdrop_t *b, int x, int y, int w, int h, int r, int a);
void gfx_backdrop_free(backdrop_t *b);
/* Blur a whole surface into dst (same size) -- the lock screen's background.
 * dst may be the same buffer as src. */
void gfx_blur_surface(const surface_t *src, surface_t *dst, const glass_t *g);
