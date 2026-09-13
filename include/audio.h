#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Audio output.
 *
 *  Two codec drivers sit behind one interface: Intel HD Audio (drivers/hda.c),
 *  which is what every machine built since about 2005 actually has, and AC'97
 *  (drivers/ac97.c) for older hardware and QEMU's -device AC97. audio_init()
 *  probes HDA first and falls back to AC'97, then to the PC speaker, so
 *  audio_tone() always makes a sound even with no PCI codec at all.
 *
 *  Everything above this line -- Music, Piano, DOOM, the `play` command --
 *  speaks only 48 kHz / 16-bit / signed / stereo PCM and never learns which
 *  codec it reached.
 * ===========================================================================*/

/* Every codec here is programmed for one format, and everything above them
 * produces it: signed 16-bit stereo at this rate. */
#define AUDIO_RATE 48000u

void        audio_init(void);
int         audio_present(void);          /* 1 if a PCI codec was claimed */
const char *audio_name(void);             /* "Intel HDA" / "AC97" / "PC speaker" */

/* Play a square-wave tone of `freq` Hz for `ms` milliseconds (blocking). */
void audio_tone(uint32_t freq, uint32_t ms);

/* Play raw 16-bit signed stereo PCM at 48 kHz (frames = L/R pairs). Blocking. */
void audio_play_pcm(const int16_t *interleaved, uint32_t frames);

/* Non-blocking streaming: start a buffer and return; poll audio_busy() and push
 * the next chunk once it drains. audio_pcm_ok() = real PCM device available. */
void audio_play_async(const int16_t *interleaved, uint32_t frames);
int  audio_busy(void);
void audio_stop(void);
int  audio_pcm_ok(void);

/* A blocking play takes the device away from whatever was streaming through it.
 * The dispatch layer has no idea what that is, so the desktop registers a
 * callback here that stops the streaming players (Media, Music) first --
 * without it, `play` and a track already running push buffers at one DMA
 * engine and both come out broken. Same arrangement as notify_set_hook(). */
void audio_set_preempt(void (*stop_streams)(void));

void audio_set_volume(int percent);       /* 0..100 master volume */
int  audio_volume(void);

/* --- which knob a sound answers to ---------------------------------------
 *  The calls above are the master channel, kept because half the desktop
 *  already speaks them. The ones below name the source instead, so Media can
 *  be turned down without turning the notification chime down with it. See
 *  include/mixer.h -- including the honest note about one stream at a time.
 * ------------------------------------------------------------------------*/
void audio_play_async_ch(int mix_ch, const int16_t *interleaved, uint32_t frames);
void audio_play_pcm_ch(int mix_ch, const int16_t *interleaved, uint32_t frames);
void audio_tone_ch(int mix_ch, uint32_t freq, uint32_t ms);

/* Re-apply the gain for whichever channel owns the device. The mixer UI calls
 * this after moving a slider so the change is heard in the buffer already
 * playing rather than at the next one. */
void audio_gain_changed(void);

/* --- capture --------------------------------------------------------------
 *  The other direction, and it is not the output path with the arrow turned
 *  round. An output route is chosen once at boot and left open; an input route
 *  costs something to hold (a mic bias supply, a running DMA engine, a stream
 *  descriptor the output side would otherwise be free to use), so it is opened
 *  on demand and closed again.
 *
 *  The ring is cyclic and the engine never stops while it is open: read()
 *  takes whatever has arrived since the last call. Not reading for longer than
 *  the ring holds loses the oldest samples rather than stalling the engine --
 *  a meter that falls behind should skip, not drag.
 *
 *  Same PCM contract as playback: 48 kHz, signed 16-bit, stereo.
 * ------------------------------------------------------------------------*/
int         audio_capture_ok(void);      /* an input route exists on this codec */
const char *audio_capture_name(void);    /* "Mic" / "Line in" / "" if none      */
int         audio_capture_open(void);    /* 0 on success, -1 if there is none   */
uint32_t    audio_capture_read(int16_t *out, uint32_t frames);  /* frames taken */
void        audio_capture_close(void);
int         audio_capture_active(void);

/* Take whatever has arrived and feed it to the mixer's capture meter, throwing
 * the samples away. What a level meter needs when nobody is recording. */
void        audio_capture_pump(void);

/* --- codec back end -------------------------------------------------------
 *  A codec driver publishes one of these from its probe. Nothing outside
 *  drivers/audio.c calls through it; the dispatch layer owns the fallbacks,
 *  the tone generator and the volume bookkeeping, so a driver only has to move
 *  PCM at 48 kHz and say when it has finished.
 * ------------------------------------------------------------------------*/
typedef struct audio_dev {
    const char *name;
    uint32_t (*capacity)(void);                        /* max frames per submit */
    void     (*submit)(const int16_t *pcm, uint32_t frames);  /* copy + start DMA */
    int      (*busy)(void);                            /* 1 while it is playing */
    void     (*stop)(void);
    void     (*volume)(int percent);                   /* 0..100 */

    /* Capture. All five are NULL together on a codec with no input route --
     * which is most of what a driver's probe has to decide, since a machine
     * with a microphone jack it cannot reach is not a machine with a
     * microphone. `cap_name` is what the jack calls itself. */
    int         (*cap_open)(void);
    uint32_t    (*cap_read)(int16_t *out, uint32_t frames);
    void        (*cap_close)(void);
    void        (*cap_gain)(int percent);
    const char *(*cap_name)(void);
} audio_dev_t;

/* Claim the device if present, otherwise return NULL. Called only by
 * audio_init(), in preference order. */
const audio_dev_t *hda_probe(void);
const audio_dev_t *ac97_probe(void);
