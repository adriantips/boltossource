#pragma once
#include <stdint.h>

/* BoltAS -- an x86-64 assembler that writes an ELF64 executable.
 *
 * This is the first thing in the tree that turns text typed on BoltOS into
 * machine code in a file BoltOS can run. `cc` compiles C to bytecode and an
 * interpreter runs it; nothing produced an actual program. Now something does.
 *
 * NASM syntax, because that is what boot/ is written in and what anyone
 * looking at this repo has already read. It sizes, then settles the symbols,
 * then emits; kernel/asm.c explains why that is three passes and not two.
 * Nothing shrinks between them -- every jump is rel32 and every label-valued
 * immediate is a movabs -- which is what makes one pass's addresses valid in
 * the next.
 *
 * The instruction set is a working subset, not an x86 manual: enough to write
 * a program that computes something, loops, calls, and talks to the kernel
 * through `syscall`. What is missing is missing because nothing needed it yet,
 * and asm_error names it rather than assembling something else.
 */

#define ASM_LOAD_BASE 0x400000ull      /* where user/user.ld puts a program */
#define ASM_MAX_ERR   128

/* Assemble `src` into a freshly allocated ELF64 image. On success returns the
 * image (caller kfree()s it) and fills *len. On failure returns 0 and leaves a
 * message in err -- with a line number, since an assembler that says only
 * "syntax error" is one you debug by bisection. */
uint8_t *asm_assemble(const char *src, uint32_t *len, char *err, uint32_t errcap);

/* The same source as a relocatable object instead of a program: the sections
 * start at zero, `extern` names are left undefined, and every field that
 * mentions a symbol becomes a hole with a relocation on it. `ld` is what turns
 * a pile of these back into something with an entry point.
 *
 * The subset is the same subset; what changes is only who resolves the names.
 */
uint8_t *asm_assemble_obj(const char *src, uint32_t *len, char *err, uint32_t errcap);
