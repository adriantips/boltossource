#include <stdint.h>
#include <stddef.h>
#include "asm.h"
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

#define MAX_SYMS   512
#define TEXT_OFF   0x1000
#define PAGE       0x1000

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

typedef struct { char name[32]; uint64_t value; int defined; } Sym;

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

static void sym_define(AS *a, const char *name, uint64_t value) {
    Sym *s = sym_find(a, name);
    if (s) {
        if (a->pass == P_SCAN) { fail(a, "duplicate label", name); return; }
        if (a->pass == P_FIX) { s->value = value; return; }
        /* The last pass re-defines every label and must land on the address the
         * one before it settled on. If it does not, the two disagreed about how
         * long some instruction is, and every reference past this point is
         * wrong -- worth stopping for rather than emitting. */
        if (s->value != value) { fail(a, "internal: passes disagree at", name); return; }
        return;
    }
    if (a->nsym >= MAX_SYMS) { fail(a, "too many labels", 0); return; }
    s = &a->syms[a->nsym++];
    strlcpy(s->name, name, sizeof s->name);
    s->value = value;
    s->defined = 1;
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
/* Index is the encoding; the 64-bit and 32-bit spellings share it. */
static const char *r64[16] = { "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                               "r8","r9","r10","r11","r12","r13","r14","r15" };
static const char *r32[16] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi",
                               "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d" };

/* -1 if it is not a register; *wide says 64 or 32 bit. */
static int reg_of(const char *s, int *wide) {
    for (int i = 0; i < 16; i++) if (strcmp(s, r64[i]) == 0) { if (wide) *wide = 1; return i; }
    for (int i = 0; i < 16; i++) if (strcmp(s, r32[i]) == 0) { if (wide) *wide = 0; return i; }
    return -1;
}

/* ---- operands ------------------------------------------------------------ */
enum { O_NONE, O_REG, O_IMM, O_MEM, O_LABELIMM };

typedef struct {
    int kind;
    int reg;           /* O_REG, or the base of O_MEM (-1 = RIP-relative) */
    int wide;          /* 64-bit operand                                   */
    int64_t imm;       /* O_IMM, or the displacement of O_MEM              */
    char label[32];    /* O_LABELIMM, or an O_MEM whose base is a label    */
} Opnd;

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
    if (a->pass == P_EMIT) fail(a, "undefined symbol", t);
    return 0;
}

static int parse_operand(AS *a, Opnd *o) {
    memset(o, 0, sizeof *o);
    skip_space(a);
    if (is_eol(*a->p)) { o->kind = O_NONE; return 0; }

    if (*a->p == '[') {                       /* [reg], [reg+disp], [label] */
        a->p++;
        char t[32];
        if (!word(a, t, sizeof t)) { fail(a, "empty [ ]", 0); return -1; }
        lower(t);
        int wide, r = reg_of(t, &wide);
        o->kind = O_MEM;
        if (r >= 0) {
            o->reg = r;
            skip_space(a);
            if (*a->p == '+' || *a->p == '-') {
                int neg = (*a->p == '-');
                a->p++;
                char d[32];
                if (!word(a, d, sizeof d)) { fail(a, "expected a displacement", 0); return -1; }
                int64_t dv = value_of(a, d);
                o->imm = neg ? -dv : dv;
            }
        } else {
            /* [label] -- RIP-relative, which is how a flat, non-relocatable
             * program reaches its own data without a fixup table. */
            o->reg = -1;
            strlcpy(o->label, t, sizeof o->label);
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
    int wide, r = reg_of(t, &wide);
    if (r >= 0) { o->kind = O_REG; o->reg = r; o->wide = wide; return 0; }

    int ok;
    int64_t v = parse_number(a, raw, &ok);
    if (ok) { o->kind = O_IMM; o->imm = v; return 0; }

    o->kind = O_LABELIMM;
    strlcpy(o->label, raw, sizeof o->label);
    return 0;
}

/* ---- encoding ------------------------------------------------------------ */
static uint64_t text_va(AS *a) { (void)a; return ASM_LOAD_BASE + TEXT_OFF; }
static uint64_t data_va(AS *a) {
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

static void rex(AS *a, int w, int r, int x, int b) {
    uint8_t v = (uint8_t)(0x40 | (w ? 8 : 0) | ((r >> 3) & 1) << 2 |
                          ((x >> 3) & 1) << 1 | ((b >> 3) & 1));
    if (v != 0x40 || w) emit(a, v);
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
        emit32(a, (uint32_t)(int32_t)(target - next));
        return;
    }
    int base = m->reg;
    int need_disp = (m->imm != 0) || ((base & 7) == 5);   /* [rbp] has no mod 0 */
    int mod = need_disp ? 2 : 0;
    if ((base & 7) == 4) {                          /* rsp/r12 need a SIB */
        modrm(a, mod, reg, 4);
        emit(a, (uint8_t)(0x24));                   /* scale 0, no index, base */
    } else {
        modrm(a, mod, reg, base);
    }
    if (need_disp) emit32(a, (uint32_t)(int32_t)m->imm);
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
        emit32(a, (uint32_t)(int32_t)(target - (int64_t)(here(a) + 4)));
        return 0;
    }

    for (unsigned i = 0; i < sizeof jccs / sizeof jccs[0]; i++) {
        if (strcmp(op, jccs[i].name)) continue;
        if (parse_operand(a, &d) != 0) return 0;
        emit(a, 0x0F);
        emit(a, (uint8_t)(0x80 | jccs[i].cc));
        int64_t target = (d.kind == O_IMM) ? d.imm : (int64_t)value_of(a, d.label);
        emit32(a, (uint32_t)(int32_t)(target - (int64_t)(here(a) + 4)));
        return 0;
    }

    if (!strcmp(op, "inc") || !strcmp(op, "dec") ||
        !strcmp(op, "neg") || !strcmp(op, "not")) {
        if (parse_operand(a, &d) != 0) return 0;
        if (d.kind != O_REG) { fail(a, "wants a register", op); return 0; }
        /* inc/dec live in the FF group, neg/not in F7, and they disagree about
         * which /digit means what. */
        uint8_t opcode = (!strcmp(op, "inc") || !strcmp(op, "dec")) ? 0xFF : 0xF7;
        int digit = !strcmp(op, "inc") ? 0 : !strcmp(op, "dec") ? 1 :
                    !strcmp(op, "not") ? 2 : 3;
        rex(a, d.wide, 0, 0, d.reg);
        emit(a, opcode);
        modrm(a, 3, digit, d.reg);
        return 0;
    }

    if (!strcmp(op, "imul")) {
        if (parse_operand(a, &d) != 0) return 0;
        skip_space(a); if (*a->p == ',') a->p++;
        if (parse_operand(a, &s) != 0) return 0;
        if (d.kind != O_REG || s.kind != O_REG) { fail(a, "imul wants two registers", 0); return 0; }
        rex(a, d.wide, d.reg, 0, s.reg);
        emit(a, 0x0F); emit(a, 0xAF);
        modrm(a, 3, d.reg, s.reg);
        return 0;
    }

    if (!strcmp(op, "shl") || !strcmp(op, "shr") || !strcmp(op, "sar")) {
        if (parse_operand(a, &d) != 0) return 0;
        skip_space(a); if (*a->p == ',') a->p++;
        if (parse_operand(a, &s) != 0) return 0;
        if (d.kind != O_REG || s.kind != O_IMM) { fail(a, "wants a register and a count", op); return 0; }
        rex(a, d.wide, 0, 0, d.reg);
        emit(a, 0xC1);
        modrm(a, 3, !strcmp(op, "shl") ? 4 : !strcmp(op, "shr") ? 5 : 7, d.reg);
        emit(a, (uint8_t)s.imm);
        return 0;
    }

    if (!strcmp(op, "test")) {
        if (parse_operand(a, &d) != 0) return 0;
        skip_space(a); if (*a->p == ',') a->p++;
        if (parse_operand(a, &s) != 0) return 0;
        if (d.kind != O_REG || s.kind != O_REG) { fail(a, "test wants two registers", 0); return 0; }
        rex(a, d.wide, s.reg, 0, d.reg);
        emit(a, 0x85);
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
            rex(a, d.wide, s.reg, 0, d.reg);
            emit(a, 0x89);
            modrm(a, 3, s.reg, d.reg);
            return 0;
        }
        if (d.kind == O_REG && s.kind == O_MEM) {
            rex(a, d.wide, d.reg, 0, s.reg < 0 ? 0 : s.reg);
            emit(a, 0x8B);
            mem_operand(a, d.reg, &s, 0);
            return 0;
        }
        if (d.kind == O_MEM && s.kind == O_REG) {
            rex(a, s.wide, s.reg, 0, d.reg < 0 ? 0 : d.reg);
            emit(a, 0x89);
            mem_operand(a, s.reg, &d, 0);
            return 0;
        }
        if (d.kind == O_REG && s.kind == O_IMM && d.wide && fits32(s.imm)) {
            rex(a, 1, 0, 0, d.reg);
            emit(a, 0xC7);
            modrm(a, 3, 0, d.reg);
            emit32(a, (uint32_t)(int32_t)s.imm);
            return 0;
        }
        if (d.kind == O_REG && s.kind == O_IMM && !d.wide) {
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
            emit64(a, (uint64_t)(s.kind == O_IMM ? s.imm : value_of(a, s.label)));
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
            rex(a, d.wide, s.reg, 0, d.reg);
            emit(a, alus[i].op_rm_r);
            modrm(a, 3, s.reg, d.reg);
            return 0;
        }
        if (d.kind == O_REG && (s.kind == O_IMM || s.kind == O_LABELIMM)) {
            int64_t v = (s.kind == O_IMM) ? s.imm : value_of(a, s.label);
            rex(a, d.wide, 0, 0, d.reg);
            emit(a, 0x81);
            modrm(a, 3, alus[i].digit, d.reg);
            emit32(a, (uint32_t)(int32_t)v);
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
            int64_t v = value_of(a, t);
            for (int k = 0; k < width; k++) emit(a, (uint8_t)(v >> (k * 8)));
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
        if (word(a, s, sizeof s) && lo[0] == 'g') strlcpy(a->entry, s, sizeof a->entry);
        return;
    }
    if (!strcmp(lo, "db")) { directive_data(a, 1); return; }
    if (!strcmp(lo, "dw")) { directive_data(a, 2); return; }
    if (!strcmp(lo, "dd")) { directive_data(a, 4); return; }
    if (!strcmp(lo, "dq")) { directive_data(a, 8); return; }
    if (!strcmp(lo, "resb")) {
        char s[32];
        if (!word(a, s, sizeof s)) { fail(a, "resb needs a count", 0); return; }
        int64_t n = value_of(a, s);
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
        Sym *old = sym_find(a, t);
        if (old) old->value = (uint64_t)value_of(a, s);
        else sym_define(a, t, (uint64_t)value_of(a, s));
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
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8)); }

uint8_t *asm_assemble(const char *src, uint32_t *len, char *err, uint32_t errcap) {
    AS *a = (AS *)kmalloc(sizeof(AS));
    if (!a) { strlcpy(err, "out of memory", errcap); return 0; }
    memset(a, 0, sizeof *a);
    a->src = src;
    strlcpy(a->entry, "_start", sizeof a->entry);

    run_pass(a, P_SCAN);
    a->text_total = a->tlen;
    if (!a->failed) { run_pass(a, P_FIX); a->text_total = a->tlen; }
    if (!a->failed) run_pass(a, P_EMIT);

    uint8_t *out = 0;
    if (!a->failed) {
        Sym *e = sym_find(a, a->entry);
        if (!e) fail(a, "no entry point; expected a label called", a->entry);
        else {
            uint32_t tpad  = (a->tlen + PAGE - 1) & ~(PAGE - 1);
            uint32_t doff  = TEXT_OFF + tpad;
            uint32_t total = doff + a->dlen;
            out = (uint8_t *)kmalloc(total ? total : 1);
            if (!out) fail(a, "out of memory writing the image", 0);
            else {
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
                put64(h + 40, 0);               /* e_shoff     */
                put32(h + 48, 0);               /* e_flags     */
                h[52] = 64; h[53] = 0;          /* e_ehsize    */
                h[54] = 56; h[55] = 0;          /* e_phentsize */
                h[56] = 2;  h[57] = 0;          /* e_phnum     */
                h[58] = 64; h[59] = 0;          /* e_shentsize */

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
                *len = total;
            }
        }
    }

    if (a->failed) { strlcpy(err, a->err, errcap); if (out) { kfree(out); out = 0; } }
    if (a->text) kfree(a->text);
    if (a->data) kfree(a->data);
    kfree(a);
    return out;
}
