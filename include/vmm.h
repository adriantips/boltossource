#pragma once
#include <stdint.h>

/* x86-64 page-table entry flags */
#define PTE_PRESENT 0x001ull
#define PTE_WRITE   0x002ull
#define PTE_USER    0x004ull
#define PTE_PWT     0x008ull
#define PTE_PCD     0x010ull
#define PTE_PS      0x080ull           /* large page (2 MiB at PD level)        */
#define PTE_NX      0x8000000000000000ull

void     vmm_init(void);                /* capture the kernel PML4 from CR3      */
uint64_t vmm_kernel_pml4(void);         /* physical address of the kernel PML4   */
uint64_t vmm_current_pml4(void);        /* CR3 & frame mask                      */
void     vmm_switch(uint64_t pml4_phys);/* load CR3                              */

/* 4 KiB mappings. pml4_phys selects the address space; flags are OR'd with
 * PRESENT. Intermediate tables are allocated from the PMM as needed and the
 * path is widened to USER when flags requests it. Returns 0 on success. */
int      vmm_map(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);
int      vmm_unmap(uint64_t pml4_phys, uint64_t virt);
uint64_t vmm_get_phys(uint64_t pml4_phys, uint64_t virt);  /* 0 if not mapped   */

/* Fresh PML4 that shares the kernel higher half (direct map + kernel image) so
 * interrupts/syscalls stay valid after a CR3 switch. Low half is empty and
 * free for user pages. Returns the PML4 physical address, 0 on failure. */
uint64_t vmm_new_address_space(void);

/* Free every frame the low (user) half of an address space owns -- the data
 * pages and the page tables that map them -- and then the PML4 itself. The
 * shared kernel entries (256, 511) are left alone: they belong to the kernel
 * PML4, and freeing them would unmap the kernel from every other process.
 *
 * Must not be called while the address space is loaded in CR3. Process teardown
 * therefore defers it until the scheduler has switched away (see the zombie
 * reaping in kernel/sched.c). */
void     vmm_free_address_space(uint64_t pml4_phys);
