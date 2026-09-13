#include <stdint.h>
#include "commands.h"
#include "link.h"
#include "ar.h"
#include "fs.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"

/* `ld` -- put the objects together.
 *
 * The pieces `as -c` and `cc -c` leave behind are not programs; this is what
 * makes one out of them. It is the last step of every build on this machine
 * that produces something `run` can execute, and `make` calls it by exactly
 * the command line you would have typed.
 *
 * The inputs stay where they are: a file's bytes are handed to the linker in
 * place rather than copied, because linking touches no filesystem and a
 * 32-object link has no reason to duplicate 32 buffers on a heap this small.
 */

static void usage(void) {
    kprintf("usage: ld [-o OUT] [-e ENTRY] FILE.o|LIB.a ...\n"
            "  -o FILE   where the program goes (default: a.out)\n"
            "  -e NAME   the entry point (default: _start)\n"
            "  -v        say what the link did\n"
            "\n"
            "Objects join the link in the order given. An archive is different:\n"
            "a member joins only if it defines something still missing, and the\n"
            "scan repeats until a pass adds nothing -- so an archive that needs\n"
            "another one works whichever way round you list them.\n");
}

int cmd_ld(int argc, char **argv) {
    ld_input_t in[LD_MAX_INPUT];
    int n = 0, verbose = 0;
    const char *out = "a.out", *entry = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc)      out = argv[++i];
        else if (!strcmp(a, "-e") && i + 1 < argc) entry = argv[++i];
        else if (!strcmp(a, "-v"))                 verbose = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] == '-') { kprintf("ld: unknown option %s\n", a); return 1; }
        else if (n >= LD_MAX_INPUT) {
            kprintf("ld: too many inputs (max %d)\n", LD_MAX_INPUT);
            return 1;
        } else {
            fs_node *f = fs_lookup(a);
            if (!f || f->is_dir) { kprintf("ld: %s: no such file\n", a); return 1; }
            if (!fs_may(f, FS_PERM_READ)) { kprintf("ld: %s: permission denied\n", a); return 1; }
            in[n].name = a;
            in[n].data = f->data;
            in[n].len  = f->size;
            n++;
        }
    }

    if (!n) { usage(); return 1; }

    char err[LD_MAX_ERR] = "";
    uint32_t len = 0;
    ld_stats_t st;
    memset(&st, 0, sizeof st);
    uint8_t *img = ld_link(in, n, entry, &len, &st, err, sizeof err);
    if (!img) {
        kprintf("ld: %s\n", err[0] ? err : "link failed");
        return 1;
    }

    fs_node *o = fs_lookup(out);
    if (!o) o = fs_create(out, 0);
    if (!o || o->is_dir || !fs_may(o, FS_PERM_WRITE) || fs_write(o, img, len) != 0) {
        kprintf("ld: cannot write %s\n", out);
        kfree(img);
        return 1;
    }
    kfree(img);

    if (verbose) {
        kprintf("ld: %u object%s", st.objects, st.objects == 1 ? "" : "s");
        if (st.from_archives) kprintf(" (%u from archives)", st.from_archives);
        kprintf(", %u relocation%s, text %u, data %u\n",
                st.relocs, st.relocs == 1 ? "" : "s", st.text, st.data);
    }
    kprintf("ld: -> %s (%u bytes, entry 0x%x)\n", out, (unsigned)len,
            (unsigned)st.entry);
    kprintf("    run it with: run %s\n", out);
    return 0;
}
