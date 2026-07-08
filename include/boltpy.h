#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltPython  -  a small, freestanding Python-3 subset interpreter.
 *
 *  Tree-walking interpreter with an indentation-aware lexer, recursive-descent
 *  parser and an arena allocator backed by the kernel heap. It is INTEGER-ONLY
 *  (no floating point: the kernel is built with SSE/x87 disabled), so `/`
 *  behaves like `//`. Output is written through kputc()/kprintf(), so the GUI
 *  terminal/IDE can capture it via console_set_sink().
 *
 *  Supported: int/bool/str/list/None, arithmetic + bitwise + comparison +
 *  boolean ops, chained comparisons, slicing, if/elif/else, while, for-in,
 *  range(), def/return/recursion, break/continue/pass, global, augmented
 *  assignment, and a library of builtins (print, len, range, str, int, bool,
 *  abs, min, max, sum, ord, chr, hex, type, input, sorted, reversed, ...) plus
 *  list methods (append/pop/insert) and str methods (upper/lower/split/...).
 * ===========================================================================*/

typedef struct bpy_vm bpy_vm;

/* One-shot: parse + run a complete program. Returns 0 on success, 1 on a
 * syntax or runtime error (already reported to the output). */
int  bpy_run(const char *src);

/* Persistent session (REPL): globals survive across bpy_exec() calls. */
bpy_vm *bpy_new(void);
void    bpy_free(bpy_vm *vm);

/* Run source in an existing VM. If echo!=0 and the program is a single bare
 * expression, its repr is printed (REPL behaviour). Returns 0 ok / 1 error. */
int     bpy_exec(bpy_vm *vm, const char *src, int echo);

/* Host hook for input(): reader fills buf (cap incl. NUL), returns length.
 * Default reads a line from the PS/2 keyboard with echo. */
void    bpy_set_input(int (*reader)(char *buf, int cap));

/* Version string for the banner. */
const char *bpy_version(void);
