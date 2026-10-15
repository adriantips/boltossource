/* ===========================================================================
 *  BoltOS  -  kernel/icons.c
 *
 *  The atlases mkicons.py renders, embedded as PNGs and decoded on first
 *  use. App icons are premultiplied once after decoding so scaling them
 *  bilinearly cannot drag a dark fringe in from the transparent pixels
 *  around them -- the classic halo you get from interpolating straight
 *  alpha, and the first thing you see on a light wallpaper.
 * ===========================================================================*/
#include <stdint.h>
#include "icons.h"
#include "gui.h"
#include "gfx.h"
#include "font.h"
#include "image.h"
#include "kheap.h"
#include "hw.h"
#include "string.h"
#include "kprintf.h"

#define PNG(n) extern const uint8_t _binary_##n##_png_start[], _binary_##n##_png_end[];
PNG(apps_16) PNG(apps_20) PNG(apps_24) PNG(apps_32) PNG(apps_40) PNG(apps_48) PNG(apps_64) PNG(apps_96)
PNG(glyphs_16) PNG(glyphs_20) PNG(glyphs_24) PNG(cursors_32)
#undef PNG

typedef struct {
    int            size;
    const uint8_t *png, *end;
    uint32_t      *px;          /* apps: premultiplied ARGB          */
    uint8_t       *mask;        /* glyphs: alpha only                */
    int            w, h;
    int            failed;
} atlas_t;

#define A(n, s) { s, _binary_##n##_png_start, _binary_##n##_png_end, 0, 0, 0, 0, 0 }
static atlas_t apps[] = {
    A(apps_16, 16), A(apps_20, 20), A(apps_24, 24), A(apps_32, 32), A(apps_40, 40), A(apps_48, 48),
    A(apps_64, 64), A(apps_96, 96)
};
static atlas_t glyphs[] = { A(glyphs_16, 16), A(glyphs_20, 20), A(glyphs_24, 24) };
static atlas_t cursors = A(cursors_32, 32);
#undef A
#define NAPP   ((int)(sizeof(apps) / sizeof(apps[0])))
#define NGLYPH ((int)(sizeof(glyphs) / sizeof(glyphs[0])))

static image_t *decode(atlas_t *a) {
    uint32_t len = (uint32_t)(a->end - a->png);
    if (len < 8) return 0;
    return image_decode_upto(a->png, len, 1024);
}

static int load_app(atlas_t *a) {
    if (a->px) return 1;
    if (a->failed) return 0;
    image_t *im = decode(a);
    if (!im || !im->px) { a->failed = 1; if (im) image_free(im); kprintf("[icons] apps-%d.png did not decode\n", a->size); return 0; }
    /* premultiply in place and keep the decoder's buffer */
    int n = im->w * im->h;
    for (int i = 0; i < n; i++) {
        uint32_t c = im->px[i], al = c >> 24;
        if (al == 255) continue;
        if (!al) { im->px[i] = 0; continue; }
        uint32_t r = ((c >> 16 & 0xFF) * al + 127) / 255, g = ((c >> 8 & 0xFF) * al + 127) / 255,
                 b = ((c & 0xFF) * al + 127) / 255;
        im->px[i] = al << 24 | r << 16 | g << 8 | b;
    }
    a->px = im->px; a->w = im->w; a->h = im->h;
    im->px = 0;
    image_free(im);
    return 1;
}

static int load_glyph(atlas_t *a) {
    if (a->mask) return 1;
    if (a->failed) return 0;
    image_t *im = decode(a);
    if (!im || !im->px) { a->failed = 1; if (im) image_free(im); return 0; }
    int n = im->w * im->h;
    a->mask = (uint8_t *)kmalloc((uint64_t)n);
    if (!a->mask) { a->failed = 1; image_free(im); return 0; }
    for (int i = 0; i < n; i++) a->mask[i] = (uint8_t)(im->px[i] >> 24);
    a->w = im->w; a->h = im->h;
    image_free(im);
    return 1;
}

/* the smallest atlas at least `size`, else the biggest */
static int pick(const atlas_t *set, int n, int size) {
    for (int i = 0; i < n; i++) if (set[i].size >= size) return i;
    return n - 1;
}

static void calendar_face(int x, int y, int size, int alpha) {
    if (size < 24 || alpha < 40) return;
    static const char *MON[13] = { "", "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                   "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    struct rtc_time t; rtc_now(&t);
    int mo = (t.mon >= 1 && t.mon <= 12) ? t.mon : 1;
    char day[4]; int d = t.day ? t.day : 1, k = 0;
    if (d >= 10) day[k++] = (char)('0' + d / 10);
    day[k++] = (char)('0' + d % 10); day[k] = 0;
    /* the band is rows 8..42 of the 128-unit drawing; the page below it */
    int band_cy = y + size * 25 / 128;
    int mpx = size * 15 / 128; if (mpx < 7) mpx = 7;
    int dpx = size * 50 / 128;
    int cx = x + size / 2;
    int mw = font_width(FONT_BOLD, mpx, MON[mo], -1);
    font_draw(FONT_BOLD, mpx, cx - mw / 2, band_cy + font_capheight(FONT_BOLD, mpx) / 2, MON[mo], -1, 0xFFFFFF, alpha);
    int dw = font_width(FONT_REGULAR, dpx, day, -1);
    int base = y + size * 99 / 128;
    font_draw(size >= 48 ? FONT_DISPLAY : FONT_REGULAR, dpx, cx - dw / 2, base, day, -1, 0x1C1C1E, alpha);
}

void icon_draw(int id, int x, int y, int size, int alpha) {
    if (id <= ICON_NONE || id >= ICON_COUNT || size <= 0 || alpha <= 0) return;
    if (!gfx_clip_hits(x, y, size, size)) return;
    int ai = pick(apps, NAPP, size);
    atlas_t *a = &apps[ai];
    if (!load_app(a)) {
        /* no atlas: a plain tile, so the shell still has something to click */
        gfx_rrect(x + size / 16, y + size / 16, size - size / 8, size - size / 8, size / 5, 0x6B7280, alpha);
        return;
    }
    int s = a->size;
    int col = id % ICON_COLS, row = id / ICON_COLS;
    const uint32_t *src = a->px + row * s * a->w + col * s;
    gfx_image_sub(x, y, size, size, src, s, s, a->w, alpha);
    if (id == ICON_CALENDAR) calendar_face(x, y, size, alpha);
}

void icon_draw_c(int id, int cx, int cy, int size, int alpha) {
    icon_draw(id, cx - size / 2, cy - size / 2, size, alpha);
}

/* A mask scaled to an odd size: bilinear on the alpha, into a small buffer. */
static uint8_t gbuf[48 * 48];

void glyph_draw(int id, int x, int y, int size, uint32_t color, int alpha) {
    if (id < 0 || id >= GLYPH_COUNT || size <= 0 || alpha <= 0) return;
    if (!gfx_clip_hits(x, y, size, size)) return;
    int gi = pick(glyphs, NGLYPH, size);
    atlas_t *a = &glyphs[gi];
    if (!load_glyph(a)) return;
    int s = a->size;
    int col = id % GLYPH_COLS, row = id / GLYPH_COLS;
    const uint8_t *src = a->mask + row * s * a->w + col * s;
    if (size == s) { gfx_mask(x, y, src, s, s, a->w, color, alpha); return; }
    if (size > 48) size = 48;
    for (int j = 0; j < size; j++) {
        int vy = ((j * 2 + 1) * s * 128) / size - 128;      /* (j+.5)*s/size - .5 in 8.8 */
        if (vy < 0) vy = 0;
        int sy = vy >> 8, fy = vy & 0xFF;
        int sy1 = sy + 1 < s ? sy + 1 : s - 1;
        for (int i = 0; i < size; i++) {
            int vx = ((i * 2 + 1) * s * 128) / size - 128;
            if (vx < 0) vx = 0;
            int sx = vx >> 8, fx = vx & 0xFF;
            int sx1 = sx + 1 < s ? sx + 1 : s - 1;
            int a00 = src[sy * a->w + sx], a10 = src[sy * a->w + sx1];
            int a01 = src[sy1 * a->w + sx], a11 = src[sy1 * a->w + sx1];
            int top = a00 + ((a10 - a00) * fx >> 8), bot = a01 + ((a11 - a01) * fx >> 8);
            gbuf[j * size + i] = (uint8_t)(top + ((bot - top) * fy >> 8));
        }
    }
    gfx_mask(x, y, gbuf, size, size, size, color, alpha);
}

void glyph_draw_c(int id, int cx, int cy, int size, uint32_t color, int alpha) {
    glyph_draw(id, cx - size / 2, cy - size / 2, size, color, alpha);
}

int cursor_image(int kind, const uint32_t **px, int *stride) {
    if (kind < 0 || kind >= CURSOR_COUNT || !load_app(&cursors)) return 0;
    *px = cursors.px + kind * 32;
    *stride = cursors.w;
    return 1;
}

void icons_preload(void) {
    load_app(&cursors);
    load_app(&apps[0]);       /* 16: title bars, menus      */
    load_app(&apps[4]);       /* 40: the dock and Start     */
    load_app(&apps[5]);       /* 48: desktop icons, Files   */
    load_glyph(&glyphs[0]);
    load_glyph(&glyphs[1]);
}

/* The old entry point: a 16*scale square, colour ignored now that icons
 * carry their own. */
void gui_icon(int id, int x, int y, int scale, uint32_t color) {
    (void)color;
    if (scale < 1) scale = 1;
    icon_draw(id, x, y, 16 * scale, 255);
}
