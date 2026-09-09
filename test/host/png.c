/* Compile the kernel's PNG encoder on the host and write out a set of images
 * whose exact pixels are known. test/t_shot.py decodes each one with Python's
 * own zlib and asserts pixel equality.
 *
 * The point of doing it this way: an encoder that is only ever read back by
 * its own decoder can be wrong in matching pairs and nobody finds out. The
 * files these produce are checked by code that shares nothing with them. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
/* Not <string.h>: the encoder includes the kernel's, and the two declare the
 * same names with different dllimport attributes. The host libc still supplies
 * memcpy at link time, which is all png_enc.c needs. */

static void *kmalloc(uint64_t n) { return malloc((size_t)n); }
static void  kfree(void *p) { free(p); }

#include "../../kernel/png_enc.c"

static int emit(const char *path, const uint32_t *px, int w, int h) {
    uint8_t *out = 0; uint32_t len = 0;
    if (png_encode(px, w, h, &out, &len) != 0) {
        fprintf(stderr, "encode failed for %s\n", path);
        return 1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); free(out); return 1; }
    fwrite(out, 1, len, f);
    fclose(f);
    printf("%s %d %d %u\n", path, w, h, len);
    free(out);
    return 0;
}

/* A deterministic pseudo-random fill: the incompressible case, which is where
 * a deflate encoder that mis-sizes its output buffer falls over. */
static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: png OUTDIR\n"); return 2; }
    const char *dir = argv[1];
    char path[1024];
    int bad = 0;

    /* 1x1: the smallest legal image, and the one that catches off-by-ones in
     * the row filter and the final deflate block. */
    { uint32_t px = 0xFF3366CC;
      snprintf(path, sizeof path, "%s/one.png", dir);
      bad |= emit(path, &px, 1, 1); }

    /* Flat colour: everything is one long LZ77 match. */
    { int w = 320, h = 200;
      uint32_t *px = malloc((size_t)w * h * 4);
      for (int i = 0; i < w * h; i++) px[i] = 0xFF102030;
      snprintf(path, sizeof path, "%s/flat.png", dir);
      bad |= emit(path, px, w, h); free(px); }

    /* Horizontal gradient: the case the Sub row filter is chosen for. */
    { int w = 256, h = 64;
      uint32_t *px = malloc((size_t)w * h * 4);
      for (int y = 0; y < h; y++)
          for (int x = 0; x < w; x++)
              px[y * w + x] = 0xFF000000u | ((uint32_t)x << 16) | ((uint32_t)(y * 4) << 8) | (uint32_t)(255 - x);
      snprintf(path, sizeof path, "%s/gradient.png", dir);
      bad |= emit(path, px, w, h); free(px); }

    /* Noise: no matches at all, so the output is larger than the input and the
     * buffer has to grow. */
    { int w = 128, h = 128; uint32_t s = 12345;
      uint32_t *px = malloc((size_t)w * h * 4);
      for (int i = 0; i < w * h; i++) px[i] = 0xFF000000u | (lcg(&s) & 0xFFFFFF);
      snprintf(path, sizeof path, "%s/noise.png", dir);
      bad |= emit(path, px, w, h); free(px); }

    /* Long thin shapes: a row wider than the 32 KiB window, and a column of
     * single-pixel rows. Both have caught real bugs in encoders. */
    { int w = 20000, h = 1;
      uint32_t *px = malloc((size_t)w * h * 4);
      for (int i = 0; i < w * h; i++) px[i] = 0xFF000000u | (uint32_t)((i * 7) & 0xFFFF);
      snprintf(path, sizeof path, "%s/wide.png", dir);
      bad |= emit(path, px, w, h); free(px); }

    { int w = 1, h = 1000;
      uint32_t *px = malloc((size_t)w * h * 4);
      for (int i = 0; i < w * h; i++) px[i] = 0xFF000000u | (uint32_t)(i & 0xFF);
      snprintf(path, sizeof path, "%s/tall.png", dir);
      bad |= emit(path, px, w, h); free(px); }

    /* A stand-in for what this is actually for: flat panels, repeated glyph-
     * like runs, a gradient. Its compression ratio is asserted by the test. */
    { int w = 1024, h = 768;
      uint32_t *px = malloc((size_t)w * h * 4);
      for (int y = 0; y < h; y++)
          for (int x = 0; x < w; x++) {
              uint32_t c = 0xFF16161E;
              if (y > 700)                     c = 0xFF0E0E16;           /* taskbar */
              else if (x > 100 && x < 700 && y > 80 && y < 500) {
                  c = 0xFF1F1F2B;                                        /* a window */
                  if (((x / 8) % 9) == 0 && ((y / 16) % 3) == 0) c = 0xFFE0E0E0;  /* "text" */
              }
              px[y * w + x] = c;
          }
      snprintf(path, sizeof path, "%s/desktop.png", dir);
      bad |= emit(path, px, w, h); free(px); }

    /* Rejections: nothing should be allocated or written for these. */
    { uint8_t *o = (uint8_t *)1; uint32_t n = 1; uint32_t p = 0;
      if (png_encode(0,   1, 1, &o, &n) != -1) { fprintf(stderr, "null pixels accepted\n");  bad = 1; }
      if (png_encode(&p,  0, 1, &o, &n) != -1) { fprintf(stderr, "zero width accepted\n");   bad = 1; }
      if (png_encode(&p,  1, 0, &o, &n) != -1) { fprintf(stderr, "zero height accepted\n");  bad = 1; }
      if (png_encode(&p, -4, 4, &o, &n) != -1) { fprintf(stderr, "negative width accepted\n"); bad = 1; }
      if (png_encode(&p, 70000, 1, &o, &n) != -1) { fprintf(stderr, "absurd width accepted\n"); bad = 1; }
      if (png_encode(&p, 60000, 60000, &o, &n) != -1) { fprintf(stderr, "overflowing area accepted\n"); bad = 1; } }

    /* Known checksum vectors, so a broken CRC or Adler is caught here rather
     * than as "the file will not open". */
    if (png_crc32(0, (const uint8_t *)"123456789", 9) != 0xCBF43926u) {
        fprintf(stderr, "crc32 vector failed\n"); bad = 1;
    }
    if (png_adler32((const uint8_t *)"Wikipedia", 9) != 0x11E60398u) {
        fprintf(stderr, "adler32 vector failed\n"); bad = 1;
    }

    if (!bad) printf("host-ok\n");
    return bad;
}
