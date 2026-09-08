#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltCPP  -  the C preprocessor.
 *
 *  Without this, BoltCC could only ever compile one self-contained file: no
 *  #include means no headers, no headers means no way to split a program
 *  across files, and a language you cannot split across files is not one you
 *  can write real software in. This is the piece that makes a multi-file
 *  project on BoltOS possible at all.
 *
 *  It is a text-to-text pass that runs before the lexer, which is what a C
 *  preprocessor actually is. Supported:
 *
 *      #include "file"   relative to the including file, then the search path
 *      #include <file>   the search path only
 *      #define NAME body            object-like macros
 *      #define NAME(a,b) body       function-like macros, with # stringify
 *      #undef NAME
 *      #ifdef / #ifndef / #if / #elif / #else / #endif
 *      #if with defined(X), integer literals, ! && || comparisons and ( )
 *      #pragma once
 *      #error message
 *
 *  Not supported, deliberately, because nothing in the tree needs them and each
 *  is a sharp edge: token pasting (##), variadic macros, and #line.
 * ===========================================================================*/

/* Preprocess `src`, which came from `path` (used to resolve relative includes
 * and to report errors; may be 0 for a buffer with no file behind it).
 *
 * Returns a heap buffer the caller must kfree(), or 0 on failure -- in which
 * case `err` holds a one-line diagnostic. */
char *cpp_run(const char *src, const char *path, char *err, uint32_t errcap);

/* Add a directory to the <> search path. /usr/include is registered by default,
 * which is where `cc` installs the bundled headers. */
void  cpp_add_include_dir(const char *dir);
void  cpp_reset_include_dirs(void);

/* Define a macro from the command line, as `cc -DNAME=value` does. */
void  cpp_define_cli(const char *namevalue);
void  cpp_reset_defines(void);
