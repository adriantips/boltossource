#include <stdint.h>
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "mm.h"
#include "kprintf.h"

#define PG_DN(x) ((x) & ~(PAGE_SIZE - 1))
#define PG_UP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

/* Ensure user virtual page `va` is mapped in `cr3` with at least `flags`;
 * allocate+zero a fresh frame if absent (segments can share a boundary page).
 * Returns the frame physical base, or 0 on OOM. */
static uint64_t ensure_page(uint64_t cr3, uint64_t va, uint64_t flags) {
    uint64_t ph = vmm_get_phys(cr3, va);
    if (ph) return PG_DN(ph);
    uint64_t fr = pmm_alloc_frame();
    if (!fr) return 0;
    uint8_t *kv = (uint8_t *)P2V(fr);
    for (int i = 0; i < (int)PAGE_SIZE; i++) kv[i] = 0;
    if (vmm_map(cr3, va, fr, flags) != 0) { pmm_free_frame(fr); return 0; }
    return fr;
}

int elf_load(uint64_t cr3, const uint8_t *image, uint64_t size,
             uint64_t *entry, uint64_t *brk_end) {
    if (size < sizeof(Elf64_Ehdr)) return -1;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;

    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') return -1;
    if (eh->e_ident[4] != ELFCLASS64) return -1;
    if (eh->e_machine != EM_X86_64)   return -1;
    if (eh->e_type != ET_EXEC)        return -1;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) return -1;

    uint64_t hi = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph =
            (const Elf64_Phdr *)(image + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_offset + ph->p_filesz > size) return -1;

        uint64_t flags = PTE_PRESENT | PTE_USER;
        if (ph->p_flags & PF_W) flags |= PTE_WRITE;

        uint64_t va_s = PG_DN(ph->p_vaddr);
        uint64_t va_e = PG_UP(ph->p_vaddr + ph->p_memsz);
        for (uint64_t va = va_s; va < va_e; va += PAGE_SIZE)
            if (!ensure_page(cr3, va, flags)) return -1;

        /* copy file-backed bytes through the per-page translation; .bss tail is
         * already zero from ensure_page(). */
        for (uint64_t j = 0; j < ph->p_filesz; j++) {
            uint64_t uva = ph->p_vaddr + j;
            uint64_t dst = vmm_get_phys(cr3, uva);
            if (!dst) return -1;
            *(uint8_t *)P2V(dst) = image[ph->p_offset + j];
        }
        if (va_e > hi) hi = va_e;
    }
    if (!hi) return -1;             /* no loadable segment */

    *entry   = eh->e_entry;
    *brk_end = hi;
    return 0;
}

/* --------------------------------------------------------- symbolication --- */
/* Find the function containing `addr` in the image's .symtab and format it as
 * "name+0x1f". A post-mortem backtrace of bare addresses is a puzzle; the same
 * backtrace with names is a bug report, and the symbols are already sitting in
 * the file the program was loaded from. */
int elf_symbolize(const uint8_t *image, uint64_t size, uint64_t addr,
                  char *out, uint32_t cap) {
    if (!image || size < sizeof(Elf64_Ehdr) || cap < 8) return 0;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') return 0;
    if (!eh->e_shoff || !eh->e_shnum) return 0;
    if (eh->e_shoff + (uint64_t)eh->e_shnum * eh->e_shentsize > size) return 0;

    const Elf64_Shdr *sh = (const Elf64_Shdr *)(image + eh->e_shoff);
    const Elf64_Sym  *syms = 0;
    const char       *strs = 0;
    uint64_t          nsym = 0, strsz = 0;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB || !sh[i].sh_entsize) continue;
        if (sh[i].sh_offset + sh[i].sh_size > size) return 0;
        uint16_t link = (uint16_t)sh[i].sh_link;
        if (link >= eh->e_shnum) return 0;
        if (sh[link].sh_offset + sh[link].sh_size > size) return 0;
        syms  = (const Elf64_Sym *)(image + sh[i].sh_offset);
        nsym  = sh[i].sh_size / sh[i].sh_entsize;
        strs  = (const char *)(image + sh[link].sh_offset);
        strsz = sh[link].sh_size;
        break;
    }
    if (!syms || !strs) return 0;

    /* The best match is the function with the highest start at or below addr,
     * so a symbol with no size still names the address after it. */
    const Elf64_Sym *best = 0;
    for (uint64_t i = 0; i < nsym; i++) {
        const Elf64_Sym *s = &syms[i];
        if (ELF_ST_TYPE(s->st_info) != STT_FUNC || !s->st_value) continue;
        if (s->st_value > addr) continue;
        if (s->st_size && addr >= s->st_value + s->st_size) continue;
        if (!best || s->st_value > best->st_value) best = s;
    }
    if (!best || best->st_name >= strsz) return 0;

    const char *nm = strs + best->st_name;
    uint32_t o = 0;
    while (nm[o] && o < cap - 8) { out[o] = nm[o]; o++; }
    uint64_t off = addr - best->st_value;
    if (off) {
        out[o++] = '+'; out[o++] = '0'; out[o++] = 'x';
        int shift = 60;
        while (shift > 0 && !((off >> shift) & 0xF)) shift -= 4;
        for (; shift >= 0 && o < cap - 1; shift -= 4)
            out[o++] = "0123456789abcdef"[(off >> shift) & 0xF];
    }
    out[o] = 0;
    return 1;
}
