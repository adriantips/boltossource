/* ===========================================================================
 *  BoltOS  -  kernel/app_media.c
 *  Media: the player for sound that came off a disk.
 *
 *  `play FILE` proved the decoder works, but it plays a track the way a test
 *  does -- it holds the thread that called it until the last sample is gone. A
 *  media player is the opposite: it has to keep a codec fed while the machine
 *  gets on with everything else, and let you stop, seek and skip in the middle
 *  of doing it.
 *
 *  So the streaming does not live in the window. media_pump() is called at the
 *  top of the compositor's main loop, for two reasons. The first is that a
 *  window's tick() runs at 6.6 Hz, and a codec that has drained its buffer and
 *  is waiting up to 150 ms for the next one is a codec you can hear waiting;
 *  the main loop wakes on every timer interrupt, so the handover costs about a
 *  millisecond and the HDA driver's trailing block of silence covers it. The
 *  second is that music you can only hear while its window has focus is not
 *  music anybody would put on. Being above every early-out in that loop, it
 *  keeps playing while the window is minimised or on another desktop, while the
 *  session is locked, and while a ring-3 program owns the panel.
 *
 *  Half a second of audio is handed over at a time, decoded straight out of
 *  the file's own bytes (the decoders convert as they read), so a track is
 *  never held twice and a seek is a move of a cursor rather than a re-decode.
 *
 *  That half second is not decoded in one go, though, and MP3 is why. A WAV
 *  costs a multiply a sample and could be converted in a single pass of the
 *  loop without anyone noticing; an MPEG frame costs a Huffman decode, an
 *  IMDCT and a polyphase synthesis, and twenty of them in one pass is a
 *  dropped frame of compositing you can see. So the buffer is filled one
 *  decoder step at a time -- about 26 ms of audio per pass of a loop that runs
 *  every millisecond -- while the codec is still playing the last one, and
 *  handed over whole when it drains.
 *
 *  The level meter is drawn from the samples that were actually submitted, not
 *  from a random number generator: each chunk is measured in sixteen slices on
 *  the way to the device, and the display walks through them at the rate the
 *  device is consuming them. A silent passage shows silence.
 *
 *  One codec, one stream: starting a file stops the Music app's synthesiser and
 *  vice versa, because the alternative is two programs fighting over one DMA
 *  engine and both sounding broken.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "audio.h"
#include "mixer.h"
#include "sound.h"
#include "fs.h"
#include "users.h"
#include "kheap.h"
#include "keyboard.h"
#include "string.h"
#include "pit.h"

#define CHUNK_MS      500u
#define CHUNK_FRAMES  (AUDIO_RATE / 1000u * CHUNK_MS)   /* 24000 frames = 96 KiB */
#define SLICES        16                                /* level readings per chunk */
#define PEAKS         256                               /* ~8 s of level history    */
#define PLMAX         64
#define SEEK_STEP_MS  5000u

static window_t *win;

/* ---- the open track ------------------------------------------------------ */
static snd_t    trk;                  /* WAV or MP3; sound.c decides which      */
static char     name[FS_NAME_MAX];
static int      have;                 /* a track is open and decodable          */
static int      playing;
static int      repeat = 1;
static const char *problem = "";      /* why the last open or play did not work */

/* ---- the chunk in flight ------------------------------------------------- */
static int16_t *chunk;
static uint32_t chunk_frames;
static uint32_t chunk_pos_ms;         /* track position of its first sample     */
static uint64_t chunk_tick;           /* pit_ticks() when it was handed over    */
static uint32_t chunk_slice0;         /* peak_head before its slices went in    */
static uint32_t fill;                 /* frames staged but not yet handed over  */
static uint32_t fill_pos_ms;          /* track position of the staged buffer    */
static int      drained;              /* the source has no more to give         */

/* ---- level history ------------------------------------------------------- */
static uint8_t  peaks[PEAKS];
static uint32_t peak_head;            /* monotonic; the slot is head % PEAKS    */

/* ---- the playlist -------------------------------------------------------- */
static char pl_dir[FS_PATH_MAX] = "/usr/share/sounds";
static char pl[PLMAX][FS_NAME_MAX];
static int  pl_n, pl_cur = -1, pl_scroll;
static int  picked;                   /* the start folder has been chosen once */

static int load_index(int i);

/* ---- helpers ------------------------------------------------------------- */
static void basename_of(const char *path, char *out, uint32_t cap) {
    const char *b = path;
    for (const char *s = path; *s; s++) if (*s == '/') b = s + 1;
    strlcpy(out, b, cap);
}

static void join(const char *dir, const char *leaf, char *out, uint32_t cap) {
    out[0] = 0;
    strlcat(out, dir, cap);
    uint32_t l = (uint32_t)strlen(out);
    if (l && out[l - 1] != '/') strlcat(out, "/", cap);
    strlcat(out, leaf, cap);
}

/* "m:ss", or "h:mm:ss" for something long enough to need it. */
static void mmss(uint32_t ms, char *out) {
    uint32_t s = ms / 1000, m = s / 60, h = m / 60;
    int n = 0;
    s %= 60; m %= 60;
    if (h) {
        out[n++] = (char)('0' + h % 10);
        out[n++] = ':';
        out[n++] = (char)('0' + m / 10);
    } else if (m >= 10) {
        out[n++] = (char)('0' + m / 10);
    }
    out[n++] = (char)('0' + m % 10);
    out[n++] = ':';
    out[n++] = (char)('0' + s / 10);
    out[n++] = (char)('0' + s % 10);
    out[n] = 0;
}

static void utoa_small(uint32_t v, char *out) {
    char tmp[12];
    int n = 0;
    if (!v) { out[0] = '0'; out[1] = 0; return; }
    while (v && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

/* ---- the stream ---------------------------------------------------------- */

/* Everything the device is doing on our behalf, undone. The position in the
 * track is deliberately left alone: this is what pause is built out of. */
static void release_device(void) {
    audio_stop();
    chunk_frames = 0;
    /* Whatever was staged was decoded for a position we are about to leave --
     * every caller of this either pauses (and rewinds to what was audible) or
     * seeks. Keeping it would play half a second of the old place first. */
    fill = 0;
    drained = 0;
}

static void drop_track(void) {
    playing = 0;
    have = 0;
    release_device();
    snd_close(&trk);
    name[0] = 0;
    /* The level history is deliberately not cleared. It is a rolling window of
     * what came out of the speakers, and the speakers do not stop between one
     * track and the next -- blanking it at every track change would leave the
     * meter empty for a second each time, which is a display of the software's
     * bookkeeping rather than of the sound. */
}

/* Measure the chunk on its way past, in SLICES equal parts, so the meter shows
 * the samples the codec is playing rather than an animation that happens to be
 * running at the same time. */
static void measure(const int16_t *pcm, uint32_t frames) {
    if (!frames) return;
    uint32_t per = frames / SLICES;
    if (!per) per = frames;
    for (uint32_t s = 0; s < SLICES; s++) {
        uint32_t a = s * per, b = a + per;
        if (b > frames) b = frames;
        int32_t hi = 0;
        for (uint32_t i = a; i < b; i++) {
            int32_t v = pcm[i * 2];
            if (v < 0) v = -v;
            if (v > hi) hi = v;
        }
        peaks[peak_head % PEAKS] = (uint8_t)(hi >> 7);
        peak_head++;
        if (b >= frames) {          /* a short final chunk: leave the rest quiet */
            for (uint32_t k = s + 1; k < SLICES; k++) { peaks[peak_head % PEAKS] = 0; peak_head++; }
            return;
        }
    }
}

/* One decoder step into the staging buffer -- bounded work, whatever the
 * format. Called on every pass of the compositor's loop while the codec is
 * busy, which is how half a second gets decoded without any one pass doing
 * more than a frame of it. */
static void decode_ahead(void) {
    if (!have || !chunk || drained || fill >= CHUNK_FRAMES) return;
    if (!fill) fill_pos_ms = snd_position_ms(&trk);
    uint32_t got = snd_step(&trk, chunk + (uint64_t)fill * 2, CHUNK_FRAMES - fill);
    if (!got) drained = 1;
    else      fill += got;
}

/* Hand the staged audio to the codec. Returns 0 when there was none. */
static int push_chunk(void) {
    if (!have || !chunk || !fill) return 0;
    chunk_slice0 = peak_head;
    measure(chunk, fill);
    chunk_frames = fill;
    chunk_pos_ms = fill_pos_ms;
    chunk_tick = pit_ticks();
    audio_play_async_ch(MIX_MEDIA, chunk, fill);
    fill = 0;
    return 1;
}

/* Fill the buffer now and hand it over. For the moments where waiting for the
 * loop to trickle it in would be heard as a gap: opening a track, resuming,
 * and landing a seek. */
static int prime_chunk(void) {
    if (!have || !chunk) return 0;
    while (fill < CHUNK_FRAMES && !drained) decode_ahead();
    return push_chunk();
}

/* The end of a track: the next one, the top of the list, or silence. */
static void track_finished(void) {
    release_device();
    if (pl_n > 1 && pl_cur >= 0 && pl_cur + 1 < pl_n) { load_index(pl_cur + 1); return; }
    if (repeat) {
        if (pl_n > 1) { load_index(0); return; }
        snd_seek_ms(&trk, 0);
        drained = 0;
        if (prime_chunk()) return;
    }
    playing = 0;
    if (have) snd_seek_ms(&trk, 0);
}

void media_pump(void) {
    if (!playing) return;
    /* Closing the window closes the app. Music is fed from its tick(), which
     * the compositor stops calling for a closed window, so it stops on its own;
     * this is fed from the main loop, which does not care, and would otherwise
     * play on out of a window that is not there any more. */
    if (win && !win->open) { media_halt(); return; }
    if (!audio_pcm_ok()) { playing = 0; problem = "no PCM audio device"; return; }
    if (audio_busy()) {
        decode_ahead();          /* the next buffer, a frame at a time */
        /* A stream engine that never reports itself drained would otherwise
         * park the track forever, silently. Wait the length of the buffer it
         * was given plus half a second, then move on regardless -- the same
         * deadline audio_play_pcm() uses, for the same reason. */
        uint32_t span = chunk_frames / (AUDIO_RATE / 1000u);
        if (pit_ticks() - chunk_tick < (uint64_t)span + 500u) return;
    }
    if (!push_chunk() && !prime_chunk()) track_finished();
}

/* Hand the codec back -- to the Music app, or to a blocking `play`. Cheap when
 * nothing is streaming, because it is called before every buffer such a caller
 * submits. */
void media_halt(void) {
    if (!playing) return;
    playing = 0;
    release_device();
}

/* ---- opening files ------------------------------------------------------- */

/* The folder a track came out of is its playlist. Names are kept in order, so
 * Next means the next file you can see in File Explorer rather than the next
 * node the filesystem happens to have linked. */
static void scan_dir(const char *dir) {
    pl_n = 0; pl_cur = -1; pl_scroll = 0;   /* an index into the old list means nothing here */
    strlcpy(pl_dir, dir, sizeof pl_dir);
    fs_node *d = fs_lookup(dir);
    if (!d || !d->is_dir) return;
    for (fs_node *c = d->child; c && pl_n < PLMAX; c = c->next) {
        if (c->is_dir || !snd_is_audio_name(c->name)) continue;
        int at = pl_n;
        while (at > 0 && strcmp(pl[at - 1], c->name) > 0) {
            strlcpy(pl[at], pl[at - 1], FS_NAME_MAX);
            at--;
        }
        strlcpy(pl[at], c->name, FS_NAME_MAX);
        pl_n++;
    }
}

static int open_file(const char *path) {
    drop_track();
    problem = "";
    basename_of(path, name, sizeof name);

    const char *err = "cannot open";
    if (snd_open(path, &trk, &err) != 0) { problem = err; return -1; }

    if (!chunk) {
        chunk = (int16_t *)kmalloc((uint64_t)CHUNK_FRAMES * 4);
        if (!chunk) { snd_close(&trk); problem = "out of memory"; return -1; }
    }
    have = 1;
    if (!audio_pcm_ok()) problem = "no PCM audio device";
    return 0;
}

static int load_index(int i) {
    if (i < 0 || i >= pl_n) return -1;
    char path[FS_PATH_MAX];
    join(pl_dir, pl[i], path, sizeof path);
    pl_cur = i;
    if (open_file(path) != 0) { playing = 0; return -1; }
    music_halt();                        /* one codec, one stream */
    playing = audio_pcm_ok();
    if (playing && !prime_chunk()) playing = 0;
    return 0;
}

static void open_path(const char *path, int focus) {
    if (!path || !path[0]) return;
    picked = 1;                       /* they chose a folder; do not second-guess it */
    char dir[FS_PATH_MAX], leaf[FS_NAME_MAX];
    basename_of(path, leaf, sizeof leaf);
    strlcpy(dir, path, sizeof dir);
    int cut = (int)strlen(dir) - (int)strlen(leaf) - 1;
    if (cut <= 0) strlcpy(dir, "/", sizeof dir);
    else          dir[cut] = 0;

    scan_dir(dir);
    int idx = -1;
    for (int i = 0; i < pl_n; i++) if (strcmp(pl[i], leaf) == 0) { idx = i; break; }

    if (idx >= 0) {
        load_index(idx);
    } else {
        /* Not a name the folder scan turned up -- an unusual extension, or a
         * path in a directory we cannot list. Play it anyway, on its own. */
        if (open_file(path) == 0) {
            pl_n = 0; pl_cur = -1;
            music_halt();
            playing = audio_pcm_ok();
            if (playing && !prime_chunk()) playing = 0;
        }
    }
    if (win) { if (focus) gui_open(win); else gui_show_unfocused(win); }
    gui_request_redraw();
}

/* Opened from the desktop -- a double-click in File Explorer -- so the player
 * comes to the front and takes the keyboard, which is what was asked for. */
void media_open_path(const char *path) { open_path(path, 1); }

/* Opened from a prompt. The window appears, but the caret stays in the shell
 * that is still waiting for your next line. */
void media_open_path_bg(const char *path) { open_path(path, 0); }

/* ---- transport ----------------------------------------------------------- */
void media_toggle(void) {
    if (!have) { if (pl_n > 0) load_index(pl_cur >= 0 ? pl_cur : 0); return; }
    if (playing) {
        /* Rewind the decoder to what was actually audible before letting go of
         * the device. It runs up to half a second ahead; without this the seek
         * bar jumps forward on pause and resume skips what you just heard. */
        uint32_t audible = media_position_ms();
        playing = 0;
        release_device();
        snd_seek_ms(&trk, audible);
        return;
    }
    if (!audio_pcm_ok()) { problem = "no PCM audio device"; return; }
    music_halt();
    playing = 1;
    if (snd_done(&trk)) { snd_seek_ms(&trk, 0); drained = 0; }
    if (!prime_chunk()) playing = 0;
}

void media_stop(void) {
    playing = 0;
    release_device();
    if (have) snd_seek_ms(&trk, 0);
}

int media_skip(int delta) {
    if (pl_n <= 0) return -1;
    int i = pl_cur < 0 ? 0 : pl_cur + delta;
    if (i < 0) i = pl_n - 1;
    if (i >= pl_n) i = 0;
    return load_index(i);
}

int media_select(int i) { return load_index(i); }

void media_seek_ms(uint32_t ms) {
    if (!have) return;
    uint32_t dur = snd_duration_ms(&trk);
    if (ms > dur) ms = dur;
    snd_seek_ms(&trk, ms);
    /* Drop what the device is holding: it is the old position, and playing it
     * out first is exactly the lag that makes a seek bar feel broken. */
    release_device();
    if (playing && !prime_chunk()) playing = 0;
    gui_request_redraw();
}

/* Where the sound is, not where the decoder is: the decode cursor runs up to
 * one chunk ahead of what has actually reached the speakers. */
uint32_t media_position_ms(void) {
    if (!have) return 0;
    if (!chunk_frames) return snd_position_ms(&trk);
    uint32_t span = chunk_frames / (AUDIO_RATE / 1000u);
    uint64_t el = pit_ticks() - chunk_tick;
    if (el > span) el = span;
    return chunk_pos_ms + (uint32_t)el;
}

uint32_t    media_duration_ms(void) { return have ? snd_duration_ms(&trk) : 0; }
int         media_playing(void)     { return playing; }
int         media_loaded(void)      { return have; }
const char *media_track(void)       { return have ? name : ""; }
/* What the open file turned out to be, in the decoder's own words. There
 * is more than one kind of sound file now, so "which one is this" is a
 * question a player has to be able to answer. */
const char *media_format(void)      { return have ? snd_describe(&trk) : ""; }
const char *media_status(void)      { return problem; }
const char *media_folder(void)      { return pl_dir; }
int         media_repeat(void)      { return repeat; }
void        media_set_repeat(int on){ repeat = on ? 1 : 0; }
int         media_index(void)       { return pl_cur; }

int media_list(int i, const char **out) {
    if (out && i >= 0 && i < pl_n) *out = pl[i];
    return pl_n;
}

/* ===========================================================================
 *  The window
 * ===========================================================================*/
enum { H_PLAY = 1, H_STOP, H_PREV, H_NEXT, H_REPEAT, H_MUTE, H_SEEK, H_VOL, H_TRACK0 };

typedef struct { int x, y, w, h, id; } hot_t;
static hot_t hots[PLMAX + 12];
static int   nhot, ox, oy;
static int   grab;                    /* H_SEEK / H_VOL while a drag is in flight */
static int   list_y, list_rows = 1;
static const int row_h = 24;

static void reg(int x, int y, int w, int h, int id) {
    if (nhot < (int)(sizeof hots / sizeof hots[0])) {
        hots[nhot].x = x - ox; hots[nhot].y = y - oy;
        hots[nhot].w = w;      hots[nhot].h = h;
        hots[nhot].id = id;
        nhot++;
    }
}

static int button(int x, int y, const char *label, int id, uint32_t bg, uint32_t fg) {
    int w = g_text_width(label, 1) + 20, h = 26;
    g_round(x, y, w, h, 6, bg, 255);
    g_text(x + 10, y + (h - 8) / 2, label, fg, 1);
    reg(x, y, w, h, id);
    return w;
}

static void draw_meter(int x, int y, int w, int h) {
    g_fill(x, y, w, h, 0x12121A);
    int bw = 4, gap = 2, n = w / (bw + gap);
    if (n > PEAKS) n = PEAKS;

    /* Which slice is audible *now*: the chunk's own slices, walked at the rate
     * the device is consuming them. */
    uint32_t cur = peak_head;
    if (playing && chunk_frames) {
        uint32_t span = chunk_frames / (AUDIO_RATE / 1000u);
        if (!span) span = 1;
        uint64_t el = pit_ticks() - chunk_tick;
        if (el > span) el = span;
        cur = chunk_slice0 + (uint32_t)(el * SLICES / span);
        if (cur > peak_head) cur = peak_head;
    }

    for (int i = 0; i < n; i++) {
        int32_t slot = (int32_t)cur - (n - i);
        int lvl = 0;
        if (slot >= 0 && (uint32_t)slot + PEAKS > peak_head) lvl = peaks[(uint32_t)slot % PEAKS];
        int bh = lvl * (h - 4) / 255;
        int bx = x + i * (bw + gap);
        if (bh < 2) { g_fill(bx, y + h - 3, bw, 1, 0x2A2A3A); continue; }
        g_fill(bx, y + h - 2 - bh, bw, bh, lvl > 200 ? COL_WARN : COL_ACCENT);
    }
}

static void draw_slider(int x, int y, int w, int pct, int id, uint32_t fill) {
    g_round(x, y, w, 6, 3, 0x33333F, 255);
    if (pct > 0) g_round(x, y, w * pct / 100, 6, 3, fill, 255);
    g_round(x + w * pct / 100 - 4, y - 4, 9, 14, 4, COL_TEXT, 255);
    reg(x - 6, y - 8, w + 12, 22, id);
}

static void media_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    nhot = 0; ox = cx; oy = cy;
    g_fill(cx, cy, cw, ch, 0x0E0E16);

    int y = cy + 12;
    g_text(cx + 16, y, have ? name : "Nothing playing", COL_TEXT, 2);
    y += 26;

    char sub[96]; sub[0] = 0;
    if (!have && problem[0]) {
        g_text(cx + 16, y, problem, COL_BAD, 1);
    } else if (have) {
        /* Whatever the file turned out to be, in its own words -- the two
         * formats have nothing in common to tabulate (an MP3 has no bit depth
         * and a WAV has no bitrate), so each names itself. */
        strlcat(sub, snd_describe(&trk), sizeof sub);
        strlcat(sub, "  -  ", sizeof sub);
        strlcat(sub, problem[0] ? problem : audio_name(), sizeof sub);
        g_text(cx + 16, y, sub, problem[0] ? COL_WARN : COL_TEXT_DIM, 1);
    } else {
        g_text(cx + 16, y, "Open a .wav or .mp3 from File Explorer, or pick one below", COL_TEXT_DIM, 1);
    }
    y += 22;

    draw_meter(cx + 16, y, cw - 32, 56);
    y += 66;

    /* seek bar + times */
    uint32_t pos = media_position_ms(), dur = media_duration_ms();
    int pct = dur ? (int)((uint64_t)pos * 100 / dur) : 0;
    draw_slider(cx + 16, y + 6, cw - 32, pct, H_SEEK, COL_ACCENT);
    char tl[12], tr[12];
    mmss(pos, tl); mmss(dur, tr);
    g_text(cx + 16, y + 20, tl, COL_TEXT_DIM, 1);
    g_text(cx + cw - 16 - g_text_width(tr, 1), y + 20, tr, COL_TEXT_DIM, 1);
    y += 40;

    /* transport */
    int bx = cx + 16;
    bx += button(bx, y, "|<", H_PREV, COL_PANEL_3, COL_TEXT) + 6;
    bx += button(bx, y, playing ? "Pause" : "Play", H_PLAY, COL_ACCENT, 0xFFFFFF) + 6;
    bx += button(bx, y, "Stop", H_STOP, COL_PANEL_3, COL_TEXT) + 6;
    bx += button(bx, y, ">|", H_NEXT, COL_PANEL_3, COL_TEXT) + 6;
    button(bx, y, "Repeat", H_REPEAT, repeat ? COL_ACCENT_DIM : COL_PANEL_3,
           repeat ? 0xFFFFFF : COL_TEXT_DIM);
    y += 34;

    /* volume: the system's, so this and the tray slider are one control */
    int vol = audio_volume();
    g_text(cx + 16, y + 3, "Vol", COL_TEXT_DIM, 1);
    reg(cx + 12, y - 2, 34, 20, H_MUTE);
    draw_slider(cx + 52, y + 6, cw - 52 - 60, vol, H_VOL, COL_ACCENT_DIM);
    char pc[8];
    utoa_small((uint32_t)vol, pc);
    strlcat(pc, "%", sizeof pc);
    g_text(cx + cw - 16 - g_text_width(pc, 1), y + 3, pc, COL_TEXT_DIM, 1);
    y += 28;

    /* playlist */
    g_hline(cx + 16, y, cw - 32, 0x24242E);
    y += 8;
    g_text(cx + 16, y, pl_dir, COL_TEXT_DIM, 1);
    y += 18;

    list_y = y;
    list_rows = (cy + ch - 8 - y) / row_h;
    if (list_rows < 1) list_rows = 1;
    if (pl_cur >= 0) {                            /* keep the playing track visible */
        if (pl_cur < pl_scroll) pl_scroll = pl_cur;
        if (pl_cur >= pl_scroll + list_rows) pl_scroll = pl_cur - list_rows + 1;
    }
    if (pl_scroll > pl_n - list_rows) pl_scroll = pl_n - list_rows;
    if (pl_scroll < 0) pl_scroll = 0;

    if (pl_n == 0)
        g_text(cx + 16, y + 4, "No .wav files in this folder", COL_TEXT_DIM, 1);

    for (int i = pl_scroll; i < pl_n && i < pl_scroll + list_rows; i++) {
        int ry = list_y + (i - pl_scroll) * row_h;
        int sel = (i == pl_cur);
        if (sel) g_round(cx + 12, ry, cw - 24, row_h - 2, 5, 0x223046, 255);
        if (sel && playing) g_text(cx + 20, ry + (row_h - 10) / 2, ">", COL_ACCENT, 1);
        g_text(cx + 34, ry + (row_h - 10) / 2, pl[i], sel ? COL_ACCENT : COL_TEXT, 1);
        reg(cx + 12, ry, cw - 24, row_h - 2, H_TRACK0 + i);
    }
}

/* ---- input --------------------------------------------------------------- */
static void set_from_slider(int id, int lx) {
    for (int i = 0; i < nhot; i++) {
        if (hots[i].id != id) continue;
        int inner_x = hots[i].x + 6, inner_w = hots[i].w - 12;
        if (inner_w < 1) inner_w = 1;
        int pct = (lx - inner_x) * 100 / inner_w;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        if (id == H_SEEK) media_seek_ms((uint32_t)((uint64_t)media_duration_ms() * pct / 100));
        else              audio_set_volume(pct);
        return;
    }
}

static void media_click(window_t *w, int lx, int ly) {
    (void)w;
    grab = 0;
    for (int i = 0; i < nhot; i++) {
        hot_t *h = &hots[i];
        if (lx < h->x || lx >= h->x + h->w || ly < h->y || ly >= h->y + h->h) continue;
        switch (h->id) {
        case H_PLAY:   media_toggle(); break;
        case H_STOP:   media_stop(); break;
        case H_PREV:   media_skip(-1); break;
        case H_NEXT:   media_skip(1); break;
        case H_REPEAT: repeat = !repeat; break;
        case H_MUTE:   mixer_toggle_mute(MIX_MASTER); audio_gain_changed(); break;
        case H_SEEK:   grab = H_SEEK; set_from_slider(H_SEEK, lx); break;
        case H_VOL:    grab = H_VOL;  set_from_slider(H_VOL, lx); break;
        default:       if (h->id >= H_TRACK0) media_select(h->id - H_TRACK0); break;
        }
        gui_request_redraw();
        return;
    }
}

/* A drag only moves the control the press landed on, so pulling the seek knob
 * off the end of its bar and across the volume slider does not also change the
 * volume. Every drag stream begins with a click, which is where grab is set. */
static void media_drag(window_t *w, int lx, int ly) {
    (void)w; (void)ly;
    if (grab) { set_from_slider(grab, lx); gui_request_redraw(); }
}

static void media_scroll(window_t *w, int delta) {
    (void)w;
    pl_scroll -= delta;
    if (pl_scroll > pl_n - list_rows) pl_scroll = pl_n - list_rows;
    if (pl_scroll < 0) pl_scroll = 0;
    gui_request_redraw();
}

static void media_key(window_t *w, char c) {
    (void)w;
    uint32_t pos = media_position_ms();
    switch (c) {
    case ' ': media_toggle(); break;
    case 'n': case 'N': media_skip(1); break;
    case 'p': case 'P': media_skip(-1); break;
    case 's': case 'S': media_stop(); break;
    case 'r': case 'R': repeat = !repeat; break;
    case 'm': case 'M': mixer_toggle_mute(MIX_MASTER); audio_gain_changed(); break;
    case KEY_RIGHT: media_seek_ms(pos + SEEK_STEP_MS); break;
    case KEY_LEFT:  media_seek_ms(pos > SEEK_STEP_MS ? pos - SEEK_STEP_MS : 0); break;
    case KEY_UP:    audio_set_volume(audio_volume() + 5); break;
    case KEY_DOWN:  audio_set_volume(audio_volume() - 5); break;
    default: return;
    }
    gui_request_redraw();
}

/* Where to look the first time somebody opens the player. The app is built
 * before anyone has signed in, so it cannot know whose Music folder to show
 * until it is opened -- and an empty Music folder is a worse first impression
 * than the two sounds the system ships, so it only wins if it has something in
 * it. */
static void pick_start_folder(void) {
    char p[FS_PATH_MAX];
    strlcpy(p, users_home(), sizeof p);
    strlcat(p, "/Music", sizeof p);
    scan_dir(p);
    if (pl_n == 0) scan_dir("/usr/share/sounds");
}

/* The window only has to keep the clock and the meter moving; the audio is fed
 * from the main loop whether this window is on screen or not. */
static void media_tick(window_t *w) {
    (void)w;
    if (!picked && !have) { picked = 1; pick_start_folder(); gui_request_redraw(); }
    if (playing) gui_request_redraw();
}

void media_app_init(void) {
    scan_dir(pl_dir);
    win = gui_add_window("Media", 480, 520, 0x5AC8FA, ICON_MEDIA);
    if (!win) return;
    win->draw   = media_draw;
    win->click  = media_click;
    win->drag   = media_drag;
    win->scroll = media_scroll;
    win->key    = media_key;
    win->tick   = media_tick;
    win->min_w  = 380; win->min_h = 400;
    win->x = 260; win->y = 70;
}
