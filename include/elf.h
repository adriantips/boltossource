#pragma once
#include <stdint.h>

/* Minimal ELF64 definitions + loader for static ET_EXEC x86-64 binaries. */

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} __attribute__((packed)) Elf64_Phdr;

typedef struct {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} __attribute__((packed)) Elf64_Shdr;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} __attribute__((packed)) Elf64_Sym;

#define SHT_SYMTAB  2
#define SHT_STRTAB  3
#define STT_FUNC    2
#define ELF_ST_TYPE(i)  ((i) & 0xF)

#define PT_LOAD     1
#define PF_X        1
#define PF_W        2
#define PF_R        4
#define ET_EXEC     2
#define EM_X86_64   62
#define ELFCLASS64  2

/* Map every PT_LOAD segment of `image` (size bytes) into the address space
 * `cr3`. On success returns 0, writes the entry point to *entry and the first
 * page-aligned address past the highest segment (the initial program break) to
 * *brk_end. Returns -1 on a malformed/unsupported image. */
int elf_load(uint64_t cr3, const uint8_t *image, uint64_t size,
             uint64_t *entry, uint64_t *brk_end);

/* Name the function containing `addr`, from the image's .symtab, as
 * "name+0x1f". Returns 1 when a symbol covers the address, 0 otherwise (a
 * stripped binary, or an address outside every function). This is what turns a
 * post-mortem backtrace from a column of hex into something a person can read,
 * so it lives here with the rest of what the kernel knows about ELF. */
int elf_symbolize(const uint8_t *image, uint64_t size, uint64_t addr,
                  char *out, uint32_t cap);
