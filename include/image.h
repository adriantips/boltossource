#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS image decoder. Turns an in-memory PNG / JPEG (baseline) / GIF / BMP
 *  / TGA byte stream into a 32-bit ARGB pixel buffer. Everything is integer-only (the
 *  kernel is built with no FPU), and decoded images are box-downscaled so a
 *  single image never pins more than a few hundred KiB of the kheap -- unless
 *  the caller asks otherwise via image_decode_upto().
 * ===========================================================================*/

typedef struct {
    int       w, h;       /* dimensions in pixels                    */
    uint32_t *px;         /* w*h pixels, 0xAARRGGBB, row-major        */
} image_t;

/* Decode buf[0..len) (format detected from the magic bytes). Returns a heap
 * image on success (free with image_free) or NULL on any failure / OOM. */
image_t *image_decode(const uint8_t *buf, uint32_t len);

/* As image_decode, but allowed to keep the picture up to `fit_dim` pixels on
 * its long edge rather than the browser's 640. The desktop wallpaper is the
 * caller that needs it: it is handed a render cut for this exact panel, and
 * downscaling that to 640 only to stretch it back would throw the whole point
 * away. Costs heap in proportion -- a 4K decode is tens of megabytes live. */
image_t *image_decode_upto(const uint8_t *buf, uint32_t len, int fit_dim);

void     image_free(image_t *im);
