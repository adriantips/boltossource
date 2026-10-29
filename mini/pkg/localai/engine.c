/* ===========================================================================
 *  BoltOS Local AI  -  engine.c
 *
 *  The model in RAM, and the Pi's other cores running it.
 *
 *  The GGUF file is read off the SD card into the high window of RAM (512
 *  MiB, the one the video player streams into; a video that starts takes it
 *  back and the model is unloaded). The low window (382 MiB) holds what the
 *  model works in: the tokenizer's tables, the cache of keys and values,
 *  the vectors. Reading happens a couple of megabytes per main-loop turn,
 *  so the screen keeps moving.
 *
 *  A request -- tokens to read, maybe a reply to write -- goes to core 3,
 *  the leader, as one pool job. The leader takes cores 1 and 2 as helpers
 *  for as long as the request lasts, and splits every matrix product and
 *  every attention step with them; core 0 stays with the screen and the
 *  network, and picks up the reply's tokens as they come. A helper whose
 *  core the video player asks for (core_wanted) finishes the step it is on
 *  and leaves at the next one.
 *
 *  What the cores share: the boot core writes the request, then bumps a
 *  sequence number; each helper has a line it reads (its task) and a line
 *  it writes (that it finished), never the same one.
 * ===========================================================================*/
#include "ai.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"
#include "llm_int.h"
#include <arm_neon.h>

#define LEADER 3
#define CHUNK  (2u << 20)

uint64_t llm_now_ns(void) { return now_us() * 1000ull; }


static struct {
    int state, model;
    char err[160];
    uint8_t *lo, *hi;                          /* the two windows                     */
    uint64_t size, lgot, hgot;                 /* the file; read into each so far     */
    int split; uint64_t x, x0, lo_len;         /* in two parts: llm_open_split's terms */
    const store_entry_t *e;
    arena_t arena;
    llm_t *m;
    int have_high, have_low;
} E;

/* ---- the request, shared with the leader --------------------------------------------- */
static struct {
    int toks[4096]; int n, max_new;
    sampler_t s;
    volatile int stop;
    volatile int done __attribute__((aligned(64)));
    volatile int fed, nout;
    int out[2048];
    uint64_t t0, t_first, t_end, t_fed;
    int solo;                                  /* no pool: run here, a step at a time */
    int solo_gen;
} R;

/* Before anything: no model, no request (set here, not by initializers, so
 * these stay out of .data, which a package carries whole in its file). */
void engine_init(void) { E.model = -1; R.done = 1; }

/* ---- the helpers ----------------------------------------------------------------------- */
typedef struct { volatile uint32_t seq; volatile int quit, leave_ok; } __attribute__((aligned(64))) hpost_t;
typedef struct { volatile uint32_t done; volatile int leaving, running; } __attribute__((aligned(64))) hack_t;
static hpost_t HP[4];
static hack_t  HA[4];
static par_fn T_fn; static void *T_arg; static int T_n; static int T_part[4];
static int active[4], nactive;                /* the leader's: helpers it has        */

static inline void sev(void) { __asm__ volatile("dsb ish\nsev" ::: "memory"); }
static inline void wfe(void) { __asm__ volatile("wfe" ::: "memory"); }

static void helper(void *arg) {
    int c = (int)(intptr_t)arg;
    uint32_t seen = HP[c].seq;
    __atomic_store_n(&HA[c].running, 1, __ATOMIC_RELEASE);
    sev();
    for (;;) {
        uint32_t q;
        while ((q = __atomic_load_n(&HP[c].seq, __ATOMIC_ACQUIRE)) == seen) {
            if (HP[c].quit || HP[c].leave_ok) goto out;
            if (core_wanted(c) && !HA[c].leaving) { __atomic_store_n(&HA[c].leaving, 1, __ATOMIC_RELEASE); sev(); }
            wfe();
        }
        seen = q;
        T_fn(T_arg, T_part[c], T_n);
        __atomic_store_n(&HA[c].done, seen, __ATOMIC_RELEASE);
        sev();
    }
out:
    __atomic_store_n(&HA[c].running, 0, __ATOMIC_RELEASE);
    sev();
}

static void par(par_fn fn, void *arg) {
    int n = 1, cores[4];
    for (int i = 0; i < nactive; i++) {
        int c = active[i];
        if (__atomic_load_n(&HA[c].leaving, __ATOMIC_ACQUIRE)) {   /* the video wants it */
            HP[c].leave_ok = 1; sev();
            for (int k = i; k + 1 < nactive; k++) active[k] = active[k + 1];
            nactive--; i--;
            continue;
        }
        cores[n++] = c;
    }
    T_fn = fn; T_arg = arg; T_n = n;
    for (int i = 1; i < n; i++) T_part[cores[i]] = i;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    for (int i = 1; i < n; i++) __atomic_store_n(&HP[cores[i]].seq, HP[cores[i]].seq + 1, __ATOMIC_RELEASE);
    if (n > 1) sev();
    fn(arg, 0, n);
    for (int i = 1; i < n; i++) {
        int c = cores[i];
        while (__atomic_load_n(&HA[c].done, __ATOMIC_ACQUIRE) != HP[c].seq) __asm__ volatile("yield");
    }
}

static void helpers_start(int leader) {
    nactive = 0;
    for (int c = 1; c < 4; c++) {
        if (c == leader || !core_pooled(c) || core_busy(c) || core_wanted(c)) continue;
        HP[c].quit = 0; HP[c].leave_ok = 0; HA[c].leaving = 0; HA[c].done = HP[c].seq;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        if (core_run(c, helper, (void *)(intptr_t)c) != 0) continue;
        uint64_t end = now_us() + 5000;
        while (!__atomic_load_n(&HA[c].running, __ATOMIC_ACQUIRE) && now_us() < end) __asm__ volatile("yield");
        if (HA[c].running) active[nactive++] = c;
        else HP[c].quit = 1;                       /* it'll leave whenever it starts */
    }
}

static void helpers_stop(void) {
    for (int i = 0; i < nactive; i++) HP[active[i]].quit = 1;
    sev();
    for (int i = 0; i < nactive; i++) {
        int c = active[i];
        uint64_t end = now_us() + 100000;
        while (__atomic_load_n(&HA[c].running, __ATOMIC_ACQUIRE) && now_us() < end) { sev(); __asm__ volatile("yield"); }
    }
    nactive = 0;
}

/* ---- one request ---------------------------------------------------------------------------- */
static int is_stop(int t) {
    const llm_info_t *I = llm_info(E.m);
    return t == I->eos || t == I->im_end || t == I->eot || t == I->tool_call_end;
}

/* One token of the reply; 0 when it's over. */
static int gen_step(void) {
    if (R.stop || R.nout >= R.max_new || R.nout >= (int)(sizeof R.out / sizeof R.out[0]) ||
        llm_pos(E.m) >= llm_info(E.m)->ctx) return 0;
    uint64_t ts = llm_now_ns();
    int t = llm_sample(E.m, &R.s, R.out, R.nout);
    llm_stats(E.m)->sample_ns += llm_now_ns() - ts;
    R.out[R.nout] = t;
    __atomic_store_n(&R.nout, R.nout + 1, __ATOMIC_RELEASE);
    if (R.nout == 1) R.t_first = now_us();
    if (is_stop(t)) {
        if (t == llm_info(E.m)->tool_call_end || t == llm_info(E.m)->im_end) llm_feed(E.m, &t, 1, 0);   /* the template has it */
        return 0;
    }
    llm_feed(E.m, &t, 1, &R.stop);
    return 1;
}

static void leader(void *arg) {
    (void)arg;
    helpers_start(LEADER);
    llm_set_par(E.m, par, 1 + nactive);
    for (int at = 0; at < R.n && !R.stop; ) {
        int k = R.n - at < 16 ? R.n - at : 16;
        int fed = llm_feed(E.m, R.toks + at, k, &R.stop);
        at += fed;
        __atomic_store_n(&R.fed, at, __ATOMIC_RELEASE);
        if (fed < k) break;
    }
    R.t_fed = now_us();
    if (R.max_new > 0) while (gen_step()) { }
    helpers_stop();
    R.t_end = now_us();
    __atomic_store_n(&R.done, 1, __ATOMIC_RELEASE);
    sev();
}

int engine_run(const int *toks, int n, int max_new, const sampler_t *s) {
    if (E.state != EN_READY || !R.done || n > (int)(sizeof R.toks / sizeof R.toks[0])) return -1;
    memcpy(R.toks, toks, sizeof(int) * (size_t)n);
    R.n = n; R.max_new = max_new; R.s = *s;
    R.stop = 0; R.fed = 0; R.nout = 0;
    R.t0 = now_us(); R.t_first = R.t_end = R.t_fed = 0;
    memset(llm_stats(E.m), 0, sizeof(llm_stats_t));
    R.done = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    E.state = EN_BUSY;
    R.solo = core_run(LEADER, leader, 0) != 0;
    if (R.solo) { llm_set_par(E.m, 0, 1); R.solo_gen = 0; }
    return 0;
}

/* With no core to give it to (QEMU with one CPU): a little each turn, here. */
static void solo_step(void) {
    uint64_t end = now_us() + 30000;
    while (now_us() < end && !R.done) {
        if (R.fed < R.n && !R.stop) {
            int k = R.n - R.fed < 4 ? R.n - R.fed : 4;
            int fed = llm_feed(E.m, R.toks + R.fed, k, &R.stop);
            R.fed += fed;
            if (fed < k) R.fed = R.n;
            if (R.fed >= R.n) R.t_fed = now_us();
            continue;
        }
        if (R.max_new > 0 && gen_step()) continue;
        R.t_end = now_us();
        R.done = 1;
    }
}

void engine_stop(void) { R.stop = 1; sev(); }
int  engine_done(void) { return __atomic_load_n(&R.done, __ATOMIC_ACQUIRE); }
int  engine_out(const int **t) { if (t) *t = R.out; return __atomic_load_n(&R.nout, __ATOMIC_ACQUIRE); }
int  engine_fed(void) { return __atomic_load_n(&R.fed, __ATOMIC_ACQUIRE); }
int  engine_req_len(void) { return R.n; }
float engine_rate(void) {
    int n = engine_out(0);
    uint64_t t1 = R.t_end ? R.t_end : now_us();
    if (n < 2 || !R.t_first || t1 <= R.t_first) return 0;
    return (float)(n - 1) * 1e6f / (float)(t1 - R.t_first);
}
float engine_prompt_rate(void) {
    if (!R.t_fed || R.t_fed <= R.t0 || R.n < 1) return 0;
    return (float)R.n * 1e6f / (float)(R.t_fed - R.t0);
}

/* ---- measuring (dev: /app?localai&bench) ------------------------------------------------------- *
 *  Each format's biggest matrix times a vector, on one core and on the
 *  leader with its helpers, and plain reading speed over the file. */
static struct { char *out; int cap, n; volatile int done; } B;

typedef struct { const uint8_t *p; uint64_t n; volatile uint64_t sum[4]; } rd_t;
static void read_part(void *arg, int part, int nparts) {
    rd_t *r = arg;
    uint64_t a = r->n * (uint64_t)part / (uint64_t)nparts & ~63ull, b = r->n * (uint64_t)(part + 1) / (uint64_t)nparts & ~63ull;
    uint64x2_t acc = vdupq_n_u64(0);
    for (uint64_t i = a; i < b; i += 64) {
        uint8x16_t x = vld1q_u8(r->p + i), y = vld1q_u8(r->p + i + 16), z = vld1q_u8(r->p + i + 32), w = vld1q_u8(r->p + i + 48);
        acc = vaddq_u64(acc, vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(veorq_u8(veorq_u8(x, y), veorq_u8(z, w))))));
    }
    r->sum[part & 3] = vgetq_lane_u64(acc, 0) + vgetq_lane_u64(acc, 1);
}

typedef struct { const gtensor_t *t; aq_t *xq; float *xf, *y; } mv_t;
static void mv_part(void *arg, int part, int nparts) {
    mv_t *j = arg;
    int n = (int)j->t->ne[0], rows = (int)j->t->ne[1];
    int r0 = (int)((int64_t)rows * part / nparts), r1 = (int)((int64_t)rows * (part + 1) / nparts);
    if (j->t->type & GT_X4) {
        uint64_t gb = 4 * row_bytes(j->t->type & 0xFF, n);
        const aq_t *xs[4] = { j->xq, j->xq, j->xq, j->xq };
        for (int g = r0 / 4; g < r1 / 4; g++) {
            float o[4][4];
            mm_x4(j->t->type, j->t->data + gb * (uint64_t)g, n, xs, 1, o);
            memcpy(j->y + 4 * g, o[0], 16);
        }
        return;
    }
    uint64_t rb = row_bytes(j->t->type, n);
    for (int r = r0; r < r1; r++) j->y[r] = dot_row(j->t->type, j->t->data + rb * (uint64_t)r, j->xq, j->xf, n);
}

static void nop_part(void *arg, int part, int nparts) { (void)arg; (void)part; (void)nparts; }

static void bench_job(void *arg) {
    (void)arg;
    llm_t *m = E.m;
    static float xf[8192];
    static aq_t xq[256];
    float *y = kmalloc(4ull * 160000);
    if (!y) { B.n = snprintf(B.out, (size_t)B.cap, "out of memory\n"); __atomic_store_n(&B.done, 1, __ATOMIC_RELEASE); return; }
    for (int i = 0; i < 8192; i++) xf[i] = (float)((i * 7919) % 200 - 100) / 100.0f;
    helpers_start(LEADER);
    int seen[512] = { 0 };
    for (int i = 0; i < m->nt; i++) {
        const gtensor_t *t = &m->t[i];
        if (t->nd < 2 || (int)t->ne[0] > 8192) continue;
        int best = i;
        for (int k = 0; k < m->nt; k++) if (m->t[k].type == t->type && m->t[k].nd >= 2 && m->t[k].bytes > m->t[best].bytes && (int)m->t[k].ne[0] <= 8192) best = k;
        if (seen[t->type & 511]++) continue;
        t = &m->t[best];
        quantize_act(xf, (int)t->ne[0], xq);
        mv_t j = { t, xq, xf, y };
        double macs = (double)t->ne[0] * (double)t->ne[1];
        uint64_t a = now_us(); mv_part(&j, 0, 1); uint64_t b = now_us();
        par(mv_part, &j); uint64_t c = now_us();
        B.n += snprintf(B.out + B.n, (size_t)(B.cap - B.n), "type %2d %-24.*s %5.1f MB: 1 core %6.1f ms (%.2f GMAC/s, %.2f GB/s); %d cores %6.1f ms (%.2f GMAC/s, %.2f GB/s)\n",
                        t->type, t->name_len, t->name, t->bytes / 1e6, (b - a) / 1e3, macs / (b - a) / 1e3, t->bytes / (double)(b - a) / 1e3,
                        1 + nactive, (c - b) / 1e3, macs / (c - b) / 1e3, t->bytes / (double)(c - b) / 1e3);
    }
    static void (*volatile nop)(void *, int, int);
    nop = 0;
    uint64_t pa = now_us();
    for (int k = 0; k < 2000; k++) par(nop_part, 0);
    uint64_t pb = now_us();
    B.n += snprintf(B.out + B.n, (size_t)(B.cap - B.n), "an empty hand-off to %d cores: %.1f us\n", 1 + nactive, (pb - pa) / 2000.0);
    (void)nop;
    rd_t r = { E.hi, (E.split ? E.size - E.x0 : E.size) & ~63ull, { 0 } };
    uint64_t a = now_us(); read_part(&r, 0, 1); uint64_t b = now_us();
    par(read_part, &r); uint64_t c = now_us();
    B.n += snprintf(B.out + B.n, (size_t)(B.cap - B.n), "reading %lu MB: 1 core %.2f GB/s, %d cores %.2f GB/s\n",
                    (unsigned long)(r.n >> 20), r.n / (double)(b - a) / 1e3, 1 + nactive, r.n / (double)(c - b) / 1e3);
    helpers_stop();
    kfree(y);
    __atomic_store_n(&B.done, 1, __ATOMIC_RELEASE);
}

int engine_bench(char *out, int cap) {
    if (E.state != EN_READY || !R.done) return snprintf(out, (size_t)cap, "no model loaded, or busy\n");
    B.out = out; B.cap = cap; B.n = 0; B.done = 0;
    if (core_run(LEADER, bench_job, 0) != 0) return snprintf(out, (size_t)cap, "no core\n");
    uint64_t end = now_ms() + 60000;
    while (!B.done && now_ms() < end) mini_pump();
    return B.n;
}

/* ---- loading -------------------------------------------------------------------------------- */
static void fail(const char *why) {
    snprintf(E.err, sizeof E.err, "%s", why);
    kprintf("[ai] %s\n", why);
    engine_unload();
    E.state = EN_FAILED;
}

static void release_high(void);

int engine_load(int i) {
    if (E.state == EN_LOADING || E.state == EN_BUSY) return -1;
    if (model_state(i, 0, 0, 0) != MS_READY) {                 /* keep what's loaded */
        snprintf(E.err, sizeof E.err, "That model isn't on the SD card yet");
        return -1;
    }
    engine_unload();
    char path[48];
    model_path(i, path, sizeof path);
    E.e = store_find(path);
    if (!E.e || !(E.e->flags & STORE_DONE)) { fail("That model isn't on the SD card yet"); return -1; }
    E.size = E.e->size;
    if (E.size > MINI_HIGH_SIZE + MINI_LOW_SIZE / 2) { fail("That model is too big for this Pi's memory"); return -1; }
    E.hi = bigmem_claim(1, "Local AI", release_high);
    if (!E.hi) {
        snprintf(E.err, sizeof E.err, "The memory it needs is in use by %s", bigmem_owner(1) ? bigmem_owner(1) : "something");
        E.state = EN_FAILED; return -1;
    }
    E.have_high = 1;
    E.lo = bigmem_claim(0, "Local AI", 0);
    if (!E.lo) { fail("The working memory it needs is in use"); return -1; }
    E.have_low = 1;
    /* Bigger than the high window: the start of the file goes low, ahead of
     * the working memory; where to cut is known once the header is in. */
    E.split = E.size > MINI_HIGH_SIZE;
    E.x = E.x0 = 0;
    E.lo_len = E.split ? (16u << 20) : 0;
    E.lgot = E.hgot = 0;
    E.model = i;
    E.state = EN_LOADING;
    E.err[0] = 0;
    kprintf("[ai] loading %s (%lu MiB) from the card%s\n", ai_models[i].name, (unsigned long)(E.size >> 20),
            E.split ? ", in two parts" : "");
    return 0;
}

static uint64_t load_t0, load_read_ms;
static struct { volatile int done; char err[160]; } O;

static void open_job(void *arg) {
    (void)arg;
    const model_t *md = &ai_models[E.model];
    llm_t *m = E.split ? llm_open_split(E.lo, E.lo_len, E.hi, E.x, E.size, &E.arena, md->ctx, O.err, sizeof O.err)
                       : llm_open(E.hi, E.size, &E.arena, md->ctx, O.err, sizeof O.err);
    if (m) {
        helpers_start(LEADER);
        llm_set_par(m, par, 1 + nactive);
        if (llm_prepare(m, &E.arena)) { snprintf(O.err, sizeof O.err, "Not enough memory to get the model ready"); m = 0; }
        helpers_stop();
    }
    E.m = m;
    __atomic_store_n(&O.done, m ? 1 : -1, __ATOMIC_RELEASE);
    sev();
}

static uint64_t file_end(void) { return (E.size + 511) & ~511ull; }
static int all_read(void) { return E.x0 + E.hgot >= file_end() && (!E.split || (E.x && E.lgot >= E.lo_len)); }

static void load_step(void) {
    if (!E.lgot && !E.hgot) load_t0 = now_ms();
    if (E.split && E.lgot < E.lo_len) {
        uint32_t n = E.lo_len - E.lgot < CHUNK ? (uint32_t)(E.lo_len - E.lgot) : CHUNK;
        if (store_read(E.e, E.lgot, E.lo + E.lgot, n) != 0) { fail("The SD card wouldn't read the model"); return; }
        E.lgot += n;
        if (E.lgot == E.lo_len && !E.x) {                          /* the header is in: where to cut */
            E.x = llm_split_point(E.lo, E.lgot, E.size, MINI_HIGH_SIZE);
            if (!E.x) { fail("That model can't be laid out in this Pi's memory"); return; }
            E.x0 = E.x & ~511ull;
            uint64_t ll = (E.x + 511) & ~511ull;
            if (ll > E.lo_len) E.lo_len = ll;
            if (E.lo_len > MINI_LOW_SIZE / 2) { fail("That model is too big for this Pi's memory"); return; }
        }
        return;
    }
    if (E.x0 + E.hgot < file_end()) {
        uint64_t left = file_end() - E.x0 - E.hgot;
        uint32_t n = left < CHUNK ? (uint32_t)left : CHUNK;
        if (store_read(E.e, E.x0 + E.hgot, E.hi + E.hgot, n) != 0) { fail("The SD card wouldn't read the model"); return; }
        E.hgot += n;
        if (!all_read()) return;
    }
    load_read_ms = now_ms() - load_t0;
    uint64_t lo_used = E.split ? (E.lo_len + 63) & ~63ull : 0;
    E.arena.base = E.lo + lo_used; E.arena.size = MINI_LOW_SIZE - lo_used; E.arena.used = 0;
    /* Open it and rearrange its weights on the other cores (a second or so
     * of work); here if there are none. */
    O.done = 0;
    if (core_run(LEADER, open_job, 0) != 0) open_job(0);
}

static void opened(void) {
    if (O.done < 0) { fail(O.err); return; }
    E.state = EN_READY;
    const model_t *md = &ai_models[E.model];
    const llm_info_t *I = llm_info(E.m);
    kprintf("[ai] %s is loaded: %lu MiB read in %lu ms, made ready in %lu ms, %lu MiB of working memory, %d tokens of context\n",
            md->name, (unsigned long)(E.size >> 20), (unsigned long)load_read_ms,
            (unsigned long)(now_ms() - load_t0 - load_read_ms), (unsigned long)(E.arena.used >> 20), I->ctx);
}

void engine_unload(void) {
    if (E.state == EN_LOADING && all_read()) {                     /* the open job may be running */
        uint64_t end = now_ms() + 10000;
        while (!__atomic_load_n(&O.done, __ATOMIC_ACQUIRE) && now_ms() < end) mini_pump();
    }
    if (E.state == EN_BUSY || !R.done) {
        engine_stop();
        uint64_t end = now_ms() + 5000;
        while (!engine_done() && now_ms() < end) { if (R.solo) solo_step(); mini_pump(); }
    }
    if (E.have_high) bigmem_free(1);
    if (E.have_low) bigmem_free(0);
    E.have_high = E.have_low = 0;
    E.m = 0; E.lo = E.hi = 0; E.lgot = E.hgot = 0; E.x = E.x0 = 0; E.split = 0;
    E.model = -1;
    E.state = EN_NONE;
}

/* A video wants the high window back. */
static void release_high(void) {
    int was = E.model;
    E.have_high = 0;                               /* bigmem.c takes it back itself */
    engine_unload();
    if (was >= 0) { snprintf(E.err, sizeof E.err, "The model was put away to play a video; it loads again when you come back here."); E.state = EN_FAILED; E.model = was; }
}

int engine_poll(void) {
    if (E.state == EN_LOADING) {
        if (!all_read()) load_step();
        else if (__atomic_load_n(&O.done, __ATOMIC_ACQUIRE)) opened();
        return 1;
    }
    if (E.state == EN_BUSY) {
        if (R.solo) solo_step();
        if (engine_done()) E.state = EN_READY;
        return 1;
    }
    return 0;
}

int engine_state(void) { return E.state; }
int engine_model(void) { return E.model; }
const char *engine_error(void) { return E.err; }
int engine_load_pct(void) {
    uint64_t need = (E.split ? E.lo_len : 0) + file_end() - E.x0;
    return E.size && need ? (int)((E.lgot + E.hgot) * 100 / need) : 0;
}
llm_t *engine_llm(void) { return E.state == EN_READY || E.state == EN_BUSY ? E.m : 0; }
int engine_cores(void) {
    int n = 1;
    for (int c = 1; c < 4; c++) if (core_pooled(c) && (c == LEADER || !core_busy(c) || HA[c].running)) n++;
    return n - 1 > 0 ? n - 1 : 1;
}
