/* ===========================================================================
 *  BoltOS TV  -  tv/video.c
 *
 *  The films: six seamless 1080p loops cut from 4K originals by
 *  tv/mkmedia.py, embedded in the kernel image, decoded here with pl_mpeg
 *  (vendor/pl_mpeg, MIT) -- an MPEG-1 decoder in one header, which is the
 *  whole reason the films are MPEG-1.
 *
 *  A player decodes on the shell's own thread, one frame per call at most,
 *  and only when the next frame is due. If the machine cannot keep up the
 *  film plays slower rather than the remote going dead while it catches up:
 *  on a television, a laggy menu is a broken television, and a scenery film
 *  at 21 fps instead of 24 is something nobody will ever notice.
 * ===========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include "tv.h"
#include "kheap.h"
#include "pmm.h"
#include "mm.h"
#include "image.h"
#include "string.h"
#include "kprintf.h"
#include "timeout.h"

#define PL_MPEG_IMPLEMENTATION
#define PLM_NO_STDIO
#include "pl_mpeg.h"

#define FILM(n) extern const uint8_t _binary_film_##n##_m1v_start[], _binary_film_##n##_m1v_end[], \
                                     _binary_film_##n##_jpg_start[], _binary_film_##n##_jpg_end[]
FILM(lofoten); FILM(lagoon); FILM(sunset); FILM(aurora); FILM(kauai); FILM(autumn);
#define SRC(n) _binary_film_##n##_m1v_start, _binary_film_##n##_m1v_end, \
               _binary_film_##n##_jpg_start, _binary_film_##n##_jpg_end

/* Filmed by the people named, published on Pexels under the Pexels licence. */
const film_t tv_films[] = {
    { "lofoten", "Lofoten Islands",  "Reine, Norway",          "Fjords",
      "Granite peaks and a fishing village at the edge of the Arctic.",
      "Cl\xE9ment Proust", 0x6F8FB0, SRC(lofoten) },
    { "lagoon",  "Turquoise Coast",  "The Aegean, from above", "Coast",
      "Clear water over pale stone, and one boat crossing it.",
      "Kenan Turgu\xE7",   0x2FB5B0, SRC(lagoon) },
    { "sunset",  "Sundown at Sea",   "Open water, at dusk",    "Golden Hour",
      "The sun going down into a calm sea, filmed low over the water.",
      "Oskar Gross",       0xE0803A, SRC(sunset) },
    { "aurora",  "Northern Lights",  "Finland, in winter",     "Aurora",
      "The aurora borealis moving over a forest at night.",
      "T Honkamies",       0x3FD08A, SRC(aurora) },
    { "kauai",   "Kauai Coastline",  "Hawaii",                 "Islands",
      "Surf breaking along the green cliffs of the Garden Isle.",
      "Logan Voss",        0x3A9BC8, SRC(kauai) },
    { "autumn",  "Autumn Forest",    "Fall colour, from above", "Autumn",
      "A forest at the turn of the season, at golden hour.",
      "AP Vibes",          0xE06A2A, SRC(autumn) },
};
const int tv_nfilms = (int)(sizeof tv_films / sizeof tv_films[0]);

/* ---- players ------------------------------------------------------------- */
int player_open(player_t *p, int film) {
    if (film < 0 || film >= tv_nfilms) return -1;
    if (p->v && p->film == film) return 0;
    if (p->v) { plm_video_destroy(p->v); p->v = 0; p->buf = 0; }
    const film_t *f = &tv_films[film];
    /* The buffer points straight at the bytes in the kernel image: nothing is
     * copied, and free_when_done is 0 because they were never allocated. */
    p->buf = plm_buffer_create_with_memory((uint8_t *)f->m1v, (size_t)(f->m1v_end - f->m1v), 0);
    if (!p->buf) return -1;
    p->v = plm_video_create_with_buffer(p->buf, 1);
    if (!p->v) { plm_buffer_destroy(p->buf); p->buf = 0; return -1; }
    plm_video_set_no_delay(p->v, 0);
    p->w = plm_video_get_width(p->v);
    p->h = plm_video_get_height(p->v);
    p->fps = plm_video_get_framerate(p->v);
    if (p->fps < 1.0 || p->fps > 120.0) p->fps = 24.0;
    if (p->w <= 0 || p->h <= 0 || p->w > 4096 || p->h > 2304) {
        kprintf("[tv] film %s: bad stream (%dx%d)\n", f->key, p->w, p->h);
        player_close(p);
        return -1;
    }
    uint32_t need = (uint32_t)(p->w * p->h);
    if (need > p->cap) {
        /* 8 MiB at 1080p: from the page allocator, not the heap, because two
         * of these plus pl_mpeg's own reference frames would take a third of
         * the kernel heap for something that never moves. */
        uint64_t pages = ((uint64_t)need * 4 + 4095) / 4096;
        uint64_t pa = pmm_alloc_contig(pages);
        if (!pa) { player_close(p); return -1; }
        p->rgb = (uint32_t *)P2V(pa);
        p->cap = (uint32_t)(pages * 4096 / 4);
    }
    p->film = film;
    p->have = 0;
    p->shown = 0;
    p->t0 = 0;
    return 0;
}

void player_close(player_t *p) {
    if (p->v) plm_video_destroy(p->v);       /* destroys the buffer too */
    p->v = 0; p->buf = 0; p->film = -1; p->have = 0;
    /* rgb is kept for the next film: the page allocator gets it back never,
     * which for two buffers over the life of a television is fine. */
}

static void to_rgb(plm_frame_t *fr, uint32_t *dst, int w) {
    plm_frame_to_bgra(fr, (uint8_t *)dst, w * 4);
}

int player_tick(player_t *p, uint64_t now) {
    if (!p->v) return 0;
    if (!p->t0) p->t0 = now;
    uint64_t due = (uint64_t)((double)(now - p->t0) * p->fps / 1000.0);
    if (p->have && p->shown > due) return 0;
    uint64_t t0 = now_us();
    plm_frame_t *fr = plm_video_decode(p->v);
    if (!fr) {                                 /* end of the loop: round again */
        plm_video_rewind(p->v);
        fr = plm_video_decode(p->v);
        if (!fr) return 0;
    }
    to_rgb(fr, p->rgb, p->w);
    p->decode_us = (uint32_t)(now_us() - t0);
    p->decoded++;
    p->have = 1;
    p->shown++;
    /* More than half a second behind: this machine is slower than the film.
     * Re-anchor instead of racing, so it plays smoothly at its own pace. */
    if (due > p->shown + (uint64_t)(p->fps / 2)) {
        p->t0 = now - (uint64_t)((double)p->shown * 1000.0 / p->fps);
    }
    return 1;
}

/* Copy (or crossfade) the frame over the canvas rectangle, scaled to cover
 * it. The common case -- a 1920x1080 frame onto a 1920x1080 canvas -- is a
 * row copy. */
void player_draw(const player_t *p, int x, int y, int w, int h, int alpha) {
    if (!p->have || alpha <= 0 || w <= 0 || h <= 0) return;
    int ox, oy;
    surface_t *t = gfx_target(&ox, &oy);
    if (!t) return;
    int x0 = x - ox, y0 = y - oy;
    uint32_t a = gfx_a256(alpha > 255 ? 255 : alpha);
    if (w == p->w && h == p->h) {
        for (int j = 0; j < h; j++) {
            int ty = y0 + j;
            if (ty < 0 || ty >= t->h) continue;
            uint32_t *d = t->px + (int64_t)ty * t->stride;
            const uint32_t *s = p->rgb + (int64_t)j * p->w;
            int xs = x0 < 0 ? -x0 : 0, xe = (x0 + w > t->w) ? t->w - x0 : w;
            if (xe <= xs) continue;
            if (a >= 256) gfx_memcpy32(d + x0 + xs, s + xs, xe - xs);
            else for (int i = xs; i < xe; i++) d[x0 + i] = gfx_mix(d[x0 + i], s[i], a);
        }
        return;
    }
    /* Scale to cover: crop whichever axis overflows, nearest-neighbour. A
     * film under a television's menus is not where anyone inspects pixels. */
    int64_t sw = p->w, sh = p->h;
    int64_t cw = sw, ch = sw * h / w;
    if (ch > sh) { ch = sh; cw = sh * w / h; }
    int64_t cx = (sw - cw) / 2, cy = (sh - ch) / 2;
    static int colmap[4096];
    int ww = w > 4096 ? 4096 : w;
    for (int i = 0; i < ww; i++) colmap[i] = (int)(cx + (int64_t)i * cw / w);
    for (int j = 0; j < h; j++) {
        int ty = y0 + j;
        if (ty < 0 || ty >= t->h) continue;
        const uint32_t *s = p->rgb + (cy + (int64_t)j * ch / h) * sw;
        uint32_t *d = t->px + (int64_t)ty * t->stride;
        for (int i = 0; i < ww; i++) {
            int tx = x0 + i;
            if (tx < 0 || tx >= t->w) continue;
            uint32_t c = s[colmap[i]];
            d[tx] = a >= 256 ? c : gfx_mix(d[tx], c, a);
        }
    }
}

uint32_t player_average(const player_t *p) {
    if (!p->have) return 0x203040;
    uint64_t r = 0, g = 0, b = 0, n = 0;
    for (int y = p->h / 16; y < p->h; y += p->h / 8)
        for (int x = p->w / 16; x < p->w; x += p->w / 8) {
            uint32_t c = p->rgb[y * p->w + x];
            r += (c >> 16) & 0xFF; g += (c >> 8) & 0xFF; b += c & 0xFF; n++;
        }
    if (!n) return 0x203040;
    return (uint32_t)(((r / n) << 16) | ((g / n) << 8) | (b / n));
}

/* ---- guide stills -------------------------------------------------------- */
static image_t *stills[8];
static int      still_tried[8];

const uint32_t *film_still(int film, int *w, int *h) {
    if (film < 0 || film >= tv_nfilms || film >= 8) return 0;
    if (!stills[film] && !still_tried[film]) {
        still_tried[film] = 1;
        const film_t *f = &tv_films[film];
        stills[film] = image_decode(f->jpg, (uint32_t)(f->jpg_end - f->jpg));
        if (stills[film])                       /* opaque: premultiplied as is */
            for (int i = 0; i < stills[film]->w * stills[film]->h; i++)
                stills[film]->px[i] |= 0xFF000000u;
    }
    if (!stills[film]) return 0;
    *w = stills[film]->w;
    *h = stills[film]->h;
    return stills[film]->px;
}
