/* ===========================================================================
 *  BoltOS  -  kernel/gfx.c
 *
 *  Pixels. Every edge that is not a straight horizontal or vertical line is
 *  drawn with coverage, not with a yes/no test per pixel -- that single
 *  difference is most of what separated the old desktop from a real one.
 *
 *  The cost model is QEMU's TCG, because that is where this runs most of the
 *  time: there a sqrt is cheap next to a branch that mispredicts, and a dword
 *  loop is ruinous next to `rep movs`. So rounded corners come from cached
 *  coverage tables, shadows from cached nine-slices, glass from a blur done
 *  at a quarter of the resolution, and bulk copies from rep movs/stos.
 *
 *  Floating point is used where it reads better (circles, lines); the build
 *  is -msse2 and the scheduler fxsaves on every switch, as kernel/mp3.c
 *  already relies on.
 * ===========================================================================*/
#include <stdint.h>
#include "gfx.h"
#include "gui.h"
#include "kheap.h"
#include "string.h"
#include "math.h"

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline float fsq(float x) { float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }
static inline int ffloor(float v) { int i = (int)v; return (v < (float)i) ? i - 1 : i; }
static inline int fceil(float v)  { int i = (int)v; return (v > (float)i) ? i + 1 : i; }
static inline float fclamp1(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

/* ===========================================================================
 *  Bulk moves
 * ===========================================================================*/
void gfx_memcpy32(uint32_t *d, const uint32_t *s, int n) {
    if (n <= 0) return;
    uint64_t c = (uint64_t)n;
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(c) : : "memory");
}
void gfx_memset32(uint32_t *d, uint32_t v, int n) {
    if (n <= 0) return;
    uint64_t c = (uint64_t)n;
    __asm__ volatile("rep stosl" : "+D"(d), "+c"(c) : "a"(v) : "memory");
}

/* ===========================================================================
 *  Target, origin, clip
 * ===========================================================================*/
static surface_t *T;
static int OX, OY;                       /* desktop coords of T's pixel (0,0) */
static int bx0, by0, bx1, by1;           /* base clip, exclusive hi           */
static int cx0, cy0, cx1, cy1;           /* active clip                       */
#define CLIP_DEPTH 16
static int cstk[CLIP_DEPTH][4];
static int csp;

static inline uint32_t *PX(int x, int y) { return T->px + (y - OY) * T->stride + (x - OX); }

void gfx_bind(surface_t *s, int ox, int oy, int bx, int by, int bw, int bh) {
    T = s; OX = ox; OY = oy;
    bx0 = imax(bx, ox);          by0 = imax(by, oy);
    bx1 = imin(bx + bw, ox + s->w); by1 = imin(by + bh, oy + s->h);
    if (bx1 < bx0) bx1 = bx0;
    if (by1 < by0) by1 = by0;
    cx0 = bx0; cy0 = by0; cx1 = bx1; cy1 = by1;
    csp = 0;
}

surface_t *gfx_target(int *ox, int *oy) { if (ox) *ox = OX; if (oy) *oy = OY; return T; }

void g_set_clip(int x, int y, int w, int h) {
    cx0 = imax(x, bx0); cy0 = imax(y, by0);
    cx1 = imin(x + w, bx1); cy1 = imin(y + h, by1);
    if (cx1 < cx0) cx1 = cx0;
    if (cy1 < cy0) cy1 = cy0;
}
void g_clear_clip(void) { cx0 = bx0; cy0 = by0; cx1 = bx1; cy1 = by1; }

void gfx_clip_push(int x, int y, int w, int h) {
    if (csp < CLIP_DEPTH) {
        cstk[csp][0] = cx0; cstk[csp][1] = cy0; cstk[csp][2] = cx1; cstk[csp][3] = cy1;
        csp++;
    }
    cx0 = imax(x, cx0); cy0 = imax(y, cy0);
    cx1 = imin(x + w, cx1); cy1 = imin(y + h, cy1);
    if (cx1 < cx0) cx1 = cx0;
    if (cy1 < cy0) cy1 = cy0;
}
void gfx_clip_pop(void) {
    if (csp <= 0) { g_clear_clip(); return; }
    csp--;
    cx0 = cstk[csp][0]; cy0 = cstk[csp][1]; cx1 = cstk[csp][2]; cy1 = cstk[csp][3];
}
void gfx_clip_get(int *x, int *y, int *w, int *h) {
    if (x) *x = cx0;  if (y) *y = cy0;
    if (w) *w = cx1 - cx0;  if (h) *h = cy1 - cy0;
}
int gfx_clip_empty(void) { return cx1 <= cx0 || cy1 <= cy0; }
int gfx_clip_hits(int x, int y, int w, int h) {
    return x < cx1 && y < cy1 && x + w > cx0 && y + h > cy0 && w > 0 && h > 0;
}

/* ===========================================================================
 *  Colour helpers
 * ===========================================================================*/
uint32_t gfx_lerp(uint32_t a, uint32_t b, int t, int tmax) {
    if (tmax <= 0) return b;
    if (t <= 0) return a;
    if (t >= tmax) return b;
    int ar = a >> 16 & 0xFF, ag = a >> 8 & 0xFF, ab = a & 0xFF;
    int br = b >> 16 & 0xFF, bg = b >> 8 & 0xFF, bb = b & 0xFF;
    int r = ar + (br - ar) * t / tmax, g = ag + (bg - ag) * t / tmax, c = ab + (bb - ab) * t / tmax;
    return (uint32_t)(r << 16 | g << 8 | c);
}
uint32_t gfx_shade(uint32_t c, int pct) {
    int r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, b = c & 0xFF;
    if (pct <= 100) {
        r = r * pct / 100; g = g * pct / 100; b = b * pct / 100;
    } else {                       /* toward white, keeping the hue */
        int k = pct - 100; if (k > 100) k = 100;
        r += (255 - r) * k / 100; g += (255 - g) * k / 100; b += (255 - b) * k / 100;
    }
    return (uint32_t)(r << 16 | g << 8 | b);
}
int gfx_luma(uint32_t c) {
    return (int)(((c >> 16 & 0xFF) * 77 + (c >> 8 & 0xFF) * 150 + (c & 0xFF) * 29) >> 8);
}

/* ===========================================================================
 *  Spans
 * ===========================================================================*/
static inline void px_a(int x, int y, uint32_t c, uint32_t a256) {
    if (x < cx0 || x >= cx1 || y < cy0 || y >= cy1 || !a256) return;
    uint32_t *p = PX(x, y);
    *p = a256 >= 256 ? c : gfx_mix(*p, c, a256);
}
/* coverage (0..255) times alpha (0..256) -> 0..256 */
static inline uint32_t cova(int cov, uint32_t a256) { return (gfx_a256(cov) * a256) >> 8; }

static void span(int y, int x0, int x1, uint32_t c, uint32_t a256) {
    if (y < cy0 || y >= cy1) return;
    if (x0 < cx0) x0 = cx0;
    if (x1 > cx1) x1 = cx1;
    if (x1 <= x0 || !a256) return;
    uint32_t *p = PX(x0, y);
    int n = x1 - x0;
    if (a256 >= 256) { gfx_memset32(p, c, n); return; }
    uint32_t srb = (c & 0xFF00FFu) * a256, sg = (c & 0xFF00u) * a256, ia = 256 - a256;
    for (int i = 0; i < n; i++) {
        uint32_t d = p[i];
        uint32_t rb = (srb + (d & 0xFF00FFu) * ia) >> 8;
        uint32_t g  = (sg  + (d & 0x00FF00u) * ia) >> 8;
        p[i] = (rb & 0xFF00FFu) | (g & 0x00FF00u);
    }
}

void gfx_fill(int x, int y, int w, int h, uint32_t c) {
    int y0 = imax(y, cy0), y1 = imin(y + h, cy1);
    int x0 = imax(x, cx0), x1 = imin(x + w, cx1);
    if (x1 <= x0) return;
    for (int j = y0; j < y1; j++) gfx_memset32(PX(x0, j), c, x1 - x0);
}
void gfx_fill_a(int x, int y, int w, int h, uint32_t c, int a) {
    if (a <= 0) return;
    if (a >= 255) { gfx_fill(x, y, w, h, c); return; }
    uint32_t a256 = gfx_a256(a);
    int y0 = imax(y, cy0), y1 = imin(y + h, cy1);
    for (int j = y0; j < y1; j++) span(j, x, x + w, c, a256);
}
void gfx_vgrad(int x, int y, int w, int h, uint32_t top, uint32_t bot, int a) {
    if (h <= 0 || a <= 0) return;
    uint32_t a256 = gfx_a256(a);
    int y0 = imax(y, cy0), y1 = imin(y + h, cy1);
    for (int j = y0; j < y1; j++) span(j, x, x + w, gfx_lerp(top, bot, j - y, h > 1 ? h - 1 : 1), a256);
}
void gfx_hgrad(int x, int y, int w, int h, uint32_t left, uint32_t right, int a) {
    if (w <= 0 || a <= 0) return;
    uint32_t a256 = gfx_a256(a);
    int x0 = imax(x, cx0), x1 = imin(x + w, cx1), y0 = imax(y, cy0), y1 = imin(y + h, cy1);
    for (int i = x0; i < x1; i++) {
        uint32_t c = gfx_lerp(left, right, i - x, w > 1 ? w - 1 : 1);
        for (int j = y0; j < y1; j++) px_a(i, j, c, a256);
    }
}

/* ---- legacy names --------------------------------------------------------- */
void g_fill(int x, int y, int w, int h, uint32_t c) { gfx_fill(x, y, w, h, c); }
void g_blend(int x, int y, int w, int h, uint32_t c, uint8_t a) { gfx_fill_a(x, y, w, h, c, a); }
void g_hline(int x, int y, int w, uint32_t c) { gfx_fill(x, y, w, 1, c); }
void g_vline(int x, int y, int h, uint32_t c) { gfx_fill(x, y, 1, h, c); }
void g_rect(int x, int y, int w, int h, uint32_t c) {
    gfx_fill(x, y, w, 1, c); gfx_fill(x, y + h - 1, w, 1, c);
    gfx_fill(x, y + 1, 1, h - 2, c); gfx_fill(x + w - 1, y + 1, 1, h - 2, c);
}

/* ===========================================================================
 *  Rounded rectangles
 *
 *  A corner of radius r is an r x r table of coverage, built once. Row k of
 *  the table also records where ink starts and where the pixels become fully
 *  covered, so the solid middle of every row is one memset and only the
 *  handful of edge pixels pay for a blend.
 * ===========================================================================*/
typedef struct { int r; uint8_t *cov, *ink, *solid; } ctab_t;
#define NCT 32
static ctab_t ctabs[NCT];
static int    ct_next;

static const ctab_t *corner(int r) {
    for (int i = 0; i < NCT; i++) if (ctabs[i].r == r && ctabs[i].cov) return &ctabs[i];
    ctab_t *t = &ctabs[ct_next];
    ct_next = (ct_next + 1) % NCT;
    if (t->cov) kfree(t->cov);
    t->cov = (uint8_t *)kmalloc((uint64_t)r * r + 2 * (uint64_t)r);
    if (!t->cov) { t->r = 0; return 0; }
    t->r = r;
    t->ink = t->cov + r * r;
    t->solid = t->ink + r;
    for (int y = 0; y < r; y++) {
        int ink = r, solid = r;
        for (int x = 0; x < r; x++) {
            float dx = (float)r - ((float)x + 0.5f), dy = (float)r - ((float)y + 0.5f);
            float v = (float)r - fsq(dx * dx + dy * dy) + 0.5f;
            int c = (int)(fclamp1(v) * 255.0f + 0.5f);
            t->cov[y * r + x] = (uint8_t)c;
            if (c > 0 && ink == r) ink = x;
            if (c >= 255 && solid == r) solid = x;
        }
        t->ink[y] = (uint8_t)(ink > 255 ? 255 : ink);
        t->solid[y] = (uint8_t)(solid > 255 ? 255 : solid);
    }
    return t;
}

static int clamp_r(int r, int w, int h) {
    int m = imin(w, h) / 2;
    if (r > m) r = m;
    if (r > 250) r = 250;
    return r < 0 ? 0 : r;
}

/* One row of a rounded shape: the corner on each side (or none) decides where
 * the solid span starts and which edge pixels get partial coverage. */
static void rr_row(int py, int x, int w, int j, int h,
                   int tl, int tr, int br, int bl,
                   const ctab_t *ttl, const ctab_t *ttr, const ctab_t *tbr, const ctab_t *tbl,
                   uint32_t c, uint32_t a256) {
    int sl = x, sr = x + w;
    const ctab_t *lt = 0; int lr = 0, lrow = 0;
    if (j < tl)           { lt = ttl; lr = tl; lrow = j; }
    else if (j >= h - bl) { lt = tbl; lr = bl; lrow = h - 1 - j; }
    const ctab_t *rt = 0; int rr = 0, rrow = 0;
    if (j < tr)           { rt = ttr; rr = tr; rrow = j; }
    else if (j >= h - br) { rt = tbr; rr = br; rrow = h - 1 - j; }
    if (lt) {
        int ink = lt->ink[lrow], solid = lt->solid[lrow];
        const uint8_t *cv = lt->cov + lrow * lr;
        for (int k = ink; k < solid; k++) px_a(x + k, py, c, cova(cv[k], a256));
        sl = x + solid;
    }
    if (rt) {
        int ink = rt->ink[rrow], solid = rt->solid[rrow];
        const uint8_t *cv = rt->cov + rrow * rr;
        for (int k = ink; k < solid; k++) px_a(x + w - 1 - k, py, c, cova(cv[k], a256));
        sr = x + w - solid;
    }
    if (sr > sl) span(py, sl, sr, c, a256);
}

void gfx_rrect4(int x, int y, int w, int h, int tl, int tr, int br, int bl, uint32_t c, int a) {
    if (w <= 0 || h <= 0 || a <= 0) return;
    if (x >= cx1 || y >= cy1 || x + w <= cx0 || y + h <= cy0) return;
    tl = clamp_r(tl, w, h); tr = clamp_r(tr, w, h); br = clamp_r(br, w, h); bl = clamp_r(bl, w, h);
    const ctab_t *ttl = tl ? corner(tl) : 0, *ttr = tr ? corner(tr) : 0;
    const ctab_t *tbr = br ? corner(br) : 0, *tbl = bl ? corner(bl) : 0;
    if ((tl && !ttl) || (tr && !ttr) || (br && !tbr) || (bl && !tbl)) { tl = tr = br = bl = 0; }
    uint32_t a256 = gfx_a256(a > 255 ? 255 : a);
    int y0 = imax(y, cy0), y1 = imin(y + h, cy1);
    for (int py = y0; py < y1; py++)
        rr_row(py, x, w, py - y, h, tl, tr, br, bl, ttl, ttr, tbr, tbl, c, a256);
}
void gfx_rrect(int x, int y, int w, int h, int r, uint32_t c, int a) {
    gfx_rrect4(x, y, w, h, r, r, r, r, c, a);
}
void g_round(int x, int y, int w, int h, int r, uint32_t c, uint8_t a) {
    gfx_rrect(x, y, w, h, r, c, a);
}

void gfx_rrect_vgrad(int x, int y, int w, int h, int r, uint32_t top, uint32_t bot, int a) {
    if (w <= 0 || h <= 0 || a <= 0) return;
    if (x >= cx1 || y >= cy1 || x + w <= cx0 || y + h <= cy0) return;
    r = clamp_r(r, w, h);
    const ctab_t *t = r ? corner(r) : 0;
    if (r && !t) r = 0;
    uint32_t a256 = gfx_a256(a > 255 ? 255 : a);
    int y0 = imax(y, cy0), y1 = imin(y + h, cy1);
    for (int py = y0; py < y1; py++)
        rr_row(py, x, w, py - y, h, r, r, r, r, t, t, t, t,
               gfx_lerp(top, bot, py - y, h > 1 ? h - 1 : 1), a256);
}

/* Ring coverage for one corner pixel: outer disc minus the inset disc. */
static inline float ring_cov(int k, int row, int r, int t) {
    float dx = (float)r - ((float)k + 0.5f), dy = (float)r - ((float)row + 0.5f);
    float d = fsq(dx * dx + dy * dy);
    float out = fclamp1((float)r - d + 0.5f);
    float in;
    if (r - t > 0) in = fclamp1((float)(r - t) - d + 0.5f);
    else           in = (k >= t && row >= t) ? 1.0f : 0.0f;
    return out - in;
}

static void stroke_impl(int x, int y, int w, int h, int r, int t, uint32_t c, int a_top, int a_bot) {
    if (w <= 0 || h <= 0 || t <= 0) return;
    if (x >= cx1 || y >= cy1 || x + w <= cx0 || y + h <= cy0) return;
    r = clamp_r(r, w, h);
    if (r > 0 && t > r) t = r;                     /* see the note in gfx.h */
    if (t * 2 > w) t = w / 2;
    if (t * 2 > h) t = h / 2;
    if (t <= 0) return;
    int hm = h > 1 ? h - 1 : 1;
    #define AY(py) gfx_a256(a_top + (a_bot - a_top) * ((py) - y) / hm)
    /* corners */
    for (int row = 0; row < r; row++) {
        for (int k = 0; k < r; k++) {
            float cv = ring_cov(k, row, r, t);
            if (cv <= 0.0f) continue;
            int cov = (int)(cv * 255.0f + 0.5f);
            int ty = y + row, by = y + h - 1 - row;
            px_a(x + k, ty, c, cova(cov, AY(ty)));
            px_a(x + w - 1 - k, ty, c, cova(cov, AY(ty)));
            px_a(x + k, by, c, cova(cov, AY(by)));
            px_a(x + w - 1 - k, by, c, cova(cov, AY(by)));
        }
    }
    /* straight edges: top and bottom between the corners, then the sides */
    int ex = imax(r, t);
    for (int i = 0; i < t; i++) {
        span(y + i, x + ex, x + w - ex, c, AY(y + i));
        span(y + h - 1 - i, x + ex, x + w - ex, c, AY(y + h - 1 - i));
    }
    if (r < t) {      /* r == 0: the side strips own the corners */
        for (int i = 0; i < t; i++) {
            span(y + i, x, x + ex, c, AY(y + i));
            span(y + i, x + w - ex, x + w, c, AY(y + i));
            span(y + h - 1 - i, x, x + ex, c, AY(y + h - 1 - i));
            span(y + h - 1 - i, x + w - ex, x + w, c, AY(y + h - 1 - i));
        }
    }
    int y0 = imax(y + imax(r, t), cy0), y1 = imin(y + h - imax(r, t), cy1);
    for (int py = y0; py < y1; py++) {
        uint32_t a256 = AY(py);
        span(py, x, x + t, c, a256);
        span(py, x + w - t, x + w, c, a256);
    }
    #undef AY
}
void gfx_rrect_stroke(int x, int y, int w, int h, int r, int t, uint32_t c, int a) {
    stroke_impl(x, y, w, h, r, t, c, a, a);
}
void gfx_rrect_stroke2(int x, int y, int w, int h, int r, uint32_t c, int a_top, int a_bot) {
    stroke_impl(x, y, w, h, r, 1, c, a_top, a_bot);
}

/* ===========================================================================
 *  Circles, rings, lines, arcs
 * ===========================================================================*/
void gfx_circle(float cx, float cy, float r, uint32_t c, int a) {
    if (r <= 0.0f || a <= 0) return;
    uint32_t a256 = gfx_a256(a > 255 ? 255 : a);
    int x0 = imax(ffloor(cx - r - 1.0f), cx0), x1 = imin(fceil(cx + r + 1.0f), cx1);
    int y0 = imax(ffloor(cy - r - 1.0f), cy0), y1 = imin(fceil(cy + r + 1.0f), cy1);
    float rin = r - 0.71f, rout = r + 0.71f;
    float rin2 = rin > 0 ? rin * rin : -1.0f, rout2 = rout * rout;
    for (int py = y0; py < y1; py++) {
        float dy = (float)py + 0.5f - cy, dy2 = dy * dy;
        if (dy2 >= rout2) continue;
        int run0 = -1;
        for (int px = x0; px < x1; px++) {
            float dx = (float)px + 0.5f - cx, d2 = dx * dx + dy2;
            if (d2 <= rin2) { if (run0 < 0) run0 = px; continue; }
            if (run0 >= 0) { span(py, run0, px, c, a256); run0 = -1; }
            if (d2 >= rout2) continue;
            float cv = fclamp1(r - fsq(d2) + 0.5f);
            if (cv > 0.0f) px_a(px, py, c, cova((int)(cv * 255.0f + 0.5f), a256));
        }
        if (run0 >= 0) span(py, run0, x1, c, a256);
    }
}

void gfx_ring(float cx, float cy, float r, float t, uint32_t c, int a) {
    if (r <= 0.0f || t <= 0.0f || a <= 0) return;
    uint32_t a256 = gfx_a256(a > 255 ? 255 : a);
    float ri = r - t;
    int x0 = imax(ffloor(cx - r - 1.0f), cx0), x1 = imin(fceil(cx + r + 1.0f), cx1);
    int y0 = imax(ffloor(cy - r - 1.0f), cy0), y1 = imin(fceil(cy + r + 1.0f), cy1);
    float rout2 = (r + 0.71f) * (r + 0.71f);
    float rin2 = ri - 0.71f > 0 ? (ri - 0.71f) * (ri - 0.71f) : -1.0f;
    for (int py = y0; py < y1; py++) {
        float dy = (float)py + 0.5f - cy;
        for (int px = x0; px < x1; px++) {
            float dx = (float)px + 0.5f - cx, d2 = dx * dx + dy * dy;
            if (d2 >= rout2 || d2 <= rin2) continue;
            float d = fsq(d2);
            float cv = fclamp1(r - d + 0.5f) - fclamp1(ri - d + 0.5f);
            if (cv > 0.0f) px_a(px, py, c, cova((int)(cv * 255.0f + 0.5f), a256));
        }
    }
}

void gfx_line(float x0, float y0, float x1, float y1, float width, uint32_t c, int a) {
    if (a <= 0 || width <= 0.0f) return;
    uint32_t a256 = gfx_a256(a > 255 ? 255 : a);
    float hw = width * 0.5f;
    float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;
    float len = fsq(len2);
    int bx0 = imax(ffloor((x0 < x1 ? x0 : x1) - hw - 1.0f), cx0);
    int bx1 = imin(fceil((x0 > x1 ? x0 : x1) + hw + 1.0f), cx1);
    int by0 = imax(ffloor((y0 < y1 ? y0 : y1) - hw - 1.0f), cy0);
    int by1 = imin(fceil((y0 > y1 ? y0 : y1) + hw + 1.0f), cy1);
    float lim = hw + 1.0f;
    for (int py = by0; py < by1; py++) {
        float wy = (float)py + 0.5f - y0;
        for (int px = bx0; px < bx1; px++) {
            float wx = (float)px + 0.5f - x0;
            float d;
            if (len2 < 1e-6f) {
                d = fsq(wx * wx + wy * wy);
            } else {
                float perp = (wx * vy - wy * vx) / len;
                if (perp > lim || perp < -lim) continue;
                float t = (wx * vx + wy * vy) / len2;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                float dx = wx - t * vx, dy = wy - t * vy;
                d = fsq(dx * dx + dy * dy);
            }
            float cv = fclamp1(hw - d + 0.5f);
            if (cv > 0.0f) px_a(px, py, c, cova((int)(cv * 255.0f + 0.5f), a256));
        }
    }
}

/* atan2 in degrees, clockwise from 12 o'clock, 0..360. Good to a few tenths
 * of a degree, which is a fraction of a pixel at any radius drawn here. */
static float angle_cw(float dx, float dy) {
    float ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
    if (mx < 1e-6f) return 0.0f;
    float q = mn / mx, s = q * q;
    float t = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * q + q; /* atan(q), rad */
    if (ay > ax) t = 1.5707963f - t;
    float ang = t * 57.29578f;                 /* 0..90 from +x toward +y */
    /* screen coords: +y is down. Convert to clockwise-from-up. */
    float deg;
    if (dx >= 0 && dy < 0)       deg = 90.0f - ang;      /* upper right */
    else if (dx >= 0)            deg = 90.0f + ang;      /* lower right */
    else if (dy >= 0)            deg = 270.0f - ang;     /* lower left  */
    else                         deg = 270.0f + ang;     /* upper left  */
    return deg;
}

void gfx_arc(float cx, float cy, float r, float t, float a0, float a1, uint32_t c, int a) {
    if (a <= 0 || r <= 0.0f) return;
    while (a0 < 0) { a0 += 360.0f; a1 += 360.0f; }
    if (a1 - a0 >= 360.0f) { gfx_ring(cx, cy, r, t, c, a); return; }
    uint32_t a256 = gfx_a256(a > 255 ? 255 : a);
    float ri = r - t;
    int x0 = imax(ffloor(cx - r - 1.0f), cx0), x1 = imin(fceil(cx + r + 1.0f), cx1);
    int y0 = imax(ffloor(cy - r - 1.0f), cy0), y1 = imin(fceil(cy + r + 1.0f), cy1);
    for (int py = y0; py < y1; py++) {
        float dy = (float)py + 0.5f - cy;
        for (int px = x0; px < x1; px++) {
            float dx = (float)px + 0.5f - cx, d2 = dx * dx + dy * dy;
            if (d2 > (r + 1.0f) * (r + 1.0f) || (ri > 1.0f && d2 < (ri - 1.0f) * (ri - 1.0f))) continue;
            float ang = angle_cw(dx, dy);
            if (ang < a0) ang += 360.0f;
            if (ang > a1) continue;
            float d = fsq(d2);
            float cv = fclamp1(r - d + 0.5f) - fclamp1(ri - d + 0.5f);
            if (cv > 0.0f) px_a(px, py, c, cova((int)(cv * 255.0f + 0.5f), a256));
        }
    }
    /* round caps, so the ends are as smooth as the sides */
    float mid = r - t * 0.5f;
    for (int e = 0; e < 2; e++) {
        float deg = (e ? a1 : a0) * 0.017453293f;
        float ex = cx + mid * (float)sin(deg), ey = cy - mid * (float)cos(deg);
        gfx_circle(ex, ey, t * 0.5f, c, a);
    }
}

/* ===========================================================================
 *  Shadows
 *
 *  A box blurred by a Gaussian is separable: its alpha at (x, y) is the
 *  blurred profile across x times the blurred profile across y, each the
 *  difference of two Gaussian CDFs. So a shadow is two short tables, built
 *  per call from one cached CDF, and a multiply per pixel. The rounding of the
 *  object's corners is not carried into its shadow -- at the blur radii used
 *  here the difference is a few levels in a few pixels nobody can see.
 *
 *  This replaced a nine-slice of an erfc'd signed-distance field, which was
 *  correct and cost a function call and a clip test for every pixel of every
 *  corner tile on every damage rectangle, whether or not any of it was inside
 *  the rectangle -- and fell back to an exp() per pixel for anything shorter
 *  than about 200 px, which is to say the dock.
 * ===========================================================================*/
static float erfc_f(float x) {
    /* Abramowitz & Stegun 7.1.26 */
    float z = x < 0 ? -x : x;
    float t = 1.0f / (1.0f + 0.3275911f * z);
    float y = t * (0.254829592f + t * (-0.284496736f + t * (1.421413741f +
              t * (-1.453152027f + t * 1.061405429f))));
    float e = y * (float)exp(-(double)(z * z));
    return x >= 0 ? e : 2.0f - e;
}

/* The Gaussian CDF, sampled at pixel centres from -ext to +ext, 0..4096. */
typedef struct { int blur, ext; uint16_t *cdf; } cdf_t;
#define NCDF 8
static cdf_t cdfs[NCDF];
static int   cdf_next;

static const cdf_t *cdf_get(int blur) {
    for (int i = 0; i < NCDF; i++) if (cdfs[i].cdf && cdfs[i].blur == blur) return &cdfs[i];
    cdf_t *c = &cdfs[cdf_next];
    cdf_next = (cdf_next + 1) % NCDF;
    if (c->cdf) kfree(c->cdf);
    float sigma = (float)blur * 0.5f;
    if (sigma < 0.5f) sigma = 0.5f;
    int ext = fceil(sigma * 3.0f);
    c->cdf = (uint16_t *)kmalloc((uint64_t)(2 * ext + 1) * 2);
    if (!c->cdf) return 0;
    c->blur = blur; c->ext = ext;
    float k = 1.0f / (sigma * 1.41421356f);
    for (int i = 0; i <= 2 * ext; i++) {
        float d = (float)(i - ext) + 0.5f;           /* pixel centre relative to the edge */
        float v = 1.0f - 0.5f * erfc_f(d * k);        /* P(sample < d) */
        c->cdf[i] = (uint16_t)(fclamp1(v) * 4096.0f + 0.5f);
    }
    return c;
}
static inline int cdf_at(const cdf_t *c, int d) {      /* d = distance past the edge */
    if (d < -c->ext) return 0;
    if (d >= c->ext) return 4096;
    return c->cdf[d + c->ext];
}

static uint16_t shx[4096 + 512], shy[4096 + 512];

void gfx_shadow(int x, int y, int w, int h, int r, int blur, int dy, int spread, int opacity) {
    if (opacity <= 0 || w <= 0 || h <= 0) return;
    if (opacity > 255) opacity = 255;
    if (blur < 1) blur = 1;
    const cdf_t *c = cdf_get(blur);
    if (!c) return;
    int ext = c->ext;
    int sx = x - spread, sy = y + dy - spread, sw = w + 2 * spread, sh = h + 2 * spread;
    int ex0 = sx - ext, ey0 = sy - ext, ex1 = sx + sw + ext, ey1 = sy + sh + ext;
    int x0 = imax(ex0, cx0), x1 = imin(ex1, cx1), y0 = imax(ey0, cy0), y1 = imin(ey1, cy1);
    if (x1 <= x0 || y1 <= y0) return;
    if (x1 - ex0 > (int)(sizeof shx / 2) || y1 - ey0 > (int)(sizeof shy / 2)) return;
    /* profiles: inside the box along x, along y */
    for (int px = x0; px < x1; px++)
        shx[px - ex0] = (uint16_t)(cdf_at(c, px - sx) - cdf_at(c, px - (sx + sw)));
    for (int py = y0; py < y1; py++)
        shy[py - ey0] = (uint16_t)(cdf_at(c, py - sy) - cdf_at(c, py - (sy + sh)));
    int op = opacity + (opacity >> 7);                /* 0..256 */
    int hr = clamp_r(r, w, h);
    for (int py = y0; py < y1; py++) {
        uint32_t fy = shy[py - ey0];
        if (!fy) continue;
        /* the object's opaque part, which hides the shadow under it */
        int sk0 = x1, sk1 = x1;
        if (py >= y && py < y + h) {
            if (py >= y + hr && py < y + h - hr) { sk0 = x; sk1 = x + w; }
            else                                  { sk0 = x + hr; sk1 = x + w - hr; }
        }
        uint32_t *row = PX(0, py);
        uint32_t fyo = (fy * (uint32_t)op) >> 8;     /* 0..4096 */
        for (int px = x0; px < x1; px++) {
            if (px >= sk0 && px < sk1) { px = sk1 - 1; continue; }
            uint32_t a = (shx[px - ex0] * fyo) >> 16; /* 0..256 */
            if (!a) continue;
            uint32_t ia = 256 - a;
            uint32_t d = row[px];
            row[px] = ((((d & 0xFF00FFu) * ia) >> 8) & 0xFF00FFu) | ((((d & 0x00FF00u) * ia) >> 8) & 0x00FF00u);
        }
    }
}

/* ===========================================================================
 *  Images, masks, surfaces
 * ===========================================================================*/
/* premultiplied ARGB over the target, alpha scaled by a256 */
static inline void blend_pm(uint32_t *p, uint32_t s, uint32_t a256) {
    uint32_t sa = s >> 24;
    if (a256 < 256) {
        /* scale the whole premultiplied pixel */
        uint32_t rb = ((s & 0xFF00FFu) * a256 >> 8) & 0xFF00FFu;
        uint32_t g  = ((s & 0x00FF00u) * a256 >> 8) & 0x00FF00u;
        sa = (sa * a256) >> 8;
        s = rb | g;
    }
    if (!sa) return;
    uint32_t ia = 256 - (sa + (sa >> 7));
    uint32_t d = *p;
    uint32_t rb = (((d & 0xFF00FFu) * ia) >> 8) & 0xFF00FFu;
    uint32_t g  = (((d & 0x00FF00u) * ia) >> 8) & 0x00FF00u;
    uint32_t o_rb = rb + (s & 0xFF00FFu), o_g = g + (s & 0x00FF00u);
    if (o_rb & 0x01000100u) { if (o_rb & 0x01000000u) o_rb |= 0xFF0000u; if (o_rb & 0x100u) o_rb |= 0xFFu; }
    if (o_g & 0x10000u) o_g = 0xFF00u;
    *p = (o_rb & 0xFF00FFu) | (o_g & 0x00FF00u);
}

/* bilinear fetch of premultiplied ARGB, fx/fy in 0..255 */
static inline uint32_t bilerp4(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t fx, uint32_t fy) {
    uint32_t ifx = 256 - fx, ify = 256 - fy;
    uint32_t w00 = (ifx * ify) >> 8, w10 = (fx * ify) >> 8, w01 = (ifx * fy) >> 8, w11 = (fx * fy) >> 8;
    uint32_t rb = ((a & 0x00FF00FFu) * w00 + (b & 0x00FF00FFu) * w10 +
                   (c & 0x00FF00FFu) * w01 + (d & 0x00FF00FFu) * w11) >> 8;
    uint32_t ag = (((a >> 8) & 0x00FF00FFu) * w00 + ((b >> 8) & 0x00FF00FFu) * w10 +
                   ((c >> 8) & 0x00FF00FFu) * w01 + ((d >> 8) & 0x00FF00FFu) * w11);
    return (rb & 0x00FF00FFu) | (ag & 0xFF00FF00u);
}

static void image_impl(int x, int y, int w, int h, const uint32_t *src, int sw, int sh,
                       int stride, int r, int alpha);

void gfx_image(int x, int y, int w, int h, const uint32_t *src, int sw, int sh, int alpha) {
    image_impl(x, y, w, h, src, sw, sh, sw, 0, alpha);
}
void gfx_image_rr(int x, int y, int w, int h, const uint32_t *src, int sw, int sh, int r, int alpha) {
    image_impl(x, y, w, h, src, sw, sh, sw, r, alpha);
}
void gfx_image_sub(int x, int y, int w, int h, const uint32_t *src, int sw, int sh, int stride, int alpha) {
    image_impl(x, y, w, h, src, sw, sh, stride, 0, alpha);
}

static void image_impl(int x, int y, int w, int h, const uint32_t *src, int sw, int sh,
                       int stride, int r, int alpha) {
    if (!src || w <= 0 || h <= 0 || sw <= 0 || sh <= 0 || alpha <= 0) return;
    int x0 = imax(x, cx0), y0 = imax(y, cy0), x1 = imin(x + w, cx1), y1 = imin(y + h, cy1);
    if (x1 <= x0 || y1 <= y0) return;
    uint32_t a256 = gfx_a256(alpha > 255 ? 255 : alpha);
    r = clamp_r(r, w, h);
    const ctab_t *ct = r ? corner(r) : 0;
    if (r && !ct) r = 0;
    int same = (w == sw && h == sh);
    /* source step in 16.16 */
    int64_t stx = ((int64_t)sw << 16) / w, sty = ((int64_t)sh << 16) / h;
    for (int py = y0; py < y1; py++) {
        int j = py - y;
        int cl = 0, cr = 0; const uint8_t *cvrow = 0;
        if (r) {
            int crow = j < r ? j : (j >= h - r ? h - 1 - j : -1);
            if (crow >= 0) { cvrow = ct->cov + crow * r; cl = cr = r; }
        }
        uint32_t *dp = PX(0, py);
        if (same) {
            const uint32_t *sp = src + j * stride;
            for (int px = x0; px < x1; px++) {
                int i = px - x;
                uint32_t aa = a256;
                if (cvrow) {
                    if (i < cl)          aa = cova(cvrow[i], a256);
                    else if (i >= w - cr) aa = cova(cvrow[w - 1 - i], a256);
                }
                uint32_t s = sp[i];
                if (!(s >> 24)) continue;
                blend_pm(dp + px, s, aa);
            }
            continue;
        }
        int64_t vy = (int64_t)j * sty + (sty >> 1) - 32768;
        if (vy < 0) vy = 0;
        int sy0 = (int)(vy >> 16), fy = (int)((vy >> 8) & 0xFF);
        int sy1 = sy0 + 1 < sh ? sy0 + 1 : sh - 1;
        if (sy0 >= sh) sy0 = sh - 1;
        const uint32_t *r0 = src + sy0 * stride, *r1 = src + sy1 * stride;
        for (int px = x0; px < x1; px++) {
            int i = px - x;
            int64_t vx = (int64_t)i * stx + (stx >> 1) - 32768;
            if (vx < 0) vx = 0;
            int sx0 = (int)(vx >> 16), fx = (int)((vx >> 8) & 0xFF);
            if (sx0 >= sw) sx0 = sw - 1;
            int sx1 = sx0 + 1 < sw ? sx0 + 1 : sw - 1;
            uint32_t s = bilerp4(r0[sx0], r0[sx1], r1[sx0], r1[sx1], (uint32_t)fx, (uint32_t)fy);
            if (!(s >> 24)) continue;
            uint32_t aa = a256;
            if (cvrow) {
                if (i < cl)           aa = cova(cvrow[i], a256);
                else if (i >= w - cr) aa = cova(cvrow[w - 1 - i], a256);
            }
            blend_pm(dp + px, s, aa);
        }
    }
}

void gfx_mask(int x, int y, const uint8_t *m, int mw, int mh, int mstride, uint32_t c, int a) {
    if (!m || a <= 0) return;
    int x0 = imax(x, cx0), y0 = imax(y, cy0), x1 = imin(x + mw, cx1), y1 = imin(y + mh, cy1);
    if (x1 <= x0 || y1 <= y0) return;
    uint32_t a256 = gfx_a256(a > 255 ? 255 : a);
    uint32_t crb = c & 0xFF00FFu, cg = c & 0x00FF00u;
    for (int py = y0; py < y1; py++) {
        const uint8_t *mr = m + (py - y) * mstride;
        uint32_t *dp = PX(0, py);
        for (int px = x0; px < x1; px++) {
            int cv = mr[px - x];
            if (!cv) continue;
            uint32_t aa = (gfx_a256(cv) * a256) >> 8;
            uint32_t d = dp[px], ia = 256 - aa;
            uint32_t rb = (crb * aa + (d & 0xFF00FFu) * ia) >> 8;
            uint32_t g  = (cg  * aa + (d & 0x00FF00u) * ia) >> 8;
            dp[px] = (rb & 0xFF00FFu) | (g & 0x00FF00u);
        }
    }
}

void gfx_copy(int dx, int dy, const surface_t *s, int sx, int sy, int w, int h) {
    int x0 = imax(dx, cx0), y0 = imax(dy, cy0), x1 = imin(dx + w, cx1), y1 = imin(dy + h, cy1);
    if (x1 <= x0 || y1 <= y0) return;
    for (int py = y0; py < y1; py++)
        gfx_memcpy32(PX(x0, py), s->px + (sy + py - dy) * s->stride + sx + (x0 - dx), x1 - x0);
}

void gfx_copy_rr(int dx, int dy, const surface_t *s, int r, int a) {
    int w = s->w, h = s->h;
    if (a <= 0) return;
    int x0 = imax(dx, cx0), y0 = imax(dy, cy0), x1 = imin(dx + w, cx1), y1 = imin(dy + h, cy1);
    if (x1 <= x0 || y1 <= y0) return;
    r = clamp_r(r, w, h);
    const ctab_t *ct = r ? corner(r) : 0;
    if (r && !ct) r = 0;
    uint32_t a256 = gfx_a256(a > 255 ? 255 : a);
    for (int py = y0; py < y1; py++) {
        int j = py - dy;
        const uint32_t *sp = s->px + j * s->stride;
        uint32_t *dp = PX(0, py);
        int crow = r ? (j < r ? j : (j >= h - r ? h - 1 - j : -1)) : -1;
        int sl = x0, sr = x1;
        if (crow >= 0) {
            const uint8_t *cv = ct->cov + crow * r;
            for (int k = 0; k < r; k++) {
                int lx = dx + k, rx = dx + w - 1 - k;
                uint32_t aa = cova(cv[k], a256);
                if (!aa) continue;
                if (lx >= x0 && lx < x1) dp[lx] = aa >= 256 ? sp[k] : gfx_mix(dp[lx], sp[k], aa);
                if (rx >= x0 && rx < x1) dp[rx] = aa >= 256 ? sp[w - 1 - k] : gfx_mix(dp[rx], sp[w - 1 - k], aa);
            }
            sl = imax(x0, dx + r); sr = imin(x1, dx + w - r);
        }
        if (sr <= sl) continue;
        if (a256 >= 256) gfx_memcpy32(dp + sl, sp + (sl - dx), sr - sl);
        else for (int px = sl; px < sr; px++) dp[px] = gfx_mix(dp[px], sp[px - dx], a256);
    }
}

void gfx_surface_scaled(int x, int y, int w, int h, const surface_t *s, int r, int a) {
    if (!s || !s->px || w <= 0 || h <= 0 || a <= 0) return;
    if (w == s->w && h == s->h) { gfx_copy_rr(x, y, s, r, a); return; }
    int x0 = imax(x, cx0), y0 = imax(y, cy0), x1 = imin(x + w, cx1), y1 = imin(y + h, cy1);
    if (x1 <= x0 || y1 <= y0) return;
    uint32_t a256 = gfx_a256(a > 255 ? 255 : a);
    r = clamp_r(r, w, h);
    const ctab_t *ct = r ? corner(r) : 0;
    if (r && !ct) r = 0;
    int sw = s->w, sh = s->h;
    int64_t stx = ((int64_t)sw << 16) / w, sty = ((int64_t)sh << 16) / h;
    for (int py = y0; py < y1; py++) {
        int j = py - y;
        int64_t vy = (int64_t)j * sty + (sty >> 1) - 32768;
        if (vy < 0) vy = 0;
        int sy0 = (int)(vy >> 16); uint32_t fy = (uint32_t)((vy >> 8) & 0xFF);
        if (sy0 >= sh) sy0 = sh - 1;
        int sy1 = sy0 + 1 < sh ? sy0 + 1 : sh - 1;
        const uint32_t *r0 = s->px + sy0 * s->stride, *r1 = s->px + sy1 * s->stride;
        int crow = r ? (j < r ? j : (j >= h - r ? h - 1 - j : -1)) : -1;
        const uint8_t *cv = crow >= 0 ? ct->cov + crow * r : 0;
        uint32_t *dp = PX(0, py);
        for (int px = x0; px < x1; px++) {
            int i = px - x;
            int64_t vx = (int64_t)i * stx + (stx >> 1) - 32768;
            if (vx < 0) vx = 0;
            int sx0 = (int)(vx >> 16); uint32_t fx = (uint32_t)((vx >> 8) & 0xFF);
            if (sx0 >= sw) sx0 = sw - 1;
            int sx1 = sx0 + 1 < sw ? sx0 + 1 : sw - 1;
            uint32_t c = bilerp4(r0[sx0] | 0xFF000000u, r0[sx1] | 0xFF000000u,
                                 r1[sx0] | 0xFF000000u, r1[sx1] | 0xFF000000u, fx, fy) & 0xFFFFFFu;
            uint32_t aa = a256;
            if (cv) {
                if (i < r)           aa = cova(cv[i], a256);
                else if (i >= w - r) aa = cova(cv[w - 1 - i], a256);
            }
            if (!aa) continue;
            dp[px] = aa >= 256 ? c : gfx_mix(dp[px], c, aa);
        }
    }
}

/* Legacy: straight-alpha ARGB, nearest-neighbour. The pixel-art apps (DOOM,
 * the Game Boy) depend on staying nearest, so this keeps its old behaviour. */
void g_blit(int dx, int dy, int dw, int dh, const uint32_t *src, int sw, int sh) {
    if (!src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int x0 = imax(dx, cx0), y0 = imax(dy, cy0), x1 = imin(dx + dw, cx1), y1 = imin(dy + dh, cy1);
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y < y1; y++) {
        int sy = (y - dy) * sh / dh; if (sy >= sh) sy = sh - 1;
        const uint32_t *srow = src + sy * sw;
        uint32_t *dp = PX(0, y);
        if (dw == sw) {
            const uint32_t *sp = srow + (x0 - dx);
            int n = x1 - x0, allop = 1;
            for (int i = 0; i < n; i++) if ((sp[i] >> 24) != 255) { allop = 0; break; }
            if (allop) {
                for (int i = 0; i < n; i++) dp[x0 + i] = sp[i] & 0xFFFFFFu;
                continue;
            }
        }
        for (int x = x0; x < x1; x++) {
            int sx = (x - dx) * sw / dw; if (sx >= sw) sx = sw - 1;
            uint32_t c = srow[sx];
            uint32_t a = c >> 24;
            if (a == 255)    dp[x] = c & 0xFFFFFFu;
            else if (a)      dp[x] = gfx_mix(dp[x], c & 0xFFFFFFu, gfx_a256((int)a));
        }
    }
}

/* ===========================================================================
 *  Glass
 * ===========================================================================*/
/* One separable box pass over a w*h buffer, radius k, clamped at the edges.
 * Channels are summed in parallel: R and B share one word, G the other. */
static void box_pass(uint32_t *buf, uint32_t *tmp, int w, int h, int k, int horiz) {
    int n = horiz ? w : h, lines = horiz ? h : w;
    int step = horiz ? 1 : w, lstep = horiz ? w : 1;
    uint32_t div = (uint32_t)(2 * k + 1);
    uint32_t inv = (65536u + div / 2) / div;
    for (int l = 0; l < lines; l++) {
        uint32_t *line = buf + l * lstep;
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int i = -k; i <= k; i++) {
            int ii = i < 0 ? 0 : (i >= n ? n - 1 : i);
            uint32_t c = line[ii * step];
            sr += c >> 16 & 0xFF; sg += c >> 8 & 0xFF; sb += c & 0xFF;
        }
        for (int i = 0; i < n; i++) {
            tmp[i] = ((sr * inv >> 16) << 16) | ((sg * inv >> 16) << 8) | (sb * inv >> 16);
            int out = i - k, in = i + k + 1;
            if (out < 0) out = 0;
            if (in >= n) in = n - 1;
            uint32_t co = line[out * step], ci = line[in * step];
            sr += (ci >> 16 & 0xFF) - (co >> 16 & 0xFF);
            sg += (ci >> 8 & 0xFF) - (co >> 8 & 0xFF);
            sb += (ci & 0xFF) - (co & 0xFF);
        }
        for (int i = 0; i < n; i++) line[i * step] = tmp[i];
    }
}

static uint32_t tone(uint32_t c, const glass_t *g) {
    int r = c >> 16 & 0xFF, gg = c >> 8 & 0xFF, b = c & 0xFF;
    if (g->sat != 100) {
        int L = (r * 77 + gg * 150 + b * 29) >> 8;
        r = L + (r - L) * g->sat / 100; gg = L + (gg - L) * g->sat / 100; b = L + (b - L) * g->sat / 100;
        r = r < 0 ? 0 : r > 255 ? 255 : r; gg = gg < 0 ? 0 : gg > 255 ? 255 : gg; b = b < 0 ? 0 : b > 255 ? 255 : b;
    }
    if (g->tint_a > 0) {
        int ta = g->tint_a;
        r  = (r  * (255 - ta) + (int)(g->tint >> 16 & 0xFF) * ta) / 255;
        gg = (gg * (255 - ta) + (int)(g->tint >> 8 & 0xFF) * ta) / 255;
        b  = (b  * (255 - ta) + (int)(g->tint & 0xFF) * ta) / 255;
    }
    if (g->lift) {
        r += g->lift; gg += g->lift; b += g->lift;
        r = r < 0 ? 0 : r > 255 ? 255 : r; gg = gg < 0 ? 0 : gg > 255 ? 255 : gg; b = b < 0 ? 0 : b > 255 ? 255 : b;
    }
    return (uint32_t)(r << 16 | gg << 8 | b);
}

static uint32_t *scratch;
static int       scratch_cap;
static uint32_t *get_scratch(int n) {
    if (n > scratch_cap) {
        if (scratch) kfree(scratch);
        scratch = (uint32_t *)kmalloc((uint64_t)n * 4);
        scratch_cap = scratch ? n : 0;
    }
    return scratch;
}

/* Stretch the small image up over (x, y, w, h), separably: each column's
 * sample position is worked out once, a row is stretched horizontally only
 * when the source row changes, and each output pixel is then one mix of two
 * stretched rows. */
static void build_full(backdrop_t *b, int x, int y, int w, int h) {
    static int      cxi[4096];
    static uint8_t  cxf[4096];
    static uint32_t H0[4096], H1[4096];
    if (w <= 0 || h <= 0 || w > 4096) { b->fw = b->fh = 0; return; }
    if (w * h > b->fcap) {
        if (b->full) kfree(b->full);
        b->full = (uint32_t *)kmalloc((uint64_t)w * h * 4);
        b->fcap = b->full ? w * h : 0;
        if (!b->full) { b->fw = b->fh = 0; return; }
    }
    b->fx = x; b->fy = y; b->fw = w; b->fh = h;
    int fsh = b->f == 8 ? 3 : 2, sw = b->sw, sh = b->sh;
    for (int c = 0; c < w; c++) {
        int u = ((((x + c - b->x) << 8) + 128) >> fsh) - 128;
        if (u < 0) u = 0;
        int i = u >> 8, fr = u & 0xFF;
        if (i >= sw - 1) { i = sw - 1; fr = 0; }
        cxi[c] = i; cxf[c] = (uint8_t)fr;
    }
    int h0 = -1, h1 = -1;
    #define HROW(dst, j) do { const uint32_t *sr_ = b->small + (j) * sw; \
        for (int c_ = 0; c_ < w; c_++) { int i_ = cxi[c_]; \
            (dst)[c_] = cxf[c_] ? gfx_mix(sr_[i_], sr_[i_ + 1], cxf[c_]) : sr_[i_]; } } while (0)
    for (int r = 0; r < h; r++) {
        int v = ((((y + r - b->y) << 8) + 128) >> fsh) - 128;
        if (v < 0) v = 0;
        int j = v >> 8, fy = v & 0xFF;
        if (j >= sh - 1) { j = sh - 1; fy = 0; }
        int j1 = j + 1 < sh ? j + 1 : j;
        if (h0 != j) {
            if (h1 == j) { for (int c = 0; c < w; c++) H0[c] = H1[c]; }
            else HROW(H0, j);
            h0 = j;
        }
        uint32_t *out = b->full + r * w;
        if (!fy) { for (int c = 0; c < w; c++) out[c] = H0[c]; continue; }
        if (h1 != j1) { HROW(H1, j1); h1 = j1; }
        for (int c = 0; c < w; c++) out[c] = gfx_mix(H0[c], H1[c], (uint32_t)fy);
    }
    #undef HROW
}

void gfx_backdrop_capture(backdrop_t *b, int x, int y, int w, int h, const glass_t *g) {
    if (!T || w <= 0 || h <= 0) { b->valid = 0; return; }
    int f = g->blur > 40 ? 8 : 4;
    int m = g->blur + f;
    int rx = x - m, ry = y - m, rw = w + 2 * m, rh = h + 2 * m;
    int sw = (rw + f - 1) / f, sh = (rh + f - 1) / f;
    if (sw * sh > b->cap) {
        if (b->small) kfree(b->small);
        b->small = (uint32_t *)kmalloc((uint64_t)sw * sh * 4);
        b->cap = b->small ? sw * sh : 0;
        if (!b->small) { b->valid = 0; return; }
    }
    b->x = rx; b->y = ry; b->w = rw; b->h = rh; b->sw = sw; b->sh = sh; b->f = f;
    /* What we may read is the target's extent; outside it, the edge pixel
     * repeats. The clamping is done once, into a column map, and the sums
     * run over row pointers -- R and B packed in one word, G in another, so
     * a sample is a load, two ANDs and two adds. 64 samples of 255 is 14
     * bits, which leaves R (bits 16..29) and B (0..13) clear of each other. */
    int ax0 = OX, ay0 = OY, ax1 = OX + T->w - 1, ay1 = OY + T->h - 1;
    uint32_t shift = f == 8 ? 6 : 4;
    static int      xmap[4096 + 64];
    static uint32_t arb[1024 + 16], ag[1024 + 16];
    if (sw * f > (int)(sizeof xmap / sizeof xmap[0]) || sw > (int)(sizeof arb / sizeof arb[0])) { b->valid = 0; return; }
    for (int k = 0; k < sw * f; k++) {
        int xx = rx + k;
        xx = xx < ax0 ? ax0 : (xx > ax1 ? ax1 : xx);
        xmap[k] = xx - OX;
    }
    for (int j = 0; j < sh; j++) {
        for (int i = 0; i < sw; i++) { arb[i] = 0; ag[i] = 0; }
        for (int v = 0; v < f; v++) {
            int yy = ry + j * f + v;
            yy = yy < ay0 ? ay0 : (yy > ay1 ? ay1 : yy);
            const uint32_t *row = T->px + (yy - OY) * T->stride;
            const int *xm = xmap;
            for (int i = 0; i < sw; i++) {
                uint32_t rb = 0, gg = 0;
                for (int u = 0; u < f; u++) {
                    uint32_t c = row[*xm++];
                    rb += c & 0xFF00FFu; gg += c & 0x00FF00u;
                }
                arb[i] += rb; ag[i] += gg;
            }
        }
        uint32_t *out = b->small + j * sw;
        for (int i = 0; i < sw; i++) {
            uint32_t r = (arb[i] >> 16) >> shift, bb = (arb[i] & 0xFFFFu) >> shift, g = (ag[i] >> 8) >> shift;
            out[i] = (r << 16) | (g << 8) | bb;
        }
    }
    int k = (g->blur + f / 2) / f;
    if (k < 1) k = 1;
    uint32_t *tmp = get_scratch(sw > sh ? sw : sh);
    if (tmp) {
        for (int pass = 0; pass < 3; pass++) {
            box_pass(b->small, tmp, sw, sh, k, 1);
            box_pass(b->small, tmp, sw, sh, k, 0);
        }
    }
    for (int i = 0; i < sw * sh; i++) b->small[i] = tone(b->small[i], g);
    b->valid = 1;
    build_full(b, x, y, w, h);
}

void gfx_backdrop_free(backdrop_t *b) {
    if (b->small) kfree(b->small);
    if (b->full) kfree(b->full);
    b->small = 0; b->cap = 0; b->valid = 0;
    b->full = 0; b->fcap = 0; b->fw = b->fh = 0;
}

void gfx_backdrop_draw(const backdrop_t *b, int x, int y, int w, int h, int r, int a) {
    if (!b->valid || !b->small || w <= 0 || h <= 0 || a <= 0) return;
    int x0 = imax(x, cx0), y0 = imax(y, cy0), x1 = imin(x + w, cx1), y1 = imin(y + h, cy1);
    if (x1 <= x0 || y1 <= y0) return;
    r = clamp_r(r, w, h);
    const ctab_t *ct = r ? corner(r) : 0;
    if (r && !ct) r = 0;
    uint32_t a256 = gfx_a256(a > 255 ? 255 : a);
    /* the fast path: the full-size copy covers what is asked for */
    if (b->full && x0 >= b->fx && y0 >= b->fy && x1 <= b->fx + b->fw && y1 <= b->fy + b->fh) {
        for (int py = y0; py < y1; py++) {
            int j = py - y;
            const uint32_t *sp = b->full + (py - b->fy) * b->fw - b->fx;   /* index by px */
            uint32_t *dp = PX(0, py);
            int crow = r ? (j < r ? j : (j >= h - r ? h - 1 - j : -1)) : -1;
            int sl = x0, sr = x1;
            if (crow >= 0) {
                const uint8_t *cv = ct->cov + crow * r;
                for (int k = 0; k < r; k++) {
                    int lx = x + k, rx = x + w - 1 - k;
                    uint32_t aa = cova(cv[k], a256);
                    if (!aa) continue;
                    if (lx >= x0 && lx < x1) dp[lx] = aa >= 256 ? sp[lx] : gfx_mix(dp[lx], sp[lx], aa);
                    if (rx >= x0 && rx < x1) dp[rx] = aa >= 256 ? sp[rx] : gfx_mix(dp[rx], sp[rx], aa);
                }
                sl = imax(x0, x + r); sr = imin(x1, x + w - r);
            }
            if (sr <= sl) continue;
            if (a256 >= 256) gfx_memcpy32(dp + sl, sp + sl, sr - sl);
            else for (int px = sl; px < sr; px++) dp[px] = gfx_mix(dp[px], sp[px], a256);
        }
        return;
    }
    int f = b->f, sw = b->sw, sh = b->sh;
    uint32_t *rowbuf = get_scratch(sw + 1);
    if (!rowbuf) return;
    /* u = (x + 0.5 - b->x) / f - 0.5 in 8.8 fixed point */
    int fsh = f == 8 ? 3 : 2;                     /* f is 4 or 8: shift, not divide */
    for (int py = y0; py < y1; py++) {
        int j = py - y;
        int v256 = ((((py - b->y) << 8) + 128) >> fsh) - 128;
        if (v256 < 0) v256 = 0;
        int sy0 = v256 >> 8; uint32_t fy = (uint32_t)(v256 & 0xFF);
        if (sy0 >= sh - 1) { sy0 = sh - 1; fy = 0; }
        const uint32_t *r0 = b->small + sy0 * sw, *r1 = b->small + (sy0 + 1 < sh ? sy0 + 1 : sy0) * sw;
        for (int i = 0; i < sw; i++) rowbuf[i] = gfx_mix(r0[i], r1[i], fy);
        rowbuf[sw] = rowbuf[sw - 1];
        int crow = r ? (j < r ? j : (j >= h - r ? h - 1 - j : -1)) : -1;
        const uint8_t *cv = crow >= 0 ? ct->cov + crow * r : 0;
        uint32_t *dp = PX(0, py);
        int du = 256 >> fsh;
        int u256 = ((((x0 - b->x) << 8) + 128) >> fsh) - 128;
        for (int px = x0; px < x1; px++, u256 += du) {
            int uu = u256 < 0 ? 0 : u256;
            int sx0 = uu >> 8;
            if (sx0 >= sw) sx0 = sw - 1;
            uint32_t c = gfx_mix(rowbuf[sx0], rowbuf[sx0 + 1], (uint32_t)(uu & 0xFF));
            uint32_t aa = a256;
            int i = px - x;
            if (cv) {
                if (i < r)           aa = cova(cv[i], a256);
                else if (i >= w - r) aa = cova(cv[w - 1 - i], a256);
            }
            if (!aa) continue;
            dp[px] = aa >= 256 ? c : gfx_mix(dp[px], c, aa);
        }
    }
}

void gfx_blur_surface(const surface_t *src, surface_t *dst, const glass_t *g) {
    surface_t *ot = T; int oox = OX, ooy = OY;
    int s0 = bx0, s1 = by0, s2 = bx1, s3 = by1;
    backdrop_t b = {0};
    gfx_bind((surface_t *)src, 0, 0, 0, 0, src->w, src->h);
    gfx_backdrop_capture(&b, 0, 0, src->w, src->h, g);
    gfx_bind(dst, 0, 0, 0, 0, dst->w, dst->h);
    gfx_backdrop_draw(&b, 0, 0, dst->w, dst->h, 0, 255);
    gfx_backdrop_free(&b);
    if (ot) gfx_bind(ot, oox, ooy, s0, s1, s2 - s0, s3 - s1);
}
