#include <stdint.h>
#include <stddef.h>
#include "link.h"
#include "ar.h"
#include "elf.h"
#include "asm.h"
#include "kheap.h"
#include "string.h"

/* ===========================================================================
 *  BoltLD. See include/link.h for what it is; this is how.
 *
 *  Three walks over the inputs and no more:
 *
 *    1. parse     each object's section table, symbol table and relocations
 *                 are located and bounds-checked, once, up front
 *    2. lay out   every allocatable section is given an offset in the merged
 *                 .text or the merged .data, and then every symbol an address
 *    3. patch     each relocation writes one field
 *
 *  Archives sit in front of that: a member is parsed only to ask whether it
 *  defines something still missing, and only joins the link if it does.
 *
 *  Everything is bounded and nothing recurses -- this runs in the kernel with
 *  a kernel's stack, so a malformed object has to come back as a message
 *  rather than as a fault. Every offset out of a file is checked against that
 *  file's length before it is dereferenced, which is why there are so many
 *  variations on `if (off > len || size > len - off)`; it reads as noise and
 *  it is the only thing standing between a bad .o and a page fault in ring 0.
 * ===========================================================================*/

#define MAX_UNITS   48
#define MAX_SEC     48
#define MAX_GLOBALS 2048
#define PAGE        0x1000
#define TEXT_OFF    0x1000
#define MAX_IMAGE   (16u * 1024 * 1024)

enum { CLS_NONE = -1, CLS_TEXT = 0, CLS_DATA = 1 };

typedef struct {
    char            label[96];      /* "libc.a(printf.o)" -- what errors say */
    const uint8_t  *img;
    uint32_t        len;

    const Elf64_Shdr *sh;
    uint16_t          nsh;

    const Elf64_Sym *syms;
    uint32_t         nsym;
    const char      *strs;
    uint32_t         strsz;

    int8_t    cls[MAX_SEC];
    uint32_t  off[MAX_SEC];         /* where that section landed in the merge */

    uint64_t *symval;               /* resolved absolute address per symbol   */
    uint8_t  *symok;
    int       from_archive;
} Unit;

typedef struct {
    const char *name;
    uint64_t    value;
    int         unit;
} Global;

typedef struct {
    Unit    units[MAX_UNITS];
    int     nunit;

    Global  globals[MAX_GLOBALS];
    int     nglobal;

    char    err[LD_MAX_ERR];
    int     failed;
} LD;

/* A null `l` means the caller is only probing -- `member_wanted` parses a
 * member to see what it defines and must not turn a member it decides against
 * into a link failure. */
static void lfail(LD *l, const char *msg, const char *what) {
    if (!l || l->failed) return;
    l->failed = 1;
    l->err[0] = 0;
    strlcat(l->err, msg, sizeof l->err);
    if (what && what[0]) {
        strlcat(l->err, " '", sizeof l->err);
        strlcat(l->err, what, sizeof l->err);
        strlcat(l->err, "'", sizeof l->err);
    }
}

static uint32_t align_up(uint32_t v, uint32_t a) {
    if (a < 2) return v;
    return (v + a - 1) & ~(a - 1);
}

/* ---- reading one object -------------------------------------------------- */

/* Fill in `u` from a relocatable ELF. Returns 0 on success; anything wrong with
 * the file is reported here rather than surviving into the layout pass. */
static int unit_parse(LD *l, Unit *u, const char *label,
                      const uint8_t *img, uint32_t len) {
    memset(u, 0, sizeof *u);
    strlcpy(u->label, label, sizeof u->label);
    u->img = img;
    u->len = len;

    if (len < sizeof(Elf64_Ehdr)) { lfail(l, "not an object file:", label); return -1; }
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F' ||
        eh->e_ident[4] != ELFCLASS64) {
        lfail(l, "not an ELF64 object:", label);
        return -1;
    }
    if (eh->e_type != ET_REL) {
        /* The mistake this catches is `ld hello hello2.o` -- a program where an
         * object was meant. Saying which it is beats "bad magic". */
        lfail(l, eh->e_type == ET_EXEC
                 ? "already a program, not something to link:"
                 : "not a relocatable object:", label);
        return -1;
    }
    if (eh->e_machine != EM_X86_64) { lfail(l, "not x86-64:", label); return -1; }
    if (eh->e_shentsize != sizeof(Elf64_Shdr) || !eh->e_shnum) {
        lfail(l, "no section table in", label); return -1;
    }
    if (eh->e_shnum > MAX_SEC) { lfail(l, "too many sections in", label); return -1; }
    if (eh->e_shoff > len ||
        (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > len - eh->e_shoff) {
        lfail(l, "section table runs past the end of", label); return -1;
    }

    u->sh  = (const Elf64_Shdr *)(img + eh->e_shoff);
    u->nsh = eh->e_shnum;

    for (uint16_t i = 0; i < u->nsh; i++) {
        const Elf64_Shdr *s = &u->sh[i];
        u->cls[i] = CLS_NONE;

        if (s->sh_type != SHT_NOBITS) {
            if (s->sh_offset > len || s->sh_size > len - s->sh_offset) {
                lfail(l, "a section runs past the end of", label); return -1;
            }
        }
        if (s->sh_flags & SHF_ALLOC)
            u->cls[i] = (s->sh_flags & SHF_EXECINSTR) ? CLS_TEXT : CLS_DATA;

        if (s->sh_type == SHT_SYMTAB) {
            if (u->syms) { lfail(l, "two symbol tables in", label); return -1; }
            if (s->sh_entsize != sizeof(Elf64_Sym) || s->sh_link >= u->nsh) {
                lfail(l, "unreadable symbol table in", label); return -1;
            }
            const Elf64_Shdr *st = &u->sh[s->sh_link];
            if (st->sh_type != SHT_STRTAB || st->sh_offset > len ||
                st->sh_size > len - st->sh_offset || !st->sh_size) {
                lfail(l, "unreadable string table in", label); return -1;
            }
            u->syms  = (const Elf64_Sym *)(img + s->sh_offset);
            u->nsym  = (uint32_t)(s->sh_size / sizeof(Elf64_Sym));
            u->strs  = (const char *)(img + st->sh_offset);
            u->strsz = (uint32_t)st->sh_size;
            /* A string table whose last byte is not a NUL would let a name run
             * off the end of the mapping. Cheaper to refuse than to bound every
             * later read of it. */
            if (u->strs[u->strsz - 1] != 0) {
                lfail(l, "an unterminated string table in", label); return -1;
            }
        }
        if (s->sh_type == SHT_RELA) {
            if (s->sh_entsize != sizeof(Elf64_Rela) || s->sh_info >= u->nsh) {
                lfail(l, "unreadable relocations in", label); return -1;
            }
        }
    }
    if (!u->syms) { lfail(l, "no symbol table in", label); return -1; }
    return 0;
}

static const char *sym_name(const Unit *u, const Elf64_Sym *s) {
    if (s->st_name >= u->strsz) return "";
    return u->strs + s->st_name;
}

/* ---- the global namespace ------------------------------------------------ */

static Global *global_find(LD *l, const char *name) {
    for (int i = 0; i < l->nglobal; i++)
        if (strcmp(l->globals[i].name, name) == 0) return &l->globals[i];
    return 0;
}

/* Publish every global this unit defines. A second definition of the same name
 * is an error rather than a silent winner -- a linker that picks one is a
 * linker you debug by disassembling. */
static void publish(LD *l, int ui) {
    Unit *u = &l->units[ui];
    for (uint32_t i = 0; i < u->nsym; i++) {
        const Elf64_Sym *s = &u->syms[i];
        if (ELF_ST_BIND(s->st_info) != STB_GLOBAL) continue;
        if (s->st_shndx == SHN_UNDEF) continue;
        const char *nm = sym_name(u, s);
        if (!nm[0]) continue;
        Global *g = global_find(l, nm);
        if (g) {
            char both[LD_MAX_ERR];
            both[0] = 0;
            strlcat(both, "duplicate definition of '", sizeof both);
            strlcat(both, nm, sizeof both);
            strlcat(both, "', in both ", sizeof both);
            strlcat(both, l->units[g->unit].label, sizeof both);
            strlcat(both, " and ", sizeof both);
            strlcat(both, u->label, sizeof both);
            lfail(l, both, 0);
            return;
        }
        if (l->nglobal >= MAX_GLOBALS) { lfail(l, "too many global symbols", 0); return; }
        g = &l->globals[l->nglobal++];
        g->name  = nm;
        g->value = 0;
        g->unit  = ui;
    }
}

/* Is there a name nothing has defined yet? Used to decide whether another pass
 * over the archives is worth making. */
static int any_unresolved(LD *l, const char **out) {
    for (int i = 0; i < l->nunit; i++) {
        Unit *u = &l->units[i];
        for (uint32_t k = 0; k < u->nsym; k++) {
            const Elf64_Sym *s = &u->syms[k];
            if (s->st_shndx != SHN_UNDEF) continue;
            if (ELF_ST_BIND(s->st_info) != STB_GLOBAL) continue;
            const char *nm = sym_name(u, s);
            if (!nm[0] || global_find(l, nm)) continue;
            if (out) *out = nm;
            return 1;
        }
    }
    return 0;
}

/* ---- archives ------------------------------------------------------------ */
/* The format itself lives in kernel/ar.c, which also writes it. This half only
 * decides which members are wanted. */

/* Does this member define a name nothing has defined yet? Parsed into a
 * throwaway unit, because answering the question needs the whole symbol table
 * anyway and a second parse later costs nothing worth saving. */
static int member_wanted(LD *l, const ar_member_t *m) {
    Unit tmp;
    if (unit_parse(0, &tmp, m->name, m->data, m->len) != 0) return 0;

    for (uint32_t i = 0; i < tmp.nsym; i++) {
        const Elf64_Sym *s = &tmp.syms[i];
        if (ELF_ST_BIND(s->st_info) != STB_GLOBAL || s->st_shndx == SHN_UNDEF) continue;
        const char *nm = sym_name(&tmp, s);
        if (!nm[0] || global_find(l, nm)) continue;
        /* Defined here, and wanted by something already in the link. */
        for (int ui = 0; ui < l->nunit; ui++) {
            Unit *u = &l->units[ui];
            for (uint32_t k = 0; k < u->nsym; k++) {
                const Elf64_Sym *w = &u->syms[k];
                if (w->st_shndx != SHN_UNDEF) continue;
                if (ELF_ST_BIND(w->st_info) != STB_GLOBAL) continue;
                if (strcmp(sym_name(u, w), nm) == 0) return 1;
            }
        }
    }
    return 0;
}

/* ---- layout and resolution ----------------------------------------------- */

static void ld_free_units(LD *l) {
    for (int i = 0; i < l->nunit; i++) {
        if (l->units[i].symval) kfree(l->units[i].symval);
        if (l->units[i].symok)  kfree(l->units[i].symok);
        l->units[i].symval = 0;
        l->units[i].symok  = 0;
    }
}

uint8_t *ld_link(const ld_input_t *in, int nin, const char *entry,
                 uint32_t *outlen, ld_stats_t *stats, char *err, uint32_t errcap) {
    if (!entry || !entry[0]) entry = "_start";

    LD *l = (LD *)kmalloc(sizeof(LD));
    if (!l) { strlcpy(err, "out of memory", errcap); return 0; }
    memset(l, 0, sizeof *l);

    uint8_t *out = 0;
    uint8_t *text = 0, *data = 0;

    if (nin <= 0) { lfail(l, "nothing to link", 0); goto done; }

    /* 1. every plain object joins unconditionally, in the order given. */
    for (int i = 0; i < nin && !l->failed; i++) {
        if (ar_is_archive(in[i].data, in[i].len)) continue;
        if (l->nunit >= MAX_UNITS) { lfail(l, "too many objects", 0); break; }
        if (unit_parse(l, &l->units[l->nunit], in[i].name,
                       in[i].data, in[i].len) != 0) break;
        publish(l, l->nunit);
        l->nunit++;
    }
    if (l->failed) goto done;
    if (!l->nunit) { lfail(l, "no object files to link (an archive alone is not a program)", 0); goto done; }

    /* 2. pull from the archives until a pass adds nothing. */
    for (int round = 0; round < MAX_UNITS && !l->failed; round++) {
        int added = 0;
        for (int i = 0; i < nin && !l->failed; i++) {
            if (!ar_is_archive(in[i].data, in[i].len)) continue;

            const char *longnames = 0;
            uint32_t    longlen = 0;
            uint32_t    pos = 8;
            ar_member_t m;
            int         rc;
            while ((rc = ar_next(in[i].data, in[i].len, &pos, &longnames, &longlen, &m)) == 1) {
                if (m.kind != AR_FILE || !m.name[0]) continue;

                int already = 0;
                for (int u = 0; u < l->nunit; u++)
                    if (l->units[u].img == m.data) { already = 1; break; }
                if (already) continue;
                if (!member_wanted(l, &m)) continue;

                if (l->nunit >= MAX_UNITS) { lfail(l, "too many objects", 0); break; }
                char label[96];
                label[0] = 0;
                strlcat(label, in[i].name, sizeof label);
                strlcat(label, "(", sizeof label);
                strlcat(label, m.name, sizeof label);
                strlcat(label, ")", sizeof label);
                if (unit_parse(l, &l->units[l->nunit], label, m.data, m.len) != 0) break;
                l->units[l->nunit].from_archive = 1;
                publish(l, l->nunit);
                l->nunit++;
                added = 1;
            }
            if (rc < 0) { lfail(l, "malformed archive:", in[i].name); break; }
        }
        if (!added) break;
    }
    if (l->failed) goto done;

    {
        const char *missing = 0;
        if (any_unresolved(l, &missing)) { lfail(l, "undefined reference to", missing); goto done; }
    }

    /* 3. lay the allocatable sections out end to end, .text then .data.
     *
     * Data goes in two rounds: everything with contents first, then every
     * SHT_NOBITS (.bss) after it. That ordering is what lets the data segment
     * declare a p_memsz larger than its p_filesz, so a megabyte of .bss costs a
     * megabyte of address space and nothing at all in the file. Interleaving
     * them would force every zero byte to be written out, which is what the
     * first version of this did. */
    uint32_t tsize = 0, dsize = 0, dfile = 0;
    for (int i = 0; i < l->nunit; i++) {
        Unit *u = &l->units[i];
        for (uint16_t s = 0; s < u->nsh; s++) {
            if (u->cls[s] != CLS_TEXT) continue;
            uint32_t a = (uint32_t)(u->sh[s].sh_addralign ? u->sh[s].sh_addralign : 1);
            if (a > PAGE) a = PAGE;
            tsize = align_up(tsize, a);
            u->off[s] = tsize;
            tsize += (uint32_t)u->sh[s].sh_size;
            if (tsize > MAX_IMAGE) {
                lfail(l, "the linked image is larger than this linker will build", 0);
                goto done;
            }
        }
    }
    for (int round = 0; round < 2; round++) {
        if (round == 1) dfile = dsize;
        for (int i = 0; i < l->nunit; i++) {
            Unit *u = &l->units[i];
            for (uint16_t s = 0; s < u->nsh; s++) {
                if (u->cls[s] != CLS_DATA) continue;
                int nobits = (u->sh[s].sh_type == SHT_NOBITS);
                if (nobits != round) continue;
                uint32_t a = (uint32_t)(u->sh[s].sh_addralign ? u->sh[s].sh_addralign : 1);
                if (a > PAGE) a = PAGE;
                dsize = align_up(dsize, a);
                u->off[s] = dsize;
                dsize += (uint32_t)u->sh[s].sh_size;
                if (dsize > MAX_IMAGE) {
                    lfail(l, "the linked image is larger than this linker will build", 0);
                    goto done;
                }
            }
        }
    }

    uint64_t text_base = ASM_LOAD_BASE + TEXT_OFF;
    uint32_t doff      = TEXT_OFF + align_up(tsize, PAGE);
    uint64_t data_base = ASM_LOAD_BASE + doff;

    text = (uint8_t *)kmalloc(tsize ? tsize : 1);
    data = (uint8_t *)kmalloc(dsize ? dsize : 1);
    if (!text || !data) { lfail(l, "out of memory merging sections", 0); goto done; }
    memset(text, 0, tsize ? tsize : 1);
    memset(data, 0, dsize ? dsize : 1);

    for (int i = 0; i < l->nunit; i++) {
        Unit *u = &l->units[i];
        for (uint16_t s = 0; s < u->nsh; s++) {
            if (u->cls[s] == CLS_NONE || u->sh[s].sh_type == SHT_NOBITS) continue;
            uint32_t sz = (uint32_t)u->sh[s].sh_size;
            if (!sz) continue;
            memcpy((u->cls[s] == CLS_TEXT ? text : data) + u->off[s],
                   u->img + u->sh[s].sh_offset, sz);
        }
    }

    /* 4. every symbol gets an address. Locals from their own unit, globals from
     *    whoever defined them, and an `equ` from nobody at all. */
    for (int i = 0; i < l->nunit; i++) {
        Unit *u = &l->units[i];
        u->symval = (uint64_t *)kmalloc((u->nsym ? u->nsym : 1) * sizeof(uint64_t));
        u->symok  = (uint8_t *)kmalloc(u->nsym ? u->nsym : 1);
        if (!u->symval || !u->symok) { lfail(l, "out of memory resolving symbols", 0); goto done; }
        memset(u->symval, 0, (u->nsym ? u->nsym : 1) * sizeof(uint64_t));
        memset(u->symok, 0, u->nsym ? u->nsym : 1);
    }

    for (int i = 0; i < l->nunit; i++) {
        Unit *u = &l->units[i];
        for (uint32_t k = 0; k < u->nsym; k++) {
            const Elf64_Sym *s = &u->syms[k];
            if (s->st_shndx == SHN_ABS) { u->symval[k] = s->st_value; u->symok[k] = 1; continue; }
            if (s->st_shndx == SHN_UNDEF) continue;      /* filled in below */
            if (s->st_shndx >= u->nsh || u->cls[s->st_shndx] == CLS_NONE) continue;
            uint64_t base = (u->cls[s->st_shndx] == CLS_TEXT) ? text_base : data_base;
            u->symval[k] = base + u->off[s->st_shndx] + s->st_value;
            u->symok[k]  = 1;
        }
    }

    /* Globals now have real addresses, so the table the undefined ones look
     * themselves up in can be filled in. */
    for (int gi = 0; gi < l->nglobal; gi++) {
        Global *g = &l->globals[gi];
        Unit *u = &l->units[g->unit];
        for (uint32_t k = 0; k < u->nsym; k++) {
            const Elf64_Sym *s = &u->syms[k];
            if (ELF_ST_BIND(s->st_info) != STB_GLOBAL || s->st_shndx == SHN_UNDEF) continue;
            if (strcmp(sym_name(u, s), g->name)) continue;
            g->value = u->symval[k];
            break;
        }
    }
    for (int i = 0; i < l->nunit; i++) {
        Unit *u = &l->units[i];
        for (uint32_t k = 0; k < u->nsym; k++) {
            if (u->syms[k].st_shndx != SHN_UNDEF) continue;
            const char *nm = sym_name(u, &u->syms[k]);
            if (!nm[0]) continue;
            Global *g = global_find(l, nm);
            if (!g) { lfail(l, "undefined reference to", nm); goto done; }
            u->symval[k] = g->value;
            u->symok[k]  = 1;
        }
    }

    /* 5. fill the holes. */
    uint32_t nrel = 0;
    for (int i = 0; i < l->nunit && !l->failed; i++) {
        Unit *u = &l->units[i];
        for (uint16_t s = 0; s < u->nsh && !l->failed; s++) {
            if (u->sh[s].sh_type != SHT_RELA) continue;
            uint16_t tsec = (uint16_t)u->sh[s].sh_info;
            if (tsec >= u->nsh || u->cls[tsec] == CLS_NONE) continue;

            uint8_t *dst  = (u->cls[tsec] == CLS_TEXT) ? text : data;
            uint32_t dcap = (u->cls[tsec] == CLS_TEXT) ? tsize : dsize;
            uint64_t base = (u->cls[tsec] == CLS_TEXT) ? text_base : data_base;

            const Elf64_Rela *r = (const Elf64_Rela *)(u->img + u->sh[s].sh_offset);
            uint32_t n = (uint32_t)(u->sh[s].sh_size / sizeof(Elf64_Rela));
            for (uint32_t j = 0; j < n; j++, r++) {
                uint32_t sym = ELF64_R_SYM(r->r_info);
                uint32_t typ = ELF64_R_TYPE(r->r_info);
                if (sym >= u->nsym || !u->symok[sym]) {
                    lfail(l, "a relocation names a symbol that is not there, in", u->label);
                    break;
                }
                uint32_t width = (typ == R_X86_64_64) ? 8 : 4;
                uint32_t at = u->off[tsec] + (uint32_t)r->r_offset;
                if (r->r_offset + width > u->sh[tsec].sh_size || at + width > dcap) {
                    lfail(l, "a relocation points outside its own section, in", u->label);
                    break;
                }
                uint64_t S = u->symval[sym];
                int64_t  A = r->r_addend;
                uint64_t P = base + at;
                int64_t  v;
                switch (typ) {
                case R_X86_64_NONE:
                    continue;
                case R_X86_64_64:
                    for (int b = 0; b < 8; b++) dst[at + b] = (uint8_t)((S + (uint64_t)A) >> (b * 8));
                    nrel++;
                    continue;
                case R_X86_64_PC32:
                case R_X86_64_PLT32:
                    v = (int64_t)S + A - (int64_t)P;
                    break;
                case R_X86_64_32:
                case R_X86_64_32S:
                    v = (int64_t)S + A;
                    break;
                default:
                    lfail(l, "a relocation type this linker does not know, in", u->label);
                    v = 0;
                    break;
                }
                if (l->failed) break;
                if (v < -2147483648LL || v > 2147483647LL) {
                    /* Only reachable with a >2GB image, which this machine
                     * cannot allocate -- kept because "the branch went
                     * somewhere else" is a far worse way to find out. */
                    lfail(l, "a reference is too far away to encode, in", u->label);
                    break;
                }
                for (int b = 0; b < 4; b++) dst[at + b] = (uint8_t)(((uint32_t)(int32_t)v) >> (b * 8));
                nrel++;
            }
        }
    }
    if (l->failed) goto done;

    /* 6. the entry point, and then the file. */
    Global *ge = global_find(l, entry);
    if (!ge) { lfail(l, "no entry point; nothing defines", entry); goto done; }

    {
        /* The symbol table goes out as it came in, bindings and types intact,
         * so a crash in the linked program is symbolised the same way a crash
         * in an `as`-built one is. Locals are kept: they are most of what makes
         * a backtrace through generated code readable. */
        uint32_t symbytes = 24, strbytes = 1;
        for (int i = 0; i < l->nunit; i++) {
            Unit *u = &l->units[i];
            for (uint32_t k = 0; k < u->nsym; k++) {
                const Elf64_Sym *s = &u->syms[k];
                if (!u->symok[k] || s->st_shndx == SHN_UNDEF) continue;
                const char *nm = sym_name(u, s);
                if (!nm[0]) continue;
                symbytes += 24;
                strbytes += (uint32_t)strlen(nm) + 1;
            }
        }

        static const char SHSTR[] = "\0.text\0.data\0.symtab\0.strtab\0.shstrtab";
        const uint32_t n_text = 1, n_data = 7, n_sym = 13, n_str = 21, n_shs = 29;
        const uint32_t shsbytes = sizeof SHSTR;

        #define A8(x) (((x) + 7u) & ~7u)
        uint32_t o_sym = A8(doff + dfile);
        uint32_t o_str = o_sym + symbytes;
        uint32_t o_shs = o_str + strbytes;
        uint32_t o_sh  = A8(o_shs + shsbytes);
        uint32_t total = o_sh + 6 * 64;
        #undef A8

        out = (uint8_t *)kmalloc(total);
        if (!out) { lfail(l, "out of memory writing the program", 0); goto done; }
        memset(out, 0, total);

        uint8_t *h = out;
        h[0] = 0x7F; h[1] = 'E'; h[2] = 'L'; h[3] = 'F';
        h[4] = ELFCLASS64; h[5] = 1; h[6] = 1;
        h[16] = ET_EXEC;
        h[18] = EM_X86_64;
        h[20] = 1;
        for (int b = 0; b < 8; b++) h[24 + b] = (uint8_t)(ge->value >> (b * 8));
        h[32] = 64;                                  /* e_phoff */
        for (int b = 0; b < 4; b++) h[40 + b] = (uint8_t)(o_sh >> (b * 8));
        h[52] = 64;                                  /* e_ehsize    */
        h[54] = 56;                                  /* e_phentsize */
        h[56] = 2;                                   /* e_phnum     */
        h[58] = 64;                                  /* e_shentsize */
        h[60] = 6;                                   /* e_shnum     */
        h[62] = 5;                                   /* e_shstrndx  */

        /* The data segment is the only one where memsz and filesz differ: the
         * .bss sections were laid out after everything with contents, so the
         * tail of it is address space the loader zeroes rather than bytes the
         * file carries. */
        struct { uint32_t type, flags; uint64_t off, va, filesz, memsz; } ph[2] = {
            { PT_LOAD, PF_R | PF_X, 0,    ASM_LOAD_BASE,      TEXT_OFF + tsize, TEXT_OFF + tsize },
            { PT_LOAD, PF_R | PF_W, doff, ASM_LOAD_BASE+doff, dfile,            dsize },
        };
        for (int i = 0; i < 2; i++) {
            uint8_t *p = out + 64 + i * 56;
            for (int b = 0; b < 4; b++) p[0 + b] = (uint8_t)(ph[i].type >> (b * 8));
            for (int b = 0; b < 4; b++) p[4 + b] = (uint8_t)(ph[i].flags >> (b * 8));
            for (int b = 0; b < 8; b++) p[8 + b]  = (uint8_t)(ph[i].off >> (b * 8));
            for (int b = 0; b < 8; b++) p[16 + b] = (uint8_t)(ph[i].va >> (b * 8));
            for (int b = 0; b < 8; b++) p[24 + b] = (uint8_t)(ph[i].va >> (b * 8));
            for (int b = 0; b < 8; b++) p[32 + b] = (uint8_t)(ph[i].filesz >> (b * 8));
            for (int b = 0; b < 8; b++) p[40 + b] = (uint8_t)(ph[i].memsz >> (b * 8));
            for (int b = 0; b < 8; b++) p[48 + b] = (uint8_t)(((uint64_t)PAGE) >> (b * 8));
        }

        if (tsize) memcpy(out + TEXT_OFF, text, tsize);
        if (dfile) memcpy(out + doff, data, dfile);

        uint32_t so = o_sym + 24, to = o_str + 1;
        for (int i = 0; i < l->nunit; i++) {
            Unit *u = &l->units[i];
            for (uint32_t k = 0; k < u->nsym; k++) {
                const Elf64_Sym *s = &u->syms[k];
                if (!u->symok[k] || s->st_shndx == SHN_UNDEF) continue;
                const char *nm = sym_name(u, s);
                if (!nm[0]) continue;
                uint32_t nlen = (uint32_t)strlen(nm) + 1;
                memcpy(out + to, nm, nlen);
                uint8_t *e = out + so;
                /* st_name is an offset into .strtab, not into the file. nm
                 * reported every symbol as <corrupt> until this stopped being
                 * the file offset it was written at. */
                uint32_t nameoff = to - o_str;
                for (int b = 0; b < 4; b++) e[b] = (uint8_t)(nameoff >> (b * 8));
                e[4] = s->st_info;
                e[6] = (uint8_t)((s->st_shndx == SHN_ABS) ? 0xf1 : 1);
                e[7] = (uint8_t)((s->st_shndx == SHN_ABS) ? 0xff : 0);
                if (s->st_shndx != SHN_ABS && s->st_shndx < u->nsh &&
                    u->cls[s->st_shndx] == CLS_DATA) e[6] = 2;
                for (int b = 0; b < 8; b++) e[8 + b]  = (uint8_t)(u->symval[k] >> (b * 8));
                for (int b = 0; b < 8; b++) e[16 + b] = (uint8_t)(s->st_size >> (b * 8));
                so += 24;
                to += nlen;
            }
        }
        memcpy(out + o_shs, SHSTR, shsbytes);

        struct { uint32_t name, type; uint64_t flags, addr, off, size;
                 uint32_t link, info; uint64_t align, ent; } sh[6] = {
            { 0,      0,            0,                         0,         0,        0,        0, 0, 0,  0 },
            { n_text, SHT_PROGBITS, SHF_ALLOC|SHF_EXECINSTR,   text_base, TEXT_OFF, tsize,    0, 0, 16, 0 },
            { n_data, SHT_PROGBITS, SHF_ALLOC|SHF_WRITE,       data_base, doff,     dfile,    0, 0, 16, 0 },
            { n_sym,  SHT_SYMTAB,   0,                         0,         o_sym,    symbytes, 4, 1, 8,  24 },
            { n_str,  SHT_STRTAB,   0,                         0,         o_str,    strbytes, 0, 0, 1,  0 },
            { n_shs,  SHT_STRTAB,   0,                         0,         o_shs,    shsbytes, 0, 0, 1,  0 },
        };
        for (int i = 0; i < 6; i++) {
            uint8_t *p = out + o_sh + (uint32_t)i * 64;
            for (int b = 0; b < 4; b++) p[0 + b]  = (uint8_t)(sh[i].name >> (b * 8));
            for (int b = 0; b < 4; b++) p[4 + b]  = (uint8_t)(sh[i].type >> (b * 8));
            for (int b = 0; b < 8; b++) p[8 + b]  = (uint8_t)(sh[i].flags >> (b * 8));
            for (int b = 0; b < 8; b++) p[16 + b] = (uint8_t)(sh[i].addr >> (b * 8));
            for (int b = 0; b < 8; b++) p[24 + b] = (uint8_t)(sh[i].off >> (b * 8));
            for (int b = 0; b < 8; b++) p[32 + b] = (uint8_t)(sh[i].size >> (b * 8));
            for (int b = 0; b < 4; b++) p[40 + b] = (uint8_t)(sh[i].link >> (b * 8));
            for (int b = 0; b < 4; b++) p[44 + b] = (uint8_t)(sh[i].info >> (b * 8));
            for (int b = 0; b < 8; b++) p[48 + b] = (uint8_t)(sh[i].align >> (b * 8));
            for (int b = 0; b < 8; b++) p[56 + b] = (uint8_t)(sh[i].ent >> (b * 8));
        }

        *outlen = total;
        if (stats) {
            stats->objects = (uint32_t)l->nunit;
            stats->from_archives = 0;
            for (int i = 0; i < l->nunit; i++)
                if (l->units[i].from_archive) stats->from_archives++;
            stats->relocs = nrel;
            stats->text   = tsize;
            stats->data   = dfile;
            stats->bss    = dsize - dfile;
            stats->entry  = ge->value;
        }
    }

done:
    if (l->failed) { strlcpy(err, l->err, errcap); if (out) { kfree(out); out = 0; } }
    ld_free_units(l);
    if (text) kfree(text);
    if (data) kfree(data);
    kfree(l);
    return out;
}
