#pragma once
#include <stdint.h>

/* How much of the heap is in use, and -- just as important -- how much of what
 * is left can actually be handed out in one piece. A heap that is 40% free in
 * a thousand fragments fails allocations that a heap 40% free in one block
 * would satisfy, and `used` alone cannot tell the two apart. */
typedef struct {
    uint64_t used;            /* payload bytes handed out                     */
    uint64_t total;           /* bytes under management, headers included     */
    uint64_t free_bytes;      /* payload bytes not handed out                 */
    uint64_t largest_free;    /* the biggest kmalloc that can still succeed   */
    uint32_t blocks;
    uint32_t free_blocks;
    uint64_t failures;        /* kmalloc calls that returned 0 since boot     */
    uint64_t bad_frees;       /* kfree of a pointer the heap never handed out */
} kheap_stats;

void  kheap_init(void);
void  kheap_window(uint64_t *phys, uint64_t *bytes);  /* where it ended up */
void *kmalloc(uint64_t size);
void  kfree(void *p);
void  kheap_usage(uint64_t *used, uint64_t *total);   /* live payload / managed bytes */
void  kheap_get_stats(kheap_stats *s);
int   kheap_check(void);      /* 0 if the heap's own structure is sound */
