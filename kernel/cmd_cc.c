#include <stdint.h>
#include "commands.h"
#include "ccbuild.h"
#include "boltcc.h"
#include "asm.h"
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
            "  -S          print the x86-64 the native backend would emit\n"
            "  -N          compile to a native ELF64 program instead of running\n"
            "  -q          run, but do not print the compile banner\n"
            "\n"
            "Without -N a program runs on BoltCC's bytecode VM, which is the whole\n"
            "language. -N compiles it through the assembler into a real program the\n"
            "machine executes directly -- a subset: integers, functions, control\n"
            "flow, printf and puts. Anything outside that is named, not miscompiled.\n"
            "\n"
            "Several files are preprocessed and combined into one translation unit\n"
            "(a unity build -- BoltCC has no separate linking step), then compiled\n"
            "and run. The dialect comes from the first file's extension:\n"
            "  .c -> C    .cpp/.cc/.cxx -> C++    .cs -> C#\n"
            "\n"
            "To build a project instead of a file list, write the files down in a\n"
            "build.txt beside them and use `build` (see: help build).\n");
}

/* Strip a source file's extension to name what comes out of it, the way every
 * compiler has since long before any of this. */
static void derive_out(const char *src, const char *ext, char *out, uint32_t cap) {
    strlcpy(out, src, cap);
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash)) *dot = 0;
    if (ext) strlcat(out, ext, cap);
}

static int write_file(const char *path, const void *data, uint32_t len, const char *what) {
    fs_node *f = fs_lookup(path);
    if (!f) f = fs_create(path, 0);
    if (!f || f->is_dir || !fs_may(f, FS_PERM_WRITE) || fs_write(f, data, len) != 0) {
        kprintf("cc: cannot write %s\n", path);
        (void)what;
        return 1;
    }
    return 0;
}

/* -S prints (or writes) the assembly; -N hands that same text to the assembler
 * and writes the program. They are one function because -S is exactly -N with
 * the last step left off, and that is the property that makes a miscompile
 * something you can look at. */
static int cc_native(ccb_target_t *t, const char *unit, const char *first_src,
                     const char *outpath, int native, int quiet) {
    int lang = ccb_lang(t);
    char *text = 0;
    if (boltcc_native(lang, unit, &text) != 0 || !text) return 1;

    uint32_t tlen = (uint32_t)strlen(text);
    char namebuf[128];

    if (!native) {
        if (outpath) {
            int rc = write_file(outpath, text, tlen, "assembly");
            if (!rc) kprintf("cc: -> %s (%u bytes of assembly)\n", outpath, (unsigned)tlen);
            kfree(text);
            return rc;
        }
        kprintf("%s", text);
        kfree(text);
        return 0;
    }

    char err[ASM_MAX_ERR] = "";
    uint32_t len = 0;
    uint8_t *img = asm_assemble(text, &len, err, sizeof err);
    if (!img) {
        /* The assembler failing on generated code is this backend's bug, not
         * the program's, so say which side of the line the message came from. */
        kprintf("cc: the generated assembly did not assemble: %s\n",
                err[0] ? err : "unknown");
        kprintf("    `cc -S` on the same files prints what it tried to assemble\n");
        kfree(text);
        return 1;
    }
    kfree(text);

    if (!outpath) { derive_out(first_src, 0, namebuf, sizeof namebuf); outpath = namebuf; }
    int rc = write_file(outpath, img, len, "program");
    kfree(img);
    if (rc) return rc;
    if (!quiet) {
        kprintf("cc: %s -> %s (%u bytes, native)\n",
                boltcc_lang_name(lang), outpath, (unsigned)len);
        kprintf("    run it with: run %s\n", outpath);
    }
    return 0;
}

int cmd_cc(int argc, char **argv) {
    const char *srcs[CCB_MAX_SRC];
    int nsrc = 0;
    const char *outpath = 0;
    int only_pp = 0, quiet = 0, debug = 0, emit_asm = 0, native = 0;
    ccb_target_t t;

    ccb_from_list(&t, 0, 0);          /* zero it; sources are added below */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc)      outpath = argv[++i];
        else if (!strcmp(a, "-E"))                 only_pp = 1;
        else if (!strcmp(a, "-g"))                 debug = 1;
        else if (!strcmp(a, "-S"))                 emit_asm = 1;
        else if (!strcmp(a, "-N"))                 native = 1;
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
    } else if (emit_asm || native) {
        failed = cc_native(&t, unit, srcs[0], outpath, native, quiet);
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
