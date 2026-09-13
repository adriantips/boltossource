#pragma once
#include <stdint.h>

/* ===========================================================================
 *  RIFF/WAVE decoding, straight into the format the codecs want.
 *
 *  Every audio device in BoltOS is fixed at 48 kHz, 16-bit, stereo (see
 *  audio.h), and almost no WAV file on a real disk is any of those three
 *  things: a system sound is likely 22050 Hz, mono, 8-bit unsigned. So the
 *  decoder is also the converter -- it resamples, expands mono to stereo and
 *  sign-converts on the way out, and the caller only ever sees AUDIO_RATE
 *  stereo frames.
 *
 *  Decoding is incremental. wav_read() fills whatever buffer it is given and
 *  keeps its place, so a player pushes one chunk, waits for the device to drain
 *  it and asks for the next -- a track never has to be converted in full, or
 *  held twice, to start playing.
 *
 *  Supported: PCM (and WAVE_FORMAT_EXTENSIBLE wrapping PCM), 8-bit unsigned or
 *  16-bit signed, one or more channels. Anything else is rejected by name
 *  rather than played as noise.
 * ===========================================================================*/

typedef struct {
    const uint8_t *data;      /* the data chunk, not the file                 */
    uint32_t bytes;           /* its length                                   */
    uint32_t rate;            /* source sample rate                           */
    uint16_t channels;        /* source channel count (we mix down to two)    */
    uint16_t bits;            /* 8 or 16                                      */
    uint32_t frames;          /* total source frames                          */
    uint64_t step;            /* source frames per output frame, 16.16 fixed  */
    uint64_t posf;            /* read cursor in source frames, 16.16 fixed    */
} wav_t;

/* Parse `len` bytes of a WAV file. Returns 0 and fills *w, or -1 and points
 * *err (optional) at a static reason. The buffer must outlive `w`: decoding
 * reads from it in place rather than copying it. */
int wav_open(const void *file, uint32_t len, wav_t *w, const char **err);

/* Convert up to `frames` output frames into `out` (interleaved L/R at
 * AUDIO_RATE). Returns how many it produced -- fewer than asked for, or zero,
 * means the end of the track. */
uint32_t wav_read(wav_t *w, int16_t *out, uint32_t frames);

uint32_t wav_duration_ms(const wav_t *w);
uint32_t wav_position_ms(const wav_t *w);
void     wav_seek_ms(wav_t *w, uint32_t ms);
int      wav_done(const wav_t *w);

/* Read a whole file through the VFS (so it works on BoltFS, FAT32 and ext2
 * alike) and parse it. On success *buf owns a kmalloc'd copy the caller must
 * kfree once it has finished with *w. Returns 0, or -1 with *err set. */
int wav_load(const char *path, wav_t *w, uint8_t **buf, const char **err);

/* --- and back out again ---------------------------------------------------
 *  The other direction, which only arrived when there was something to record.
 *  No conversion here and none wanted: a capture is already 48 kHz signed
 *  16-bit stereo, and anything that resampled it on the way to the disk would
 *  be throwing away the only copy.
 *
 *  wav_header() fills a 44-byte canonical RIFF/WAVE header so a recorder can
 *  write it first and append frames as they arrive -- the length fields have to
 *  be patched once the recording stops, which is what wav_fix_length() is for.
 * ------------------------------------------------------------------------*/
#define WAV_HDR_BYTES 44
void wav_header(uint8_t *out44, uint32_t rate, uint16_t channels, uint32_t frames);
void wav_fix_length(uint8_t *hdr44, uint16_t channels, uint32_t frames);

/* Write a complete file through the VFS. Returns 0, or -1 with *err set. */
int wav_save(const char *path, const int16_t *pcm, uint32_t frames,
             uint32_t rate, uint16_t channels, const char **err);
