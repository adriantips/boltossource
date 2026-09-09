#pragma once
#include <stdint.h>

/* ===========================================================================
 *  PNG encoder.
 *
 *  BoltOS could read five image formats and write none, which is fine until
 *  the machine wants to hand something back: a screenshot, a Paint drawing, a
 *  captured frame. A picture the OS can only look at is not a file.
 *
 *  This writes a real PNG -- 8-bit truecolour, zlib with fixed-Huffman deflate
 *  and LZ77 over a 32 KiB window -- so the result opens anywhere, including in
 *  BoltOS's own decoder. Flat UI screenshots compress 20-50x, which is the
 *  difference between a screenshot being cheap and being something you delete
 *  afterwards.
 *
 *  Integer-only, like the decoder: the kernel is built with no FPU.
 * ========================================================================= */

/* Encode w*h ARGB pixels (0xAARRGGBB, row-major) as an RGB PNG. Alpha is
 * dropped -- everything that produces one of these is opaque, and a colour
 * type nobody needs costs a byte a pixel.
 *
 * On success returns 0 and hands back a kmalloc'd buffer in *out (free it with
 * kfree) and its length in *outlen. Returns -1 on bad arguments or OOM. */
int png_encode(const uint32_t *px, int w, int h, uint8_t **out, uint32_t *outlen);

/* The pieces, exposed because they are generally useful and because the host
 * test checks them against known vectors. */
uint32_t png_crc32(uint32_t crc, const uint8_t *d, uint32_t n);   /* seed 0 */
uint32_t png_adler32(const uint8_t *d, uint32_t n);
