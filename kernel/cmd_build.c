#include <stdint.h>
#include "commands.h"
#include "ccbuild.h"
#include "boltcc.h"
#include "fs.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_build.c
 *  `build` -- build a project, rather than a file list.
 *
 *  `cc a.c b.c util.c` is a multi-file build whose file list lives in the head
 *  of whoever types it. Writing that list down beside the sources is the whole
 *  difference between a directory of files and a project: the build is the
 *  same one every time, and the same one the Code app runs.
 * ===========================================================================*/

static void usage(void) {
    kprintf("usage: build [options] [DIR]\n"
            "  -t NAME     build this target (default: the first one)\n"
            "  -l          list the targets and their sources, and build nothing\n"
            "  -g          run under the debugger\n"
            "  -o FILE     write the combined unit here instead of running it\n"
            "  -q          do not print the build banner\n"
            "\n"
            "DIR defaults to the working directory. Its %s names the targets:\n"
            "\n"
            "  target hello          # a target; everything until the next one\n"
            "    src main.c util.c   # sources, relative to the project directory\n"
            "    include inc         # an -I directory\n"
            "    define DEBUG=1      # a -D macro\n"
            "\n"
            "Lines before the first `target` are a target named after the directory,\n"
            "so a one-target project is just `src main.c util.c`. A directory with\n"
            "no %s builds every source file in it.\n", CCB_FILE, CCB_FILE);
}

static void show(const ccb_target_t *t) {
    kprintf("target %s  (%s, %d file%s)\n", t->name,
            boltcc_lang_name(ccb_lang(t)), t->nsrc, t->nsrc == 1 ? "" : "s");
    for (int i = 0; i < t->nsrc; i++) kprintf("  src      %s\n", t->src[i]);
    for (int i = 0; i < t->ninc; i++) kprintf("  include  %s\n", t->inc[i]);
    for (int i = 0; i < t->ndef; i++) kprintf("  define   %s\n", t->def[i]);
}

int cmd_build(int argc, char **argv) {
    const char *want = 0, *outpath = 0, *dir = 0;
    int list = 0, debug = 0, quiet = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "-t") && i + 1 < argc) want = argv[++i];
        else if (!strcmp(a, "-o") && i + 1 < argc) outpath = argv[++i];
        else if (!strcmp(a, "-l")) list = 1;
        else if (!strcmp(a, "-g")) debug = 1;
        else if (!strcmp(a, "-q")) quiet = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] == '-') { kprintf("build: unknown option '%s'\n", a); return 1; }
        else dir = a;
    }

    char here[CCB_PATH_MAX];
    if (!dir) { fs_abspath(fs_cwd(), here, sizeof here); dir = here; }

    if (list) {
        char names[CCB_MAX_TGT][CCB_NAME_MAX];
        int n = ccb_list(dir, names, CCB_MAX_TGT);
        char err[192];
        ccb_target_t t;
        if (n == 0) {
            /* No build file, or one with only a preamble: whatever ccb_load
             * resolves is the answer, and saying so is more use than "none". */
            if (ccb_load(dir, 0, &t, err, sizeof err) != 0) {
                kprintf("build: %s\n", err);
                return 1;
            }
            show(&t);
            if (t.implicit) kprintf("  (no %s -- every source in the directory)\n", CCB_FILE);
            return 0;
        }
        for (int i = 0; i < n; i++) {
            if (ccb_load(dir, names[i], &t, err, sizeof err) != 0) {
                kprintf("build: %s\n", err);
                return 1;
            }
            show(&t);
        }
        return 0;
    }

    ccb_target_t t;
    char err[192];
    if (ccb_load(dir, want, &t, &err[0], sizeof err) != 0) {
        kprintf("build: %s\n", err);
        return 1;
    }

    char *unit = ccb_unit(&t, err, sizeof err);
    if (!unit) { kprintf("build: %s\n", err[0] ? err : "build failed"); return 1; }

    int failed = 0;
    uint32_t len = (uint32_t)strlen(unit);

    if (outpath) {
        fs_node *f = fs_lookup(outpath);
        if (!f) f = fs_create(outpath, 0);
        if (!f || !fs_may(f, FS_PERM_WRITE)) {
            kprintf("build: cannot write %s\n", outpath);
            failed = 1;
        } else if (fs_write(f, unit, len) != 0) {
            kprintf("build: write failed\n");
            failed = 1;
        } else {
            kprintf("build: %s -> %s (%u bytes)\n", t.name, outpath, len);
        }
    } else {
        if (!quiet)
            kprintf("build: %s, %d file%s, %s\n", t.name, t.nsrc,
                    t.nsrc == 1 ? "" : "s", boltcc_lang_name(ccb_lang(&t)));
        int rc = debug ? boltcc_debug(ccb_lang(&t), unit)
                       : boltcc_run(ccb_lang(&t), unit);
        if (rc != 0) failed = 1;
    }

    kfree(unit);
    return failed ? 1 : 0;
}
