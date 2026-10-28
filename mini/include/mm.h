#pragma once
#include <stdint.h>

/* BoltOS Mini's mm.h: the shared heap asks for P2V, and on Mini physical and
 * virtual addresses are the same. */
#define PAGE_SIZE 0x1000ull
static inline void    *P2V(uint64_t p) { return (void *)(uintptr_t)p; }
static inline uint64_t V2P_DIRECT(void *v) { return (uint64_t)(uintptr_t)v; }
