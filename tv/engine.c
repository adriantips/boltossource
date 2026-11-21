/* ===========================================================================
 *  BoltOS TV  -  tv/engine.c
 *
 *  Netflix, Prime Video, Disney+ and HBO Max only play inside a real
 *  browser with a licensed DRM module (Widevine), and no small operating
 *  system gets one. So they play in a real Firefox on another computer on
 *  the network -- the BoltOS Web Engine, webengine/boltweb.py, the one
 *  BoltOS Mini's Browser uses -- and this is its other end: the page comes
 *  here as H.264 and its sound as PCM, and the pointer, the keys and the
 *  wheel go back.
 *
 *  One TCP connection, port 7310, messages of a type byte, a 32-bit length
 *  and a body (webengine/boltweb.py has the list). The connection is held
 *  by a kernel thread of its own, which reads it, queues the frames, writes
 *  the sound into the audio ring, and sends what the shell has queued. The
 *  shell decodes (mini/h264.c, the decoder YouTube uses) and draws.
 * ===========================================================================*/
#include <stdint.h>
#include "tv.h"
#include "engine.h"
#include "h264.h"
#include "web.h"
#include "net.h"
#include "audio.h"
#include "sched.h"
#include "sysupdate.h"
#include "io.h"
#include "kheap.h"
#include "kprintf.h"
#include "timeout.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define DEFAULT_PORT 7310
#define PROTO        2
#define QN           6
#define FRAME_MAX    (2u << 20)
#define SMALL_CAP    (20 * 1024)
#define OUT_CAP      4096
#define AUDIO_MAX_MS 220                    /* more queued than this: drop, to stay in step */

enum { C_HELLO = 0x01, C_NAV, C_BACK, C_FORWARD, C_RELOAD, C_STOP,
       C_MOUSE = 0x10, C_WHEEL, C_KEY, C_ACK = 0x20, C_KEYFRAME };
enum { S_HELLO = 0x81, S_CONFIG, S_FRAME, S_STATE, S_CURSOR, S_ERROR, S_AUDIO };
enum { HELLO_SOUND = 1 };

static char cfg_addr[96];
static int  cfg_loaded;

/* ---- what the thread and the shell share -------------------------------- */
static struct {
    volatile int state;
    char     msg[160];
    volatile int quit, alive;
    int      w, h;
    char     start_url[512];
    /* the frames: the thread fills [wr], the shell decodes [rd] */
    uint8_t *buf[QN]; uint32_t cap[QN], len[QN], no[QN];
    volatile uint32_t wr, rd;
    uint8_t *cfgbuf; volatile int cfglen, cfg_new;   /* a new avcC, for the shell */
    volatile uint32_t cfg_at;                         /* frames before this are the old stream's */
    /* the page */
    char     url[1024], title[200];
    volatile int loading, back, fwd, page_new;
    char     version[48];
    /* the pointer's picture */
    uint32_t *cur; int cw, ch, chx, chy; volatile uint32_t cgen;
    /* outgoing, from the shell */
    uint8_t  out[OUT_CAP]; volatile int on;
    /* numbers */
    uint32_t frames, dropped, audio_ms_dropped;
    uint64_t bytes;
} E;

/* the shell's */
static h264 *dec;
static int   dec_err, key_asked;
static surface_t page;
static int   have_pic;
static uint32_t shown, decoded;
static uint64_t dec_us;

/* ---- settings ------------------------------------------------------------ */
static void load_cfg(void) {
    if (cfg_loaded || !sysupd_prefs_loaded()) return;
    cfg_loaded = 1;
    char p[512];
    sysupd_prefs_get(p, sizeof p);
    const char *a = strstr(p, "engine=");
    if (a) { int k = 0; for (a += 7; *a && *a != '\n' && k < (int)sizeof cfg_addr - 1; a++) cfg_addr[k++] = *a; cfg_addr[k] = 0; }
}

void engine_get_address(char *out, int cap) {
    load_cfg();
    snprintf(out, (size_t)cap, "%s", cfg_addr);
}

/* "host", "host:port", or "key@host:port" */
void engine_set_address(const char *s) {
    load_cfg();
    while (*s == ' ') s++;
    snprintf(cfg_addr, sizeof cfg_addr, "%s", s);
    int n = (int)strlen(cfg_addr);
    while (n && cfg_addr[n - 1] == ' ') cfg_addr[--n] = 0;
    char p[512], keep[512] = "";
    sysupd_prefs_get(p, sizeof p);
    /* every other line as it was */
    for (char *line = p; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (strncmp(line, "engine=", 7) && line[0]) { strncat(keep, line, sizeof keep - strlen(keep) - 2); strcat(keep, "\n"); }
        line = nl ? nl + 1 : 0;
    }
    if (cfg_addr[0]) snprintf(p, sizeof p, "%sengine=%s\n", keep, cfg_addr);
    else snprintf(p, sizeof p, "%s", keep);
    sysupd_prefs_set(p);
    engine_stop();
}

int engine_configured(void) { load_cfg(); return cfg_addr[0] != 0; }

/* ---- sending (shell side) --------------------------------------------------- */
static void put(int type, const void *p, int n) {
    uint64_t f = irq_save();
    if (E.state == ENG_LIVE && E.on + 5 + n <= OUT_CAP) {
        uint8_t *o = E.out + E.on;
        o[0] = (uint8_t)type; o[1] = (uint8_t)n; o[2] = (uint8_t)(n >> 8); o[3] = (uint8_t)(n >> 16); o[4] = (uint8_t)(n >> 24);
        if (n) memcpy(o + 5, p, (size_t)n);
        E.on += 5 + n;
    }
    irq_restore(f);
}

/* ---- the thread ------------------------------------------------------------- */
static void say(int state, const char *fmt, const char *a) {
    E.state = state;
    snprintf(E.msg, sizeof E.msg, fmt, a ? a : "");
    E.page_new = 1;
    if (state == ENG_ERROR) kprintf("[engine] %s\n", E.msg);
}

static struct { uint8_t hdr[5]; int hn; int type; uint32_t len, have; uint8_t *dst; int slot; uint8_t *small; uint8_t head[5]; } R;
static int lost;                 /* a frame was dropped: skip to the next keyframe */

static void begin_msg(void) {
    R.type = R.hdr[0];
    R.len = (uint32_t)R.hdr[1] | (uint32_t)R.hdr[2] << 8 | (uint32_t)R.hdr[3] << 16 | (uint32_t)R.hdr[4] << 24;
    R.have = 0; R.dst = 0; R.slot = -1;
    if (R.type == S_FRAME) {
        uint32_t wr = E.wr;
        if (R.len > FRAME_MAX + 5 || wr - E.rd >= QN) { E.dropped++; return; }   /* the shell is behind: skip */
        int k = (int)(wr % QN);
        if (E.cap[k] < R.len) {
            if (E.buf[k]) kfree(E.buf[k]);
            E.cap[k] = R.len + 64 * 1024;
            E.buf[k] = kmalloc(E.cap[k]);
            if (!E.buf[k]) { E.cap[k] = 0; E.dropped++; return; }
        }
        R.dst = E.buf[k]; R.slot = k;
    } else if (R.len <= SMALL_CAP) R.dst = R.small;
}

static void got_state(const uint8_t *p, int n) {
    if (n < 4) return;
    E.loading = p[0]; E.back = p[1]; E.fwd = p[2];
    const char *s = (const char *)p + 4;
    int ul = 0;
    while (4 + ul < n && s[ul]) ul++;
    utf8_to_cp1252(s, ul, E.url, sizeof E.url);
    int tl = n - 4 - ul - 1;
    if (tl > 0) utf8_to_cp1252(s + ul + 1, tl, E.title, sizeof E.title); else E.title[0] = 0;
    E.page_new = 1;
}

static void got_cursor(const uint8_t *p, int n) {
    if (n < 8) return;
    int w = p[0] | p[1] << 8, h = p[2] | p[3] << 8;
    if (w < 1 || h < 1 || w > 64 || h > 64 || n < 8 + w * h * 4) return;
    if (!E.cur && !(E.cur = kmalloc(64 * 64 * 4))) return;
    memcpy(E.cur, p + 8, (size_t)w * h * 4);
    /* premultiply, for gfx_image */
    for (int i = 0; i < w * h; i++) {
        uint32_t c = E.cur[i], a = c >> 24;
        E.cur[i] = a << 24 | ((c >> 16 & 255) * a / 255) << 16 | ((c >> 8 & 255) * a / 255) << 8 | (c & 255) * a / 255;
    }
    E.cw = w; E.ch = h; E.chx = p[4] | p[5] << 8; E.chy = p[6] | p[7] << 8;
    E.cgen++;
}

static int sound_on;
static void got_audio(const uint8_t *p, int n) {
    if (!sound_on) { if (audio_ring_open() != 0) return; sound_on = 1; }
    uint32_t frames = (uint32_t)n / 4;
    /* behind by more than a fifth of a second: drop this block, so the sound
     * catches up with the picture rather than trailing it forever */
    if (audio_ring_queued() > AUDIO_RATE * AUDIO_MAX_MS / 1000) { E.audio_ms_dropped += frames * 1000 / AUDIO_RATE; return; }
    audio_ring_write((const int16_t *)p, frames);
}

static void end_msg(void) {
    uint8_t *p = R.dst;
    int n = (int)R.len;
    switch (R.type) {
    case S_HELLO:
        if (!p || n < 4) break;
        snprintf(E.version, sizeof E.version, "%.*s", n - 4 > 40 ? 40 : n - 4, (const char *)p + 4);
        say(ENG_LIVE, "", 0);
        kprintf("[engine] live: %s at %dx%d\n", E.version, E.w, E.h);
        break;
    case S_CONFIG:
        if (!p || n > 512) break;
        if (!E.cfgbuf) E.cfgbuf = kmalloc(512);
        if (E.cfgbuf) { memcpy(E.cfgbuf, p, (size_t)n); E.cfglen = n; E.cfg_at = E.wr; E.cfg_new = 1; }
        break;
    case S_FRAME: {
        if (n < 5) break;
        const uint8_t *h = p ? p : R.head;
        uint32_t no = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
        if (!p || (lost && !h[4])) {
            /* Dropped (the shell is behind), or a picture that needs one that was:
             * acknowledged, so the engine keeps sending, and a keyframe asked for. */
            uint8_t a[4] = { h[0], h[1], h[2], h[3] };
            put(C_ACK, a, 4);
            if (!lost) put(C_KEYFRAME, 0, 0);
            lost = 1;
            break;
        }
        lost = 0;
        E.len[R.slot] = (uint32_t)n - 5;
        E.no[R.slot] = no;
        __atomic_store_n(&E.wr, E.wr + 1, __ATOMIC_RELEASE);
        E.frames++; E.bytes += (uint64_t)n;
        break;
    }
    case S_STATE:  if (p) got_state(p, n); break;
    case S_CURSOR: if (p) got_cursor(p, n); break;
    case S_AUDIO:  if (p) got_audio(p, n); break;
    case S_ERROR: {
        char why[160] = "The engine hung up";
        if (p && n) utf8_to_cp1252((const char *)p, n, why, sizeof why);
        say(ENG_ERROR, "%s", why);
        E.quit = 1;
        break;
    }
    }
}

static void feed(const uint8_t *p, int n) {
    while (n > 0 && !E.quit) {
        if (R.hn < 5) {
            R.hdr[R.hn++] = *p++; n--;
            if (R.hn == 5) { begin_msg(); if (!R.len) { end_msg(); R.hn = 0; } }
            continue;
        }
        uint32_t take = R.len - R.have;
        if (take > (uint32_t)n) take = (uint32_t)n;
        if (R.dst) memcpy(R.dst + R.have, p, take);
        for (uint32_t i = 0; R.have + i < 5 && i < take; i++) R.head[R.have + i] = p[i];   /* a dropped frame's number */
        R.have += take; p += take; n -= (int)take;
        if (R.have == R.len) { end_msg(); R.hn = 0; }
    }
}

static void thread_main(void) {
    char host[96];
    snprintf(host, sizeof host, "%s", cfg_addr);
    char *at = strchr(host, '@');
    char key[48] = "";
    if (at) { *at = 0; snprintf(key, sizeof key, "%s", host); memmove(host, at + 1, strlen(at + 1) + 1); }
    int port = DEFAULT_PORT;
    char *colon = strrchr(host, ':');
    if (colon) { port = atoi(colon + 1); *colon = 0; }
    struct tcp_conn *c = 0;
    netaddr na;
    if (netsvc_state() != NET_UP) say(ENG_ERROR, "No network: plug in Ethernet", 0);
    else if (!dns_resolve_addr(host, &na, 3000)) say(ENG_ERROR, "The engine's address (%s) wasn't found", host);
    else if (!(c = tcp_connect_addr(&na, (uint16_t)port, 5000))) say(ENG_ERROR, "Firefox at %s doesn't answer", cfg_addr);
    else {
        uint8_t hello[5 + 8 + 48];
        int kl = (int)strlen(key), n = 8 + kl;
        uint8_t *h = hello + 5;
        hello[0] = C_HELLO; hello[1] = (uint8_t)n; hello[2] = hello[3] = hello[4] = 0;
        h[0] = PROTO; h[1] = 0;
        h[2] = (uint8_t)E.w; h[3] = (uint8_t)(E.w >> 8);
        h[4] = (uint8_t)E.h; h[5] = (uint8_t)(E.h >> 8);
        h[6] = audio_pcm_ok() ? HELLO_SOUND : 0; h[7] = 0;
        memcpy(h + 8, key, (size_t)kl);
        say(ENG_CONNECTING, "Starting Firefox\x85", 0);
        if (tcp_send(c, hello, (uint32_t)(5 + n)) < 0) say(ENG_ERROR, "Firefox at %s turned the connection down", cfg_addr);
    }
    int sent_nav = 0;
    static uint8_t buf[16384];
    while (c && !E.quit && E.state != ENG_ERROR) {
        int n = tcp_recv(c, buf, sizeof buf, 4);
        if (n > 0) { feed(buf, n); continue; }
        if (n == 0 || !tcp_is_open(c)) { say(ENG_ERROR, "Firefox at %s hung up", cfg_addr); break; }
        if (E.state == ENG_LIVE && !sent_nav && E.start_url[0]) {
            sent_nav = 1;
            int l = (int)strlen(E.start_url);
            uint8_t m[5 + 512];
            m[0] = C_NAV; m[1] = (uint8_t)l; m[2] = (uint8_t)(l >> 8); m[3] = m[4] = 0;
            memcpy(m + 5, E.start_url, (size_t)l);
            tcp_send(c, m, (uint32_t)(5 + l));
        }
        /* what the shell queued */
        uint8_t out[OUT_CAP]; int on;
        uint64_t f = irq_save();
        on = E.on; memcpy(out, E.out, (size_t)on); E.on = 0;
        irq_restore(f);
        if (on && tcp_send(c, out, (uint32_t)on) < 0) { say(ENG_ERROR, "Firefox at %s hung up", cfg_addr); break; }
    }
    if (c) tcp_close(c);
    if (sound_on) { audio_ring_close(); sound_on = 0; }
    E.alive = 0;
    sched_exit_current();
    for (;;) __asm__ volatile("hlt");
}

/* ---- the shell's side --------------------------------------------------------- */
int engine_start(const char *url, int w, int h) {
    load_cfg();
    if (!cfg_addr[0]) return -1;
    if (E.alive) {                                  /* already up: just go there */
        engine_go(url);
        return 0;
    }
    if (!R.small && !(R.small = kmalloc(SMALL_CAP))) return -1;
    R.hn = 0; lost = 0;
    E.quit = 0; E.wr = E.rd = 0; E.on = 0; E.cfg_new = 0;
    E.w = w & ~1; E.h = h & ~1;
    E.url[0] = E.title[0] = 0; E.back = E.fwd = 0;
    snprintf(E.start_url, sizeof E.start_url, "%s", url ? url : "");
    have_pic = 0; dec_err = 0; key_asked = 0;
    if (!page.px || page.w != E.w || page.h != E.h) {
        if (page.px) kfree(page.px);
        page.px = kmalloc((uint64_t)E.w * E.h * 4);
        if (!page.px) { say(ENG_ERROR, "Out of memory for the page", 0); return -1; }
        page.w = E.w; page.h = E.h; page.stride = E.w;
    }
    char m[160]; snprintf(m, sizeof m, "Connecting to Firefox at %s\x85", cfg_addr);
    say(ENG_CONNECTING, "%s", m);
    E.alive = 1;
    if (sched_add(thread_main, "engine") < 0) { E.alive = 0; say(ENG_ERROR, "No thread for the engine", 0); return -1; }
    return 0;
}

void engine_stop(void) {
    if (E.alive) {
        E.quit = 1;
        uint64_t end = tv_now_ms() + 3000;
        while (E.alive && tv_now_ms() < end) __asm__ volatile("hlt");
    }
    if (dec) { h264_close(dec); dec = 0; }
    if (E.state != ENG_ERROR) E.state = ENG_OFF;
    have_pic = 0;
}

int engine_state(void) { return E.state; }
const char *engine_message(void) { return E.msg; }

/* BT.601, limited range: what x264 is fed. */
static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void to_page(const h264_pic *p) {
    int w = p->w < page.w ? p->w : page.w, h = p->h < page.h ? p->h : page.h;
    for (int y = 0; y + 1 < h; y += 2) {
        const uint8_t *y0 = p->y + y * p->stride, *y1 = y0 + p->stride;
        const uint8_t *u = p->u + (y / 2) * p->cstride, *v = p->v + (y / 2) * p->cstride;
        uint32_t *o0 = page.px + y * page.stride, *o1 = o0 + page.stride;
        for (int x = 0; x + 1 < w; x += 2) {
            int d = u[x / 2] - 128, e = v[x / 2] - 128;
            int rr = 409 * e + 128, gg = -100 * d - 208 * e + 128, bb = 516 * d + 128, c;
#define PX(o, yy) c = 298 * ((yy) - 16); o = 0xFF000000u | (uint32_t)clamp8((c + rr) >> 8) << 16 | (uint32_t)clamp8((c + gg) >> 8) << 8 | (uint32_t)clamp8((c + bb) >> 8)
            PX(o0[x], y0[x]); PX(o0[x + 1], y0[x + 1]); PX(o1[x], y1[x]); PX(o1[x + 1], y1[x + 1]);
#undef PX
        }
    }
}

int engine_tick(void) {
    int r = 0;
    if (E.page_new) { E.page_new = 0; r = 1; }
    if (E.state != ENG_LIVE) return r;
    if (E.cfg_new) {
        E.cfg_new = 0;
        if ((int32_t)(E.cfg_at - E.rd) > 0) E.rd = E.cfg_at;   /* queued before it: the old stream's */
        if (dec) h264_close(dec);
        dec = h264_open(E.cfgbuf, E.cfglen);
        dec_err = !dec;
        key_asked = 0;
        if (!dec) kprintf("[engine] the stream's parameters can't be decoded here\n");
    }
    /* decode what's queued, newest picture shown; at most ~25 ms of it a turn */
    uint64_t t0 = now_us();
    while (E.rd != __atomic_load_n(&E.wr, __ATOMIC_ACQUIRE) && now_us() - t0 < 25000) {
        int k = (int)(E.rd % QN);
        uint32_t no = E.no[k];
        if (dec && !dec_err) {
            uint64_t d0 = now_us();
            if (h264_decode(dec, E.buf[k] + 5, (int)E.len[k], no) < 0) dec_err = 1;
            else {
                h264_pic pic;
                int got = 0;
                while (h264_output(dec, &pic)) if (!got++) { to_page(&pic); have_pic = 1; r = 1; shown++; }
            }
            dec_us += now_us() - d0; decoded++;
        }
        __atomic_store_n(&E.rd, E.rd + 1, __ATOMIC_RELEASE);
        uint8_t a[4] = { (uint8_t)no, (uint8_t)(no >> 8), (uint8_t)(no >> 16), (uint8_t)(no >> 24) };
        put(C_ACK, a, 4);
    }
    if (dec_err && !key_asked) { put(C_KEYFRAME, 0, 0); key_asked = 1; }
    return r;
}

const surface_t *engine_picture(void) { return E.state == ENG_LIVE && have_pic ? &page : 0; }

void engine_page(char *url, int ucap, char *title, int tcap, int *loading, int *back, int *fwd) {
    uint64_t f = irq_save();
    if (url) snprintf(url, (size_t)ucap, "%s", E.url);
    if (title) snprintf(title, (size_t)tcap, "%s", E.title);
    irq_restore(f);
    if (loading) *loading = E.loading;
    if (back) *back = E.back;
    if (fwd) *fwd = E.fwd;
}

void engine_go(const char *url) {
    if (!url) return;
    snprintf(E.start_url, sizeof E.start_url, "%s", url);
    put(C_NAV, url, (int)strlen(url));
}
void engine_back(void)    { put(C_BACK, 0, 0); }
void engine_reload(void)  { put(C_RELOAD, 0, 0); }

void engine_mouse(int x, int y, int buttons) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= E.w) x = E.w - 1;
    if (y >= E.h) y = E.h - 1;
    uint8_t m[5] = { (uint8_t)x, (uint8_t)(x >> 8), (uint8_t)y, (uint8_t)(y >> 8), (uint8_t)buttons };
    put(C_MOUSE, m, 5);
}
void engine_wheel(int dy) { int8_t w[2] = { (int8_t)dy, 0 }; put(C_WHEEL, w, 2); }
void engine_keysym(uint32_t sym, int mods) {
    uint8_t k[5] = { (uint8_t)sym, (uint8_t)(sym >> 8), (uint8_t)(sym >> 16), (uint8_t)(sym >> 24), (uint8_t)(mods & 15) };
    put(C_KEY, k, 5);
}

const uint32_t *engine_cursor(int *w, int *h, int *hx, int *hy) {
    *w = E.cw; *h = E.ch; *hx = E.chx; *hy = E.chy;
    return E.state == ENG_LIVE && E.cw ? E.cur : 0;
}

void engine_stats(char *out, int cap) {
    snprintf(out, (size_t)cap, "%s, %u frames in, %u shown, %u dropped, %lu us a frame",
             E.version, E.frames, shown, E.dropped, (unsigned long)(decoded ? dec_us / decoded : 0));
}
