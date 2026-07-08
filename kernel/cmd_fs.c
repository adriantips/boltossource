#include <stdint.h>
#include "commands.h"
#include "fs.h"
#include "kprintf.h"
#include "string.h"
#include "pit.h"

/* ===========================================================================
 *  File & Directory + File Inspection commands (all backed by the RAM fs)
 * ===========================================================================*/

static fs_node *need(const char *p, const char *cmd) {
    fs_node *n = fs_lookup(p);
    if (!n) kprintf("%s: %s: no such file or directory\n", cmd, p);
    return n;
}
static fs_node *need_file(const char *p, const char *cmd) {
    fs_node *n = need(p, cmd);
    if (n && n->is_dir) { kprintf("%s: %s: is a directory\n", cmd, p); return 0; }
    return n;
}
static int contains(const char *hay, const char *needle) {
    if (!*needle) return 1;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return 1;
    }
    return 0;
}
static void put_n(const uint8_t *d, uint32_t n) { while (n--) kputc((char)*d++); }
static int is_printable(uint8_t c) { return (c >= 32 && c < 127) || c == '\n' || c == '\t' || c == '\r'; }

/* ------------------------------- ls -------------------------------------- */
int cmd_ls(int argc, char **argv) {
    fs_node *d = (argc > 1) ? fs_lookup(argv[1]) : fs_cwd();
    if (!d) { kprintf("ls: %s: not found\n", argv[1]); return 1; }
    if (!d->is_dir) {
        char sz[12]; sh_human(d->size, sz);
        kprintf("- "); sh_pad(sz, 8); kprintf(" %s\n", d->name);
        return 0;
    }
    int n = 0;
    for (fs_node *c = d->child; c; c = c->next) {
        char sz[12];
        if (c->is_dir) { kprintf("d "); sh_pad("<dir>", 8); }
        else           { sh_human(c->size, sz); kprintf("- "); sh_pad(sz, 8); }
        kprintf(" %s%s\n", c->name, c->is_dir ? "/" : "");
        n++;
    }
    if (!n) kprintf("(empty)\n");
    return 0;
}

/* ------------------------------ tree ------------------------------------- */
static void tree_rec(fs_node *d, int depth) {
    if (depth >= FS_MAX_DEPTH) return;
    for (fs_node *c = d->child; c; c = c->next) {
        for (int i = 0; i < depth; i++) kprintf("  ");
        kprintf("%s%s\n", c->name, c->is_dir ? "/" : "");
        if (c->is_dir) tree_rec(c, depth + 1);
    }
}
int cmd_tree(int argc, char **argv) {
    fs_node *d = (argc > 1) ? fs_lookup(argv[1]) : fs_cwd();
    if (!d) { kprintf("tree: not found\n"); return 1; }
    char path[FS_PATH_MAX]; fs_abspath(d, path, sizeof(path));
    kprintf("%s\n", path);
    if (d->is_dir) tree_rec(d, 1);
    return 0;
}

/* ------------------------------- cd -------------------------------------- */
int cmd_cd(int argc, char **argv) {
    const char *p = (argc > 1) ? argv[1] : "/";
    if (fs_chdir(p) != 0) { kprintf("cd: %s: not a directory\n", p); return 1; }
    return 0;
}
int cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char path[FS_PATH_MAX]; fs_abspath(fs_cwd(), path, sizeof(path));
    kprintf("%s\n", path);
    return 0;
}

/* ------------------------------ mkdir ------------------------------------ */
int cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: mkdir DIR...\n"); return 1; }
    for (int i = 1; i < argc; i++)
        if (!fs_create(argv[i], 1)) kprintf("mkdir: %s: cannot create\n", argv[i]);
    return 0;
}

/* ------------------------------- rm -------------------------------------- */
int cmd_rm(int argc, char **argv) {
    int rec = 0, first = 1;
    if (argc > 1 && strcmp(argv[1], "-r") == 0) { rec = 1; first = 2; }
    if (argc <= first) { kprintf("usage: rm [-r] PATH...\n"); return 1; }
    for (int i = first; i < argc; i++) {
        fs_node *n = need(argv[i], "rm");
        if (!n) continue;
        if (n->is_dir && n->child && !rec) {
            kprintf("rm: %s: directory not empty (use -r)\n", argv[i]);
            continue;
        }
        fs_remove_node(n);
    }
    return 0;
}

/* ------------------------------- cp -------------------------------------- */
int cmd_cp(int argc, char **argv) {
    if (argc != 3) { kprintf("usage: cp SRC DST\n"); return 1; }
    fs_node *src = need_file(argv[1], "cp");
    if (!src) return 1;

    fs_node *d = fs_lookup(argv[2]);
    fs_node *dst;
    if (d && d->is_dir) {                     /* cp file dir/ -> dir/srcname */
        char full[FS_PATH_MAX];
        strncpy(full, argv[2], sizeof(full));
        uint32_t L = (uint32_t)strlen(full);
        if (L && full[L - 1] != '/' && L + 1 < sizeof(full)) { full[L++] = '/'; full[L] = 0; }
        for (const char *s = src->name; *s && L + 1 < sizeof(full); ) full[L++] = *s++;
        full[L] = 0;
        dst = fs_create(full, 0);
        if (!dst) dst = fs_lookup(full);
    } else if (d && !d->is_dir) {
        dst = d;                              /* overwrite existing file */
    } else {
        dst = fs_create(argv[2], 0);
    }
    if (!dst || dst->is_dir) { kprintf("cp: cannot create %s\n", argv[2]); return 1; }
    fs_write(dst, src->data, src->size);
    return 0;
}

/* ------------------------------- mv -------------------------------------- */
int cmd_mv(int argc, char **argv) {
    if (argc != 3) { kprintf("usage: mv SRC DST\n"); return 1; }
    fs_node *src = need(argv[1], "mv");
    if (!src) return 1;

    fs_node *d = fs_lookup(argv[2]);
    if (d && d->is_dir) {                      /* move into directory */
        if (fs_move(src, d, src->name) != 0) { kprintf("mv: failed\n"); return 1; }
        return 0;
    }
    char leaf[FS_NAME_MAX];
    fs_node *parent = fs_parent_of(argv[2], leaf, sizeof(leaf));
    if (!parent) { kprintf("mv: %s: bad destination\n", argv[2]); return 1; }
    if (fs_move(src, parent, leaf) != 0) { kprintf("mv: failed\n"); return 1; }
    return 0;
}

/* ------------------------------ find ------------------------------------- */
static void find_rec(fs_node *d, const char *pat) {
    for (fs_node *c = d->child; c; c = c->next) {
        if (contains(c->name, pat)) {
            char path[FS_PATH_MAX]; fs_abspath(c, path, sizeof(path));
            kprintf("%s%s\n", path, c->is_dir ? "/" : "");
        }
        if (c->is_dir) find_rec(c, pat);
    }
}
int cmd_find(int argc, char **argv) {
    const char *pat; fs_node *start;
    if (argc >= 3)      { start = fs_lookup(argv[1]); pat = argv[2]; }
    else if (argc == 2) { start = fs_cwd();           pat = argv[1]; }
    else { kprintf("usage: find [DIR] PATTERN\n"); return 1; }
    if (!start) { kprintf("find: start dir not found\n"); return 1; }
    find_rec(start, pat);
    return 0;
}

/* ----------------------------- trash ------------------------------------- */
int cmd_trash(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: trash PATH...\n"); return 1; }
    fs_node *tr = fs_lookup("/.trash");
    if (!tr) tr = fs_create("/.trash", 1);
    if (!tr) { kprintf("trash: cannot create /.trash\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        fs_node *n = need(argv[i], "trash");
        if (!n) continue;
        char orig[FS_TRASH_MAX]; fs_abspath(n, orig, sizeof(orig));
        char name[FS_NAME_MAX]; strncpy(name, n->name, sizeof(name));
        /* avoid name clash inside /.trash */
        int suffix = 1;
        while (fs_child(tr, name)) {
            char base[FS_NAME_MAX]; strncpy(base, n->name, sizeof(base));
            char num[8]; sh_utoa((uint64_t)suffix++, num);
            strncpy(name, base, sizeof(name));
            kstrlcat(name, "~", FS_NAME_MAX); kstrlcat(name, num, FS_NAME_MAX);
        }
        if (fs_move(n, tr, name) == 0) {
            strncpy(n->tpath, orig, sizeof(n->tpath));
            kprintf("trashed %s\n", orig);
        }
    }
    return 0;
}
int cmd_recover(int argc, char **argv) {
    fs_node *tr = fs_lookup("/.trash");
    if (!tr || !tr->child) { kprintf("recover: trash is empty\n"); return 0; }
    if (argc < 2) {
        kprintf("trash contents (recover NAME to restore):\n");
        for (fs_node *c = tr->child; c; c = c->next)
            kprintf("  %s  <- %s\n", c->name, c->tpath[0] ? c->tpath : "?");
        return 0;
    }
    fs_node *n = fs_child(tr, argv[1]);
    if (!n) { kprintf("recover: %s: not in trash\n", argv[1]); return 1; }
    if (!n->tpath[0]) { kprintf("recover: unknown original path\n"); return 1; }
    char leaf[FS_NAME_MAX];
    fs_node *parent = fs_parent_of(n->tpath, leaf, sizeof(leaf));
    if (!parent || fs_move(n, parent, leaf) != 0) { kprintf("recover: failed\n"); return 1; }
    kprintf("restored %s\n", n->tpath);
    n->tpath[0] = 0;
    return 0;
}

/* ----------------------------- touch ------------------------------------- */
int cmd_touch(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: touch FILE...\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        fs_node *n = fs_lookup(argv[i]);
        if (!n) { if (!fs_create(argv[i], 0)) kprintf("touch: %s: cannot create\n", argv[i]); }
        else      n->mtime = pit_ticks();
    }
    return 0;
}

/* ----------------------------- write ------------------------------------- */
int cmd_write(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: write FILE [TEXT...]\n"); return 1; }
    fs_node *n = fs_lookup(argv[1]);
    if (!n) n = fs_create(argv[1], 0);
    if (!n || n->is_dir) { kprintf("write: %s: not a writable file\n", argv[1]); return 1; }

    char buf[512]; uint32_t len = 0;
    for (int i = 2; i < argc; i++) {
        if (i > 2 && len < sizeof(buf) - 1) buf[len++] = ' ';
        for (const char *s = argv[i]; *s && len < sizeof(buf) - 1; ) buf[len++] = *s++;
    }
    if (len < sizeof(buf) - 1) buf[len++] = '\n';
    fs_write(n, buf, len);
    return 0;
}

/* =========================== File Inspection ============================== */

int cmd_cat(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: cat FILE...\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        fs_node *n = need_file(argv[i], "cat");
        if (n) put_n(n->data, n->size);
    }
    return 0;
}

int cmd_head(int argc, char **argv) {
    int lines = 10, fi = 1;
    if (argc > 1 && argv[1][0] == '-') { lines = atoi(&argv[1][1]); fi = 2; }
    if (argc <= fi) { kprintf("usage: head [-N] FILE\n"); return 1; }
    fs_node *n = need_file(argv[fi], "head");
    if (!n) return 1;
    int shown = 0;
    for (uint32_t i = 0; i < n->size && shown < lines; i++) {
        kputc((char)n->data[i]);
        if (n->data[i] == '\n') shown++;
    }
    if (n->size && n->data[n->size - 1] != '\n') kputc('\n');
    return 0;
}

int cmd_tail(int argc, char **argv) {
    int lines = 10, fi = 1;
    if (argc > 1 && argv[1][0] == '-') { lines = atoi(&argv[1][1]); fi = 2; }
    if (argc <= fi) { kprintf("usage: tail [-N] FILE\n"); return 1; }
    fs_node *n = need_file(argv[fi], "tail");
    if (!n) return 1;
    int total = 0;
    for (uint32_t i = 0; i < n->size; i++) if (n->data[i] == '\n') total++;
    int skip = total - lines; if (skip < 0) skip = 0;
    int seen = 0;
    uint32_t i = 0;
    for (; i < n->size && seen < skip; i++) if (n->data[i] == '\n') seen++;
    put_n(n->data + i, n->size - i);
    if (n->size && n->data[n->size - 1] != '\n') kputc('\n');
    return 0;
}

int cmd_hex(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: hex FILE\n"); return 1; }
    fs_node *n = need_file(argv[1], "hex");
    if (!n) return 1;
    for (uint32_t off = 0; off < n->size; off += 16) {
        kprintf("%x: ", off);   /* offset */
        for (uint32_t j = 0; j < 16; j++) {
            if (off + j < n->size) kprintf("%x%x ", n->data[off + j] >> 4, n->data[off + j] & 0xF);
            else kprintf("   ");
        }
        kprintf(" ");
        for (uint32_t j = 0; j < 16 && off + j < n->size; j++) {
            uint8_t c = n->data[off + j];
            kputc((c >= 32 && c < 127) ? (char)c : '.');
        }
        kputc('\n');
    }
    if (!n->size) kprintf("(empty)\n");
    return 0;
}

int cmd_meta(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: meta PATH\n"); return 1; }
    fs_node *n = need(argv[1], "meta");
    if (!n) return 1;
    char path[FS_PATH_MAX]; fs_abspath(n, path, sizeof(path));
    char sz[12]; sh_human(n->size, sz);
    kprintf("name : %s\n", n->name);
    kprintf("path : %s\n", path);
    kprintf("type : %s\n", n->is_dir ? "directory" : "file");
    if (!n->is_dir) kprintf("size : %lu bytes (%s)\n", (unsigned long)n->size, sz);
    else {
        int kids = 0; for (fs_node *c = n->child; c; c = c->next) kids++;
        kprintf("items: %d\n", kids);
    }
    kprintf("mtime: %lu ticks (~%lus uptime)\n",
            (unsigned long)n->mtime, (unsigned long)(n->mtime / (pit_hz() ? pit_hz() : 1000)));
    return 0;
}

int cmd_diff(int argc, char **argv) {
    if (argc != 3) { kprintf("usage: diff A B\n"); return 1; }
    fs_node *a = need_file(argv[1], "diff"), *b = need_file(argv[2], "diff");
    if (!a || !b) return 1;
    uint32_t ia = 0, ib = 0; int ln = 1, diffs = 0;
    while (ia < a->size || ib < b->size) {
        uint32_t la = ia, lb = ib;
        while (la < a->size && a->data[la] != '\n') la++;
        while (lb < b->size && b->data[lb] != '\n') lb++;
        uint32_t na = la - ia, nb = lb - ib;
        int same = (na == nb) && (memcmp(a->data + ia, b->data + ib, na) == 0);
        if (!same) {
            diffs++;
            kprintf("%d< ", ln); put_n(a->data + ia, na); kputc('\n');
            kprintf("%d> ", ln); put_n(b->data + ib, nb); kputc('\n');
        }
        ia = (la < a->size) ? la + 1 : la;
        ib = (lb < b->size) ? lb + 1 : lb;
        ln++;
    }
    if (!diffs) kprintf("files are identical\n");
    return 0;
}

int cmd_grep(int argc, char **argv) {
    if (argc < 3) { kprintf("usage: grep PATTERN FILE\n"); return 1; }
    fs_node *n = need_file(argv[2], "grep");
    if (!n) return 1;
    char line[256]; int ln = 1, hits = 0;
    uint32_t i = 0;
    while (i < n->size) {
        uint32_t j = 0;
        while (i < n->size && n->data[i] != '\n' && j < sizeof(line) - 1) line[j++] = (char)n->data[i++];
        while (i < n->size && n->data[i] != '\n') i++;     /* skip overlong */
        if (i < n->size) i++;                              /* eat newline */
        line[j] = 0;
        if (contains(line, argv[1])) { kprintf("%d: %s\n", ln, line); hits++; }
        ln++;
    }
    if (!hits) kprintf("(no matches)\n");
    return 0;
}

int cmd_checksum(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: checksum FILE\n"); return 1; }
    fs_node *n = need_file(argv[1], "checksum");
    if (!n) return 1;
    uint64_t h = 1469598103934665603ull;                   /* FNV-1a 64 */
    for (uint32_t i = 0; i < n->size; i++) {
        h ^= n->data[i];
        h *= 1099511628211ull;
    }
    kprintf("fnv1a64  %lx  %s  (%lu bytes)\n",
            (unsigned long)h, n->name, (unsigned long)n->size);
    return 0;
}

int cmd_count(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: count FILE\n"); return 1; }
    fs_node *n = need_file(argv[1], "count");
    if (!n) return 1;
    uint32_t lines = 0, words = 0, chars = n->size; int inword = 0;
    for (uint32_t i = 0; i < n->size; i++) {
        uint8_t c = n->data[i];
        if (c == '\n') lines++;
        if (c == ' ' || c == '\n' || c == '\t') inword = 0;
        else if (!inword) { inword = 1; words++; }
    }
    kprintf("lines %lu  words %lu  chars %lu  %s\n",
            (unsigned long)lines, (unsigned long)words, (unsigned long)chars, n->name);
    return 0;
}

int cmd_preview(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: preview FILE\n"); return 1; }
    fs_node *n = need(argv[1], "preview");
    if (!n) return 1;
    if (n->is_dir) { kprintf("[dir] %s\n", n->name); return cmd_ls(argc, argv); }
    int binary = 0;
    uint32_t scan = n->size < 256 ? n->size : 256;
    for (uint32_t i = 0; i < scan; i++) if (!is_printable(n->data[i])) { binary = 1; break; }
    char sz[12]; sh_human(n->size, sz);
    kprintf("[file] %s  %s  %s\n", n->name, sz, binary ? "(binary)" : "(text)");
    kprintf("------------------------------------\n");
    if (binary) {
        char *av[2] = { (char *)"hex", argv[1] };
        return cmd_hex(2, av);
    }
    int shown = 0;
    for (uint32_t i = 0; i < n->size && shown < 10; i++) {
        kputc((char)n->data[i]);
        if (n->data[i] == '\n') shown++;
    }
    if (n->size && n->data[n->size - 1] != '\n') kputc('\n');
    return 0;
}
