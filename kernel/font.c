/* ===========================================================================
 *  BoltOS  -  kernel/font.c
 *
 *  Text. Outlines come from the .bfnt blobs mkuifont.py builds out of Inter
 *  and DejaVu Sans Mono; each glyph is rasterised the first time it is asked
 *  for at a given size and kept in a cache keyed on face, size, subpixel
 *  phase and colour polarity.
 *
 *  The rasteriser accumulates the exact signed area each edge covers in each
 *  pixel and then integrates across the row -- the font-rs / stb_truetype v2
 *  method. It is the reason small text looks like text and not like the 4x4
 *  supersampled smear kernel/ttf.c produces: every pixel gets the true
 *  fraction of it the glyph covers, not one of seventeen levels.
 *
 *  The legacy entry points the apps use (g_text, g_char and friends) live
 *  here too. g_text is proportional Inter now; g_char stays monospace,
 *  because the terminal and the editor lay text out on a grid of 8px cells
 *  and want every glyph to fill one.
 * ===========================================================================*/
#include <stdint.h>
#include "font.h"
#include "gfx.h"
#include "gui.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"
#include "math.h"

static inline float fsqrt(float x) { float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }

/* ---- the blobs build.sh embeds -------------------------------------------- */
#define BLOB(n) extern const uint8_t _binary_##n##_bfnt_start[], _binary_##n##_bfnt_end[];
BLOB(font_r) BLOB(font_m) BLOB(font_sb) BLOB(font_b) BLOB(font_d) BLOB(font_mono) BLOB(font_monob)
#undef BLOB

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline int16_t  rds16(const uint8_t *p) { return (int16_t)rd16(p); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

typedef struct {
    const uint8_t *base;
    uint32_t       size;
    int            upm, asc, desc, gap, cap, xh;
    int            nglyph, nkern, mono;
    const uint8_t *gt, *pb, *kt;
    int16_t        slot[256];       /* byte -> glyph record, -1 if absent */
    uint16_t       kfirst[257];     /* kern entries for left byte b: [kfirst[b], kfirst[b+1]) */
    int            ok;
} face_t;

static face_t faces[FONT_FACES];
static int    fonts_ok;

static void face_load(face_t *f, const uint8_t *s, const uint8_t *e) {
    memset(f, 0, sizeof(*f));
    for (int i = 0; i < 256; i++) f->slot[i] = -1;
    uint32_t n = (uint32_t)(e - s);
    if (n < 40 || s[0] != 'B' || s[1] != 'F' || s[2] != 'N' || s[3] != 'T') return;
    if (rd16(s + 4) != 1) return;
    f->base = s; f->size = n;
    f->upm = rd16(s + 6);
    f->asc = rds16(s + 8); f->desc = rds16(s + 10); f->gap = rds16(s + 12);
    f->cap = rds16(s + 14); f->xh = rds16(s + 16);
    f->nglyph = rd16(s + 18); f->nkern = rd16(s + 20); f->mono = rd16(s + 22) & 1;
    uint32_t gt = rd32(s + 24), pb = rd32(s + 28), kt = rd32(s + 32), tot = rd32(s + 36);
    if (!f->upm || tot > n || gt + 20u * f->nglyph > n || kt + 4u * f->nkern > n || pb > n) return;
    f->gt = s + gt; f->pb = s + pb; f->kt = s + kt;
    for (int i = 0; i < f->nglyph; i++) f->slot[f->gt[i * 20]] = (int16_t)i;
    /* kern index: entries are sorted by (left, right) */
    int k = 0;
    for (int b = 0; b < 256; b++) {
        f->kfirst[b] = (uint16_t)k;
        while (k < f->nkern && f->kt[k * 4] == b) k++;
    }
    f->kfirst[256] = (uint16_t)k;
    f->ok = 1;
}

int font_init(void) {
    face_load(&faces[FONT_REGULAR],   _binary_font_r_bfnt_start,     _binary_font_r_bfnt_end);
    face_load(&faces[FONT_MEDIUM],    _binary_font_m_bfnt_start,     _binary_font_m_bfnt_end);
    face_load(&faces[FONT_SEMIBOLD],  _binary_font_sb_bfnt_start,    _binary_font_sb_bfnt_end);
    face_load(&faces[FONT_BOLD],      _binary_font_b_bfnt_start,     _binary_font_b_bfnt_end);
    face_load(&faces[FONT_DISPLAY],   _binary_font_d_bfnt_start,     _binary_font_d_bfnt_end);
    face_load(&faces[FONT_MONO],      _binary_font_mono_bfnt_start,  _binary_font_mono_bfnt_end);
    face_load(&faces[FONT_MONO_BOLD], _binary_font_monob_bfnt_start, _binary_font_monob_bfnt_end);
    /* A face that failed to load borrows a sibling, so a missing weight is a
     * slightly wrong weight rather than no text at all. */
    for (int i = 0; i < FONT_FACES; i++) {
        if (faces[i].ok) continue;
        int alt = (i == FONT_MONO_BOLD) ? FONT_MONO : FONT_REGULAR;
        if (faces[alt].ok) faces[i] = faces[alt];
    }
    fonts_ok = faces[FONT_REGULAR].ok;
    if (fonts_ok)
        kprintf("[ok] UI fonts: Inter (%d glyphs, %d kern pairs), %s\n",
                faces[FONT_REGULAR].nglyph, faces[FONT_REGULAR].nkern,
                faces[FONT_MONO].ok ? "DejaVu Sans Mono" : "no monospace face");
    else
        kprintf("[!!] UI fonts missing; the desktop falls back to its bitmap face\n");
    return fonts_ok ? 0 : -1;
}
int font_ready(void) { return fonts_ok; }

static inline face_t *F(int face) {
    if (face < 0 || face >= FONT_FACES) face = FONT_REGULAR;
    return &faces[face];
}

/* kerning in font units, 0 if the pair has none */
static int kern(const face_t *f, unsigned char l, unsigned char r) {
    int lo = f->kfirst[l], hi = f->kfirst[l + 1] - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        unsigned char rr = f->kt[mid * 4 + 1];
        if (rr == r) return rds16(f->kt + mid * 4 + 2);
        if (rr < r) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static inline const uint8_t *grec(const face_t *f, unsigned char c) {
    int s = f->slot[c];
    if (s < 0) s = f->slot['?'];
    return s < 0 ? 0 : f->gt + s * 20;
}

/* advance of c in 1/64 px at size q (quarter pixels) */
static inline int adv64(const face_t *f, const uint8_t *g, int q) {
    return (int)(((int64_t)rd16(g + 2) * q * 16 + f->upm / 2) / f->upm);
}

/* ===========================================================================
 *  Rasteriser
 * ===========================================================================*/
static float *acc;
static int    acc_cap;

static void acc_line(float x0, float y0, float x1, float y1, int w, int h, int stride) {
    if (y0 == y1) return;
    float dir;
    if (y0 < y1) dir = 1.0f;
    else { dir = -1.0f; float t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    if (y0 < 0.0f) x -= y0 * dxdy;
    int ys = y0 < 0.0f ? 0 : (int)y0;
    int ye = (int)y1; if ((float)ye < y1) ye++;
    if (ye > h) ye = h;
    for (int y = ys; y < ye; y++) {
        float *row = acc + y * stride;
        float yt = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        float yb = (float)y > y0 ? (float)y : y0;
        float dy = yt - yb;
        float xnext = x + dxdy * dy;
        float d = dy * dir;
        float xa = x < xnext ? x : xnext, xb = x < xnext ? xnext : x;
        if (xa < 0.0f) xa = 0.0f;
        if (xb < 0.0f) xb = 0.0f;
        if (xa > (float)w) xa = (float)w;
        if (xb > (float)w) xb = (float)w;
        int x0i = (int)xa;
        float x0f = (float)x0i;
        int x1i = (int)xb; if ((float)x1i < xb) x1i++;
        if (x1i <= x0i + 1) {
            float xmf = 0.5f * (xa + xb) - x0f;
            row[x0i] += d - d * xmf;
            row[x0i + 1] += d * xmf;
        } else {
            float s = 1.0f / (xb - xa);
            float x0frac = xa - x0f;
            float a0 = 0.5f * s * (1.0f - x0frac) * (1.0f - x0frac);
            float x1frac = xb - (float)x1i + 1.0f;
            float am = 0.5f * s * x1frac * x1frac;
            row[x0i] += d * a0;
            if (x1i == x0i + 2) {
                row[x0i + 1] += d * (1.0f - a0 - am);
            } else {
                float a1 = s * (1.5f - x0frac);
                row[x0i + 1] += d * (a1 - a0);
                for (int xi = x0i + 2; xi < x1i - 1; xi++) row[xi] += d * s;
                float a2 = a1 + (float)(x1i - x0i - 3) * s;
                row[x1i - 1] += d * (1.0f - a2 - am);
            }
            row[x1i] += d * am;
        }
        x = xnext;
    }
}

/* Quadratic bezier, flattened to within ~0.2 px. */
static void acc_quad(float x0, float y0, float cx, float cy, float x1, float y1, int w, int h, int stride) {
    float ddx = x0 - 2.0f * cx + x1, ddy = y0 - 2.0f * cy + y1;
    float dd2 = ddx * ddx + ddy * ddy;
    int n = 1;
    while (n < 16 && (float)(n * n) * 1.6f < (dd2 > 0 ? fsqrt(dd2) : 0.0f)) n++;
    float px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, mt = 1.0f - t;
        float x = mt * mt * x0 + 2.0f * mt * t * cx + t * t * x1;
        float y = mt * mt * y0 + 2.0f * mt * t * cy + t * t * y1;
        acc_line(px, py, x, y, w, h, stride);
        px = x; py = y;
    }
}

/* ---- the cache ------------------------------------------------------------ */
typedef struct {
    uint32_t key;               /* 0 = empty */
    int16_t  ox, oy;            /* bitmap top-left relative to pen x / baseline */
    uint16_t w, h;
    uint8_t *bm;
} gent_t;

#define GC_SLOTS 4096
#define GC_ARENA (2u * 1024u * 1024u)
static gent_t   gc[GC_SLOTS];
static uint8_t *arena;
static uint32_t arena_used;
static int      gc_used;

static void gc_flush(void) {
    memset(gc, 0, sizeof(gc));
    arena_used = 0;
    gc_used = 0;
}

static uint8_t *arena_alloc(uint32_t n) {
    if (!arena) {
        arena = (uint8_t *)kmalloc(GC_ARENA);
        if (!arena) return 0;
    }
    n = (n + 7) & ~7u;
    if (arena_used + n > GC_ARENA) return 0;
    uint8_t *p = arena + arena_used;
    arena_used += n;
    return p;
}

/* Two contrast curves. Blending in gamma space makes light-on-dark text look
 * thinner than the same glyph dark-on-light; lifting the partial coverage of
 * light text by a power curve evens the two out. */
static uint8_t lut_light[256], lut_dark[256];
static int     luts_ok;
static void build_luts(void) {
    for (int i = 0; i < 256; i++) {
        float v = (float)i / 255.0f;
        lut_light[i] = (uint8_t)(pow((double)v, 0.78) * 255.0 + 0.5);
        lut_dark[i]  = (uint8_t)(pow((double)v, 1.04) * 255.0 + 0.5);
    }
    luts_ok = 1;
}

static uint32_t mkkey(int face, int q, int phase, int italic, int light, unsigned char c) {
    return 1u | (uint32_t)c << 1 | (uint32_t)(light & 1) << 9 | (uint32_t)(italic & 1) << 10 |
           (uint32_t)(phase & 3) << 11 | (uint32_t)(q & 0x3FF) << 13 | (uint32_t)(face & 7) << 23;
}

static const gent_t *glyph(int face, int q, int phase, int italic, int light, unsigned char c) {
    uint32_t key = mkkey(face, q, phase, italic, light, c);
    uint32_t h = (key * 2654435761u) >> 20;            /* 12 bits */
    for (int probe = 0; probe < 32; probe++) {
        gent_t *e = &gc[(h + probe) & (GC_SLOTS - 1)];
        if (e->key == key) return e;
        if (!e->key) break;
    }
    if (gc_used > GC_SLOTS * 3 / 4) gc_flush();
    face_t *f = F(face);
    const uint8_t *g = grec(f, c);
    if (!g) return 0;
    if (!luts_ok) build_luts();

    float scale = (float)q / (4.0f * (float)f->upm);
    float ph = (float)phase * 0.25f;
    float sl = italic ? 0.2f : 0.0f;
    int xmin = rds16(g + 4), ymin = rds16(g + 6), xmax = rds16(g + 8), ymax = rds16(g + 10);
    int nc = rd16(g + 12), np = rd16(g + 14);
    gent_t ent = {0};
    ent.key = key;
    if (nc && np) {
        float fx0 = (float)xmin * scale + ph + (sl > 0 ? (float)ymin * scale * sl : 0);
        float fx1 = (float)xmax * scale + ph + (sl > 0 ? (float)ymax * scale * sl : 0);
        float fy0 = -(float)ymax * scale, fy1 = -(float)ymin * scale;
        int bx0 = (int)fx0 - (fx0 < 0 ? 1 : 0), bx1 = (int)fx1 + 1;
        int by0 = (int)fy0 - (fy0 < 0 ? 1 : 0), by1 = (int)fy1 + 1;
        int w = bx1 - bx0 + 1, hgt = by1 - by0 + 1;
        if (w > 0 && hgt > 0 && w < 1024 && hgt < 1024) {
            int stride = w + 2;
            int need = stride * hgt + 4;
            if (need > acc_cap) {
                if (acc) kfree(acc);
                acc = (float *)kmalloc((uint64_t)need * sizeof(float));
                acc_cap = acc ? need : 0;
            }
            uint8_t *bm = acc ? arena_alloc((uint32_t)(w * hgt)) : 0;
            if (!bm) { gc_flush(); bm = acc ? arena_alloc((uint32_t)(w * hgt)) : 0; }
            if (bm) {
                memset(acc, 0, (size_t)need * sizeof(float));
                const uint8_t *pts = f->pb + rd32(g + 16);
                const uint8_t *ends = pts;
                const uint8_t *xy = pts + 2 * nc;
                const uint8_t *bits = xy + 4 * np;
                int start = 0;
                for (int ci = 0; ci < nc; ci++) {
                    int end = rd16(ends + 2 * ci);
                    int n = end - start + 1;
                    if (n >= 2) {
                        #define PTX(i) ((float)rds16(xy + 4 * (i)) * scale + ph - (float)bx0 + sl * (float)rds16(xy + 4 * (i) + 2) * scale)
                        #define PTY(i) (-(float)rds16(xy + 4 * (i) + 2) * scale - (float)by0)
                        #define ON(i)  ((bits[(i) >> 3] >> ((i) & 7)) & 1)
                        int first = -1;
                        for (int k = 0; k < n; k++) if (ON(start + k)) { first = start + k; break; }
                        float sx, sy;
                        if (first < 0) {
                            sx = 0.5f * (PTX(start) + PTX(start + 1));
                            sy = 0.5f * (PTY(start) + PTY(start + 1));
                            first = start;
                        } else { sx = PTX(first); sy = PTY(first); }
                        float cxp = sx, cyp = sy, qx = 0, qy = 0;
                        int have = 0;
                        for (int k = 1; k <= n; k++) {
                            int i = start + ((first - start + k) % n);
                            float px = PTX(i), py = PTY(i);
                            if (ON(i)) {
                                if (have) { acc_quad(cxp, cyp, qx, qy, px, py, w, hgt, stride); have = 0; }
                                else acc_line(cxp, cyp, px, py, w, hgt, stride);
                                cxp = px; cyp = py;
                            } else {
                                if (have) {
                                    float mx = 0.5f * (qx + px), my = 0.5f * (qy + py);
                                    acc_quad(cxp, cyp, qx, qy, mx, my, w, hgt, stride);
                                    cxp = mx; cyp = my;
                                }
                                qx = px; qy = py; have = 1;
                            }
                        }
                        if (have) acc_quad(cxp, cyp, qx, qy, sx, sy, w, hgt, stride);
                        else      acc_line(cxp, cyp, sx, sy, w, hgt, stride);
                        #undef PTX
                        #undef PTY
                        #undef ON
                    }
                    start = end + 1;
                }
                const uint8_t *lut = light ? lut_light : lut_dark;
                for (int y = 0; y < hgt; y++) {
                    float a = 0.0f;
                    const float *row = acc + y * stride;
                    for (int x = 0; x < w; x++) {
                        a += row[x];
                        float v = a < 0 ? -a : a;
                        if (v > 1.0f) v = 1.0f;
                        bm[y * w + x] = lut[(int)(v * 255.0f + 0.5f)];
                    }
                }
                ent.bm = bm; ent.w = (uint16_t)w; ent.h = (uint16_t)hgt;
                ent.ox = (int16_t)bx0; ent.oy = (int16_t)by0;
            }
        }
    }
    for (int probe = 0; probe < 32; probe++) {
        gent_t *e = &gc[(h + probe) & (GC_SLOTS - 1)];
        if (!e->key) { *e = ent; gc_used++; return e; }
    }
    gc_flush();
    gc[h & (GC_SLOTS - 1)] = ent;
    gc_used = 1;
    return &gc[h & (GC_SLOTS - 1)];
}

/* ===========================================================================
 *  Drawing and measuring
 * ===========================================================================*/
static int draw_q(int face, int q, int x, int y, const char *s, int len, uint32_t color, int alpha, int italic) {
    face_t *f = F(face);
    if (!f->ok || !s) return 0;
    if (len < 0) len = (int)strlen(s);
    int light = gfx_luma(color) > 140;
    int pen = x * 64;
    unsigned char prev = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        const uint8_t *g = grec(f, c);
        if (!g) continue;
        if (prev && f->nkern) pen += (int)(((int64_t)kern(f, prev, c) * q * 16) / f->upm);
        int sub = ((pen & 63) + 8) >> 4;       /* quarter-pixel phase */
        int px = pen >> 6;
        if (sub == 4) { sub = 0; px++; }
        const gent_t *e = glyph(face, q, sub, italic, light, c);
        if (e && e->bm) gfx_mask(px + e->ox, y + e->oy, e->bm, e->w, e->h, e->w, color, alpha);
        pen += adv64(f, g, q);
        prev = c;
    }
    return (pen - x * 64 + 63) >> 6;
}

static int width_q(int face, int q, const char *s, int len) {
    face_t *f = F(face);
    if (!f->ok || !s) return 0;
    if (len < 0) len = (int)strlen(s);
    int pen = 0;
    unsigned char prev = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        const uint8_t *g = grec(f, c);
        if (!g) continue;
        if (prev && f->nkern) pen += (int)(((int64_t)kern(f, prev, c) * q * 16) / f->upm);
        pen += adv64(f, g, q);
        prev = c;
    }
    return (pen + 63) >> 6;
}

int font_draw(int face, int px, int x, int y, const char *s, int len, uint32_t color, int alpha) {
    return draw_q(face, px * 4, x, y, s, len, color, alpha, 0);
}
int font_draw_italic(int face, int px, int x, int y, const char *s, int len, uint32_t color, int alpha) {
    return draw_q(face, px * 4, x, y, s, len, color, alpha, 1);
}
int font_width(int face, int px, const char *s, int len) { return width_q(face, px * 4, s, len); }

int font_fit(int face, int px, const char *s, int len, int max_w) {
    face_t *f = F(face);
    if (!f->ok || !s) return 0;
    if (len < 0) len = (int)strlen(s);
    int q = px * 4, pen = 0, lim = max_w * 64;
    unsigned char prev = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        const uint8_t *g = grec(f, c);
        if (!g) continue;
        int k = (prev && f->nkern) ? (int)(((int64_t)kern(f, prev, c) * q * 16) / f->upm) : 0;
        int a = adv64(f, g, q);
        if (pen + k + a > lim) return i;
        pen += k + a;
        prev = c;
    }
    return len;
}

int font_draw_fit(int face, int px, int x, int y, const char *s, int max_w, uint32_t color, int alpha) {
    if (!s) return 0;
    int len = (int)strlen(s);
    if (font_width(face, px, s, len) <= max_w) return font_draw(face, px, x, y, s, len, color, alpha);
    int ew = font_width(face, px, TXT_ELLIPSIS, 1);
    int n = font_fit(face, px, s, len, max_w - ew);
    while (n > 0 && s[n - 1] == ' ') n--;
    int w = font_draw(face, px, x, y, s, n, color, alpha);
    return w + font_draw(face, px, x + w, y, TXT_ELLIPSIS, 1, color, alpha);
}

int font_ascent(int face, int px)   { face_t *f = F(face); return f->ok ? (f->asc * px + f->upm - 1) / f->upm : px; }
int font_descent(int face, int px)  { face_t *f = F(face); return f->ok ? (-f->desc * px + f->upm - 1) / f->upm : px / 4; }
int font_capheight(int face, int px){ face_t *f = F(face); return f->ok ? (f->cap * px + f->upm / 2) / f->upm : px * 7 / 10; }
int font_has(int face, unsigned char c) { face_t *f = F(face); return f->ok && f->slot[c] >= 0; }

/* baseline that centres the cap height on cy */
static inline int base_c(int cy, int face, int px) { return cy + (font_capheight(face, px) + 1) / 2; }

int text_l(int x, int cy, const char *s, int face, int px, uint32_t color) {
    return font_draw(face, px, x, base_c(cy, face, px), s, -1, color, 255);
}
int text_c(int cx, int cy, const char *s, int face, int px, uint32_t color) {
    int w = font_width(face, px, s, -1);
    font_draw(face, px, cx - w / 2, base_c(cy, face, px), s, -1, color, 255);
    return w;
}
int text_r(int rx, int cy, const char *s, int face, int px, uint32_t color) {
    int w = font_width(face, px, s, -1);
    font_draw(face, px, rx - w, base_c(cy, face, px), s, -1, color, 255);
    return w;
}
int text_fit_l(int x, int cy, const char *s, int max_w, int face, int px, uint32_t color) {
    return font_draw_fit(face, px, x, base_c(cy, face, px), s, max_w, color, 255);
}

int text_wrap(int x, int top, int w, const char *s, int face, int px, int line_h,
              int max_lines, uint32_t color) {
    if (!s) return 0;
    int lines = 0, base = top + font_ascent(face, px);
    while (*s && lines < max_lines) {
        while (*s == ' ') s++;
        int len = (int)strlen(s);
        int nl = 0; while (nl < len && s[nl] != '\n') nl++;
        int n = font_fit(face, px, s, nl, w);
        if (n < nl) {                          /* break at the last space */
            int b = n;
            while (b > 0 && s[b] != ' ') b--;
            if (b > 0) n = b;
        }
        if (n <= 0) n = 1;
        if (lines == max_lines - 1 && n < nl) {
            char tmp[256];
            int m = nl < 255 ? nl : 255;
            memcpy(tmp, s, (size_t)m); tmp[m] = 0;
            font_draw_fit(face, px, x, base, tmp, w, color, 255);
        } else {
            font_draw(face, px, x, base, s, n, color, 255);
        }
        lines++;
        base += line_h;
        s += n;
        if (*s == '\n') s++;
    }
    return lines;
}

/* ===========================================================================
 *  The legacy text API (include/gui.h). Every app draws with these, and they
 *  keep their old geometry: y is the top of a 16 px line (x scale), and the
 *  baseline sits 12 px below it, where the 8x16 bitmap face had it.
 * ===========================================================================*/
extern const unsigned char font8x8_basic[128][8];
extern const unsigned char font8x8_latin1[128][8];

static int g_face = FONT_ARIAL;

void g_set_font(int face) {
    int f = (face == FONT_ARIAL) ? FONT_ARIAL : FONT_RETRO;
    if (f == g_face) return;
    g_face = f;
    gui_request_redraw();
}
int g_get_font(void) { return g_face; }
int g_font_height(int scale) { return (g_face == FONT_RETRO ? 8 : 16) * scale; }
/* Two pixels under the baseline. The browser had 8 * scale + 1 written in,
 * which is right for the 8x8 face and straight through the x-height of
 * every other one -- links came out struck through. */
int g_underline_y(int scale) { return g_face == FONT_RETRO ? 8 * scale + 1 : 12 * scale + 2; }

static int legacy_retro(void) { return g_face == FONT_RETRO || !fonts_ok; }

static const unsigned char *retro_rows(unsigned char u) {
    if (u < 128) return font8x8_basic[u];
    return font8x8_latin1[u - 0x80];
}

static void retro_char(int x, int y, unsigned char c, uint32_t color, int s) {
    const unsigned char *g = retro_rows(c);
    for (int ry = 0; ry < 8; ry++)
        for (int rx = 0; rx < 8; rx++)
            if (g[ry] & (1u << rx)) gfx_fill(x + rx * s, y + ry * s, s, s, color);
}

/* UI size and weight for a legacy scale: 1 is body text, anything bigger was
 * always a heading. */
static int lg_px(int scale)   { return scale <= 1 ? 13 : scale == 2 ? 24 : 12 * scale; }
static int lg_face(int scale) { return scale <= 1 ? FONT_REGULAR : FONT_SEMIBOLD; }

void g_char(int x, int y, char ch, uint32_t color, int s) {
    if (s < 1) s = 1;
    unsigned char c = (unsigned char)ch;
    if (legacy_retro()) { retro_char(x, y, c, color, s); return; }
    if (c == ' ') return;
    /* 53 quarter-pixels: DejaVu Sans Mono's advance comes out at 8 px, so a
     * glyph fills its terminal cell exactly. */
    draw_q(FONT_MONO, 53 * s, x, y + 12 * s, (const char *)&c, 1, color, 255, 0);
}

void g_text(int x, int y, const char *str, uint32_t color, int s) {
    if (!str) return;
    if (s < 1) s = 1;
    if (legacy_retro()) {
        for (; *str; str++) { retro_char(x, y, (unsigned char)*str, color, s); x += 8 * s; }
        return;
    }
    font_draw(lg_face(s), lg_px(s), x, y + 12 * s, str, -1, color, 255);
}

void g_text_retro(int x, int y, const char *s, uint32_t color, int scale) {
    if (!s) return;
    if (scale < 1) scale = 1;
    for (; *s; s++) { retro_char(x, y, (unsigned char)*s, color, scale); x += 8 * scale; }
}

int g_text_width(const char *s, int scale) {
    if (!s) return 0;
    if (scale < 1) scale = 1;
    if (legacy_retro()) return (int)strlen(s) * 8 * scale;
    return font_width(lg_face(scale), lg_px(scale), s, -1);
}

int g_glyph_ink(char ch) {
    unsigned char c = (unsigned char)ch;
    if (legacy_retro()) {
        const unsigned char *g = retro_rows(c);
        for (int i = 0; i < 8; i++) if (g[i]) return 1;
        return 0;
    }
    face_t *f = F(FONT_REGULAR);
    if (f->slot[c] < 0) return 0;
    return rd16(f->gt + f->slot[c] * 20 + 12) != 0;
}

/* Proportional text for the browser: one glyph at a time, whole-pixel
 * advances, an optional slant for italics. */
int g_glyph_adv(char c, int scale) {
    if (scale < 1) scale = 1;
    if (legacy_retro()) return 8 * scale;
    face_t *f = F(FONT_REGULAR);
    const uint8_t *g = grec(f, (unsigned char)c);
    if (!g) return 4 * scale;
    return (adv64(f, g, lg_px(scale) * 4) + 32) >> 6;
}
void g_char_p(int x, int y, char ch, uint32_t color, int s, int italic) {
    if (s < 1) s = 1;
    unsigned char c = (unsigned char)ch;
    if (legacy_retro()) { retro_char(x, y, c, color, s); return; }
    draw_q(FONT_REGULAR, lg_px(s) * 4, x, y + 12 * s, (const char *)&c, 1, color, 255, italic);
}
int g_text_width_pn(const char *s, int len, int scale) {
    int w = 0;
    for (int i = 0; i < len; i++) w += g_glyph_adv(s[i], scale);
    return w;
}
int g_text_pn(int x, int y, const char *s, int len, uint32_t color, int scale, int italic) {
    for (int i = 0; i < len; i++) { g_char_p(x, y, s[i], color, scale, italic); x += g_glyph_adv(s[i], scale); }
    return x;
}

/* The old TrueType helpers the sign-in screen used. y is the text top. */
void g_text_tt(int x, int y_top, const char *s, uint32_t color, int px) {
    font_draw(FONT_REGULAR, px, x, y_top + font_ascent(FONT_REGULAR, px), s, -1, color, 255);
}
int g_text_tt_width(const char *s, int px) { return font_width(FONT_REGULAR, px, s, -1); }
