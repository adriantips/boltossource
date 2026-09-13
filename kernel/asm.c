#include <stdint.h>
#include <stddef.h>
#include "asm.h"
#include "elf.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"

/* ===========================================================================
 *  BoltAS. See include/asm.h for what this is and why the two passes have to
 *  agree about instruction lengths.
 *
 *  Layout of what comes out:
 *
 *      file 0x0000  ELF header + two program headers
 *      file 0x1000  .text            vaddr 0x401000
 *      file 0x2000  .data            vaddr 0x402000   (next page after .text)
 *
 *  Two segments, both page aligned, so the loader never has to map one page
 *  for two of them. The entry point is `_start`, or whatever `global` names.
 * ===========================================================================*/

#define MAX_SYMS   4096
#define TEXT_OFF   0x1000
#define PAGE       0x1000

/* Which section a name belongs to. ABS is an `equ`, UNDEF is a name this file
 * uses and does not define -- only legal in an object, where the linker is the
 * one that has to find it. */
enum { S_TEXT = 0, S_DATA = 1, S_ABS = 2, S_UNDEF = 3 };

/* Three passes, not two, and the middle one is the reason.
 *
 * A .data label's address depends on how long .text turned out to be, which
 * P_SCAN only knows once it has finished. For a file that puts .data last that
 * is fine -- .text was already complete by the time the section changed. For a
 * file that goes back to .text afterwards it is not: P_SCAN placed those data
 * labels above a partial .text and they move once the real length is known.
 *
 * P_FIX re-runs with the right .text length and lets every symbol settle at its
 * final address. P_EMIT then has to reproduce those addresses exactly, and says
 * so if it cannot. Instruction lengths never depend on a symbol's value -- every
 * jump is rel32, every label immediate a movabs, every [label] a disp32 -- so
 * .text is the same length in all three, and the addresses P_FIX settles on are
 * the ones P_EMIT writes. */
#define P_SCAN 1
#define P_FIX  2
#define P_EMIT 3

typedef struct { char name[32]; uint64_t value; int defined, sect, global; } Sym;

/* A hole in a section and the name that fills it. Recorded on the last pass
 * only: the earlier two exist to settle lengths and addresses, and a
 * relocation from a pass that later gets re-run is a duplicate. */
typedef struct { uint32_t off; int sect, sym, type; int64_t addend; } Reloc;

typedef struct {
    const char *src;
    const char *p;            /* cursor */
    int   line;
    int   pass;

    uint8_t  *text; uint32_t tlen, tcap;
    uint32_t  text_total;     /* .text's final length, known after pass one */
    uint8_t  *data; uint32_t dlen, dcap;
    int       in_data;        /* which section the cursor is writing to */

    Sym  syms[MAX_SYMS]; int nsym;
    char entry[32];
    int  saw_global;

    int     obj;              /* writing a relocatable object, not a program */
    Reloc  *relocs; int nrel, relcap;

    char err[ASM_MAX_ERR];
    int  failed;
} AS;

static void fail(AS *a, const char *msg, const char *what) {
    if (a->failed) return;                  /* keep the first one */
    a->failed = 1;
    char num[12]; int n = 0, v = a->line;
    char tmp[12]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t) num[n++] = tmp[--t];
    num[n] = 0;
    a->err[0] = 0;
    strlcat(a->err, "line ", sizeof a->err);
    strlcat(a->err, num, sizeof a->err);
    strlcat(a->err, ": ", sizeof a->err);
    strlcat(a->err, msg, sizeof a->err);
    if (what && what[0]) {
        strlcat(a->err, ": ", sizeof a->err);
        strlcat(a->err, what, sizeof a->err);
    }
}

/* ---- output buffers ------------------------------------------------------ */
static int grow(uint8_t **buf, uint32_t *cap, uint32_t need) {
    if (need <= *cap) return 0;
    uint32_t nc = *cap ? *cap * 2 : 4096;
    while (nc < need) nc *= 2;
    uint8_t *nb = (uint8_t *)kmalloc(nc);
    if (!nb) return -1;
    if (*buf) { memcpy(nb, *buf, *cap); kfree(*buf); }
    *buf = nb; *cap = nc;
    return 0;
}

static void emit(AS *a, uint8_t b) {
    uint8_t **buf = a->in_data ? &a->data : &a->text;
    uint32_t *len = a->in_data ? &a->dlen : &a->tlen;
    uint32_t *cap = a->in_data ? &a->dcap : &a->tcap;
    if (grow(buf, cap, *len + 1) != 0) { fail(a, "out of memory", 0); return; }
    (*buf)[(*len)++] = b;
}
static void emit32(AS *a, uint32_t v) { for (int i = 0; i < 4; i++) emit(a, (uint8_t)(v >> (i * 8))); }
static void emit64(AS *a, uint64_t v) { for (int i = 0; i < 8; i++) emit(a, (uint8_t)(v >> (i * 8))); }

/* Where the next byte will land once loaded. */
static uint64_t here(AS *a);

/* ---- symbols ------------------------------------------------------------- */
static Sym *sym_find(AS *a, const char *name) {
    for (int i = 0; i < a->nsym; i++)
        if (strcmp(a->syms[i].name, name) == 0) return &a->syms[i];
    return 0;
}

/* The symbol of that name, making an undefined one if it is new. A file can
 * mention a name before (or without) defining it -- a forward jump, an
 * `extern`, a `global` written above the label it names -- and all three want
 * the same entry rather than three of them. */
static Sym *sym_get(AS *a, const char *name) {
    Sym *s = sym_find(a, name);
    if (s) return s;
    if (a->nsym >= MAX_SYMS) { fail(a, "too many symbols", 0); return 0; }
    s = &a->syms[a->nsym++];
    memset(s, 0, sizeof *s);
    strlcpy(s->name, name, sizeof s->name);
    s->sect = S_UNDEF;
    return s;
}

static void sym_define(AS *a, const char *name, uint64_t value) {
    Sym *s = sym_find(a, name);
    if (s && s->defined) {
        if (a->pass == P_SCAN) { fail(a, "duplicate label", name); return; }
        if (a->pass == P_FIX) {
            s->value = value;
            s->sect  = a->in_data ? S_DATA : S_TEXT;
            return;
        }
        /* The last pass re-defines every label and must land on the address the
         * one before it settled on. If it does not, the two disagreed about how
         * long some instruction is, and every reference past this point is
         * wrong -- worth stopping for rather than emitting. */
        if (s->value != value) { fail(a, "internal: passes disagree at", name); return; }
        return;
    }
    if (!s) s = sym_get(a, name);
    if (!s) return;
    s->value   = value;
    s->defined = 1;
    s->sect    = a->in_data ? S_DATA : S_TEXT;
}

/* Where a field that names a symbol lands, and what fills it. Nothing is
 * recorded while the passes are still agreeing on lengths. */
static void reloc_add(AS *a, int type, const char *name, int64_t addend) {
    if (a->pass != P_EMIT) return;
    Sym *s = sym_get(a, name);
    if (!s) return;
    if (a->nrel >= a->relcap) {
        int nc = a->relcap ? a->relcap * 2 : 256;
        Reloc *nb = (Reloc *)kmalloc((uint32_t)nc * sizeof(Reloc));
        if (!nb) { fail(a, "out of memory recording relocations", 0); return; }
        if (a->relocs) {
            memcpy(nb, a->relocs, (uint32_t)a->nrel * sizeof(Reloc));
            kfree(a->relocs);
        }
        a->relocs = nb; a->relcap = nc;
    }
    Reloc *r = &a->relocs[a->nrel++];
    r->sect   = a->in_data;
    r->off    = a->in_data ? a->dlen : a->tlen;
    r->sym    = (int)(s - a->syms);
    r->type   = type;
    r->addend = addend;
}

/* ---- lexing -------------------------------------------------------------- */
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
/* Lines are walked in place rather than copied out, so end-of-line is a
 * newline as often as it is a NUL, and a comment ends one too. Getting this
 * wrong made every blank line in a file a syntax error. */
static int is_eol(char c) { return c == 0 || c == '\n' || c == ';'; }
static int is_idch(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '$';
}
static void skip_space(AS *a) { while (is_space(*a->p)) a->p++; }

static int word(AS *a, char *out, uint32_t cap) {
    skip_space(a);
    uint32_t n = 0;
    while (is_idch(*a->p) && n < cap - 1) out[n++] = *a->p++;
    out[n] = 0;
    return n > 0;
}

static void lower(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }

/* ---- registers ----------------------------------------------------------- */
/* Index is the encoding; every width's spelling of a register shares it. */
static const char *r64[16] = { "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                               "r8","r9","r10","r11","r12","r13","r14","r15" };
static const char *r32[16] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi",
                               "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d" };
/* The low byte of each. ah/ch/dh/bh are deliberately absent: they cannot be
 * named in an instruction that carries a REX prefix, and every instruction
 * here that touches r8-r15 does. Assembling `ah` as `spl` because the table
 * happened to line up is exactly the kind of silence this assembler avoids. */
static const char *r8l[16] = { "al","cl","dl","bl","spl","bpl","sil","dil",
                               "r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b" };

/* -1 if it is not a register; *size is the operand width in bytes. */
static int reg_of(const char *s, int *size) {
    for (int i = 0; i < 16; i++) if (strcmp(s, r64[i]) == 0) { if (size) *size = 8; return i; }
    for (int i = 0; i < 16; i++) if (strcmp(s, r32[i]) == 0) { if (size) *size = 4; return i; }
    for (int i = 0; i < 16; i++) if (strcmp(s, r8l[i]) == 0) { if (size) *size = 1; return i; }
    return -1;
}

/* spl, bpl, sil and dil are only reachable *because* a REX prefix is present:
 * without one those four encodings mean ah, ch, dh and bh. So a byte operand
 * naming one of them forces an otherwise-empty 0x40 to be emitted. */
static int byte_needs_rex(int size, int reg) { return size == 1 && reg >= 4 && reg < 8; }

/* ---- operands ------------------------------------------------------------ */
enum { O_NONE, O_REG, O_IMM, O_MEM, O_LABELIMM };

typedef struct {
    int kind;
    int reg;           /* O_REG, or the base of O_MEM (-1 = RIP-relative)  */
    int size;          /* operand width in bytes: 8, 4, 1 -- 0 if unstated */
    int64_t imm;       /* O_IMM, or the displacement of O_MEM              */
    char label[32];    /* O_LABELIMM, or an O_MEM whose base is a label    */
    char dlabel[32];   /* [reg+name] -- the displacement is a symbol       */
} Opnd;

/* A memory operand carries no width of its own, so `movzx rax, [rsi]` does not
 * say how much to load. NASM spells that with a size keyword in front of the
 * bracket and so does this. */
static int size_keyword(const char *t) {
    if (!strcmp(t, "byte"))  return 1;
    if (!strcmp(t, "word"))  return 2;
    if (!strcmp(t, "dword")) return 4;
    if (!strcmp(t, "qword")) return 8;
    return 0;
}

static int64_t parse_number(AS *a, const char *s, int *ok) {
    *ok = 1;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    int64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (!*s) { *ok = 0; return 0; }
        for (; *s; s++) {
            int d;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else { *ok = 0; return 0; }
            v = v * 16 + d;
        }
    } else {
        if (!(*s >= '0' && *s <= '9')) { *ok = 0; return 0; }
        for (; *s; s++) {
            if (*s < '0' || *s > '9') { *ok = 0; return 0; }
            v = v * 10 + (*s - '0');
        }
    }
    (void)a;
    return neg ? -v : v;
}

/* A name or a number, wherever a value is wanted. A name defined further down
 * the file is zero until the pass that reaches it -- that is the point of there
 * being more than one -- and an error on the last, by which time every label
 * that exists has been seen. */
static int64_t value_of(AS *a, const char *t) {
    int ok;
    int64_t v = parse_number(a, t, &ok);
    if (ok) return v;
    Sym *s = sym_find(a, t);
    if (s) return (int64_t)s->value;
    /* An object may name what it cannot resolve; that is what a relocation is
     * for, and an unresolved name becomes the linker's complaint rather than
     * this file's. A program has nobody else to ask. */
    if (a->obj) { sym_get(a, t); return 0; }
    if (a->pass == P_EMIT) fail(a, "undefined symbol", t);
    return 0;
}

/* Where the number has to be a number: a repeat count, an `equ`. These decide
 * how much gets emitted, so nothing downstream can patch them up later. */
static int64_t value_const(AS *a, const char *t) {
    int ok;
    int64_t v = parse_number(a, t, &ok);
    if (ok) return v;
    Sym *s = sym_find(a, t);
    if (s && s->defined) return (int64_t)s->value;
    if (a->pass == P_EMIT) fail(a, "expected a constant, got", t);
    return 0;
}

/* A field that names a symbol. Assembling a program resolves it here;
 * assembling an object leaves a hole and a note saying what goes in it, which
 * is the only difference between the two outputs. */
static void emit_sym32(AS *a, const char *name, int type, int64_t addend, uint32_t val) {
    if (a->obj && name && name[0]) { reloc_add(a, type, name, addend); val = 0; }
    emit32(a, val);
}
static void emit_sym64(AS *a, const char *name, int type, int64_t addend, uint64_t val) {
    if (a->obj && name && name[0]) { reloc_add(a, type, name, addend); val = 0; }
    emit64(a, val);
}

static int parse_operand(AS *a, Opnd *o) {
    memset(o, 0, sizeof *o);
    skip_space(a);
    if (is_eol(*a->p)) { o->kind = O_NONE; return 0; }

    int stated = 0;
    if (*a->p != '[') {
        /* Peek for a size keyword without consuming anything else: `byte` is
         * only a keyword when a memory operand follows it, and is otherwise an
         * ordinary name someone may have used as a label. */
        const char *save = a->p;
        char t[32];
        if (word(a, t, sizeof t)) {
            lower(t);
            int sz = size_keyword(t);
            skip_space(a);
            if (sz && *a->p == '[') stated = sz;
            else a->p = save;
        } else {
            a->p = save;
        }
    }

    if (*a->p == '[') {                       /* [reg], [reg+disp], [label] */
        a->p++;
        char t[32];
        if (!word(a, t, sizeof t)) { fail(a, "empty [ ]", 0); return -1; }
        /* A register is spelled in any case; a label is spelled exactly once,
         * and lowercasing it here would leave `[Msg]` unable to find `Msg:`. */
        char raw_mem[32];
        strlcpy(raw_mem, t, sizeof raw_mem);
        lower(t);
        int rsz, r = reg_of(t, &rsz);
        o->kind = O_MEM;
        o->size = stated;
        if (r >= 0) {
            if (rsz != 8) { fail(a, "an address wants a 64-bit register", t); return -1; }
            o->reg = r;
            skip_space(a);
            if (*a->p == '+' || *a->p == '-') {
                int neg = (*a->p == '-');
                a->p++;
                char d[32];
                if (!word(a, d, sizeof d)) { fail(a, "expected a displacement", 0); return -1; }
                int isnum;
                parse_number(a, d, &isnum);
                if (!isnum && !neg) strlcpy(o->dlabel, d, sizeof o->dlabel);
                else if (!isnum && neg && a->obj) {
                    fail(a, "a subtracted label cannot be relocated", d); return -1;
                }
                int64_t dv = value_of(a, d);
                o->imm = neg ? -dv : dv;
            }
        } else {
            /* [label] -- RIP-relative, which is how a flat, non-relocatable
             * program reaches its own data without a fixup table. */
            o->reg = -1;
            strlcpy(o->label, raw_mem, sizeof o->label);
        }
        skip_space(a);
        if (*a->p != ']') { fail(a, "expected ]", 0); return -1; }
        a->p++;
        return 0;
    }

    char t[32];
    if (!word(a, t, sizeof t)) { fail(a, "expected an operand", 0); return -1; }
    char raw[32];
    strlcpy(raw, t, sizeof raw);
    lower(t);
    int rsz, r = reg_of(t, &rsz);
    if (r >= 0) { o->kind = O_REG; o->reg = r; o->size = rsz; return 0; }

    int ok;
    int64_t v = parse_number(a, raw, &ok);
    if (ok) { o->kind = O_IMM; o->imm = v; return 0; }

    o->kind = O_LABELIMM;
    strlcpy(o->label, raw, sizeof o->label);
    return 0;
}

/* ---- encoding ------------------------------------------------------------ */
/* An object's sections both start at zero -- they have no address until
 * something links them -- which incidentally makes the middle pass a no-op
 * there, since a data label can no longer move when .text changes length. */
static uint64_t text_va(AS *a) { return a->obj ? 0 : ASM_LOAD_BASE + TEXT_OFF; }
static uint64_t data_va(AS *a) {
    if (a->obj) return 0;
    /* .data sits on the page after .text, so its address depends on how long
     * .text turned out to be -- which is only known once a whole pass has run.
     * Before that (P_SCAN) the running length is the best guess available; see
     * the pass constants above for why one more pass is what makes it right. */
    uint32_t t = a->text_total ? a->text_total : a->tlen;
    t = (t + PAGE - 1) & ~(PAGE - 1);
    return ASM_LOAD_BASE + TEXT_OFF + t;
}
static uint64_t here(AS *a) {
    return a->in_data ? data_va(a) + a->dlen : text_va(a) + a->tlen;
}

static void rex_f(AS *a, int w, int r, int x, int b, int force) {
    uint8_t v = (uint8_t)(0x40 | (w ? 8 : 0) | ((r >> 3) & 1) << 2 |
                          ((x >> 3) & 1) << 1 | ((b >> 3) & 1));
    if (v != 0x40 || w || force) emit(a, v);
}
static void rex(AS *a, int w, int r, int x, int b) { rex_f(a, w, r, x, b, 0); }

/* The REX for an instruction whose operands are `size` bytes wide, where `r` is
 * the ModRM.reg operand and `b` the r/m one. Either may be -1 for "not a
 * register here". */
static void rex_sz(AS *a, int size, int r, int b) {
    int force = (r >= 0 && byte_needs_rex(size, r)) ||
                (b >= 0 && byte_needs_rex(size, b));
    rex_f(a, size == 8, r < 0 ? 0 : r, 0, b < 0 ? 0 : b, force);
}

/* The same, but `base` addresses memory rather than holding an operand. Its
 * width is not the instruction's, so it never forces a prefix: `mov [rdi], al`
 * needs no REX, and emitting one because rdi's number happens to fall in the
 * range that means dil for a byte operand would be reading the wrong field. */
static void rex_mem(AS *a, int size, int r, int base) {
    rex_f(a, size == 8, r < 0 ? 0 : r, 0, base < 0 ? 0 : base,
          r >= 0 && byte_needs_rex(size, r));
}
static void modrm(AS *a, int mod, int reg, int rm) {
    emit(a, (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
}

/* reg-with-memory: the ModRM/SIB/disp for `op reg, [mem]` and its reverse. */
static void mem_operand(AS *a, int reg, const Opnd *m, int extra_bytes) {
    if (m->reg < 0) {                              /* RIP-relative [label] */
        modrm(a, 0, reg, 5);
        int64_t target = (int64_t)value_of(a, m->label);
        /* The displacement is measured from the end of the instruction, which
         * is four bytes of displacement plus whatever trails it. */
        int64_t next = (int64_t)(here(a) + 4 + extra_bytes);
        emit_sym32(a, m->label, R_X86_64_PC32, -(4 + extra_bytes),
                   (uint32_t)(int32_t)(target - next));
        return;
    }
    int base = m->reg;
    /* A label displacement always gets its four bytes even when the value is
     * zero: a relocation needs a field to land in, and mod 0 would not leave
     * one. */
    int need_disp = (m->imm != 0) || m->dlabel[0] || ((base & 7) == 5);
    int mod = need_disp ? 2 : 0;
    if ((base & 7) == 4) {                          /* rsp/r12 need a SIB */
        modrm(a, mod, reg, 4);
        emit(a, (uint8_t)(0x24));                   /* scale 0, no index, base */
    } else {
        modrm(a, mod, reg, base);
    }
    if (need_disp)
        emit_sym32(a, m->dlabel, R_X86_64_32S, 0, (uint32_t)(int32_t)m->imm);
}

/* ---- the instruction table ----------------------------------------------- */
/* The ALU group: the opcode for `r/m, r` and the /digit for the imm32 form. */
typedef struct { const char *name; uint8_t op_rm_r; uint8_t digit; } Alu;
static const Alu alus[] = {
    { "add", 0x01, 0 }, { "or",  0x09, 1 }, { "and", 0x21, 4 },
    { "sub", 0x29, 5 }, { "xor", 0x31, 6 }, { "cmp", 0x39, 7 },
};

typedef struct { const char *name; uint8_t cc; } Jcc;
static const Jcc jccs[] = {
    { "jo",0x0 },{ "jno",0x1 },{ "jb",0x2 },{ "jnb",0x3 },{ "jae",0x3 },
    { "je",0x4 },{ "jz",0x4 },{ "jne",0x5 },{ "jnz",0x5 },{ "jbe",0x6 },
    { "ja",0x7 },{ "js",0x8 },{ "jns",0x9 },{ "jl",0xC },{ "jge",0xD },
    { "jle",0xE },{ "jg",0xF },
};

static int fits32(int64_t v) { return v >= -2147483648LL && v <= 2147483647LL; }

/* One instruction. Returns 0 if it handled the mnemonic. */
static int encode(AS *a, const char *op) {
    Opnd d, s;

    if (!strcmp(op, "syscall")) { emit(a, 0x0F); emit(a, 0x05); return 0; }
    if (!strcmp(op, "ret"))     { emit(a, 0xC3); return 0; }
    if (!strcmp(op, "nop"))     { emit(a, 0x90); return 0; }
    if (!strcmp(op, "int3"))    { emit(a, 0xCC); return 0; }
    if (!strcmp(op, "cqo"))     { rex(a, 1, 0, 0, 0); emit(a, 0x99); return 0; }

    if (!strcmp(op, "push") || !strcmp(op, "pop")) {
        if (parse_operand(a, &d) != 0) return 0;
        if (d.kind != O_REG) { fail(a, "push/pop wants a register", 0); return 0; }
        if (d.reg >= 8) emit(a, 0x41);
        emit(a, (uint8_t)((op[1] == 'u' ? 0x50 : 0x58) + (d.reg & 7)));
        return 0;
    }

    if (!strcmp(op, "call") || !strcmp(op, "jmp")) {
        if (parse_operand(a, &d) != 0) return 0;
        emit(a, op[0] == 'c' ? 0xE8 : 0xE9);
        int64_t target = (d.kind == O_IMM) ? d.imm : (int64_t)value_of(a, d.label);
        emit_sym32(a, d.kind == O_IMM ? 0 : d.label, R_X86_64_PLT32, -4,
                   (uint32_t)(int32_t)(target - (int64_t)(here(a) + 4)));
        return 0;
    }

    for (unsigned i = 0; i < sizeof jccs / sizeof jccs[0]; i++) {
        if (strcmp(op, jccs[i].name)) continue;
        if (parse_operand(a, &d) != 0) return 0;
        emit(a, 0x0F);
        emit(a, (uint8_t)(0x80 | jccs[i].cc));
        int64_t target = (d.kind == O_IMM) ? d.imm : (int64_t)value_of(a, d.label);
        emit_sym32(a, d.kind == O_IMM ? 0 : d.label, R_X86_64_PC32, -4,
                   (uint32_t)(int32_t)(target - (int64_t)(here(a) + 4)));
        return 0;
    }

    /* The one-operand groups. inc/dec live in FF, the rest in F7, and the two
     * disagree about which /digit means what; the byte form of each is the
     * opcode one lower. mul/div are unsigned and imul/idiv signed, and they
     * are the only instructions here with implicit operands: all four take
     * rdx:rax apart, which is why anything dividing has to `cqo` first. */
    {
        static const struct { const char *name; uint8_t op; int digit; } un[] = {
            { "inc", 0xFF, 0 }, { "dec", 0xFF, 1 },
            { "not", 0xF7, 2 }, { "neg", 0xF7, 3 },
            { "mul", 0xF7, 4 }, { "div", 0xF7, 6 }, { "idiv", 0xF7, 7 },
        };
        for (unsigned i = 0; i < sizeof un / sizeof un[0]; i++) {
            if (strcmp(op, un[i].name)) continue;
            if (parse_operand(a, &d) != 0) return 0;
            if (d.kind != O_REG) { fail(a, "wants a register", op); return 0; }
            rex_sz(a, d.size, -1, d.reg);
            emit(a, (uint8_t)(d.size == 1 ? un[i].op - 1 : un[i].op));
            modrm(a, 3, un[i].digit, d.reg);
            return 0;
        }
    }

    /* setcc writes one byte, so it only ever takes an 8-bit register. */
    if (op[0] == 's' && op[1] == 'e' && op[2] == 't') {
        for (unsigned i = 0; i < sizeof jccs / sizeof jccs[0]; i++) {
            if (strcmp(op + 3, jccs[i].name + 1)) continue;
            if (parse_operand(a, &d) != 0) return 0;
            if (d.kind != O_REG || d.size != 1) {
                fail(a, "setcc wants an 8-bit register such as al", op); return 0;
            }
            rex_sz(a, 1, -1, d.reg);
            emit(a, 0x0F);
            emit(a, (uint8_t)(0x90 | jccs[i].cc));
            modrm(a, 3, 0, d.reg);
            return 0;
        }
    }

    /* movzx/movsx: the destination's width comes from the register named, the
     * source's from the register or the size keyword in front of the bracket.
     * Without the latter `movzx rax, [rsi]` would not say how much to load, so
     * it is an error rather than a guess. */
    if (!strcmp(op, "movzx") || !strcmp(op, "movsx")) {
        if (parse_operand(a, &d) != 0) return 0;
        skip_space(a); if (*a->p == ',') a->p++;
        if (parse_operand(a, &s) != 0) return 0;
        if (d.kind != O_REG || d.size == 1) { fail(a, "wants a wider destination register", op); return 0; }
        int ssz = (s.kind == O_REG) ? s.size : s.size;
        if (s.kind != O_REG && s.kind != O_MEM) { fail(a, "wants a register or [mem] source", op); return 0; }
        if (ssz != 1 && ssz != 2) { fail(a, "source must be byte or word", op); return 0; }
        uint8_t base = !strcmp(op, "movzx") ? 0xB6 : 0xBE;
        int srcreg = (s.kind == O_REG) ? s.reg : (s.reg < 0 ? 0 : s.reg);
        int force = (s.kind == O_REG) && byte_needs_rex(ssz, s.reg);
        rex_f(a, d.size == 8, d.reg, 0, srcreg, force);
        emit(a, 0x0F);
        emit(a, (uint8_t)(ssz == 1 ? base : base + 1));
        if (s.kind == O_REG) modrm(a, 3, d.reg, s.reg);
        else                 mem_operand(a, d.reg, &s, 0);
        return 0;
    }

    if (!strcmp(op, "imul")) {
        if (parse_operand(a, &d) != 0) return 0;
        skip_space(a); if (*a->p == ',') a->p++;
        if (parse_operand(a, &s) != 0) return 0;
        if (d.kind != O_REG || s.kind != O_REG) { fail(a, "imul wants two registers", 0); return 0; }
        rex_sz(a, d.size, d.reg, s.reg);
        emit(a, 0x0F); emit(a, 0xAF);
        modrm(a, 3, d.reg, s.reg);
        return 0;
    }

    if (!strcmp(op, "shl") || !strcmp(op, "shr") || !strcmp(op, "sar")) {
        if (parse_operand(a, &d) != 0) return 0;
        skip_space(a); if (*a->p == ',') a->p++;
        if (parse_operand(a, &s) != 0) return 0;
        if (d.kind != O_REG) { fail(a, "wants a register to shift", op); return 0; }
        int digit = !strcmp(op, "shl") ? 4 : !strcmp(op, "shr") ? 5 : 7;
        /* A count is either a constant or cl, and cl is not a choice: it is the
         * only register the instruction can take one from. */
        int by_cl = (s.kind == O_REG && s.reg == 1 && s.size == 1);
        if (!by_cl && s.kind != O_IMM) {
            fail(a, "wants a constant count or cl", op); return 0;
        }
        rex_sz(a, d.size, -1, d.reg);
        emit(a, (uint8_t)(by_cl ? (d.size == 1 ? 0xD2 : 0xD3)
                                : (d.size == 1 ? 0xC0 : 0xC1)));
        modrm(a, 3, digit, d.reg);
        if (!by_cl) emit(a, (uint8_t)s.imm);
        return 0;
    }

    if (!strcmp(op, "test")) {
        if (parse_operand(a, &d) != 0) return 0;
        skip_space(a); if (*a->p == ',') a->p++;
        if (parse_operand(a, &s) != 0) return 0;
        if (d.kind != O_REG || s.kind != O_REG) { fail(a, "test wants two registers", 0); return 0; }
        rex_sz(a, d.size, s.reg, d.reg);
        emit(a, (uint8_t)(d.size == 1 ? 0x84 : 0x85));
        modrm(a, 3, s.reg, d.reg);
        return 0;
    }

    if (!strcmp(op, "lea")) {
        if (parse_operand(a, &d) != 0) return 0;
        skip_space(a); if (*a->p == ',') a->p++;
        if (parse_operand(a, &s) != 0) return 0;
        if (d.kind != O_REG || s.kind != O_MEM) { fail(a, "lea wants a register and [mem]", 0); return 0; }
        rex(a, 1, d.reg, 0, s.reg < 0 ? 0 : s.reg);
        emit(a, 0x8D);
        mem_operand(a, d.reg, &s, 0);
        return 0;
    }

    if (!strcmp(op, "mov")) {
        if (parse_operand(a, &d) != 0) return 0;
        skip_space(a); if (*a->p == ',') a->p++;
        if (parse_operand(a, &s) != 0) return 0;

        if (d.kind == O_REG && s.kind == O_REG) {
            if (d.size != s.size) { fail(a, "mov between two widths", 0); return 0; }
            rex_sz(a, d.size, s.reg, d.reg);
            emit(a, (uint8_t)(d.size == 1 ? 0x88 : 0x89));
            modrm(a, 3, s.reg, d.reg);
            return 0;
        }
        if (d.kind == O_REG && s.kind == O_MEM) {
            rex_mem(a, d.size, d.reg, s.reg);
            emit(a, (uint8_t)(d.size == 1 ? 0x8A : 0x8B));
            mem_operand(a, d.reg, &s, 0);
            return 0;
        }
        if (d.kind == O_MEM && s.kind == O_REG) {
            rex_mem(a, s.size, s.reg, d.reg);
            emit(a, (uint8_t)(s.size == 1 ? 0x88 : 0x89));
            mem_operand(a, s.reg, &d, 0);
            return 0;
        }
        if (d.kind == O_REG && s.kind == O_IMM && d.size == 1) {
            rex_sz(a, 1, -1, d.reg);
            emit(a, (uint8_t)(0xB0 + (d.reg & 7)));
            emit(a, (uint8_t)s.imm);
            return 0;
        }
        if (d.kind == O_MEM && s.kind == O_IMM) {
            /* `mov byte [rdi], 10` -- the bracket says nothing about width, so
             * the size keyword is the only thing that can. */
            if (d.size != 1 && d.size != 8) { fail(a, "say byte or qword for a store of a constant", 0); return 0; }
            rex_mem(a, d.size, -1, d.reg);
            emit(a, (uint8_t)(d.size == 1 ? 0xC6 : 0xC7));
            mem_operand(a, 0, &d, d.size == 1 ? 1 : 4);
            if (d.size == 1) emit(a, (uint8_t)s.imm);
            else             emit32(a, (uint32_t)(int32_t)s.imm);
            return 0;
        }
        if (d.kind == O_REG && s.kind == O_IMM && d.size == 8 && fits32(s.imm)) {
            rex(a, 1, 0, 0, d.reg);
            emit(a, 0xC7);
            modrm(a, 3, 0, d.reg);
            emit32(a, (uint32_t)(int32_t)s.imm);
            return 0;
        }
        if (d.kind == O_REG && s.kind == O_IMM && d.size == 4) {
            if (d.reg >= 8) emit(a, 0x41);
            emit(a, (uint8_t)(0xB8 + (d.reg & 7)));
            emit32(a, (uint32_t)s.imm);
            return 0;
        }
        if (d.kind == O_REG && (s.kind == O_IMM || s.kind == O_LABELIMM)) {
            /* movabs. A label's value is not known on the first pass, so this
             * form is chosen by what the operand *is*, never by how big it
             * turned out to be -- otherwise the two passes would disagree
             * about the length and every later address would be wrong. */
            rex(a, 1, 0, 0, d.reg);
            emit(a, (uint8_t)(0xB8 + (d.reg & 7)));
            emit_sym64(a, s.kind == O_IMM ? 0 : s.label, R_X86_64_64, 0,
                       (uint64_t)(s.kind == O_IMM ? s.imm : value_of(a, s.label)));
            return 0;
        }
        fail(a, "unsupported mov", 0);
        return 0;
    }

    for (unsigned i = 0; i < sizeof alus / sizeof alus[0]; i++) {
        if (strcmp(op, alus[i].name)) continue;
        if (parse_operand(a, &d) != 0) return 0;
        skip_space(a); if (*a->p == ',') a->p++;
        if (parse_operand(a, &s) != 0) return 0;
        if (d.kind == O_REG && s.kind == O_REG) {
            if (d.size != s.size) { fail(a, "two widths in one operation", op); return 0; }
            rex_sz(a, d.size, s.reg, d.reg);
            /* The byte form of each of these is the opcode one lower. */
            emit(a, (uint8_t)(d.size == 1 ? alus[i].op_rm_r - 1 : alus[i].op_rm_r));
            modrm(a, 3, s.reg, d.reg);
            return 0;
        }
        if (d.kind == O_REG && s.kind == O_MEM) {
            rex_mem(a, d.size, d.reg, s.reg);
            emit(a, (uint8_t)(d.size == 1 ? alus[i].op_rm_r + 1 : alus[i].op_rm_r + 2));
            mem_operand(a, d.reg, &s, 0);
            return 0;
        }
        if (d.kind == O_MEM && s.kind == O_REG) {
            rex_mem(a, s.size, s.reg, d.reg);
            emit(a, (uint8_t)(s.size == 1 ? alus[i].op_rm_r - 1 : alus[i].op_rm_r));
            mem_operand(a, s.reg, &d, 0);
            return 0;
        }
        if (d.kind == O_REG && (s.kind == O_IMM || s.kind == O_LABELIMM)) {
            int64_t v = (s.kind == O_IMM) ? s.imm : value_of(a, s.label);
            rex_sz(a, d.size, -1, d.reg);
            emit(a, (uint8_t)(d.size == 1 ? 0x80 : 0x81));
            modrm(a, 3, alus[i].digit, d.reg);
            if (d.size == 1) {
                if (a->obj && s.kind == O_LABELIMM) {
                    fail(a, "a byte immediate has no room for an address", op);
                    return 0;
                }
                emit(a, (uint8_t)v);
            } else {
                emit_sym32(a, s.kind == O_IMM ? 0 : s.label, R_X86_64_32S, 0,
                           (uint32_t)(int32_t)v);
            }
            return 0;
        }
        fail(a, "unsupported operands for", op);
        return 0;
    }

    return -1;                                     /* not a mnemonic we know */
}

/* ---- data directives ----------------------------------------------------- */
static void directive_data(AS *a, int width) {
    for (;;) {
        skip_space(a);
        if (*a->p == '"' || *a->p == '\'') {
            char q = *a->p++;
            while (*a->p && *a->p != q) {
                char c = *a->p++;
                if (c == '\\' && *a->p) {          /* \n \t \0 \\ \" */
                    char e = *a->p++;
                    c = e == 'n' ? '\n' : e == 't' ? '\t' : e == '0' ? '\0' :
                        e == 'r' ? '\r' : e;
                }
                emit(a, (uint8_t)c);
                for (int k = 1; k < width; k++) emit(a, 0);
            }
            if (*a->p == q) a->p++;
        } else {
            char t[32];
            if (!word(a, t, sizeof t)) { fail(a, "expected a value", 0); return; }
            int isnum;
            parse_number(a, t, &isnum);
            if (a->obj && !isnum && width == 8) {
                emit_sym64(a, t, R_X86_64_64, 0, 0);
            } else if (a->obj && !isnum && width == 4) {
                emit_sym32(a, t, R_X86_64_32, 0, 0);
            } else if (a->obj && !isnum) {
                fail(a, "an address does not fit in db/dw; use dd or dq for", t);
                return;
            } else {
                int64_t v = value_of(a, t);
                for (int k = 0; k < width; k++) emit(a, (uint8_t)(v >> (k * 8)));
            }
        }
        skip_space(a);
        if (*a->p != ',') return;
        a->p++;
    }
}

/* ---- one line ------------------------------------------------------------ */
static void do_line(AS *a) {
    skip_space(a);
    if (is_eol(*a->p)) return;

    char t[32];
    if (!word(a, t, sizeof t)) { fail(a, "cannot parse", 0); return; }

    /* A label: either `name:` or a bare name at the start of a line. */
    skip_space(a);
    if (*a->p == ':') {
        a->p++;
        sym_define(a, t, here(a));
        skip_space(a);
        if (is_eol(*a->p)) return;
        if (!word(a, t, sizeof t)) return;
        skip_space(a);
    }

    char lo[32];
    strlcpy(lo, t, sizeof lo);
    lower(lo);

    if (!strcmp(lo, "bits") || !strcmp(lo, "default")) return;     /* nasm noise */

    if (!strcmp(lo, "section") || !strcmp(lo, "segment")) {
        char s[32];
        word(a, s, sizeof s);
        lower(s);
        if (!strcmp(s, ".data") || !strcmp(s, ".bss") || !strcmp(s, ".rodata"))
            a->in_data = 1;
        else if (!strcmp(s, ".text"))
            a->in_data = 0;
        else
            fail(a, "unknown section", s);
        return;
    }
    if (!strcmp(lo, "global") || !strcmp(lo, "extern")) {
        char s[32];
        if (!word(a, s, sizeof s)) return;
        if (lo[0] == 'g') {
            Sym *g = sym_get(a, s);
            if (g) g->global = 1;
            /* A file may make several names global now that it can be linked
             * with others, so the entry point is the first of them rather than
             * the last one seen. An object has no entry point at all. */
            if (!a->obj && !a->saw_global) {
                strlcpy(a->entry, s, sizeof a->entry);
                a->saw_global = 1;
            }
        } else {
            sym_get(a, s);        /* undefined on purpose: the linker finds it */
        }
        return;
    }
    if (!strcmp(lo, "db")) { directive_data(a, 1); return; }
    if (!strcmp(lo, "dw")) { directive_data(a, 2); return; }
    if (!strcmp(lo, "dd")) { directive_data(a, 4); return; }
    if (!strcmp(lo, "dq")) { directive_data(a, 8); return; }
    if (!strcmp(lo, "resb")) {
        char s[32];
        if (!word(a, s, sizeof s)) { fail(a, "resb needs a count", 0); return; }
        int64_t n = value_const(a, s);
        for (int64_t i = 0; i < n; i++) emit(a, 0);
        return;
    }
    if (!strcmp(lo, "equ")) { fail(a, "equ needs a name in front of it", 0); return; }

    /* `name equ value` -- the name was already taken as a label above, so it
     * only reaches here when there was no colon. */
    skip_space(a);
    if (a->p[0] == 'e' && a->p[1] == 'q' && a->p[2] == 'u' && is_space(a->p[3])) {
        a->p += 3;
        char s[32];
        if (!word(a, s, sizeof s)) { fail(a, "equ needs a value", 0); return; }
        int64_t v = value_const(a, s);
        Sym *old = sym_get(a, t);
        if (old) { old->value = (uint64_t)v; old->defined = 1; old->sect = S_ABS; }
        return;
    }

    if (encode(a, lo) != 0) fail(a, "unknown instruction", lo);
}

static void run_pass(AS *a, int pass) {
    a->pass = pass;
    a->p = a->src;
    a->line = 1;
    a->in_data = 0;
    a->tlen = a->dlen = 0;
    while (*a->p && !a->failed) {
        const char *eol = a->p;
        while (*eol && *eol != '\n') eol++;
        /* do_line reads to the end of the line through a->p; the loop below
         * puts the cursor on the next one whatever it stopped at. */
        do_line(a);
        a->p = eol;
        if (*a->p == '\n') { a->p++; a->line++; }
    }
}

/* ---- ELF ----------------------------------------------------------------- */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8)); }

/* A growing byte buffer for the tables that only exist at the end. Small and
 * local on purpose -- the section contents are built once and copied once. */
typedef struct { uint8_t *b; uint32_t len, cap; } Buf;

static int bput(Buf *o, const void *p, uint32_t n) {
    if (o->len + n > o->cap) {
        uint32_t nc = o->cap ? o->cap * 2 : 256;
        while (nc < o->len + n) nc *= 2;
        uint8_t *nb = (uint8_t *)kmalloc(nc);
        if (!nb) return -1;
        if (o->b) { memcpy(nb, o->b, o->len); kfree(o->b); }
        o->b = nb; o->cap = nc;
    }
    if (p) memcpy(o->b + o->len, p, n);
    else   memset(o->b + o->len, 0, n);
    o->len += n;
    return 0;
}
static uint32_t bstr(Buf *o, const char *s) {
    uint32_t at = o->len;
    if (bput(o, s, (uint32_t)strlen(s) + 1) != 0) return 0xFFFFFFFFu;
    return at;
}

#define SEC_TEXT 1
#define SEC_DATA 2

/* The symbol table both outputs carry: locals first, then globals, because
 * sh_info has to name the boundary between them and every reader believes it.
 * `idx` maps an assembler symbol to its index here, which is what the
 * relocations refer to. Returns 0 on success. */
static int build_symtab(AS *a, Buf *sym, Buf *str, int *idx, uint32_t *first_global) {
    uint8_t zero[24];
    memset(zero, 0, sizeof zero);
    if (bput(str, "", 1) != 0) return -1;       /* index 0 is the empty name */
    if (bput(sym, zero, sizeof zero) != 0) return -1;

    for (int i = 0; i < a->nsym; i++) idx[i] = 0;

    uint32_t n = 1;
    for (int round = 0; round < 2; round++) {
        if (round == 1) *first_global = n;
        for (int i = 0; i < a->nsym; i++) {
            Sym *s = &a->syms[i];
            int is_global = s->global || !s->defined;
            if ((round == 0) == (is_global != 0)) continue;
            if (!a->obj && !s->defined) continue;   /* a program has none of these */

            uint32_t noff = bstr(str, s->name);
            if (noff == 0xFFFFFFFFu) return -1;

            int type = STT_NOTYPE;
            if (s->global && s->sect == S_TEXT) type = STT_FUNC;
            else if (s->global && s->sect == S_DATA) type = STT_OBJECT;

            uint16_t shndx = SHN_UNDEF;
            if (s->sect == S_TEXT) shndx = SEC_TEXT;
            else if (s->sect == S_DATA) shndx = SEC_DATA;
            else if (s->sect == S_ABS) shndx = SHN_ABS;

            uint8_t e[24];
            memset(e, 0, sizeof e);
            put32(e + 0, noff);
            e[4] = ELF_ST_INFO(is_global ? STB_GLOBAL : STB_LOCAL, type);
            put16(e + 6, shndx);
            put64(e + 8, s->defined ? s->value : 0);
            if (bput(sym, e, sizeof e) != 0) return -1;
            idx[i] = (int)n++;
        }
    }
    if (*first_global == 0) *first_global = n;
    return 0;
}

static void free_buf(Buf *o) { if (o->b) kfree(o->b); o->b = 0; o->len = o->cap = 0; }

/* ---- relocatable object -------------------------------------------------- */
/* Eight sections, always all eight even when a relocation table comes out
 * empty. A reader that has to cope with a missing .rela.data is a reader with
 * one more branch in it for no reason.
 *
 *   0 null   1 .text   2 .data   3 .rela.text   4 .rela.data
 *   5 .symtab   6 .strtab   7 .shstrtab
 */
static uint8_t *write_obj(AS *a, uint32_t *len) {
    Buf sym = {0,0,0}, str = {0,0,0}, rt = {0,0,0}, rd = {0,0,0}, shs = {0,0,0};
    uint8_t *out = 0;
    uint32_t first_global = 0;
    int *idx = (int *)kmalloc((uint32_t)(a->nsym ? a->nsym : 1) * sizeof(int));
    if (!idx) { fail(a, "out of memory building the symbol table", 0); return 0; }

    if (build_symtab(a, &sym, &str, idx, &first_global) != 0) {
        fail(a, "out of memory building the symbol table", 0);
        goto done;
    }

    for (int i = 0; i < a->nrel; i++) {
        Reloc *r = &a->relocs[i];
        uint8_t e[24];
        put64(e + 0, r->off);
        put64(e + 8, ELF64_R_INFO((uint32_t)idx[r->sym], (uint32_t)r->type));
        put64(e + 16, (uint64_t)r->addend);
        if (bput(r->sect ? &rd : &rt, e, sizeof e) != 0) {
            fail(a, "out of memory writing relocations", 0);
            goto done;
        }
    }

    uint32_t n_null = bstr(&shs, "");
    uint32_t n_text = bstr(&shs, ".text");
    uint32_t n_data = bstr(&shs, ".data");
    uint32_t n_rt   = bstr(&shs, ".rela.text");
    uint32_t n_rd   = bstr(&shs, ".rela.data");
    uint32_t n_sym  = bstr(&shs, ".symtab");
    uint32_t n_str  = bstr(&shs, ".strtab");
    uint32_t n_shs  = bstr(&shs, ".shstrtab");
    if (n_shs == 0xFFFFFFFFu) { fail(a, "out of memory naming sections", 0); goto done; }

    #define ALIGN8(x) (((x) + 7u) & ~7u)
    uint32_t o_text = 64;
    uint32_t o_data = o_text + a->tlen;
    uint32_t o_rt   = ALIGN8(o_data + a->dlen);
    uint32_t o_rd   = o_rt + rt.len;
    uint32_t o_sym  = o_rd + rd.len;
    uint32_t o_str  = o_sym + sym.len;
    uint32_t o_shs  = o_str + str.len;
    uint32_t o_sh   = ALIGN8(o_shs + shs.len);
    uint32_t total  = o_sh + 8 * 64;
    #undef ALIGN8

    out = (uint8_t *)kmalloc(total);
    if (!out) { fail(a, "out of memory writing the object", 0); goto done; }
    memset(out, 0, total);

    uint8_t *h = out;
    h[0] = 0x7F; h[1] = 'E'; h[2] = 'L'; h[3] = 'F';
    h[4] = ELFCLASS64; h[5] = 1; h[6] = 1;
    put16(h + 16, ET_REL);
    put16(h + 18, EM_X86_64);
    put32(h + 20, 1);
    put64(h + 32, 0);                      /* no program headers in an object */
    put64(h + 40, o_sh);
    put16(h + 52, 64);
    put16(h + 54, 56);
    put16(h + 56, 0);
    put16(h + 58, 64);
    put16(h + 60, 8);
    put16(h + 62, 7);                      /* e_shstrndx */

    if (a->tlen) memcpy(out + o_text, a->text, a->tlen);
    if (a->dlen) memcpy(out + o_data, a->data, a->dlen);
    if (rt.len)  memcpy(out + o_rt,  rt.b,  rt.len);
    if (rd.len)  memcpy(out + o_rd,  rd.b,  rd.len);
    if (sym.len) memcpy(out + o_sym, sym.b, sym.len);
    if (str.len) memcpy(out + o_str, str.b, str.len);
    if (shs.len) memcpy(out + o_shs, shs.b, shs.len);

    struct { uint32_t name, type; uint64_t flags, addr, off, size;
             uint32_t link, info; uint64_t align, ent; } sh[8] = {
        { n_null, 0,           0,                            0, 0,      0,       0, 0, 0,  0 },
        { n_text, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,   0, o_text, a->tlen, 0, 0, 16, 0 },
        { n_data, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE,       0, o_data, a->dlen, 0, 0, 16, 0 },
        { n_rt,   SHT_RELA,     0,                           0, o_rt,   rt.len,  5, 1, 8,  24 },
        { n_rd,   SHT_RELA,     0,                           0, o_rd,   rd.len,  5, 2, 8,  24 },
        { n_sym,  SHT_SYMTAB,   0,                           0, o_sym,  sym.len, 6, first_global, 8, 24 },
        { n_str,  SHT_STRTAB,   0,                           0, o_str,  str.len, 0, 0, 1,  0 },
        { n_shs,  SHT_STRTAB,   0,                           0, o_shs,  shs.len, 0, 0, 1,  0 },
    };
    for (int i = 0; i < 8; i++) {
        uint8_t *p = out + o_sh + (uint32_t)i * 64;
        put32(p + 0,  sh[i].name);
        put32(p + 4,  sh[i].type);
        put64(p + 8,  sh[i].flags);
        put64(p + 16, sh[i].addr);
        put64(p + 24, sh[i].off);
        put64(p + 32, sh[i].size);
        put32(p + 40, sh[i].link);
        put32(p + 44, sh[i].info);
        put64(p + 48, sh[i].align);
        put64(p + 56, sh[i].ent);
    }
    *len = total;

done:
    kfree(idx);
    free_buf(&sym); free_buf(&str); free_buf(&rt); free_buf(&rd); free_buf(&shs);
    if (a->failed && out) { kfree(out); out = 0; }
    return out;
}

/* ---- program ------------------------------------------------------------- */
/* Two page-aligned segments, so the loader never has to map one page for two
 * of them, and a symbol table on the end. Nothing loads the symbols -- they
 * are there so a crash in this program comes back as names rather than as a
 * column of hex (see elf_symbolize). */
static uint8_t *write_exec(AS *a, uint32_t *len) {
    Sym *e = sym_find(a, a->entry);
    if (!e || !e->defined) {
        fail(a, "no entry point; expected a label called", a->entry);
        return 0;
    }

    Buf sym = {0,0,0}, str = {0,0,0}, shs = {0,0,0};
    uint8_t *out = 0;
    uint32_t first_global = 0;
    int *idx = (int *)kmalloc((uint32_t)(a->nsym ? a->nsym : 1) * sizeof(int));
    if (!idx) { fail(a, "out of memory building the symbol table", 0); return 0; }
    if (build_symtab(a, &sym, &str, idx, &first_global) != 0) {
        fail(a, "out of memory building the symbol table", 0);
        goto done;
    }

    uint32_t n_null = bstr(&shs, "");
    uint32_t n_text = bstr(&shs, ".text");
    uint32_t n_data = bstr(&shs, ".data");
    uint32_t n_sym  = bstr(&shs, ".symtab");
    uint32_t n_str  = bstr(&shs, ".strtab");
    uint32_t n_shs  = bstr(&shs, ".shstrtab");
    if (n_shs == 0xFFFFFFFFu) { fail(a, "out of memory naming sections", 0); goto done; }

    #define ALIGN8(x) (((x) + 7u) & ~7u)
    uint32_t tpad  = (a->tlen + PAGE - 1) & ~(PAGE - 1);
    uint32_t doff  = TEXT_OFF + tpad;
    uint32_t o_sym = ALIGN8(doff + a->dlen);
    uint32_t o_str = o_sym + sym.len;
    uint32_t o_shs = o_str + str.len;
    uint32_t o_sh  = ALIGN8(o_shs + shs.len);
    uint32_t total = o_sh + 6 * 64;
    #undef ALIGN8

    out = (uint8_t *)kmalloc(total);
    if (!out) { fail(a, "out of memory writing the image", 0); goto done; }
    memset(out, 0, total);

    uint8_t *h = out;
    h[0] = 0x7F; h[1] = 'E'; h[2] = 'L'; h[3] = 'F';
    h[4] = 2;                       /* 64-bit      */
    h[5] = 1;                       /* little end  */
    h[6] = 1;                       /* version     */
    h[16] = 2; h[17] = 0;           /* e_type    ET_EXEC   */
    h[18] = 0x3E; h[19] = 0;        /* e_machine EM_X86_64 */
    put32(h + 20, 1);               /* e_version   */
    put64(h + 24, e->value);        /* e_entry     */
    put64(h + 32, 64);              /* e_phoff     */
    put64(h + 40, o_sh);            /* e_shoff     */
    put32(h + 48, 0);               /* e_flags     */
    h[52] = 64; h[53] = 0;          /* e_ehsize    */
    h[54] = 56; h[55] = 0;          /* e_phentsize */
    h[56] = 2;  h[57] = 0;          /* e_phnum     */
    h[58] = 64; h[59] = 0;          /* e_shentsize */
    put16(h + 60, 6);               /* e_shnum     */
    put16(h + 62, 5);               /* e_shstrndx  */

    uint8_t *p1 = out + 64;
    put32(p1 + 0, 1);               /* PT_LOAD          */
    put32(p1 + 4, 5);               /* PF_R | PF_X      */
    put64(p1 + 8,  0);              /* p_offset         */
    put64(p1 + 16, ASM_LOAD_BASE);  /* p_vaddr          */
    put64(p1 + 24, ASM_LOAD_BASE);  /* p_paddr          */
    put64(p1 + 32, TEXT_OFF + a->tlen);
    put64(p1 + 40, TEXT_OFF + a->tlen);
    put64(p1 + 48, PAGE);

    uint8_t *p2 = p1 + 56;
    put32(p2 + 0, 1);
    put32(p2 + 4, 6);               /* PF_R | PF_W      */
    put64(p2 + 8,  doff);
    put64(p2 + 16, ASM_LOAD_BASE + doff);
    put64(p2 + 24, ASM_LOAD_BASE + doff);
    put64(p2 + 32, a->dlen);
    put64(p2 + 40, a->dlen);
    put64(p2 + 48, PAGE);

    if (a->tlen) memcpy(out + TEXT_OFF, a->text, a->tlen);
    if (a->dlen) memcpy(out + doff, a->data, a->dlen);
    if (sym.len) memcpy(out + o_sym, sym.b, sym.len);
    if (str.len) memcpy(out + o_str, str.b, str.len);
    if (shs.len) memcpy(out + o_shs, shs.b, shs.len);

    struct { uint32_t name, type; uint64_t flags, addr, off, size;
             uint32_t link, info; uint64_t align, ent; } sh[6] = {
        { n_null, 0,            0,                          0,                          0,        0,       0, 0, 0,  0 },
        { n_text, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,  ASM_LOAD_BASE + TEXT_OFF,   TEXT_OFF, a->tlen, 0, 0, 16, 0 },
        { n_data, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE,      ASM_LOAD_BASE + doff,       doff,     a->dlen, 0, 0, 16, 0 },
        { n_sym,  SHT_SYMTAB,   0,                          0,                          o_sym,    sym.len, 4, first_global, 8, 24 },
        { n_str,  SHT_STRTAB,   0,                          0,                          o_str,    str.len, 0, 0, 1,  0 },
        { n_shs,  SHT_STRTAB,   0,                          0,                          o_shs,    shs.len, 0, 0, 1,  0 },
    };
    for (int i = 0; i < 6; i++) {
        uint8_t *p = out + o_sh + (uint32_t)i * 64;
        put32(p + 0,  sh[i].name);
        put32(p + 4,  sh[i].type);
        put64(p + 8,  sh[i].flags);
        put64(p + 16, sh[i].addr);
        put64(p + 24, sh[i].off);
        put64(p + 32, sh[i].size);
        put32(p + 40, sh[i].link);
        put32(p + 44, sh[i].info);
        put64(p + 48, sh[i].align);
        put64(p + 56, sh[i].ent);
    }
    *len = total;

done:
    kfree(idx);
    free_buf(&sym); free_buf(&str); free_buf(&shs);
    if (a->failed && out) { kfree(out); out = 0; }
    return out;
}

static uint8_t *assemble(const char *src, int obj, uint32_t *len,
                         char *err, uint32_t errcap) {
    AS *a = (AS *)kmalloc(sizeof(AS));
    if (!a) { strlcpy(err, "out of memory", errcap); return 0; }
    memset(a, 0, sizeof *a);
    a->src = src;
    a->obj = obj;
    strlcpy(a->entry, "_start", sizeof a->entry);

    run_pass(a, P_SCAN);
    a->text_total = a->tlen;
    if (!a->failed) { run_pass(a, P_FIX); a->text_total = a->tlen; }
    if (!a->failed) run_pass(a, P_EMIT);

    uint8_t *out = 0;
    if (!a->failed) out = obj ? write_obj(a, len) : write_exec(a, len);

    if (a->failed) { strlcpy(err, a->err, errcap); if (out) { kfree(out); out = 0; } }
    if (a->text) kfree(a->text);
    if (a->data) kfree(a->data);
    if (a->relocs) kfree(a->relocs);
    kfree(a);
    return out;
}

uint8_t *asm_assemble(const char *src, uint32_t *len, char *err, uint32_t errcap) {
    return assemble(src, 0, len, err, errcap);
}

uint8_t *asm_assemble_obj(const char *src, uint32_t *len, char *err, uint32_t errcap) {
    return assemble(src, 1, len, err, errcap);
}
