#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS Mini  -  mini/h264.h
 *
 *  An H.264 decoder: Main profile, progressive, 8-bit 4:2:0 -- I, P and B
 *  slices, CAVLC and CABAC, weighted prediction, spatial and temporal
 *  direct, the deblocking filter. That is what YouTube's 360p stream (its
 *  format 18) is, whatever its codecs string says. Written from the
 *  standard (ITU-T H.264); test/host/h264test.c holds it to ffmpeg, frame
 *  for frame, bit for bit.
 * ===========================================================================*/
typedef struct h264 h264;

typedef struct {
    const uint8_t *y, *u, *v;          /* the visible picture, cropped          */
    int w, h, stride, cstride;
    int64_t pts;                        /* as handed to h264_decode              */
} h264_pic;

/* `avcc` is the MP4's avcC box body: the parameter sets and NAL length size. */
h264 *h264_open(const uint8_t *avcc, int len);
void  h264_close(h264 *d);
/* One access unit (an MP4 sample: length-prefixed NAL units). Returns 0, or
 * -1 if the stream uses something this decoder doesn't do. */
int   h264_decode(h264 *d, const uint8_t *au, int len, int64_t pts);
/* The next picture in display order, if one is ready. 1 if *p was filled. */
int   h264_output(h264 *d, h264_pic *p);
/* After the last access unit: let the held-back pictures out. */
void  h264_flush(h264 *d);
const char *h264_error(h264 *d);
/* Allocate up front what decoding allocates as it goes (a NAL buffer of
 * nal_bytes, the first `pics` pictures), for a decoder run on another core. */
int   h264_prealloc(h264 *d, int nal_bytes, int pics);
