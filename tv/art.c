/* ===========================================================================
 *  BoltOS TV  -  tv/art.c
 *
 *  App tiles and UI glyphs, from the strips tv/mkart.py draws. Each strip is
 *  decoded once; a size that is asked for is then resampled from the nearest
 *  strip that is at least that big (by area-averaging, because a bilinear
 *  shrink of more than 2x aliases the thin strokes) and kept, so every draw
 *  after the first is a straight copy. A TV draws the same dozen sizes all
 *  evening, so the cache is small and never evicts.
 * ===========================================================================*/
#include <stdint.h>
#include "tv.h"
#include "image.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"

#define BLOB(n) extern const uint8_t _binary_##n##_start[], _binary_##n##_end[]
BLOB(art_tiles_96_png);
BLOB(art_tiles_192_png);
BLOB(art_glyphs_32_png);
BLOB(art_glyphs_64_png);

typedef struct { int size; uint32_t *px; int n; } strip_t;

static strip_t src_tiles[2], src_glyphs[2];           /* decoded, premultiplied */
static strip_t tile_cache[12], glyph_cache[16];
static uint8_t *glyph_mask_cache[16];
static int      glyph_mask_size[16];

static uint32_t premul(uint32_t c) {
    uint32_t a = c >> 24;
    if (a == 255) return c;
    if (a == 0) return 0;
    uint32_t r = ((c >> 16) & 0xFF) * a / 255, g = ((c >> 8) & 0xFF) * a / 255, b = (c & 0xFF) * a / 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static void load_strip(strip_t *s, const uint8_t *a, const uint8_t *b, int size, int n) {
    image_t *im = image_decode_upto(a, (uint32_t)(b - a), 4096);
    if (!im || im->h != size || im->w != size * n) {
        kprintf("[tv] art strip %dpx did not decode (%dx%d)\n", size, im ? im->w : 0, im ? im->h : 0);
        if (im) image_free(im);
        return;
    }
    s->size = size; s->n = n;
    s->px = (uint32_t *)kmalloc((uint64_t)size * size * n * 4);
    if (s->px) for (int i = 0; i < size * size * n; i++) s->px[i] = premul(im->px[i]);
    image_free(im);
}

void art_init(void) {
    load_strip(&src_tiles[0], _binary_art_tiles_96_png_start, _binary_art_tiles_96_png_end, 96, ART_TILES);
    load_strip(&src_tiles[1], _binary_art_tiles_192_png_start, _binary_art_tiles_192_png_end, 192, ART_TILES);
    load_strip(&src_glyphs[0], _binary_art_glyphs_32_png_start, _binary_art_glyphs_32_png_end, 32, GL_COUNT);
    load_strip(&src_glyphs[1], _binary_art_glyphs_64_png_start, _binary_art_glyphs_64_png_end, 64, GL_COUNT);
}

/* Resample cell by cell so one glyph never bleeds into its neighbour. */
static void resample_cell(const uint32_t *src, int sstride, int ss,
                          uint32_t *dst, int dstride, int ds) {
    if (ds <= ss) {
        /* area average: each destination pixel is the mean of the source box
         * it covers, boxes rounded to whole source pixels */
        for (int y = 0; y < ds; y++) {
            int y0 = y * ss / ds, y1 = (y + 1) * ss / ds;
            if (y1 <= y0) y1 = y0 + 1;
            for (int x = 0; x < ds; x++) {
                int x0 = x * ss / ds, x1 = (x + 1) * ss / ds;
                if (x1 <= x0) x1 = x0 + 1;
                uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
                for (int v = y0; v < y1; v++) {
                    const uint32_t *row = src + v * sstride;
                    for (int u = x0; u < x1; u++) {
                        uint32_t c = row[u];
                        a += c >> 24; r += (c >> 16) & 0xFF; g += (c >> 8) & 0xFF; b += c & 0xFF; n++;
                    }
                }
                dst[y * dstride + x] = ((a / n) << 24) | ((r / n) << 16) | ((g / n) << 8) | (b / n);
            }
        }
        return;
    }
    /* bilinear up */
    for (int y = 0; y < ds; y++) {
        int fy = ((y * 2 + 1) * ss * 128 / ds) - 128;           /* 8.8 */
        if (fy < 0) fy = 0;
        int sy = fy >> 8, wy = fy & 0xFF;
        int sy1 = sy + 1 < ss ? sy + 1 : sy;
        for (int x = 0; x < ds; x++) {
            int fx = ((x * 2 + 1) * ss * 128 / ds) - 128;
            if (fx < 0) fx = 0;
            int sx = fx >> 8, wx = fx & 0xFF;
            int sx1 = sx + 1 < ss ? sx + 1 : sx;
            uint32_t c00 = src[sy * sstride + sx], c01 = src[sy * sstride + sx1];
            uint32_t c10 = src[sy1 * sstride + sx], c11 = src[sy1 * sstride + sx1];
            uint32_t out = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                int a = (c00 >> sh) & 0xFF, b = (c01 >> sh) & 0xFF;
                int c = (c10 >> sh) & 0xFF, d = (c11 >> sh) & 0xFF;
                int top = a * (256 - wx) + b * wx, bot = c * (256 - wx) + d * wx;
                out |= (uint32_t)(((top * (256 - wy) + bot * wy) >> 16) & 0xFF) << sh;
            }
            dst[y * dstride + x] = out;
        }
    }
}

static strip_t *sized(strip_t *cache, int ncache, strip_t *src2, int size) {
    for (int i = 0; i < ncache; i++) if (cache[i].px && cache[i].size == size) return &cache[i];
    const strip_t *s = (src2[0].px && size <= src2[0].size) ? &src2[0] : &src2[1];
    if (!s->px) s = &src2[0];
    if (!s->px) return 0;
    int slot = -1;
    for (int i = 0; i < ncache; i++) if (!cache[i].px) { slot = i; break; }
    if (slot < 0) slot = ncache - 1;                       /* full: reuse the last */
    if (cache[slot].px) kfree(cache[slot].px);
    uint32_t *px = (uint32_t *)kmalloc((uint64_t)size * size * s->n * 4);
    if (!px) return 0;
    for (int k = 0; k < s->n; k++)
        resample_cell(s->px + k * s->size, s->size * s->n, s->size, px + k * size, size * s->n, size);
    cache[slot].px = px; cache[slot].size = size; cache[slot].n = s->n;
    return &cache[slot];
}

void art_tile(int id, int x, int y, int size, int alpha) {
    if (id < 0 || id >= ART_TILES || size <= 0) return;
    strip_t *s = sized(tile_cache, 12, src_tiles, size);
    if (!s) return;
    gfx_image_sub(x, y, size, size, s->px + id * size, size, size, size * s->n, alpha);
}

void art_glyph(int id, int cx, int cy, int size, uint32_t color, int alpha) {
    if (id < 0 || id >= GL_COUNT || size <= 0) return;
    strip_t *s = sized(glyph_cache, 16, src_glyphs, size);
    if (!s) return;
    int slot = (int)(s - glyph_cache);
    if (!glyph_mask_cache[slot] || glyph_mask_size[slot] != size) {
        if (glyph_mask_cache[slot]) kfree(glyph_mask_cache[slot]);
        uint8_t *m = (uint8_t *)kmalloc((uint64_t)size * size * s->n);
        if (!m) return;
        for (int i = 0; i < size * size * s->n; i++) m[i] = (uint8_t)(s->px[i] >> 24);
        glyph_mask_cache[slot] = m;
        glyph_mask_size[slot] = size;
    }
    gfx_mask(cx - size / 2, cy - size / 2, glyph_mask_cache[slot] + id * size,
             size, size, size * s->n, color, alpha);
}
