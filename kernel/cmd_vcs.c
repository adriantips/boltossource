#include <stdint.h>
#include "commands.h"
#include "fs.h"
#include "users.h"
#include "hw.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include <stdio.h>

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_vcs.c
 *  BoltVCS: local version control.
 *
 *  A machine you can write software on is not one you can only write software
 *  on once. Without history there is no way to try something, see that it was
 *  wrong and get back what worked -- which is most of what writing software
 *  actually consists of. `snapshot` already copied the whole filesystem, but a
 *  whole-machine snapshot is not a project's history: it cannot say what
 *  changed, or why, or restore one directory without restoring everything.
 *
 *  The model is the one everybody already knows, kept to what it needs:
 *
 *      .bvcs/objects/<hash>   file contents, addressed by their FNV-1a hash,
 *                             so an unchanged file is stored once no matter
 *                             how many commits mention it
 *      .bvcs/commits/<n>      a commit: parent, branch, author, time, message
 *                             and the (hash, size, path) of every file in it
 *      .bvcs/refs/<branch>    the commit a branch points at
 *      .bvcs/HEAD             the branch that is checked out
 *      .bvcs/index            what `add` has staged for the next commit
 *
 *  Commits are numbered rather than named by hash. A content hash buys
 *  distributed identity, and nothing here is distributed; a number you can type
 *  is worth more than one you cannot.
 *
 *  The repository lives in the filesystem, so it is committed to disk by the
 *  same atomic mechanism as everything else and survives a power cut with the
 *  rest of the tree.
 * ===========================================================================*/

#define VCS_DIR   ".bvcs"
#define MAXENT    64            /* files per commit                          */
#define PATHMAX   224
#define NAMEMAX   128           /* path relative to the repository root       */
#define MAXDIFF   800           /* lines per side the LCS diff will attempt   */

typedef struct { char hash[17]; uint32_t size; char path[NAMEMAX]; } entry_t;

typedef struct {
    int     parent;
    char    branch[32];
    char    author[32];
    char    when[24];
    char    msg[128];
    entry_t ent[MAXENT];
    int     nent;
} commit_t;

/* ------------------------------------------------------------- utilities --- */
static void sset(char *dst, uint32_t cap, const char *src) {
    uint32_t i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}
static void put_n(const uint8_t *p, uint32_t n) { for (uint32_t i = 0; i < n; i++) kputc((char)p[i]); }

/* FNV-1a, 64-bit, printed as 16 hex digits. Not a cryptographic hash and not
 * pretending to be one: it is here to notice that a file changed, and to let an
 * unchanged file share one copy in the object store. */
static void hash_hex(const void *data, uint32_t len, char *out17) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ull; }
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 16; i++) out17[i] = H[(h >> ((15 - i) * 4)) & 0xF];
    out17[16] = 0;
}

static fs_node *rd(const char *path) {
    fs_node *n = fs_lookup(path);
    if (!n || n->is_dir || !fs_may(n, FS_PERM_READ)) return 0;
    return n;
}
static int wr(const char *path, const void *data, uint32_t len) {
    fs_node *n = fs_lookup(path);
    if (!n) n = fs_create(path, 0);
    if (!n || !fs_may(n, FS_PERM_WRITE)) return -1;
    return fs_write(n, data, len);
}
static int wr_str(const char *path, const char *s) { return wr(path, s, (uint32_t)strlen(s)); }

/* Read a whole file into a kmalloc'd, NUL-terminated buffer. */
static char *slurp(const char *path, uint32_t *len_out) {
    fs_node *n = rd(path);
    if (!n) return 0;
    char *b = (char *)kmalloc(n->size + 1);
    if (!b) return 0;
    if (n->data && n->size) memcpy(b, n->data, n->size);
    b[n->size] = 0;
    if (len_out) *len_out = n->size;
    return b;
}

/* -------------------------------------------------------- repo locations --- */
static char g_root[PATHMAX];         /* the directory holding .bvcs           */

/* The root is the one path that already ends in its separator, and a "//" in
 * the middle of a lookup is not worth risking. */
static void wpath(char *out, uint32_t cap, const char *rel) {
    if (g_root[0] == '/' && !g_root[1]) snprintf(out, cap, "/%s", rel);
    else                                snprintf(out, cap, "%s/%s", g_root, rel);
}
static void vpath(char *out, uint32_t cap, const char *fmt, const char *a) {
    char rel[96];
    if (a) snprintf(rel, sizeof rel, VCS_DIR "/%s%s", fmt, a);
    else   snprintf(rel, sizeof rel, VCS_DIR "/%s", fmt);
    wpath(out, cap, rel);
}
/* The part of an absolute path that is inside the repository, or 0. */
static const char *repo_rel(const char *full) {
    int rl = (int)strlen(g_root);
    if (g_root[0] == '/' && !g_root[1]) return full[0] == '/' ? full + 1 : 0;
    if (strncmp(full, g_root, rl) || full[rl] != '/') return 0;
    return full + rl + 1;
}

/* Walk up from the working directory looking for a .bvcs, the way every tool
 * of this kind does: a repository is a place, and you are usually somewhere
 * inside it rather than exactly at its root. */
static int repo_find(void) {
    char cur[PATHMAX];
    fs_abspath(fs_cwd(), cur, sizeof cur);
    for (;;) {
        char probe[PATHMAX];
        snprintf(probe, sizeof probe, "%s/" VCS_DIR, cur[1] ? cur : "");
        fs_node *n = fs_lookup(probe);
        if (n && n->is_dir) { sset(g_root, PATHMAX, cur[1] ? cur : "/"); return 0; }
        if (!cur[1]) break;                       /* reached "/" */
        int i = (int)strlen(cur) - 1;
        while (i > 0 && cur[i] != '/') i--;
        cur[i ? i : 1] = 0;
    }
    return -1;
}

/* ------------------------------------------------------------ HEAD / refs --- */
static void head_branch(char *out, uint32_t cap) {
    char p[PATHMAX];
    vpath(p, sizeof p, "HEAD", 0);
    uint32_t len = 0;
    char *b = slurp(p, &len);
    if (!b) { sset(out, cap, "main"); return; }
    uint32_t i = 0;
    while (i < len && b[i] != '\n' && i < cap - 1) { out[i] = b[i]; i++; }
    out[i] = 0;
    kfree(b);
    if (!out[0]) sset(out, cap, "main");
}
static int ref_get(const char *branch) {
    char p[PATHMAX];
    vpath(p, sizeof p, "refs/", branch);
    uint32_t len = 0;
    char *b = slurp(p, &len);
    if (!b) return -1;
    int v = atoi(b);
    kfree(b);
    return v;
}
static void ref_set(const char *branch, int commit) {
    char p[PATHMAX], v[16];
    vpath(p, sizeof p, "refs/", branch);
    snprintf(v, sizeof v, "%d\n", commit);
    wr_str(p, v);
}
static int head_commit(void) {
    char b[32];
    head_branch(b, sizeof b);
    return ref_get(b);
}

/* ------------------------------------------------------------ the index ---- */
static int index_load(entry_t *e, int max) {
    char p[PATHMAX];
    vpath(p, sizeof p, "index", 0);
    uint32_t len = 0;
    char *b = slurp(p, &len);
    if (!b) return 0;
    int n = 0;
    char *s = b;
    while (*s && n < max) {
        char *eol = s;
        while (*eol && *eol != '\n') eol++;
        char save = *eol; *eol = 0;
        /* "<hash> <size> <path>" */
        if (strlen(s) > 18) {
            memcpy(e[n].hash, s, 16); e[n].hash[16] = 0;
            const char *q = s + 17;
            e[n].size = (uint32_t)atoi(q);
            while (*q && *q != ' ') q++;
            if (*q == ' ') q++;
            sset(e[n].path, NAMEMAX, q);
            if (e[n].path[0]) n++;
        }
        *eol = save;
        s = *eol ? eol + 1 : eol;
    }
    kfree(b);
    return n;
}
static void index_save(const entry_t *e, int n) {
    uint32_t cap = (uint32_t)n * (NAMEMAX + 32) + 16;
    char *b = (char *)kmalloc(cap);
    if (!b) return;
    uint32_t o = 0;
    for (int i = 0; i < n; i++) {
        int k = snprintf(b + o, cap - o, "%s %u %s\n", e[i].hash, e[i].size, e[i].path);
        if (k > 0) { o += (uint32_t)k; if (o > cap - 1) { o = cap - 1; break; } }
    }
    char p[PATHMAX];
    vpath(p, sizeof p, "index", 0);
    wr(p, b, o);
    kfree(b);
}
static int ent_find(const entry_t *e, int n, const char *path) {
    for (int i = 0; i < n; i++) if (!strcmp(e[i].path, path)) return i;
    return -1;
}

/* ---------------------------------------------------------- object store --- */
static int object_put(const void *data, uint32_t len, char *hash17) {
    hash_hex(data, len, hash17);
    char p[PATHMAX];
    vpath(p, sizeof p, "objects/", hash17);
    if (fs_lookup(p)) return 0;                  /* already stored */
    return wr(p, data, len);
}
static fs_node *object_get(const char *hash) {
    char p[PATHMAX];
    vpath(p, sizeof p, "objects/", hash);
    return rd(p);
}

/* --------------------------------------------------------------- commits --- */
static int commit_load(int n, commit_t *c) {
    if (n < 0) return -1;
    char p[PATHMAX], num[16];
    snprintf(num, sizeof num, "%d", n);
    vpath(p, sizeof p, "commits/", num);
    uint32_t len = 0;
    char *b = slurp(p, &len);
    if (!b) return -1;

    memset(c, 0, sizeof *c);
    c->parent = -1;
    char *s = b;
    while (*s) {
        char *eol = s;
        while (*eol && *eol != '\n') eol++;
        char save = *eol; *eol = 0;
        if      (!strncmp(s, "parent ", 7)) c->parent = atoi(s + 7);
        else if (!strncmp(s, "branch ", 7)) sset(c->branch, sizeof c->branch, s + 7);
        else if (!strncmp(s, "author ", 7)) sset(c->author, sizeof c->author, s + 7);
        else if (!strncmp(s, "when ",   5)) sset(c->when,   sizeof c->when,   s + 5);
        else if (!strncmp(s, "msg ",    4)) sset(c->msg,    sizeof c->msg,    s + 4);
        else if (!strncmp(s, "file ",   5) && c->nent < MAXENT) {
            const char *q = s + 5;
            if (strlen(q) > 18) {
                entry_t *e = &c->ent[c->nent];
                memcpy(e->hash, q, 16); e->hash[16] = 0;
                e->size = (uint32_t)atoi(q + 17);
                q += 17;
                while (*q && *q != ' ') q++;
                if (*q == ' ') q++;
                sset(e->path, NAMEMAX, q);
                if (e->path[0]) c->nent++;
            }
        }
        *eol = save;
        s = *eol ? eol + 1 : eol;
    }
    kfree(b);
    return 0;
}

static int commit_save(int n, const commit_t *c) {
    uint32_t cap = (uint32_t)c->nent * (NAMEMAX + 32) + 512;
    char *b = (char *)kmalloc(cap);
    if (!b) return -1;
    /* snprintf reports the length it *wanted*, so every advance is clamped:
     * letting `o` run past `cap` would turn the next `cap - o` into a very
     * large size_t and the write into a very bad idea. */
    uint32_t o = 0;
    #define PUT(...) do { int k_ = snprintf(b + o, cap - o, __VA_ARGS__); \
                          if (k_ > 0) o += (uint32_t)k_;                  \
                          if (o > cap - 1) o = cap - 1; } while (0)
    PUT("parent %d\n", c->parent);
    PUT("branch %s\n", c->branch);
    PUT("author %s\n", c->author);
    PUT("when %s\n", c->when);
    PUT("msg %s\n", c->msg);
    for (int i = 0; i < c->nent && o < cap - 1; i++)
        PUT("file %s %u %s\n", c->ent[i].hash, c->ent[i].size, c->ent[i].path);
    #undef PUT
    char p[PATHMAX], num[16];
    snprintf(num, sizeof num, "%d", n);
    vpath(p, sizeof p, "commits/", num);
    int rc = wr(p, b, o);
    kfree(b);
    return rc;
}

/* ------------------------------------------------- the working tree walk --- */
/* Every file under the repository root, as paths relative to it. The .bvcs
 * directory is skipped: a repository that tracked itself would grow without
 * bound, one object per object. */
static void walk(fs_node *dir, const char *prefix, entry_t *out, int *n, int max) {
    for (fs_node *c = dir->child; c; c = c->next) {
        if (!strcmp(c->name, VCS_DIR)) continue;
        char rel[NAMEMAX];
        if (prefix[0]) snprintf(rel, sizeof rel, "%s/%s", prefix, c->name);
        else           snprintf(rel, sizeof rel, "%s", c->name);
        if (c->is_dir) { walk(c, rel, out, n, max); continue; }
        if (*n >= max) return;
        sset(out[*n].path, NAMEMAX, rel);
        out[*n].size = c->size;
        hash_hex(c->data ? c->data : (const uint8_t *)"", c->size, out[*n].hash);
        (*n)++;
    }
}
static int work_scan(entry_t *out, int max) {
    fs_node *root = fs_lookup(g_root);
    int n = 0;
    if (root && root->is_dir) walk(root, "", out, &n, max);
    return n;
}

/* Create the directories a relative path needs, then write the file. */
static int restore_file(const char *rel, const char *hash) {
    fs_node *obj = object_get(hash);
    if (!obj) return -1;
    char full[PATHMAX];
    wpath(full, sizeof full, rel);
    /* make intermediate directories */
    int skip = (int)(strlen(full) - strlen(rel));
    for (int i = skip; full[i]; i++) {
        if (full[i] != '/') continue;
        full[i] = 0;
        if (!fs_lookup(full)) fs_create(full, 1);
        full[i] = '/';
    }
    return wr(full, obj->data, obj->size);
}

/* --------------------------------------------------------------- the diff -- */
typedef struct { const uint8_t *p; uint32_t n; } line_t;

static int split_lines(const uint8_t *b, uint32_t len, line_t *out, int max) {
    int n = 0;
    uint32_t i = 0;
    while (i < len && n < max) {
        uint32_t j = i;
        while (j < len && b[j] != '\n') j++;
        out[n].p = b + i; out[n].n = j - i;
        n++;
        i = (j < len) ? j + 1 : j;
    }
    return n;
}
static int line_eq(const line_t *a, const line_t *b) {
    return a->n == b->n && (a->n == 0 || memcmp(a->p, b->p, a->n) == 0);
}
static void put_line(char sign, const line_t *l) {
    kputc(sign); kputc(' ');
    put_n(l->p, l->n);
    kputc('\n');
}

/* A real diff, not a line-by-line compare: an inserted line at the top must not
 * make every line below it look changed. Common head and tail are trimmed first
 * (which is most of a real edit), and the remaining middle goes through an LCS.
 * Past MAXDIFF lines the table is not worth its memory and the file is reported
 * as changed without a body. */
static void diff_text(const uint8_t *a, uint32_t alen, const uint8_t *b, uint32_t blen) {
    line_t *la = (line_t *)kmalloc(sizeof(line_t) * (MAXDIFF + 1));
    line_t *lb = (line_t *)kmalloc(sizeof(line_t) * (MAXDIFF + 1));
    if (!la || !lb) { if (la) kfree(la); if (lb) kfree(lb); kprintf("  (out of memory)\n"); return; }

    int na = split_lines(a, alen, la, MAXDIFF + 1);
    int nb = split_lines(b, blen, lb, MAXDIFF + 1);
    if (na > MAXDIFF || nb > MAXDIFF) {
        kprintf("  (file too large to diff line by line: %d vs %d lines)\n", na, nb);
        kfree(la); kfree(lb);
        return;
    }

    int lo = 0;
    while (lo < na && lo < nb && line_eq(&la[lo], &lb[lo])) lo++;
    int ta = na, tb = nb;
    while (ta > lo && tb > lo && line_eq(&la[ta-1], &lb[tb-1])) { ta--; tb--; }

    int m = ta - lo, n = tb - lo;
    if (m == 0 && n == 0) { kfree(la); kfree(lb); return; }

    /* LCS length table over the changed middle only. Still O(m*n) uint16_t's --
     * fine at MAXDIFF=800 (~1.2MB worst case), not something to raise casually. */
    uint16_t *L = (uint16_t *)kmalloc((uint64_t)(m + 1) * (n + 1) * sizeof(uint16_t));
    if (!L) { kprintf("  (out of memory)\n"); kfree(la); kfree(lb); return; }
    for (int i = m; i >= 0; i--)
        for (int j = n; j >= 0; j--) {
            uint16_t v;
            if (i == m || j == n)                       v = 0;
            else if (line_eq(&la[lo+i], &lb[lo+j]))     v = (uint16_t)(L[(i+1)*(n+1) + (j+1)] + 1);
            else {
                uint16_t d = L[(i+1)*(n+1) + j], r = L[i*(n+1) + (j+1)];
                v = d > r ? d : r;
            }
            L[i*(n+1) + j] = v;
        }

    int i = 0, j = 0;
    while (i < m && j < n) {
        if (line_eq(&la[lo+i], &lb[lo+j])) { i++; j++; continue; }
        if (L[(i+1)*(n+1) + j] >= L[i*(n+1) + (j+1)]) { put_line('-', &la[lo+i]); i++; }
        else                                          { put_line('+', &lb[lo+j]); j++; }
    }
    while (i < m) { put_line('-', &la[lo+i]); i++; }
    while (j < n) { put_line('+', &lb[lo+j]); j++; }

    kfree(L); kfree(la); kfree(lb);
}

/* ------------------------------------------------------------- commands ---- */
static void usage(void) {
    kprintf("usage: vcs COMMAND [args]\n"
            "  init                 start a repository in the working directory\n"
            "  add FILE... | .      stage files (`.` stages everything below the root)\n"
            "  status               what is staged, changed, or untracked\n"
            "  commit -m MESSAGE    record the staged files as a commit\n"
            "  log [-n N]           the current branch's history, newest first\n"
            "  show N               one commit: its message and its files\n"
            "  diff [FILE]          working tree against the last commit\n"
            "  checkout N | BRANCH  restore a commit's files, or switch branch\n"
            "  branch [NAME]        list branches, or start one here and switch to it\n");
    /* TODO: no `merge`. checkout just overwrites the working tree, so two
     * branches that touched the same file have to be reconciled by hand
     * (diff one against the other, edit, commit). Fine for the one-person
     * case this was built for; would need real thought for anything else. */
}

static void now_string(char *out, uint32_t cap) {
    struct rtc_time t;
    rtc_now(&t);
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d",
             t.year, t.mon, t.day, t.hour, t.min, t.sec);
}

static int vcs_init(void) {
    char here[PATHMAX];
    fs_abspath(fs_cwd(), here, sizeof here);
    char p[PATHMAX];
    snprintf(p, sizeof p, "%s/" VCS_DIR, here[1] ? here : "");
    if (fs_lookup(p)) { kprintf("vcs: a repository already exists here\n"); return 1; }
    if (!fs_create(p, 1)) { kprintf("vcs: cannot create %s\n", p); return 1; }
    sset(g_root, PATHMAX, here[1] ? here : "/");

    char q[PATHMAX];
    vpath(q, sizeof q, "objects", 0); fs_create(q, 1);
    vpath(q, sizeof q, "commits", 0); fs_create(q, 1);
    vpath(q, sizeof q, "refs",    0); fs_create(q, 1);
    vpath(q, sizeof q, "HEAD",    0); wr_str(q, "main\n");
    ref_set("main", -1);
    vpath(q, sizeof q, "index",   0); wr_str(q, "");
    fs_sync();
    kprintf("vcs: repository in %s on branch main\n", g_root);
    return 0;
}

static int vcs_add(int argc, char **argv) {
    entry_t *idx = (entry_t *)kmalloc(sizeof(entry_t) * MAXENT);
    entry_t *work = (entry_t *)kmalloc(sizeof(entry_t) * MAXENT);
    if (!idx || !work) { if (idx) kfree(idx); if (work) kfree(work); kprintf("vcs: out of memory\n"); return 1; }
    int n = index_load(idx, MAXENT);
    int added = 0, failed = 0;

    for (int a = 2; a < argc; a++) {
        int nw = 0;
        entry_t one;
        entry_t *list; int count;

        if (!strcmp(argv[a], ".")) {
            nw = work_scan(work, MAXENT);
            list = work; count = nw;
        } else {
            /* a path, relative to the working directory or absolute */
            char full[PATHMAX];
            if (argv[a][0] == '/') snprintf(full, sizeof full, "%s", argv[a]);
            else {
                char here[PATHMAX];
                fs_abspath(fs_cwd(), here, sizeof here);
                snprintf(full, sizeof full, "%s/%s", here[1] ? here : "", argv[a]);
            }
            fs_node *f = rd(full);
            if (!f) { kprintf("vcs: cannot read %s\n", argv[a]); failed = 1; continue; }
            const char *rel = repo_rel(full);
            if (!rel || !rel[0]) {
                kprintf("vcs: %s is outside the repository\n", argv[a]);
                failed = 1;
                continue;
            }
            sset(one.path, NAMEMAX, rel);
            one.size = f->size;
            hash_hex(f->data ? f->data : (const uint8_t *)"", f->size, one.hash);
            list = &one; count = 1;
        }

        for (int i = 0; i < count; i++) {
            char full[PATHMAX];
            wpath(full, sizeof full, list[i].path);
            fs_node *f = rd(full);
            if (!f) continue;
            char h[17];
            if (object_put(f->data ? f->data : (const uint8_t *)"", f->size, h) != 0) {
                kprintf("vcs: cannot store %s\n", list[i].path);
                failed = 1;
                continue;
            }
            int at = ent_find(idx, n, list[i].path);
            if (at < 0) {
                if (n >= MAXENT) { kprintf("vcs: too many files (max %d)\n", MAXENT); failed = 1; break; }
                at = n++;
                sset(idx[at].path, NAMEMAX, list[i].path);
            }
            sset(idx[at].hash, 17, h);
            idx[at].size = f->size;
            added++;
        }
    }

    index_save(idx, n);
    fs_sync();
    kprintf("vcs: staged %d file%s (%d tracked)\n", added, added == 1 ? "" : "s", n);
    kfree(idx); kfree(work);
    return failed ? 1 : 0;
}

static int vcs_commit(int argc, char **argv) {
    const char *msg = 0;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "-m") && i + 1 < argc) msg = argv[++i];
    if (!msg) { kprintf("usage: vcs commit -m \"a message\"\n"); return 1; }

    commit_t *c = (commit_t *)kmalloc(sizeof(commit_t));
    if (!c) { kprintf("vcs: out of memory\n"); return 1; }
    memset(c, 0, sizeof *c);
    c->nent = index_load(c->ent, MAXENT);
    if (!c->nent) { kprintf("vcs: nothing staged -- `vcs add` first\n"); kfree(c); return 1; }

    char br[32];
    head_branch(br, sizeof br);
    int parent = ref_get(br);

    /* A commit that changes nothing is noise in a history someone has to read. */
    if (parent >= 0) {
        commit_t *p = (commit_t *)kmalloc(sizeof(commit_t));
        if (p && commit_load(parent, p) == 0 && p->nent == c->nent) {
            int same = 1;
            for (int i = 0; i < c->nent && same; i++) {
                int at = ent_find(p->ent, p->nent, c->ent[i].path);
                if (at < 0 || strcmp(p->ent[at].hash, c->ent[i].hash)) same = 0;
            }
            if (same) {
                kprintf("vcs: nothing to commit -- the staged files match commit %d\n", parent);
                kfree(p); kfree(c);
                return 1;
            }
        }
        if (p) kfree(p);
    }

    c->parent = parent;
    sset(c->branch, sizeof c->branch, br);
    sset(c->author, sizeof c->author, users_current()[0] ? users_current() : "nobody");
    now_string(c->when, sizeof c->when);
    sset(c->msg, sizeof c->msg, msg);

    int n = parent + 1;
    /* Numbers are per repository, not per branch, so a branch never reuses one. */
    for (;;) {
        char p[PATHMAX], num[16];
        snprintf(num, sizeof num, "%d", n);
        vpath(p, sizeof p, "commits/", num);
        if (!fs_lookup(p)) break;
        n++;
    }
    if (commit_save(n, c) != 0) { kprintf("vcs: cannot write the commit\n"); kfree(c); return 1; }
    ref_set(br, n);
    fs_sync();
    kprintf("vcs: [%s %d] %s  (%d file%s)\n", br, n, c->msg, c->nent, c->nent == 1 ? "" : "s");
    kfree(c);
    return 0;
}

static int vcs_status(void) {
    entry_t *idx  = (entry_t *)kmalloc(sizeof(entry_t) * MAXENT);
    entry_t *work = (entry_t *)kmalloc(sizeof(entry_t) * MAXENT);
    commit_t *h   = (commit_t *)kmalloc(sizeof(commit_t));
    if (!idx || !work || !h) { if (idx) kfree(idx); if (work) kfree(work); if (h) kfree(h);
                              kprintf("vcs: out of memory\n"); return 1; }
    int ni = index_load(idx, MAXENT);
    int nw = work_scan(work, MAXENT);
    int hc = head_commit();
    int have_head = (hc >= 0 && commit_load(hc, h) == 0);
    if (!have_head) memset(h, 0, sizeof *h);

    char br[32];
    head_branch(br, sizeof br);
    kprintf("on branch %s", br);
    if (hc >= 0) kprintf(", at commit %d", hc);
    else         kprintf(", no commits yet");
    kprintf("\n");

    int quiet = 1;
    for (int i = 0; i < ni; i++) {
        int at = have_head ? ent_find(h->ent, h->nent, idx[i].path) : -1;
        if (at < 0 || strcmp(h->ent[at].hash, idx[i].hash)) {
            if (quiet) { kprintf("staged:\n"); quiet = 0; }
            kprintf("  %-8s %s\n", at < 0 ? "new" : "changed", idx[i].path);
        }
    }
    quiet = 1;
    for (int i = 0; i < ni; i++) {
        int at = ent_find(work, nw, idx[i].path);
        if (at < 0) {
            if (quiet) { kprintf("not staged:\n"); quiet = 0; }
            kprintf("  %-8s %s\n", "deleted", idx[i].path);
        } else if (strcmp(work[at].hash, idx[i].hash)) {
            if (quiet) { kprintf("not staged:\n"); quiet = 0; }
            kprintf("  %-8s %s\n", "modified", idx[i].path);
        }
    }
    quiet = 1;
    for (int i = 0; i < nw; i++) {
        if (ent_find(idx, ni, work[i].path) >= 0) continue;
        if (quiet) { kprintf("untracked:\n"); quiet = 0; }
        kprintf("  %s\n", work[i].path);
    }
    kfree(idx); kfree(work); kfree(h);
    return 0;
}

static int vcs_log(int argc, char **argv) {
    int limit = 20;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "-n") && i + 1 < argc) limit = atoi(argv[++i]);

    int n = head_commit();
    if (n < 0) { kprintf("vcs: no commits yet\n"); return 0; }
    commit_t *c = (commit_t *)kmalloc(sizeof(commit_t));
    if (!c) { kprintf("vcs: out of memory\n"); return 1; }
    int shown = 0;
    while (n >= 0 && shown < limit) {
        if (commit_load(n, c) != 0) break;
        kprintf("commit %d  %s  %s\n", n, c->when, c->author);
        kprintf("    %s\n", c->msg);
        n = c->parent;
        shown++;
    }
    kfree(c);
    return 0;
}

static int vcs_show(int argc, char **argv) {
    if (argc < 3) { kprintf("usage: vcs show N\n"); return 1; }
    commit_t *c = (commit_t *)kmalloc(sizeof(commit_t));
    if (!c) { kprintf("vcs: out of memory\n"); return 1; }
    int n = atoi(argv[2]);
    if (commit_load(n, c) != 0) { kprintf("vcs: no commit %d\n", n); kfree(c); return 1; }
    kprintf("commit %d on %s\n", n, c->branch);
    kprintf("author  %s\n", c->author);
    kprintf("when    %s\n", c->when);
    kprintf("parent  %d\n", c->parent);
    kprintf("message %s\n", c->msg);
    for (int i = 0; i < c->nent; i++)
        kprintf("  %s  %8u  %s\n", c->ent[i].hash, c->ent[i].size, c->ent[i].path);
    kfree(c);
    return 0;
}

static int vcs_diff(int argc, char **argv) {
    int hc = head_commit();
    commit_t *h = (commit_t *)kmalloc(sizeof(commit_t));
    if (!h) { kprintf("vcs: out of memory\n"); return 1; }
    if (hc < 0 || commit_load(hc, h) != 0) { memset(h, 0, sizeof *h); h->parent = -1; }

    const char *only = (argc >= 3) ? argv[2] : 0;
    entry_t *work = (entry_t *)kmalloc(sizeof(entry_t) * MAXENT);
    if (!work) { kfree(h); kprintf("vcs: out of memory\n"); return 1; }
    int nw = work_scan(work, MAXENT);
    int changes = 0;

    /* files present now */
    for (int i = 0; i < nw; i++) {
        if (only && strcmp(work[i].path, only)) continue;
        int at = ent_find(h->ent, h->nent, work[i].path);
        if (at >= 0 && !strcmp(h->ent[at].hash, work[i].hash)) continue;
        char full[PATHMAX];
        wpath(full, sizeof full, work[i].path);
        fs_node *nowf = rd(full);
        if (!nowf) continue;
        changes++;
        if (at < 0) {
            kprintf("--- (new file) %s\n", work[i].path);
            diff_text((const uint8_t *)"", 0, nowf->data, nowf->size);
        } else {
            fs_node *old = object_get(h->ent[at].hash);
            kprintf("--- %s (commit %d)\n+++ %s (working tree)\n", work[i].path, hc, work[i].path);
            diff_text(old ? old->data : (const uint8_t *)"", old ? old->size : 0,
                      nowf->data, nowf->size);
        }
    }
    /* files the commit had and the working tree no longer does */
    for (int i = 0; i < h->nent; i++) {
        if (only && strcmp(h->ent[i].path, only)) continue;
        if (ent_find(work, nw, h->ent[i].path) >= 0) continue;
        changes++;
        kprintf("--- %s (deleted)\n", h->ent[i].path);
    }

    if (!changes) kprintf("vcs: no changes against commit %d\n", hc);
    kfree(work); kfree(h);
    return 0;
}

static int is_number(const char *s) {
    if (!*s) return 0;
    for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}

static int vcs_checkout(int argc, char **argv) {
    if (argc < 3) { kprintf("usage: vcs checkout N | BRANCH\n"); return 1; }
    int n;
    int switching = 0;
    if (is_number(argv[2])) {
        n = atoi(argv[2]);
    } else {
        char p[PATHMAX];
        vpath(p, sizeof p, "refs/", argv[2]);
        if (!fs_lookup(p)) { kprintf("vcs: no branch '%s'\n", argv[2]); return 1; }
        n = ref_get(argv[2]);
        switching = 1;
        if (n < 0) {
            char q[PATHMAX], line[40];
            vpath(q, sizeof q, "HEAD", 0);
            snprintf(line, sizeof line, "%s\n", argv[2]);
            wr_str(q, line);
            fs_sync();
            kprintf("vcs: on branch %s (no commits yet)\n", argv[2]);
            return 0;
        }
    }

    commit_t *c = (commit_t *)kmalloc(sizeof(commit_t));
    if (!c) { kprintf("vcs: out of memory\n"); return 1; }
    if (commit_load(n, c) != 0) { kprintf("vcs: no commit %d\n", n); kfree(c); return 1; }

    int restored = 0, failed = 0;
    for (int i = 0; i < c->nent; i++) {
        if (restore_file(c->ent[i].path, c->ent[i].hash) == 0) restored++;
        else { kprintf("vcs: cannot restore %s\n", c->ent[i].path); failed = 1; }
    }
    index_save(c->ent, c->nent);

    if (switching) {
        char q[PATHMAX], line[40];
        vpath(q, sizeof q, "HEAD", 0);
        snprintf(line, sizeof line, "%s\n", argv[2]);
        wr_str(q, line);
    }
    fs_sync();
    kprintf("vcs: restored %d file%s from commit %d\n", restored, restored == 1 ? "" : "s", n);
    if (!switching)
        kprintf("vcs: the branch still points at commit %d -- commit to move it\n", head_commit());
    kfree(c);
    return failed ? 1 : 0;
}

static int vcs_branch(int argc, char **argv) {
    char cur[32];
    head_branch(cur, sizeof cur);
    if (argc < 3) {
        char p[PATHMAX];
        vpath(p, sizeof p, "refs", 0);
        fs_node *d = fs_lookup(p);
        if (!d) { kprintf("vcs: no branches\n"); return 1; }
        for (fs_node *c = d->child; c; c = c->next)
            kprintf("%s %-16s commit %d\n", strcmp(c->name, cur) ? " " : "*",
                    c->name, ref_get(c->name));
        return 0;
    }
    const char *name = argv[2];
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == ' ') { kprintf("vcs: a branch name cannot contain '%c'\n", *p); return 1; }
    char p[PATHMAX];
    vpath(p, sizeof p, "refs/", name);
    if (fs_lookup(p)) { kprintf("vcs: branch '%s' already exists\n", name); return 1; }
    ref_set(name, head_commit());
    char q[PATHMAX], line[40];
    vpath(q, sizeof q, "HEAD", 0);
    snprintf(line, sizeof line, "%s\n", name);
    wr_str(q, line);
    fs_sync();
    kprintf("vcs: branch %s starts at commit %d, and is now checked out\n", name, head_commit());
    return 0;
}

int cmd_vcs(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    const char *sub = argv[1];
    if (!strcmp(sub, "-h") || !strcmp(sub, "--help") || !strcmp(sub, "help")) { usage(); return 0; }
    if (!strcmp(sub, "init")) return vcs_init();

    if (repo_find() != 0) {
        kprintf("vcs: no repository here or above -- run `vcs init` first\n");
        return 1;
    }
    if (!strcmp(sub, "add"))      return argc < 3 ? (kprintf("usage: vcs add FILE... | .\n"), 1)
                                                  : vcs_add(argc, argv);
    if (!strcmp(sub, "status"))   return vcs_status();
    if (!strcmp(sub, "commit"))   return vcs_commit(argc, argv);
    if (!strcmp(sub, "log"))      return vcs_log(argc, argv);
    if (!strcmp(sub, "show"))     return vcs_show(argc, argv);
    if (!strcmp(sub, "diff"))     return vcs_diff(argc, argv);
    if (!strcmp(sub, "checkout")) return vcs_checkout(argc, argv);
    if (!strcmp(sub, "branch"))   return vcs_branch(argc, argv);
    kprintf("vcs: unknown command '%s'\n", sub);
    usage();
    return 1;
}
