#include <stdint.h>
#include "commands.h"
#include "pkg.h"
#include "fs.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "boltpy.h"
#include "proc.h"
#include "http.h"
#include "netif.h"
#include <stdio.h>

/* ===========================================================================
 *  BoltOS  -  kernel/pkg.c
 *  The package manager.
 *
 *  What was here before was five BoltPython programs compiled into the kernel;
 *  "installing" one copied a string into /apps. Nothing could be added to it
 *  without rebuilding the kernel on another machine, which is the one thing a
 *  package manager exists to avoid.
 *
 *  There is now a package format and a repository format (include/pkg.h), and
 *  every command here is built on them:
 *
 *      pkg build DIR      make a package out of a directory and its pkg.txt
 *      pkg publish FILE   put one into a repository, and index it
 *      pkg update         copy the index from the repository in use
 *      pkg install NAME   dependencies first, hash checked, files recorded
 *      pkg remove NAME    take back exactly what was put down
 *
 *  The repository is a path or an http:// URL and nothing downstream of
 *  fetch() knows which, so a package installed off the disk and the same
 *  package installed over the network take the same path through the code.
 *
 *  The catalogue that ships with the OS (kernel/pkgcat.c) is written out as a
 *  real repository the first time one is needed rather than being installed
 *  directly, because a built-in that took a shortcut would not be exercising
 *  the code that matters.
 * ===========================================================================*/

#define LINE_MAX 512

/* ------------------------------------------------------------- utilities --- */
static void sset(char *dst, uint32_t cap, const char *src) {
    uint32_t i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* FNV-1a over the package bytes, 16 hex digits: enough to notice a truncated
 * download or a repository that changed under us. It is not a signature and is
 * not presented as one. */
static void hash_hex(const void *data, uint32_t len, char *out17) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ull; }
    for (int i = 0; i < 16; i++) out17[i] = "0123456789abcdef"[(h >> ((15 - i) * 4)) & 0xF];
    out17[16] = 0;
}

static void ensure_dirs(void) {
    if (!fs_lookup("/etc"))     fs_create("/etc", 1);
    if (!fs_lookup("/apps"))    fs_create("/apps", 1);
    if (!fs_lookup("/var"))     fs_create("/var", 1);
    if (!fs_lookup("/var/lib")) fs_create("/var/lib", 1);
    if (!fs_lookup(PKG_ROOT))   fs_create(PKG_ROOT, 1);
    if (!fs_lookup(PKG_DB))     fs_create(PKG_DB, 1);
}

/* Create the directories a path needs, then write it. */
static int write_file(const char *path, const void *data, uint32_t len) {
    char tmp[256];
    sset(tmp, sizeof tmp, path);
    for (int i = 1; tmp[i]; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = 0;
        if (!fs_lookup(tmp)) fs_create(tmp, 1);
        tmp[i] = '/';
    }
    fs_node *f = fs_lookup(path);
    if (!f) f = fs_create(path, 0);
    if (!f || f->is_dir) return -1;
    if (!fs_may(f, FS_PERM_WRITE)) return -1;
    return fs_write(f, data, len);
}

static char *slurp(const char *path, uint32_t *len_out) {
    fs_node *f = fs_lookup(path);
    if (!f || f->is_dir || !fs_may(f, FS_PERM_READ)) return 0;
    char *b = (char *)kmalloc(f->size + 1);
    if (!b) return 0;
    if (f->data && f->size) memcpy(b, f->data, f->size);
    b[f->size] = 0;
    if (len_out) *len_out = f->size;
    return b;
}

/* One line out of a text blob, NUL-terminated in place of its newline. `*p`
 * ends up on the next line. Returns 0 at the end. */
static char *next_line(char **p) {
    char *s = *p;
    if (!*s) return 0;
    char *e = s;
    while (*e && *e != '\n') e++;
    if (*e) { *e = 0; *p = e + 1; } else { *p = e; }
    /* tolerate CRLF, since an index may have been written anywhere */
    uint32_t n = (uint32_t)strlen(s);
    if (n && s[n - 1] == '\r') s[n - 1] = 0;
    return s;
}

/* "key rest-of-line": returns the value if the line starts with `key`. */
static const char *field(const char *line, const char *key) {
    uint32_t n = (uint32_t)strlen(key);
    if (strncmp(line, key, n) || (line[n] != ' ' && line[n] != 0)) return 0;
    const char *v = line + n;
    while (*v == ' ') v++;
    return v;
}

/* ------------------------------------------------------- the repository --- */
static void repo_get(char *out, uint32_t cap) {
    uint32_t n = 0;
    char *b = slurp(PKG_CONF, &n);
    if (!b) { sset(out, cap, PKG_REPO); return; }
    char *p = b;
    char *l = next_line(&p);
    sset(out, cap, (l && l[0]) ? l : PKG_REPO);
    kfree(b);
}

static int is_url(const char *s) { return !strncmp(s, "http://", 7) || !strncmp(s, "https://", 8); }

/* Read one file out of the repository, wherever it is. Everything downstream
 * of this is the same for a disk repository and a network one. */
static char *fetch(const char *repo, const char *name, uint32_t *len_out) {
    char url[320];
    uint32_t rl = (uint32_t)strlen(repo);
    int slash = (rl && repo[rl - 1] == '/');
    snprintf(url, sizeof url, "%s%s%s", repo, slash ? "" : "/", name);

    if (!is_url(repo)) return slurp(url, len_out);

    if (!netif_default()) { kprintf("pkg: no network interface\n"); return 0; }
    char *buf = (char *)kmalloc(PKG_MAX + 1);
    if (!buf) { kprintf("pkg: out of memory\n"); return 0; }
    int code = 0;
    char loc[256];
    int n = http_get(url, buf, PKG_MAX, &code, loc, sizeof loc);
    if (n < 0 || code != 200) {
        kprintf("pkg: cannot fetch %s (HTTP %d)\n", url, code);
        kfree(buf);
        return 0;
    }
    if (len_out) *len_out = (uint32_t)n;
    return buf;
}

/* --------------------------------------------------------- the installed -- */
static int installed(const char *name) {
    char p[128];
    snprintf(p, sizeof p, PKG_DB "/%s", name);
    return fs_lookup(p) != 0;
}

/* Does any installed package require `name`? */
static int required_by(const char *name, char *who, uint32_t cap) {
    fs_node *d = fs_lookup(PKG_DB);
    if (!d || !d->is_dir) return 0;
    for (fs_node *c = d->child; c; c = c->next) {
        if (c->is_dir || !c->data) continue;
        char path[128];
        snprintf(path, sizeof path, PKG_DB "/%s", c->name);
        uint32_t n = 0;
        char *b = slurp(path, &n);
        if (!b) continue;
        char *p = b, *l;
        int hit = 0;
        while ((l = next_line(&p))) {
            const char *v = field(l, "requires");
            if (!v) continue;
            char tok[64]; int t = 0;
            for (const char *q = v; ; q++) {
                if (*q && *q != ' ') { if (t < 63) tok[t++] = *q; continue; }
                tok[t] = 0;
                if (t && !strcmp(tok, name)) hit = 1;
                t = 0;
                if (!*q) break;
            }
        }
        kfree(b);
        if (hit) { sset(who, cap, c->name); return 1; }
    }
    return 0;
}

/* ------------------------------------------------ packing and unpacking ---- */
/* A growable byte buffer, because a package is built up a file at a time. */
typedef struct { char *b; uint32_t len, cap; } buf_t;

static int buf_need(buf_t *B, uint32_t extra) {
    if (B->len + extra <= B->cap) return 0;
    uint32_t want = B->cap ? B->cap * 2 : 4096;
    while (want < B->len + extra) want *= 2;
    if (want > PKG_MAX) want = PKG_MAX;
    if (B->len + extra > want) return -1;
    char *n = (char *)kmalloc(want);
    if (!n) return -1;
    if (B->len) memcpy(n, B->b, B->len);
    if (B->b) kfree(B->b);
    B->b = n; B->cap = want;
    return 0;
}
static int buf_add(buf_t *B, const void *d, uint32_t n) {
    if (buf_need(B, n) != 0) return -1;
    memcpy(B->b + B->len, d, n);
    B->len += n;
    return 0;
}
static int buf_str(buf_t *B, const char *s) { return buf_add(B, s, (uint32_t)strlen(s)); }

/* Write the header stanza shared by a package and its index entry. */
static void stanza(buf_t *B, const char *name, const char *version, const char *kind,
                   const char *summary, const char *requires, const char *run) {
    char l[LINE_MAX];
    snprintf(l, sizeof l, "package %s\n", name);       buf_str(B, l);
    snprintf(l, sizeof l, "version %s\n", version);    buf_str(B, l);
    snprintf(l, sizeof l, "kind %s\n", kind);          buf_str(B, l);
    snprintf(l, sizeof l, "summary %s\n", summary);    buf_str(B, l);
    if (requires && requires[0]) { snprintf(l, sizeof l, "requires %s\n", requires); buf_str(B, l); }
    if (run && run[0])           { snprintf(l, sizeof l, "run %s\n", run);           buf_str(B, l); }
}

/* Build a .pkg image in memory from a header and a list of (path, bytes). */
static char *pack(const char *name, const char *version, const char *kind,
                  const char *summary, const char *requires, const char *run,
                  const char **paths, const char **bodies, const uint32_t *sizes,
                  int nfiles, uint32_t *len_out) {
    buf_t B = { 0, 0, 0 };
    buf_str(&B, "BOLTPKG 1\n");
    stanza(&B, name, version, kind, summary, requires, run);
    buf_str(&B, "\n");
    for (int i = 0; i < nfiles; i++) {
        char l[LINE_MAX];
        snprintf(l, sizeof l, "file %u %s\n", sizes[i], paths[i]);
        if (buf_str(&B, l) != 0 || buf_add(&B, bodies[i], sizes[i]) != 0 ||
            buf_str(&B, "\n") != 0) {
            if (B.b) kfree(B.b);
            return 0;
        }
    }
    if (len_out) *len_out = B.len;
    return B.b;
}

/* Header fields of a package or an index stanza. */
typedef struct {
    char name[40], version[16], kind[12], summary[96];
    char requires[128], run[96], file[80], sha[17];
} meta_t;

/* Parse header lines out of `text` (which is modified). Stops at a blank line
 * or at the next `package` line, and reports where it stopped. */
static char *parse_meta(char *text, meta_t *m, int stop_at_next) {
    memset(m, 0, sizeof *m);
    char *p = text, *l;
    int seen = 0;
    while (*p) {
        char *save = p;
        l = next_line(&p);
        if (!l) break;
        if (!l[0]) { if (seen) break; else continue; }
        const char *v;
        if ((v = field(l, "package"))) {
            if (seen && stop_at_next) { p = save; break; }
            sset(m->name, sizeof m->name, v); seen = 1;
        }
        else if ((v = field(l, "version")))  sset(m->version,  sizeof m->version,  v);
        else if ((v = field(l, "kind")))     sset(m->kind,     sizeof m->kind,     v);
        else if ((v = field(l, "summary")))  sset(m->summary,  sizeof m->summary,  v);
        else if ((v = field(l, "requires"))) sset(m->requires, sizeof m->requires, v);
        else if ((v = field(l, "run")))      sset(m->run,      sizeof m->run,      v);
        else if ((v = field(l, "file")))     sset(m->file,     sizeof m->file,     v);
        else if ((v = field(l, "sha")))      sset(m->sha,      sizeof m->sha,      v);
        else if (!strncmp(l, "BOLTPKG", 7))  continue;
        else { /* an unknown key is ignored: a newer repository must not be fatal */ }
    }
    return p;
}

/* Unpack a package image, writing its files and returning the list of paths
 * written (newline separated, kmalloc'd) so the install record can own them. */
static char *unpack(char *img, uint32_t len, meta_t *m, int *nfile_out) {
    /* The header ends at the first blank line; the body is byte-exact after it,
     * so it must be found without disturbing anything past it. */
    uint32_t h = 0;
    while (h + 1 < len && !(img[h] == '\n' && img[h + 1] == '\n')) h++;
    if (h + 1 >= len) return 0;
    img[h] = 0;
    parse_meta(img, m, 0);
    if (!m->name[0]) return 0;

    uint32_t o = h + 2;
    buf_t list = { 0, 0, 0 };
    int nf = 0;
    while (o < len) {
        /* "file <size> <path>\n" */
        uint32_t e = o;
        while (e < len && img[e] != '\n') e++;
        if (e >= len) break;
        img[e] = 0;
        const char *v = field(img + o, "file");
        if (!v) { img[e] = '\n'; break; }
        uint32_t size = (uint32_t)atoi(v);
        while (*v && *v != ' ') v++;
        while (*v == ' ') v++;
        char path[224];
        sset(path, sizeof path, v);
        img[e] = '\n';
        o = e + 1;
        if (o + size > len) break;
        if (!path[0]) break;
        if (write_file(path, img + o, size) != 0) {
            kprintf("pkg: cannot write %s\n", path);
            if (list.b) kfree(list.b);
            return 0;
        }
        buf_str(&list, "file ");
        buf_str(&list, path);
        buf_str(&list, "\n");
        nf++;
        o += size;
        if (o < len && img[o] == '\n') o++;      /* the separator */
    }
    if (nfile_out) *nfile_out = nf;
    if (!list.b) { list.b = (char *)kmalloc(1); if (list.b) list.b[0] = 0; return list.b; }
    if (buf_add(&list, "", 1) != 0) { kfree(list.b); return 0; }
    return list.b;
}

/* --------------------------------------------------------- the catalogue -- */
/* Write the built-in packages out as a real repository. */
static int seed_repo(void) {
    int n = 0;
    const pkgdef_t *cat = pkg_builtin(&n);
    if (!fs_lookup(PKG_REPO) && !fs_create(PKG_REPO, 1)) return -1;

    buf_t index = { 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        const pkgdef_t *d = &cat[i];
        const char *paths[16], *bodies[16];
        uint32_t sizes[16];
        int nf = d->nfiles > 16 ? 16 : d->nfiles;
        for (int k = 0; k < nf; k++) {
            paths[k] = d->files[k].path;
            bodies[k] = d->files[k].body;
            sizes[k] = (uint32_t)strlen(d->files[k].body);
        }
        uint32_t plen = 0;
        char *img = pack(d->name, d->version, d->kind, d->summary, d->requires,
                         d->run, paths, bodies, sizes, nf, &plen);
        if (!img) continue;

        char fname[80], path[160], sha[17];
        snprintf(fname, sizeof fname, "%s-%s.pkg", d->name, d->version);
        snprintf(path, sizeof path, PKG_REPO "/%s", fname);
        write_file(path, img, plen);
        hash_hex(img, plen, sha);
        kfree(img);

        stanza(&index, d->name, d->version, d->kind, d->summary, d->requires, d->run);
        char l[LINE_MAX];
        snprintf(l, sizeof l, "file %s\nsha %s\n\n", fname, sha);
        buf_str(&index, l);
    }
    if (index.b) {
        write_file(PKG_REPO "/index", index.b, index.len);
        kfree(index.b);
    }
    return 0;
}

/* The index in use, loaded from /var/lib/pkg/index. */
static char *load_index(uint32_t *len_out) { return slurp(PKG_INDEX, len_out); }

/* Find a package's stanza in the index. Returns 0 when it is not there. */
static int index_find(const char *name, meta_t *m) {
    uint32_t n = 0;
    char *idx = load_index(&n);
    if (!idx) return 0;
    char *p = idx;
    int found = 0;
    while (*p) {
        meta_t cur;
        p = parse_meta(p, &cur, 1);
        if (!cur.name[0]) break;
        if (!strcmp(cur.name, name)) { *m = cur; found = 1; break; }
    }
    kfree(idx);
    return found;
}

/* -------------------------------------------------------------- commands -- */
static int pkg_update(void) {
    ensure_dirs();
    char repo[256];
    repo_get(repo, sizeof repo);
    if (!is_url(repo) && !fs_lookup(repo) && !strcmp(repo, PKG_REPO)) seed_repo();

    uint32_t n = 0;
    char *idx = fetch(repo, "index", &n);
    if (!idx) { kprintf("pkg: cannot read the index of %s\n", repo); return 1; }
    if (write_file(PKG_INDEX, idx, n) != 0) { kfree(idx); kprintf("pkg: cannot save the index\n"); return 1; }

    int count = 0;
    for (uint32_t i = 0; i + 8 <= n; i++)
        if (!strncmp(idx + i, "package ", 8) && (i == 0 || idx[i - 1] == '\n')) count++;
    kfree(idx);
    fs_sync();
    kprintf("pkg: %d package%s available from %s\n", count, count == 1 ? "" : "s", repo);
    return 0;
}

/* Make sure there is an index to work from; a first-run user should not have to
 * know that `update` exists. */
static void ensure_index(void) {
    if (fs_lookup(PKG_INDEX)) return;
    pkg_update();
}

static int pkg_list(void) {
    ensure_dirs();
    fs_node *d = fs_lookup(PKG_DB);
    int n = 0;
    if (d && d->is_dir) {
        for (fs_node *c = d->child; c; c = c->next) {
            if (c->is_dir) continue;
            char path[128];
            snprintf(path, sizeof path, PKG_DB "/%s", c->name);
            uint32_t sz = 0;
            char *b = slurp(path, &sz);
            meta_t m;
            memset(&m, 0, sizeof m);
            int files = 0;
            if (b) {
                /* parse_meta chews up the buffer, so count the file lines off a
                 * second copy rather than reading the record twice. */
                char *copy = (char *)kmalloc(sz + 1);
                if (copy) {
                    memcpy(copy, b, sz); copy[sz] = 0;
                    char *q = copy, *l;
                    while ((l = next_line(&q))) if (field(l, "file")) files++;
                    kfree(copy);
                }
                parse_meta(b, &m, 0);
                kfree(b);
            }
            kprintf("  %-12s %-8s %-8s %d file%s\n", c->name,
                    m.version[0] ? m.version : "?", m.kind[0] ? m.kind : "?",
                    files, files == 1 ? "" : "s");
            n++;
        }
    }
    if (!n) kprintf("no packages installed (try: pkg avail)\n");
    return 0;
}

static int pkg_avail(const char *term) {
    ensure_index();
    uint32_t n = 0;
    char *idx = load_index(&n);
    if (!idx) { kprintf("pkg: no index -- run `pkg update`\n"); return 1; }
    char *p = idx;
    int shown = 0;
    while (*p) {
        meta_t m;
        p = parse_meta(p, &m, 1);
        if (!m.name[0]) break;
        if (term && !strstr(m.name, term) && !strstr(m.summary, term)) continue;
        kprintf("  %-12s %-6s %-8s %-42s %s\n", m.name, m.version, m.kind, m.summary,
                installed(m.name) ? "[installed]" : "");
        shown++;
    }
    kfree(idx);
    if (!shown) kprintf("  (nothing matches)\n");
    return 0;
}

static int pkg_info(const char *name) {
    ensure_index();
    meta_t m;
    if (!index_find(name, &m)) { kprintf("pkg: no package '%s'\n", name); return 1; }
    kprintf("%s %s (%s)\n  %s\n", m.name, m.version, m.kind, m.summary);
    if (m.requires[0]) kprintf("  requires: %s\n", m.requires);
    if (m.run[0])      kprintf("  runs:     %s\n", m.run);
    kprintf("  file:     %s\n  sha:      %s\n  status:   %s\n",
            m.file, m.sha, installed(name) ? "installed" : "not installed");
    if (installed(name)) {
        char path[128];
        snprintf(path, sizeof path, PKG_DB "/%s", name);
        uint32_t sz = 0;
        char *b = slurp(path, &sz);
        if (b) {
            char *p = b, *l;
            while ((l = next_line(&p))) {
                const char *v = field(l, "file");
                if (v) kprintf("    %s\n", v);
            }
            kfree(b);
        }
    }
    return 0;
}

static int install_one(const char *name, int depth) {
    if (depth > PKG_MAX_DEPTH) {
        kprintf("pkg: dependencies nest too deeply at '%s' (a cycle?)\n", name);
        return 1;
    }
    if (installed(name)) {
        if (depth == 0) kprintf("pkg: '%s' is already installed\n", name);
        return 0;
    }
    meta_t m;
    if (!index_find(name, &m)) {
        kprintf("pkg: no package '%s' in the index\n", name);
        return 1;
    }

    /* Dependencies first, so a package is never on disk before what it needs. */
    if (m.requires[0]) {
        char tok[64]; int t = 0;
        for (const char *q = m.requires; ; q++) {
            if (*q && *q != ' ') { if (t < 63) tok[t++] = *q; continue; }
            tok[t] = 0;
            if (t) {
                if (!installed(tok)) kprintf("pkg: %s requires %s\n", name, tok);
                if (install_one(tok, depth + 1) != 0) return 1;
            }
            t = 0;
            if (!*q) break;
        }
    }

    char repo[256];
    repo_get(repo, sizeof repo);
    uint32_t len = 0;
    char *img = fetch(repo, m.file[0] ? m.file : name, &len);
    if (!img) { kprintf("pkg: cannot fetch %s\n", m.file); return 1; }

    if (m.sha[0]) {
        char sha[17];
        hash_hex(img, len, sha);
        if (strcmp(sha, m.sha)) {
            kprintf("pkg: %s does not match its index entry (%s, expected %s)\n",
                    m.file, sha, m.sha);
            kfree(img);
            return 1;
        }
    }

    meta_t pm;
    int nf = 0;
    char *files = unpack(img, len, &pm, &nf);
    kfree(img);
    if (!files) { kprintf("pkg: %s is not a valid package\n", m.file); return 1; }

    /* The install record: what to remove later, and what depends on what. */
    buf_t rec = { 0, 0, 0 };
    char l[LINE_MAX];
    snprintf(l, sizeof l, "version %s\nkind %s\n", m.version, m.kind); buf_str(&rec, l);
    if (m.requires[0]) { snprintf(l, sizeof l, "requires %s\n", m.requires); buf_str(&rec, l); }
    if (pm.run[0])     { snprintf(l, sizeof l, "run %s\n", pm.run);          buf_str(&rec, l); }
    buf_str(&rec, files);
    kfree(files);

    ensure_dirs();
    char dbp[128];
    snprintf(dbp, sizeof dbp, PKG_DB "/%s", name);
    write_file(dbp, rec.b, rec.len);
    if (rec.b) kfree(rec.b);
    fs_sync();

    kprintf("pkg: installed %s %s (%d file%s)\n", name, m.version, nf, nf == 1 ? "" : "s");
    if (pm.run[0]) kprintf("     run it with: pkg run %s\n", name);
    return 0;
}

static int pkg_remove(const char *name) {
    if (!installed(name)) { kprintf("pkg: '%s' is not installed\n", name); return 1; }
    char who[64];
    if (required_by(name, who, sizeof who)) {
        kprintf("pkg: '%s' is required by '%s' -- remove that first\n", name, who);
        return 1;
    }
    char dbp[128];
    snprintf(dbp, sizeof dbp, PKG_DB "/%s", name);
    uint32_t n = 0;
    char *rec = slurp(dbp, &n);
    if (!rec) { kprintf("pkg: cannot read the install record for '%s'\n", name); return 1; }

    int removed = 0;
    char *p = rec, *l;
    while ((l = next_line(&p))) {
        const char *v = field(l, "file");
        if (!v || !v[0]) continue;
        if (fs_remove(v) == 0) removed++;
    }
    kfree(rec);
    fs_remove(dbp);
    fs_sync();
    kprintf("pkg: removed %s (%d file%s)\n", name, removed, removed == 1 ? "" : "s");
    return 0;
}

static int pkg_run(const char *name) {
    char dbp[128];
    snprintf(dbp, sizeof dbp, PKG_DB "/%s", name);
    uint32_t n = 0;
    char *rec = slurp(dbp, &n);
    if (!rec) { kprintf("pkg: '%s' is not installed (try: pkg install %s)\n", name, name); return 1; }
    meta_t m;
    parse_meta(rec, &m, 0);
    kfree(rec);
    if (!m.run[0]) { kprintf("pkg: '%s' has nothing to run\n", name); return 1; }

    if (!strcmp(m.kind, "program")) {
        int pid = proc_exec(m.run);
        if (pid < 0) { kprintf("pkg: cannot run %s\n", m.run); return 1; }
        return 0;
    }
    uint32_t sz = 0;
    char *src = slurp(m.run, &sz);
    if (!src) { kprintf("pkg: %s is missing\n", m.run); return 1; }
    int rc = bpy_run(src);
    kfree(src);
    return rc;
}

/* ------------------------------------------------------------ authoring --- */
/* `pkg build DIR` -- a directory with a pkg.txt becomes a package.
 *
 *      package thing
 *      version 1.0
 *      kind    script
 *      summary what it is
 *      requires a b
 *      run     /apps/thing.py
 *      install thing.py /apps/thing.py     <- source, then where it goes
 */
static int pkg_build(const char *dir, const char *out) {
    char manifest[224];
    snprintf(manifest, sizeof manifest, "%s/pkg.txt", dir);
    uint32_t n = 0;
    char *text = slurp(manifest, &n);
    if (!text) { kprintf("pkg: no %s\n", manifest); return 1; }

    /* The manifest's header, and then its install lines. */
    char *copy = (char *)kmalloc(n + 1);
    if (!copy) { kfree(text); kprintf("pkg: out of memory\n"); return 1; }
    memcpy(copy, text, n + 1);
    meta_t m;
    parse_meta(copy, &m, 0);
    kfree(copy);
    if (!m.name[0]) { kfree(text); kprintf("pkg: %s does not name a package\n", manifest); return 1; }
    if (!m.version[0]) sset(m.version, sizeof m.version, "1.0");
    if (!m.kind[0])    sset(m.kind, sizeof m.kind, "source");

    const char *paths[32], *bodies[32];
    uint32_t sizes[32];
    char *held[32];
    int nf = 0;
    static char dests[32][224];

    char *p = text, *l;
    while ((l = next_line(&p)) && nf < 32) {
        const char *v = field(l, "install");
        if (!v) continue;
        char src[128], dst[224];
        int i = 0;
        while (*v && *v != ' ' && i < 127) src[i++] = *v++;
        src[i] = 0;
        while (*v == ' ') v++;
        sset(dst, sizeof dst, v);
        if (!src[0] || !dst[0]) continue;

        char full[224];
        if (src[0] == '/') snprintf(full, sizeof full, "%s", src);
        else               snprintf(full, sizeof full, "%s/%s", dir, src);
        uint32_t sz = 0;
        char *body = slurp(full, &sz);
        if (!body) { kprintf("pkg: cannot read %s\n", full); continue; }
        held[nf] = body;
        bodies[nf] = body;
        sizes[nf] = sz;
        sset(dests[nf], sizeof dests[nf], dst);
        paths[nf] = dests[nf];
        nf++;
    }
    kfree(text);
    if (!nf) { kprintf("pkg: %s installs nothing\n", manifest); return 1; }

    uint32_t plen = 0;
    char *img = pack(m.name, m.version, m.kind, m.summary, m.requires, m.run,
                     paths, bodies, sizes, nf, &plen);
    for (int i = 0; i < nf; i++) kfree(held[i]);
    if (!img) { kprintf("pkg: the package is too large\n"); return 1; }

    char target[224];
    if (out) sset(target, sizeof target, out);
    else     snprintf(target, sizeof target, "%s/%s-%s.pkg", dir, m.name, m.version);
    int rc = write_file(target, img, plen);
    kfree(img);
    if (rc != 0) { kprintf("pkg: cannot write %s\n", target); return 1; }
    fs_sync();
    kprintf("pkg: built %s (%u bytes, %d file%s)\n", target, plen, nf, nf == 1 ? "" : "s");
    return 0;
}

/* `pkg publish FILE [REPO]` -- put a package into a repository and index it. */
static int pkg_publish(const char *file, const char *repo_in) {
    char repo[256];
    if (repo_in) sset(repo, sizeof repo, repo_in);
    else         repo_get(repo, sizeof repo);
    if (is_url(repo)) { kprintf("pkg: cannot publish to a URL (%s)\n", repo); return 1; }

    uint32_t n = 0;
    char *img = slurp(file, &n);
    if (!img) { kprintf("pkg: cannot read %s\n", file); return 1; }

    char *copy = (char *)kmalloc(n + 1);
    if (!copy) { kfree(img); kprintf("pkg: out of memory\n"); return 1; }
    memcpy(copy, img, n); copy[n] = 0;
    meta_t m;
    parse_meta(copy, &m, 0);
    kfree(copy);
    if (!m.name[0]) { kfree(img); kprintf("pkg: %s is not a package\n", file); return 1; }

    char fname[96], dest[256], sha[17];
    snprintf(fname, sizeof fname, "%s-%s.pkg", m.name, m.version[0] ? m.version : "1.0");
    snprintf(dest, sizeof dest, "%s/%s", repo, fname);
    if (write_file(dest, img, n) != 0) { kfree(img); kprintf("pkg: cannot write %s\n", dest); return 1; }
    hash_hex(img, n, sha);
    kfree(img);

    /* Rewrite the index without this package, then append its new stanza. */
    char ipath[256];
    snprintf(ipath, sizeof ipath, "%s/index", repo);
    buf_t out = { 0, 0, 0 };
    uint32_t ilen = 0;
    char *idx = slurp(ipath, &ilen);
    if (idx) {
        char *p = idx;
        while (*p) {
            char *start = p;
            meta_t cur;
            p = parse_meta(p, &cur, 1);
            if (!cur.name[0]) break;
            if (!strcmp(cur.name, m.name)) continue;      /* replaced below */
            buf_add(&out, start, (uint32_t)(p - start));
        }
        kfree(idx);
    }
    stanza(&out, m.name, m.version[0] ? m.version : "1.0", m.kind[0] ? m.kind : "source",
           m.summary, m.requires, m.run);
    char l[LINE_MAX];
    snprintf(l, sizeof l, "file %s\nsha %s\n\n", fname, sha);
    buf_str(&out, l);
    int rc = write_file(ipath, out.b, out.len);
    if (out.b) kfree(out.b);
    if (rc != 0) { kprintf("pkg: cannot write %s\n", ipath); return 1; }
    fs_sync();
    kprintf("pkg: published %s %s to %s\n", m.name, m.version, repo);
    return 0;
}

static int pkg_repo(const char *set) {
    char repo[256];
    if (!set) {
        repo_get(repo, sizeof repo);
        kprintf("pkg: repository %s\n", repo);
        return 0;
    }
    ensure_dirs();
    char line[264];
    snprintf(line, sizeof line, "%s\n", set);
    if (write_file(PKG_CONF, line, (uint32_t)strlen(line)) != 0) {
        kprintf("pkg: cannot write " PKG_CONF "\n");
        return 1;
    }
    fs_remove(PKG_INDEX);                  /* the old index belongs to the old repo */
    fs_sync();
    kprintf("pkg: repository set to %s (run `pkg update`)\n", set);
    return 0;
}

static void usage(void) {
    kprintf("usage: pkg COMMAND [args]\n"
            "  avail [TERM]      what the repository offers (searched, if TERM)\n"
            "  list              what is installed here\n"
            "  info NAME         one package, and the files it owns\n"
            "  install NAME      dependencies first, hash checked\n"
            "  remove NAME       take back exactly what it put down\n"
            "  run NAME          run an installed script or program\n"
            "  update            re-read the repository's index\n"
            "  repo [URL|PATH]   show, or change, the repository in use\n"
            "  build DIR [OUT]   pack DIR (which needs a pkg.txt) into a .pkg\n"
            "  publish FILE [REPO]  add a package to a repository and index it\n"
            "\n"
            "The repository is a directory or an http:// URL; everything past\n"
            "fetching works the same either way. Default: " PKG_REPO "\n");
}

int cmd_pkg(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "list";
    const char *a2  = argc > 2 ? argv[2] : 0;
    const char *a3  = argc > 3 ? argv[3] : 0;

    if (!strcmp(sub, "list"))    return pkg_list();
    if (!strcmp(sub, "avail") || !strcmp(sub, "search")) return pkg_avail(a2);
    if (!strcmp(sub, "update"))  return pkg_update();
    if (!strcmp(sub, "repo"))    return pkg_repo(a2);
    if (!strcmp(sub, "info"))    return a2 ? pkg_info(a2) : (usage(), 1);
    if (!strcmp(sub, "remove"))  return a2 ? pkg_remove(a2) : (usage(), 1);
    if (!strcmp(sub, "run"))     return a2 ? pkg_run(a2) : (usage(), 1);
    if (!strcmp(sub, "build"))   return a2 ? pkg_build(a2, a3) : (usage(), 1);
    if (!strcmp(sub, "publish")) return a2 ? pkg_publish(a2, a3) : (usage(), 1);
    if (!strcmp(sub, "install")) {
        if (!a2) { usage(); return 1; }
        ensure_index();
        int bad = 0;
        for (int i = 2; i < argc; i++) if (install_one(argv[i], 0) != 0) bad = 1;
        return bad;
    }
    usage();
    return 1;
}
