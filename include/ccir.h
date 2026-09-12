#pragma once

/* ===========================================================================
 *  BoltCC's bytecode, as something other than BoltCC can read it.
 *
 *  The opcodes and the instruction layout used to live inside kernel/boltcc.c,
 *  where only the VM could see them. They are here because there is now a
 *  second consumer -- kernel/ccnative.c, which turns the same bytecode into
 *  x86-64 instead of interpreting it -- and an IR with two backends is an
 *  interface whether or not anyone writes it down.
 *
 *  This is the compiler's private IR, not a stable format: it is versioned by
 *  being in the same tree as both of its consumers, and nothing outside the
 *  kernel should build against it. include/syscall.h is the interface for
 *  programs; this one is for the compiler.
 * ===========================================================================*/

#include <stdint.h>

enum {
    OP_PUSHI, OP_PUSHS, OP_LOADL, OP_STOREL, OP_LOADG, OP_STOREG,
    OP_POP, OP_DUP,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG,
    OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_NOT,
    OP_JMP, OP_JZ, OP_JNZ,
    OP_CALL, OP_RET,
    OP_PRINT, OP_PRINTLN, OP_PRINTF, OP_CSPRINT, OP_PUTC,
    OP_BUILTIN, OP_INDEX, OP_HALT,
    OP_NEWARR, OP_ASTORE,
    OP_NEWSTRUCT, OP_NEWSTRUCTARR, OP_FIELD, OP_FIELDSET, OP_REF,
};

/* `line` is what turns a runtime error from "something went wrong" into a
 * place in the source. It costs four bytes an instruction and is the difference
 * between a language you can debug and one you can only re-read. */
typedef struct { int op; long a, b; const char *s; int line; } Instr;

/* ---------------------------------------------------------------------------
 *  What the native backend needs to know about a compiled program. The VM
 *  reads this out of the compiler's own context; this is the same information
 *  with nothing else attached, so the backend cannot reach into compiler state
 *  that is only valid while parsing.
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *name;
    int nparams;
    int nlocals;        /* every slot the body used, params included */
    int entry;          /* pc of the function's first instruction    */
} CCFunc;

typedef struct {
    const Instr  *code;  int ncode;
    const CCFunc *funcs; int nfunc;
    int nglob;
    int start;           /* pc of the preamble: globals, then main   */
} CCProgram;

/* Emit NASM-syntax x86-64 for `p`, as a kmalloc'd NUL-terminated string the
 * caller kfree()s. Returns 0 and fills `err` when the program uses something
 * the native backend does not implement -- naming it, because "cannot compile
 * this" with no subject is a message you cannot act on. */
char *ccnative_emit(const CCProgram *p, char *err, uint32_t errcap);
