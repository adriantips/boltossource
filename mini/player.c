/* ===========================================================================
 *  BoltOS Mini  -  mini/player.c
 *
 *  YouTube, played on the Pi. A watch page asks YouTube's player API what
 *  the video is and where it lives, as YouTube's Android app does -- that
 *  client is handed plain stream URLs, with no signature to work out. Of
 *  what it offers, format 18 is the one to play: 360p H.264 with AAC in one
 *  MP4 whose index is at the front, so it streams.
 *
 *  The file arrives over one kept-alive TLS connection, a megabyte range at
 *  a time, into a 384 MiB window of RAM above the heap at the file's own
 *  offsets; a bitmap of 64 KiB blocks says what is there. mini/mp4.c reads
 *  the index and mini/h264.c decodes -- on the Pi's second core, which does
 *  nothing else, into a ring of RGB pictures; this core fetches and shows
 *  each picture when the video's clock reaches it. (With one core, as under
 *  QEMU, the decoding happens here, between frames.) If the next frame's
 *  bytes haven't come yet, the clock waits ("Buffering").
 *
 *  There is no sound: Mini has no audio output yet. The picture plays.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "web.h"
#include "gfx.h"
#include "mp4.h"
#include "h264.h"
#include "tls.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

#define YT_UA    "com.google.android.youtube/20.10.38 (Linux; U; Android 11) gzip"
#define BIG      ((uint8_t *)(uintptr_t)MINI_BIG_BASE)
#define BLOCK    (64u * 1024)
#define CHUNK    (1024u * 1024)
#define AHEAD    (48ull * 1024 * 1024)          /* stop fetching this far past playback */

enum { S_IDLE, S_LOADING, S_PLAYING, S_PAUSED, S_ERROR = -1 };

int bt_dns_resolve_addr(const char *host, netaddr *out, uint32_t timeout_ms);
void player_close(void);

static struct {
    int  state;
    char msg[160];
    char url[2048], host[160], path[1900];
    uint64_t file_len;
    uint8_t  have[(MINI_BIG_SIZE / BLOCK) / 8];
    /* the download */
    struct tls_conn *tc;
    int      in_body;                    /* reading a response's body                 */
    uint64_t req_at, req_end, body_at;   /* this request's range; the next byte to land */
    char     hdr[4096]; int hdr_len;
    uint64_t want;                       /* fetch from here on                         */
    uint64_t last_byte_ms;
    int      failures;
    /* the media */
    mp4_t    mp4; int have_mp4;
    h264    *dec;
    int      next;                       /* the next sample to decode                  */
    int64_t  first_pts;
    uint64_t clock0;                     /* now_ms() when video time 0 was             */
    uint64_t paused_ms;
    int      hide_before;                /* after a seek: decode, but don't show, earlier frames */
    int      shown;                      /* the ring slot on screen, -1 none           */
    int      buffering, at_end;
    int      len_ms, pos_ms;
    int      dropped, shown_n, last_sec;
    uint64_t stat_at;
} P;

/* The decoder's side, shared with the core it runs on. */
#define RING 5
static struct {
    volatile int run, solo, started, flushed, eos, err, buffering;
    volatile int next;                   /* the next sample to decode                 */
    volatile uint32_t req, ack;          /* pause handshake: ack catches up to req    */
    volatile uint32_t wr, rd;            /* the ring: [rd, wr) decoded, not yet shown */
    surface_t ring[RING];
    volatile int ring_ms[RING];
    volatile int decoded, skipped;
    volatile uint64_t dec_us;
} W;

/* Is this sample a picture nothing else refers to (nal_ref_idc 0)? Those can be
 * skipped when playback is behind. */
static int disposable(const uint8_t *s, int len) {
    int nl = (P.mp4.avcc[4] & 3) + 1, o = 0;
    while (o + nl < len) {
        int n = 0;
        for (int i = 0; i < nl; i++) n = n << 8 | s[o + i];
        o += nl;
        if (n <= 0 || o + n > len) return 0;
        int t = s[o] & 31;
        if (t == 1 || t == 5) return ((s[o] >> 5) & 3) == 0;
        o += n;
    }
    return 0;
}

static int have_range(uint64_t a, uint64_t b) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);              /* the bytes, after the bits that say so */
    if (b > P.file_len) b = P.file_len;
    for (uint64_t k = a / BLOCK; k * BLOCK < b; k++) if (!(P.have[k >> 3] >> (k & 7) & 1)) return 0;
    return 1;
}
/* The request's blocks that are complete: requests start on a block, and its bytes
 * arrive in order, so a block is whole once body_at is past its end (or at the file's). */
static void mark_arrived(void) {
    __atomic_thread_fence(__ATOMIC_RELEASE);              /* the bytes before the bits */
    for (uint64_t k = P.req_at / BLOCK; k * BLOCK < P.body_at; k++)
        if ((k + 1) * BLOCK <= P.body_at || P.body_at >= P.file_len) P.have[k >> 3] |= (uint8_t)(1 << (k & 7));
}

static void commas(long long v, char *out, int cap) {
    char d[32]; int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 30);
    int o = 0;
    for (int i = n - 1; i >= 0 && o < cap - 2; i--) { out[o++] = d[i]; if (i && i % 3 == 0) out[o++] = ','; }
    out[o] = 0;
}

static void fail(const char *why) {
    P.state = S_ERROR;
    snprintf(P.msg, sizeof P.msg, "%s", why);
    kprintf("[yt] %s\n", why);
}

/* ---- the download ----------------------------------------------------------------------- */
static void drop_conn(void) {
    if (P.tc) tls_close(P.tc);
    P.tc = 0; P.in_body = 0; P.hdr_len = 0;
}

static int connect_cdn(void) {
    netaddr a;
    if (!bt_dns_resolve_addr(P.host, &a, 3000)) return -1;
    P.tc = tls_connect_addr(&a, 443, P.host, 8000);
    return P.tc ? 0 : -1;
}

/* The next range to ask for: the first missing block from `want`, a megabyte of it. */
static int next_range(uint64_t *a, uint64_t *b) {
    uint64_t lim = P.file_len < MINI_BIG_SIZE ? P.file_len : MINI_BIG_SIZE;
    for (uint64_t k = P.want / BLOCK; k * BLOCK < lim; k++) {
        if (P.have[k >> 3] >> (k & 7) & 1) continue;
        if (k * BLOCK > P.want + AHEAD) return 0;
        *a = k * BLOCK;
        *b = *a + CHUNK;
        if (*b > lim) *b = lim;
        return 1;
    }
    return 0;
}

static int send_request(uint64_t a, uint64_t b) {
    char req[2300];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " YT_UA "\r\nRange: bytes=%llu-%llu\r\n"
                     "Connection: keep-alive\r\n\r\n", P.path, P.host, (unsigned long long)a, (unsigned long long)(b - 1));
    if (tls_send(P.tc, req, (uint32_t)n) < 0) return -1;
    P.req_at = a; P.req_end = b; P.body_at = a; P.in_body = 0; P.hdr_len = 0;
    P.last_byte_ms = now_ms();
    return 0;
}

/* The body's bytes land at their file offsets. */
static void take(const uint8_t *p, int n) {
    uint64_t room = P.req_end - P.body_at;
    if ((uint64_t)n > room) n = (int)room;
    if (P.body_at + (uint64_t)n <= MINI_BIG_SIZE) memcpy(BIG + P.body_at, p, (size_t)n);
    P.body_at += (uint64_t)n;
    mark_arrived();
}

/* A slice of the download: at most `ms` of it. 1 if anything arrived. */
static int dl_tick(int ms) {
    uint64_t end = now_ms() + (uint64_t)ms;
    int got = 0;
    while (now_ms() < end) {
        if (!P.tc || (!P.in_body && !P.hdr_len && P.body_at >= P.req_end)) {
            uint64_t a, b;
            if (!next_range(&a, &b)) return got;
            if (!P.tc && connect_cdn() < 0) {
                if (++P.failures > 4) fail("The video's server can't be reached");
                return got;
            }
            if (send_request(a, b) < 0) { drop_conn(); return got; }
        }
        uint8_t buf[16384];
        int n = tls_recv(P.tc, buf, sizeof buf, 1);
        if (n == 0) { drop_conn(); return got; }               /* closed: reconnect next time */
        if (n < 0) {
            if (now_ms() - P.last_byte_ms > 15000) { drop_conn(); if (++P.failures > 4) fail("The video stopped arriving"); }
            return got;
        }
        P.last_byte_ms = now_ms();
        got = 1;
        int i = 0;
        if (!P.in_body) {
            while (i < n && P.hdr_len < (int)sizeof P.hdr - 1) {
                P.hdr[P.hdr_len++] = (char)buf[i++];
                if (P.hdr_len >= 4 && !memcmp(P.hdr + P.hdr_len - 4, "\r\n\r\n", 4)) break;
            }
            if (P.hdr_len < 4 || memcmp(P.hdr + P.hdr_len - 4, "\r\n\r\n", 4)) continue;
            P.hdr[P.hdr_len] = 0;
            int status = 0;
            const char *sp = strchr(P.hdr, ' ');
            if (sp) status = (int)(sp[1] - '0') * 100 + (sp[2] - '0') * 10 + (sp[3] - '0');
            if (status != 206 && status != 200) {
                char w[80]; snprintf(w, sizeof w, "The video's server said %d", status);
                drop_conn(); fail(w); return got;
            }
            const char *cr = strcasestr(P.hdr, "Content-Range: bytes ");
            if (cr) {
                const char *sl = strchr(cr, '/');
                if (sl && sl[1] != '*') {
                    uint64_t t = 0;
                    for (const char *q = sl + 1; *q >= '0' && *q <= '9'; q++) t = t * 10 + (uint64_t)(*q - '0');
                    if (t) P.file_len = t;
                }
            }
            P.in_body = 1; P.hdr_len = 0;
        }
        if (i < n) take(buf + i, n - i);
        if (P.body_at >= P.req_end) { P.in_body = 0; P.failures = 0; }
    }
    return got;
}

/* Blocking: until [a, b) has arrived, or the time is up. */
static int fetch_now(uint64_t a, uint64_t b, uint32_t ms) {
    P.want = a;
    uint64_t end = now_ms() + ms;
    while (!have_range(a, b) && now_ms() < end && P.state != S_ERROR) { dl_tick(50); mini_pump(); }
    return have_range(a, b);
}

/* ---- pictures --------------------------------------------------------------------------------- */
static void to_rgb(const h264_pic *p, surface_t *s) {
    if (!s->px || s->w != p->w || s->h != p->h) {
        if (s->px) kfree(s->px);
        s->px = kmalloc((uint64_t)p->w * p->h * 4);
        s->w = p->w; s->h = p->h; s->stride = p->w;
        if (!s->px) return;
    }
    for (int y = 0; y < p->h; y += 2) {
        const uint8_t *y0 = p->y + y * p->stride, *y1 = y0 + p->stride;
        const uint8_t *u = p->u + (y / 2) * p->cstride, *v = p->v + (y / 2) * p->cstride;
        uint32_t *o0 = s->px + y * s->stride, *o1 = o0 + s->stride;
        for (int x = 0; x < p->w; x += 2) {
            int d = u[x / 2] - 128, e = v[x / 2] - 128;
            int rr = 409 * e + 128, gg = -100 * d - 208 * e + 128, bb = 516 * d + 128;
            const uint8_t *yy[4] = { y0 + x, y0 + x + 1, y1 + x, y1 + x + 1 };
            uint32_t *oo[4] = { o0 + x, o0 + x + 1, o1 + x, o1 + x + 1 };
            for (int k = 0; k < 4; k++) {
                if (k >= 2 && y + 1 >= p->h) break;
                int c = 298 * (*yy[k] - 16);
                int r = (c + rr) >> 8, g = (c + gg) >> 8, b = (c + bb) >> 8;
                r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
                *oo[k] = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
            }
        }
    }
}

static int sample_ms(const mp4_sample *s) {
    return (int)((s->pts - P.first_pts) * 1000 / (P.mp4.video.timescale ? P.mp4.video.timescale : 1000));
}

static int64_t video_now(void) {
    if (P.state == S_PAUSED || P.buffering) return P.paused_ms;
    return (int64_t)(now_ms() - P.clock0);
}

/* ---- decoding ----------------------------------------------------------------------------------- */
static int pic_ms(const h264_pic *p) { return (int)((p->pts - P.first_pts) * 1000 / P.mp4.video.timescale); }

/* One step of the decoder's work: a picture out into the ring, or a sample in.
 * 0 if there is nothing it can do now. Runs on the decoding core. */
static int decode_step(void) {
    if (W.wr - W.rd >= RING - 1) return 0;                   /* full: one slot is on screen */
    h264_pic pic;
    if (h264_output(P.dec, &pic)) {
        int ms = pic_ms(&pic);
        if (ms < P.hide_before) return 1;                    /* before a seek's target */
        uint32_t k = W.wr % RING;
        to_rgb(&pic, &W.ring[k]);
        W.ring_ms[k] = ms;
        __atomic_store_n(&W.wr, W.wr + 1, __ATOMIC_RELEASE);
        return 1;
    }
    if (W.eos || W.err) return 0;
    if (W.next >= P.mp4.video.n) {
        if (!W.flushed) { h264_flush(P.dec); W.flushed = 1; return 1; }
        W.eos = 1;
        return 0;
    }
    mp4_sample *s = &P.mp4.video.s[W.next];
    if (s->off + s->size > MINI_BIG_SIZE) { W.err = 2; return 0; }
    if (!have_range(s->off, s->off + s->size)) { W.buffering = 1; return 0; }
    W.buffering = 0;
    if (sample_ms(s) + 150 < video_now() && disposable(BIG + s->off, (int)s->size)) {
        W.next++; W.skipped++;                               /* behind: skip a picture nobody needs */
        return 1;
    }
    uint64_t t0 = now_us();
    int r = h264_decode(P.dec, BIG + s->off, (int)s->size, s->pts);
    W.dec_us += now_us() - t0;
    if (r < 0) { W.err = 1; return 0; }
    W.next++; W.decoded++;
    return 1;
}

static void worker(void *arg) {
    (void)arg;
    for (;;) {
        while (!__atomic_load_n(&W.run, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&W.ack, __atomic_load_n(&W.req, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
            __asm__ volatile("yield");
        }
        if (!decode_step()) for (int i = 0; i < 2000; i++) __asm__ volatile("yield");
    }
}

/* Stop the decoding core at a step boundary, so its state can be changed. */
static void pause_worker(void) {
    if (!W.started || W.solo) { W.run = 0; return; }
    __atomic_store_n(&W.run, 0, __ATOMIC_RELEASE);
    uint32_t r = __atomic_add_fetch(&W.req, 1, __ATOMIC_SEQ_CST);
    uint64_t end = now_ms() + 2000;
    while (__atomic_load_n(&W.ack, __ATOMIC_ACQUIRE) != r && now_ms() < end) __asm__ volatile("yield");
}
static void resume_worker(void) { __atomic_store_n(&W.run, 1, __ATOMIC_RELEASE); }

static void restart_at(int ms) {
    pause_worker();
    if (P.dec) h264_close(P.dec);
    P.dec = h264_open(P.mp4.avcc, P.mp4.avcc_len);
    int k = 0;
    for (int i = 0; i < P.mp4.video.n; i++)
        if (P.mp4.video.s[i].key && sample_ms(&P.mp4.video.s[i]) <= ms) k = i;
    W.next = k; W.flushed = 0; W.eos = 0; W.err = 0; W.buffering = 0;
    W.wr = W.rd = 0;
    P.shown = -1;
    P.hide_before = ms;
    P.clock0 = now_ms() - (uint64_t)ms;
    P.paused_ms = (uint64_t)ms;
    P.pos_ms = ms;
    P.want = P.mp4.video.s[k].off;
    P.at_end = 0;
    resume_worker();
}

/* ---- the API ---------------------------------------------------------------------------------- */
int player_open(const char *id, char *title, int title_cap, char *meta, int meta_cap) {
    player_close();
    P.state = S_LOADING;
    snprintf(P.msg, sizeof P.msg, "Asking YouTube for the video\x85");
    char body[600];
    int bl = snprintf(body, sizeof body,
        "{\"context\":{\"client\":{\"clientName\":\"ANDROID\",\"clientVersion\":\"20.10.38\","
        "\"androidSdkVersion\":30,\"osName\":\"Android\",\"osVersion\":\"11\",\"hl\":\"en\",\"gl\":\"US\"}},"
        "\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}", id);
    char *buf = kmalloc(1024 * 1024);
    if (!buf) { fail("Out of memory"); return -1; }
    int code = 0;
    int n = web_request("POST", "https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
                        "User-Agent: " YT_UA "\r\nContent-Type: application/json\r\n", body, bl, buf, 1024 * 1024, &code, 0, 0);
    jarena *ar = 0;
    jv *root = n > 0 ? json_parse(buf, n, &ar) : 0;
    if (!root) { kfree(buf); fail(n < 0 ? "YouTube didn't answer" : "YouTube's answer couldn't be read"); return -1; }
    jv *vd = json_get(root, "videoDetails");
    const char *t = json_s(json_get(vd, "title"));
    utf8_to_cp1252(t, (int)strlen(t), title, title_cap);
    char views[32]; commas(json_i(json_get(vd, "viewCount")), views, sizeof views);
    const char *author = json_s(json_get(vd, "author"));
    char au[120]; utf8_to_cp1252(author, (int)strlen(author), au, sizeof au);
    P.len_ms = (int)json_i(json_get(vd, "lengthSeconds")) * 1000;
    snprintf(meta, (size_t)meta_cap, "%s  \xB7  %s views", au, views);
    const char *status = json_s(json_path(root, "playabilityStatus.status"));
    if (strcmp(status, "OK")) {
        const char *why = json_s(json_path(root, "playabilityStatus.reason"));
        if (!why[0]) why = "YouTube won't play this one here";
        char w[160]; utf8_to_cp1252(why, (int)strlen(why), w, sizeof w);
        fail(w);
    } else {
        jv *fm = json_path(root, "streamingData.formats");
        for (jv *f = fm ? fm->kid : 0; f; f = f->next)
            if (json_i(json_get(f, "itag")) == 18) {
                snprintf(P.url, sizeof P.url, "%s", json_s(json_get(f, "url")));
                const char *cl = json_s(json_get(f, "contentLength"));
                for (; *cl >= '0' && *cl <= '9'; cl++) P.file_len = P.file_len * 10 + (uint64_t)(*cl - '0');
            }
        if (!P.url[0]) fail(json_get(json_get(root, "streamingData"), "hlsManifestUrl") ? "Live streams don't play here yet" :
                                                                                           "This video has no 360p stream");
    }
    kprintf("[yt] %s: %s, %d s, %s\n", id, status, P.len_ms / 1000, P.url[0] ? "format 18" : "no stream");
    json_free(ar);
    kfree(buf);
    if (P.state == S_ERROR) return -1;

    /* The CDN: host and path, then the head of the file with its index. */
    url_host(P.url, P.host, sizeof P.host);
    const char *path = strchr(P.url + 8, '/');
    snprintf(P.path, sizeof P.path, "%s", path ? path : "/");
    if (!P.file_len) P.file_len = MINI_BIG_SIZE;
    snprintf(P.msg, sizeof P.msg, "Loading the video\x85");
    if (!fetch_now(0, 256 * 1024 < P.file_len ? 256 * 1024 : P.file_len, 15000)) { if (P.state != S_ERROR) fail("The video didn't start arriving"); return -1; }
    uint64_t off, size;
    if (!mp4_find_box(BIG, 256 * 1024, "moov", &off, &size)) { fail("The video's index isn't at its start"); return -1; }
    if (off + size > MINI_BIG_SIZE || !fetch_now(0, off + size, 20000)) { fail("The video's index didn't arrive"); return -1; }
    if (mp4_parse(BIG + off, size, &P.mp4) < 0 || !(P.dec = h264_open(P.mp4.avcc, P.mp4.avcc_len))) { fail("The video's format isn't one this plays"); return -1; }
    P.have_mp4 = 1;
    if (!W.started) {                                          /* the decoding core, once */
        W.started = 1;
        W.solo = cpu_start(1, worker, 0) != 0;
        kprintf("[yt] decoding on %s\n", W.solo ? "this core (no second one)" : "the second core");
    }
    P.first_pts = P.mp4.video.s[0].pts;
    for (int i = 0; i < P.mp4.video.n; i++) if (P.mp4.video.s[i].pts < P.first_pts) P.first_pts = P.mp4.video.s[i].pts;
    if (!P.len_ms && P.mp4.video.timescale) P.len_ms = (int)(P.mp4.video.duration * 1000 / P.mp4.video.timescale);
    kprintf("[yt] %dx%d, %d frames, %llu bytes\n", P.mp4.w, P.mp4.h, P.mp4.video.n, (unsigned long long)P.file_len);
    restart_at(0);
    P.state = S_PLAYING;
    P.msg[0] = 0;
    return 0;
}

void player_close(void) {
    pause_worker();
    drop_conn();
    if (P.dec) h264_close(P.dec);
    if (P.have_mp4) mp4_free(&P.mp4);
    memset(&P, 0, sizeof P);
    P.shown = -1;
    W.wr = W.rd = 0; W.decoded = W.skipped = 0; W.dec_us = 0;
}

/* Each turn of the loop while the watch page is up: fetch, and show what is due.
 * 1 if the page should be drawn again, 2 if only the picture changed. */
int player_tick(void) {
    if (P.state != S_PLAYING && P.state != S_PAUSED) return 0;
    if (now_ms() >= P.stat_at) {
        if (P.stat_at && W.decoded)
            kprintf("[yt] at %d.%ds: %d decoded (%lu us each%s), %d shown, %d late, %d skipped, %s\n", P.pos_ms / 1000, P.pos_ms / 100 % 10,
                    W.decoded, (unsigned long)(W.dec_us / (uint64_t)W.decoded), W.solo ? ", one core" : ", second core",
                    P.shown_n, P.dropped, W.skipped, P.buffering ? "buffering" : P.state == S_PAUSED ? "paused" : "playing");
        P.stat_at = now_ms() + 5000;
    }
    if (W.next < P.mp4.video.n) P.want = P.mp4.video.s[W.next].off;
    dl_tick(W.solo ? 3 : 6);
    if (P.state == S_ERROR) return 1;
    if (W.err) {
        char w[120]; snprintf(w, sizeof w, W.err == 2 ? "The video is too long to hold" : "The video couldn't be decoded: %s", h264_error(P.dec));
        fail(w); return 1;
    }
    if (W.solo && P.state == S_PLAYING) for (int i = 0; i < 4 && decode_step(); i++) { }
    if (P.state == S_PAUSED) return 0;
    int changed = 0;
    uint32_t rd = W.rd, wr = __atomic_load_n(&W.wr, __ATOMIC_ACQUIRE);
    /* Buffering: nothing decoded to show, and the decoder waits for bytes. */
    int starved = rd == wr && W.buffering;
    if (starved && !P.buffering) { P.buffering = 1; P.paused_ms = (uint64_t)video_now(); snprintf(P.msg, sizeof P.msg, "Buffering\x85"); return 1; }
    if (!starved && P.buffering && rd != wr) { P.buffering = 0; P.clock0 = now_ms() - P.paused_ms; P.msg[0] = 0; changed = 1; }
    if (rd != wr) {
        int now = (int)video_now();
        while (rd + 1 != wr && W.ring_ms[(rd + 1) % RING] <= now) { rd++; P.dropped++; }   /* a newer one is due: skip */
        if (W.ring_ms[rd % RING] <= now) {
            P.shown = (int)(rd % RING);
            P.pos_ms = W.ring_ms[rd % RING];
            P.shown_n++;
            __atomic_store_n(&W.rd, rd + 1, __ATOMIC_RELEASE);
            if (!changed) changed = 2;
        } else if (rd != W.rd) __atomic_store_n(&W.rd, rd, __ATOMIC_RELEASE);
    } else if (W.eos && !P.at_end) {                          /* the end */
        P.at_end = 1;
        P.state = S_PAUSED; P.paused_ms = (uint64_t)P.pos_ms;
        snprintf(P.msg, sizeof P.msg, "Space plays it again");
        return 1;
    }
    if (changed == 2 && P.pos_ms / 1000 != P.last_sec) { P.last_sec = P.pos_ms / 1000; changed = 1; }
    return changed;
}

const surface_t *player_frame(void) { return P.shown >= 0 && W.ring[P.shown].px ? &W.ring[P.shown] : 0; }

int player_status(char *msg, int cap) {
    snprintf(msg, (size_t)cap, "%s", P.msg);
    if (P.state == S_PLAYING) return P.buffering ? 1 : 2;
    return P.state;
}

void player_toggle(void) {
    if (P.at_end) { restart_at(0); P.state = S_PLAYING; P.msg[0] = 0; return; }
    if (P.state == S_PLAYING) { P.paused_ms = (uint64_t)video_now(); P.state = S_PAUSED; snprintf(P.msg, sizeof P.msg, "Paused"); }
    else if (P.state == S_PAUSED) { P.clock0 = now_ms() - P.paused_ms; P.state = S_PLAYING; P.msg[0] = 0; }
}

void player_seek(int secs) {
    if (!P.have_mp4) return;
    int to = P.pos_ms + secs * 1000;
    if (to < 0) to = 0;
    if (P.len_ms && to > P.len_ms - 1000) to = P.len_ms - 1000;
    int was = P.state;
    restart_at(to);
    if (was == S_PAUSED) P.state = S_PAUSED;
}

int player_pos_ms(void) { return P.pos_ms; }
int player_len_ms(void) { return P.len_ms; }
