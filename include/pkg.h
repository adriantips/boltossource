#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS packages.
 *
 *  The old `pkg` was five BoltPython programs compiled into the kernel. You
 *  could "install" one, which copied a string into /apps. Nothing could be
 *  added to it without rebuilding the kernel on another machine, which is the
 *  one thing a package manager exists to avoid.
 *
 *  So there is now a *format*, and everything else is built on it.
 *
 *  A package (`.pkg`) is a text header, a blank line, and then the files:
 *
 *      BOLTPKG 1
 *      package cproject
 *      version 1.0
 *      kind    source          # script | program | source
 *      summary a starter multi-file C project
 *      requires boltlib mathx
 *      run     /apps/thing.py  # optional: what `pkg run` executes
 *
 *      file 41 /home/Documents/newproj/build.txt
 *      <41 raw bytes>
 *      file 120 /home/Documents/newproj/main.c
 *      <120 raw bytes>
 *
 *  Sizes are explicit and the payload is copied verbatim, so a package can
 *  carry an ELF as easily as a script -- which is what makes binary packages
 *  possible at all.
 *
 *  A repository is a directory holding those files plus an `index`: the same
 *  header stanzas, one per package, separated by blank lines, each naming its
 *  file and that file's hash. `pkg update` copies the index from the
 *  repository -- over HTTP when the repository is a URL, out of the filesystem
 *  when it is a path -- and everything after that works the same either way.
 *
 *  Installed state lives in /var/lib/pkg/db/<name>: the version, the kind, what
 *  it requires and every file it owns, so `pkg remove` can take back exactly
 *  what was put down and nothing else.
 * ===========================================================================*/

#define PKG_ROOT     "/var/lib/pkg"
#define PKG_DB       PKG_ROOT "/db"
#define PKG_INDEX    PKG_ROOT "/index"
#define PKG_REPO     PKG_ROOT "/repo"      /* the repository shipped with the OS */
#define PKG_CONF     "/etc/pkg.conf"       /* the repository in use              */
#define PKG_MAX      (512 * 1024)          /* the largest package accepted       */
#define PKG_MAX_DEPTH 8                    /* dependency recursion limit         */

/* The packages built into the kernel, written out as a real repository the
 * first time one is needed. They go through the same packer, index and install
 * path as anything else -- a built-in catalogue that took a shortcut would not
 * be exercising the code that matters. */
typedef struct { const char *path, *body; } pkgfile_t;
typedef struct {
    const char *name, *version, *kind, *summary, *requires, *run;
    const pkgfile_t *files;
    int nfiles;
} pkgdef_t;

const pkgdef_t *pkg_builtin(int *count);
