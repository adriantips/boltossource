#include <stdint.h>
#include "ccir.h"
#include "kheap.h"
#include "string.h"

/* ===========================================================================
 *  BoltCC's second backend: the same bytecode, as x86-64 instead of as
 *  something the VM steps through.
 *
 *  It emits NASM text rather than machine code, and `as` assembles it. That is
 *  a slower route than encoding directly and it is the right one twice over:
 *  the assembler is a program on this machine that now has a real consumer
 *  rather than a demonstration, and `cc -S` falls out for free -- when native
 *  code does the wrong thing you can read what it was asked to do, which is
 *  the difference between a backend you can fix and one you can only distrust.
 *
 *  The translation is deliberately literal. BoltCC's bytecode is a stack
 *  machine, so the machine stack *is* the operand stack: every instruction
 *  pops its operands off it, pushes its result back, and leaves nothing in a
 *  register between instructions. Nothing is kept in a register across an
 *  opcode boundary, so there is no register allocator, no liveness, and no
 *  class of bug where two values believe they own rcx. The code is perhaps
 *  four times longer than it needs to be, and it is code whose correctness you
 *  can check by reading one opcode's worth at a time.
 *
 *  A frame is the ordinary one: rbp saved, locals below it at [rbp-8*(n+1)],
 *  arguments read out of the caller's pushes at [rbp+16] and up. The caller
 *  drops the arguments after the call, because the callee copied them into its
 *  own slots and the operand stack has to come back to where it was.
 *
 *  ---- what this does not compile ------------------------------------------
 *
 *  Every value here is a plain 64-bit integer. The VM's values carry a tag and
 *  this backend has nowhere to put one, so everything the tag exists for --
 *  strings as values, arrays, structs, the builtin library -- is refused by
 *  name in check_supported() rather than compiled into something that would
 *  run and be wrong. String *literals* work, as pointers into .data, because a
 *  literal handed to printf or puts never needs to be told apart from an
 *  integer at runtime: the format string already says which it is.
 *
 *  That is a real subset and it is the honest one. `cc` without -N still runs
 *  the whole language on the VM, and says so when -N cannot take a program.
 * ===========================================================================*/

#define MAX_STRS 256

typedef struct {
    char    *buf;
    uint32_t len, cap;

    const CCProgram *p;
    uint8_t *is_target;        /* pc -> something jumps here, so it needs a label */

    const char *strs[MAX_STRS]; int nstr;

    char err[128];
    int  failed;
} NE;

/* BoltRT links this file into a Windows .exe alongside only libc/string.c,
 * which has memcpy and strcmp but not the BSD pair. One append that truncates
 * is all this file wanted from them anyway. */
static void sapp(char *d, const char *s, uint32_t cap) {
    uint32_t n = 0;
    while (n + 1 < cap && d[n]) n++;
    while (n + 1 < cap && *s) d[n++] = *s++;
    if (cap) d[n] = 0;
}

static void ne_fail(NE *e, const char *msg, const char *what) {
    if (e->failed) return;
    e->failed = 1;
    e->err[0] = 0;
    sapp(e->err, msg, sizeof e->err);
    if (what && what[0]) {
        sapp(e->err, ": ", sizeof e->err);
        sapp(e->err, what, sizeof e->err);
    }
}

/* ---- the output buffer --------------------------------------------------- */
static void put(NE *e, const char *s) {
    if (e->failed) return;
    uint32_t n = (uint32_t)strlen(s);
    if (e->len + n + 1 > e->cap) {
        uint32_t nc = e->cap ? e->cap * 2 : 8192;
        while (nc < e->len + n + 1) nc *= 2;
        char *nb = (char *)kmalloc(nc);
        if (!nb) { ne_fail(e, "out of memory generating code", 0); return; }
        if (e->buf) { memcpy(nb, e->buf, e->len); kfree(e->buf); }
        e->buf = nb; e->cap = nc;
    }
    memcpy(e->buf + e->len, s, n);
    e->len += n;
    e->buf[e->len] = 0;
}

static void putl(NE *e, long v) {
    char t[24]; int n = 0;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-(v + 1)) + 1u : (unsigned long)v;
    char d[24]; int k = 0;
    if (!u) d[k++] = '0';
    while (u) { d[k++] = (char)('0' + u % 10); u /= 10; }
    if (neg) t[n++] = '-';
    while (k) t[n++] = d[--k];
    t[n] = 0;
    put(e, t);
}

/* `put` three times is most of this file, so these keep the emission itself
 * readable rather than surrounding every instruction with punctuation. */
static void op0(NE *e, const char *m)                        { put(e, "    "); put(e, m); put(e, "\n"); }
static void op1(NE *e, const char *m, const char *a)         { put(e, "    "); put(e, m); put(e, " "); put(e, a); put(e, "\n"); }
static void op2(NE *e, const char *m, const char *a, const char *b) {
    put(e, "    "); put(e, m); put(e, " "); put(e, a); put(e, ", "); put(e, b); put(e, "\n");
}
static void op2n(NE *e, const char *m, const char *a, long b) {
    put(e, "    "); put(e, m); put(e, " "); put(e, a); put(e, ", "); putl(e, b); put(e, "\n");
}
/* `mov rax, [rbp-24]` and friends: one register, one frame slot. */
static void op2slot(NE *e, const char *m, const char *a, const char *base, long off) {
    put(e, "    "); put(e, m); put(e, " "); put(e, a); put(e, ", [");
    put(e, base);
    if (off >= 0) put(e, "+");
    putl(e, off);
    put(e, "]\n");
}
static void op2slotd(NE *e, const char *m, const char *base, long off, const char *b) {
    put(e, "    "); put(e, m); put(e, " ["); put(e, base);
    if (off >= 0) put(e, "+");
    putl(e, off);
    put(e, "], "); put(e, b); put(e, "\n");
}
static void label_pc(NE *e, const char *pfx, long pc, const char *tail) {
    put(e, pfx); putl(e, pc); put(e, tail);
}

/* ---- what the backend will and will not take ----------------------------- */
/* Refusing by name matters more than refusing. "cannot compile natively" tells
 * you to give up; "arrays" tells you what to take out, or that -N is simply
 * not for this program yet. */
static const char *unsupported_op(int op) {
    switch (op) {
    case OP_BUILTIN:       return "the builtin library (len, str, abs, min, ...)";
    case OP_INDEX:         return "indexing a string or an array";
    case OP_NEWARR:
    case OP_ASTORE:        return "arrays";
    case OP_NEWSTRUCT:
    case OP_NEWSTRUCTARR:
    case OP_FIELD:
    case OP_FIELDSET:
    case OP_REF:           return "structs";
    case OP_CSPRINT:       return "Console.Write (the C# front end)";
    default:               return 0;
    }
}

/* A string literal is a pointer here and an integer everywhere else, and
 * nothing at runtime can tell the two apart. That is fine for a literal handed
 * straight to printf or puts, where the format says what it is, and wrong for
 * one that gets stored, concatenated, indexed or returned -- so those are
 * refused. Scanning forward linearly misses paths a branch could take; it
 * catches every way a program actually writes these, and the cost of the miss
 * is an error at `as` time rather than a wrong answer at run time. */
static int string_escapes(const CCProgram *p, int at) {
    for (int i = at + 1; i < p->ncode; i++) {
        switch (p->code[i].op) {
        case OP_PRINT: case OP_PRINTLN: case OP_PRINTF:
            return 0;                       /* consumed by something that prints */
        case OP_STOREL: case OP_STOREG: case OP_ADD:
        case OP_INDEX:  case OP_RET:
            return 1;
        default: break;
        }
    }
    return 1;
}

static void check_supported(NE *e) {
    const CCProgram *p = e->p;
    for (int i = 0; i < p->ncode && !e->failed; i++) {
        const char *why = unsupported_op(p->code[i].op);
        if (why) { ne_fail(e, "-N cannot compile", why); return; }

        if (p->code[i].op == OP_PUSHS && string_escapes(p, i)) {
            ne_fail(e, "-N cannot compile",
                    "a string used as a value; literals work as printf/puts arguments");
            return;
        }
        if ((p->code[i].op == OP_PRINT || p->code[i].op == OP_PRINTLN) &&
            (i == 0 || p->code[i - 1].op != OP_PUSHS)) {
            ne_fail(e, "-N cannot compile",
                    "printing something that is not a string literal");
            return;
        }
    }
}

/* ---- strings ------------------------------------------------------------- */
static int str_intern(NE *e, const char *s) {
    if (!s) s = "";
    for (int i = 0; i < e->nstr; i++)
        if (strcmp(e->strs[i], s) == 0) return i;
    if (e->nstr >= MAX_STRS) { ne_fail(e, "too many string literals", 0); return 0; }
    e->strs[e->nstr] = s;
    return e->nstr++;
}

/* db with every byte spelled as a number. Quoting would mean deciding what to
 * do about quotes, backslashes and newlines inside the literal, and the
 * assembler would then have to agree with that decision; numbers cannot be
 * misread. */
static void emit_string_data(NE *e, int idx) {
    const char *s = e->strs[idx];
    put(e, "s"); putl(e, idx); put(e, ": db ");
    for (const char *q = s; *q; q++) { putl(e, (unsigned char)*q); put(e, ", "); }
    put(e, "0\n");
}

/* ---- the runtime --------------------------------------------------------- */
/* Written out as text because that is what the backend emits: there is no
 * library to link against on this machine, and a program that prints a number
 * has to carry the code that turns one into digits. Every routine leaves rbx
 * and r12 alone, which is what lets __printf keep its two cursors across the
 * calls it makes. */
static const char *RUNTIME =
"; ---- runtime ------------------------------------------------------------\n"
"; __putc(rdi = byte)\n"
"__putc:\n"
"    push rdi\n"                     /* the low byte of the pushed word is it */
"    mov rax, 1\n"
"    mov rdi, 1\n"
"    mov rsi, rsp\n"
"    mov rdx, 1\n"
"    syscall\n"
"    pop rdi\n"
"    ret\n"
"\n"
"; __puts(rdi = pointer to NUL-terminated bytes)\n"
"__puts:\n"
"    mov rsi, rdi\n"
"    mov rcx, rdi\n"
"__puts_len:\n"
"    mov al, [rcx]\n"
"    cmp al, 0\n"
"    je __puts_out\n"
"    inc rcx\n"
"    jmp __puts_len\n"
"__puts_out:\n"
"    mov rdx, rcx\n"
"    sub rdx, rsi\n"
"    cmp rdx, 0\n"
"    je __puts_done\n"
"    mov rax, 1\n"
"    mov rdi, 1\n"
"    syscall\n"
"__puts_done:\n"
"    ret\n"
"\n"
"; __putu(rdi = value), digits built backwards into the scratch buffer\n"
"__putu:\n"
"    push rbx\n"
"    lea rbx, [__numend]\n"
"    mov rax, rdi\n"
"    mov rcx, 10\n"
"__putu_loop:\n"
"    xor rdx, rdx\n"
"    div rcx\n"
"    add rdx, 48\n"
"    dec rbx\n"
"    mov [rbx], dl\n"
"    cmp rax, 0\n"
"    jne __putu_loop\n"
"    mov rsi, rbx\n"
"    lea rdx, [__numend]\n"
"    sub rdx, rbx\n"
"    mov rax, 1\n"
"    mov rdi, 1\n"
"    syscall\n"
"    pop rbx\n"
"    ret\n"
"\n"
"; __puti(rdi = signed value)\n"
"__puti:\n"
"    mov rax, rdi\n"
"    cmp rax, 0\n"
"    jge __puti_pos\n"
"    push rax\n"
"    mov rdi, 45\n"
"    call __putc\n"
"    pop rax\n"
"    neg rax\n"
"__puti_pos:\n"
"    mov rdi, rax\n"
"    call __putu\n"
"    ret\n"
"\n"
"; __putx(rdi = value), lower-case hex\n"
"__putx:\n"
"    push rbx\n"
"    lea rbx, [__numend]\n"
"    mov rax, rdi\n"
"__putx_loop:\n"
"    mov rdx, rax\n"
"    and rdx, 15\n"
"    cmp rdx, 10\n"
"    jl __putx_dig\n"
"    add rdx, 87\n"
"    jmp __putx_put\n"
"__putx_dig:\n"
"    add rdx, 48\n"
"__putx_put:\n"
"    dec rbx\n"
"    mov [rbx], dl\n"
"    shr rax, 4\n"
"    cmp rax, 0\n"
"    jne __putx_loop\n"
"    mov rsi, rbx\n"
"    lea rdx, [__numend]\n"
"    sub rdx, rbx\n"
"    mov rax, 1\n"
"    mov rdi, 1\n"
"    syscall\n"
"    pop rbx\n"
"    ret\n"
"\n"
"; __printf(rdi = format, rsi = first vararg slot; each next one is 8 lower)\n"
"__printf:\n"
"    push rbx\n"
"    push r12\n"
"    mov rbx, rdi\n"
"    mov r12, rsi\n"
"__pf_loop:\n"
"    movzx rax, byte [rbx]\n"
"    cmp rax, 0\n"
"    je __pf_done\n"
"    cmp rax, 37\n"
"    je __pf_spec\n"
"    mov rdi, rax\n"
"    call __putc\n"
"    inc rbx\n"
"    jmp __pf_loop\n"
"__pf_spec:\n"
"    inc rbx\n"
"    movzx rax, byte [rbx]\n"
"    inc rbx\n"
"    cmp rax, 37\n"
"    je __pf_pct\n"
"    cmp rax, 100\n"
"    je __pf_d\n"
"    cmp rax, 105\n"
"    je __pf_d\n"
"    cmp rax, 117\n"
"    je __pf_u\n"
"    cmp rax, 120\n"
"    je __pf_x\n"
"    cmp rax, 99\n"
"    je __pf_c\n"
"    cmp rax, 115\n"
"    je __pf_s\n"
"    mov rdi, rax\n"                 /* an unknown conversion prints itself */
"    call __putc\n"
"    jmp __pf_loop\n"
"__pf_pct:\n"
"    mov rdi, 37\n"
"    call __putc\n"
"    jmp __pf_loop\n"
"__pf_d:\n"
"    mov rdi, [r12]\n"
"    sub r12, 8\n"
"    call __puti\n"
"    jmp __pf_loop\n"
"__pf_u:\n"
"    mov rdi, [r12]\n"
"    sub r12, 8\n"
"    call __putu\n"
"    jmp __pf_loop\n"
"__pf_x:\n"
"    mov rdi, [r12]\n"
"    sub r12, 8\n"
"    call __putx\n"
"    jmp __pf_loop\n"
"__pf_c:\n"
"    mov rdi, [r12]\n"
"    sub r12, 8\n"
"    call __putc\n"
"    jmp __pf_loop\n"
"__pf_s:\n"
"    mov rdi, [r12]\n"
"    sub r12, 8\n"
"    call __puts\n"
"    jmp __pf_loop\n"
"__pf_done:\n"
"    pop r12\n"
"    pop rbx\n"
"    ret\n";

/* ---- instruction emission ------------------------------------------------ */
/* Pop the two operands of a binary opcode: right into rcx, left into rax,
 * because the right one was pushed last. */
static void pop2(NE *e) { op1(e, "pop", "rcx"); op1(e, "pop", "rax"); }
static void push_rax(NE *e) { op1(e, "push", "rax"); }

/* cmp, then turn the flag into the 0 or 1 the bytecode expects. */
static void compare(NE *e, const char *setcc) {
    pop2(e);
    op2(e, "cmp", "rax", "rcx");
    op1(e, setcc, "al");
    op2(e, "movzx", "rax", "al");
    push_rax(e);
}

static const CCFunc *func_at(const CCProgram *p, int pc) {
    for (int i = 0; i < p->nfunc; i++)
        if (p->funcs[i].entry == pc) return &p->funcs[i];
    return 0;
}

static void emit_prologue(NE *e, const CCFunc *f) {
    put(e, "; ---- "); put(e, f->name ? f->name : "(function)"); put(e, "\n");
    op1(e, "push", "rbp");
    op2(e, "mov", "rbp", "rsp");
    if (f->nlocals > 0) op2n(e, "sub", "rsp", 8L * f->nlocals);
    /* Arguments were pushed left to right, so the last one sits nearest the
     * return address. Copy each into its slot, then zero the rest -- the VM
     * gave every frame zeroed locals and a program is allowed to notice. */
    for (int i = 0; i < f->nparams; i++) {
        op2slot(e, "mov", "rax", "rbp", 16L + 8L * (f->nparams - 1 - i));
        op2slotd(e, "mov", "rbp", -8L * (i + 1), "rax");
    }
    if (f->nlocals > f->nparams) {
        op2n(e, "mov", "rax", 0);
        for (int i = f->nparams; i < f->nlocals; i++)
            op2slotd(e, "mov", "rbp", -8L * (i + 1), "rax");
    }
}

static void emit_instr(NE *e, int pc) {
    const CCProgram *p = e->p;
    const Instr *in = &p->code[pc];

    switch (in->op) {
    case OP_PUSHI:  op2n(e, "mov", "rax", in->a); push_rax(e); break;
    case OP_PUSHS: {
        int idx = str_intern(e, in->s);
        put(e, "    lea rax, [s"); putl(e, idx); put(e, "]\n");
        push_rax(e);
        break; }
    case OP_LOADL:  op2slot(e, "mov", "rax", "rbp", -8L * (in->a + 1)); push_rax(e); break;
    case OP_STOREL: op1(e, "pop", "rax"); op2slotd(e, "mov", "rbp", -8L * (in->a + 1), "rax"); break;
    case OP_LOADG:
        put(e, "    mov rax, [g"); putl(e, in->a); put(e, "]\n"); push_rax(e); break;
    case OP_STOREG:
        op1(e, "pop", "rax");
        put(e, "    mov [g"); putl(e, in->a); put(e, "], rax\n"); break;

    case OP_POP:
        /* `call main; pop; halt` is how every program ends, and the value it
         * drops is main's. Keeping it makes the exit status mean something. */
        if (pc + 1 < p->ncode && p->code[pc + 1].op == OP_HALT) op1(e, "pop", "rdi");
        else op2n(e, "add", "rsp", 8);
        break;
    case OP_DUP:    op2(e, "mov", "rax", "[rsp]"); push_rax(e); break;

    case OP_ADD:  pop2(e); op2(e, "add",  "rax", "rcx"); push_rax(e); break;
    case OP_SUB:  pop2(e); op2(e, "sub",  "rax", "rcx"); push_rax(e); break;
    case OP_MUL:  pop2(e); op2(e, "imul", "rax", "rcx"); push_rax(e); break;
    /* A divide by zero faults here rather than raising the VM's error. The
     * process dies with a core dump naming the instruction, which on this
     * machine is a thing you can actually read back -- see `core`. */
    case OP_DIV:  pop2(e); op0(e, "cqo"); op1(e, "idiv", "rcx"); push_rax(e); break;
    case OP_MOD:  pop2(e); op0(e, "cqo"); op1(e, "idiv", "rcx");
                  op2(e, "mov", "rax", "rdx"); push_rax(e); break;
    case OP_BAND: pop2(e); op2(e, "and", "rax", "rcx"); push_rax(e); break;
    case OP_BOR:  pop2(e); op2(e, "or",  "rax", "rcx"); push_rax(e); break;
    case OP_BXOR: pop2(e); op2(e, "xor", "rax", "rcx"); push_rax(e); break;
    case OP_SHL:  pop2(e); op2(e, "shl", "rax", "cl");  push_rax(e); break;
    case OP_SHR:  pop2(e); op2(e, "sar", "rax", "cl");  push_rax(e); break;
    case OP_NEG:  op1(e, "pop", "rax"); op1(e, "neg", "rax"); push_rax(e); break;
    case OP_NOT:
        op1(e, "pop", "rax"); op2n(e, "cmp", "rax", 0);
        op1(e, "sete", "al"); op2(e, "movzx", "rax", "al"); push_rax(e); break;

    case OP_EQ: compare(e, "sete");  break;
    case OP_NE: compare(e, "setne"); break;
    case OP_LT: compare(e, "setl");  break;
    case OP_LE: compare(e, "setle"); break;
    case OP_GT: compare(e, "setg");  break;
    case OP_GE: compare(e, "setge"); break;

    case OP_JMP: put(e, "    jmp "); label_pc(e, "L", in->a, "\n"); break;
    case OP_JZ:
        op1(e, "pop", "rax"); op2n(e, "cmp", "rax", 0);
        put(e, "    je "); label_pc(e, "L", in->a, "\n"); break;
    case OP_JNZ:
        op1(e, "pop", "rax"); op2n(e, "cmp", "rax", 0);
        put(e, "    jne "); label_pc(e, "L", in->a, "\n"); break;

    case OP_CALL: {
        const CCFunc *f = &p->funcs[in->a];
        put(e, "    call "); label_pc(e, "F", f->entry, "\n");
        if (in->b > 0) op2n(e, "add", "rsp", 8L * in->b);   /* drop the arguments */
        push_rax(e);
        break; }
    case OP_RET:
        op1(e, "pop", "rax");
        op2(e, "mov", "rsp", "rbp");
        op1(e, "pop", "rbp");
        op0(e, "ret");
        break;

    case OP_PUTC:
        op1(e, "pop", "rdi"); op1(e, "call", "__putc");
        op2n(e, "mov", "rax", 0); push_rax(e); break;
    case OP_PRINT:
        op1(e, "pop", "rdi"); op1(e, "call", "__puts");
        op2n(e, "mov", "rax", 0); push_rax(e); break;
    case OP_PRINTLN:
        op1(e, "pop", "rdi"); op1(e, "call", "__puts");
        op2n(e, "mov", "rdi", 10); op1(e, "call", "__putc");
        op2n(e, "mov", "rax", 0); push_rax(e); break;
    case OP_PRINTF: {
        long argc = in->b;
        if (argc < 1) { ne_fail(e, "printf with no format string", 0); break; }
        /* The pushes put the format deepest and the last argument on top, so
         * the format is argc-1 slots up and the varargs run downwards from
         * there -- which is the order __printf walks them in. */
        op2slot(e, "mov", "rdi", "rsp", 8L * (argc - 1));
        if (argc >= 2) put(e, "    lea rsi, [rsp+"), putl(e, 8L * (argc - 2)), put(e, "]\n");
        else           op2n(e, "mov", "rsi", 0);
        op1(e, "call", "__printf");
        op2n(e, "add", "rsp", 8L * argc);
        op2n(e, "mov", "rax", 0); push_rax(e);
        break; }

    case OP_HALT:
        /* rdi already holds main's value when the POP above handed it over. */
        if (!(pc > 0 && p->code[pc - 1].op == OP_POP)) op2n(e, "mov", "rdi", 0);
        op2n(e, "mov", "rax", 60);
        op0(e, "syscall");
        break;

    default:
        ne_fail(e, "-N met an opcode it does not know", 0);
        break;
    }
}

/* ---- the whole program --------------------------------------------------- */
char *ccnative_emit(const CCProgram *p, char *err, uint32_t errcap) {
    NE *e = (NE *)kmalloc(sizeof(NE));
    if (!e) { if (errcap) err[0] = 0; sapp(err, "out of memory", errcap); return 0; }
    memset(e, 0, sizeof *e);
    e->p = p;

    check_supported(e);

    e->is_target = (uint8_t *)kmalloc((uint32_t)p->ncode + 1);
    if (!e->is_target) ne_fail(e, "out of memory", 0);
    else {
        memset(e->is_target, 0, (uint32_t)p->ncode + 1);
        for (int i = 0; i < p->ncode; i++) {
            int op = p->code[i].op;
            if (op == OP_JMP || op == OP_JZ || op == OP_JNZ) {
                long t = p->code[i].a;
                if (t >= 0 && t <= p->ncode) e->is_target[t] = 1;
            }
        }
        /* Function entries get an F label unconditionally; they only need an L
         * as well when something really does jump to one. */
        if (p->start >= 0 && p->start <= p->ncode) e->is_target[p->start] = 1;
    }

    if (!e->failed) {
        put(e, "; generated by BoltCC -N. `cc -S` prints this instead of running it.\n");
        put(e, "bits 64\n");
        put(e, "section .text\n");
        put(e, "global _start\n");
        put(e, "_start:\n");
        op2(e, "xor", "rbp", "rbp");
        put(e, "    jmp "); label_pc(e, "L", p->start, "\n");
        put(e, "\n");

        for (int pc = 0; pc < p->ncode && !e->failed; pc++) {
            const CCFunc *f = func_at(p, pc);
            /* Two labels, and the order between them is the whole point. A
             * function whose body opens with a loop has its entry pc and that
             * loop's back edge on the same instruction: F goes in front of the
             * prologue because a call has to run it, L goes behind because a
             * back edge must not. One label serving both re-ran the prologue
             * every iteration, building a fresh frame each time out of
             * arguments read at offsets from an rbp that had just moved. */
            if (f) { label_pc(e, "F", pc, ":\n"); emit_prologue(e, f); }
            if (e->is_target[pc]) label_pc(e, "L", pc, ":\n");
            emit_instr(e, pc);
        }
        /* A jump can target one past the last instruction: a loop whose exit
         * is the end of the program has nowhere else to land. */
        if (e->is_target[p->ncode]) {
            label_pc(e, "L", p->ncode, ":\n");
            op2n(e, "mov", "rdi", 0);
            op2n(e, "mov", "rax", 60);
            op0(e, "syscall");
        }

        put(e, "\n");
        put(e, RUNTIME);

        put(e, "\nsection .data\n");
        for (int i = 0; i < p->nglob; i++) { put(e, "g"); putl(e, i); put(e, ": dq 0\n"); }
        for (int i = 0; i < e->nstr; i++) emit_string_data(e, i);
        put(e, "__numbuf: resb 24\n");
        put(e, "__numend:\n");
    }

    char *out = e->buf;
    if (e->failed) { if (errcap) err[0] = 0; sapp(err, e->err, errcap); if (out) kfree(out); out = 0; }
    if (e->is_target) kfree(e->is_target);
    kfree(e);
    return out;
}
