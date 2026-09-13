#include <stdint.h>
#include "commands.h"
#include "ar.h"
#include "fs.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"

/* `ar` -- a static library, which is a pile of object files and a wrapper.
 *
 * The key letter comes first and without a dash, the way it has since 1971,
 * because every Makefile in the world is written that way and one that spells
 * it `--replace` would be a library nobody could build. Modifiers after the
 * key (c, s, u, v) are accepted and ignored: there is no symbol index to build
 * or skip here -- see include/ar.h for why.
 */

#define AR_MAX_MEMBERS 32

static void usage(void) {
    kprintf("usage: ar r ARCHIVE.a FILE.o ...    add or replace members\n"
            "       ar t ARCHIVE.a               list what is in it\n"
            "       ar x ARCHIVE.a [FILE ...]    extract members\n"
            "\n"
            "`ld` pulls from an archive by need: a member joins the link only\n"
            "if it defines a name nothing else has.\n");
}

static fs_node *readable(const char *path, const char *who) {
    fs_node *f = fs_lookup(path);
    if (!f || f->is_dir) { kprintf("%s: %s: no such file\n", who, path); return 0; }
    if (!fs_may(f, FS_PERM_READ)) { kprintf("%s: %s: permission denied\n", who, path); return 0; }
    return f;
}

static int do_list(const char *path) {
    fs_node *f = readable(path, "ar");
    if (!f) return 1;
    if (!ar_is_archive(f->data, f->size)) { kprintf("ar: %s: not an archive\n", path); return 1; }

    const char *ln = 0;
    uint32_t lnlen = 0, pos = 8;
    ar_member_t m;
    int rc, n = 0;
    while ((rc = ar_next(f->data, f->size, &pos, &ln, &lnlen, &m)) == 1) {
        if (m.kind != AR_FILE || !m.name[0]) continue;
        kprintf("%-24s %u\n", m.name, (unsigned)m.len);
        n++;
    }
    if (rc < 0) { kprintf("ar: %s: malformed archive\n", path); return 1; }
    if (!n) kprintf("ar: %s is empty\n", path);
    return 0;
}

static int do_extract(const char *path, int argc, char **argv, int first) {
    fs_node *f = readable(path, "ar");
    if (!f) return 1;
    if (!ar_is_archive(f->data, f->size)) { kprintf("ar: %s: not an archive\n", path); return 1; }

    const char *ln = 0;
    uint32_t lnlen = 0, pos = 8;
    ar_member_t m;
    int rc, n = 0;
    while ((rc = ar_next(f->data, f->size, &pos, &ln, &lnlen, &m)) == 1) {
        if (m.kind != AR_FILE || !m.name[0]) continue;
        if (first < argc) {
            int wanted = 0;
            for (int i = first; i < argc; i++)
                if (!strcmp(argv[i], m.name)) { wanted = 1; break; }
            if (!wanted) continue;
        }
        fs_node *o = fs_lookup(m.name);
        if (!o) o = fs_create(m.name, 0);
        if (!o || o->is_dir || !fs_may(o, FS_PERM_WRITE) || fs_write(o, m.data, m.len) != 0) {
            kprintf("ar: cannot write %s\n", m.name);
            return 1;
        }
        kprintf("x %s\n", m.name);
        n++;
    }
    if (rc < 0) { kprintf("ar: %s: malformed archive\n", path); return 1; }
    if (!n) kprintf("ar: nothing extracted\n");
    return 0;
}

static const char *base_of(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int do_replace(const char *path, int argc, char **argv, int first) {
    if (first >= argc) { kprintf("ar: r needs at least one file to put in\n"); return 1; }

    ar_input_t in[AR_MAX_MEMBERS];
    int n = 0, nkept = 0;

    /* The existing members come first and keep their order; a name given on
     * the command line replaces the one already in there rather than joining
     * it, which is the r in `ar r` and the reason a rebuilt object does not
     * leave its old self behind to be linked instead. */
    fs_node *old = fs_lookup(path);
    if (old && !old->is_dir && ar_is_archive(old->data, old->size)) {
        const char *ln = 0;
        uint32_t lnlen = 0, pos = 8;
        ar_member_t m;
        int rc;
        while ((rc = ar_next(old->data, old->size, &pos, &ln, &lnlen, &m)) == 1) {
            if (m.kind != AR_FILE || !m.name[0]) continue;
            int replaced = 0;
            for (int i = first; i < argc; i++)
                if (!strcmp(base_of(argv[i]), m.name)) { replaced = 1; break; }
            if (replaced) continue;
            if (n >= AR_MAX_MEMBERS) { kprintf("ar: too many members (max %d)\n", AR_MAX_MEMBERS); return 1; }
            /* m.name points into the archive we are about to overwrite, so the
             * name has to be copied out; the bytes are read before the write. */
            char *keep = (char *)kmalloc((uint32_t)strlen(m.name) + 1);
            if (!keep) { kprintf("ar: out of memory\n"); return 1; }
            strlcpy(keep, m.name, (uint32_t)strlen(m.name) + 1);
            in[n].name = keep;
            in[n].data = m.data;
            in[n].len  = m.len;
            n++;
        }
        if (rc < 0) { kprintf("ar: %s: malformed archive\n", path); return 1; }
        nkept = n;
    }

    for (int i = first; i < argc; i++) {
        fs_node *f = readable(argv[i], "ar");
        if (!f) return 1;
        if (n >= AR_MAX_MEMBERS) { kprintf("ar: too many members (max %d)\n", AR_MAX_MEMBERS); return 1; }
        in[n].name = argv[i];
        in[n].data = f->data;
        in[n].len  = f->size;
        n++;
    }

    char err[AR_MAX_ERR] = "";
    uint32_t len = 0;
    uint8_t *img = ar_create(in, n, &len, err, sizeof err);
    /* TODO: the early returns above walk out on these; only the success path
     * gives them back. A handful of bytes on a path that already printed an
     * error, so it has never been worth the goto. */
    for (int i = 0; i < nkept; i++) kfree((void *)in[i].name);
    if (!img) { kprintf("ar: %s\n", err[0] ? err : "could not build the archive"); return 1; }

    fs_node *o = fs_lookup(path);
    if (!o) o = fs_create(path, 0);
    if (!o || o->is_dir || !fs_may(o, FS_PERM_WRITE) || fs_write(o, img, len) != 0) {
        kprintf("ar: cannot write %s\n", path);
        kfree(img);
        return 1;
    }
    kfree(img);
    kprintf("ar: %s (%d member%s, %u bytes)\n", path, n, n == 1 ? "" : "s", (unsigned)len);
    return 0;
}

int cmd_ar(int argc, char **argv) {
    if (argc < 3) { usage(); return argc < 2 ? 1 : 0; }

    const char *key = argv[1];
    if (key[0] == '-') key++;              /* `ar -rcs` is tolerated too */
    char op = key[0];
    const char *path = argv[2];

    if (op == 'r') return do_replace(path, argc, argv, 3);
    if (op == 't') return do_list(path);
    if (op == 'x') return do_extract(path, argc, argv, 3);

    kprintf("ar: unknown key '%s'\n", argv[1]);
    usage();
    return 1;
}
