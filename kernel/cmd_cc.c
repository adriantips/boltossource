#include <stdint.h>
#include "commands.h"
#include "cpp.h"
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
 * ===========================================================================*/

#define MAX_SRCS 32

static char *slurp(const char *path, uint32_t *len_out) {
    fs_node *f = fs_lookup(path);
    if (!f || f->is_dir || !f->data) return 0;
    if (!fs_may(f, FS_PERM_READ)) return 0;
    char *b = (char *)kmalloc(f->size + 1);
    if (!b) return 0;
    memcpy(b, f->data, f->size);
    b[f->size] = 0;
    if (len_out) *len_out = f->size;
    return b;
}

static int lang_of(const char *path) {
    const char *dot = 0;
    for (const char *p = path; *p; p++) if (*p == '.') dot = p;
    if (!dot) return BCC_C;
    if (!strcmp(dot, ".cpp") || !strcmp(dot, ".cc") || !strcmp(dot, ".cxx")) return BCC_CPP;
    if (!strcmp(dot, ".cs")) return BCC_CSHARP;
    return BCC_C;
}

static void usage(void) {
    kprintf("usage: cc [options] FILE...\n"
            "  -o FILE     write the combined, preprocessed unit here instead of running\n"
            "  -E          preprocess only, and print the result\n"
            "  -I DIR      add a directory to the #include <> search path\n"
            "  -D N[=V]    define a macro\n"
            "  -q          run, but do not print the compile banner\n"
            "\n"
            "Several files are preprocessed and combined into one translation unit\n"
            "(a unity build -- BoltCC has no separate linking step), then compiled\n"
            "and run. The dialect comes from the first file's extension:\n"
            "  .c -> C    .cpp/.cc/.cxx -> C++    .cs -> C#\n");
}

int cmd_cc(int argc, char **argv) {
    const char *srcs[MAX_SRCS];
    int nsrc = 0;
    const char *outpath = 0;
    int only_pp = 0, quiet = 0;

    cpp_reset_defines();
    cpp_reset_include_dirs();
    cpp_add_include_dir("/usr/include");
    cpp_add_include_dir("/include");

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc)      outpath = argv[++i];
        else if (!strcmp(a, "-E"))                 only_pp = 1;
        else if (!strcmp(a, "-q"))                 quiet = 1;
        else if (!strcmp(a, "-I") && i + 1 < argc) cpp_add_include_dir(argv[++i]);
        else if (!strncmp(a, "-I", 2) && a[2])     cpp_add_include_dir(a + 2);
        else if (!strcmp(a, "-D") && i + 1 < argc) cpp_define_cli(argv[++i]);
        else if (!strncmp(a, "-D", 2) && a[2])     cpp_define_cli(a + 2);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] == '-') { kprintf("cc: unknown option '%s'\n", a); return 1; }
        else if (nsrc < MAX_SRCS) srcs[nsrc++] = a;
        else { kprintf("cc: too many source files (max %d)\n", MAX_SRCS); return 1; }
    }

    if (!nsrc) { usage(); return 1; }

    /* Preprocess each file, then join. A banner comment marks each boundary so
     * a compile error in the combined unit can still be traced to a file. */
    uint32_t total = 0;
    char *parts[MAX_SRCS];
    for (int i = 0; i < nsrc; i++) parts[i] = 0;

    int failed = 0;
    for (int i = 0; i < nsrc && !failed; i++) {
        uint32_t rawlen = 0;
        char *raw = slurp(srcs[i], &rawlen);
        if (!raw) { kprintf("cc: cannot read %s\n", srcs[i]); failed = 1; break; }

        char err[192];
        parts[i] = cpp_run(raw, srcs[i], err, sizeof err);
        kfree(raw);
        if (!parts[i]) { kprintf("cc: %s\n", err[0] ? err : "preprocessing failed"); failed = 1; break; }
        total += (uint32_t)strlen(parts[i]) + 64;
    }

    char *unit = 0;
    if (!failed) {
        unit = (char *)kmalloc(total + 1);
        if (!unit) { kprintf("cc: out of memory\n"); failed = 1; }
    }
    if (!failed) {
        uint32_t o = 0;
        for (int i = 0; i < nsrc; i++) {
            int n = snprintf(unit + o, total - o, "/* --- %s --- */\n", srcs[i]);
            if (n > 0) o += (uint32_t)n;
            const char *s = parts[i];
            while (*s && o < total) unit[o++] = *s++;
            if (o < total) unit[o++] = '\n';
        }
        unit[o] = 0;

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
            int lang = lang_of(srcs[0]);
            if (!quiet)
                kprintf("cc: %d file%s, %s\n", nsrc, nsrc == 1 ? "" : "s",
                        boltcc_lang_name(lang));
            int rc = boltcc_run(lang, unit);
            if (rc != 0) failed = 1;
        }
    }

    for (int i = 0; i < nsrc; i++) if (parts[i]) kfree(parts[i]);
    if (unit) kfree(unit);
    return failed ? 1 : 0;
}
