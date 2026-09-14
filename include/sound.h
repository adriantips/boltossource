#pragma once
#include <stdint.h>
#include "wav.h"
#include "mp3.h"

/* ===========================================================================
 *  One audio source, whatever the file turned out to be.
 *
 *  `play`, the Media player and the Music app all want the same four things --
 *  open a path, pull 48 kHz stereo frames out of it, seek, close -- and none of
 *  them should have to know whether the bytes were RIFF or MPEG. This is that
 *  seam. Adding a format means teaching snd_open() to recognise it and adding a
 *  branch to four short functions, not editing every player.
 *
 *  The format is decided by looking at the bytes, not at the name: a `.wav`
 *  that is really an MP3 plays, and a `.mp3` that is really a PNG is refused by
 *  name rather than played as noise.
 *
 *  snd_step() is the one thing here that is not just dispatch. A WAV costs
 *  almost nothing per sample, so the players got away with decoding half a
 *  second in one go; an MPEG frame costs real arithmetic, and 500 ms of it in
 *  one pass of the compositor's loop is a visible stutter. snd_step() does at
 *  most one frame of work and returns what it managed, so a caller sharing a
 *  thread with the screen can fill its buffer a little at a time.
 * ===========================================================================*/

enum { SND_NONE = 0, SND_WAV, SND_MP3 };

typedef struct {
    int       kind;
    wav_t     wav;          /* valid when kind == SND_WAV                     */
    mp3_t    *mp3;          /* valid when kind == SND_MP3                     */
    uint8_t  *bytes;        /* the file, owned here; both decoders read it in
                             * place rather than copying it                   */
    uint32_t  len;
} snd_t;

/* Read a path through the VFS and open whichever decoder fits. Returns 0, or
 * -1 with *err (optional) pointing at a static reason. */
int  snd_open(const char *path, snd_t *s, const char **err);
void snd_close(snd_t *s);

/* Interleaved L/R at AUDIO_RATE. snd_read fills what it can; snd_step does at
 * most one decoder step first. Both return frames produced, and zero means the
 * source is finished. */
uint32_t snd_read(snd_t *s, int16_t *out, uint32_t frames);
uint32_t snd_step(snd_t *s, int16_t *out, uint32_t frames);

uint32_t snd_duration_ms(const snd_t *s);
uint32_t snd_position_ms(const snd_t *s);
void     snd_seek_ms(snd_t *s, uint32_t ms);
int      snd_done(const snd_t *s);

/* One line naming the stream, for a player that wants to show it:
 * "MPEG-1 Layer III, 44.1 kHz joint stereo, 192 kbps" or
 * "WAV, 22.0 kHz mono, 8-bit". Points into the source; valid until close. */
const char *snd_describe(const snd_t *s);

/* Does this name look like something snd_open would take? Used to decide what
 * belongs in a playlist, so it is about the name and not the bytes. */
int snd_is_audio_name(const char *name);
