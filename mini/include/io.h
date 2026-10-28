#pragma once
#include <stdint.h>

/* BoltOS Mini's io.h. The shared code includes "io.h" for the x86 port-I/O
 * header; Mini's include path finds this one first. AArch64 has no port I/O,
 * so what is left is the interrupt mask, in the shapes the shared code uses. */
static inline void cli(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("wfi" ::: "memory"); }
/* On Mini only the heap (mm/kheap.c) uses these, and with a second core
 * decoding video the heap is shared: so they also take a spinlock. */
extern volatile uint32_t mini_heap_lock[16];
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #2" : "=r"(f) :: "memory");
    /* Wait with plain loads until it looks free, then try: an exclusive store
     * attempted in a tight loop against the other core can keep failing. */
    for (;;) {
        while (__atomic_load_n(&mini_heap_lock[0], __ATOMIC_RELAXED)) __asm__ volatile("yield");
        if (!__atomic_exchange_n(&mini_heap_lock[0], 1u, __ATOMIC_ACQUIRE)) break;
    }
    return f;
}
static inline void irq_restore(uint64_t f) {
    __atomic_store_n(&mini_heap_lock[0], 0u, __ATOMIC_RELEASE);
    __asm__ volatile("msr daif, %0" :: "r"(f) : "memory");
}
