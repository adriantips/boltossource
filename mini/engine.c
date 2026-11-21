/* ===========================================================================
 *  BoltOS Mini  -  mini/engine.c
 *
 *  The web as Firefox draws it. A page today is megabytes of JavaScript
 *  written for a JIT, CSS as Firefox and Chrome do it, and video through
 *  Media Source Extensions -- a browser engine's worth, and more than a
 *  1 GHz A53 with a gigabyte will ever run well. So a real Firefox runs on
 *  a bigger machine (webengine/: a Linux VM on the Mac, set up to look like
 *  Firefox on Windows), and this is its other end: the page arrives as
 *  H.264, and the pointer, the wheel and the keys go back.
 *
 *  One TCP connection, port 7310, messages of a type byte, a 32-bit length
 *  and a body (webengine/boltweb.py has the list). This core reads it in
 *  the main loop: the page's address and title, Firefox's pointer, and the
 *  frames, which go into a queue for the decoding core.
 *
 *  Core 1 decodes (mini/h264.c, the same decoder YouTube uses) and compares
 *  each picture with the one before in 16x16 tiles: only the tiles that
 *  changed are turned into RGB, and the box around them is what this core
 *  copies to the screen and VNC sends on. A caret blinking costs one tile;
 *  a video playing in the page costs its own rectangle. The engine sends
 *  nothing while the page stands still, and a frame is acknowledged once
 *  it is on screen, so the engine never has more than two on the way: a
 *  slow moment here means fewer frames, never older ones.
 *
 *  The heap is this core's alone, so everything core 1 touches is
 *  allocated here first -- the queue, the pictures, the decoder's buffers
 *  (h264_prealloc) -- and the decoder is opened and closed here, with core
 *  1 paused, when the stream's parameter sets change.
 * ===========================================================================*/
#include <stdint.h>
#include "ui.h"
#include "engine.h"
#include "h264.h"
#include "web.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define DEFAULT_PORT 7310
#define QN         6                       /* frames in, not yet decoded       */
#define FRAME_MAX  (2u << 20)
#define SMALL_CAP  (20 * 1024)              /* everything that isn't a frame    */
#define OUT_CAP    4096
#define TILE       16

enum { C_HELLO = 0x01, C_NAV, C_BACK, C_FORWARD, C_RELOAD, C_STOP,
       C_MOUSE = 0x10, C_WHEEL, C_KEY, C_ACK = 0x20, C_KEYFRAME };
enum { S_HELLO = 0x81, S_CONFIG, S_FRAME, S_STATE, S_CURSOR, S_ERROR };

typedef struct { char addr[96]; char key[48]; int on; } cfg_t;
static cfg_t cfg;
static int cfg_loaded;

static struct {
    int      state;
    char     msg[160];
    tcb_t   *c;
    uint64_t t0, failed_at;
    int      w, h;                          /* the page asked for               */
    int      sent_hello;
    /* reading */
    uint8_t  hdr[5]; int hn;
    int      mtype; uint32_t mlen, mhave;
    uint8_t *mdst; int mslot;
    uint8_t *small;
    /* writing */
    uint8_t  out[OUT_CAP]; int on;
    /* the page */
    char     url[1024], title[200];
    int      loading, back, fwd;
    char     version[48];
    surface_t page;                         /* what is on screen: this core's   */
    int      have_pic, changed;
    /* the pointer */
    int      px, py, pb, moved;
    uint64_t moved_at;
    uint32_t *cur; int cw, ch, chx, chy; uint32_t cgen;
    int      key_asked;
    /* numbers */
    uint32_t frames, shown, dropped;
    uint64_t bytes;
} E;

/* What the decoding core shares. Each counter one side writes is on its own
 * cache line (see player.c: an atomic on a line the other core writes never
 * finishes). */
static struct {
    uint8_t *buf[QN]; uint32_t cap[QN], len[QN], no[QN];
    volatile uint32_t wr __attribute__((aligned(64)));      /* this core        */
    volatile uint32_t rd __attribute__((aligned(64)));      /* core 1           */
} Q;

static struct {
    volatile int run, quit, started, solo, err;
    volatile uint32_t req  __attribute__((aligned(64)));    /* pause: this core */
    volatile uint32_t ack  __attribute__((aligned(64)));    /*        core 1    */
    volatile uint32_t pub  __attribute__((aligned(64)));    /* pictures: core 1 */
    volatile uint32_t took __attribute__((aligned(64)));    /*   this core      */
    uint8_t  pad_ __attribute__((aligned(64)));
    int      dx, dy, dw, dh; uint32_t pic_no; /* the picture published         */
    h264    *dec;
    surface_t rgb;                            /* core 1's, written while took == pub */
    uint8_t *py, *pu, *pv; int have_prev;     /* the last picture, as decoded   */
    uint64_t dec_us; uint32_t decoded;
} W;

/* ---- settings ---------------------------------------------------------------- */
static void load_cfg(void) {
    if (cfg_loaded) return;
    cfg_loaded = 1;
    if (keep_get("engine", &cfg, (int)sizeof cfg) != (int)sizeof cfg) memset(&cfg, 0, sizeof cfg);
    cfg.addr[sizeof cfg.addr - 1] = 0; cfg.key[sizeof cfg.key - 1] = 0;
}

int engine_enabled(void) { load_cfg(); return cfg.on && cfg.addr[0]; }

void engine_configure(const char *addr, const char *key, int on) {
    load_cfg();
    if (addr) snprintf(cfg.addr, sizeof cfg.addr, "%s", addr);
    if (key) snprintf(cfg.key, sizeof cfg.key, "%s", key);
    cfg.on = on;
    keep_put("engine", &cfg, (int)sizeof cfg);
    engine_stop();
}

void engine_get_config(char *addr, int acap, char *key, int kcap, int *on) {
    load_cfg();
    if (addr) snprintf(addr, (size_t)acap, "%s", cfg.addr);
    if (key) snprintf(key, (size_t)kcap, "%s", cfg.key);
    if (on) *on = cfg.on;
}

/* ---- sending ------------------------------------------------------------------ */
static void put(int type, const void *p, int n) {
    if (!E.c || E.on + 5 + n > OUT_CAP) return;
    uint8_t *o = E.out + E.on;
    o[0] = (uint8_t)type; o[1] = (uint8_t)n; o[2] = (uint8_t)(n >> 8); o[3] = (uint8_t)(n >> 16); o[4] = (uint8_t)(n >> 24);
    if (n) memcpy(o + 5, p, (size_t)n);
    E.on += 5 + n;
}

static void flush(void) {
    if (!E.c || !E.on) return;
    int n = tcp_write(E.c, E.out, E.on);
    if (n > 0) { memmove(E.out, E.out + n, (size_t)(E.on - n)); E.on -= n; }
}

static void put_mouse(void) {
    uint8_t m[5] = { (uint8_t)E.px, (uint8_t)(E.px >> 8), (uint8_t)E.py, (uint8_t)(E.py >> 8), (uint8_t)E.pb };
    put(C_MOUSE, m, 5);
    E.moved = 0; E.moved_at = now_ms();
}

/* ---- the decoding core --------------------------------------------------------- */
static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static int same_tile(const h264_pic *p, int x, int y, int w, int h) {
    int s = W.rgb.w, cs = W.rgb.w / 2;
    for (int j = 0; j < h; j++)
        if (memcmp(p->y + (y + j) * p->stride + x, W.py + (y + j) * s + x, (size_t)w)) return 0;
    for (int j = 0; j < h / 2; j++) {
        if (memcmp(p->u + (y / 2 + j) * p->cstride + x / 2, W.pu + (y / 2 + j) * cs + x / 2, (size_t)w / 2)) return 0;
        if (memcmp(p->v + (y / 2 + j) * p->cstride + x / 2, W.pv + (y / 2 + j) * cs + x / 2, (size_t)w / 2)) return 0;
    }
    return 1;
}

/* BT.601, limited range: what x264 is fed (swscale's default). */
static void convert_tile(const h264_pic *p, int x, int y, int w, int h) {
    int s = W.rgb.w, cs = W.rgb.w / 2;
    for (int j = 0; j < h; j++) memcpy(W.py + (y + j) * s + x, p->y + (y + j) * p->stride + x, (size_t)w);
    for (int j = 0; j < h / 2; j++) {
        memcpy(W.pu + (y / 2 + j) * cs + x / 2, p->u + (y / 2 + j) * p->cstride + x / 2, (size_t)w / 2);
        memcpy(W.pv + (y / 2 + j) * cs + x / 2, p->v + (y / 2 + j) * p->cstride + x / 2, (size_t)w / 2);
    }
    for (int j = 0; j < h; j += 2) {
        const uint8_t *y0 = p->y + (y + j) * p->stride + x, *y1 = y0 + p->stride;
        const uint8_t *u = p->u + ((y + j) / 2) * p->cstride + x / 2, *v = p->v + ((y + j) / 2) * p->cstride + x / 2;
        uint32_t *o0 = W.rgb.px + (y + j) * W.rgb.stride + x, *o1 = o0 + W.rgb.stride;
        for (int i = 0; i < w; i += 2) {
            int d = u[i / 2] - 128, e = v[i / 2] - 128;
            int rr = 409 * e + 128, gg = -100 * d - 208 * e + 128, bb = 516 * d + 128;
            int c;
            c = 298 * (y0[i] - 16);     o0[i]     = 0xFF000000u | (uint32_t)clamp8((c + rr) >> 8) << 16 | (uint32_t)clamp8((c + gg) >> 8) << 8 | (uint32_t)clamp8((c + bb) >> 8);
            c = 298 * (y0[i + 1] - 16); o0[i + 1] = 0xFF000000u | (uint32_t)clamp8((c + rr) >> 8) << 16 | (uint32_t)clamp8((c + gg) >> 8) << 8 | (uint32_t)clamp8((c + bb) >> 8);
            c = 298 * (y1[i] - 16);     o1[i]     = 0xFF000000u | (uint32_t)clamp8((c + rr) >> 8) << 16 | (uint32_t)clamp8((c + gg) >> 8) << 8 | (uint32_t)clamp8((c + bb) >> 8);
            c = 298 * (y1[i + 1] - 16); o1[i + 1] = 0xFF000000u | (uint32_t)clamp8((c + rr) >> 8) << 16 | (uint32_t)clamp8((c + gg) >> 8) << 8 | (uint32_t)clamp8((c + bb) >> 8);
        }
    }
}

/* The picture into rgb, the tiles that changed only; the box around them is published. */
static void publish(const h264_pic *p) {
    int w = p->w < W.rgb.w ? p->w : W.rgb.w, h = p->h < W.rgb.h ? p->h : W.rgb.h;
    int x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (int ty = 0; ty < h; ty += TILE)
        for (int tx = 0; tx < w; tx += TILE) {
            int tw = w - tx < TILE ? w - tx : TILE, th = h - ty < TILE ? h - ty : TILE;
            if (W.have_prev && same_tile(p, tx, ty, tw, th)) continue;
            convert_tile(p, tx, ty, tw, th);
            if (tx < x0) x0 = tx;
            if (ty < y0) y0 = ty;
            if (tx + tw > x1) x1 = tx + tw;
            if (ty + th > y1) y1 = ty + th;
        }
    W.have_prev = 1;
    if (x1 > x0) { W.dx = x0; W.dy = y0; W.dw = x1 - x0; W.dh = y1 - y0; }
    else W.dw = W.dh = 0;
    W.pic_no = (uint32_t)p->pts;
    __atomic_store_n(&W.pub, W.pub + 1, __ATOMIC_RELEASE);
}

/* One frame from the queue, decoded and published. 0 if there's nothing to do
 * now (no frame, or the last picture isn't on screen yet). */
static int decode_step(void) {
    uint32_t rd = Q.rd, wr = __atomic_load_n(&Q.wr, __ATOMIC_ACQUIRE);
    if (rd == wr) return 0;
    if (__atomic_load_n(&W.took, __ATOMIC_ACQUIRE) != W.pub) return 0;
    int k = (int)(rd % QN);
    if (W.dec && !W.err && W.rgb.px) {
        uint64_t t0 = now_us();
        if (h264_decode(W.dec, Q.buf[k] + 5, (int)Q.len[k], Q.no[k]) < 0) W.err = 1;
        else {
            h264_pic pic;
            int got = 0;
            while (h264_output(W.dec, &pic)) if (!got++) publish(&pic);
        }
        W.dec_us += now_us() - t0;
        W.decoded++;
    }
    __atomic_store_n(&Q.rd, rd + 1, __ATOMIC_RELEASE);
    return 1;
}

static void worker(void *arg) {
    (void)arg;
    for (;;) {
        while (!__atomic_load_n(&W.run, __ATOMIC_ACQUIRE)) {
            if (__atomic_load_n(&W.quit, __ATOMIC_ACQUIRE)) return;    /* core 1 goes back to the pool */
            uint32_t q = __atomic_load_n(&W.req, __ATOMIC_ACQUIRE);
            if (__atomic_load_n(&W.ack, __ATOMIC_RELAXED) != q) __atomic_store_n(&W.ack, q, __ATOMIC_RELEASE);
            for (int i = 0; i < 64; i++) __asm__ volatile("yield");
        }
        if (!decode_step()) for (int i = 0; i < 500; i++) __asm__ volatile("yield");
    }
}

/* Stop core 1 between frames, so what it uses can be changed. */
static void pause_worker(void) {
    if (!W.started || W.solo) { W.run = 0; return; }
    __atomic_store_n(&W.run, 0, __ATOMIC_RELEASE);
    uint32_t r = W.req + 1;
    __atomic_store_n(&W.req, r, __ATOMIC_SEQ_CST);
    uint64_t end = now_ms() + 2000;
    while (__atomic_load_n(&W.ack, __ATOMIC_ACQUIRE) != r && now_ms() < end) __asm__ volatile("yield");
}
static void resume_worker(void) { __atomic_store_n(&W.run, 1, __ATOMIC_RELEASE); }

static void stop_worker(void) {
    pause_worker();
    if (W.started && !W.solo) {
        __atomic_store_n(&W.quit, 1, __ATOMIC_RELEASE);
        uint64_t end = now_ms() + 2000;
        while (core_busy(1) && now_ms() < end) __asm__ volatile("yield");
        W.quit = 0;
    }
    W.started = 0;
}

static void free_surface(surface_t *s) { if (s->px) kfree(s->px); memset(s, 0, sizeof *s); }
static int alloc_surface(surface_t *s, int w, int h) {
    free_surface(s);
    s->px = kmalloc((uint64_t)w * h * 4);
    if (!s->px) return -1;
    memset(s->px, 0xFF, (size_t)w * h * 4);
    s->w = w; s->h = h; s->stride = w;
    return 0;
}

/* The page's size is known: everything both cores need, allocated here. */
static int setup_pictures(int w, int h) {
    pause_worker();
    if (W.py) { kfree(W.py); W.py = 0; }
    int r = alloc_surface(&W.rgb, w, h) | alloc_surface(&E.page, w, h);
    W.py = kmalloc((uint64_t)w * h * 3 / 2);
    if (r || !W.py) { free_surface(&W.rgb); free_surface(&E.page); if (W.py) kfree(W.py); W.py = 0; return -1; }
    W.pu = W.py + w * h; W.pv = W.pu + w * h / 4;
    W.have_prev = 0;
    if (!W.started) {
        W.started = 1;
        W.solo = cpu_start(1, worker, 0) != 0;
        kprintf("[engine] decoding on %s\n", W.solo ? "this core (no second one)" : "the second core");
    }
    resume_worker();
    return 0;
}

/* New parameter sets come before every keyframe: a new decoder for them.
 * Frames still queued belong to the old stream. */
static void apply_config(const uint8_t *avcc, int len) {
    pause_worker();
    Q.rd = Q.wr;
    if (W.dec) h264_close(W.dec);
    W.dec = h264_open(avcc, len);
    W.err = 0;
    if (!W.dec || h264_prealloc(W.dec, FRAME_MAX, 4) < 0) {
        if (W.dec) h264_close(W.dec);
        W.dec = 0; W.err = 1;
        kprintf("[engine] the stream's parameters can't be decoded here\n");
    }
    W.have_prev = 0;
    E.key_asked = 0;
    resume_worker();
}

/* ---- the connection ------------------------------------------------------------- */
static void hang_up(void) {
    if (E.c) { tcp_abort(E.c); E.c = 0; }
    pause_worker();
    Q.rd = Q.wr;
    E.hn = 0; E.on = 0; E.mdst = 0; E.sent_hello = 0;
}

static void fail(const char *fmt, const char *a) {
    hang_up();
    E.state = ENG_ERROR;
    snprintf(E.msg, sizeof E.msg, fmt, a ? a : "");
    E.failed_at = now_ms();
    E.changed = 1;
    kprintf("[engine] %s\n", E.msg);
}

int engine_start(int w, int h) {
    load_cfg();
    w &= ~1; h &= ~1;
    if (!cfg.on || !cfg.addr[0]) { if (E.state != ENG_OFF) engine_stop(); return -1; }
    if (E.c && w == E.w && h == E.h) return 0;
    if (E.state == ENG_ERROR && w == E.w && h == E.h && now_ms() - E.failed_at < 4000) return -1;
    hang_up();
    E.w = w; E.h = h;
    if (!E.small && !(E.small = kmalloc(SMALL_CAP))) { fail("Out of memory", 0); return -1; }
    char host[96]; snprintf(host, sizeof host, "%s", cfg.addr);
    int port = DEFAULT_PORT;
    char *colon = strrchr(host, ':');
    if (colon) { port = atoi(colon + 1); *colon = 0; }
    int ok = 0;
    uint32_t ip = net_parse_ip(host, &ok);
    if (!ok) ip = inet_resolve(host, 3000);
    if (!ip) { fail("The engine's address (%s) wasn't found", host); return -1; }
    E.c = tcp_connect(ip, (uint16_t)port);
    if (!E.c) { fail("No network reaches the engine at %s", cfg.addr); return -1; }
    E.state = ENG_CONNECTING; E.t0 = now_ms();
    snprintf(E.msg, sizeof E.msg, "Connecting to Firefox at %s\x85", cfg.addr);
    E.changed = 1;
    return 0;
}

void engine_stop(void) {
    hang_up();
    stop_worker();
    if (W.dec) { h264_close(W.dec); W.dec = 0; }
    E.state = ENG_OFF; E.have_pic = 0; E.w = E.h = 0;
    E.msg[0] = 0;
}

int engine_state(void) { return E.state; }
const char *engine_message(void) { return E.msg; }

/* ---- reading -------------------------------------------------------------------- */
static void begin_msg(void) {
    E.mtype = E.hdr[0];
    E.mlen = (uint32_t)E.hdr[1] | (uint32_t)E.hdr[2] << 8 | (uint32_t)E.hdr[3] << 16 | (uint32_t)E.hdr[4] << 24;
    E.mhave = 0; E.mdst = 0; E.mslot = -1;
    if (E.mtype == S_FRAME) {
        uint32_t wr = Q.wr;
        if (E.mlen > FRAME_MAX + 5 || wr - __atomic_load_n(&Q.rd, __ATOMIC_ACQUIRE) >= QN) { E.dropped++; W.err = 1; return; }
        int k = (int)(wr % QN);                    /* not in [rd, wr): core 1 isn't using it */
        if (Q.cap[k] < E.mlen) {
            if (Q.buf[k]) kfree(Q.buf[k]);
            Q.cap[k] = E.mlen + 64 * 1024;
            Q.buf[k] = kmalloc(Q.cap[k]);
            if (!Q.buf[k]) { Q.cap[k] = 0; E.dropped++; return; }
        }
        E.mdst = Q.buf[k]; E.mslot = k;
    } else if (E.mlen <= SMALL_CAP) E.mdst = E.small;
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
    E.changed = 1;
}

static void got_cursor(const uint8_t *p, int n) {
    if (n < 8) return;
    int w = p[0] | p[1] << 8, h = p[2] | p[3] << 8;
    if (w < 1 || h < 1 || w > 64 || h > 64 || n < 8 + w * h * 4) return;
    if (!E.cur && !(E.cur = kmalloc(64 * 64 * 4))) return;
    memcpy(E.cur, p + 8, (size_t)w * h * 4);
    E.cw = w; E.ch = h; E.chx = p[4] | p[5] << 8; E.chy = p[6] | p[7] << 8;
    E.cgen++;
}

static void end_msg(void) {
    uint8_t *p = E.mdst;
    int n = (int)E.mlen;
    switch (E.mtype) {
    case S_HELLO:
        if (!p || n < 4) break;
        snprintf(E.version, sizeof E.version, "%.*s", n - 4 > 40 ? 40 : n - 4, (const char *)p + 4);
        if (setup_pictures(E.w, E.h) < 0) { fail("Out of memory for the page", 0); return; }
        E.state = ENG_LIVE; E.msg[0] = 0; E.changed = 1;
        kprintf("[engine] live: %s at %dx%d\n", E.version, E.w, E.h);
        break;
    case S_CONFIG:
        if (p) apply_config(p, n);
        break;
    case S_FRAME:
        if (!p || n < 5) break;
        Q.len[E.mslot] = (uint32_t)n - 5;
        Q.no[E.mslot] = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        __atomic_store_n(&Q.wr, Q.wr + 1, __ATOMIC_RELEASE);
        E.frames++; E.bytes += (uint64_t)n;
        break;
    case S_STATE:  if (p) got_state(p, n); break;
    case S_CURSOR: if (p) got_cursor(p, n); break;
    case S_ERROR: {
        char why[160] = "The engine hung up";
        if (p && n) utf8_to_cp1252((const char *)p, n, why, sizeof why);
        fail("%s", why);
        return;
    }
    }
}

static void feed(const uint8_t *p, int n) {
    while (n > 0 && E.c) {
        if (E.hn < 5) {
            E.hdr[E.hn++] = *p++; n--;
            if (E.hn == 5) { begin_msg(); if (!E.mlen) { end_msg(); E.hn = 0; } }
            continue;
        }
        uint32_t take = E.mlen - E.mhave;
        if (take > (uint32_t)n) take = (uint32_t)n;
        if (E.mdst) memcpy(E.mdst + E.mhave, p, take);
        E.mhave += take; p += take; n -= (int)take;
        if (E.mhave == E.mlen) { end_msg(); E.hn = 0; }
    }
}

/* ---- each turn -------------------------------------------------------------------- */
/* A picture core 1 published: onto the page, and acknowledged. */
static int take_picture(int *x, int *y, int *w, int *h) {
    uint32_t pub = __atomic_load_n(&W.pub, __ATOMIC_ACQUIRE);
    if (pub == W.took) return 0;
    int dx = W.dx, dy = W.dy, dw = W.dw, dh = W.dh;
    uint32_t no = W.pic_no;
    for (int j = 0; j < dh; j++)
        memcpy(E.page.px + (dy + j) * E.page.stride + dx, W.rgb.px + (dy + j) * W.rgb.stride + dx, (size_t)dw * 4);
    __atomic_store_n(&W.took, pub, __ATOMIC_RELEASE);
    uint8_t a[4] = { (uint8_t)no, (uint8_t)(no >> 8), (uint8_t)(no >> 16), (uint8_t)(no >> 24) };
    put(C_ACK, a, 4);
    E.shown++;
    if (!dw) return 0;
    int first = !E.have_pic;
    E.have_pic = 1;
    if (first) { E.changed = 1; return 0; }
    *x = dx; *y = dy; *w = dw; *h = dh;
    return 2;
}

int engine_tick(int *x, int *y, int *w, int *h) {
    int r = 0;
    if (E.c) {
        if (E.state == ENG_CONNECTING && !E.sent_hello) {
            if (tcp_connecting(E.c)) {
                if (now_ms() - E.t0 > 5000) fail("Firefox at %s doesn't answer", cfg.addr);
            } else if (!tcp_open(E.c)) fail("Firefox at %s turned the connection down", cfg.addr);
            else {
                uint8_t hello[8 + 48];
                int kl = (int)strlen(cfg.key);
                hello[0] = 1; hello[1] = 0;
                hello[2] = (uint8_t)E.w; hello[3] = (uint8_t)(E.w >> 8);
                hello[4] = (uint8_t)E.h; hello[5] = (uint8_t)(E.h >> 8);
                hello[6] = hello[7] = 0;
                memcpy(hello + 8, cfg.key, (size_t)kl);
                put(C_HELLO, hello, 8 + kl);
                E.sent_hello = 1;
                snprintf(E.msg, sizeof E.msg, "Starting Firefox\x85");
                E.changed = 1;
            }
        }
        if (E.c && E.sent_hello) {
            uint8_t buf[8192];
            for (int guard = 0; guard < 128 && E.c; guard++) {
                int n = tcp_read(E.c, buf, sizeof buf);
                if (n <= 0) break;
                feed(buf, n);
            }
            if (E.c && (!tcp_open(E.c) || tcp_peer_closed(E.c))) fail("Firefox at %s hung up", cfg.addr);
        }
        if (E.c && E.moved && now_ms() - E.moved_at >= 15) put_mouse();
        if (E.c && W.err && !E.key_asked && E.state == ENG_LIVE) { put(C_KEYFRAME, 0, 0); E.key_asked = 1; }
        if (W.solo && W.run) for (int i = 0; i < 2 && decode_step(); i++) { }
        if (E.state == ENG_LIVE) r = take_picture(x, y, w, h);
        flush();
    }
    if (E.changed) { E.changed = 0; return 1; }
    return r;
}

const surface_t *engine_picture(void) { return E.state == ENG_LIVE && E.have_pic ? &E.page : 0; }

void engine_page(char *url, int ucap, char *title, int tcap, int *loading, int *back, int *fwd) {
    if (url) snprintf(url, (size_t)ucap, "%s", E.url);
    if (title) snprintf(title, (size_t)tcap, "%s", E.title);
    if (loading) *loading = E.loading;
    if (back) *back = E.back;
    if (fwd) *fwd = E.fwd;
}

void engine_go(const char *url) {
    snprintf(E.url, sizeof E.url, "%s", url);
    E.title[0] = 0; E.loading = 1;
    put(C_NAV, url, (int)strlen(url));
    flush();
}
void engine_back(void)    { put(C_BACK, 0, 0); flush(); }
void engine_forward(void) { put(C_FORWARD, 0, 0); flush(); }
void engine_reload(void)  { put(C_RELOAD, 0, 0); flush(); }
void engine_halt(void)    { put(C_STOP, 0, 0); flush(); }

/* ---- input ----------------------------------------------------------------------- */
void engine_pointer(int x, int y, int buttons, int wheel) {
    if (E.state != ENG_LIVE) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= E.w) x = E.w - 1;
    if (y >= E.h) y = E.h - 1;
    int moved = x != E.px || y != E.py;
    E.px = x; E.py = y;
    if (buttons != E.pb || wheel) {
        E.pb = buttons;
        put_mouse();                          /* where the click or the wheel happens */
        if (wheel) { int8_t wv[2] = { (int8_t)wheel, 0 }; put(C_WHEEL, wv, 2); }
        flush();
    } else if (moved) E.moved = 1;
}

/* Mini's keys as X keysyms, which is what the engine types with. */
static uint32_t keysym(int key, int ch) {
    switch (key) {
    case KEY_CHAR:      return ch < 0x100 ? (uint32_t)ch : 0x01000000u | (uint32_t)ch;
    case KEY_ENTER:     return 0xFF0D;
    case KEY_ESC:       return 0xFF1B;
    case KEY_BACKSPACE: return 0xFF08;
    case KEY_TAB:       return 0xFF09;
    case KEY_DELETE:    return 0xFFFF;
    case KEY_INSERT:    return 0xFF63;
    case KEY_UP:        return 0xFF52;
    case KEY_DOWN:      return 0xFF54;
    case KEY_LEFT:      return 0xFF51;
    case KEY_RIGHT:     return 0xFF53;
    case KEY_HOME:      return 0xFF50;
    case KEY_END:       return 0xFF57;
    case KEY_PGUP:      return 0xFF55;
    case KEY_PGDN:      return 0xFF56;
    case KEY_SUPER:     return 0xFFEB;
    }
    if (key >= KEY_F1 && key <= KEY_F12) return 0xFFBE + (uint32_t)(key - KEY_F1);
    return 0;
}

void engine_key(int key, int ch, int mods) {
    if (E.state != ENG_LIVE) return;
    uint32_t s = keysym(key, ch);
    if (!s) return;
    uint8_t k[5] = { (uint8_t)s, (uint8_t)(s >> 8), (uint8_t)(s >> 16), (uint8_t)(s >> 24), (uint8_t)(mods & 15) };
    put(C_KEY, k, 5);
    flush();
}

const uint32_t *engine_cursor(int *w, int *h, int *hx, int *hy, uint32_t *gen) {
    *w = E.cw; *h = E.ch; *hx = E.chx; *hy = E.chy;
    if (gen) *gen = E.cgen;
    return E.state == ENG_LIVE && E.cw ? E.cur : 0;
}

/* ---- the dev page ------------------------------------------------------------------ */
static int qparam(const char *q, const char *name, char *out, int cap) {
    int n = (int)strlen(name);
    for (const char *p = q; p && *p; ) {
        if ((p == q || p[-1] == '?' || p[-1] == '&') && !strncmp(p, name, (size_t)n) && p[n] == '=') {
            const char *v = p + n + 1, *e = strchr(v, '&');
            url_decode(v, e ? (int)(e - v) : (int)strlen(v), out, cap);
            return 1;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return 0;
}

int engine_dev(const char *q, char *out, int cap) {
    char v[600], k[64];
    load_cfg();
    if (qparam(q, "set", v, sizeof v)) {
        int hk = qparam(q, "key", k, sizeof k);
        engine_configure(v, hk ? k : cfg.key, 1);
    } else if (strstr(q, "?on")) engine_configure(0, 0, 1);
    else if (strstr(q, "?off")) engine_configure(0, 0, 0);
    if (qparam(q, "go", v, sizeof v)) {
        void browser_open_url(const char *url);
        browser_open_url(v);
    }
    static const char *const names[] = { "error", "off", "connecting", "live" };
    int o = snprintf(out, (size_t)cap, "engine %s, %s (%s)%s%s\n", cfg.addr[0] ? cfg.addr : "(no address)",
                     cfg.on ? "on" : "off", names[E.state + 1], E.msg[0] ? ": " : "", E.msg);
    if (E.state == ENG_LIVE)
        o += snprintf(out + o, (size_t)(cap - o), "%s, page %dx%d\n%s\n%s%s\n", E.version, E.w, E.h, E.url, E.title,
                      E.loading ? " (loading)" : "");
    o += snprintf(out + o, (size_t)(cap - o), "frames %u in, %u shown, %u dropped, %lu KB; decoding %lu us each on %s\n",
                  E.frames, E.shown, E.dropped, (unsigned long)(E.bytes / 1024),
                  (unsigned long)(W.decoded ? W.dec_us / W.decoded : 0), W.solo ? "this core" : "core 1");
    return o;
}
