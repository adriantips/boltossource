#pragma once
/* ===========================================================================
 *  BoltOS  -  include/boltmark.h
 *
 *  The BoltOS mark, the one on boltos.dev: a bolt through a broken ring.
 *  Drawn from the website's own path data (website/src/components/site/
 *  Logo.tsx) so it is the same shape at 14 px in a menu bar and at 400 px on
 *  a television, and so the startup animations can take it apart and put it
 *  back together: the two arcs sweep in, the bolt strikes down through them.
 *
 *  One file, kernel/boltmark.c, shared by the desktop, BoltOS TV and BoltOS
 *  Mini. It only rasterises coverage; colour, and where it goes, is the
 *  caller's, through gfx_mask().
 *
 *  Units: the website's viewBox, 76 units from 12 to 88 on both axes. "size"
 *  is how many pixels those 76 units span.
 * ===========================================================================*/
#include <stdint.h>

typedef struct {
    float ring;    /* 0..1: how much of each arc, drawn clockwise from its first end  */
    float spin;    /* degrees the arcs are still turned back; they settle at 0        */
    float bolt;    /* 0..1: how much of the bolt, from its top point down             */
    float sheen;   /* <0: none; 0..1: a band of light crossing top left to bottom right */
    float glow;    /* 0: none; else how far the glow reaches, in mark units           */
    float stroke;  /* the ring's width in mark units; 0 = 3.2, the website's          */
} bm_pose_t;

/* The finished mark, nothing moving. */
#define BM_STILL ((bm_pose_t){ 1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f })

/* Coverage of pixels [x0, x0+w) x [y0, y0+h) of a mark `size` px across whose
 * box starts at pixel (0, 0) -- negative x0/y0 reach into the glow around it.
 * Each mask is 0..255, `stride` bytes a row; any of them can be 0 (not wanted).
 *   mark   the shape itself
 *   sheen  the light band and the bolt's hot leading edge, inside the shape
 *   glow   a soft falloff around the shape, under it */
void boltmark_raster(const bm_pose_t *p, float size, int x0, int y0, int w, int h,
                     uint8_t *mark, uint8_t *sheen, uint8_t *glow, int stride);

/* Where a mark `size` px across goes so its ring is centred on (cx, cy):
 * the top left of its box. (The ring, not the box: the box is the website's,
 * and the bolt reaches further up than down.) */
void boltmark_center(int cx, int cy, int size, int *x, int *y);

/* The ring's width for a mark this many px across: the website's 3.2 units,
 * heavier when that would be thinner than about a pixel. */
float bm_stroke_for(int size);

/* The finished mark, `size` px square, top left at (x, y), in colour c.
 * Cached per size, so drawing it every frame costs a mask blit. */
void boltmark_draw(int x, int y, int size, uint32_t c, int alpha);

/* A pose, for the animations: the glow (gc/ga) under the mark (mc/ma) under
 * the sheen (sc/sa). The box is `size` px at (x, y); the glow spills past it
 * by p->glow units. The masks are kept until the pose or size changes, so a
 * settled animation costs no rasterising. Alphas of 0 skip that layer. */
void boltmark_draw_pose(int x, int y, int size, const bm_pose_t *p,
                        uint32_t mc, int ma, uint32_t sc, int sa, uint32_t gc, int ga);

/* Let go of the animation's masks once it is over (the finished-mark cache
 * stays: it is small, and menus and About pages draw from it). */
void boltmark_release(void);

/* Animation helpers, shared so the three editions move the same way. */
float bm_clamp01(float t);
float bm_ease_out(float t);          /* cubic: fast, then settling      */
float bm_ease_in_out(float t);       /* smoothstep                     */
float bm_span(float t, float a, float b);   /* 0 before a, 1 after b, linear between */
