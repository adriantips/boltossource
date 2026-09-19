/* Fuzzing kernel/image.c, which is the largest thing in this kernel that a web
 * page gets to choose the bytes of.
 *
 * app_browser.c hands whatever came back from an <img> straight to
 * image_decode(), in ring 0, and behind that one call sit five decoders and an
 * inflate: PNG with its own DEFLATE, baseline JPEG with Huffman tables and an
 * IDCT, GIF with LZW, and BMP and TGA. The wallpaper path goes through the same
 * code with the ceilings lifted, from a file the user chose, which is a lower
 * bar for the attacker and a higher one for us.
 *
 * The five decoders are targeted directly rather than through image_decode(),
 * because the dispatch is by magic bytes: a mutation that flips the first byte
 * of a PNG would otherwise send the input to the TGA fallback and quietly stop
 * fuzzing the thing the target is named after. `dispatch` covers the chooser
 * itself, and `wall` covers image_decode_upto's ceiling swap -- it saves and
 * restores two file-scope variables around a decode that can fail in the
 * middle, which is a shape worth a target of its own.
 *
 * Seeds are test/seeds/img: sixteen small pictures that between them reach
 * every colour type PNG has and every depth it packs, both chroma layouts
 * JPEG has, the LZW table, the RLE run decoder, the interlaced row map and
 * the palette-with-transparency path. */
#include "fuzz.h"
#include "image.h"

/* Included, not linked: the five decoders are static, and a target that goes
 * through the magic-byte dispatch is not fuzzing the decoder it claims to. */
#include "../../kernel/image.c"

/* ---- corpus ------------------------------------------------------------- */

#define MAXSEED (1u << 16)

typedef struct { const char *name; int which; } seedspec;

enum { S_PNG, S_JPEG, S_GIF, S_BMP, S_TGA, S_KINDS };

static fz_seed by_kind[S_KINDS][8];
static int     n_by_kind[S_KINDS];
static fz_seed all_seeds[20];
static int     n_all;
/* Parallel to seedlist, so a check can name the seed it means. */
static fz_seed seed_at[20];
static int     seed_ok[20];

static const seedspec seedlist[] = {
    { "test/seeds/img/png-rgb8.png",        S_PNG  },
    { "test/seeds/img/png-rgba8.png",       S_PNG  },
    { "test/seeds/img/png-palette.png",     S_PNG  },
    { "test/seeds/img/png-gray8.png",       S_PNG  },
    { "test/seeds/img/png-palette4.png",    S_PNG  },   /* 4 bits a pixel */
    { "test/seeds/img/png-bilevel1.png",    S_PNG  },   /* 1 bit a pixel  */
    { "test/seeds/img/jpeg-420.jpg",        S_JPEG },
    { "test/seeds/img/jpeg-444.jpg",        S_JPEG },
    { "test/seeds/img/jpeg-gray.jpg",       S_JPEG },
    { "test/seeds/img/gif-89a.gif",         S_GIF  },
    { "test/seeds/img/gif-transparent.gif", S_GIF  },
    { "test/seeds/img/bmp-24.bmp",          S_BMP  },
    { "test/seeds/img/bmp-32.bmp",          S_BMP  },
    { "test/seeds/img/tga-24.tga",          S_TGA  },
    { "test/seeds/img/tga-rle.tga",         S_TGA  },
    { "test/seeds/img/tga-gray.tga",        S_TGA  },
};

static void load_corpus(void) {
    for (unsigned i = 0; i < sizeof seedlist / sizeof *seedlist; i++) {
        FILE *f = fopen(seedlist[i].name, "rb");
        if (!f) continue;
        uint8_t *buf = (uint8_t *)malloc(MAXSEED);
        size_t n = fread(buf, 1, MAXSEED, f);
        fclose(f);
        if (!n) { free(buf); continue; }
        int k = seedlist[i].which;
        by_kind[k][n_by_kind[k]].d = buf;
        by_kind[k][n_by_kind[k]].n = (uint32_t)n;
        n_by_kind[k]++;
        all_seeds[n_all].d = buf;
        all_seeds[n_all].n = (uint32_t)n;
        n_all++;
        seed_at[i].d = buf;
        seed_at[i].n = (uint32_t)n;
        seed_ok[i] = 1;
    }
}

/* ---- targets ------------------------------------------------------------ */

static void t_png(const uint8_t *d, uint32_t n)  { image_free(decode_png(d, n)); }
static void t_jpeg(const uint8_t *d, uint32_t n) { image_free(decode_jpeg(d, n)); }
static void t_gif(const uint8_t *d, uint32_t n)  { image_free(decode_gif(d, n)); }
static void t_bmp(const uint8_t *d, uint32_t n)  { image_free(decode_bmp(d, n)); }
static void t_tga(const uint8_t *d, uint32_t n)  { image_free(decode_tga(d, n)); }

/* The chooser, and the pixels afterwards. Reading every pixel back is not
 * ceremony: a decoder that returns a w and an h it did not fill leaves the
 * caller to touch memory the guarded heap can then report, and the browser
 * paints every one of these. */
static void t_dispatch(const uint8_t *d, uint32_t n) {
    image_t *im = image_decode(d, n);
    if (!im) return;
    uint32_t sum = 0;
    if (im->px)
        for (int i = 0; i < im->w * im->h; i++) sum += im->px[i];
    (void)sum;
    image_free(im);
}

/* The wallpaper's ceiling: a taller limit, and two file-scope variables saved
 * and put back around a decode that is free to fail anywhere in the middle. */
static void t_wall(const uint8_t *d, uint32_t n) {
    int fit = n ? 640 + (int)d[n - 1] * 8 : 1024;
    image_free(image_decode_upto(d, n, fit));
}

/* ---- selftest ----------------------------------------------------------- */

static uint32_t at(const image_t *im, int x, int y) { return im->px[y * im->w + x]; }

static int near8(uint32_t px, uint8_t r, uint8_t g, uint8_t b, int tol) {
    int dr = (int)((px >> 16) & 0xFF) - r;
    int dg = (int)((px >> 8) & 0xFF) - g;
    int db = (int)(px & 0xFF) - b;
    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    return dr <= tol && dg <= tol && db <= tol;
}

int fz_selftest(void) {
    fz_check_failures = 0;

    FZ_CHECK(n_all == 16);               /* test/seeds/img is the corpus */
    for (int k = 0; k < S_KINDS; k++) FZ_CHECK(n_by_kind[k] >= 2);

    /* Every seed decodes, at the size it was written, through the same
     * dispatch the browser uses. A corpus entry that stopped decoding is a
     * target that has quietly stopped being fuzzed. */
    for (int i = 0; i < n_all; i++) {
        image_t *im = image_decode(all_seeds[i].d, all_seeds[i].n);
        FZ_CHECK(im != NULL);
        if (!im) continue;
        FZ_CHECK(im->w == 64 && im->h == 48 && im->px != NULL);
        if (im->px) FZ_CHECK((at(im, 0, 0) >> 24) == 0xFF);   /* opaque background */
        image_free(im);
    }

    /* And that the pixels are the ones that went in. The first stroke of the
     * drawing starts at the top-left corner, so pixel (0,0) is #DC5028 in every
     * seed that carries colour; PNG, BMP and TGA have to hand it back exactly
     * and JPEG only nearly, which is why both are checked -- a decoder that
     * returned a plausible grey for everything would pass a "did it decode"
     * test all day. The indices are positions in seedlist above: png-rgb8,
     * bmp-24, tga-24, jpeg-444. */
    struct { int idx; int tol; } shades[] = { { 0, 0 }, { 11, 0 }, { 13, 0 }, { 7, 24 } };
    for (unsigned e = 0; e < sizeof shades / sizeof *shades; e++) {
        int i = shades[e].idx;
        if (!seed_ok[i]) { FZ_CHECK(seed_ok[i]); continue; }
        image_t *im = image_decode(seed_at[i].d, seed_at[i].n);
        FZ_CHECK(im && im->px && near8(at(im, 0, 0), 220, 80, 40, shades[e].tol));
        image_free(im);
    }

    /* The two ceilings differ, and image_decode_upto has to put back what it
     * borrowed even when the decode fails. */
    uint32_t px_before = img_max_px;
    int      dim_before = img_fit_dim;
    image_free(image_decode_upto((const uint8_t *)"\x89PNGnonsense", 12, 2048));
    FZ_CHECK(img_max_px == px_before && img_fit_dim == dim_before);

    return fz_check_failures;
}

/* ---- the table ---------------------------------------------------------- */

static fz_target targets[] = {
    { "png",      t_png,      by_kind[S_PNG],  0, 1 },
    { "jpeg",     t_jpeg,     by_kind[S_JPEG], 0, 1 },
    { "gif",      t_gif,      by_kind[S_GIF],  0, 1 },
    { "bmp",      t_bmp,      by_kind[S_BMP],  0, 1 },
    { "tga",      t_tga,      by_kind[S_TGA],  0, 1 },
    { "dispatch", t_dispatch, all_seeds,       0, 1 },
    { "wall",     t_wall,     all_seeds,       0, 1 },
};

int main(int argc, char **argv) {
    load_corpus();
    for (unsigned i = 0; i < sizeof targets / sizeof *targets; i++) {
        int k;
        for (k = 0; k < S_KINDS; k++)
            if (targets[i].corpus == by_kind[k]) { targets[i].ncorpus = n_by_kind[k]; break; }
        if (k == S_KINDS) targets[i].ncorpus = n_all;
    }
    return fz_main(argc, argv, targets, (int)(sizeof targets / sizeof *targets));
}
