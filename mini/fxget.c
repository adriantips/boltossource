/* ===========================================================================
 *  BoltOS Mini  -  mini/fxget.c
 *
 *  Getting Firefox onto the card. The Firefox machine (mini/vm.c) needs two
 *  files in the store -- its Linux kernel and its root file system, made by
 *  mini/firefox/build.sh -- about 310 MB together. boltos.dev keeps them
 *  beside the packages, with a list saying which version is current, how
 *  big each file is and its SHA-256:
 *
 *      https://boltos.dev/pkg/firefox/firefox.json
 *      { "version": "159.0-1",
 *        "files": [ { "name": "fx/Image", "url": "...", "size": N, "sha256": "..." }, ... ] }
 *
 *  Each file comes down a piece at a time from the main loop (mini/dl.c),
 *  into the store at its own offsets, and carries on from where it stopped
 *  if the Pi restarts or the network drops; then it is read back and hashed
 *  before the Browser trusts it. A file already there at the list's version
 *  is skipped, so a new version only fetches what changed.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "vm.h"
#include "dl.h"
#include "store.h"
#include "web.h"
#include "crypto.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

#define LIST_URL "https://boltos.dev/pkg/firefox/firefox.json"
#define BUF      (512u << 10)
#define NFILE    4

static struct {
    int      state;
    char     err[160];
    char     version[32];
    struct { char name[48], url[300], sha[65]; uint64_t size; } f[NFILE];
    int      nf, i;
    dl_t    *d;
    char    *list; uint32_t llen;
    const store_entry_t *e;
    uint8_t *buf; uint32_t blen;
    uint64_t written, last_saved, check_at;
    sha256_ctx sha;
    uint64_t total, done_before;
} G;

int fxget_state(uint64_t *got, uint64_t *total, uint32_t *rate) {
    if (got) {
        uint64_t g = G.done_before;
        if (G.state == FX_DOWNLOADING) g += G.written + G.blen;
        else if (G.state == FX_CHECKING) g += G.f[G.i].size;
        *got = g;
    }
    if (total) *total = G.total;
    if (rate) *rate = G.d ? dl_rate(G.d) : 0;
    return G.state;
}
const char *fxget_error(void) { return G.err; }

static void stop(const char *why) {
    if (G.d) { dl_free(G.d); G.d = 0; }
    if (G.state == FX_DOWNLOADING && G.e) store_progress(G.f[G.i].name, G.written, 0, 0);
    if (G.buf) { kfree(G.buf); G.buf = 0; }
    if (G.list) { kfree(G.list); G.list = 0; }
    if (why) { snprintf(G.err, sizeof G.err, "%s", why); kprintf("[fx] %s\n", why); G.state = FX_FAILED; }
}

static int list_sink(void *ctx, uint64_t off, const uint8_t *p, uint32_t n) {
    (void)ctx;
    if (off + n > 16383) return -1;
    memcpy(G.list + off, p, n);
    if (off + n > G.llen) G.llen = (uint32_t)(off + n);
    return 0;
}

static int flush(void) {
    if (!G.blen) return 0;
    if (store_write(G.e, G.written, G.buf, G.blen)) return -1;
    G.written += G.blen; G.blen = 0;
    if (G.written - G.last_saved >= (32u << 20)) { G.last_saved = G.written; store_progress(G.f[G.i].name, G.written, 0, 0); }
    return 0;
}

static int sink(void *ctx, uint64_t off, const uint8_t *p, uint32_t n) {
    (void)ctx;
    if (off != G.written + G.blen) return -1;
    while (n) {
        uint32_t k = BUF - G.blen < n ? BUF - G.blen : n;
        memcpy(G.buf + G.blen, p, k);
        G.blen += k; p += k; n -= k;
        if (G.blen == BUF && flush()) { snprintf(G.err, sizeof G.err, "The SD card won't take a write"); return -1; }
    }
    return 0;
}

/* A file already on the card at this version, whole. */
static int have(int i) {
    const store_entry_t *e = store_find(G.f[i].name);
    return e && (e->flags & STORE_DONE) && e->size == G.f[i].size && !strcmp(e->meta, G.version);
}

/* The next file to fetch, or ready. */
static void next_file(void) {
    for (; G.i < G.nf; G.i++) {
        if (have(G.i)) { G.done_before += G.f[G.i].size; continue; }
        const store_entry_t *e = store_find(G.f[G.i].name);
        uint64_t from = 0;
        if (e && !(e->flags & STORE_DONE) && e->size == G.f[G.i].size && !strcmp(e->meta, G.version))
            from = e->written & ~(uint64_t)(BUF - 1);
        else {
            if (e) store_remove(G.f[G.i].name);
            e = store_create(G.f[G.i].name, G.f[G.i].size);
            if (!e) { stop(store_free() < G.f[G.i].size ? "The SD card hasn't room for Firefox" : store_why()); return; }
            store_set_meta(G.f[G.i].name, G.version);
            e = store_find(G.f[G.i].name);
        }
        G.e = e; G.written = G.last_saved = from; G.blen = 0;
        if (!G.buf && !(G.buf = kmalloc(BUF))) { stop("Not enough memory"); return; }
        G.d = dl_start(G.f[G.i].url, from, sink, 0);
        if (!G.d) { stop("Firefox can't be downloaded from there"); return; }
        G.state = FX_DOWNLOADING;
        kprintf("[fx] downloading %s (%lu MB)%s\n", G.f[G.i].name, (unsigned long)(G.f[G.i].size >> 20), from ? ", carrying on" : "");
        return;
    }
    stop(0);
    G.state = FX_READY;
    kprintf("[fx] Firefox %s is on the card\n", G.version);
    shell_toast("Firefox", "is downloaded");
}

static void got_list(void) {
    jarena *ar = 0;
    jv *root = json_parse(G.list, (int)G.llen, &ar);
    G.nf = 0; G.total = 0;
    if (root) {
        snprintf(G.version, sizeof G.version, "%s", json_s(json_get(root, "version")));
        jv *fl = json_get(root, "files");
        for (jv *f = fl ? fl->kid : 0; f && G.nf < NFILE; f = f->next) {
            snprintf(G.f[G.nf].name, sizeof G.f[0].name, "%s", json_s(json_get(f, "name")));
            snprintf(G.f[G.nf].url, sizeof G.f[0].url, "%s", json_s(json_get(f, "url")));
            snprintf(G.f[G.nf].sha, sizeof G.f[0].sha, "%s", json_s(json_get(f, "sha256")));
            G.f[G.nf].size = (uint64_t)json_i(json_get(f, "size"));
            G.total += G.f[G.nf].size;
            G.nf++;
        }
        json_free(ar);
    }
    kfree(G.list); G.list = 0;
    if (!G.nf || !G.version[0]) { stop("boltos.dev's Firefox list couldn't be read"); return; }
    G.i = 0; G.done_before = 0;
    next_file();
}

void fxget_start(void) {
    if (G.state == FX_LIST || G.state == FX_DOWNLOADING || G.state == FX_CHECKING) return;
    memset(&G, 0, sizeof G);
    if (store_open(1) != 0) { snprintf(G.err, sizeof G.err, "%s", store_why()); G.state = FX_FAILED; return; }
    G.list = kmalloc(16384);
    if (!G.list) { stop("Not enough memory"); return; }
    G.d = dl_start(LIST_URL, 0, list_sink, 0);
    if (!G.d) { stop("boltos.dev can't be reached"); return; }
    G.state = FX_LIST;
}

void fxget_cancel(void) { if (G.state == FX_LIST || G.state == FX_DOWNLOADING || G.state == FX_CHECKING) { stop(0); G.state = FX_NONE; } }

static int unhex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0; }

/* Each turn of the main loop: a slice of the download, or of the check. */
int fxget_poll(void) {
    if (G.state == FX_LIST) {
        int r = dl_poll(G.d, 4);
        if (r == DL_DONE) { dl_free(G.d); G.d = 0; got_list(); return 1; }
        if (r == DL_FAILED) { char w[160]; snprintf(w, sizeof w, "boltos.dev: %s", dl_error(G.d)); stop(w); return 1; }
        return 0;
    }
    if (G.state == FX_DOWNLOADING) {
        int r = dl_poll(G.d, 8);
        if (r == DL_RUNNING) return 0;
        if (r == DL_FAILED) { char w[160]; snprintf(w, sizeof w, "The download stopped: %s", dl_error(G.d)); stop(w); return 1; }
        dl_free(G.d); G.d = 0;
        if (flush()) { stop("The SD card won't take a write"); return 1; }
        store_progress(G.f[G.i].name, G.written, 0, 0);
        sha256_init(&G.sha);
        G.check_at = 0;
        G.state = FX_CHECKING;
        return 1;
    }
    if (G.state == FX_CHECKING) {
        uint64_t size = G.f[G.i].size, want = (size + 511) & ~511ull;
        uint32_t n = want - G.check_at < BUF ? (uint32_t)(want - G.check_at) : BUF;
        if (store_read(G.e, G.check_at, G.buf, n)) { stop("The SD card wouldn't read Firefox back"); return 1; }
        uint32_t real = G.check_at + n > size ? (uint32_t)(size - G.check_at) : n;
        sha256_update(&G.sha, G.buf, real);
        G.check_at += n;
        if (G.check_at < want) return 0;
        uint8_t sum[32], ref[32];
        sha256_final(&G.sha, sum);
        for (int k = 0; k < 32; k++) ref[k] = (uint8_t)(unhex(G.f[G.i].sha[2 * k]) << 4 | unhex(G.f[G.i].sha[2 * k + 1]));
        if (memcmp(sum, ref, 32)) {
            store_remove(G.f[G.i].name); G.e = 0;
            stop("A Firefox file didn't match its checksum; it was removed. Try again.");
            return 1;
        }
        store_progress(G.f[G.i].name, size, 1, sum);
        G.done_before += size;
        G.i++;
        next_file();
        return 1;
    }
    return 0;
}
