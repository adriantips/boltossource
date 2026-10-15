/* Drive the real kernel heap. Only the physical memory under it is
 * substituted: mm/kheap.c is the allocator that ships.
 *
 * The property worth testing is not that kmalloc returns memory -- anything
 * does that once. It is that the heap comes back: that a workload which takes
 * and releases memory in the orders real code uses leaves it able to satisfy
 * the same large allocation afterwards that it could before. The allocator
 * this replaced could not, and nothing in the OS would have said so. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

/* kheap.c takes its window from the PMM and converts the physical base of
 * that window to a virtual address. Here the window is a plain host
 * allocation, so P2V is shadowed to point at it -- mm.h is included first so
 * its own definition is in place and complete before being replaced. The PMM
 * hands back 18 MiB, not the 16 the heap used to hard-code, so a heap that
 * still assumed its old address would fall over here rather than at boot. */
#include "mm.h"
#include "pmm.h"
#include "io.h"
#undef P2V
/* kmalloc holds interrupts off while it walks its lists. `cli` is privileged,
 * so in a user-mode process it is a fault rather than a critical section; the
 * heap has no concurrency here to protect it from anyway. */
#define irq_save()     0ull
#define irq_restore(f) ((void)(f))
static uint8_t *arena;
static uint64_t arena_base;
#define HOST_HEAP_BASE 0x1200000ull
void pmm_reserve_range(uint64_t base, uint64_t len) { (void)base; (void)len; }
uint64_t pmm_alloc_contig(uint64_t n) { (void)n; return HOST_HEAP_BASE; }
void panic(const char *msg) { fprintf(stderr, "panic: %s\n", msg); abort(); }
#define P2V(x) ((void *)(arena + ((uint64_t)(x) - arena_base)))

#include "kheap.h"
#include "../../mm/kheap.c"

static void heap_setup(void) {
    if (!arena) {
        arena = malloc(KHEAP_SIZE + 4096);
        assert(arena);
        arena = (uint8_t *)(((uintptr_t)arena + 4095) & ~(uintptr_t)4095);
    }
    arena_base = HOST_HEAP_BASE;
    kheap_init();
    assert(kheap_check() == 0);
}

static uint64_t largest(void) {
    kheap_stats s; kheap_get_stats(&s); return s.largest_free;
}
static uint32_t blocks(void) {
    kheap_stats s; kheap_get_stats(&s); return s.blocks;
}
static uint64_t used(void) {
    kheap_stats s; kheap_get_stats(&s); return s.used;
}

/* ---- everything given back comes back ------------------------------------ */
static void whole_heap_returns(void) {
    heap_setup();
    uint64_t start = largest();

    /* Freed in the order allocated: the case the old allocator could not
     * merge at all, because each block's successor was still in use when it
     * was released and its predecessor was already behind it. */
    void *p[64];
    for (int i = 0; i < 64; i++) { p[i] = kmalloc(4096); assert(p[i]); }
    for (int i = 0; i < 64; i++) { kfree(p[i]); assert(kheap_check() == 0); }
    assert(blocks() == 1 && largest() == start);

    /* Backwards, and from the middle outwards. */
    for (int i = 0; i < 64; i++) p[i] = kmalloc(4096);
    for (int i = 63; i >= 0; i--) kfree(p[i]);
    assert(blocks() == 1 && largest() == start);

    for (int i = 0; i < 64; i++) p[i] = kmalloc(4096);
    for (int i = 32; i < 64; i++) kfree(p[i]);
    for (int i = 31; i >= 0; i--) kfree(p[i]);
    assert(blocks() == 1 && largest() == start);

    /* Mixed sizes, freed in a scattered order. */
    for (int i = 0; i < 64; i++) p[i] = kmalloc((uint64_t)(i * 37 + 1));
    for (int i = 0; i < 64; i += 2) kfree(p[i]);
    for (int i = 1; i < 64; i += 2) kfree(p[i]);
    assert(kheap_check() == 0 && blocks() == 1 && largest() == start);
    puts("  ok: every allocation order leaves the heap one whole block again");
}

/* ---- the allocation that used to start failing --------------------------- */
static void no_slow_death(void) {
    heap_setup();
    uint64_t start = largest();
    void *live[256] = {0};

    /* Churn: a long run of takes and releases of assorted sizes, the shape of
     * a machine that has been on all day. Between rounds the heap must still
     * be able to hand out a big contiguous buffer -- a compositor backbuffer,
     * a decoded image, a downloaded page. */
    uint32_t rng = 12345;
    for (int round = 0; round < 200; round++) {
        for (int i = 0; i < 256; i++) {
            rng = rng * 1103515245u + 12345u;
            int slot = (int)((rng >> 16) & 255);
            if (live[slot]) { kfree(live[slot]); live[slot] = 0; }
            else {
                uint64_t n = 16 + ((rng >> 8) & 0x3FFF);
                live[slot] = kmalloc(n);
                assert(live[slot]);
                memset(live[slot], 0xAB, (size_t)n);   /* and it is really ours */
            }
        }
        void *big = kmalloc(8u << 20);
        assert(big);                       /* 8 MiB, every round, for 200 rounds */
        kfree(big);
    }
    for (int i = 0; i < 256; i++) if (live[i]) kfree(live[i]);
    assert(kheap_check() == 0);
    assert(blocks() == 1 && largest() == start && used() == 0);
    puts("  ok: 51200 allocations of churn leave no fragmentation behind");
}

/* ---- running out is a returned zero, not a crash ------------------------- */
static void exhaustion(void) {
    heap_setup();
    /* Take the heap down to nothing in 1 MiB pieces, then check that the
     * failure is reported and that everything still works afterwards. */
    void **held = malloc(sizeof(void *) * 4096);
    int n = 0;
    void *q;
    while (n < 4096 && (q = kmalloc(1u << 20)) != 0) held[n++] = q;
    assert(n > 64);
    kheap_stats s; kheap_get_stats(&s);
    assert(s.failures >= 1);
    assert(kmalloc(1u << 20) == 0);           /* still says no, still no crash */
    assert(kheap_check() == 0);

    /* Give one back and the same request succeeds again. */
    kfree(held[n / 2]);
    void *again = kmalloc(1u << 20);
    assert(again);
    held[n / 2] = again;
    for (int i = 0; i < n; i++) kfree(held[i]);
    free(held);
    assert(kheap_check() == 0 && blocks() == 1);

    /* An impossible size is refused rather than wrapping the round-up into a
     * small one and handing back a buffer that is not the size asked for. */
    assert(kmalloc(UINT64_MAX) == 0);
    assert(kmalloc(UINT64_MAX - 8) == 0);
    assert(kmalloc(KHEAP_SIZE) == 0);
    assert(kheap_check() == 0);
    puts("  ok: exhaustion and impossible sizes are refused and recovered from");
}

/* ---- a pointer the heap never handed out --------------------------------- */
static void bad_frees(void) {
    heap_setup();
    char stack_object[64];
    kheap_stats before, after;

    kfree(0);                                  /* the documented no-op */
    kheap_get_stats(&before);
    assert(before.bad_frees == 0);

    kfree(stack_object);                       /* not in the heap at all */
    kfree(arena);                              /* inside it, but not a payload */
    void *p = kmalloc(128);
    assert(p);
    kfree(p);
    kfree(p);                                  /* the second time is the bug */
    kheap_get_stats(&after);
    assert(after.bad_frees == 3);
    assert(kheap_check() == 0);

    /* And the heap is still usable, which is the whole point of noticing. */
    void *q = kmalloc(128);
    assert(q);
    kfree(q);
    assert(kheap_check() == 0 && blocks() == 1);
    puts("  ok: a stray, a misaligned and a double free are counted, not honoured");
}

/* ---- alignment and payload separation ------------------------------------ */
static void alignment_and_bounds(void) {
    heap_setup();
    void *p[128];
    for (int i = 0; i < 128; i++) {
        uint64_t n = (uint64_t)(i * 7 + 1);
        p[i] = kmalloc(n);
        assert(p[i]);
        assert(((uintptr_t)p[i] & 15) == 0);   /* 16-byte aligned, every size */
        memset(p[i], i, (size_t)n);            /* fill to the byte asked for */
    }
    for (int i = 0; i < 128; i++) {            /* nothing overlapped anything */
        uint64_t n = (uint64_t)(i * 7 + 1);
        for (uint64_t k = 0; k < n; k++) assert(((uint8_t *)p[i])[k] == (uint8_t)i);
    }
    for (int i = 0; i < 128; i++) kfree(p[i]);
    assert(kheap_check() == 0 && blocks() == 1);
    puts("  ok: every size is 16-byte aligned and owns the bytes it was given");
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    whole_heap_returns();
    no_slow_death();
    exhaustion();
    bad_frees();
    alignment_and_bounds();
    puts("PASS heap-host: coalescing, churn without fragmentation, exhaustion, bad frees");
    return 0;
}
