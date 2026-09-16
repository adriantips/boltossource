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

void vmm_init(void) {
    kernel_pml4 = vmm_current_pml4();
    /* Force the tables behind the device-MMIO window into existence now, while
     * there is still only one address space to put them in.
     *
     * vmm_new_address_space() copies PML4 entries, not the trees under them, so
     * an entry that is still empty when a process is created stays empty in
     * that process for ever -- and a BAR mapped afterwards would be missing
     * from exactly the address space a syscall or an interrupt runs in. Mapping
     * and immediately unmapping one page leaves the PDPT, PD and PT allocated
     * and the slot filled, which is all the copy needs to pick up. */
    vmm_map(kernel_pml4, MMIO_BASE, 0, PTE_WRITE);
    vmm_unmap(kernel_pml4, MMIO_BASE);
}

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
    np[256] = kp[256];     /* direct physical map   */
    np[257] = kp[257];     /* device MMIO window    */
    np[511] = kp[511];     /* higher-half kernel    */
    return p;
}

/* Free everything a user page table owns, then the tables themselves.
 *
 * `lvl` is how many further table levels sit below this one:
 *   2 = PDPT (its entries are PDs)
 *   1 = PD   (its entries are PTs)
 *   0 = PT   (its entries are the data pages -- the leaves)
 * so a PDPT is walked with lvl 2. Getting this off by one frees the tables but
 * leaks every page they map, which is most of a process. */
static void free_level(uint64_t table_phys, int lvl) {
    uint64_t *t = table(table_phys);
    for (int i = 0; i < 512; i++) {
        uint64_t e = t[i];
        if (!(e & PTE_PRESENT)) continue;
        if (e & PTE_PS) continue;                  /* large page: not ours */
        uint64_t child = e & ADDR_MASK;
        if (lvl > 0) free_level(child, lvl - 1);   /* the level below, first */
        pmm_free_frame(child);                     /* then this entry itself */
        t[i] = 0;
    }
}

void vmm_free_address_space(uint64_t pml4_phys) {
    if (!pml4_phys || pml4_phys == kernel_pml4) return;
    /* Only the low half is the process's own. Entries 256, 257 and 511 are the
     * shared kernel mappings -- direct map, device MMIO window, kernel image --
     * borrowed from the kernel PML4, and freeing one would unmap the kernel out
     * from under every other address space. */
    uint64_t *t = table(pml4_phys);
    for (int i = 0; i < 256; i++) {
        if (!(t[i] & PTE_PRESENT) || (t[i] & PTE_PS)) { t[i] = 0; continue; }
        uint64_t pdpt = t[i] & ADDR_MASK;
        free_level(pdpt, 2);                       /* PDPT -> PD -> PT -> pages */
        pmm_free_frame(pdpt);
        t[i] = 0;
    }
    pmm_free_frame(pml4_phys);
}
