#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS virtual-memory layout (after higher-half relink)
 *
 *   0x0000000000000000 .. user space (per-process, ring 3)
 *   0xFFFF800000000000 .. PHYS_BASE : direct map of phys 0..4 GiB (supervisor)
 *   0xFFFF808000000000 .. MMIO_BASE : device BARs above the direct map
 *   0xFFFFFFFF80000000 .. KERNEL_VBASE : kernel image, phys 0 + (v - VBASE)
 *
 *  The low 0..4 GiB identity map from the bootloader is also still present in
 *  the kernel address space, so boot-time physical accesses keep working; new
 *  code should prefer P2V()/the direct map so it is valid under any CR3.
 *
 *  MMIO_BASE exists because the direct map stops at 4 GiB and firmware does
 *  not. UEFI in particular likes to put a 64-bit BAR far above it -- OVMF uses
 *  a window at 768 GiB, which is past even the 512 GiB a single PML4 slot can
 *  reach -- and a controller whose registers cannot be mapped is a controller
 *  that does not exist. For an xHCI that means a machine with no USB at all:
 *  no disk, and no keyboard. kernel/pci.c fills this window from the bottom,
 *  uncached, one BAR at a time.
 * ===========================================================================*/
#define KERNEL_VBASE 0xFFFFFFFF80000000ull
#define PHYS_BASE    0xFFFF800000000000ull
#define MMIO_BASE    0xFFFF808000000000ull   /* PML4 slot 257 */
#define PAGE_SIZE    0x1000ull

/* phys <-> direct-map virtual */
static inline void    *P2V(uint64_t p) { return (void *)(p + PHYS_BASE); }
static inline uint64_t V2P_DIRECT(void *v) { return (uint64_t)v - PHYS_BASE; }

/* kernel-image virtual (a linked symbol) -> physical */
static inline uint64_t KV2P(void *v) { return (uint64_t)v - KERNEL_VBASE; }
