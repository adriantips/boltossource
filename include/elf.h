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

/* RELA, not REL: the addend lives in the relocation rather than in the field
 * it patches, which is what x86-64 uses and what saves the linker from having
 * to read back a half-written displacement to find out what it meant. */
typedef struct {
    uint64_t r_offset;
    uint64_t r_info;        /* symbol index << 32 | type */
    int64_t  r_addend;
} __attribute__((packed)) Elf64_Rela;

#define SHT_PROGBITS 1
#define SHT_SYMTAB  2
#define SHT_STRTAB  3
#define SHT_RELA    4
#define SHT_NOBITS  8

#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4

#define SHN_UNDEF   0
#define SHN_ABS     0xfff1

#define STB_LOCAL   0
#define STB_GLOBAL  1
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define ELF_ST_TYPE(i)  ((i) & 0xF)
#define ELF_ST_BIND(i)  ((i) >> 4)
#define ELF_ST_INFO(b, t) (uint8_t)(((b) << 4) | ((t) & 0xF))

#define ELF64_R_SYM(i)   ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i)  ((uint32_t)((i) & 0xffffffffu))
#define ELF64_R_INFO(s, t) (((uint64_t)(s) << 32) | (uint32_t)(t))

/* The five relocation types this toolchain produces and consumes. PLT32 is
 * here because a call to a global is spelled that way by every assembler that
 * has ever met a shared library; with nothing dynamic on this machine it is
 * resolved exactly like PC32, which is what a static link does with it too. */
#define R_X86_64_NONE   0
#define R_X86_64_64     1
#define R_X86_64_PC32   2
#define R_X86_64_PLT32  4
#define R_X86_64_32     10
#define R_X86_64_32S    11

#define PT_LOAD     1
#define PF_X        1
#define PF_W        2
#define PF_R        4
#define ET_REL      1
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
