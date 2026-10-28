#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS Mini  -  mini/mp4.h
 *
 *  An MP4's index (its moov box) read into a table of samples: where each
 *  video frame and audio packet is in the file, how big, and when.
 * ===========================================================================*/
typedef struct {
    uint64_t off;
    uint32_t size;
    int64_t  dts, pts;                 /* in the track's timescale               */
    uint8_t  key;
} mp4_sample;

typedef struct {
    int         timescale;
    int64_t     duration;
    mp4_sample *s; int n;
} mp4_track;

typedef struct {
    mp4_track video, audio;
    int       w, h;
    uint8_t   avcc[512]; int avcc_len;  /* the decoder's configuration          */
    uint8_t   asc[64];   int asc_len;   /* AAC's AudioSpecificConfig            */
    int       channels, rate;
} mp4_t;

/* Find a top-level box: returns its offset and size (header included), 0 if not in buf. */
int  mp4_find_box(const uint8_t *buf, uint64_t len, const char *type, uint64_t *off, uint64_t *size);
/* moov's whole box, from its header on. 0 on success. */
int  mp4_parse(const uint8_t *moov, uint64_t len, mp4_t *m);
void mp4_free(mp4_t *m);
