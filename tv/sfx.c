/* ===========================================================================
 *  BoltOS TV  -  tv/sfx.c
 *
 *  The small sounds a remote makes on screen: a soft tick when focus moves,
 *  a rounder tone when something opens, a falling one for Back. Synthesised
 *  once at boot into 48 kHz stereo buffers -- a few hundred milliseconds of
 *  sine with an exponential tail is all any of them are -- and played on the
 *  system channel, so the mixer's master volume and mute apply to them.
 * ===========================================================================*/
#include <stdint.h>
#include "tv.h"
#include "audio.h"
#include "mixer.h"
#include "math.h"

#define RATE 48000
#define MAXF (RATE / 4)

typedef struct { int16_t pcm[MAXF * 2]; uint32_t frames; } snd_t;
static snd_t s_move, s_select, s_back, s_vol;
static int   ready;

/* f0 -> f1 glide over `ms`, decaying with time constant `tau_ms`. */
static void synth(snd_t *s, float f0, float f1, int ms, float tau_ms, float amp) {
    uint32_t n = (uint32_t)(RATE * ms / 1000);
    if (n > MAXF) n = MAXF;
    float ph = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float t = (float)i / RATE;
        float k = (float)i / (float)n;
        float f = f0 + (f1 - f0) * k;
        ph += 6.2831853f * f / RATE;
        if (ph > 6.2831853f) ph -= 6.2831853f;
        float env = expf(-t * 1000.0f / tau_ms);
        float attack = i < 96 ? (float)i / 96.0f : 1.0f;     /* no click */
        float v = (sinf(ph) * 0.85f + sinf(ph * 2.0f) * 0.15f) * env * attack * amp;
        int16_t q = (int16_t)(v * 32767.0f);
        s->pcm[i * 2] = q;
        s->pcm[i * 2 + 1] = q;
    }
    s->frames = n;
}

void sfx_init(void) {
    synth(&s_move,   1480.0f, 1420.0f,  45,  9.0f, 0.16f);
    synth(&s_select,  880.0f, 1320.0f, 140, 45.0f, 0.22f);
    synth(&s_back,   1100.0f,  660.0f, 120, 40.0f, 0.18f);
    synth(&s_vol,    1000.0f, 1000.0f,  60, 14.0f, 0.20f);
    ready = 1;
}

static void play(const snd_t *s) {
    if (!ready || !tvs.sounds || tvs.muted || !audio_pcm_ok()) return;
    audio_play_async_ch(MIX_SYSTEM, s->pcm, s->frames);
}

void sfx_move(void)   { play(&s_move); }
void sfx_select(void) { play(&s_select); }
void sfx_back(void)   { play(&s_back); }
void sfx_volume(void) { if (ready && !tvs.muted && audio_pcm_ok()) audio_play_async_ch(MIX_SYSTEM, s_vol.pcm, s_vol.frames); }
