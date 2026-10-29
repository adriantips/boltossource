/* ===========================================================================
 *  BoltOS Mini  -  mini/rt.c
 *
 *  What a program run from the Code app stands on (rt.h): its region of
 *  memory, its core, its stack, its heap and collector, and the mailbox
 *  between it and the boot core.
 *
 *  The heap is a "big bag of pages": 4 KiB pages, each holding objects of
 *  one size (16 bytes to 2 KiB), and larger objects on runs of whole pages.
 *  The size of anything is known from its page, so any address inside an
 *  object finds the object's start, and the collector can be conservative:
 *  it marks from the program's stack, its registers and a few roots, and
 *  treats every word that points into the heap as a pointer. Nothing in the
 *  interpreters has to say what's a pointer, and nothing is moved.
 *
 *  Between the cores: the output is a ring the program fills and the boot
 *  core drains (each index written by one side only, with acquire/release,
 *  never an atomic read-modify-write on a line the other core writes); a
 *  request slot carries input() and file reads and writes the other way.
 * ===========================================================================*/
#include <stdint.h>
#include <stdarg.h>
#include "mini.h"
#include "rt.h"
#include "fs.h"
#include "pkg.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

/* ---- setjmp/longjmp, and a call on another stack ----------------------------------------------- */
__asm__(
    ".text\n.global rt_setjmp\n.type rt_setjmp,%function\nrt_setjmp:\n"
    "  stp x19, x20, [x0, #0]\n  stp x21, x22, [x0, #16]\n  stp x23, x24, [x0, #32]\n"
    "  stp x25, x26, [x0, #48]\n  stp x27, x28, [x0, #64]\n  stp x29, x30, [x0, #80]\n"
    "  mov x2, sp\n  str x2, [x0, #96]\n"
    "  stp d8, d9, [x0, #104]\n  stp d10, d11, [x0, #120]\n  stp d12, d13, [x0, #136]\n  stp d14, d15, [x0, #152]\n"
    "  mov x0, #0\n  ret\n"
    ".global rt_longjmp\n.type rt_longjmp,%function\nrt_longjmp:\n"
    "  ldp x19, x20, [x0, #0]\n  ldp x21, x22, [x0, #16]\n  ldp x23, x24, [x0, #32]\n"
    "  ldp x25, x26, [x0, #48]\n  ldp x27, x28, [x0, #64]\n  ldp x29, x30, [x0, #80]\n"
    "  ldr x2, [x0, #96]\n  mov sp, x2\n"
    "  ldp d8, d9, [x0, #104]\n  ldp d10, d11, [x0, #120]\n  ldp d12, d13, [x0, #136]\n  ldp d14, d15, [x0, #152]\n"
    "  mov x0, x1\n  cbnz x0, 1f\n  mov x0, #1\n1: br x30\n"
    /* rt_on_stack(fn, arg, top): fn(arg) with sp = top, then back. */
    ".global rt_on_stack\n.type rt_on_stack,%function\nrt_on_stack:\n"
    "  stp x29, x30, [sp, #-16]!\n  mov x29, sp\n"
    "  mov x3, sp\n  mov sp, x2\n  sub sp, sp, #16\n  str x3, [sp]\n"
    "  mov x4, x0\n  mov x0, x1\n  blr x4\n"
    "  ldr x3, [sp]\n  mov sp, x3\n  ldp x29, x30, [sp], #16\n  ret\n");
void rt_on_stack(void (*fn)(void *), void *arg, void *top);

static inline uintptr_t cur_sp(void) { uintptr_t s; __asm__ volatile("mov %0, sp" : "=r"(s)); return s; }

/* ---- the shared state --------------------------------------------------------------------------- */
#define RING      (64 * 1024)
#define XBUF      (1024 * 1024)                  /* files to and from the program  */
#define STACK     (8u << 20)
#define ERRCAP    3072

enum { RQ_NONE, RQ_INPUT, RQ_READ, RQ_WRITE, RQ_APPEND };

static struct {
    /* written by the boot core */
    volatile int      stop __attribute__((aligned(64)));
    volatile uint32_t tail;                      /* output consumed up to here      */
    volatile int      rq_done;                   /* the request's answer is in      */
    int               rq_result;
    uint32_t          rq_len;
    char              rq_why[96];
    /* written by the program's core */
    volatile uint32_t head __attribute__((aligned(64)));
    volatile int      state;
    volatile int      rq;                        /* RQ_*: a request is waiting      */
    char              rq_path[FS_PATH];
    char              prompt[160];
    int               ok, err_line;
    char              err[ERRCAP];
    /* set before the start, read by both */
    int               lang, cpu;
    uint8_t          *base;  uint64_t size;  int big;
    char             *src;   uint32_t src_len;
    char              path[FS_PATH], dir[FS_PATH];
    char             *ring, *xbuf;
    uint8_t          *stack_lo, *stack_top;
    uint64_t          t0, t1;
    rt_jmp_t          top;
} R;

static void bump_state(int s) { __atomic_store_n(&R.state, s, __ATOMIC_RELEASE); }
int run_state(void) { return __atomic_load_n(&R.state, __ATOMIC_ACQUIRE); }

/* ---- the heap: mini/gc.c --------------------------------------------------------------------------- */
void gc_init(uint8_t *base, uint64_t size, uint8_t *stack_top);
int  gc_debug(char *out, int cap);

/* ---- the program's side: output, input, files ---------------------------------------------------- */
int rt_stopping(void) { return __atomic_load_n(&R.stop, __ATOMIC_ACQUIRE); }

void rt_check(void) {
    if (rt_stopping()) {
        R.ok = 0; R.err_line = 0;
        snprintf(R.err, sizeof R.err, "Stopped");
        rt_longjmp(&R.top, 2);
    }
}

void rt_write(const char *s, size_t n) {
    while (n) {
        uint32_t h = R.head, t = __atomic_load_n(&R.tail, __ATOMIC_ACQUIRE);
        uint32_t room = RING - 1 - (h - t);
        if (!room) { rt_check(); delay_us(200); continue; }
        uint32_t k = n < room ? (uint32_t)n : room;
        for (uint32_t i = 0; i < k; i++) R.ring[(h + i) % RING] = s[i];
        __atomic_store_n(&R.head, h + k, __ATOMIC_RELEASE);
        s += k; n -= k;
    }
}

void rt_puts(const char *s) { rt_write(s, strlen(s)); }

void rt_printf(const char *fmt, ...) {
    char b[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
    if (n > 0) rt_write(b, (size_t)n);
}

/* Hand a request to the boot core and wait for its answer. */
static int request(int kind) {
    __atomic_store_n(&R.rq_done, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&R.rq, kind, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&R.rq_done, __ATOMIC_ACQUIRE)) {
        if (rt_stopping()) { __atomic_store_n(&R.rq, RQ_NONE, __ATOMIC_RELEASE); rt_check(); }
        delay_us(300);
    }
    __atomic_store_n(&R.rq, RQ_NONE, __ATOMIC_RELEASE);
    return R.rq_result;
}

int rt_readline(const char *prompt, char *buf, int cap) {
    snprintf(R.prompt, sizeof R.prompt, "%s", prompt ? prompt : "");
    if (prompt && *prompt) rt_puts(prompt);
    /* The prompt has to be on screen before the wait. */
    while (__atomic_load_n(&R.tail, __ATOMIC_ACQUIRE) != R.head) { rt_check(); delay_us(300); }
    bump_state(RUN_INPUT);
    int r = request(RQ_INPUT);
    bump_state(RUN_RUNNING);
    if (r < 0) return -1;
    int n = (int)R.rq_len < cap - 1 ? (int)R.rq_len : cap - 1;
    memcpy(buf, R.xbuf, (size_t)n);
    buf[n] = 0;
    return n;
}

int rt_file_read(const char *path, char **data, uint32_t *len, const char **why) {
    snprintf(R.rq_path, sizeof R.rq_path, "%s", path);
    if (request(RQ_READ) < 0) { if (why) *why = R.rq_why; return -1; }
    char *b = rt_alloc_leaf(R.rq_len + 1);
    memcpy(b, R.xbuf, R.rq_len);
    b[R.rq_len] = 0;
    *data = b; if (len) *len = R.rq_len;
    return 0;
}

int rt_file_write(const char *path, const char *data, uint32_t len, int append, const char **why) {
    if (len > XBUF) { if (why) *why = "files a program writes are up to 1 MiB"; return -1; }
    snprintf(R.rq_path, sizeof R.rq_path, "%s", path);
    memcpy(R.xbuf, data, len);
    R.rq_len = len;
    if (request(append ? RQ_APPEND : RQ_WRITE) < 0) { if (why) *why = R.rq_why; return -1; }
    return 0;
}

const char *rt_cwd(void) { return R.dir; }
uint64_t rt_ms(void) { return now_ms(); }
void rt_sleep_ms(uint64_t ms) {
    uint64_t end = now_ms() + ms;
    while (now_ms() < end) { rt_check(); delay_us(500); }
}

int rt_stack_left(void) { return (int)(cur_sp() - (uintptr_t)R.stack_lo); }

static uint64_t rng = 0x9E3779B97F4A7C15ull;
void rt_seed(uint64_t s) { rng = s ? s : 0x9E3779B97F4A7C15ull; }
uint32_t rt_rand(void) {
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return (uint32_t)((rng * 0x2545F4914F6CDD1Dull) >> 32);
}

void rt_fail(int line, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(R.err, sizeof R.err, fmt, ap);
    va_end(ap);
    R.ok = 0; R.err_line = line;
    rt_longjmp(&R.top, 1);
}

/* ---- the program's core -------------------------------------------------------------------------- */
static void on_stack(void *arg) {
    (void)arg;
    if (rt_setjmp(&R.top) == 0) {
        int r = R.lang == LANG_PY ? py_main(R.src, R.path) : cc_main(R.src, R.path, R.lang == LANG_CPP);
        R.ok = r == 0;
        if (r && !R.err[0]) snprintf(R.err, sizeof R.err, "The program ended with exit code %d", r);
    }
    R.t1 = now_ms();
}

static void job(void *arg) {
    (void)arg;
    bump_state(RUN_RUNNING);
    rt_on_stack(on_stack, 0, R.stack_top);
    bump_state(RUN_DONE);
}

/* ---- the boot core's side ------------------------------------------------------------------------ */
static void give_back(void);

static void release_cb(void) {
    /* Local AI wants the window: the program stops, and must be out of it. */
    run_stop();
    for (int i = 0; i < 2000 && run_state() != RUN_DONE; i++) delay_ms(1);
    give_back();
}

static void give_back(void) {
    if (!R.base) return;
    if (R.big) bigmem_free(0); else kfree(R.base);
    R.base = 0;
}

int lang_of(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return LANG_NONE;
    if (!strcasecmp(dot, ".py")) return LANG_PY;
    if (!strcasecmp(dot, ".c") || !strcasecmp(dot, ".h")) return LANG_C;
    if (!strcasecmp(dot, ".cpp") || !strcasecmp(dot, ".cc") || !strcasecmp(dot, ".cxx") || !strcasecmp(dot, ".hpp")) return LANG_CPP;
    return LANG_NONE;
}

const char *lang_name(int l) { return l == LANG_PY ? "Python" : l == LANG_C ? "C" : l == LANG_CPP ? "C++" : "Text"; }

int run_start(int lang, const char *src, uint32_t len, const char *path) {
    int st = run_state();
    if (st == RUN_STARTING || st == RUN_RUNNING || st == RUN_INPUT) { snprintf(R.err, sizeof R.err, "A program is still running"); return -1; }
    if (lang != LANG_PY && lang != LANG_C && lang != LANG_CPP) { snprintf(R.err, sizeof R.err, "Only .py, .c and .cpp files run"); return -1; }
    give_back();
    /* Memory: the low window if nobody has it, or a piece of the heap. */
    R.base = bigmem_claim(0, "Code", release_cb);
    R.big = R.base != 0;
    R.size = MINI_LOW_SIZE;
    if (!R.base) {
        static const uint32_t tries[] = { 48u << 20, 32u << 20, 20u << 20 };
        for (unsigned i = 0; i < 3 && !R.base; i++) { R.base = kmalloc(tries[i]); R.size = tries[i]; }
        if (!R.base) { snprintf(R.err, sizeof R.err, "There isn't memory free to run a program"); return -1; }
    }
    /* stack | ring | xbuf | source | heap */
    uint8_t *p = R.base;
    R.stack_lo = p; R.stack_top = p + STACK; p += STACK;
    R.ring = (char *)p; p += RING;
    R.xbuf = (char *)p; p += XBUF;
    R.src = (char *)p; memcpy(R.src, src, len); R.src[len] = 0; R.src_len = len;
    p += (len + 1 + 4095) & ~4095u;
    gc_init(p, R.size - (uint64_t)(p - R.base), R.stack_top);
    snprintf(R.path, sizeof R.path, "%s", path ? path : "/Code/untitled");
    snprintf(R.dir, sizeof R.dir, "%s", R.path);
    char *slash = strrchr(R.dir, '/');
    if (slash && slash != R.dir) *slash = 0; else strcpy(R.dir, "/");
    R.lang = lang;
    R.stop = 0; R.head = R.tail = 0; R.rq = RQ_NONE; R.rq_done = 0;
    R.ok = 0; R.err[0] = 0; R.err_line = 0; R.prompt[0] = 0;
    rt_seed(now_us() ^ 0xB0175EED);
    R.t0 = now_ms(); R.t1 = 0;
    bump_state(RUN_STARTING);
    dsb();
    /* A core: the last ones first, as video takes core 1. */
    static const int order[] = { 3, 2, 1 };
    for (int i = 0; i < 3; i++) {
        int c = order[i];
        if (!core_pooled(c) || core_busy(c) || core_wanted(c)) continue;
        if (core_run(c, job, 0) == 0) { R.cpu = c; return 0; }
    }
    bump_state(RUN_IDLE);
    give_back();
    snprintf(R.err, sizeof R.err, "Every spare core is busy (a video or Local AI?)");
    return -1;
}

void run_stop(void) { __atomic_store_n(&R.stop, 1, __ATOMIC_RELEASE); }

int run_output(char *out, int cap) {
    if (!R.ring) return 0;
    uint32_t h = __atomic_load_n(&R.head, __ATOMIC_ACQUIRE), t = R.tail;
    int n = 0;
    while (t != h && n < cap) out[n++] = R.ring[t++ % RING];
    __atomic_store_n(&R.tail, t, __ATOMIC_RELEASE);
    return n;
}

void run_input(const char *line) {
    if (__atomic_load_n(&R.rq, __ATOMIC_ACQUIRE) != RQ_INPUT || R.rq_done) return;
    uint32_t n = (uint32_t)strlen(line);
    if (n > 4000) n = 4000;
    memcpy(R.xbuf, line, n);
    R.rq_len = n; R.rq_result = 0;
    __atomic_store_n(&R.rq_done, 1, __ATOMIC_RELEASE);
}

int run_poll(void) {
    int st = run_state(), news = 0;
    int rq = __atomic_load_n(&R.rq, __ATOMIC_ACQUIRE);
    if ((rq == RQ_READ || rq == RQ_WRITE || rq == RQ_APPEND) && !R.rq_done) {
        R.rq_result = 0; R.rq_why[0] = 0;
        if (rq == RQ_READ) {
            char *b; uint32_t n;
            if (fs_read(R.rq_path, &b, &n)) { R.rq_result = -1; snprintf(R.rq_why, sizeof R.rq_why, "%s", fs_why()); }
            else if (n > XBUF) { kfree(b); R.rq_result = -1; snprintf(R.rq_why, sizeof R.rq_why, "files a program reads are up to 1 MiB"); }
            else { memcpy(R.xbuf, b, n); R.rq_len = n; kfree(b); }
        } else {
            char *old = 0; uint32_t on = 0;
            if (rq == RQ_APPEND && fs_read(R.rq_path, &old, &on) != 0) { old = 0; on = 0; }
            char *all = kmalloc(on + R.rq_len + 1);
            if (!all) { R.rq_result = -1; snprintf(R.rq_why, sizeof R.rq_why, "out of memory"); }
            else {
                if (on) memcpy(all, old, on);
                memcpy(all + on, R.xbuf, R.rq_len);
                if (fs_write(R.rq_path, all, on + R.rq_len)) { R.rq_result = -1; snprintf(R.rq_why, sizeof R.rq_why, "%s", fs_why()); }
                kfree(all);
            }
            kfree(old);
            news = 1;                                /* a file changed: the Code app's list */
        }
        __atomic_store_n(&R.rq_done, 1, __ATOMIC_RELEASE);
    }
    if (st == RUN_DONE && R.base) {
        /* Everything it printed is read before the memory goes back. */
        if (__atomic_load_n(&R.head, __ATOMIC_ACQUIRE) == R.tail) { give_back(); news = 1; }
    }
    if (R.ring && __atomic_load_n(&R.head, __ATOMIC_ACQUIRE) != R.tail) news = 1;
    return news;
}

const char *run_message(void) {
    static char m[64];
    int st = run_state();
    if (st == RUN_RUNNING || st == RUN_STARTING) return "Running";
    if (st == RUN_INPUT) return "Waiting for input";
    if (st == RUN_DONE && R.ok) {
        uint64_t ms = R.t1 - R.t0;
        snprintf(m, sizeof m, "Finished in %lu.%02lu s", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 10));
        return m;
    }
    return R.err;
}

int run_ok(void) { return run_state() == RUN_DONE && R.ok; }
int run_error_line(void) { return R.err_line; }
const char *run_prompt(void) { return R.prompt; }
uint64_t run_elapsed_ms(void) {
    int st = run_state();
    if (st == RUN_IDLE) return 0;
    return (R.t1 ? R.t1 : now_ms()) - R.t0;
}

/* Development: GET /run?file=/Code/x.py runs a file, ?status says how it went
 * (with what it printed), ?input=... answers input(), ?stop stops it. */
int run_dev(const char *cmd, char *out, int cap) {
    static char acc[16384]; static int acc_n;
    int n0 = run_output(acc + acc_n, (int)sizeof acc - 1 - acc_n);
    acc_n += n0; acc[acc_n] = 0;
    if (!strncmp(cmd, "file=", 5)) {
        char *b; uint32_t n;
        if (fs_read(cmd + 5, &b, &n)) return snprintf(out, (size_t)cap, "can't read %s: %s\n", cmd + 5, fs_why());
        acc_n = 0; acc[0] = 0;
        int r = run_start(lang_of(cmd + 5), b, n, cmd + 5);
        kfree(b);
        if (r) return snprintf(out, (size_t)cap, "not started: %s\n", R.err);
        return snprintf(out, (size_t)cap, "started on core %d\n", R.cpu);
    }
    if (!strncmp(cmd, "input=", 6)) { run_input(cmd + 6); return snprintf(out, (size_t)cap, "sent\n"); }
    if (!strcmp(cmd, "stop")) { run_stop(); return snprintf(out, (size_t)cap, "stopping\n"); }
    int st = run_state();
    int n = snprintf(out, (size_t)cap, "state %d (%s), %lu ms, line %d, ", st, run_message(), (unsigned long)run_elapsed_ms(), R.err_line);
    n += gc_debug(out + n, cap - n);
    n += snprintf(out + n, (size_t)(cap - n), "--- output ---\n%s", acc);
    return n;
}

/* Until a C interpreter is built in (cc.c). */
__attribute__((weak)) int cc_main(const char *src, const char *path, int cpp) {
    (void)src; (void)path; (void)cpp;
    rt_fail(0, "C and C++ aren't in this build");
}
