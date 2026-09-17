/* ===========================================================================
 *  BoltStrike -- the noise.
 *
 *  Sixteen sounds, all synthesised at startup out of noise, a couple of sine
 *  tones and an envelope. That is not a compromise forced by the platform --
 *  the platform plays WAVs perfectly well -- it is that the sounds this game
 *  would otherwise want are somebody else's recordings, and a gunshot is one
 *  of the easiest things in the world to fake: a bright noise burst, a low
 *  thump under it, and a decay that is shorter than you think.
 *
 *  Mixing is eight voices into one buffer, pushed a chunk at a time whenever
 *  the codec has drained. There is no callback and no audio thread -- this is
 *  polled from the same desktop loop as everything else, which is why the
 *  chunk is 42 ms and not 5.
 * ===========================================================================*/
#include <stdint.h>
#include "strike.h"
#include "audio.h"
#include "kheap.h"
#include "math.h"
#include "string.h"

#define CHUNK   2048                        /* frames pushed at a time (42 ms) */
#define VOICES  8

typedef struct { const int16_t *pcm; int len, pos, vol, pan, on; } voice_t;

static int16_t *sfx[S_COUNT];
static int      sfx_len[S_COUNT];
static voice_t  voices[VOICES];
static int16_t *mixbuf[2];
static int      which;
static int      ready;

static uint32_t nseed = 0x13579BDFu;
static float noise(void) {
    nseed = nseed * 1664525u + 1013904223u;
    return (float)((int32_t)(nseed >> 8) & 0xFFFF) / 32767.5f - 1.0f;
}

/* One-pole low pass. Dull is what makes a noise burst sound like it came out
 * of something, rather than like static. */
typedef struct { float z; } lp_t;
static float lp(lp_t *s, float x, float a) { s->z += (x - s->z) * a; return s->z; }

static int16_t clip(float v) {
    if (v >  32000.0f) return  32000;
    if (v < -32000.0f) return -32000;
    return (int16_t)v;
}

/* ---- the sounds themselves ---------------------------------------------- */

/* A gunshot: a crack (bright noise, very fast decay), a body (filtered noise,
 * slower) and a thump (a falling sine). Change three numbers and it is a
 * different gun; that is the whole weapon-sound design. */
static void gen_shot(int16_t *out, int len, float bright, float body, float thump,
                     float amp) {
    lp_t f1 = { 0 }, f2 = { 0 };
    float ph = 0.0f;
    for (int i = 0; i < len; i++) {
        float t = (float)i / (float)len;
        float crack = lp(&f1, noise(), bright) * expf(-t * 60.0f);
        float bod   = lp(&f2, noise(), body)   * expf(-t * 13.0f);
        ph += (85.0f - t * 45.0f) * 2.0f * 3.14159265f / 48000.0f;
        float low   = sinf(ph) * expf(-t * 16.0f) * thump;
        out[i] = clip((crack * 0.75f + bod * 0.45f + low) * amp * 26000.0f);
    }
}

static void gen_tone(int16_t *out, int len, float f0, float f1, float decay, float amp) {
    float ph = 0.0f;
    for (int i = 0; i < len; i++) {
        float t = (float)i / (float)len;
        float f = f0 + (f1 - f0) * t;
        ph += f * 2.0f * 3.14159265f / 48000.0f;
        /* a little second harmonic, so it is a beep and not a test signal */
        float v = sinf(ph) * 0.8f + sinf(ph * 2.0f) * 0.2f;
        out[i] = clip(v * expf(-t * decay) * amp * 22000.0f);
    }
}

/* A short filtered click. Footsteps, magazine, bolt -- all the same shape with
 * different filters, which is also true of the real ones. */
static void gen_click(int16_t *out, int len, float bright, float decay, float amp) {
    lp_t f = { 0 };
    for (int i = 0; i < len; i++) {
        float t = (float)i / (float)len;
        out[i] = clip(lp(&f, noise(), bright) * expf(-t * decay) * amp * 20000.0f);
    }
}

/* Two clacks and a spring: a magazine out, a magazine in. */
static void gen_reload(int16_t *out, int len) {
    for (int i = 0; i < len; i++) out[i] = 0;
    int a = len / 12, b = len / 2, c = (len * 5) / 6;
    lp_t f = { 0 };
    for (int i = 0; i < len / 8 && a + i < len; i++) {
        float t = (float)i / (float)(len / 8);
        out[a + i] = clip(lp(&f, noise(), 0.5f) * expf(-t * 22.0f) * 12000.0f);
    }
    for (int i = 0; i < len / 6 && b + i < len; i++) {
        float t = (float)i / (float)(len / 6);
        out[b + i] = clip(lp(&f, noise(), 0.28f) * expf(-t * 15.0f) * 15000.0f);
    }
    for (int i = 0; i < len / 10 && c + i < len; i++) {
        float t = (float)i / (float)(len / 10);
        out[c + i] = clip(lp(&f, noise(), 0.7f) * expf(-t * 30.0f) * 17000.0f);
    }
}

/* A rising or falling three-note figure, for won and lost. */
static void gen_sting(int16_t *out, int len, int up) {
    static const float maj[3] = { 392.0f, 494.0f, 587.0f };
    static const float min_[3] = { 392.0f, 330.0f, 262.0f };
    const float *n = up ? maj : min_;
    for (int i = 0; i < len; i++) out[i] = 0;
    int seg = len / 3;
    for (int k = 0; k < 3; k++) {
        float ph = 0.0f;
        for (int i = 0; i < seg; i++) {
            float t = (float)i / (float)seg;
            ph += n[k] * 2.0f * 3.14159265f / 48000.0f;
            float v = sinf(ph) * 0.6f + sinf(ph * 2.0f) * 0.25f + sinf(ph * 3.0f) * 0.15f;
            out[k * seg + i] = clip(v * expf(-t * 3.2f) * 15000.0f);
        }
    }
}

static int alloc_sfx(int id, int ms) {
    int len = 48 * ms;
    sfx[id] = kmalloc((uint64_t)len * 2);
    if (!sfx[id]) return -1;
    sfx_len[id] = len;
    return 0;
}

int st_sfx_init(void) {
    if (ready) return 0;
    if (!audio_pcm_ok()) return -1;          /* no codec: stay silent, quietly */

    static const int ms[S_COUNT] = {
        [S_SHOT_PISTOL] = 170, [S_SHOT_RIFLE] = 200, [S_SHOT_SNIPER] = 380,
        [S_SHOT_SHOTGUN] = 300, [S_KNIFE] = 120, [S_RELOAD] = 600, [S_STEP] = 70,
        [S_HIT] = 130, [S_DIE] = 420, [S_BEEP] = 90, [S_PLANT] = 500,
        [S_DEFUSE] = 400, [S_WIN] = 900, [S_LOSE] = 900, [S_BUY] = 120,
        [S_EMPTY] = 70,
    };
    for (int i = 0; i < S_COUNT; i++)
        if (alloc_sfx(i, ms[i]) != 0) { st_sfx_free(); return -1; }

    for (int i = 0; i < 2; i++) {
        mixbuf[i] = kmalloc(CHUNK * 2 * sizeof(int16_t));
        if (!mixbuf[i]) { st_sfx_free(); return -1; }
    }

    gen_shot (sfx[S_SHOT_PISTOL],  sfx_len[S_SHOT_PISTOL],  0.60f, 0.22f, 0.35f, 0.80f);
    gen_shot (sfx[S_SHOT_RIFLE],   sfx_len[S_SHOT_RIFLE],   0.78f, 0.30f, 0.55f, 1.00f);
    gen_shot (sfx[S_SHOT_SNIPER],  sfx_len[S_SHOT_SNIPER],  0.88f, 0.16f, 0.80f, 1.00f);
    gen_shot (sfx[S_SHOT_SHOTGUN], sfx_len[S_SHOT_SHOTGUN], 0.42f, 0.35f, 0.70f, 1.00f);
    gen_click(sfx[S_KNIFE],        sfx_len[S_KNIFE],        0.90f, 26.0f, 0.55f);
    gen_reload(sfx[S_RELOAD],      sfx_len[S_RELOAD]);
    gen_click(sfx[S_STEP],         sfx_len[S_STEP],         0.16f, 34.0f, 0.35f);
    gen_click(sfx[S_HIT],          sfx_len[S_HIT],          0.30f, 24.0f, 0.70f);
    gen_shot (sfx[S_DIE],          sfx_len[S_DIE],          0.14f, 0.09f, 0.30f, 0.55f);
    gen_tone (sfx[S_BEEP],         sfx_len[S_BEEP],  2200.0f, 2200.0f, 9.0f, 0.55f);
    gen_tone (sfx[S_PLANT],        sfx_len[S_PLANT],  220.0f,  660.0f, 3.0f, 0.60f);
    gen_tone (sfx[S_DEFUSE],       sfx_len[S_DEFUSE], 880.0f,  330.0f, 3.5f, 0.55f);
    gen_sting(sfx[S_WIN],          sfx_len[S_WIN],  1);
    gen_sting(sfx[S_LOSE],         sfx_len[S_LOSE], 0);
    gen_tone (sfx[S_BUY],          sfx_len[S_BUY],  1400.0f, 1900.0f, 14.0f, 0.40f);
    gen_click(sfx[S_EMPTY],        sfx_len[S_EMPTY],        0.95f, 44.0f, 0.30f);

    ready = 1;
    return 0;
}

void st_sfx_free(void) {
    ready = 0;
    for (int i = 0; i < S_COUNT; i++) { kfree(sfx[i]); sfx[i] = 0; sfx_len[i] = 0; }
    for (int i = 0; i < 2; i++) { kfree(mixbuf[i]); mixbuf[i] = 0; }
    for (int i = 0; i < VOICES; i++) voices[i].on = 0;
}

void st_sfx_silence(void) {
    for (int i = 0; i < VOICES; i++) voices[i].on = 0;
}

void st_sfx_play(int id, int vol, int pan) {
    if (!ready || id < 0 || id >= S_COUNT || !sfx[id]) return;
    if (vol <= 0) return;
    if (vol > 255) vol = 255;

    /* Steal the oldest voice rather than dropping the new sound: the one you
     * just caused is always more interesting than the one already fading. */
    int slot = -1, most = -1;
    for (int i = 0; i < VOICES; i++) {
        if (!voices[i].on) { slot = i; break; }
        if (voices[i].pos > most) { most = voices[i].pos; slot = i; }
    }
    voices[slot].pcm = sfx[id];
    voices[slot].len = sfx_len[id];
    voices[slot].pos = 0;
    voices[slot].vol = vol;
    voices[slot].pan = pan;
    voices[slot].on  = 1;
}

void st_sfx_pump(void) {
    if (!ready || audio_busy()) return;

    int any = 0;
    for (int i = 0; i < VOICES; i++) if (voices[i].on) { any = 1; break; }
    if (!any) return;

    int16_t *out = mixbuf[which];
    which ^= 1;
    memset(out, 0, CHUNK * 2 * sizeof(int16_t));

    for (int v = 0; v < VOICES; v++) {
        voice_t *vo = &voices[v];
        if (!vo->on) continue;
        /* Constant-power-ish pan, done with two integers because this runs
         * 16,000 times per buffer and nothing here needs a cosine. */
        int lg = (vo->vol * (128 - vo->pan)) >> 8;
        int rg = (vo->vol * (128 + vo->pan)) >> 8;
        if (lg < 0) lg = 0;
        if (rg < 0) rg = 0;
        int n = vo->len - vo->pos;
        if (n > CHUNK) n = CHUNK;
        const int16_t *s = &vo->pcm[vo->pos];
        for (int i = 0; i < n; i++) {
            int sv = s[i];
            int l = out[i * 2]     + ((sv * lg) >> 8);
            int r = out[i * 2 + 1] + ((sv * rg) >> 8);
            if (l >  32000) l =  32000;
            if (l < -32000) l = -32000;
            if (r >  32000) r =  32000;
            if (r < -32000) r = -32000;
            out[i * 2]     = (int16_t)l;
            out[i * 2 + 1] = (int16_t)r;
        }
        vo->pos += n;
        if (vo->pos >= vo->len) vo->on = 0;
    }

    audio_play_async(out, CHUNK);
}
