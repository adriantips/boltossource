#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS  -  include/ccbuild.h
 *  Build targets: what "a project" means to BoltOS.
 *
 *  `cc a.c b.c` was already a multi-file build, but the file list lived on the
 *  command line, which means it lives in the head of whoever typed it. A
 *  project is that list written down: a `build.txt` beside the sources naming
 *  one or more targets, each with its sources, include directories and macros.
 *  The IDE, the `build` command and `cc` itself all resolve a target through
 *  this one module, so a project built from the desktop and the same project
 *  built from the shell are the same build.
 *
 *  A directory with no build file still has a target -- every source file in
 *  it, in the dialect of the first one. The common case should not need a
 *  file to describe it.
 * ===========================================================================*/

#define CCB_MAX_SRC   32
#define CCB_MAX_INC    8
#define CCB_MAX_DEF   16
#define CCB_MAX_TGT   16
#define CCB_NAME_MAX  40
#define CCB_SRC_MAX   80
#define CCB_PATH_MAX 192

#define CCB_FILE "build.txt"        /* the build file, in the project root */

typedef struct {
    char name[CCB_NAME_MAX];
    char dir[CCB_PATH_MAX];                 /* project root; "" = bare paths */
    char src[CCB_MAX_SRC][CCB_SRC_MAX];     /* relative to dir               */
    int  nsrc;
    char inc[CCB_MAX_INC][CCB_PATH_MAX];
    int  ninc;
    char def[CCB_MAX_DEF][64];
    int  ndef;
    int  implicit;                          /* no build file: the dir was scanned */
} ccb_target_t;

/* Names of the targets declared in DIR/build.txt, newest-first in file order.
 * Returns the count (0 when there is no build file). */
int ccb_list(const char *dir, char names[][CCB_NAME_MAX], int max);

/* Resolve a target in `dir`: the one called `want`, or the first declared when
 * `want` is null or empty. Falls back to scanning the directory when there is
 * no build file. Returns 0, or -1 with a one-line reason in `err`. */
int ccb_load(const char *dir, const char *want, ccb_target_t *t,
             char *err, uint32_t errcap);

/* Build a target out of an explicit file list (what `cc` gets from argv).
 * Pass 0 files to get an empty target and fill it in with the adders. */
void ccb_from_list(ccb_target_t *t, const char *const *files, int nfile);
void ccb_add_source(ccb_target_t *t, const char *name);
void ccb_add_include(ccb_target_t *t, const char *dir);   /* relative to t->dir */
void ccb_add_define(ccb_target_t *t, const char *namevalue);

/* The BCC_* dialect a target compiles in: the first source file's extension. */
int ccb_lang(const ccb_target_t *t);

/* 1 if `name` ends in a source extension BoltCC compiles. */
int ccb_is_source(const char *name);

/* Preprocess every source and join them into one translation unit (BoltCC has
 * no linker; see kernel/cmd_cc.c). Returns a kmalloc'd string the caller
 * frees, or 0 with `err` filled in. */
char *ccb_unit(const ccb_target_t *t, char *err, uint32_t errcap);

/* The absolute path of a target's source `i`, written into `out`. */
void ccb_src_path(const ccb_target_t *t, int i, char *out, uint32_t cap);
