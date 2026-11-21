/* ===========================================================================
 *  BoltOS TV  -  tv/ytplay.c
 *
 *  YouTube, played on the TV: search, then a video with its picture and its
 *  sound. The way in is BoltOS Mini's (mini/player.c): a watch is asked of
 *  YouTube's player API as YouTube's Android app asks, which is handed plain
 *  stream URLs, and of what it offers format 18 is the one to play -- 360p
 *  H.264 and AAC sound in one MP4 whose index is at the front, so it streams.
 *
 *  Nothing here waits on the network on the shell's thread. The search, the
 *  player API and the video's bytes all go through tv/netsvc.c's thread, as
 *  jobs (netsvc_call): the file comes over one kept-alive TLS connection, a
 *  megabyte range at a time, into a window of RAM at the file's own offsets,
 *  with a bitmap of 64 KiB blocks saying what is there. Each job fetches for
 *  a couple of seconds and returns, so a thumbnail asked for meanwhile gets
 *  its turn.
 *
 *  The shell's loop does the rest: mini/mp4.c reads the index, mini/h264.c
 *  decodes pictures into a small ring of RGB frames, tv/aac.c the sound,
 *  which is taken from the file's 44.1 kHz to the codec's 48 kHz and kept a
 *  quarter of a second ahead in the audio ring (drivers/audio.c). The sound
 *  is the clock: a picture is shown when the audio that goes with it is
 *  being heard. With no sound device the clock is the wall's.
 * ===========================================================================*/
#include <stdint.h>
#include "tv.h"
#include "yt.h"
#include "web.h"
#include "mp4.h"
#include "h264.h"
#include "aac.h"
#include "tls.h"
#include "net.h"
#include "http.h"
#include "audio.h"
#include "pmm.h"
#include "mm.h"
#include "kheap.h"
#include "kprintf.h"
#include "timeout.h"
#include "string.h"
#include "stdio.h"

#define YT_UA    "com.google.android.youtube/20.10.38 (Linux; U; Android 11) gzip"
#define BLOCK    (64u * 1024)
#define CHUNK    (1024u * 1024)
#define AHEAD    (48ull * 1024 * 1024)          /* stop fetching this far past playback */
#define WIN_MAX  (384ull * 1024 * 1024)
#define RING     5
#define AUDIO_AHEAD_MS 250

/* ======================================================================== *
 *  Search
 * ======================================================================== */
static struct {
    fetch_t  job;
    char     query[300];
    yt_item  items[YT_MAX_RESULTS];
    volatile int n;
    char     err[96];
} SR;

static int yt_video(const jv *v, void *ud) {
    (void)ud;
    const char *id = json_s(json_get(v, "videoId"));
    if (strlen(id) != 11 || SR.n >= YT_MAX_RESULTS) return SR.n >= YT_MAX_RESULTS;
    for (int i = 0; i < SR.n; i++) if (!strcmp(SR.items[i].id, id)) return 0;
    yt_item *it = &SR.items[SR.n];
    memset(it, 0, sizeof *it);
    snprintf(it->id, sizeof it->id, "%s", id);
    char t[400];
    json_text(json_get(v, "title"), t, sizeof t);
    utf8_to_cp1252(t, (int)strlen(t), it->title, sizeof it->title);
    json_text(json_get(v, "ownerText"), t, sizeof t);
    utf8_to_cp1252(t, (int)strlen(t), it->channel, sizeof it->channel);
    char views[60] = "", when[60] = "";
    json_text(json_get(v, "shortViewCountText"), views, sizeof views);
    json_text(json_get(v, "publishedTimeText"), when, sizeof when);
    snprintf(t, sizeof t, "%s%s%s", views, views[0] && when[0] ? " \xB7 " : "", when);
    utf8_to_cp1252(t, (int)strlen(t), it->meta, sizeof it->meta);
    json_text(json_get(v, "lengthText"), t, sizeof t);
    utf8_to_cp1252(t, (int)strlen(t), it->badge, sizeof it->badge);
    if (!it->badge[0] && strstr(it->meta, "watching")) snprintf(it->badge, sizeof it->badge, "LIVE");
    SR.n++;
    return 0;
}

/* On the network thread. */
static void search_run(fetch_t *f) {
    (void)f;
    char eq[700], u[800];
    url_encode(SR.query, eq, sizeof eq);
    snprintf(u, sizeof u, "https://www.youtube.com/results?search_query=%s&hl=en&gl=US", eq);
    uint32_t cap = 3u * 1024 * 1024;
    char *buf = kmalloc(cap + 1);
    if (!buf) { snprintf(SR.err, sizeof SR.err, "Out of memory"); f->len = -1; return; }
    int code = 0;
    /* SOCS=CAI: the cookie banner answered "reject all", so the results come
     * back instead of a consent page. */
    int n = http_request("GET", u, "Cookie: SOCS=CAI\r\nAccept-Language: en-US,en\r\n", 0, 0, buf, cap, &code, 0, 0);
    if (n > 0) {
        buf[n] = 0;
        const char *p = strstr(buf, "ytInitialData = ");
        if (p) {
            p += 16;
            jarena *ar = 0;
            jv *root = json_parse(p, n - (int)(p - buf), &ar);
            if (root) json_each(root, "videoRenderer", yt_video, 0);
            json_free(ar);
        }
    }
    kfree(buf);
    if (n < 0) snprintf(SR.err, sizeof SR.err, code == -3 ? "YouTube's address wasn't found" : "YouTube didn't answer");
    else if (!SR.n) snprintf(SR.err, sizeof SR.err, "No videos for that");
    kprintf("[yt] search: %d videos (HTTP %d, %d bytes)\n", SR.n, code, n);
    f->len = SR.n ? SR.n : -1;
}

int yt_search(const char *query) {
    if (SR.job.state == FETCH_QUEUED || SR.job.state == FETCH_RUNNING) return -1;
    memset(&SR.job, 0, sizeof SR.job);
    snprintf(SR.query, sizeof SR.query, "%s", query);
    SR.n = 0; SR.err[0] = 0;
    return netsvc_call(&SR.job, search_run, 0);
}

int yt_search_poll(void) {
    int s = SR.job.state;
    if (s == FETCH_QUEUED || s == FETCH_RUNNING) return 0;
    if (s == FETCH_DONE && SR.job.len > 0) return 1;
    if (!SR.err[0]) {
        int st = SR.job.status;
        snprintf(SR.err, sizeof SR.err, st == -6 ? "No network: plug in Ethernet" : st == -7 ? "The network is still coming up" :
                 "YouTube didn't answer");
    }
    return -1;
}

const yt_item *yt_results(int *n) { *n = SR.n; return SR.items; }
const char *yt_search_error(void) { return SR.err; }

/* ======================================================================== *
 *  The video
 * ======================================================================== */
static struct {
    volatile int state;
    char     msg[160];
    char     id[16], title[200], author[120];
    int      len_ms;
    /* the file, in a window of RAM at its own offsets */
    char     url[2048], host[160], path[1900];
    uint64_t file_len;
    uint8_t *big; uint64_t big_cap, big_phys;
    uint8_t  have[(WIN_MAX / BLOCK) / 8];
    /* the network thread's: the connection and the request in flight */
    struct tls_conn *tc;
    int      in_body;
    uint64_t req_at, req_end, body_at;
    char     hdr[4096]; int hdr_len;
    uint64_t last_byte_ms;
    int      failures;
    /* the shell's to set, the network thread's to read */
    volatile uint64_t want;              /* fetch from here on                     */
    volatile int stop;                   /* drop it all                            */
    volatile int opened;                 /* the open job found the index           */
    uint64_t moov_off, moov_len;
    fetch_t  job;
    int      job_open;                   /* job is the open (1) or a download (2)  */
    uint64_t next_dl;
    /* the media: the shell's */
    mp4_t    mp4; int have_mp4;
    h264    *dec;
    aac     *ad;
    int      vnext, anext;
    int64_t  first_pts;
    int      hide_before;
    int      eos, verr, flushed, buffering, at_end;
    surface_t ring[RING]; int ring_ms[RING];
    uint32_t wr, rd;
    int      shown;                      /* ring slot on screen, -1 none           */
    int      pos_ms;
    /* the clock */
    int      sound;                      /* the audio ring is ours                 */
    int      a_ms0;                      /* media time of the first frame written  */
    uint64_t a_written;                  /* 48 kHz frames written since then       */
    uint64_t wall0; int paused_ms;       /* the clock with no sound                */
    double   rs_t; int16_t rs_prev[2];   /* the resampler                          */
    int      prime;                      /* decode this unit for its overlap, drop it */
    /* numbers */
    uint32_t decoded, skipped, dropped, shown_n;
    uint64_t dec_us, stat_at;
} P = { .shown = -1 };

static void fail(const char *why) {
    P.state = YT_ERROR;
    snprintf(P.msg, sizeof P.msg, "%s", why);
    kprintf("[yt] %s\n", why);
}

static uint64_t now_ms(void) { return tv_now_ms(); }

/* ---- the window ----------------------------------------------------------- */
static int window_take(void) {
    if (P.big) return 0;
    for (uint64_t cap = WIN_MAX; cap >= 32ull << 20; cap /= 2) {
        uint64_t pa = pmm_alloc_contig(cap / PAGE_SIZE);
        if (pa) { P.big_phys = pa; P.big_cap = cap; P.big = (uint8_t *)P2V(pa); return 0; }
    }
    return -1;
}
static void window_give(void) {
    if (P.big) pmm_free_contig(P.big_phys, P.big_cap / PAGE_SIZE);
    P.big = 0; P.big_cap = 0;
}

static uint64_t limit(void) { return P.file_len < P.big_cap ? P.file_len : P.big_cap; }

static int have_range(uint64_t a, uint64_t b) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (b > P.file_len) b = P.file_len;
    if (b > P.big_cap) return 0;
    for (uint64_t k = a / BLOCK; k * BLOCK < b; k++) if (!(P.have[k >> 3] >> (k & 7) & 1)) return 0;
    return 1;
}

/* ---- the download: on the network thread -------------------------------- */
static void mark_arrived(void) {
    __atomic_thread_fence(__ATOMIC_RELEASE);
    for (uint64_t k = P.req_at / BLOCK; k * BLOCK < P.body_at; k++)
        if ((k + 1) * BLOCK <= P.body_at || P.body_at >= P.file_len) P.have[k >> 3] |= (uint8_t)(1 << (k & 7));
}

static void drop_conn(void) {
    if (P.tc) tls_close(P.tc);
    P.tc = 0; P.in_body = 0; P.hdr_len = 0;
}

static int next_range(uint64_t *a, uint64_t *b) {
    uint64_t lim = limit();
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

static void take(const uint8_t *p, int n) {
    uint64_t room = P.req_end - P.body_at;
    if ((uint64_t)n > room) n = (int)room;
    if (P.body_at + (uint64_t)n <= P.big_cap) memcpy(P.big + P.body_at, p, (size_t)n);
    P.body_at += (uint64_t)n;
    mark_arrived();
}

/* Fetch for up to `ms`: 1 if anything arrived, -1 on a failure that ends it. */
static int dl_for(int ms) {
    uint64_t end = now_ms() + (uint64_t)ms;
    int got = 0;
    static uint8_t buf[16384];
    while (now_ms() < end && !P.stop) {
        if (!P.tc || (!P.in_body && !P.hdr_len && P.body_at >= P.req_end)) {
            uint64_t a, b;
            if (!next_range(&a, &b)) return got;
            if (!P.tc) {
                netaddr na;
                if (!dns_resolve_addr(P.host, &na, 3000) || !(P.tc = tls_connect_addr(&na, 443, P.host, 8000))) {
                    if (++P.failures > 4) { fail("The video's server can't be reached"); return -1; }
                    return got;
                }
            }
            if (send_request(a, b) < 0) { drop_conn(); continue; }
        }
        int n = tls_recv(P.tc, buf, sizeof buf, 50);
        if (n == 0) { drop_conn(); continue; }                 /* closed: again on a new one */
        if (n < 0) {
            if (now_ms() - P.last_byte_ms > 15000) {
                drop_conn();
                if (++P.failures > 4) { fail("The video stopped arriving"); return -1; }
            }
            continue;
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
            if (sp) status = (sp[1] - '0') * 100 + (sp[2] - '0') * 10 + (sp[3] - '0');
            if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                const char *loc = strcasestr(P.hdr, "\r\nLocation: ");
                char u[2048] = "";
                if (loc) {
                    loc += 12;
                    int k = 0;
                    while (loc[k] && loc[k] != '\r' && k < (int)sizeof u - 1) { u[k] = loc[k]; k++; }
                    u[k] = 0;
                }
                drop_conn();
                if (!u[0] || strncmp(u, "https://", 8) || ++P.failures > 4) { fail("The video's server sent it somewhere this can't follow"); return -1; }
                url_host(u, P.host, sizeof P.host);
                const char *path = strchr(u + 8, '/');
                snprintf(P.path, sizeof P.path, "%s", path ? path : "/");
                P.body_at = P.req_end;                         /* the same range, from there */
                continue;
            }
            if (status != 206 && status != 200) {
                char w[80]; snprintf(w, sizeof w, "The video's server said %d", status);
                drop_conn(); fail(w); return -1;
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

static void dl_run(fetch_t *f) {
    if (dl_for(2500) < 0) f->len = -1;
}

/* Until [a, b) is in, or ms run out. On the network thread. */
static int dl_until(uint64_t a, uint64_t b, uint32_t ms) {
    P.want = a;
    uint64_t end = now_ms() + ms;
    while (!have_range(a, b) && now_ms() < end && !P.stop && P.state != YT_ERROR)
        if (dl_for(200) < 0) return 0;
    return have_range(a, b);
}

/* The open: the player API, then the head of the file with its index. On
 * the network thread; the shell carries on once `opened` is set. */
static void open_run(fetch_t *f) {
    char body[600];
    int bl = snprintf(body, sizeof body,
        "{\"context\":{\"client\":{\"clientName\":\"ANDROID\",\"clientVersion\":\"20.10.38\","
        "\"androidSdkVersion\":30,\"osName\":\"Android\",\"osVersion\":\"11\",\"hl\":\"en\",\"gl\":\"US\"}},"
        "\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}", P.id);
    uint32_t cap = 1024 * 1024;
    char *buf = kmalloc(cap + 1);
    if (!buf) { fail("Out of memory"); f->len = -1; return; }
    int code = 0;
    int n = http_request("POST", "https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
                         "User-Agent: " YT_UA "\r\nContent-Type: application/json\r\n", body, (uint32_t)bl, buf, cap, &code, 0, 0);
    jarena *ar = 0;
    jv *root = n > 0 ? json_parse(buf, n, &ar) : 0;
    if (!root) { kfree(buf); fail(n < 0 ? "YouTube didn't answer" : "YouTube's answer couldn't be read"); f->len = -1; return; }
    jv *vd = json_get(root, "videoDetails");
    const char *t = json_s(json_get(vd, "title"));
    if (!P.title[0]) utf8_to_cp1252(t, (int)strlen(t), P.title, sizeof P.title);
    t = json_s(json_get(vd, "author"));
    if (!P.author[0]) utf8_to_cp1252(t, (int)strlen(t), P.author, sizeof P.author);
    P.len_ms = (int)json_i(json_get(vd, "lengthSeconds")) * 1000;
    const char *status = json_s(json_path(root, "playabilityStatus.status"));
    if (strcmp(status, "OK")) {
        const char *why = json_s(json_path(root, "playabilityStatus.reason"));
        if (!why[0]) why = "YouTube won't play this one here";
        char w[160]; utf8_to_cp1252(why, (int)strlen(why), w, sizeof w);
        fail(w);
    } else {
        jv *fm = json_path(root, "streamingData.formats");
        for (jv *v = fm ? fm->kid : 0; v; v = v->next)
            if (json_i(json_get(v, "itag")) == 18) {
                snprintf(P.url, sizeof P.url, "%s", json_s(json_get(v, "url")));
                const char *cl = json_s(json_get(v, "contentLength"));
                for (; *cl >= '0' && *cl <= '9'; cl++) P.file_len = P.file_len * 10 + (uint64_t)(*cl - '0');
            }
        if (!P.url[0]) fail(json_get(json_get(root, "streamingData"), "hlsManifestUrl") ? "Live streams don't play here yet" :
                                                                                         "This video has no 360p stream");
    }
    kprintf("[yt] %s: %s, %d s, %s\n", P.id, status, P.len_ms / 1000, P.url[0] ? "format 18" : "no stream");
    json_free(ar);
    kfree(buf);
    if (P.state == YT_ERROR) { f->len = -1; return; }

    url_host(P.url, P.host, sizeof P.host);
    const char *path = strchr(P.url + 8, '/');
    snprintf(P.path, sizeof P.path, "%s", path ? path : "/");
    if (!P.file_len) P.file_len = P.big_cap;
    if (P.file_len > P.big_cap) kprintf("[yt] %llu bytes: the first %llu MB play\n", (unsigned long long)P.file_len,
                                        (unsigned long long)(P.big_cap >> 20));
    snprintf(P.msg, sizeof P.msg, "Loading the video\x85");
    uint64_t head = 256 * 1024 < P.file_len ? 256 * 1024 : P.file_len;
    if (!dl_until(0, head, 15000)) { if (P.state != YT_ERROR && !P.stop) fail("The video didn't start arriving"); f->len = -1; return; }
    uint64_t off, size;
    if (!mp4_find_box(P.big, head, "moov", &off, &size)) { fail("The video's index isn't at its start"); f->len = -1; return; }
    if (off + size > P.big_cap || !dl_until(0, off + size, 20000)) {
        if (P.state != YT_ERROR && !P.stop) fail("The video's index didn't arrive");
        f->len = -1; return;
    }
    P.moov_off = off; P.moov_len = size;
    P.opened = 1;
}

/* ---- pictures -------------------------------------------------------------- */
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
        int two = y + 1 < p->h;
        for (int x = 0; x < p->w; x += 2) {
            int d = u[x / 2] - 128, e = v[x / 2] - 128;
            int rr = 409 * e + 128, gg = -100 * d - 208 * e + 128, bb = 516 * d + 128;
            for (int k = 0; k < 4; k++) {
                if (k >= 2 && !two) break;
                if ((k & 1) && x + 1 >= p->w) continue;
                const uint8_t *yy = (k < 2 ? y0 : y1) + x + (k & 1);
                uint32_t *oo = (k < 2 ? o0 : o1) + x + (k & 1);
                int c = 298 * (*yy - 16);
                int r = (c + rr) >> 8, g = (c + gg) >> 8, b = (c + bb) >> 8;
                r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
                *oo = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
            }
        }
    }
}

static int vts(int64_t pts) {
    return (int)((pts - P.first_pts) * 1000 / (P.mp4.video.timescale ? P.mp4.video.timescale : 1000));
}
static int ats(int i) {
    if (i >= P.mp4.audio.n) i = P.mp4.audio.n - 1;
    return i < 0 ? 0 : (int)(P.mp4.audio.s[i].pts * 1000 / (P.mp4.audio.timescale ? P.mp4.audio.timescale : 1000));
}

/* A picture nothing refers to (nal_ref_idc 0): skippable when behind. */
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

/* ---- the clock ------------------------------------------------------------- */
static int clock_ms(void) {
    if (P.state == YT_PAUSED) return P.paused_ms;
    if (P.sound) {
        uint64_t q = audio_ring_queued();
        uint64_t heard = P.a_written > q ? P.a_written - q : 0;
        return P.a_ms0 + (int)(heard * 1000 / AUDIO_RATE);
    }
    if (P.buffering) return P.paused_ms;
    return (int)(now_ms() - P.wall0);
}

static void sound_start(void) {
    if (P.sound || !P.ad) return;
    if (audio_ring_open() == 0) P.sound = 1;
}
static void sound_stop(void) {
    if (P.sound) audio_ring_close();
    P.sound = 0;
}

/* The sound from `ms` on: the unit before it is decoded only for its
 * overlap, so the first one heard is whole. */
static void audio_restart(int ms) {
    if (!P.ad) return;
    aac_reset(P.ad);
    int k = 0;
    for (int i = 0; i < P.mp4.audio.n; i++) { if (ats(i) <= ms) k = i; else break; }
    P.anext = k > 0 ? k - 1 : 0;
    P.prime = k > 0;
    P.a_ms0 = ats(k);
    P.a_written = 0;
    P.rs_t = 0; P.rs_prev[0] = P.rs_prev[1] = 0;
}

/* Decode and write sound until a quarter of a second is queued. */
static void feed_audio(void) {
    if (!P.sound) return;
    static int16_t pcm[2048], out[2 * 1400];
    int guard = 0;
    while (audio_ring_queued() < AUDIO_RATE * AUDIO_AHEAD_MS / 1000 && guard++ < 8) {
        if (P.anext >= P.mp4.audio.n) return;
        mp4_sample *s = &P.mp4.audio.s[P.anext];
        if (s->off + s->size > P.big_cap || !have_range(s->off, s->off + s->size)) return;
        int got = aac_decode(P.ad, P.big + s->off, (int)s->size, pcm);
        P.anext++;
        if (P.prime) { P.prime = 0; continue; }
        if (got < 0) memset(pcm, 0, sizeof pcm);
        /* 44.1 (or whatever it is) to 48 kHz, linearly; v[0] is the last frame before */
        int rate = aac_rate(P.ad), n = 0;
        double step = (double)rate / AUDIO_RATE;
        while (P.rs_t < 1024 && n < 1400) {
            int i = (int)P.rs_t;
            float fr = (float)(P.rs_t - i);
            for (int c = 0; c < 2; c++) {
                float a = i ? pcm[(i - 1) * 2 + c] : P.rs_prev[c], b = pcm[i * 2 + c];
                out[n * 2 + c] = (int16_t)(a + (b - a) * fr);
            }
            n++;
            P.rs_t += step;
        }
        P.rs_t -= 1024;
        P.rs_prev[0] = pcm[2046]; P.rs_prev[1] = pcm[2047];
        uint32_t w = audio_ring_write(out, (uint32_t)n);
        P.a_written += w;
    }
}

/* ---- decoding ------------------------------------------------------------------ */
/* One step: a picture out into the ring, or a sample in. 0 if nothing to do now. */
static int decode_step(int now) {
    if (P.wr - P.rd >= RING - 1) return 0;
    h264_pic pic;
    if (h264_output(P.dec, &pic)) {
        int ms = vts(pic.pts);
        if (ms < P.hide_before) return 1;
        uint32_t k = P.wr % RING;
        to_rgb(&pic, &P.ring[k]);
        P.ring_ms[k] = ms;
        P.wr++;
        return 1;
    }
    if (P.eos || P.verr) return 0;
    if (P.vnext >= P.mp4.video.n) {
        if (!P.flushed) { h264_flush(P.dec); P.flushed = 1; return 1; }
        P.eos = 1;
        return 0;
    }
    mp4_sample *s = &P.mp4.video.s[P.vnext];
    if (s->off + s->size > P.big_cap) { P.eos = 1; return 0; }
    if (!have_range(s->off, s->off + s->size)) return 0;
    if (vts(s->pts) + 150 < now && vts(s->pts) >= P.hide_before && disposable(P.big + s->off, (int)s->size)) {
        P.vnext++; P.skipped++;
        return 1;
    }
    uint64_t t0 = now_us();
    int r = h264_decode(P.dec, P.big + s->off, (int)s->size, s->pts);
    P.dec_us += now_us() - t0;
    if (r < 0) { P.verr = 1; return 0; }
    P.vnext++; P.decoded++;
    return 1;
}

static void restart_at(int ms) {
    if (P.dec) h264_close(P.dec);
    P.dec = h264_open(P.mp4.avcc, P.mp4.avcc_len);
    int k = 0;
    for (int i = 0; i < P.mp4.video.n; i++)
        if (P.mp4.video.s[i].key && vts(P.mp4.video.s[i].pts) <= ms) k = i;
    P.vnext = k; P.flushed = 0; P.eos = 0; P.verr = 0; P.at_end = 0;
    P.wr = P.rd = 0;
    P.shown = -1;
    P.hide_before = ms;
    P.pos_ms = ms;
    P.paused_ms = ms;
    P.wall0 = now_ms() - (uint64_t)ms;
    P.want = P.mp4.video.s[k].off;
    if (P.sound) { audio_ring_close(); P.sound = 0; }
    audio_restart(ms);
    if (P.state != YT_PAUSED) sound_start();
}

/* ---- the API ----------------------------------------------------------------- */
static void free_media(void) {
    if (P.dec) h264_close(P.dec);
    if (P.ad) aac_close(P.ad);
    if (P.have_mp4) mp4_free(&P.mp4);
    for (int i = 0; i < RING; i++) if (P.ring[i].px) kfree(P.ring[i].px);
    P.dec = 0; P.ad = 0; P.have_mp4 = 0;
    memset(P.ring, 0, sizeof P.ring);
}

static int job_busy(void) { return P.job.state == FETCH_QUEUED || P.job.state == FETCH_RUNNING; }

/* The network thread may still be in a job that writes into the window: wait
 * it out (it checks `stop` between reads, so it's quick), then close. */
static void wait_job(void) {
    P.stop = 1;
    uint64_t end = now_ms() + 4000;
    while (job_busy() && now_ms() < end) __asm__ volatile("hlt");
}

void yt_close(void) {
    sound_stop();
    wait_job();
    if (job_busy()) {                       /* still out there: leave its memory alone */
        kprintf("[yt] the download didn't stop; its window is left as it is\n");
        P.big = 0; P.big_cap = 0;
    }
    if (P.tc) { tls_close(P.tc); P.tc = 0; }  /* the network thread is idle now */
    free_media();
    window_give();
    memset(&P, 0, sizeof P);
    P.shown = -1;
}

int yt_open(const char *id) {
    char title[200] = "", author[120] = "";
    int n; const yt_item *r = yt_results(&n);
    for (int i = 0; i < n; i++) if (!strcmp(r[i].id, id)) { snprintf(title, sizeof title, "%s", r[i].title); snprintf(author, sizeof author, "%s", r[i].channel); }
    yt_close();
    snprintf(P.id, sizeof P.id, "%.11s", id);
    snprintf(P.title, sizeof P.title, "%s", title);
    snprintf(P.author, sizeof P.author, "%s", author);
    P.state = YT_LOADING;
    snprintf(P.msg, sizeof P.msg, "Asking YouTube for the video\x85");
    if (window_take() < 0) { fail("Not enough memory for a video"); return -1; }
    memset(&P.job, 0, sizeof P.job);
    P.job_open = 1;
    if (netsvc_call(&P.job, open_run, 0) < 0) { fail("The network is busy"); return -1; }
    return 0;
}

/* The open job finished: read the index, open the decoders, start. */
static void begin(void) {
    if (mp4_parse(P.big + P.moov_off, P.moov_len, &P.mp4) < 0 || !P.mp4.video.n ||
        !(P.dec = h264_open(P.mp4.avcc, P.mp4.avcc_len))) {
        fail("The video's format isn't one this plays"); return;
    }
    P.have_mp4 = 1;
    if (P.mp4.audio.n && P.mp4.asc_len) {
        P.ad = aac_open(P.mp4.asc, P.mp4.asc_len);
        if (!P.ad) kprintf("[yt] the sound isn't AAC-LC: playing the picture alone\n");
    }
    P.first_pts = P.mp4.video.s[0].pts;
    for (int i = 0; i < P.mp4.video.n; i++) if (P.mp4.video.s[i].pts < P.first_pts) P.first_pts = P.mp4.video.s[i].pts;
    if (!P.len_ms && P.mp4.video.timescale) P.len_ms = (int)(P.mp4.video.duration * 1000 / P.mp4.video.timescale);
    kprintf("[yt] %dx%d, %d frames, %d sound units (%d Hz), %llu bytes\n", P.mp4.w, P.mp4.h, P.mp4.video.n, P.mp4.audio.n,
            P.ad ? aac_rate(P.ad) : 0, (unsigned long long)P.file_len);
    P.state = YT_PLAYING;
    P.msg[0] = 0;
    restart_at(0);
    if (P.ad && !P.sound) kprintf("[yt] no sound device takes a stream: picture only\n");
}

int yt_tick(uint64_t now) {
    (void)now;
    if (P.state == YT_IDLE) return 0;
    int changed = 0;
    if (P.job_open && !job_busy()) {
        P.job_open = 0;
        if (P.state == YT_ERROR) return 1;
        if (!P.opened) { fail("The video didn't load"); return 1; }
        begin();
        changed = 1;
    }
    if (P.state != YT_PLAYING && P.state != YT_PAUSED) return changed;
    if (P.job.state == FETCH_FAILED && P.state != YT_ERROR) { fail("The video stopped arriving"); return 1; }
    if (P.state == YT_ERROR) return 1;

    /* keep the download going from what plays next */
    uint64_t w = P.vnext < P.mp4.video.n ? P.mp4.video.s[P.vnext].off : P.file_len;
    if (P.ad && P.anext < P.mp4.audio.n && P.mp4.audio.s[P.anext].off < w) w = P.mp4.audio.s[P.anext].off;
    P.want = w;
    if (!job_busy() && now_ms() >= P.next_dl) {
        uint64_t a, b;
        if (next_range(&a, &b)) { memset(&P.job, 0, sizeof P.job); netsvc_call(&P.job, dl_run, 0); }
        else P.next_dl = now_ms() + 500;
    }
    if (P.stat_at < now_ms()) {
        if (P.stat_at && P.decoded)
            kprintf("[yt] at %d.%ds: %u decoded (%lu us each), %u shown, %u late, %u skipped, sound %s\n", P.pos_ms / 1000,
                    P.pos_ms / 100 % 10, P.decoded, (unsigned long)(P.dec_us / P.decoded), P.shown_n, P.dropped, P.skipped,
                    P.sound ? "on" : "off");
        P.stat_at = now_ms() + 10000;
    }
    if (P.verr) {
        char m[120]; snprintf(m, sizeof m, "The video couldn't be decoded: %s", h264_error(P.dec));
        fail(m); return 1;
    }
    if (P.state == YT_PAUSED) return changed;

    feed_audio();
    int clk = clock_ms();
    uint64_t t0 = now_us();
    while (now_us() - t0 < 12000 && decode_step(clk)) { }

    /* waiting for bytes: nothing decoded to show and the next sample isn't in */
    int starved = P.rd == P.wr && !P.eos && P.vnext < P.mp4.video.n &&
                  !have_range(P.mp4.video.s[P.vnext].off, P.mp4.video.s[P.vnext].off + P.mp4.video.s[P.vnext].size);
    if (starved != P.buffering) {
        P.buffering = starved;
        if (starved) { P.paused_ms = clk; snprintf(P.msg, sizeof P.msg, "Buffering\x85"); }
        else { P.wall0 = now_ms() - (uint64_t)P.paused_ms; P.msg[0] = 0; }
        changed = 1;
    }
    if (P.rd != P.wr) {
        while (P.rd + 1 != P.wr && P.ring_ms[(P.rd + 1) % RING] <= clk) { P.rd++; P.dropped++; }
        if (P.ring_ms[P.rd % RING] <= clk) {
            P.shown = (int)(P.rd % RING);
            P.pos_ms = P.ring_ms[P.shown];
            P.rd++;
            P.shown_n++;
            changed = 1;
        }
    } else if (P.eos && !P.at_end && (!P.sound || audio_ring_queued() == 0)) {
        P.at_end = 1;
        sound_stop();
        P.state = YT_PAUSED; P.paused_ms = P.pos_ms;
        snprintf(P.msg, sizeof P.msg, "The end. OK plays it again.");
        changed = 1;
    }
    return changed;
}

int  yt_state(void) { return P.state; }
int  yt_buffering(void) { return P.buffering || P.state == YT_LOADING; }
const char *yt_message(void) { return P.msg; }
const surface_t *yt_picture(void) {
    /* The slot shown stays put until the one after it is due: rd has moved
     * past it, but the decoder writes only slots outside [rd, wr) and one more. */
    return P.shown >= 0 && P.ring[P.shown].px ? &P.ring[P.shown] : 0;
}
int  yt_pos_ms(void) { return P.pos_ms; }
int  yt_len_ms(void) { return P.len_ms; }
const char *yt_title(void) { return P.title; }
const char *yt_author(void) { return P.author; }
int  yt_has_sound(void) { return P.sound; }

int yt_loaded_ms(void) {
    if (!P.have_mp4 || !P.mp4.video.n) return 0;
    int k = P.vnext;
    while (k < P.mp4.video.n && have_range(P.mp4.video.s[k].off, P.mp4.video.s[k].off + P.mp4.video.s[k].size)) k += 8;
    if (k >= P.mp4.video.n) return P.len_ms;
    return vts(P.mp4.video.s[k].pts);
}

void yt_toggle(void) {
    if (P.at_end) { P.state = YT_PLAYING; P.msg[0] = 0; restart_at(0); return; }
    if (P.state == YT_PLAYING) {
        P.paused_ms = clock_ms();
        sound_stop();
        P.state = YT_PAUSED;
        snprintf(P.msg, sizeof P.msg, "Paused");
    } else if (P.state == YT_PAUSED) {
        P.state = YT_PLAYING;
        P.msg[0] = 0;
        P.wall0 = now_ms() - (uint64_t)P.paused_ms;
        audio_restart(P.paused_ms);
        sound_start();
    }
}

void yt_seek(int secs) {
    if (!P.have_mp4 || (P.state != YT_PLAYING && P.state != YT_PAUSED)) return;
    int to = P.pos_ms + secs * 1000;
    if (to < 0) to = 0;
    if (P.len_ms && to > P.len_ms - 1000) to = P.len_ms - 1000;
    int was = P.state;
    if (P.at_end) { P.at_end = 0; was = YT_PLAYING; P.state = YT_PLAYING; P.msg[0] = 0; }
    restart_at(to);
    if (was == YT_PAUSED) { sound_stop(); P.state = YT_PAUSED; P.paused_ms = to; }
}
