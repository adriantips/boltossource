#pragma once
#include <stdint.h>

/* ===========================================================================
 *  MPEG-1/2/2.5 Layer III decoding.
 *
 *  Same contract as wav.h and for the same reason: everything above the codec
 *  drivers speaks 48 kHz signed 16-bit stereo and nothing else, so the decoder
 *  is also the resampler. An MP3 is 32, 44.1 or 48 kHz (or a half or a quarter
 *  of those on MPEG-2 and 2.5), and the caller never finds out which.
 *
 *  The state is large -- two channels of overlap, a 1024-sample synthesis FIFO
 *  each, the bit reservoir, and about 12 KB of tables built at first use -- so
 *  unlike wav_t this is opaque and heap-allocated. Open it, read from it, close
 *  it.
 *
 *  Decoding is incremental in a way that matters more than it does for WAV: an
 *  MPEG frame is a fixed 1152 (or 576) samples and costs real arithmetic, so
 *  mp3_read() decodes only as many frames as the output buffer needs and keeps
 *  its place mid-frame. mp3_step() is the same thing with a ceiling on the work
 *  -- one MPEG frame and no more -- for callers that are sharing a thread with
 *  something that has to stay responsive.
 * ===========================================================================*/

typedef struct mp3 mp3_t;

/* Parse enough of `file` to know it is an MP3 and to report its shape. The
 * buffer must outlive the decoder: frames are read from it in place. Returns
 * NULL and points *err (optional) at a static reason. */
mp3_t *mp3_open(const void *file, uint32_t len, const char **err);
void   mp3_close(mp3_t *m);

/* Convert up to `frames` output frames (interleaved L/R at AUDIO_RATE) into
 * `out`. Returns how many it produced; fewer than asked for, or zero, is the
 * end of the stream. */
uint32_t mp3_read(mp3_t *m, int16_t *out, uint32_t frames);

/* As mp3_read, but decodes at most one MPEG frame before returning -- so the
 * worst case is bounded whatever `frames` says. Zero means the stream ended;
 * a caller filling a buffer loops until it has enough or gets zero. */
uint32_t mp3_step(mp3_t *m, int16_t *out, uint32_t frames);

uint32_t mp3_duration_ms(const mp3_t *m);
uint32_t mp3_position_ms(const mp3_t *m);
void     mp3_seek_ms(mp3_t *m, uint32_t ms);
int      mp3_done(const mp3_t *m);

/* What the stream turned out to be, for a player that wants to say so:
 * "MPEG-1 Layer III, 44.1 kHz joint stereo, 192 kbps". Valid until close. */
const char *mp3_describe(const mp3_t *m);
uint32_t    mp3_rate(const mp3_t *m);
uint32_t    mp3_channels(const mp3_t *m);
uint32_t    mp3_bitrate(const mp3_t *m);      /* kbps; the first frame's, or the
                                               * average over a VBR header     */

/* 1 if these bytes begin an MP3 -- an ID3 tag or a run of frame headers that
 * agree with each other. Cheap; does not decode. */
int mp3_sniff(const void *file, uint32_t len);
