/* ===========================================================================
 *  BoltOS Local AI  -  models.c
 *
 *  The models on offer, and getting one onto the SD card. Each is a GGUF
 *  file in a Hugging Face repository, downloaded from there -- huggingface.co
 *  sends the request on to its CDN -- straight onto the card, half a
 *  megabyte at a time (a write that size takes the card about 30 ms, short
 *  enough that the network doesn't notice the pause). A download that stops (Wi-Fi drops, the Pi restarts) carries
 *  on from where the card says it got to. At the end the whole file is read
 *  back off the card and its SHA-256 checked against the one Hugging Face
 *  lists for it, so what gets loaded is what the repository holds.
 *
 *  What's offered is what fits this Pi: the file in the 512 MiB window,
 *  the working memory in the other, and -- measured on an Orange Pi Zero3
 *  at 1008 MHz, with three cores on it -- at least three tokens a second.
 * ===========================================================================*/
#include "ai.h"
#include "dl.h"
#include "crypto.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

const model_t ai_models[] = {
    { "qwen3-0.6b-q8", "Qwen3 0.6B", "Qwen (Alibaba)",
      "The best answers of the three. Runs in its non-thinking mode, so it answers straight away.",
      "Qwen/Qwen3-0.6B-GGUF", "Qwen3-0.6B-Q8_0.gguf", "Q8_0", 639446688,
      "9465e63a22add5354d9bb4b99e90117043c7124007664907259bd16d043bb031", 1536, 4.2f,
      0.7f, 0.8f, 20, 1.1f },
    { "smollm2-360m-q8", "SmolLM2 360M Instruct", "Hugging Face",
      "The quickest, and the smallest download. Good at explaining things; it leans on web search for facts.",
      "HuggingFaceTB/SmolLM2-360M-Instruct-GGUF", "smollm2-360m-instruct-q8_0.gguf", "Q8_0", 386404992,
      "48ab3034d0dd401fbc721eb1df3217902fee7dab9078992d66431f09b7750201", 2048, 7.5f,
      0.2f, 0.9f, 40, 1.1f },
    { "qwen2.5-0.5b-q8", "Qwen2.5 0.5B Instruct", "Qwen (Alibaba)",
      "Qwen's model from the year before. Short and to the point; good with simple questions.",
      "Qwen/Qwen2.5-0.5B-Instruct-GGUF", "qwen2.5-0.5b-instruct-q8_0.gguf", "Q8_0", 675710816,
      "ca59ca7f13d0e15a8cfa77bd17e65d24f6844b554a7b6c12e07a5f89ff76844e", 2048, 5.6f,
      0.7f, 0.8f, 20, 1.05f },
};
const int ai_nmodels = (int)(sizeof ai_models / sizeof ai_models[0]);

void model_path(int i, char *out, int cap) { snprintf(out, (size_t)cap, "localai/%s.gguf", ai_models[i].id); }

#define BUF (512u << 10)

static struct {
    int i, state;                             /* the one being fetched, MS_*          */
    dl_t *d;
    const store_entry_t *e;
    char path[48];
    uint8_t *buf; uint32_t blen;
    uint64_t written, check_at, last_saved;
    sha256_ctx sha;
    char err[160];
} D;

void models_init(void) { D.i = -1; }

const char *model_error(void) { return D.err; }

static int unhex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0; }

int model_state(int i, uint64_t *got, uint64_t *total, uint32_t *rate) {
    const model_t *md = &ai_models[i];
    if (total) *total = md->size;
    if (rate) *rate = D.i == i && D.d ? dl_rate(D.d) : 0;
    if (D.i == i && D.state == MS_DOWNLOADING) { if (got) *got = D.written + D.blen; return MS_DOWNLOADING; }
    if (D.i == i && D.state == MS_CHECKING) { if (got) *got = D.check_at; return MS_CHECKING; }
    char p[48]; model_path(i, p, sizeof p);
    const store_entry_t *e = store_find(p);
    if (e && e->size != md->size) e = 0;                       /* an older file under this name */
    if (got) *got = e ? e->written : 0;
    if (!e) return MS_NONE;
    if (e->flags & STORE_DONE) return MS_READY;
    return MS_PARTIAL;
}

int model_fits(int i, char *why, int cap) {
    const model_t *md = &ai_models[i];
    if (g_ram_size < (1ull << 30) - (64ull << 20)) { snprintf(why, (size_t)cap, "needs a Pi with 1 GB"); return 0; }
    if (md->size > MINI_HIGH_SIZE + MINI_LOW_SIZE / 2) { snprintf(why, (size_t)cap, "too big for this Pi's memory"); return 0; }
    return 1;
}

static int save_progress(void) { D.last_saved = D.written; return store_progress(D.path, D.written, 0, 0); }

static int flush(void) {
    if (!D.blen) return 0;
    if (store_write(D.e, D.written, D.buf, D.blen)) return -1;
    D.written += D.blen; D.blen = 0;
    if (D.written - D.last_saved >= (32u << 20)) save_progress();
    return 0;
}

static int sink(void *ctx, uint64_t off, const uint8_t *p, uint32_t n) {
    (void)ctx;
    if (off != D.written + D.blen) return -1;
    while (n) {
        uint32_t k = BUF - D.blen < n ? BUF - D.blen : n;
        memcpy(D.buf + D.blen, p, k);
        D.blen += k; p += k; n -= k;
        if (D.blen == BUF && flush()) { snprintf(D.err, sizeof D.err, "The SD card won't take a write"); return -1; }
    }
    return 0;
}

static void stop(const char *why) {
    if (why) { snprintf(D.err, sizeof D.err, "%s", why); kprintf("[ai] %s: %s\n", D.path, why); }
    if (D.d) { dl_free(D.d); D.d = 0; }
    if (D.state == MS_DOWNLOADING && D.e) save_progress();
    if (D.buf) { kfree(D.buf); D.buf = 0; }
    D.state = MS_NONE; D.i = -1;
}

int model_download(int i) {
    if (D.i == i) return 0;                                    /* already on its way */
    if (D.i >= 0) { snprintf(D.err, sizeof D.err, "Another model is downloading; one at a time"); return -1; }
    const model_t *md = &ai_models[i];
    D.err[0] = 0;
    if (store_open(1) != 0) { snprintf(D.err, sizeof D.err, "%s", store_why()); return -1; }
    if (model_state(i, 0, 0, 0) == MS_READY) return 0;           /* nothing to do */
    model_path(i, D.path, sizeof D.path);
    D.e = store_find(D.path);
    uint64_t from = 0;
    if (D.e && !(D.e->flags & STORE_DONE) && D.e->size == md->size) from = D.e->written & ~(uint64_t)(BUF - 1);
    else {
        D.e = store_create(D.path, md->size);
        if (!D.e) {
            if (store_free() < md->size)
                snprintf(D.err, sizeof D.err, "The SD card needs %lu MB free for it", (unsigned long)(md->size / 1000000));
            else snprintf(D.err, sizeof D.err, "%s", store_why());
            return -1;
        }
    }
    D.buf = kmalloc(BUF);
    if (!D.buf) { snprintf(D.err, sizeof D.err, "Not enough memory"); return -1; }
    char url[300];
    snprintf(url, sizeof url, "https://huggingface.co/%s/resolve/main/%s", md->repo, md->file);
    D.written = D.last_saved = from; D.blen = 0;
    D.d = dl_start(url, from, sink, 0);
    if (!D.d) { kfree(D.buf); D.buf = 0; snprintf(D.err, sizeof D.err, "Can't download from there"); return -1; }
    D.i = i; D.state = MS_DOWNLOADING;
    kprintf("[ai] downloading %s from %s%s\n", md->name, url, from ? " (carrying on)" : "");
    return 0;
}

void model_cancel(void) { if (D.i >= 0) stop(0); }

int model_delete(int i) {
    if (D.i == i) stop(0);
    if (engine_model() == i) engine_unload();
    char p[48]; model_path(i, p, sizeof p);
    return store_remove(p);
}

/* Read it all back and hash it: a few megabytes a turn. */
static int check_step(void) {
    const model_t *md = &ai_models[D.i];
    uint64_t want = (md->size + 511) & ~511ull;
    uint32_t n = want - D.check_at < BUF ? (uint32_t)(want - D.check_at) : BUF;
    if (store_read(D.e, D.check_at, D.buf, n)) { stop("The SD card wouldn't read it back"); return 1; }
    uint32_t real = D.check_at + n > md->size ? (uint32_t)(md->size - D.check_at) : n;
    sha256_update(&D.sha, D.buf, real);
    D.check_at += n;
    if (D.check_at < want) return 1;
    uint8_t sum[32], ref[32];
    sha256_final(&D.sha, sum);
    for (int k = 0; k < 32; k++) ref[k] = (uint8_t)(unhex(md->sha256[2 * k]) << 4 | unhex(md->sha256[2 * k + 1]));
    if (memcmp(sum, ref, 32)) {
        store_remove(D.path);
        D.e = 0;
        stop("The download didn't match Hugging Face's checksum; it was removed. Try again.");
        return 1;
    }
    store_progress(D.path, md->size, 1, sum);
    kprintf("[ai] %s is on the card and checks out\n", md->name);
    shell_toast(md->name, "is downloaded");
    D.state = MS_READY;
    if (D.buf) { kfree(D.buf); D.buf = 0; }
    D.i = -1;
    return 1;
}

/* Models from an older version of the list, still on the card: their
 * space back. */
void models_tidy(void) {
    for (int i = 0; i < STORE_MAX; i++) {
        const store_entry_t *e = store_at(i);
        if (!e || strncmp(e->name, "localai/", 8) || !strstr(e->name, ".gguf")) continue;
        int known = 0;
        for (int k = 0; k < ai_nmodels; k++) {
            char p[48]; model_path(k, p, sizeof p);
            if (!strcmp(p, e->name) && e->size == ai_models[k].size) known = 1;
        }
        if (known || (D.i >= 0 && !strcmp(e->name, D.path))) continue;
        char n[48]; memcpy(n, e->name, sizeof n);
        kprintf("[ai] %s isn't on the list any more: removed\n", n);
        store_remove(n);
        i = -1;
    }
}

int models_poll(void) {
    if (D.i < 0) return 0;
    if (D.state == MS_CHECKING) return check_step();
    uint64_t was = D.written + D.blen;
    int r = dl_poll(D.d, 12);
    if (r == DL_FAILED) { char w[160]; snprintf(w, sizeof w, "%s", dl_error(D.d)); stop(w); return 1; }
    if (r == DL_DONE) {
        if (flush()) { stop("The SD card won't take a write"); return 1; }
        dl_free(D.d); D.d = 0;
        if (D.written != ai_models[D.i].size) { stop("The download came out the wrong size"); return 1; }
        save_progress();
        D.state = MS_CHECKING; D.check_at = 0;
        sha256_init(&D.sha);
        return 1;
    }
    return D.written + D.blen != was;
}
