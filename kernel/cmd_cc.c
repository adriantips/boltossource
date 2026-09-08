#include <stdint.h>
#include "commands.h"
#include "ccbuild.h"
#include "boltcc.h"
#include "fs.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include <stdio.h>

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_cc.c
 *  `cc` -- the C compiler driver: preprocess, combine, compile, run.
 *
 *  BoltCC compiles one translation unit at a time and has no separate
 *  compilation or linking model, so `cc a.c b.c` preprocesses each file and
 *  concatenates the results into a single unit before compiling. That is a
 *  unity build, and it is worth naming plainly rather than dressing up as
 *  something it is not -- but it does mean a program really can be split across
 *  files, with headers shared between them, which is the thing that was
 *  impossible before.
 *
 *  Forward references already work in BoltCC, so the order of the files on the
 *  command line does not matter.
 *
 *  The list of files, the include path and the macros are a build target
 *  (include/ccbuild.h). `cc` builds one from the command line; `build` reads
 *  one out of a project's build file. Past that point they are the same code,
 *  because a project that builds in the shell and not in the IDE would be a bug
 *  nobody could find.
 * ===========================================================================*/

static void usage(void) {
    kprintf("usage: cc [options] FILE...\n"
            "  -o FILE     write the combined, preprocessed unit here instead of running\n"
            "  -E          preprocess only, and print the result\n"
            "  -I DIR      add a directory to the #include <> search path\n"
            "  -D N[=V]    define a macro\n"
            "  -g          run under the debugger (breakpoints, stepping, locals)\n"
            "  -q          run, but do not print the compile banner\n"
            "\n"
            "Several files are preprocessed and combined into one translation unit\n"
            "(a unity build -- BoltCC has no separate linking step), then compiled\n"
            "and run. The dialect comes from the first file's extension:\n"
            "  .c -> C    .cpp/.cc/.cxx -> C++    .cs -> C#\n"
            "\n"
            "To build a project instead of a file list, write the files down in a\n"
            "build.txt beside them and use `build` (see: help build).\n");
}

int cmd_cc(int argc, char **argv) {
    const char *srcs[CCB_MAX_SRC];
    int nsrc = 0;
    const char *outpath = 0;
    int only_pp = 0, quiet = 0, debug = 0;
    ccb_target_t t;

    ccb_from_list(&t, 0, 0);          /* zero it; sources are added below */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc)      outpath = argv[++i];
        else if (!strcmp(a, "-E"))                 only_pp = 1;
        else if (!strcmp(a, "-g"))                 debug = 1;
        else if (!strcmp(a, "-q"))                 quiet = 1;
        else if (!strcmp(a, "-I") && i + 1 < argc) ccb_add_include(&t, argv[++i]);
        else if (!strncmp(a, "-I", 2) && a[2])     ccb_add_include(&t, a + 2);
        else if (!strcmp(a, "-D") && i + 1 < argc) ccb_add_define(&t, argv[++i]);
        else if (!strncmp(a, "-D", 2) && a[2])     ccb_add_define(&t, a + 2);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] == '-') { kprintf("cc: unknown option '%s'\n", a); return 1; }
        else if (nsrc < CCB_MAX_SRC) srcs[nsrc++] = a;
        else { kprintf("cc: too many source files (max %d)\n", CCB_MAX_SRC); return 1; }
    }

    if (!nsrc) { usage(); return 1; }
    for (int i = 0; i < nsrc; i++) ccb_add_source(&t, srcs[i]);

    char err[192];
    char *unit = ccb_unit(&t, err, sizeof err);
    if (!unit) { kprintf("cc: %s\n", err[0] ? err : "build failed"); return 1; }

    int failed = 0;
    uint32_t o = (uint32_t)strlen(unit);

    if (only_pp) {
        kprintf("%s", unit);
    } else if (outpath) {
        fs_node *f = fs_lookup(outpath);
        if (!f) f = fs_create(outpath, 0);
        if (!f || !fs_may(f, FS_PERM_WRITE)) {
            kprintf("cc: cannot write %s\n", outpath);
            failed = 1;
        } else if (fs_write(f, unit, o) != 0) {
            kprintf("cc: write failed\n");
            failed = 1;
        } else {
            kprintf("cc: %d file%s -> %s (%u bytes)\n",
                    nsrc, nsrc == 1 ? "" : "s", outpath, o);
        }
    } else {
        int lang = ccb_lang(&t);
        if (!quiet)
            kprintf("cc: %d file%s, %s\n", nsrc, nsrc == 1 ? "" : "s",
                    boltcc_lang_name(lang));
        int rc = debug ? boltcc_debug(lang, unit) : boltcc_run(lang, unit);
        if (rc != 0) failed = 1;
    }

    kfree(unit);
    return failed ? 1 : 0;
}
