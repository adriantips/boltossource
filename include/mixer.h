#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS  -  include/mixer.h
 *  The volume mixer: one level and one mute per source, kept across reboots.
 *
 *  What was here before was a single number. `audio_set_volume(80)` and
 *  `audio_volume()`, and mute was spelled `audio_set_volume(vol ? 0 : 80)` in
 *  four different places -- which is not a mute, it is a volume change that
 *  forgets where you had it. Turn the music down to 15, mute, unmute, and you
 *  are at 80 with your neighbours awake.
 *
 *  So: named channels. Master, and one per kind of thing that makes noise, so
 *  a loud game and a quiet podcast can be told apart and the notification
 *  chime can be silenced without silencing the track under it. Each has a
 *  level and a mute flag that remembers the level.
 *
 *  HONEST LIMIT, stated here because it shapes the whole design: this machine
 *  plays ONE stream at a time. There is one DMA engine, one buffer, and
 *  audio_set_preempt() exists precisely because two callers pushing at it
 *  produce noise. So this does not sum streams the way a desktop mixer on a
 *  machine with a software mixing daemon does. A channel's level is applied as
 *  the codec's gain for as long as that channel owns the device -- master
 *  times channel, one multiply, no resampling, no copy. The knobs behave the
 *  way you expect; what you cannot do is hear two of them at once, and that is
 *  a property of the audio path rather than of this file.
 *
 *  Peak levels are measured from the PCM on its way past (no copy: the buffer
 *  is scanned where the caller left it), which is what the meters draw.
 * ========================================================================= */

enum {
    MIX_MASTER = 0,   /* scales everything below it                          */
    MIX_MEDIA,        /* Media player, Music -- anything playing a file      */
    MIX_SYSTEM,       /* notification chimes, `play`, the startup sound      */
    MIX_APP,          /* Piano, DOOM, ring-3 programs, bare tones            */
    MIX_CAPTURE,      /* microphone input gain -- not scaled by master       */
    MIX_COUNT
};

void mixer_init(void);            /* defaults, then /etc/mixer.conf          */
int  mixer_load(void);            /* re-read the file; 1 if it existed       */
int  mixer_save(void);            /* write it; 0 on success                  */
void mixer_reset(void);           /* back to the built-in defaults           */

int         mixer_count(void);
const char *mixer_name(int ch);        /* "media"  -- the on-disk spelling   */
const char *mixer_label(int ch);       /* "Media"  -- what the UI shows      */
const char *mixer_describe(int ch);    /* "Music, podcasts, video"           */
int         mixer_channel_by_name(const char *name);   /* -1 if unknown      */
int         mixer_is_input(int ch);    /* 1 for MIX_CAPTURE                  */

int  mixer_volume(int ch);             /* 0..100, the slider position        */
void mixer_set_volume(int ch, int pct);
int  mixer_muted(int ch);
void mixer_set_mute(int ch, int on);
void mixer_toggle_mute(int ch);

/* What the hardware is actually asked for: master * channel, and 0 if either
 * end is muted. For MIX_CAPTURE, the channel's own level with no master. */
int  mixer_effective(int ch);

/* The channel that currently owns the device, and the gain that goes with it.
 * audio.c sets this on every submit; the mixer app highlights it. */
int  mixer_active(void);
void mixer_set_active(int ch);

/* --- meters ---------------------------------------------------------------
 *  mixer_note_peak() is called by the audio dispatch layer with the buffer on
 *  its way to the codec. mixer_peak() decays towards zero with wall time, so a
 *  meter drawn at any frame rate falls smoothly and a stopped stream empties
 *  instead of freezing at whatever the last chunk happened to be.
 * ------------------------------------------------------------------------*/
void mixer_note_peak(int ch, const int16_t *pcm, uint32_t frames);
int  mixer_peak(int ch);               /* 0..100 */
