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

/* ---- animated PNG (APNG) ------------------------------------------------- *
 *  The same encoder with more frames on the end. A recording is a PNG whose
 *  first frame is an ordinary image, so a viewer that has never heard of APNG
 *  shows that frame instead of an error, and every browser plays the rest.
 *
 *  Frames after the first carry only a rectangle. A desktop changes inside one
 *  window at a time, and re-sending the three megabytes that did not change
 *  would make ten seconds of screen bigger than the filesystem.
 *
 *  Each frame is added with no duration. The duration of a frame is how long
 *  it stayed on screen, which is not known until the next one arrives -- so
 *  the recorder calls png_anim_hold() to write it into the frame already in
 *  the buffer. Guessing it from a nominal frame rate would make every
 *  recording play back at a speed the machine did not actually manage.        */
typedef struct png_anim png_anim;

png_anim *png_anim_begin(int w, int h);

/* Add a frame from the `stride`-pixel-wide image `px`, covering (x,y,w,h).
 * The first frame must cover the whole image; it becomes the PNG's own. */
int png_anim_add(png_anim *a, const uint32_t *px, int stride,
                 int x, int y, int w, int h);

/* How long the newest frame has been on screen, in ms. Call it as often as you
 * like; it overwrites rather than accumulating. */
void png_anim_hold(png_anim *a, uint32_t ms);

/* Close the file. Hands back a kmalloc'd buffer (kfree it) and frees `a`
 * either way. -1 if anything went wrong along the way. */
int  png_anim_finish(png_anim *a, uint8_t **out, uint32_t *outlen);
void png_anim_abort(png_anim *a);

uint32_t png_anim_bytes(const png_anim *a);    /* written so far */
int      png_anim_frames(const png_anim *a);

/* The pieces, exposed because they are generally useful and because the host
 * test checks them against known vectors. */
uint32_t png_crc32(uint32_t crc, const uint8_t *d, uint32_t n);   /* seed 0 */
uint32_t png_adler32(const uint8_t *d, uint32_t n);
