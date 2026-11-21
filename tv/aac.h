#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS TV  -  tv/aac.h
 *
 *  An AAC-LC decoder: what the sound of a YouTube stream is (format 18's
 *  mp4a.40.2, 44.1 kHz stereo). Long and short blocks, both window shapes,
 *  M/S and intensity stereo, noise substitution, TNS and pulses -- all of
 *  Low Complexity. Not SBR or PS: an HE-AAC stream plays its LC core, at
 *  half the rate, which is the sound with the top octave missing. Written
 *  from ISO/IEC 14496-3; test/host/aactest.c holds it up against ffmpeg.
 *
 *  One raw access unit in (an MP4 sample), 1024 frames of 16-bit PCM out,
 *  interleaved stereo at the stream's own rate (a mono stream comes out on
 *  both channels).
 * ===========================================================================*/
typedef struct aac aac;

aac *aac_open(const uint8_t *asc, int asc_len);     /* AudioSpecificConfig */
void aac_close(aac *d);
int  aac_rate(const aac *d);
int  aac_channels(const aac *d);
/* Decode one access unit into out[2048] (1024 stereo frames). Returns the
 * frames written (1024), or -1 if the unit isn't one this decodes -- out is
 * then silence, and the next unit decodes as normal. */
int  aac_decode(aac *d, const uint8_t *au, int len, int16_t *out);
/* After a seek: forget the overlap, so the old position isn't heard. */
void aac_reset(aac *d);
const char *aac_error(const aac *d);

/* ---- the tables (tv/aactab.c, made by tv/mkaactab.py) -------------------- */
extern const uint32_t aac_sf_code[121];
extern const uint8_t  aac_sf_bits[121];
extern const uint16_t *const aac_cb_code[11];
extern const uint8_t  *const aac_cb_bits[11];
extern const uint16_t aac_cb_size[11];
extern const uint8_t  aac_num_swb_1024[13], aac_num_swb_128[13];
extern const uint8_t  aac_tns_max_bands_1024[13], aac_tns_max_bands_128[13];
extern const uint16_t *const aac_swb_offset_1024[13];
extern const uint16_t *const aac_swb_offset_128[13];
extern const float    *const aac_tns_map[4];
