/* ===========================================================================
 *  BoltOS Mini  -  mini/dl.c
 *
 *  Big downloads: a package from boltos.dev, a language model from Hugging
 *  Face. One file over HTTPS, delivered a piece at a time to whoever asked
 *  (a sink), from the main loop in slices, so the screen and the network
 *  carry on while it comes.
 *
 *  It follows redirects (Hugging Face sends every file to its CDN), asks
 *  with a Range so it can start part-way, and when the connection drops
 *  it connects again and asks for the rest, a few times with a pause in
 *  between. A redirected URL is kept for the retries; if it stops working
 *  (signed CDN links expire) the original one is asked again.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "dl.h"
#include "web.h"
#include "tls.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

int bt_dns_resolve_addr(const char *host, netaddr *out, uint32_t timeout_ms);
void inet_wait_clock(uint32_t ms);

enum { ST_IDLE, ST_HEAD, ST_BODY, ST_DONE, ST_FAIL };

struct dl {
    char     url0[1024];                  /* what was asked for                    */
    char     url[2048];                   /* where it is now (after redirects)     */
    char     host[256], path[2048];
    dl_sink  sink;
    void    *ctx;
    struct tls_conn *tc;
    int      st;
    char     hdr[8192]; int hdr_len;
    uint64_t at;                          /* the next byte of the file to arrive   */
    uint64_t total;                       /* the file's length, 0 until known      */
    int      chunked;                     /* body in chunks: the state below        */
    uint64_t chunk_left; int chunk_st; char chunk_line[24]; int chunk_ll;
    int      redirects, failures;
    uint64_t retry_at, last_byte_ms;
    uint64_t rate_t0, rate_b0; uint32_t rate;
    char     err[128];
};

static void set_url(dl_t *d, const char *u) {
    snprintf(d->url, sizeof d->url, "%s", u);
    url_host(u, d->host, sizeof d->host);
    const char *s = strstr(u, "://");
    const char *p = s ? strchr(s + 3, '/') : 0;
    snprintf(d->path, sizeof d->path, "%s", p ? p : "/");
}

static void drop(dl_t *d) {
    if (d->tc) tls_close(d->tc);
    d->tc = 0; d->hdr_len = 0;
    d->st = ST_IDLE;
}

static void fail(dl_t *d, const char *why) {
    drop(d);
    d->st = ST_FAIL;
    snprintf(d->err, sizeof d->err, "%s", why);
    kprintf("[dl] %s: %s\n", d->url0, why);
}

/* Something went wrong mid-way: try again in a while, a few times. */
static void retry(dl_t *d, const char *why) {
    drop(d);
    if (++d->failures > 8) { fail(d, why); return; }
    uint64_t wait = 1000ull << (d->failures < 5 ? d->failures : 5);
    d->retry_at = now_ms() + wait;
    snprintf(d->err, sizeof d->err, "%s; trying again", why);
    kprintf("[dl] %s (try %d, in %lu s)\n", why, d->failures, (unsigned long)(wait / 1000));
    if (d->failures >= 3 && strcmp(d->url, d->url0)) set_url(d, d->url0);   /* a stale CDN link */
}

dl_t *dl_start(const char *url, uint64_t from, dl_sink sink, void *ctx) {
    if (strncmp(url, "https://", 8)) return 0;
    dl_t *d = kmalloc(sizeof *d);
    if (!d) return 0;
    memset(d, 0, sizeof *d);
    snprintf(d->url0, sizeof d->url0, "%s", url);
    set_url(d, url);
    d->sink = sink; d->ctx = ctx;
    d->at = from;
    d->rate_t0 = now_ms(); d->rate_b0 = from;
    return d;
}

static int send_get(dl_t *d) {
    inet_wait_clock(4000);
    if (!clock_known()) { retry(d, "The clock isn't set yet (no network?)"); return -1; }
    netaddr a;
    if (!bt_dns_resolve_addr(d->host, &a, 4000)) { retry(d, "The server's name didn't resolve (no network?)"); return -1; }
    d->tc = tls_connect_addr(&a, 443, d->host, 10000);
    if (!d->tc) { retry(d, "The server didn't answer"); return -1; }
    char *req = kmalloc(4096);
    if (!req) { retry(d, "Out of memory"); return -1; }
    int n = snprintf(req, 4096, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: BoltOS-Mini/1.0\r\nAccept: */*\r\n"
                     "Accept-Encoding: identity\r\nRange: bytes=%llu-\r\nConnection: close\r\n\r\n",
                     d->path, d->host, (unsigned long long)d->at);
    int r = tls_send(d->tc, req, (uint32_t)n);
    kfree(req);
    if (r < 0) { retry(d, "The request didn't go"); return -1; }
    d->st = ST_HEAD; d->hdr_len = 0;
    d->last_byte_ms = now_ms();
    return 0;
}

static const char *header(const char *h, const char *name) {
    size_t n = strlen(name);
    for (const char *p = h; (p = strstr(p, "\r\n")); p += 2)
        if (!strncasecmp(p + 2, name, n) && p[2 + n] == ':') {
            p += 3 + n;
            while (*p == ' ') p++;
            return p;
        }
    return 0;
}

static int got_head(dl_t *d) {
    d->hdr[d->hdr_len] = 0;
    const char *sp = strchr(d->hdr, ' ');
    int status = sp ? (int)strtol(sp + 1, 0, 10) : 0;
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        const char *loc = header(d->hdr, "Location");
        char u[2048];
        int k = 0;
        while (loc && loc[k] && loc[k] != '\r' && k < (int)sizeof u - 1) { u[k] = loc[k]; k++; }
        u[k] = 0;
        if (!k || ++d->redirects > 8) { fail(d, "The server sent it somewhere that can't be followed"); return -1; }
        if (u[0] == '/') {                             /* the same server, another path */
            char full[2048 + 300];
            snprintf(full, sizeof full, "https://%s%s", d->host, u);
            set_url(d, full);
        } else if (!strncmp(u, "https://", 8)) set_url(d, u);
        else { fail(d, "The server sent it to a plain-HTTP address"); return -1; }
        drop(d);
        return 0;
    }
    if (status == 416 && d->total && d->at >= d->total) { drop(d); d->st = ST_DONE; return 0; }
    if (status == 403 && strcmp(d->url, d->url0)) { retry(d, "The download link expired"); return -1; }
    if (status != 200 && status != 206) {
        char w[80];
        snprintf(w, sizeof w, status == 404 ? "The file isn't there (404)" : "The server said %d", status);
        if (status >= 500) retry(d, w); else fail(d, w);
        return -1;
    }
    const char *te = header(d->hdr, "Transfer-Encoding");
    d->chunked = te && !strncasecmp(te, "chunked", 7);
    d->chunk_st = 0; d->chunk_ll = 0;
    const char *cr = header(d->hdr, "Content-Range"), *cl = header(d->hdr, "Content-Length");
    if (status == 206 && cr) {
        const char *sl = strchr(cr, '/');
        uint64_t start = (uint64_t)strtoull(cr + 6, 0, 10);  /* "bytes N-M/T" */
        if (start != d->at) { fail(d, "The server sent the wrong part of the file"); return -1; }
        if (sl && sl[1] != '*') d->total = (uint64_t)strtoull(sl + 1, 0, 10);
    } else {
        if (d->at) { kprintf("[dl] %s: the server ignored the range; from the start\n", d->host); d->at = 0; }
        if (cl) d->total = (uint64_t)strtoull(cl, 0, 10);
    }
    d->st = ST_BODY;
    return 0;
}

/* Body bytes, after any chunk framing. */
static int deliver(dl_t *d, const uint8_t *p, uint32_t n) {
    if (!n) return 0;
    if (d->sink && d->sink(d->ctx, d->at, p, n) < 0) { fail(d, "Stopped"); return -1; }
    d->at += n;
    d->failures = 0;
    return 0;
}

static int body(dl_t *d, const uint8_t *p, uint32_t n) {
    if (!d->chunked) return deliver(d, p, n);
    while (n) {
        if (d->chunk_st == 0) {                        /* the size line            */
            char c = (char)*p++; n--;
            if (c == '\n') {
                d->chunk_line[d->chunk_ll] = 0;
                d->chunk_left = (uint64_t)strtoull(d->chunk_line, 0, 16);
                d->chunk_ll = 0;
                if (!d->chunk_left) { d->st = ST_DONE; return 0; }
                d->chunk_st = 1;
            } else if (c != '\r' && d->chunk_ll < (int)sizeof d->chunk_line - 1) d->chunk_line[d->chunk_ll++] = c;
        } else if (d->chunk_st == 1) {                 /* the data                 */
            uint32_t k = n < d->chunk_left ? n : (uint32_t)d->chunk_left;
            if (deliver(d, p, k) < 0) return -1;
            p += k; n -= k; d->chunk_left -= k;
            if (!d->chunk_left) d->chunk_st = 2;
        } else {                                       /* the CRLF after it        */
            char c = (char)*p++; n--;
            if (c == '\n') d->chunk_st = 0;
        }
    }
    return 0;
}

int dl_poll(dl_t *d, int ms) {
    if (!d) return DL_FAILED;
    uint64_t end = now_ms() + (uint64_t)ms;
    static uint8_t buf[16384];
    while (d->st != ST_DONE && d->st != ST_FAIL && now_ms() < end) {
        if (d->st == ST_IDLE) {
            if (now_ms() < d->retry_at) break;
            if (d->total && d->at >= d->total) { d->st = ST_DONE; break; }
            if (send_get(d) < 0) break;
            continue;
        }
        int n = tls_recv(d->tc, buf, sizeof buf, 1);
        if (n == 0) {                                  /* the server closed        */
            if (d->st == ST_BODY && ((d->total && d->at >= d->total) || (!d->total && !d->chunked))) {
                drop(d); d->st = ST_DONE; break;
            }
            retry(d, "The connection closed early");
            break;
        }
        if (n < 0) {
            if (now_ms() - d->last_byte_ms > 20000) retry(d, "The download stalled");
            break;
        }
        d->last_byte_ms = now_ms();
        int i = 0;
        if (d->st == ST_HEAD) {
            while (i < n && d->hdr_len < (int)sizeof d->hdr - 1) {
                d->hdr[d->hdr_len++] = (char)buf[i++];
                if (d->hdr_len >= 4 && !memcmp(d->hdr + d->hdr_len - 4, "\r\n\r\n", 4)) break;
            }
            if (d->hdr_len < 4 || memcmp(d->hdr + d->hdr_len - 4, "\r\n\r\n", 4)) {
                if (d->hdr_len >= (int)sizeof d->hdr - 1) fail(d, "The server's answer was too long");
                continue;
            }
            if (got_head(d) < 0 || d->st != ST_BODY) continue;
        }
        if (body(d, buf + i, (uint32_t)(n - i)) < 0) break;
        if (d->st == ST_BODY && d->total && d->at >= d->total) { drop(d); d->st = ST_DONE; }
    }
    uint64_t t = now_ms();
    if (t - d->rate_t0 >= 2000) {
        d->rate = (uint32_t)((d->at - d->rate_b0) * 1000 / (t - d->rate_t0));
        d->rate_t0 = t; d->rate_b0 = d->at;
    }
    return d->st == ST_DONE ? DL_DONE : d->st == ST_FAIL ? DL_FAILED : DL_RUNNING;
}

uint64_t    dl_total(const dl_t *d) { return d ? d->total : 0; }
uint64_t    dl_got(const dl_t *d)   { return d ? d->at : 0; }
uint32_t    dl_rate(const dl_t *d)  { return d ? d->rate : 0; }
const char *dl_error(const dl_t *d) { return d ? d->err : "No download"; }
void        dl_free(dl_t *d)        { if (!d) return; drop(d); kfree(d); }
