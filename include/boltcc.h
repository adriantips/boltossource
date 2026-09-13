#pragma once

/* ===========================================================================
 *  BoltCC  -  a small, freestanding C-family compiler + bytecode VM.
 *
 *  One real compiler pipeline (lexer -> recursive-descent parser -> AST ->
 *  stack-bytecode codegen -> register/stack VM) with three front-end dialects:
 *  C, C++ and C#. It genuinely compiles the source to bytecode and executes it;
 *  it is not an interpreter over the text. Values are 64-bit integers and
 *  strings (the kernel is built with no FPU, so there is no floating point).
 *
 *  Supported across all three dialects: functions (with recursion and forward
 *  references), int/char/long/bool/string/var declarations, arithmetic, bitwise
 *  and comparison operators, &&/||/! with short-circuit, ?: , ++/-- (pre+post),
 *  compound assignment, if/else, while, for, return, break, continue, blocks,
 *  structs (fields, `.` and `->`, arrays of them, self-referential ones) --
 *  which are references rather than values, so passing one does not copy it --
 *  string concatenation with +, string indexing, and a small builtin library
 *  (len, str, int, abs, min, max, chr, ord, pow, sqrt, gcd, upper, lower,
 *  substr). Per dialect:
 *    C   : printf (real %d/%i/%u/%x/%c/%s/%% formatting), puts, putchar
 *    C++ : std::cout << ... << std::endl  (chained), printf, classes ignored
 *    C#  : Console.WriteLine / Console.Write  (incl. {0} {1} format), class +
 *          namespace + using are unwrapped, entry point is Main()
 *  Output is written through kputc(), so the IDE captures it with
 *  console_set_sink(), exactly like BoltPython.
 * ===========================================================================*/

enum { BCC_C = 0, BCC_CPP = 1, BCC_CSHARP = 2 };

const char *boltcc_lang_name(int lang);
const char *boltcc_version(void);

/* Compile and run src for the given dialect. Returns 0 on success; nonzero on a
 * compile or runtime error (a diagnostic has already been printed). */
int  boltcc_run(int lang, const char *src);

/* Compile and run under the source-level debugger: the VM stops on the first
 * line and takes commands (breakpoints, stepping, backtrace, printing locals).
 * `src` is also what `list` displays, so it should be the preprocessed unit the
 * line numbers refer to. */
int  boltcc_debug(int lang, const char *src);

/* Compile src and return NASM-syntax x86-64 for it instead of running it: the
 * same front end and the same bytecode, handed to kernel/ccnative.c rather than
 * to the VM. Returns 0 with *asm_out set to a kmalloc'd string the caller
 * kfree()s, or nonzero having already printed why -- usually because the
 * program uses something the native backend does not implement, which it names.
 */
int  boltcc_native(int lang, const char *src, char **asm_out);

/* The same, as one translation unit of a larger program rather than as a whole
 * one: functions and file-scope variables become names the linker can see, a
 * prototype with no definition here becomes a call into another object, and
 * the entry point is emitted only by the unit that defines main(). `cc -c`
 * assembles the result into a relocatable; `ld` makes a program of several.
 *
 * A unit with no main() may not give a file-scope variable an initialiser,
 * because the code that would run it lives in the preamble only that unit has.
 * It says so rather than leaving the variable quietly zero. */
int  boltcc_module(int lang, const char *src, char **asm_out);
