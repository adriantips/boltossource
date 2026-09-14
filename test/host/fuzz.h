/* The bits every fuzz harness needs, and nothing that belongs to one of them.
 *
 * There is no libFuzzer here and no AddressSanitizer either -- the clang that
 * builds this project is a mingw-w64 one, and it ships neither
 * (`-fsanitize=fuzzer` is "unsupported option for target x86_64-w64-windows-gnu"
 * and the asan runtime library is simply not in the package). So the two halves
 * of a fuzzer that actually finds things have to be written out:
 *
 *   the mutator   fz_mutate(), below: a corpus, a splice, and the byte values
 *                 that break parsers more often than random ones do
 *   the detector  the guarded heap, below: every kmalloc gets redzones and a
 *                 serial, every kfree checks them, freed memory is poisoned
 *                 and quarantined, and a leak is counted
 *
 * The third thing a sanitizer gives you -- traps on undefined behaviour -- does
 * come from clang: -fsanitize=undefined,bounds -fsanitize-trap=all needs no
 * runtime library, and turns a signed overflow or a fixed-array overrun into a
 * SIGILL we catch. That covers the `char buf[64]` cases, which the guarded heap
 * cannot see because they are on the stack.
 *
 * Everything is derived from one 64-bit seed, so a crash is reproducible by
 * printing three numbers, and the harness saves the exact bytes to
 * test/out/ as well, because a corpus entry beats a seed that stops being
 * valid the moment the mutator changes. */
#ifndef BOLTOS_FUZZ_H
#define BOLTOS_FUZZ_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>

/* ------------------------------------------------------------------ random --
 * xorshift64*, because the sequence has to be identical on every machine that
 * reruns a seed and rand() is not. */
static uint64_t fz_state = 0x2545F4914F6CDD1Dull;

static void fz_srand(uint64_t s) { fz_state = s ? s : 0x9E3779B97F4A7C15ull; }

static uint64_t fz_rand(void) {
    uint64_t x = fz_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    fz_state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static uint32_t fz_below(uint32_t n) { return n ? (uint32_t)(fz_rand() % n) : 0; }

/* ------------------------------------------------------------- guarded heap --
 * The kernel's own heap is in kernel/kheap.c and has test/host/heap.c to prove
 * it. This is not that: it is malloc with the instrumentation a fuzzer needs,
 * standing in for kmalloc so an overflow by one byte is a failure here instead
 * of a corrupted neighbour that shows up three allocations later. */
#define FZ_RED       32              /* redzone bytes each side               */
#define FZ_QUAR      512             /* freed blocks held before release      */
#define FZ_LIVE      0x4C495645u
#define FZ_DEAD      0x44454144u

typedef struct fz_blk {
    uint64_t size;
    uint32_t serial;
    uint32_t state;
    struct fz_blk *prev, *next;
} fz_blk;

static fz_blk  *fz_live_list;
static uint32_t fz_serial;
static uint64_t fz_bytes_live, fz_bytes_cap = 96u << 20;
static uint64_t fz_alloc_calls;
static fz_blk  *fz_quar[FZ_QUAR];
static int      fz_quar_head;

/* Allocation-failure injection. A parser that handles a full heap by
 * dereferencing the NULL it just got is a real bug on a machine with 64 MB of
 * RAM and a page that is 30 MB of markup, and nothing else finds those. */
static int64_t  fz_oom_in = -1;      /* <0 = never fail; 0 = fail from now on */
static uint64_t fz_oom_hits;
static int      fz_no_oom;           /* --no-oom, for telling the two apart   */
static int      fz_poison = 1;       /* --no-poison, likewise                 */
/* --trace names each input before it runs it. A stack overflow kills the
 * process without the signal handler getting a stack to run on, so the only
 * thing that says which input did it is a line printed beforehand. */
static int      fz_trace;
/* --keep writes each input to test/out/pending-TARGET.bin before running
 * it. For a stack overflow, which kills the process without the handler
 * getting a stack to report from, that file is the only evidence. */
static int      fz_keep;
/* An input is only half of what a failure needs: the other half is how many
 * allocations in the heap started refusing, because most of what turns up in
 * here is an OOM path. Both are reported, and --oom replays the pair. */
static int64_t  fz_oom_force = -2;   /* -2 = let the run choose */
static int64_t  fz_oom_chosen = -1;

static const char *fz_fault;         /* set by a check that failed            */
static char        fz_fault_buf[512];

static void fz_fail(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(fz_fault_buf, sizeof fz_fault_buf, fmt, ap);
    va_end(ap);
    fz_fault = fz_fault_buf;
}

/* A redzone pattern that depends on the block, so a copy of one block's
 * trailing bytes into another is not mistaken for an intact guard. */
static uint8_t fz_pat(uint32_t serial, uint32_t i) {
    return (uint8_t)(0xA5u ^ (serial * 31u + i * 17u));
}

static void fz_paint(fz_blk *b) {
    uint8_t *p = (uint8_t *)(b + 1);
    for (uint32_t i = 0; i < FZ_RED; i++) {
        p[i] = fz_pat(b->serial, i);
        p[FZ_RED + b->size + i] = fz_pat(b->serial, i + FZ_RED);
    }
}

static int fz_guards_ok(fz_blk *b, const char **where) {
    uint8_t *p = (uint8_t *)(b + 1);
    for (uint32_t i = 0; i < FZ_RED; i++) {
        if (p[i] != fz_pat(b->serial, i))                  { *where = "under"; return 0; }
        if (p[FZ_RED + b->size + i] != fz_pat(b->serial, i + FZ_RED)) { *where = "over"; return 0; }
    }
    return 1;
}

void *kmalloc(uint64_t size) {
    fz_alloc_calls++;
    if (fz_oom_in == 0) { fz_oom_hits++; return NULL; }
    if (fz_oom_in > 0)  fz_oom_in--;
    if (!size) size = 1;                       /* kheap.c hands back a block too */
    if (size > (1u << 28) || fz_bytes_live + size > fz_bytes_cap) return NULL;

    fz_blk *b = (fz_blk *)malloc(sizeof(fz_blk) + size + 2 * FZ_RED);
    if (!b) return NULL;
    b->size   = size;
    b->serial = ++fz_serial;
    b->state  = FZ_LIVE;
    b->prev   = NULL;
    b->next   = fz_live_list;
    if (fz_live_list) fz_live_list->prev = b;
    fz_live_list = b;
    fz_bytes_live += size;
    fz_paint(b);
    /* Not zeroed: kmalloc does not zero either, and code that assumes it does
     * should fail here rather than on the machine. 0x5A is a pointer that
     * faults and a length that is absurd. */
    if (fz_poison) memset((uint8_t *)(b + 1) + FZ_RED, 0x5A, (size_t)size);
    return (uint8_t *)(b + 1) + FZ_RED;
}

static void fz_release(fz_blk *b) {
    uint8_t *p = (uint8_t *)(b + 1) + FZ_RED;
    for (uint64_t i = 0; i < b->size; i++)
        if (p[i] != 0xDF) {
            fz_fail("write to freed block #%u at +%llu (use after free)",
                    b->serial, (unsigned long long)i);
            break;
        }
    free(b);
}

void kfree(void *ptr) {
    if (!ptr) return;
    fz_blk *b = (fz_blk *)((uint8_t *)ptr - FZ_RED) - 1;
    if (b->state == FZ_DEAD) { fz_fail("double free of block #%u", b->serial); return; }
    if (b->state != FZ_LIVE) { fz_fail("kfree(%p) of memory the heap never handed out", ptr); return; }

    const char *where;
    if (!fz_guards_ok(b, &where))
        fz_fail("heap %sflow: block #%u of %llu bytes was written outside it",
                where, b->serial, (unsigned long long)b->size);

    if (b->prev) b->prev->next = b->next; else fz_live_list = b->next;
    if (b->next) b->next->prev = b->prev;
    fz_bytes_live -= b->size;
    b->state = FZ_DEAD;
    memset(ptr, 0xDF, (size_t)b->size);

    fz_blk *old = fz_quar[fz_quar_head];
    fz_quar[fz_quar_head] = b;
    fz_quar_head = (fz_quar_head + 1) % FZ_QUAR;
    if (old) fz_release(old);
}

/* Walk every live block's guards. Called after each iteration: an overflow
 * into a block that is never freed would otherwise go unseen. */
static void fz_heap_audit(void) {
    const char *where;
    for (fz_blk *b = fz_live_list; b; b = b->next)
        if (!fz_guards_ok(b, &where)) {
            fz_fail("heap %sflow: block #%u of %llu bytes was written outside it",
                    where, b->serial, (unsigned long long)b->size);
            return;
        }
}

static uint32_t fz_leak_count(void) {
    uint32_t n = 0;
    for (fz_blk *b = fz_live_list; b; b = b->next) n++;
    return n;
}

/* Between iterations. Leaks are reported by the caller, not here, because
 * whether a target is expected to free everything is the target's business. */
static void fz_heap_reset(void) {
    while (fz_live_list) {
        fz_blk *b = fz_live_list;
        fz_live_list = b->next;
        free(b);
    }
    for (int i = 0; i < FZ_QUAR; i++) { if (fz_quar[i]) free(fz_quar[i]); fz_quar[i] = NULL; }
    fz_quar_head = 0;
    fz_bytes_live = 0;
    fz_oom_in = -1;
}

/* The rest of kheap.h, for the sources that reach for it. */
void kheap_init(void) { }
void kheap_usage(uint64_t *used, uint64_t *total) {
    if (used) *used = fz_bytes_live;
    if (total) *total = fz_bytes_cap;
}
int kheap_check(void) { return 0; }

/* ----------------------------------------------------------------- mutator --
 * Corpus-driven. Purely random bytes get rejected by the first length check in
 * any of these parsers, so almost every input starts as something that parsed
 * once and is then damaged in one of the ways that historically break parsers:
 * a length field that no longer matches, a structure that ends early, a
 * nesting depth nobody tested, and a byte that is 0 or 0xFF or 0x7F. */
#define FZ_MAXLEN (1u << 18)

typedef struct { const uint8_t *d; uint32_t n; } fz_seed;

static const uint8_t fz_interesting8[] = {
    0x00, 0x01, 0x7F, 0x80, 0xFF, 0x20, 0x3C, 0x3E, 0x2F, 0x22, 0x27, 0x5C,
    0x0A, 0x0D, 0x09, 0x3B, 0x7B, 0x7D, 0x28, 0x29, 0x25, 0x26, 0x23, 0x2E,
};
static const uint16_t fz_interesting16[] = { 0, 1, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x3FFF };

/* One input, built from the corpus and damaged. Returns its length. */
static uint32_t fz_mutate(uint8_t *out, uint32_t cap,
                          const fz_seed *corpus, int ncorpus) {
    const fz_seed *s = &corpus[fz_below((uint32_t)ncorpus)];
    uint32_t n = s->n;
    if (n > cap) n = cap;
    memcpy(out, s->d, n);

    /* Once in a while hand the seed over untouched: the corpus is also the
     * regression suite, and an entry that stopped parsing would otherwise only
     * be noticed by whichever mutation happened to leave it alone. */
    if (fz_below(64) == 0) return n;

    uint32_t rounds = 1 + fz_below(8);
    for (uint32_t r = 0; r < rounds && n > 0; r++) {
        switch (fz_below(10)) {
        case 0:                                     /* flip one bit */
            out[fz_below(n)] ^= (uint8_t)(1u << fz_below(8));
            break;
        case 1:                                     /* an interesting byte */
            out[fz_below(n)] = fz_interesting8[fz_below(sizeof fz_interesting8)];
            break;
        case 2: {                                   /* an interesting 16-bit, both orders */
            if (n < 2) break;
            uint32_t at = fz_below(n - 1);
            uint16_t v = fz_interesting16[fz_below(sizeof fz_interesting16 / 2)];
            if (fz_rand() & 1) { out[at] = (uint8_t)(v >> 8); out[at + 1] = (uint8_t)v; }
            else               { out[at] = (uint8_t)v; out[at + 1] = (uint8_t)(v >> 8); }
            break;
        }
        case 3: {                                   /* delete a run */
            uint32_t at = fz_below(n), len = 1 + fz_below(n - at);
            memmove(out + at, out + at + len, n - at - len);
            n -= len;
            break;
        }
        case 4: {                                   /* insert repeated bytes */
            uint32_t at = fz_below(n), len = 1 + fz_below(64);
            if (n + len > cap) break;
            memmove(out + at + len, out + at, n - at);
            memset(out + at, out[at], len);
            n += len;
            break;
        }
        case 5: {                                   /* repeat a chunk: depth, and quadratic blowups */
            uint32_t at = fz_below(n), len = 1 + fz_below(n - at);
            uint32_t times = 1 + fz_below(8);
            for (uint32_t k = 0; k < times && n + len <= cap; k++) {
                memmove(out + at + len, out + at, n - at);
                memcpy(out + at, out + at + len, len);
                n += len;
            }
            break;
        }
        case 6: {                                   /* splice another corpus entry in */
            const fz_seed *o = &corpus[fz_below((uint32_t)ncorpus)];
            if (!o->n) break;
            uint32_t at = fz_below(n), len = 1 + fz_below(o->n);
            uint32_t from = fz_below(o->n - len + 1);
            if (n + len > cap) break;
            memmove(out + at + len, out + at, n - at);
            memcpy(out + at, o->d + from, len);
            n += len;
            break;
        }
        case 7:                                     /* truncate */
            n = 1 + fz_below(n);
            break;
        case 8: {                                   /* overwrite from another entry */
            const fz_seed *o = &corpus[fz_below((uint32_t)ncorpus)];
            if (!o->n) break;
            uint32_t len = 1 + fz_below(o->n < n ? o->n : n);
            uint32_t at = fz_below(n - len + 1);
            memcpy(out + at, o->d + fz_below(o->n - len + 1), len);
            break;
        }
        default: {                                  /* swap two runs */
            if (n < 4) break;
            uint32_t half = n / 2, len = 1 + fz_below(half);
            uint32_t a = fz_below(half), b = half + fz_below(half - len + 1);
            for (uint32_t k = 0; k < len; k++) {
                uint8_t t = out[a + k]; out[a + k] = out[b + k]; out[b + k] = t;
            }
            break;
        }
        }
    }
    return n;
}

/* ------------------------------------------------------------------ runner --*/

typedef void (*fz_target_fn)(const uint8_t *data, uint32_t len);

/* Each harness defines this. A fuzzer whose plumbing is broken reports a clean
 * run at enormous speed, which looks exactly like a fuzzer that found nothing
 * -- net/dns.c returns non-zero for success and the stub here returned zero,
 * so every HTTP target answered "DNS failed" in a microsecond and passed. So
 * before any fuzzing, each harness proves its targets actually reach the code
 * they claim to: a known input in, a known answer out. Returns 0 if so. */
int fz_selftest(void);

typedef struct {
    const char    *name;
    fz_target_fn   fn;
    const fz_seed *corpus;
    int            ncorpus;
    int            check_leaks;   /* 1 = the target must free everything it took */
} fz_target;

static uint8_t       fz_input[FZ_MAXLEN];
static uint32_t      fz_input_len;
static const char   *fz_cur_name = "?";
static uint64_t      fz_cur_seed, fz_cur_iter;
static volatile int  fz_in_target;

/* Where it faulted, not just that it did.
 *
 * A crash arrives here as SIGSEGV or SIGILL and a signal handler cannot ask
 * where: the address is in the CONTEXT record, which the CRT has already
 * thrown away by the time it raises the signal. A vectored exception handler
 * runs first and has it. It records the address and declines to handle
 * anything, so the CRT still turns the fault into the signal we catch, and the
 * report gets a line addr2line can resolve to a function and a line number.
 *
 * The arithmetic is the awkward part: the process is loaded at whatever base
 * ASLR picked, and addr2line wants the address the linker used, which is in
 * the PE header at the actual base. */
#ifdef _WIN32
/* Declared by hand rather than by including <windows.h>: that header defines
 * S_TOP and friends out from under include/layout.h, and three structure
 * fields are cheaper than the collision. EXCEPTION_RECORD and
 * EXCEPTION_POINTERS have had this layout since NT 3.1. */
typedef struct fz_exc_rec {
    unsigned long code, flags;
    struct fz_exc_rec *inner;
    void *address;
} fz_exc_rec;
typedef struct { fz_exc_rec *rec; void *ctx; } fz_exc_ptrs;
__declspec(dllimport) void *__stdcall GetModuleHandleA(const char *);
__declspec(dllimport) void *__stdcall AddVectoredExceptionHandler(
    unsigned long first, long (__stdcall *handler)(fz_exc_ptrs *));

static uintptr_t fz_fault_rip, fz_image_delta;
static void fz_save_case(const char *why);       /* defined below */

static long __stdcall fz_vectored(fz_exc_ptrs *ep) {
    unsigned long c = ep->rec->code;
    if (c == 0xC0000005u || c == 0xC000001Du || c == 0xC0000094u ||
        c == 0xC0000095u || c == 0xC00000FDu || c == 0xC000008Cu)
        fz_fault_rip = (uintptr_t)ep->rec->address;   /* AV, illegal insn, #DE, stack, bounds */

    /* An access violation and an illegal instruction both reach the signal
     * handlers below, because the CRT turns them into SIGSEGV and SIGILL. A
     * divide error does not: the process dies with no report and no saved
     * input. `INT64_MIN / -1` in a page's script is exactly that, so it is
     * reported from here, where the exception still exists. */
    if ((c == 0xC0000094u || c == 0xC0000095u) && fz_in_target) {
        fz_save_case("a divide error -- a division with no representable answer");
        _Exit(1);
    }
    return 0;                                     /* EXCEPTION_CONTINUE_SEARCH */
}

static void fz_where_init(const char *exe) {
    AddVectoredExceptionHandler(1, fz_vectored);

    /* The link-time base has to come off the disk. The copy in the mapped
     * header is no use: the loader rewrites ImageBase to wherever it actually
     * put us, so reading it in memory gives the running base back and a delta
     * of zero -- which is exactly the bug that made the first addresses
     * printed here resolve to "??". The offsets are the ones kernel/pe.c
     * already knows: e_lfanew at 0x3C, and a PE32+ optional header's ImageBase
     * 0x18 into it, so 0x30 past the signature. */
    const unsigned char *base = (const unsigned char *)GetModuleHandleA(NULL);
    FILE *f = exe ? fopen(exe, "rb") : NULL;
    if (!f || !base) { if (f) fclose(f); return; }
    int32_t lfanew = 0; uint64_t linked = 0;
    if (fseek(f, 0x3C, SEEK_SET) == 0 && fread(&lfanew, sizeof lfanew, 1, f) == 1 &&
        lfanew > 0 && fseek(f, lfanew + 0x30, SEEK_SET) == 0 &&
        fread(&linked, sizeof linked, 1, f) == 1)
        fz_image_delta = (uintptr_t)linked - (uintptr_t)base;
    fclose(f);
}

static void fz_where_print(const char *exe) {
    if (!fz_fault_rip) return;
    unsigned long long a = (unsigned long long)(fz_fault_rip + fz_image_delta);
    fprintf(stderr, "  faulted at 0x%llx -- addr2line -e %s -f -C -i 0x%llx\n", a, exe, a);
}
#else
static void fz_where_init(const char *exe) { (void)exe; }
static void fz_where_print(const char *exe) { (void)exe; }
#endif
static const char *fz_exe = "the harness";

static void fz_hexdump(const uint8_t *d, uint32_t n) {
    if (n > 512) n = 512;
    for (uint32_t i = 0; i < n; i += 16) {
        fprintf(stderr, "  %04x  ", i);
        for (uint32_t j = 0; j < 16; j++)
            if (i + j < n) fprintf(stderr, "%02x ", d[i + j]); else fprintf(stderr, "   ");
        fprintf(stderr, " |");
        for (uint32_t j = 0; j < 16 && i + j < n; j++) {
            uint8_t c = d[i + j];
            fputc(c >= 32 && c < 127 ? c : '.', stderr);
        }
        fprintf(stderr, "|\n");
    }
}

static void fz_save_case(const char *why) {
    char path[256];
    snprintf(path, sizeof path, "test/out/crash-%s-%llu-%llu.bin",
             fz_cur_name, (unsigned long long)fz_cur_seed,
             (unsigned long long)fz_cur_iter);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(fz_input, 1, fz_input_len, f); fclose(f); }
    fprintf(stderr, "\nFAIL %s: %s\n", fz_cur_name, why);
    fprintf(stderr, "  seed %llu iteration %llu, %u bytes", (unsigned long long)fz_cur_seed,
            (unsigned long long)fz_cur_iter, fz_input_len);
    if (fz_oom_chosen >= 0)
        fprintf(stderr, ", heap refusing from allocation %lld on", (long long)fz_oom_chosen);
    fputc('\n', stderr);
    if (!f) fprintf(stderr, "  could not write %s\n", path);
    else if (fz_oom_chosen >= 0)
        fprintf(stderr, "  replay with --target %s --case %s --oom %lld\n",
                fz_cur_name, path, (long long)fz_oom_chosen);
    else
        fprintf(stderr, "  replay with --target %s --case %s\n", fz_cur_name, path);
    fz_where_print(fz_exe);
    fz_hexdump(fz_input, fz_input_len);
    fflush(stderr);
}

static void fz_signal(int sig) {
    const char *what = sig == SIGSEGV ? "SIGSEGV -- a bad pointer"
                     : sig == SIGILL  ? "SIGILL -- an undefined-behaviour trap (see the ubsan flags)"
                     : sig == SIGFPE  ? "SIGFPE -- a divide by zero or an overflowing division"
                     : sig == SIGABRT ? "SIGABRT"
                     : "a fatal signal";
    if (!fz_in_target) { fprintf(stderr, "\n%s outside the target\n", what); _Exit(2); }
    fz_save_case(what);
    _Exit(1);
}

static void fz_install_handlers(void) {
    signal(SIGSEGV, fz_signal);
    signal(SIGILL,  fz_signal);
    signal(SIGFPE,  fz_signal);
    signal(SIGABRT, fz_signal);
}

/* One input through one target, with the heap checked either side. Returns 0
 * if everything held. */
static void fz_keep_pending(const fz_target *t, const uint8_t *d, uint32_t n) {
    char path[256];
    snprintf(path, sizeof path, "test/out/pending-%s.bin", t->name);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(d, 1, n, f); fclose(f); }
}

static int fz_once(const fz_target *t, const uint8_t *d, uint32_t n) {
    if (fz_keep) fz_keep_pending(t, d, n);
    fz_fault = NULL;
    fz_heap_reset();

    /* Every eighth input also runs with the heap failing partway through. The
     * countdown is drawn from the input so it is part of what a seed
     * reproduces. */
    if (fz_oom_force != -2)                 fz_oom_in = fz_oom_force;
    else if (!fz_no_oom && (fz_rand() & 7) == 0) fz_oom_in = (int64_t)fz_below(1u << fz_below(13));
    fz_oom_chosen = fz_oom_in;

    clock_t t0 = clock();
    fz_in_target = 1;
    t->fn(d, n);
    fz_in_target = 0;
    clock_t dt = clock() - t0;

    fz_heap_audit();
    if (!fz_fault && t->check_leaks && fz_oom_in < 0) {
        uint32_t leaked = fz_leak_count();
        if (leaked) fz_fail("%u allocation%s not freed", leaked, leaked == 1 ? "" : "s");
    }
    if (!fz_fault && dt > 10 * CLOCKS_PER_SEC)
        fz_fail("took %.1fs for %u bytes -- a parser this slow is a denial of service",
                (double)dt / CLOCKS_PER_SEC, n);
    fz_heap_reset();
    return fz_fault ? -1 : 0;
}

/* Used by fz_selftest() in the harnesses. */
static int fz_check_failures;
#define FZ_CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "  selftest: %s\n", #cond); fz_check_failures++; } } while (0)

static int fz_run_case(const fz_target *t, const char *path) {
    /* A replay reproduces the recorded condition and nothing else. Left to the
     * usual rule, one input in eight would be run with the heap refusing
     * partway through -- so a saved regression case could pass because the
     * allocator gave up before the bug's code was reached. --oom still says
     * so explicitly, and several of the saved cases need it. */
    if (fz_oom_force == -2) fz_oom_force = -1;

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fz_input_len = (uint32_t)fread(fz_input, 1, sizeof fz_input, f);
    fclose(f);
    fz_cur_name = t->name; fz_cur_iter = 0;
    if (fz_once(t, fz_input, fz_input_len) < 0) { fz_save_case(fz_fault); return 1; }
    printf("ok   %-10s %s (%u bytes)\n", t->name, path, fz_input_len);
    return 0;
}

/* The corpus first, unmutated -- this is the regression half, and it is what
 * makes a fuzzer worth keeping in CI rather than running once. Then the
 * mutations. */
static int fz_run_target(const fz_target *t, uint64_t seed, uint64_t iters, int quiet) {
    fz_cur_name = t->name;
    fz_cur_seed = seed;
    fz_srand(seed ^ ((uint64_t)t->name[0] << 32));

    for (int i = 0; i < t->ncorpus; i++) {
        fz_cur_iter = (uint64_t)i;
        if (fz_trace) fprintf(stderr, "[%s corpus %d]\n", t->name, i);
        fz_input_len = t->corpus[i].n > FZ_MAXLEN ? FZ_MAXLEN : t->corpus[i].n;
        memcpy(fz_input, t->corpus[i].d, fz_input_len);
        if (fz_once(t, fz_input, fz_input_len) < 0) {
            fprintf(stderr, "(corpus entry %d, before any mutation)\n", i);
            fz_save_case(fz_fault);
            return 1;
        }
    }

    uint64_t t0 = (uint64_t)clock();
    for (uint64_t i = 0; i < iters; i++) {
        fz_cur_iter = i;
        if (fz_trace) fprintf(stderr, "[%s %llu]\n", t->name, (unsigned long long)i);
        fz_input_len = fz_mutate(fz_input, FZ_MAXLEN, t->corpus, t->ncorpus);
        if (fz_once(t, fz_input, fz_input_len) < 0) { fz_save_case(fz_fault); return 1; }
    }
    double secs = (double)((uint64_t)clock() - t0) / CLOCKS_PER_SEC;
    if (!quiet)
        printf("ok   %-10s %llu inputs, %d corpus, %llu allocations, %llu refused, %.1fs\n",
               t->name, (unsigned long long)iters, t->ncorpus,
               (unsigned long long)fz_alloc_calls, (unsigned long long)fz_oom_hits, secs);
    return 0;
}

static int fz_main(int argc, char **argv, const fz_target *targets, int ntargets) {
    uint64_t seed = 1, iters = 2000;
    const char *only = NULL, *casefile = NULL;
    int quiet = 0, selftest_only = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--seed")   && i + 1 < argc) seed  = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--iters")  && i + 1 < argc) iters = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--target") && i + 1 < argc) only  = argv[++i];
        else if (!strcmp(argv[i], "--case")   && i + 1 < argc) casefile = argv[++i];
        else if (!strcmp(argv[i], "--quiet"))     quiet = 1;
        else if (!strcmp(argv[i], "--no-oom"))    fz_no_oom = 1;
        else if (!strcmp(argv[i], "--no-poison")) fz_poison = 0;
        else if (!strcmp(argv[i], "--trace"))     fz_trace = 1;
        else if (!strcmp(argv[i], "--keep"))      fz_keep = 1;
        else if (!strcmp(argv[i], "--selftest"))  selftest_only = 1;
        else if (!strcmp(argv[i], "--oom") && i + 1 < argc)
            fz_oom_force = strtoll(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--list")) {
            for (int t = 0; t < ntargets; t++) printf("%s\n", targets[t].name);
            return 0;
        } else {
            fprintf(stderr, "usage: %s [--target NAME] [--seed N] [--iters N] "
                            "[--case FILE] [--list] [--quiet] [--selftest]\n       [--no-oom] [--no-poison] [--trace] [--keep] [--oom N]\n", argv[0]);
            return 2;
        }
    }

    fz_exe = argv[0];
    fz_where_init(fz_exe);
    fz_install_handlers();
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Always, not only under --selftest: a run that fuzzes nothing should fail
     * rather than pass quickly. */
    fz_heap_reset();
    if (fz_selftest() != 0) { fprintf(stderr, "FAIL selftest: a check that has to hold before fuzzing does not\n"); return 3; }
    fz_heap_reset();
    if (selftest_only) { printf("ok   selftest\n"); return 0; }

    int rc = 0;
    for (int t = 0; t < ntargets; t++) {
        if (only && strcmp(only, targets[t].name)) continue;
        rc = casefile ? fz_run_case(&targets[t], casefile)
                      : fz_run_target(&targets[t], seed, iters, quiet);
        if (rc) return rc;
    }
    return rc;
}

#endif /* BOLTOS_FUZZ_H */
