#include <stdint.h>
#include "vmm.h"
#include "pmm.h"
#include "mm.h"

#define IDX(v, sh)  (((v) >> (sh)) & 0x1FFull)
#define ADDR_MASK   0x000FFFFFFFFFF000ull

static uint64_t kernel_pml4;

static inline void invlpg(uint64_t v) { __asm__ volatile("invlpg (%0)" :: "r"(v) : "memory"); }

/* a page table reached through the direct map, so it is valid under any CR3 */
static inline uint64_t *table(uint64_t entry) { return (uint64_t *)P2V(entry & ADDR_MASK); }

uint64_t vmm_current_pml4(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & ADDR_MASK;
}

void vmm_switch(uint64_t pml4_phys) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(pml4_phys & ADDR_MASK) : "memory");
}

uint64_t vmm_kernel_pml4(void) { return kernel_pml4; }

void vmm_init(void) { kernel_pml4 = vmm_current_pml4(); }

/* Walk PML4 -> PDPT -> PD -> &PTE. When create, missing tables are allocated
 * and zeroed; intermediate entries carry PRESENT|WRITE and gain USER when the
 * requested mapping is USER. Returns 0 on a large page in the path or OOM. */
static uint64_t *walk(uint64_t pml4_phys, uint64_t virt, int create, uint64_t flags) {
    const uint64_t inter = PTE_PRESENT | PTE_WRITE | (flags & PTE_USER);
    const int shifts[3]  = { 39, 30, 21 };
    uint64_t *t = table(pml4_phys);

    for (int lvl = 0; lvl < 3; lvl++) {
        uint64_t i = IDX(virt, shifts[lvl]);
        if (!(t[i] & PTE_PRESENT)) {
            if (!create) return 0;
            uint64_t np = pmm_alloc_frame();
            if (!np) return 0;
            uint64_t *nt = table(np);
            for (int k = 0; k < 512; k++) nt[k] = 0;
            t[i] = np | inter;
        } else {
            if (t[i] & PTE_PS) return 0;
            t[i] |= (flags & PTE_USER);
        }
        t = table(t[i]);
    }
    return &t[IDX(virt, 12)];
}

int vmm_map(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pte = walk(pml4_phys, virt, 1, flags);
    if (!pte) return -1;
    *pte = (phys & ADDR_MASK) | (flags & 0xFFFull) | PTE_PRESENT;
    invlpg(virt);
    return 0;
}

int vmm_unmap(uint64_t pml4_phys, uint64_t virt) {
    uint64_t *pte = walk(pml4_phys, virt, 0, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return -1;
    *pte = 0;
    invlpg(virt);
    return 0;
}

uint64_t vmm_get_phys(uint64_t pml4_phys, uint64_t virt) {
    uint64_t *pte = walk(pml4_phys, virt, 0, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
    return (*pte & ADDR_MASK) | (virt & 0xFFFull);
}

uint64_t vmm_new_address_space(void) {
    uint64_t p = pmm_alloc_frame();
    if (!p) return 0;
    uint64_t *np = table(p);
    uint64_t *kp = table(kernel_pml4);
    for (int i = 0; i < 512; i++) np[i] = 0;
    np[256] = kp[256];     /* direct physical map  */
    np[511] = kp[511];     /* higher-half kernel    */
    return p;
}
