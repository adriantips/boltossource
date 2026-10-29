/* ===========================================================================
 *  BoltOS Mini  -  mini/gc.c
 *
 *  The heap a program from the Code app runs on (rt.h): a "big bag of
 *  pages". Every 4 KiB page holds objects of one size, 16 bytes to 2 KiB,
 *  and bigger objects get runs of whole pages, so any address inside an
 *  object finds its start. That is what lets the collector be conservative:
 *  it marks from the stack, the registers and a few roots, taking every
 *  word that points into the heap as a pointer, and sweeps what's left
 *  into free lists. Nothing is moved and nothing has to say where its
 *  pointers are; text and other pointer-free objects ("leaves") aren't
 *  looked inside.
 *
 *  It is plain C, so the same file runs a Python program on the Mac for
 *  testing (mini/tools/pyhost.c).
 * ===========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include "rt.h"
#include "string.h"
#include "stdio.h"

static inline uintptr_t cur_sp(void) { uintptr_t s; __asm__ volatile("mov %0, sp" : "=r"(s)); return s; }
static uint8_t *stack_top;

/* ---- the heap ----------------------------------------------------------------------------------- */
#define PAGE      4096u
#define NCLS      14
static const uint16_t cls_size[NCLS] = { 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048 };
enum { PG_FREE = 0, PG_SMALL = 1, PG_LARGE = 2, PG_CONT = 3 };

typedef struct { uint8_t kind, cls, leaf, pad; uint32_t n; } page_t;   /* n: large run's pages, or a cont's start */

static struct {
    uint8_t  *base; uint32_t npages;
    page_t   *pg;
    uint64_t *abits, *mbits;                     /* per page, 256 bits each: allocated, marked */
    void     *freel[NCLS][2];
    uint32_t  page_hint;
    uint64_t  since, threshold, live, used;
    void    **roots[64]; int nroots;
    uintptr_t *mstack; uint32_t mcap, msp; int moverflow;
    int       collecting;
    uint32_t  collections;
} H;

static int size_class(size_t n) {
    for (int i = 0; i < NCLS; i++) if (n <= cls_size[i]) return i;
    return -1;
}

void gc_init(uint8_t *base, uint64_t size, uint8_t *top) {
    stack_top = top;
    memset(&H, 0, sizeof H);
    /* The page table and bitmaps come off the front. */
    uint64_t pages = size / PAGE;
    uint64_t meta = pages * (sizeof(page_t) + 64);
    meta = (meta + PAGE - 1) / PAGE * PAGE;
    uint64_t mst = 256 * 1024 * sizeof(uintptr_t);
    H.pg = (page_t *)base;
    H.abits = (uint64_t *)(base + pages * sizeof(page_t));
    H.mbits = H.abits + pages * 4;
    H.mstack = (uintptr_t *)(base + meta);
    H.mcap = 256 * 1024;
    H.base = base + meta + mst;
    H.npages = (uint32_t)((size - meta - mst) / PAGE);
    memset(base, 0, meta);
    H.threshold = 8u << 20;
}

static uint8_t *page_addr(uint32_t i) { return H.base + (uint64_t)i * PAGE; }

/* n free pages in a row: the first's index, or -1. */
static int64_t take_pages(uint32_t n) {
    for (int pass = 0; pass < 2; pass++) {
        uint32_t from = pass ? 0 : H.page_hint, run = 0;
        for (uint32_t i = from; i < H.npages; i++) {
            if (H.pg[i].kind != PG_FREE) { run = 0; continue; }
            if (++run == n) {
                uint32_t s = i + 1 - n;
                H.page_hint = i + 1;
                return s;
            }
        }
    }
    return -1;
}

static void *alloc_once(size_t n, int leaf) {
    int c = size_class(n);
    if (c >= 0) {
        void **fl = &H.freel[c][leaf];
        if (!*fl) {
            int64_t p = take_pages(1);
            if (p < 0) return 0;
            H.pg[p].kind = PG_SMALL; H.pg[p].cls = (uint8_t)c; H.pg[p].leaf = (uint8_t)leaf;
            uint8_t *a = page_addr((uint32_t)p);
            uint32_t sz = cls_size[c], cnt = PAGE / sz;
            for (int k = (int)cnt - 1; k >= 0; k--) { void **o = (void **)(a + k * sz); *o = *fl; *fl = o; }
        }
        void **o = *fl;
        *fl = *o;
        uint32_t pi = (uint32_t)(((uint8_t *)o - H.base) / PAGE), oi = (uint32_t)(((uint8_t *)o - page_addr(pi)) / cls_size[c]);
        H.abits[pi * 4 + oi / 64] |= 1ull << (oi % 64);
        memset(o, 0, cls_size[c]);
        H.since += cls_size[c]; H.used += cls_size[c];
        return o;
    }
    uint32_t np = (uint32_t)((n + PAGE - 1) / PAGE);
    int64_t p = take_pages(np);
    if (p < 0) return 0;
    H.pg[p].kind = PG_LARGE; H.pg[p].leaf = (uint8_t)leaf; H.pg[p].n = np;
    for (uint32_t k = 1; k < np; k++) { H.pg[p + k].kind = PG_CONT; H.pg[p + k].n = (uint32_t)p; }
    H.abits[p * 4] |= 1;
    uint8_t *a = page_addr((uint32_t)p);
    memset(a, 0, (size_t)np * PAGE);
    H.since += (uint64_t)np * PAGE; H.used += (uint64_t)np * PAGE;
    return a;
}

/* The object an address points into: its start and size, or 0. */
static uintptr_t object_of(uintptr_t w, uint32_t *pi_out, uint32_t *oi_out, uint64_t *size) {
    if (w < (uintptr_t)H.base || w >= (uintptr_t)H.base + (uint64_t)H.npages * PAGE) return 0;
    uint32_t pi = (uint32_t)((w - (uintptr_t)H.base) / PAGE);
    page_t *p = &H.pg[pi];
    if (p->kind == PG_CONT) { pi = p->n; p = &H.pg[pi]; }
    if (p->kind == PG_LARGE) {
        if (!(H.abits[pi * 4] & 1)) return 0;
        *pi_out = pi; *oi_out = 0; *size = (uint64_t)p->n * PAGE;
        return (uintptr_t)page_addr(pi);
    }
    if (p->kind != PG_SMALL) return 0;
    uint32_t sz = cls_size[p->cls], oi = (uint32_t)((w - (uintptr_t)page_addr(pi)) / sz);
    if (oi >= PAGE / sz || !(H.abits[pi * 4 + oi / 64] & (1ull << (oi % 64)))) return 0;
    *pi_out = pi; *oi_out = oi; *size = sz;
    return (uintptr_t)page_addr(pi) + (uintptr_t)oi * sz;
}

static void mark_word(uintptr_t w) {
    uint32_t pi, oi; uint64_t sz;
    uintptr_t o = object_of(w, &pi, &oi, &sz);
    if (!o) return;
    uint64_t bit = 1ull << (oi % 64), *m = &H.mbits[pi * 4 + oi / 64];
    if (*m & bit) return;
    *m |= bit;
    if (H.pg[pi].leaf) return;
    if (H.msp < H.mcap) H.mstack[H.msp++] = o;
    else H.moverflow = 1;
}

static void scan_range(const void *lo, const void *hi) {
    for (const uintptr_t *p = (const uintptr_t *)(((uintptr_t)lo + 7) & ~7ull); (const void *)(p + 1) <= hi; p++) mark_word(*p);
}

static void drain(void) {
    while (H.msp) {
        uintptr_t o = H.mstack[--H.msp];
        uint32_t pi, oi; uint64_t sz;
        object_of(o, &pi, &oi, &sz);
        scan_range((void *)o, (void *)(o + sz));
    }
}

void rt_gc(void) {
    if (H.collecting) return;
    H.collecting = 1;
    rt_jmp_t regs;                               /* the callee-saved registers, onto the stack */
    rt_setjmp(&regs);
    memset(H.mbits, 0, (size_t)H.npages * 32);
    H.msp = 0; H.moverflow = 0;
    for (int i = 0; i < H.nroots; i++) mark_word((uintptr_t)*H.roots[i]);
    scan_range(&regs, &regs + 1);
    scan_range((void *)cur_sp(), stack_top);
    drain();
    while (H.moverflow) {                        /* the mark stack ran out: look again */
        H.moverflow = 0;
        for (uint32_t pi = 0; pi < H.npages; pi++) {
            page_t *p = &H.pg[pi];
            if (p->leaf || (p->kind != PG_SMALL && p->kind != PG_LARGE)) continue;
            uint32_t sz = p->kind == PG_LARGE ? p->n * PAGE : cls_size[p->cls], cnt = p->kind == PG_LARGE ? 1 : PAGE / sz;
            for (uint32_t oi = 0; oi < cnt; oi++)
                if (H.mbits[pi * 4 + oi / 64] & (1ull << (oi % 64))) {
                    uintptr_t o = (uintptr_t)page_addr(pi) + (uintptr_t)oi * sz;
                    scan_range((void *)o, (void *)(o + sz));
                    drain();
                }
        }
    }
    /* Sweep: free lists made again from what's left; empty pages go back. */
    memset(H.freel, 0, sizeof H.freel);
    uint64_t live = 0;
    for (uint32_t pi = 0; pi < H.npages; pi++) {
        page_t *p = &H.pg[pi];
        if (p->kind == PG_LARGE) {
            if (H.mbits[pi * 4] & 1) { live += (uint64_t)p->n * PAGE; continue; }
            uint32_t n = p->n;
            for (uint32_t k = 0; k < n; k++) { H.pg[pi + k].kind = PG_FREE; H.abits[(pi + k) * 4] = 0; }
            continue;
        }
        if (p->kind != PG_SMALL) continue;
        uint64_t *a = &H.abits[pi * 4], *m = &H.mbits[pi * 4];
        int any = 0;
        for (int k = 0; k < 4; k++) { a[k] &= m[k]; any |= a[k] != 0; }
        if (!any) { p->kind = PG_FREE; continue; }
        uint32_t sz = cls_size[p->cls], cnt = PAGE / sz;
        for (int oi = (int)cnt - 1; oi >= 0; oi--) {
            if (a[oi / 64] & (1ull << (oi % 64))) { live += sz; continue; }
            void **o = (void **)(page_addr(pi) + (uint32_t)oi * sz);
            *o = H.freel[p->cls][p->leaf]; H.freel[p->cls][p->leaf] = o;
        }
    }
    H.live = H.used = live;
    H.since = 0;
    H.threshold = live > (8u << 20) ? live : (8u << 20);
    H.page_hint = 0;
    H.collections++;
    H.collecting = 0;
}

static void *alloc(size_t n, int leaf) {
    if (!n) n = 1;
    if (H.since > H.threshold) rt_gc();
    void *p = alloc_once(n, leaf);
    if (!p) { rt_gc(); p = alloc_once(n, leaf); }
    if (!p) rt_fail(0, "MemoryError: out of memory (%lu MiB in use)", (unsigned long)(H.used >> 20));
    return p;
}

void *rt_alloc(size_t n) { return alloc(n, 0); }
void *rt_alloc_leaf(size_t n) { return alloc(n, 1); }
void *rt_realloc(void *p, size_t old, size_t n) {
    uint32_t pi, oi; uint64_t sz;
    int leaf = 0;
    if (p && object_of((uintptr_t)p, &pi, &oi, &sz)) { if (n <= sz) return p; leaf = H.pg[pi].leaf; }
    void *q = alloc(n, leaf);
    if (p) memcpy(q, p, old < n ? old : n);
    return q;
}
void rt_root(void **p) { if (H.nroots < 64) H.roots[H.nroots++] = p; }
uint64_t rt_heap_used(void) { return H.used; }
uint64_t rt_heap_size(void) { return (uint64_t)H.npages * PAGE; }


int gc_debug(char *out, int cap) {
    return snprintf(out, (size_t)cap, "heap %lu KiB used of %lu MiB, %u collections\n", (unsigned long)(H.used >> 10),
                    (unsigned long)(((uint64_t)H.npages * PAGE) >> 20), H.collections);
}
