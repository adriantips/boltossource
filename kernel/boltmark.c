/* ===========================================================================
 *  BoltOS  -  kernel/boltmark.c
 *
 *  The BoltOS mark, rasterised from the website's path data (see
 *  include/boltmark.h). Shared by the desktop, BoltOS TV and BoltOS Mini.
 *
 *  Every pixel asks how far it is from the shape -- a signed distance, in
 *  mark units -- and the coverage is that distance over the size of a pixel.
 *  Distances rather than a scanline fill because the same number then gives
 *  the anti-aliased edge, the glow around the shape (how far out), and the
 *  pieces of the animation: an arc that is only partly drawn is the ring
 *  intersected with a wedge, a bolt that is only partly down is the bolt
 *  intersected with a half-plane, and intersecting distances is a max().
 *
 *  The geometry, from website/src/components/site/Logo.tsx:
 *    arc   M61.9 28   A23.8 23.8 0 0 1 46.3 72.1
 *    arc   M38.6 69.5 A23.8 23.8 0 0 1 53.3 25
 *    bolt  M63.2 18.2 L36.5 52 L48.2 52 L37.8 78 L63.6 44.8 L51.4 44.8 Z
 *  The arcs' centres and angles below are those SVG arcs converted to centre
 *  form (screen angles: 0 is +x, positive turns clockwise, y down).
 * ===========================================================================*/
#include <stdint.h>
#include "boltmark.h"
#include "gfx.h"
#include "kheap.h"
#include "string.h"

#if defined(__x86_64__)
static inline float bsq(float x) { float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }
#else
static inline float bsq(float x) { return __builtin_sqrtf(x); }
#endif

#define BOX0   12.0f                 /* the viewBox: 12..88 on both axes    */
#define BOXW   76.0f
#define RING_R 23.8f
#define BOLT_T 18.2f                 /* the bolt's top point, and its bottom */
#define BOLT_B 78.0f
#define DEG    0.0174532925f

static const float BV[6][2] = {
    { 63.2f, 18.2f }, { 36.5f, 52.0f }, { 48.2f, 52.0f },
    { 37.8f, 78.0f }, { 63.6f, 44.8f }, { 51.4f, 44.8f },
};

typedef struct { float cx, cy, a0, sweep; } arc_t;
static const arc_t ARCS[2] = {
    { 49.948f, 48.581f, -59.855f, 158.672f },   /* top right, round to the bottom */
    { 49.906f, 48.557f, 118.361f, 159.838f },   /* bottom left, round to the top  */
};

float bm_clamp01(float t) { return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t; }
float bm_ease_out(float t) { t = 1.0f - bm_clamp01(t); return 1.0f - t * t * t; }
float bm_ease_in_out(float t) { t = bm_clamp01(t); return t * t * (3.0f - 2.0f * t); }
float bm_span(float t, float a, float b) { return b <= a ? (t >= b ? 1.0f : 0.0f) : bm_clamp01((t - a) / (b - a)); }

/* atan2 in degrees, to about a thousandth of one: plenty for an edge that is
 * anti-aliased over a pixel. */
static float atan2deg(float y, float x) {
    float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
    if (mx == 0.0f) return 0.0f;
    float a = mn / mx, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    if (y < 0) r = -r;
    return r * 57.2957795f;
}

/* Signed distance to the bolt: negative inside. */
static float bolt_sd(float u, float v) {
    float d2 = 1e18f;
    int in = 0;
    for (int i = 0; i < 6; i++) {
        float ax = BV[i][0], ay = BV[i][1];
        float bx = BV[(i + 1) % 6][0], by = BV[(i + 1) % 6][1];
        float ex = bx - ax, ey = by - ay, wx = u - ax, wy = v - ay;
        float t = (wx * ex + wy * ey) / (ex * ex + ey * ey);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float qx = wx - ex * t, qy = wy - ey * t, q = qx * qx + qy * qy;
        if (q < d2) d2 = q;
        if ((ay > v) != (by > v) && u < ax + (v - ay) * ex / ey) in = !in;
    }
    float d = bsq(d2);
    return in ? -d : d;
}

void boltmark_raster(const bm_pose_t *p, float size, int x0, int y0, int w, int h,
                     uint8_t *mark, uint8_t *sheen, uint8_t *glow, int stride) {
    if (size <= 0 || w <= 0 || h <= 0) return;
    float k = BOXW / size;                       /* mark units per pixel */
    float hw = (p->stroke > 0 ? p->stroke : 3.2f) * 0.5f;
    float ring = bm_clamp01(p->ring), bolt = bm_clamp01(p->bolt);
    float G = glow && p->glow > 0 ? p->glow : 0.0f;
    float reach = G + k;                         /* past this, nothing to draw */
    /* The bolt's lower edge, a pixel past the tips at either end so 0 and 1
     * mean none and all of it. */
    float vcut = BOLT_T - 1.0f + bolt * (BOLT_B - BOLT_T + 2.0f);
    float len[2], st[2];
    for (int a = 0; a < 2; a++) { len[a] = ARCS[a].sweep * ring; st[a] = ARCS[a].a0 - p->spin; }
    int band = sheen && p->sheen >= 0.0f;
    float bc = -0.3f + 1.6f * p->sheen;          /* the band's centre, off the shape at 0 and 1 */
    int hot = sheen && bolt > 0.0f && bolt < 1.0f;

    for (int j = 0; j < h; j++) {
        float v = BOX0 + ((float)(y0 + j) + 0.5f) * k;
        uint8_t *mr = mark ? mark + j * stride : 0;
        uint8_t *sr = sheen ? sheen + j * stride : 0;
        uint8_t *gr = glow ? glow + j * stride : 0;
        int brow = bolt > 0.0f && v > BOLT_T - reach && v < vcut + reach;
        for (int i = 0; i < w; i++) {
            float u = BOX0 + ((float)(x0 + i) + 0.5f) * k;
            float d = 1e9f, db = 1e9f;
            if (brow && u > 36.5f - reach && u < 63.6f + reach) {
                db = bolt_sd(u, v);
                if (v - vcut > db) db = v - vcut;       /* only down to the cut */
                d = db;
            }
            if (ring > 0.0f) {
                for (int a = 0; a < 2; a++) {
                    float dx = u - ARCS[a].cx, dy = v - ARCS[a].cy;
                    float dist = bsq(dx * dx + dy * dy);
                    float rad = (dist > RING_R ? dist - RING_R : RING_R - dist) - hw;
                    if (rad > reach || rad > d) continue;
                    float t = atan2deg(dy, dx) - st[a];
                    while (t < 0.0f) t += 360.0f;
                    while (t >= 360.0f) t -= 360.0f;
                    float m;
                    if (t <= len[a]) m = t < len[a] - t ? t : len[a] - t;
                    else { float o1 = t - len[a], o2 = 360.0f - t; m = -(o1 < o2 ? o1 : o2); }
                    float da = -m * DEG * RING_R;             /* along the ring, past its ends */
                    float dd = rad > da ? rad : da;
                    if (dd < d) d = dd;
                }
            }
            float cov = 0.5f - d / k;
            cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
            if (mr) mr[i] = (uint8_t)(cov * 255.0f + 0.5f);
            if (sr) {
                float s = 0.0f;
                if (cov > 0.0f && band) {
                    float bw = ((u - BOX0) + (v - BOX0)) / (2.0f * BOXW) - bc;
                    bw = bw < 0 ? -bw : bw;
                    float b = 1.0f - bw / 0.11f;
                    if (b > 0) s = b * b;
                }
                if (cov > 0.0f && hot && db <= d + 0.001f) {    /* the strike's white-hot front */
                    float e = 1.0f - (vcut - v) / 9.0f;
                    if (e > 0 && e * e > s) s = e * e > 1.0f ? 1.0f : e * e;
                }
                sr[i] = (uint8_t)(s * cov * 255.0f + 0.5f);
            }
            if (gr) {
                float g = 0.0f;
                if (G > 0.0f) {
                    if (d <= 0.0f) g = 1.0f;
                    else if (d < G) { g = 1.0f - d / G; g = g * g; }
                }
                gr[i] = (uint8_t)(g * 255.0f + 0.5f);
            }
        }
    }
}

void boltmark_center(int cx, int cy, int size, int *x, int *y) {
    /* the ring's centre, (49.93, 48.57), from the box's corner at (12, 12) */
    *x = cx - (int)(37.93f * (float)size / BOXW + 0.5f);
    *y = cy - (int)(36.57f * (float)size / BOXW + 0.5f);
}

float bm_stroke_for(int size) {
    float minw = 1.15f * BOXW / (float)(size > 1 ? size : 1);
    return minw > 3.2f ? (minw > 7.0f ? 7.0f : minw) : 0.0f;
}

/* ---- the finished mark, cached per size --------------------------------- */
#define NSLOT 6
static struct { int size; uint8_t *m; uint32_t used; } slot[NSLOT];
static uint32_t slot_clock;

void boltmark_draw(int x, int y, int size, uint32_t c, int alpha) {
    if (size < 4 || size > 1024 || alpha <= 0) return;
    int s = -1, lru = 0;
    for (int i = 0; i < NSLOT; i++) {
        if (slot[i].m && slot[i].size == size) { s = i; break; }
        if (slot[i].used < slot[lru].used) lru = i;
    }
    if (s < 0) {
        s = lru;
        if (slot[s].m) kfree(slot[s].m);
        slot[s].m = (uint8_t *)kmalloc((uint64_t)size * size);
        slot[s].size = size;
        if (!slot[s].m) return;
        bm_pose_t p = BM_STILL;
        p.stroke = bm_stroke_for(size);      /* small marks get a heavier ring */
        boltmark_raster(&p, (float)size, 0, 0, size, size, slot[s].m, 0, 0, size);
    }
    slot[s].used = ++slot_clock;
    gfx_mask(x, y, slot[s].m, size, size, size, c, alpha);
}

/* ---- a pose, for the animations ----------------------------------------- */
static struct {
    bm_pose_t p;
    int size, pad, dim, valid, has_s, has_g;
    uint8_t *m, *s, *g;
    int cap;
} pc;

static int pose_same(const bm_pose_t *a, const bm_pose_t *b) {
    return a->ring == b->ring && a->spin == b->spin && a->bolt == b->bolt &&
           a->sheen == b->sheen && a->glow == b->glow && a->stroke == b->stroke;
}

void boltmark_draw_pose(int x, int y, int size, const bm_pose_t *p,
                        uint32_t mc, int ma, uint32_t sc, int sa, uint32_t gc, int ga) {
    if (size < 4 || size > 2048) return;
    int want_s = sa > 0, want_g = ga > 0 && p->glow > 0;
    int pad = want_g ? (int)(p->glow * (float)size / BOXW) + 2 : 0;
    int dim = size + 2 * pad;
    if (!pc.valid || pc.size != size || pc.pad != pad || !pose_same(&pc.p, p) ||
        (want_s && !pc.has_s) || (want_g && !pc.has_g)) {
        int need = dim * dim;
        if (need > pc.cap) {
            if (pc.m) kfree(pc.m);
            if (pc.s) kfree(pc.s);
            if (pc.g) kfree(pc.g);
            pc.m = (uint8_t *)kmalloc((uint64_t)need);
            pc.s = (uint8_t *)kmalloc((uint64_t)need);
            pc.g = (uint8_t *)kmalloc((uint64_t)need);
            pc.cap = (pc.m && pc.s && pc.g) ? need : 0;
            if (!pc.cap) { pc.valid = 0; return; }
        }
        boltmark_raster(p, (float)size, -pad, -pad, dim, dim, pc.m, want_s ? pc.s : 0, want_g ? pc.g : 0, dim);
        pc.p = *p; pc.size = size; pc.pad = pad; pc.dim = dim; pc.valid = 1;
        pc.has_s = want_s; pc.has_g = want_g;
    }
    int ox = x - pad, oy = y - pad;
    if (want_g) gfx_mask(ox, oy, pc.g, dim, dim, dim, gc, ga);
    if (ma > 0) gfx_mask(ox, oy, pc.m, dim, dim, dim, mc, ma);
    if (want_s) gfx_mask(ox, oy, pc.s, dim, dim, dim, sc, sa);
}

void boltmark_release(void) {
    if (pc.m) kfree(pc.m);
    if (pc.s) kfree(pc.s);
    if (pc.g) kfree(pc.g);
    pc.m = pc.s = pc.g = 0;
    pc.cap = 0;
    pc.valid = 0;
}
