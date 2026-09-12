#include <stdint.h>
#include "commands.h"
#include "asm.h"
#include "fs.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"

/* `as FILE.asm [-o OUT]` -- assemble, and write a program.
 *
 * The output is an ordinary ELF64 that `run` executes and `pkg` can ship, so
 * what comes out of here is a program in the same sense as anything else on
 * the machine. Without -o the name is the source with its extension replaced,
 * which is what every assembler has done since the seventies and what nobody
 * wants to type twice. */

static void default_out(const char *src, char *out, uint32_t cap) {
    strlcpy(out, src, cap);
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash)) *dot = 0;
    else strlcat(out, ".out", cap);
}

int cmd_as(int argc, char **argv) {
    const char *src = 0, *dst = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) dst = argv[++i];
        else if (argv[i][0] == '-') { kprintf("as: unknown option %s\n", argv[i]); return 1; }
        else if (!src) src = argv[i];
        else { kprintf("as: one source file at a time\n"); return 1; }
    }
    if (!src) {
        kprintf("usage: as FILE.asm [-o OUT]\n");
        return 1;
    }

    fs_node *f = fs_lookup(src);
    if (!f || f->is_dir) { kprintf("as: %s: no such file\n", src); return 1; }
    if (!fs_may(f, FS_PERM_READ)) { kprintf("as: %s: permission denied\n", src); return 1; }

    char *text = (char *)kmalloc(f->size + 1);
    if (!text) { kprintf("as: out of memory reading %s\n", src); return 1; }
    if (f->size) memcpy(text, f->data, f->size);
    text[f->size] = 0;

    char err[ASM_MAX_ERR] = "";
    uint32_t len = 0;
    uint8_t *img = asm_assemble(text, &len, err, sizeof err);
    kfree(text);
    if (!img) {
        kprintf("as: %s: %s\n", src, err[0] ? err : "assembly failed");
        return 1;
    }

    char outbuf[128];
    if (!dst) { default_out(src, outbuf, sizeof outbuf); dst = outbuf; }

    fs_node *o = fs_lookup(dst);
    if (!o) o = fs_create(dst, 0);
    if (!o || o->is_dir || fs_write(o, img, len) != 0) {
        kprintf("as: cannot write %s\n", dst);
        kfree(img);
        return 1;
    }
    kfree(img);
    kprintf("as: %s -> %s (%u bytes)\n", src, dst, (unsigned)len);
    kprintf("    run it with: run %s\n", dst);
    return 0;
}
