/* ===========================================================================
 *  BoltOS Mini  -  mini/pkg.c
 *
 *  Packages (pkg.h): what boltos.dev offers, installing one, loading the
 *  installed ones at every start, and the two big windows of RAM that
 *  packages and the video player share.
 *
 *  Installing is a download into RAM, a check of its length and SHA-256
 *  against the list's, a copy onto the SD card, and a load. Loading copies
 *  the image somewhere in the heap, moves its own addresses there, fills
 *  in the kernel's from the export table, makes the instruction caches of
 *  all four cores forget what was at those addresses, and calls pkg_main.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "pkg.h"
#include "store.h"
#include "dl.h"
#include "web.h"
#include "crypto.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define CATALOG_URL "https://boltos.dev/pkg/index.json"
#define MAX_OFFERS  16
#define MAX_LOADED  8
#define MAX_PKG     (16u << 20)

/* ---- loading ------------------------------------------------------------------------ */
typedef struct {
    char     id[32], name[48], version[16];
    void    *raw;                         /* what kmalloc gave; the image is aligned in it */
    uint8_t *base;
    uint32_t size;
    const pkg_info_t *info;
    char     err[128];                    /* installed, but this is why it didn't load   */
} loaded_t;
static loaded_t L[MAX_LOADED];
static int nloaded;

static const void *lookup(const char *name) {
    for (int i = 0; i < mini_nexports; i++) if (!strcmp(mini_exports[i].name, name)) return mini_exports[i].addr;
    return 0;
}

/* New code where the caches may still hold something else: clean the data
 * cache to where instructions are fetched from, then empty every core's
 * instruction cache. */
static void sync_icache(const uint8_t *p, uint32_t n) {
    for (uintptr_t a = (uintptr_t)p & ~63ull; a < (uintptr_t)p + n; a += 64) __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\nic ialluis\ndsb ish\nisb" ::: "memory");
}

static int load(const uint8_t *f, uint32_t len, loaded_t *out, char *err, int cap) {
    const bpk_head_t *h = (const bpk_head_t *)f;
    if (len < sizeof *h || memcmp(h->magic, BPK_MAGIC, 4) || h->head_size != sizeof *h) {
        snprintf(err, (size_t)cap, "It isn't a BoltOS package"); return -1;
    }
    if (h->abi != PKG_ABI) { snprintf(err, (size_t)cap, "It was made for another version of BoltOS Mini"); return -1; }
    uint64_t need = (uint64_t)sizeof *h + h->image_size + 4ull * h->n_rel + h->imp_bytes;
    if (need > len || h->mem_size < h->image_size || h->mem_size > MAX_PKG || h->entry >= h->image_size) {
        snprintf(err, (size_t)cap, "The package is damaged"); return -1;
    }
    void *raw = kmalloc((uint64_t)h->mem_size + 4096);
    if (!raw) { snprintf(err, (size_t)cap, "Not enough memory to load it"); return -1; }
    uint8_t *base = (uint8_t *)(((uintptr_t)raw + 4095) & ~4095ull);   /* its pages must line up: adrp */
    const uint8_t *img = f + sizeof *h;
    memcpy(base, img, h->image_size);
    memset(base + h->image_size, 0, h->mem_size - h->image_size);
    const uint8_t *rel = img + h->image_size;
    for (uint32_t i = 0; i < h->n_rel; i++) {
        uint32_t off; memcpy(&off, rel + 4 * i, 4);
        if (off + 8 > h->mem_size || (off & 7)) { kfree(raw); snprintf(err, (size_t)cap, "The package is damaged"); return -1; }
        *(uint64_t *)(base + off) += (uint64_t)(uintptr_t)base;
    }
    const uint8_t *ip = rel + 4ull * h->n_rel, *ie = ip + h->imp_bytes;
    for (uint32_t i = 0; i < h->n_imp; i++) {
        if (ip + 5 > ie) { kfree(raw); snprintf(err, (size_t)cap, "The package is damaged"); return -1; }
        uint32_t off; memcpy(&off, ip, 4);
        int n = ip[4];
        char name[256];
        if (ip + 5 + n > ie || off + 8 > h->mem_size || (off & 7)) { kfree(raw); snprintf(err, (size_t)cap, "The package is damaged"); return -1; }
        memcpy(name, ip + 5, (size_t)n); name[n] = 0;
        ip += 5 + n;
        const void *a = lookup(name);
        if (!a) {
            kfree(raw);
            snprintf(err, (size_t)cap, "It needs %s, which this BoltOS Mini doesn't have: update it first", name);
            return -1;
        }
        *(uint64_t *)(base + off) += (uint64_t)(uintptr_t)a;
    }
    sync_icache(base, h->mem_size);
    const pkg_info_t *info = ((pkg_entry_t)(uintptr_t)(base + h->entry))();
    if (!info || info->abi != PKG_ABI) { kfree(raw); snprintf(err, (size_t)cap, "The package didn't start"); return -1; }
    memset(out, 0, sizeof *out);
    memcpy(out->id, h->id, sizeof out->id - 1);
    memcpy(out->name, h->name, sizeof out->name - 1);
    memcpy(out->version, h->version, sizeof out->version - 1);
    out->raw = raw; out->base = base; out->size = h->mem_size; out->info = info;
    kprintf("[pkg] loaded %s %s at %lx (%u KiB, %u imports)\n", out->name, out->version,
            (unsigned long)(uintptr_t)base, (unsigned)(h->mem_size >> 10), (unsigned)h->n_imp);
    return 0;
}

static loaded_t *find_loaded(const char *id) {
    for (int i = 0; i < nloaded; i++) if (!strcmp(L[i].id, id)) return &L[i];
    return 0;
}

static void unload(loaded_t *l) {
    if (l->info) {
        if (l->info->app) shell_remove_app(l->info->app);
        if (l->info->unload) l->info->unload();
    }
    if (l->raw) kfree(l->raw);
    int i = (int)(l - L);
    for (int k = i; k + 1 < nloaded; k++) L[k] = L[k + 1];
    nloaded--;
}

/* Load it, and put its app in the bar; or remember why not. */
static int start(const uint8_t *f, uint32_t len, const char *id_hint) {
    const bpk_head_t *h = (const bpk_head_t *)f;
    char id[32] = "";
    if (len >= sizeof *h) memcpy(id, h->id, sizeof id - 1);
    if (!id[0]) snprintf(id, sizeof id, "%s", id_hint);
    loaded_t *old = find_loaded(id);
    if (old) unload(old);
    if (nloaded >= MAX_LOADED) return -1;
    loaded_t *l = &L[nloaded];
    char err[128];
    if (load(f, len, l, err, sizeof err) != 0) {
        memset(l, 0, sizeof *l);
        snprintf(l->id, sizeof l->id, "%s", id);
        snprintf(l->err, sizeof l->err, "%s", err);
        nloaded++;
        kprintf("[pkg] %s didn't load: %s\n", id, err);
        return -1;
    }
    nloaded++;
    if (l->info->app && shell_add_app(l->info->app) != 0) kprintf("[pkg] no room in the bar for %s\n", l->name);
    return 0;
}

/* ---- what's installed, at boot ---------------------------------------------------------- */
static uint64_t boot_at;

static void load_installed(void) {
    if (store_open(0) != 0) { kprintf("[pkg] nothing installed (%s)\n", store_why()); return; }
    for (int i = 0; i < STORE_MAX; i++) {
        const store_entry_t *e = store_at(i);
        if (!e || strncmp(e->name, "pkg/", 4) || !(e->flags & STORE_DONE) || e->size > MAX_PKG) continue;
        uint32_t n = (uint32_t)((e->size + 511) & ~511ull);
        uint8_t *buf = kmalloc(n);
        if (!buf) continue;
        uint8_t sum[32];
        int ok = store_read(e, 0, buf, n) == 0;
        if (ok) { sha256(buf, (uint32_t)e->size, sum); ok = !memcmp(sum, e->sha256, 32); }
        char id[32];
        snprintf(id, sizeof id, "%.*s", (int)(strlen(e->name) - 8), e->name + 4);     /* pkg/<id>.bpk */
        if (ok) start(buf, (uint32_t)e->size, id);
        else kprintf("[pkg] %s on the card is damaged; install it again\n", e->name);
        kfree(buf);
    }
}

/* ---- the list from boltos.dev ------------------------------------------------------------- */
static struct {
    int      state;                       /* 0 not yet, 1 fetching, 2 have it, -2 failed */
    pkg_offer_t o[MAX_OFFERS];
    int      n;
    char     err[128];
    dl_t    *d;
    char    *buf; uint32_t len, cap;
} K;

static int mem_sink(void *ctx, uint64_t off, const uint8_t *p, uint32_t n) {
    (void)ctx;
    if (off != K.len || K.len + n + 1 > K.cap) return -1;
    memcpy(K.buf + K.len, p, n);
    K.len += n;
    return 0;
}

static int unhex(const char *s, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[2 * i + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return -1;
            v = v << 4 | d;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

static void parse_catalog(void) {
    jarena *ar = 0;
    jv *root = json_parse(K.buf, (int)K.len, &ar);
    jv *arr = json_get(root, "packages");
    K.n = 0;
    for (jv *p = arr ? arr->kid : 0; p && K.n < MAX_OFFERS; p = p->next) {
        pkg_offer_t *o = &K.o[K.n];
        memset(o, 0, sizeof *o);
        snprintf(o->id, sizeof o->id, "%s", json_s(json_get(p, "id")));
        snprintf(o->name, sizeof o->name, "%s", json_s(json_get(p, "name")));
        snprintf(o->version, sizeof o->version, "%s", json_s(json_get(p, "version")));
        utf8_to_cp1252(json_s(json_get(p, "summary")), (int)strlen(json_s(json_get(p, "summary"))), o->summary, sizeof o->summary);
        utf8_to_cp1252(json_s(json_get(p, "about")), (int)strlen(json_s(json_get(p, "about"))), o->about, sizeof o->about);
        utf8_to_cp1252(json_s(json_get(p, "needs")), (int)strlen(json_s(json_get(p, "needs"))), o->needs, sizeof o->needs);
        snprintf(o->url, sizeof o->url, "%s", json_s(json_get(p, "url")));
        const char *sha = json_s(json_get(p, "sha256"));
        o->size = (uint32_t)json_i(json_get(p, "size"));
        jv *abi = json_get(p, "abi");
        if (!o->id[0] || !o->url[0] || strlen(sha) != 64 || unhex(sha, o->sha256, 32) || !o->size || o->size > MAX_PKG) continue;
        if (abi && json_i(abi) != PKG_ABI) continue;           /* for another BoltOS Mini */
        K.n++;
    }
    json_free(ar);
    if (!root) { K.state = -2; snprintf(K.err, sizeof K.err, "boltos.dev's list didn't make sense"); return; }
    K.state = 2;
    kprintf("[pkg] boltos.dev offers %d package%s\n", K.n, K.n == 1 ? "" : "s");
}

void pkg_refresh(void) {
    if (K.d) return;
    if (!K.buf) { K.cap = 256 * 1024; K.buf = kmalloc(K.cap); }
    if (!K.buf) { K.state = -2; snprintf(K.err, sizeof K.err, "Out of memory"); return; }
    K.len = 0;
    K.d = dl_start(CATALOG_URL, 0, mem_sink, 0);
    K.state = K.d ? 1 : -2;
}

int pkg_catalog(const pkg_offer_t **out) {
    if (out) *out = K.o;
    if (K.state == 2) return K.n;
    if (K.state == 1 || K.state == 0) return -1;
    return -2;
}
const char *pkg_catalog_error(void) { return K.err; }

/* ---- installing ---------------------------------------------------------------------------- */
static struct { pkg_job_t j; dl_t *d; uint8_t *buf; uint32_t len; pkg_offer_t o; } J;

static int job_sink(void *ctx, uint64_t off, const uint8_t *p, uint32_t n) {
    (void)ctx;
    if (off != J.len || J.len + n > J.o.size) return -1;
    memcpy(J.buf + J.len, p, n);
    J.len += n;
    J.j.got = J.len;
    return 0;
}

static void job_fail(const char *why) {
    J.j.state = PK_FAILED;
    snprintf(J.j.msg, sizeof J.j.msg, "%s", why);
    kprintf("[pkg] installing %s failed: %s\n", J.o.id, why);
    if (J.d) { dl_free(J.d); J.d = 0; }
    if (J.buf) { kfree(J.buf); J.buf = 0; }
}

int pkg_install(const pkg_offer_t *o) {
    if (J.j.state == PK_FETCHING || J.j.state == PK_INSTALLING) return -1;
    memset(&J, 0, sizeof J);
    J.o = *o;
    snprintf(J.j.id, sizeof J.j.id, "%s", o->id);
    J.j.total = o->size;
    J.buf = kmalloc(o->size + 512);
    if (!J.buf) { job_fail("Not enough memory"); return -1; }
    if (store_open(1) != 0) { job_fail(store_why()); return -1; }
    J.d = dl_start(o->url, 0, job_sink, 0);
    if (!J.d) { job_fail("That address can't be downloaded"); return -1; }
    J.j.state = PK_FETCHING;
    snprintf(J.j.msg, sizeof J.j.msg, "Downloading");
    kprintf("[pkg] installing %s %s from %s\n", o->name, o->version, o->url);
    return 0;
}

/* The file is here and checked: onto the card, and start it. */
static int keep_and_start(const uint8_t *f, uint32_t len, const char *id, const char *name, const char *version,
                          char *msg, int cap) {
    if (store_open(1) != 0) { snprintf(msg, (size_t)cap, "%s", store_why()); return -1; }
    char path[48];
    snprintf(path, sizeof path, "pkg/%s.bpk", id);
    uint8_t sum[32];
    sha256(f, len, sum);
    const store_entry_t *e = store_create(path, len);
    if (!e || store_write(e, 0, f, len) || store_progress(path, len, 1, sum)) {
        snprintf(msg, (size_t)cap, "%s", e ? "The SD card won't take a write" : store_why()); return -1;
    }
    char meta[96];
    snprintf(meta, sizeof meta, "%s|%s", name, version);
    store_set_meta(path, meta);
    if (start(f, len, id) != 0) {
        loaded_t *l = find_loaded(id);
        snprintf(msg, (size_t)cap, "%s", l ? l->err : "It didn't load");
        return -1;
    }
    return 0;
}

static void job_poll(void) {
    if (J.j.state != PK_FETCHING) return;
    int r = dl_poll(J.d, 8);
    J.j.got = J.len;
    if (r == DL_RUNNING) return;
    if (r == DL_FAILED) { job_fail(dl_error(J.d)); return; }
    dl_free(J.d); J.d = 0;
    uint8_t sum[32];
    if (J.len != J.o.size) { job_fail("The download was the wrong size"); return; }
    sha256(J.buf, J.len, sum);
    if (memcmp(sum, J.o.sha256, 32)) { job_fail("The download didn't match its checksum"); return; }
    J.j.state = PK_INSTALLING;
    char msg[128];
    if (keep_and_start(J.buf, J.len, J.o.id, J.o.name, J.o.version, msg, sizeof msg) != 0) { job_fail(msg); return; }
    kfree(J.buf); J.buf = 0;
    J.j.state = PK_NONE;
    J.j.msg[0] = 0;
    shell_toast(J.o.name, "is installed");
}

const pkg_job_t *pkg_job(void) { return &J.j; }

int pkg_install_file(const uint8_t *f, uint32_t len, char *msg, int cap) {
    const bpk_head_t *h = (const bpk_head_t *)f;
    if (len < sizeof *h || memcmp(h->magic, BPK_MAGIC, 4)) { snprintf(msg, (size_t)cap, "not a package"); return -1; }
    char id[33] = "", name[49] = "", ver[17] = "";
    memcpy(id, h->id, 32); memcpy(name, h->name, 48); memcpy(ver, h->version, 16);
    if (keep_and_start(f, len, id, name, ver, msg, cap) != 0) return -1;
    snprintf(msg, (size_t)cap, "installed %s %s", name, ver);
    return 0;
}

/* ---- removing ------------------------------------------------------------------------------- */
int pkg_remove(const char *id) {
    loaded_t *l = find_loaded(id);
    if (l) unload(l);
    if (store_open(0) != 0) return -1;
    char path[48];
    snprintf(path, sizeof path, "pkg/%s.bpk", id);
    store_remove(path);
    /* and what it kept for itself: "<id>/..." */
    char pre[40];
    int pn = snprintf(pre, sizeof pre, "%s/", id);
    for (int i = 0; i < STORE_MAX; i++) {
        const store_entry_t *e = store_at(i);
        if (e && !strncmp(e->name, pre, (size_t)pn)) { char n[48]; memcpy(n, e->name, sizeof n); store_remove(n); i = -1; }
    }
    kprintf("[pkg] removed %s\n", id);
    return 0;
}

int pkg_installed(const char *id, const app_t **app, char *version, int vcap) {
    loaded_t *l = find_loaded(id);
    if (app) *app = l && l->info ? l->info->app : 0;
    if (l) { if (version) snprintf(version, (size_t)vcap, "%s", l->version); return 1; }
    char path[48];
    snprintf(path, sizeof path, "pkg/%s.bpk", id);
    const store_entry_t *e = store_find(path);
    if (!e || !(e->flags & STORE_DONE)) return 0;
    if (version) {
        const char *bar = strchr(e->meta, '|');
        snprintf(version, (size_t)vcap, "%s", bar ? bar + 1 : "");
    }
    return 1;
}

const char *pkg_load_error(const char *id) {
    loaded_t *l = find_loaded(id);
    return l && !l->info ? l->err : 0;
}

void pkg_init(void) { boot_at = now_ms() + 600; }

int pkg_poll(void) {
    int frame = 0;
    if (boot_at && now_ms() >= boot_at) { boot_at = 0; load_installed(); frame = 1; }
    if (K.d) {
        int r = dl_poll(K.d, 4);
        if (r == DL_DONE) { parse_catalog(); dl_free(K.d); K.d = 0; frame = 1; }
        else if (r == DL_FAILED) { snprintf(K.err, sizeof K.err, "%s", dl_error(K.d)); K.state = -2; dl_free(K.d); K.d = 0; frame = 1; }
    }
    uint64_t was = J.j.got; int st = J.j.state;
    job_poll();
    if (J.j.got != was || J.j.state != st) frame = 1;
    for (int i = 0; i < nloaded; i++)
        if (L[i].info && L[i].info->background && L[i].info->background()) frame = 1;
    return frame;
}

int pkg_dev(const char *id, const char *cmd, char *out, int cap) {
    loaded_t *l = find_loaded(id);
    if (!l || !l->info || !l->info->dev) return snprintf(out, (size_t)cap, "%s isn't loaded, or has no dev hook\n", id);
    return l->info->dev(cmd, out, cap);
}

int pkg_debug(char *out, int cap) {
    int n = snprintf(out, (size_t)cap, "packages: %d loaded, catalog %d (%d offers)\n", nloaded, K.state, K.n);
    for (int i = 0; i < nloaded && n < cap - 200; i++)
        n += snprintf(out + n, (size_t)(cap - n), "  %-12s %-24s %-8s %s\n", L[i].id, L[i].name, L[i].version,
                      L[i].info ? "loaded" : L[i].err);
    return n + store_debug(out + n, cap - n);
}

/* ---- the big windows of RAM ------------------------------------------------------------------ */
static struct { const char *who; void (*release)(void); int video; } W[2];

void *bigmem_claim(int high, const char *who, void (*release)(void)) {
    high = !!high;
    /* A video nobody is watching -- its page isn't on screen -- gives way. */
    if (high && W[1].video && shell_current() != &app_browser) {
        void player_stop(const char *why);
        char why[120];
        snprintf(why, sizeof why, "Stopped so %s could use the memory. Space plays it again.", who);
        player_stop(why);
    }
    if (W[high].who || W[high].video) return 0;
    W[high].who = who; W[high].release = release;
    return (void *)(uintptr_t)(high ? MINI_HIGH_BASE : MINI_LOW_BASE);
}

void bigmem_free(int high) { high = !!high; W[high].who = 0; W[high].release = 0; }
const char *bigmem_owner(int high) { high = !!high; return W[high].video ? "a video" : W[high].who; }

int bigmem_take_for_video(void) {
    if (W[1].video) return 0;
    if (W[1].who) {
        kprintf("[mem] a video needs the high window: %s gives it back\n", W[1].who);
        void (*rel)(void) = W[1].release;
        if (rel) rel();
        W[1].who = 0; W[1].release = 0;
    }
    W[1].video = 1;
    return 0;
}

void bigmem_video_done(void) { W[1].video = 0; }

/* Something that can't start without the window (a Linux program, lx.c):
 * whoever has it is asked to let go, as a video asks -- Firefox stops,
 * Local AI puts its model away, a program from Code ends. An owner whose
 * memory must stay put (a machine that is running) says no by calling
 * bigmem_keep() from its release callback. 0 once the window is free; -1 if
 * its owner said no, or has no way to be asked. */
static int keep_said;
void bigmem_keep(void) { keep_said = 1; }

int bigmem_evict(int high) {
    high = !!high;
    if (high && W[1].video) {
        void player_stop(const char *why);
        player_stop("Stopped so a program could use the memory. Space plays it again.");
        W[1].video = 0;
    }
    if (!W[high].who) return 0;
    if (!W[high].release) return -1;
    kprintf("[mem] the %s window is needed: %s is asked for it\n", high ? "high" : "low", W[high].who);
    void (*rel)(void) = W[high].release;
    keep_said = 0;
    rel();
    if (keep_said) { kprintf("[mem] %s keeps it\n", W[high].who ? W[high].who : "its owner"); return -1; }
    W[high].who = 0; W[high].release = 0;           /* given back (or left for us, as for a video) */
    return 0;
}
