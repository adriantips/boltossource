#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS virtual-memory layout (after higher-half relink)
 *
 *   0x0000000000000000 .. user space (per-process, ring 3)
 *   0xFFFF800000000000 .. PHYS_BASE : direct map of phys 0..4 GiB (supervisor)
 *   0xFFFFFFFF80000000 .. KERNEL_VBASE : kernel image, phys 0 + (v - VBASE)
 *
 *  The low 0..4 GiB identity map from the bootloader is also still present in
 *  the kernel address space, so boot-time physical accesses keep working; new
 *  code should prefer P2V()/the direct map so it is valid under any CR3.
 * ===========================================================================*/
#define KERNEL_VBASE 0xFFFFFFFF80000000ull
#define PHYS_BASE    0xFFFF800000000000ull
#define PAGE_SIZE    0x1000ull

/* phys <-> direct-map virtual */
static inline void    *P2V(uint64_t p) { return (void *)(p + PHYS_BASE); }
static inline uint64_t V2P_DIRECT(void *v) { return (uint64_t)v - PHYS_BASE; }

/* kernel-image virtual (a linked symbol) -> physical */
static inline uint64_t KV2P(void *v) { return (uint64_t)v - KERNEL_VBASE; }
