/* ===========================================================================
 *  BoltOS  -  kernel/mixer.c
 *  Channel levels, mutes, meters, and the file that keeps them.
 *
 *  Policy only. Nothing here touches a register; drivers/audio.c asks
 *  mixer_effective() what gain to hand the codec, and that is the whole
 *  contract between the two.
 * ========================================================================= */
#include <stdint.h>
#include <stdio.h>
#include "mixer.h"
#include "fs.h"
#include "pit.h"
#include "kprintf.h"
#include "string.h"

#define MIXER_PATH "/etc/mixer.conf"

typedef struct {
    const char *name, *label, *desc;
    int         def;
    int         input;
} chan_def;

/* NB: order must match the enum in mixer.h. */
static const chan_def CHANS[MIX_COUNT] = {
 { "master",  "Master",     "Everything, scaled",              80, 0 },
 { "media",   "Media",      "Music, podcasts, video",         100, 0 },
 { "system",  "System",     "Chimes, alerts, `play`",          70, 0 },
 { "app",     "Apps",       "Games, Piano, ring-3 programs",  100, 0 },
 { "capture", "Microphone", "Input gain, not scaled by master", 60, 1 },
};

static struct {
    int      vol;
    int      mute;
    uint8_t  peak;        /* 0..100 at the moment it was measured */
    uint64_t peak_at;     /* pit_ticks() when that was            */
} ch[MIX_COUNT];

static int active = MIX_APP;

/* A meter that falls this far in a second. Slow enough to read at 60 Hz, fast
 * enough that a track ending empties the bar rather than leaving it hanging. */
#define DECAY_PER_SEC 160

int         mixer_count(void) { return MIX_COUNT; }
static int  ok(int c)         { return c >= 0 && c < MIX_COUNT; }
const char *mixer_name(int c)     { return ok(c) ? CHANS[c].name  : "?"; }
const char *mixer_label(int c)    { return ok(c) ? CHANS[c].label : "?"; }
const char *mixer_describe(int c) { return ok(c) ? CHANS[c].desc  : "";  }
int         mixer_is_input(int c) { return ok(c) ? CHANS[c].input : 0;   }
int         mixer_volume(int c)   { return ok(c) ? ch[c].vol  : 0; }
int         mixer_muted(int c)    { return ok(c) ? ch[c].mute : 0; }
int         mixer_active(void)    { return active; }
void        mixer_set_active(int c) { if (ok(c) && !CHANS[c].input) active = c; }

int mixer_channel_by_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < MIX_COUNT; i++) if (strcmp(name, CHANS[i].name) == 0) return i;
    return -1;
}

void mixer_reset(void) {
    for (int i = 0; i < MIX_COUNT; i++) {
        ch[i].vol = CHANS[i].def;
        ch[i].mute = 0;
        ch[i].peak = 0;
        ch[i].peak_at = 0;
    }
    active = MIX_APP;
}

void mixer_set_volume(int c, int pct) {
    if (!ok(c)) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    ch[c].vol = pct;
    /* Dragging a slider off zero is what people do instead of finding the mute
     * button again, so treat it as the unmute it plainly is. */
    if (pct > 0) ch[c].mute = 0;
}

void mixer_set_mute(int c, int on) { if (ok(c)) ch[c].mute = on ? 1 : 0; }
void mixer_toggle_mute(int c)      { if (ok(c)) ch[c].mute = !ch[c].mute; }

int mixer_effective(int c) {
    if (!ok(c)) return 0;
    if (ch[c].mute) return 0;
    if (CHANS[c].input) return ch[c].vol;        /* capture has no master above it */
    if (c == MIX_MASTER) return ch[MIX_MASTER].vol;
    if (ch[MIX_MASTER].mute) return 0;
    return ch[c].vol * ch[MIX_MASTER].vol / 100;
}

/* ------------------------------------------------------------- meters ---- */
void mixer_note_peak(int c, const int16_t *pcm, uint32_t frames) {
    if (!ok(c) || !pcm || !frames) return;
    /* Every sample of a half-second chunk is 48000 comparisons for one number
     * nobody can see the difference in. Step through it instead: 512 samples
     * spread across the buffer catch the peak of anything that sounds like
     * music, and cost nothing worth measuring. */
    uint32_t step = frames / 512;
    if (!step) step = 1;
    int32_t hi = 0;
    for (uint32_t i = 0; i < frames; i += step) {
        int32_t v = pcm[i * 2];
        if (v < 0) v = -v;
        if (v > hi) hi = v;
    }
    int lvl = (int)(hi * 100 / 32768);
    if (lvl > 100) lvl = 100;
    ch[c].peak = (uint8_t)lvl;
    ch[c].peak_at = pit_ticks();
    /* The master meter shows whatever is playing, so it does not sit dead
     * while a channel below it is working. */
    if (c != MIX_MASTER && lvl > mixer_peak(MIX_MASTER)) {
        ch[MIX_MASTER].peak = (uint8_t)lvl;
        ch[MIX_MASTER].peak_at = ch[c].peak_at;
    }
}

int mixer_peak(int c) {
    if (!ok(c) || !ch[c].peak) return 0;
    uint64_t age = pit_ticks() - ch[c].peak_at;      /* ms; the PIT ticks at 1 kHz */
    int fall = (int)(age * DECAY_PER_SEC / 1000u);
    int v = ch[c].peak - fall;
    return v > 0 ? v : 0;
}

/* -------------------------------------------------------------- file ----- */
int mixer_save(void) {
    char text[512];
    uint32_t n = 0;
    #define OUT(...) do { n += (uint32_t)snprintf(text + n, sizeof text - n, __VA_ARGS__); \
                          if (n >= sizeof text) return -1; } while (0)
    OUT("# BoltOS mixer levels. `mixer` edits this. One channel per line:\n"
        "# NAME = LEVEL [muted]\n");
    for (int i = 0; i < MIX_COUNT; i++)
        OUT("%s = %d%s\n", CHANS[i].name, ch[i].vol, ch[i].mute ? " muted" : "");
    #undef OUT

    fs_node *f = fs_lookup(MIXER_PATH);
    if (!f) {
        if (!fs_lookup("/etc")) fs_create("/etc", 1);
        f = fs_create(MIXER_PATH, 0);
    }
    if (!f || f->is_dir) return -1;
    return fs_write(f, text, n) == 0 ? 0 : -1;
}

int mixer_load(void) {
    fs_node *f = fs_lookup(MIXER_PATH);
    if (!f || f->is_dir || !f->data || !f->size) return 0;

    const char *p = (const char *)f->data;
    uint32_t left = f->size;
    while (left) {
        char line[80]; uint32_t n = 0;
        while (left && *p != '\n') { if (n < sizeof line - 1) line[n++] = *p; p++; left--; }
        if (left) { p++; left--; }
        line[n] = 0;

        for (uint32_t i = 0; i < n; i++) if (line[i] == '#') { line[i] = 0; n = i; break; }
        char *eq = 0;
        for (uint32_t i = 0; i < n; i++) if (line[i] == '=') { eq = line + i; break; }
        if (!eq) continue;
        *eq = 0;
        char *name = line, *val = eq + 1;
        while (*name == ' ' || *name == '\t') name++;
        for (char *e = name + strlen(name); e > name && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*val == ' ' || *val == '\t') val++;

        int c = mixer_channel_by_name(name);
        if (c < 0) { kprintf("[--] mixer: %s names no channel '%s'\n", MIXER_PATH, name); continue; }

        int v = 0, digits = 0;
        while (*val >= '0' && *val <= '9') { v = v * 10 + (*val++ - '0'); digits++; }
        if (!digits) continue;
        mixer_set_volume(c, v);
        /* set_volume clears mute, so the flag is read after it and not before. */
        if (strstr(val, "muted")) ch[c].mute = 1;
    }
    return 1;
}

void mixer_init(void) {
    mixer_reset();
    mixer_load();
}
