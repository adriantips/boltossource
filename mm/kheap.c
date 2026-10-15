#include <stdint.h>
#include "kheap.h"
#include "pmm.h"
#include "mm.h"
#include "io.h"
#include "kprintf.h"

/* A first-fit heap over one contiguous run of physical memory, taken from the
 * PMM at boot. Sized at 96 MiB so the DOOM port has room for its ~6 MiB zone,
 * the 640x400 framebuffer and WAD lump cache on top of the desktop's
 * compositor buffers.
 *
 * It used to be a fixed window at 16 MiB, which was fine while the kernel was
 * a few megabytes. Then the desktop embedded its fonts, icons and wallpapers,
 * .bss crept up to meet it, and two things went wrong in order. First the PMM
 * bitmap, which sits just past .bss and is 128 KiB on any machine with 4 GiB,
 * began to overlap the heap's first pages -- nothing noticed, because the test
 * VMs have 2 GiB and a 64 KiB bitmap. Then .bss itself crossed 16 MiB, the
 * heap's first header landed on top of live kernel data, and the machine
 * reset itself while seeding /bin with no message at all. vmm_init's page
 * tables were one frame away from the same fate the whole time, since the PMM
 * hands out the lowest free frame and they are taken before this runs.
 *
 * Asking the PMM for the run instead means the heap starts wherever the
 * kernel, the bitmap and those page tables end, and can never overlap any of
 * them however big the image gets.
 *
 * Two lists, not one. Every block, free or in use, is on a doubly-linked list
 * in address order, which is what makes a neighbour cheap to find and merge.
 * Free blocks are additionally on their own list, so an allocation walks free
 * blocks rather than every block in the heap -- with a browser open the
 * difference is thousands of headers per kmalloc.
 *
 * The previous version had one singly-linked list and coalesced forward only:
 * kfree(b) merged b with b->next if that happened to be free. Free two blocks
 * in address order -- which is the ordinary case, since things tend to be
 * released in the order they were taken -- and neither merge fires, because
 * when the first is freed the second is still in use, and when the second is
 * freed it looks past the first. The two stayed adjacent and separate for the
 * rest of the boot. An allocation big enough to need both then failed with the
 * space sitting right there, and the watchdog, which only ever looked at how
 * many bytes were in use, saw a heap that was 40% full and said nothing.
 *
 * Every list walk runs with interrupts off. The timer ISR calls schedule()
 * unconditionally, so a thread halfway through unlinking a block gets
 * preempted by another thread that allocates, and the two interleave inside
 * the same pointers. Only the BSP ever runs kernel threads -- the APs idle in
 * ap_entry64 and allocate nothing -- so switching interrupts off is enough and
 * no spinlock is needed. Giving the APs work is the change that would need
 * one. The sections are short because coalescing keeps the free list short:
 * that is the other reason it matters. */
#define KHEAP_SIZE  0x6000000ull   /* 96 MiB, if there is a run that long  */
#define KHEAP_MIN   0x2000000ull   /* below this, give up and say so       */

/* The smallest payload worth leaving behind when a block is split. It is also
 * the smallest a free block may be, because a free block stores its free-list
 * links inside its own payload. */
#define MIN_PAYLOAD 16u
#define BLK_MAGIC   0x424C4B31u    /* "BLK1" */

typedef struct block {
    uint64_t      size;            /* payload bytes, not counting this header */
    struct block *prev, *next;     /* address order: every block in the heap */
    uint32_t      free;
    uint32_t      magic;           /* a kfree of a stray pointer is caught, not
                                    * silently allowed to corrupt the heap */
} block_t;

/* A free block's payload is dead space, so the free list lives in it. */
typedef struct { struct block *prev, *next; } freelink;

static block_t *head;              /* lowest-addressed block */
static block_t *freehead;
static uint64_t heap_base, heap_size;   /* physical, as the PMM gave it */
static uint8_t *heap_lo;           /* the window itself, so kfree can tell a
                                    * pointer that came from here from one that
                                    * did not without depending on `head` still
                                    * being the lowest block */
static uint64_t g_failures, g_bad_frees;

static freelink *fl(block_t *b)      { return (freelink *)((uint8_t *)b + sizeof(block_t)); }
static void     *payload(block_t *b) { return (uint8_t *)b + sizeof(block_t); }

void kheap_window(uint64_t *phys, uint64_t *bytes) {
    if (phys) *phys = heap_base;
    if (bytes) *bytes = heap_size;
}

static void fl_insert(block_t *b) {
    fl(b)->prev = 0;
    fl(b)->next = freehead;
    if (freehead) fl(freehead)->prev = b;
    freehead = b;
}
static void fl_remove(block_t *b) {
    block_t *p = fl(b)->prev, *n = fl(b)->next;
    if (p) fl(p)->next = n; else freehead = n;
    if (n) fl(n)->prev = p;
}

void kheap_init(void) {
    /* Shrink in 16 MiB steps on a machine whose usable RAM is in pieces. */
    heap_base = 0;
    for (heap_size = KHEAP_SIZE; heap_size >= KHEAP_MIN; heap_size -= 0x1000000ull)
        if ((heap_base = pmm_alloc_contig(heap_size / PAGE_SIZE)) != 0) break;
    /* No 32 MiB run of free RAM anywhere below 4 GiB is not a machine this
     * can run on, and saying so beats the old fixed window, which would
     * now sit on top of the kernel. */
    if (!heap_base) panic("kheap: no contiguous run of 32 MiB of free RAM");
    head = (block_t *)P2V(heap_base);      /* direct map: valid under any CR3 */
    heap_lo = (uint8_t *)head;
    head->size  = heap_size - sizeof(block_t);
    head->prev  = head->next = 0;
    head->free  = 1;
    head->magic = BLK_MAGIC;
    freehead = 0;
    fl_insert(head);
    g_failures = g_bad_frees = 0;
}

static void split(block_t *b, uint64_t size) {
    block_t *nb = (block_t *)((uint8_t *)b + sizeof(block_t) + size);
    nb->size  = b->size - size - sizeof(block_t);
    nb->free  = 1;
    nb->magic = BLK_MAGIC;
    nb->prev  = b;
    nb->next  = b->next;
    if (nb->next) nb->next->prev = nb;
    b->next = nb;
    b->size = size;
    fl_insert(nb);
}

void *kmalloc(uint64_t size) {
    if (size == 0) size = 1;
    uint64_t f = irq_save();
    /* The bound is tested before the round-up, which would otherwise wrap a
     * huge request into a small one and hand back a buffer that is nothing
     * like the size that was asked for. */
    if (size <= heap_size) {
        size = (size + 15) & ~15ull;
        for (block_t *b = freehead; b; b = fl(b)->next) {
            if (b->size < size) continue;
            fl_remove(b);
            if (b->size >= size + sizeof(block_t) + MIN_PAYLOAD) split(b, size);
            b->free = 0;
            irq_restore(f);
            return payload(b);
        }
    }
    g_failures++;
    irq_restore(f);
    return 0;
}

/* Absorb b->next into b. Both must be free. */
static void merge_next(block_t *b) {
    block_t *n = b->next;
    if (!n || !n->free) return;
    fl_remove(n);
    n->magic = 0;
    b->size += sizeof(block_t) + n->size;
    b->next  = n->next;
    if (b->next) b->next->prev = b;
}

void kfree(void *p) {
    if (!p) return;
    uint8_t *q = (uint8_t *)p;
    if (q < heap_lo + sizeof(block_t) || q >= heap_lo + heap_size) {
        g_bad_frees++;
        return;
    }
    block_t *b = (block_t *)(q - sizeof(block_t));
    uint64_t f = irq_save();
    if (b->magic != BLK_MAGIC || b->free) {       /* not ours, or freed twice */
        g_bad_frees++;
        irq_restore(f);
        return;
    }
    b->free = 1;
    fl_insert(b);
    /* Both neighbours, which is enough: no two adjacent free blocks can exist
     * before this call -- every previous free merged its own -- so b's are the
     * only ones there are. */
    merge_next(b);
    if (b->prev && b->prev->free) merge_next(b->prev);
    irq_restore(f);
}

/* Walk the free list to report live allocation. *used = payload bytes handed
 * out, *total = bytes under management. Backs the Task Manager heap readout. */
void kheap_usage(uint64_t *used, uint64_t *total) {
    kheap_stats st;
    kheap_get_stats(&st);
    if (used)  *used  = st.used;
    if (total) *total = st.total;
}

void kheap_get_stats(kheap_stats *s) {
    uint64_t used = 0, total = 0, freeb = 0, largest = 0;
    uint32_t blocks = 0, freeblocks = 0;
    uint64_t f = irq_save();                      /* one consistent snapshot */
    for (block_t *b = head; b; b = b->next) {
        total += b->size + sizeof(block_t);
        blocks++;
        if (b->free) {
            freeblocks++;
            freeb += b->size;
            if (b->size > largest) largest = b->size;
        } else {
            used += b->size;
        }
    }
    s->used = used; s->total = total; s->free_bytes = freeb;
    s->largest_free = largest;
    s->blocks = blocks; s->free_blocks = freeblocks;
    s->failures = g_failures; s->bad_frees = g_bad_frees;
    irq_restore(f);
}

/* Walk both lists and check they agree with each other and with the memory
 * between them. Returns 0 if the heap is sound, or a negative code naming the
 * first thing that was not. Cheap enough to call from a test after every
 * operation; not on any hot path. */
int kheap_check(void) {
    uint64_t f = irq_save();
#define KHEAP_BAD(code) do { irq_restore(f); return (code); } while (0)
    uint64_t span = 0;
    uint32_t frees = 0;
    block_t *prev = 0;
    for (block_t *b = head; b; prev = b, b = b->next) {
        if (b->magic != BLK_MAGIC)            KHEAP_BAD(-1);
        if (b->prev != prev)                  KHEAP_BAD(-2);  /* not doubly linked */
        if (prev && (uint8_t *)b != (uint8_t *)prev + sizeof(block_t) + prev->size)
                                              KHEAP_BAD(-3);  /* hole or overlap  */
        if (prev && prev->free && b->free)    KHEAP_BAD(-4);  /* should have merged */
        span += b->size + sizeof(block_t);
        if (b->free) frees++;
    }
    if (span != heap_size)                    KHEAP_BAD(-5);
    uint32_t seen = 0;
    block_t *fprev = 0;
    for (block_t *b = freehead; b; fprev = b, b = fl(b)->next) {
        if (!b->free || b->magic != BLK_MAGIC) KHEAP_BAD(-6);
        if (fl(b)->prev != fprev)              KHEAP_BAD(-7);
        if (++seen > frees)                    KHEAP_BAD(-8); /* a cycle */
    }
    if (seen != frees)                         KHEAP_BAD(-9); /* a free block the
                                                               * free list lost */
#undef KHEAP_BAD
    irq_restore(f);
    return 0;
}
