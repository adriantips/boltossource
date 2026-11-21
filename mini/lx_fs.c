/* ===========================================================================
 *  BoltOS Mini  -  mini/lx_fs.c
 *
 *  The file tree a Linux program sees (lx.h), in four layers, the first
 *  that has a path winning:
 *    - Mini's own: /dev (lx_dev.c), /etc/resolv.conf and friends, /proc/...
 *    - the overlay: whatever the program wrote, kept on the card's FAT32
 *      partition under lx_config.rw ("/Games/demo/rw/opt/demo/settings.cfg"
 *      for /opt/demo/settings.cfg); a removed pack file is hidden for the run
 *    - the packs, read-only, in the order given (mini/tools/mklxpak.py)
 *
 *  Pack data comes off the card through the boot core in 256 KiB blocks,
 *  kept in a cache of the program's memory; big reads skip it. A file opened
 *  for writing, and every overlay file, is held whole in memory (a "memf")
 *  and written back to the card when the last descriptor on it closes, on
 *  fsync, and when the program ends.
 *
 *  Everything here runs on the program's core, except lxfs_init/lxfs_free
 *  and lxfs_store_read, which the boot core runs.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "lxint.h"
#include "fs.h"
#include "store.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "ctype.h"

/* ---- the packs --------------------------------------------------------------------------- */
typedef struct {
    const store_entry_t *se;
    char      name[48];
    uint32_t  n;
    uint64_t  size;                       /* the pack's bytes (the header says)  */
    uint8_t  *raw;                        /* header + index + names (kmalloc)    */
    lxpak_ent_t *ent;
    const char *names;
    uint32_t *hx, *hl, hmask;             /* path -> entry+1: exact, lower-case  */
} pak_t;
static pak_t P[LX_MAXPACK];
static int np;

static uint32_t hash_s(const char *s, int lower) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)(lower ? tolower((unsigned char)*s) : *s); h *= 16777619u; }
    return h;
}
static const char *ent_name(const pak_t *p, uint32_t i) { return p->names + p->ent[i].name; }

/* rel: the path without its leading '/'; "" is the root. */
static int pak_find(const pak_t *p, const char *rel) {
    uint32_t h = hash_s(rel, 0);
    for (uint32_t k = h & p->hmask; p->hx[k]; k = (k + 1) & p->hmask)
        if (!strcmp(ent_name(p, p->hx[k] - 1), rel)) return (int)p->hx[k] - 1;
    h = hash_s(rel, 1);
    for (uint32_t k = h & p->hmask; p->hl[k]; k = (k + 1) & p->hmask)
        if (!strcasecmp(ent_name(p, p->hl[k] - 1), rel)) return (int)p->hl[k] - 1;
    return -1;
}

static int pak_load(pak_t *p, const char *name, char *why, int cap) {
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->se = store_find(name);
    if (!p->se || !(p->se->flags & STORE_DONE)) { snprintf(why, (size_t)cap, "%s isn't on the card", name); return -1; }
    uint8_t hdr[512];
    if (store_read(p->se, 0, hdr, 512)) { snprintf(why, (size_t)cap, "can't read %s", name); return -1; }
    if (memcmp(hdr, "LXPAK1\0\0", 8)) { snprintf(why, (size_t)cap, "%s isn't an lx pack", name); return -1; }
    uint32_t n; uint64_t io, no, ns, d0, total;
    memcpy(&n, hdr + 8, 4); memcpy(&io, hdr + 16, 8); memcpy(&no, hdr + 24, 8); memcpy(&ns, hdr + 32, 8);
    memcpy(&d0, hdr + 40, 8); memcpy(&total, hdr + 48, 8);
    if (io != 64 || no != io + 32ull * n || d0 < no + ns || d0 & 511 || total > p->se->size + 511 || n > 1000000) {
        snprintf(why, (size_t)cap, "%s is damaged", name); return -1;
    }
    p->raw = kmalloc(d0);
    uint32_t hs = 1;
    while (hs < n * 2 + 16) hs <<= 1;
    p->hx = kmalloc(hs * 4ull); p->hl = kmalloc(hs * 4ull);
    if (!p->raw || !p->hx || !p->hl) { snprintf(why, (size_t)cap, "out of memory for %s's index", name); return -1; }
    if (store_read(p->se, 0, p->raw, (uint32_t)d0)) { snprintf(why, (size_t)cap, "can't read %s", name); return -1; }
    p->n = n; p->size = total; p->hmask = hs - 1;
    p->ent = (lxpak_ent_t *)(p->raw + io);
    p->names = (const char *)(p->raw + no);
    memset(p->hx, 0, hs * 4ull); memset(p->hl, 0, hs * 4ull);
    for (uint32_t i = 0; i < n; i++) {
        if (p->ent[i].name >= ns) { snprintf(why, (size_t)cap, "%s is damaged", name); return -1; }
        const char *s = ent_name(p, i);
        uint32_t k;
        for (k = hash_s(s, 0) & p->hmask; p->hx[k]; k = (k + 1) & p->hmask) { }
        p->hx[k] = i + 1;
        for (k = hash_s(s, 1) & p->hmask; p->hl[k]; k = (k + 1) & p->hmask) { }
        p->hl[k] = i + 1;
    }
    return 0;
}

int lxfs_store_read(int pack, uint64_t off, void *buf, uint32_t len) {
    if (pack < 0 || pack >= np) return -1;
    return store_read(P[pack].se, off, buf, len);
}

/* ---- the block cache ------------------------------------------------------------------------ */
#define BLK (256u * 1024)
typedef struct { int pack; uint64_t blk; uint8_t *data; uint64_t used; uint32_t len; } cblk_t;
static cblk_t *cb;
static int ncb;
static uint64_t ctick, c_hits, c_miss, c_direct, c_bytes;

static cblk_t *cache_get(int pack, uint64_t blk) {
    cblk_t *victim = 0;
    for (int i = 0; i < ncb; i++) {
        cblk_t *b = &cb[i];
        if (b->data && b->pack == pack && b->blk == blk && b->len) { b->used = ++ctick; c_hits++; return b; }
        if (!victim || !b->len || b->used < victim->used) if (!victim || victim->len) victim = b;
    }
    if (!victim) return 0;
    if (!victim->data) { victim->data = lx_kpages(BLK); if (!victim->data) return 0; }
    uint64_t off = blk * BLK;
    uint64_t left = P[pack].size > off ? P[pack].size - off : 0;
    uint32_t len = left < BLK ? (uint32_t)((left + 511) & ~511ull) : BLK;
    if (!len) return 0;
    lx_rq_t rq = { .kind = RQ_STORE, .pack = pack, .off = off, .len = len, .buf = victim->data };
    victim->len = 0;
    if (lx_request(&rq) < 0) return 0;
    victim->pack = pack; victim->blk = blk; victim->len = len; victim->used = ++ctick;
    c_miss++; c_bytes += len;
    return victim;
}

static int64_t pak_read(int pack, uint64_t off, uint8_t *buf, uint64_t n) {
    uint64_t done = 0;
    while (n) {
        if (n >= 512 * 1024 && !(off & 511)) {
            uint64_t k = n & ~511ull;
            if (k > (64u << 20)) k = 64u << 20;
            lx_rq_t rq = { .kind = RQ_STORE, .pack = pack, .off = off, .len = (uint32_t)k, .buf = buf };
            if (lx_request(&rq) < 0) return done ? (int64_t)done : -L_EIO;
            c_direct++; c_bytes += k;
            off += k; buf += k; n -= k; done += k;
            continue;
        }
        cblk_t *b = cache_get(pack, off / BLK);
        if (!b) return done ? (int64_t)done : -L_EIO;
        uint64_t in = off % BLK;
        if (in >= b->len) break;
        uint64_t k = b->len - in < n ? b->len - in : n;
        memcpy(buf, b->data + in, k);
        off += k; buf += k; n -= k; done += k;
    }
    return (int64_t)done;
}

/* ---- the overlay ------------------------------------------------------------------------------ */
struct memf {
    char     path[256];
    uint8_t *data;
    uint64_t size, cap;
    int      refs, dirty;
};
#define MEMF_MAX 96
static memf_t *mfs;

typedef struct {
    char    *path;                        /* "/opt/demo/settings.cfg"               */
    uint64_t size;
    uint32_t mtime;
    uint8_t  dir, gone;                   /* gone: removed, hiding a pack's file   */
} ovl_t;
#define OVL_MAX 8192
static ovl_t *ov;
static int nov;
static char rwroot[128];
static char cwd[256] = "/";
char lx_exe[256];

static int ovl_find(const char *path) {
    for (int i = 0; i < nov; i++) if (ov[i].path && !strcmp(ov[i].path, path)) return i;
    for (int i = 0; i < nov; i++) if (ov[i].path && !strcasecmp(ov[i].path, path)) return i;
    return -1;
}
static int ovl_add(const char *path, int dir, uint64_t size) {
    int i = ovl_find(path);
    if (i < 0) {
        for (i = 0; i < nov; i++) if (!ov[i].path) break;
        if (i == nov) { if (nov >= OVL_MAX) return -1; nov++; }
        size_t n = strlen(path) + 1;
        ov[i].path = lx_kalloc(n);
        if (!ov[i].path) return -1;
        memcpy(ov[i].path, path, n);
    }
    ov[i].dir = (uint8_t)dir; ov[i].gone = 0; ov[i].size = size;
    ov[i].mtime = (uint32_t)(lx_real_ns() / 1000000000ull);
    return i;
}
static void card_path(const char *path, char *out) { snprintf(out, 256, "%s%s", rwroot, path); }

/* What was written in an earlier run (boot core, at start). */
static void scan(const char *card, const char *rel, int depth) {
    if (depth > 12) return;
    fs_ent_t *e = kmalloc(sizeof(fs_ent_t) * 256);
    if (!e) return;
    int n = fs_list(card, e, 256);
    for (int i = 0; i < n; i++) {
        char c2[256], r2[256];
        snprintf(c2, sizeof c2, "%s/%s", card, e[i].name);
        snprintf(r2, sizeof r2, "%s/%s", rel, e[i].name);
        int k = ovl_add(r2, e[i].dir, e[i].size);
        if (k >= 0) ov[k].mtime = 0;
        if (e[i].dir) scan(c2, r2, depth + 1);
    }
    kfree(e);
}

/* ---- the program's own files in memory ---------------------------------------------------------- */
static int mf_reserve(memf_t *m, uint64_t want) {
    if (want <= m->cap) return 0;
    uint64_t cap = m->cap ? m->cap : 16384;
    while (cap < want) cap *= 2;
    uint8_t *d = lx_kpages(cap);
    if (!d) return -L_ENOSPC;
    if (m->size) memcpy(d, m->data, m->size);
    if (m->data) lx_kunpages((uint64_t)(uintptr_t)m->data, m->cap);
    m->data = d; m->cap = cap;
    return 0;
}

static int mf_flush(memf_t *m) {
    if (!m->dirty) return 0;
    lx_rq_t rq = { .kind = RQ_FSWRITE, .buf = m->data, .len = (uint32_t)m->size };
    card_path(m->path, rq.path);
    int r = lx_request(&rq);
    if (r == 0) m->dirty = 0;
    else lx_log("couldn't save %s to the card", m->path);
    return r;
}

static void mf_put(memf_t *m) {
    if (--m->refs > 0) return;
    mf_flush(m);
    if (m->data) lx_kunpages((uint64_t)(uintptr_t)m->data, m->cap);
    memset(m, 0, sizeof *m);
}

static memf_t *mf_get(const char *path) {
    for (int i = 0; i < MEMF_MAX; i++) if (mfs[i].refs && !strcmp(mfs[i].path, path)) { mfs[i].refs++; return &mfs[i]; }
    for (int i = 0; i < MEMF_MAX; i++)
        if (!mfs[i].refs) { memset(&mfs[i], 0, sizeof mfs[i]); snprintf(mfs[i].path, sizeof mfs[i].path, "%s", path); mfs[i].refs = 1; return &mfs[i]; }
    return 0;
}

int lxfs_flush_all(void) {
    int n = 0;
    if (!mfs) return 0;
    for (int i = 0; i < MEMF_MAX; i++) if (mfs[i].refs && mfs[i].dirty && mf_flush(&mfs[i]) == 0) n++;
    return n;
}

/* ---- Mini's own files: /dev, /etc, /proc ------------------------------------------------------------ */
static char resolv[64], hosts[128];
static char meminfo[256], cpuinfo[512];
static const char *const syn_dirs[] = { "/dev", "/dev/input", "/etc", "/proc", "/proc/self", "/tmp", 0 };
static const struct { const char *path; const char *text; } syn_files_tab[] = {
    { "/etc/hosts", hosts },
    { "/etc/passwd", "root:x:0:0:root:/root:/bin/sh\n" },
    { "/etc/group", "root:x:0:\n" },
    { "/etc/nsswitch.conf", "hosts: files dns\n" },
    { "/etc/resolv.conf", resolv },
    { "/proc/meminfo", meminfo },
    { "/proc/cpuinfo", cpuinfo },
    { "/proc/self/maps", "" },
    { 0, 0 },
};
static int syn_dir(const char *p) { for (int i = 0; syn_dirs[i]; i++) if (!strcmp(syn_dirs[i], p)) return i; return -1; }
static int syn_file(const char *p) { for (int i = 0; syn_files_tab[i].path; i++) if (!strcmp(syn_files_tab[i].path, p)) return i; return -1; }
static int is_dev(const char *p) {
    return !strcmp(p, "/dev/fb0") || !strcmp(p, "/dev/input/event0") || !strcmp(p, "/dev/tty") || !strcmp(p, "/dev/tty0") ||
           !strcmp(p, "/dev/null") || !strcmp(p, "/dev/zero") || !strcmp(p, "/dev/urandom") || !strcmp(p, "/dev/random") ||
           !strcmp(p, "/dev/console");
}

/* ---- where a path is -------------------------------------------------------------------------------- */
enum { N_NONE, N_SYNDIR, N_SYNFILE, N_DEV, N_OVL, N_PAK, N_EXE };
typedef struct { int kind, pack, idx; uint32_t mode, mtime; uint64_t size, ino; } node_t;

static int resolve(const char *path, node_t *nd) {
    memset(nd, 0, sizeof *nd);
    int i;
    if (!strcmp(path, "/proc/self/exe")) path = lx_exe;
    if (path[0] != '/') return -L_ENOENT;
    if (is_dev(path)) { nd->kind = N_DEV; nd->mode = L_S_IFCHR | 0666; nd->ino = 0x900000000ull | hash_s(path, 0); return 0; }
    if ((i = syn_file(path)) >= 0) {
        nd->kind = N_SYNFILE; nd->idx = i; nd->mode = L_S_IFREG | 0644;
        nd->size = strlen(syn_files_tab[i].text); nd->ino = 0x900001000ull + (uint64_t)i; return 0;
    }
    if ((i = ovl_find(path)) >= 0) {
        if (ov[i].gone) return -L_ENOENT;
        nd->kind = N_OVL; nd->idx = i; nd->mode = ov[i].dir ? (L_S_IFDIR | 0755) : (L_S_IFREG | 0644);
        nd->size = ov[i].size; nd->mtime = ov[i].mtime; nd->ino = 0x800000000ull + (uint64_t)i;
        for (int m = 0; m < MEMF_MAX; m++) if (mfs[m].refs && !strcmp(mfs[m].path, ov[i].path)) nd->size = mfs[m].size;
        return 0;
    }
    if (syn_dir(path) >= 0) { nd->kind = N_SYNDIR; nd->mode = L_S_IFDIR | 0755; nd->ino = 0x900002000ull + (uint64_t)syn_dir(path); return 0; }
    for (int k = 0; k < np; k++) {
        int e = pak_find(&P[k], path + 1);
        if (e < 0) continue;
        nd->kind = N_PAK; nd->pack = k; nd->idx = e; nd->mode = P[k].ent[e].mode; nd->mtime = P[k].ent[e].mtime;
        nd->size = (nd->mode & L_S_IFMT) == L_S_IFDIR ? 4096 : P[k].ent[e].b;
        nd->ino = ((uint64_t)(k + 1) << 32) | (uint32_t)e;
        return 0;
    }
    if (!strcmp(path, "/")) { nd->kind = N_SYNDIR; nd->mode = L_S_IFDIR | 0755; nd->ino = 2; return 0; }
    return -L_ENOENT;
}
static int is_dir(const node_t *n) { return (n->mode & L_S_IFMT) == L_S_IFDIR; }

static int parent_ok(const char *path) {
    char d[256];
    snprintf(d, sizeof d, "%s", path);
    char *sl = strrchr(d, '/');
    if (!sl) return -L_ENOENT;
    if (sl == d) return 0;
    *sl = 0;
    node_t nd;
    if (resolve(d, &nd)) return -L_ENOENT;
    return is_dir(&nd) ? 0 : -L_ENOTDIR;
}

/* ---- paths ------------------------------------------------------------------------------------------ */
int lxfs_path(int dirfd, const char *in, char *out) {
    char tmp[512];
    if (in[0] == '/') snprintf(tmp, sizeof tmp, "%s", in);
    else {
        const char *base = cwd;
        if (dirfd != L_AT_FDCWD) {
            if (dirfd < 0 || dirfd >= LX_MAXFD || lx_fd[dirfd].kind == FD_NONE) return -L_EBADF;
            base = lx_fd[dirfd].path;
        }
        snprintf(tmp, sizeof tmp, "%s/%s", base, in);
    }
    /* Tidy: no "//", no ".", ".." takes one off. */
    int o = 0;
    out[0] = '/';
    o = 1;
    for (const char *p = tmp; *p; ) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/') e++;
        int n = (int)(e - p);
        if (n == 1 && p[0] == '.') { p = e; continue; }
        if (n == 2 && p[0] == '.' && p[1] == '.') {
            if (o > 1) { o--; while (o > 1 && out[o - 1] != '/') o--; if (o > 1) o--; }
            if (o < 1) o = 1;
            p = e;
            continue;
        }
        if (o > 1) out[o++] = '/';
        if (o + n >= 255) return -L_ENAMETOOLONG;
        memcpy(out + o, p, (size_t)n);
        o += n;
        p = e;
    }
    out[o] = 0;
    return 0;
}

/* ---- stat --------------------------------------------------------------------------------------------- */
static void fill_stat(uint8_t *st, const node_t *nd) {
    memset(st, 0, 128);
    uint64_t dev = 0x801, nlink = is_dir(nd) ? 2 : 1, blocks = (nd->size + 511) / 512;
    uint32_t mode = nd->mode, uid = 0, blksize = 4096;
    uint64_t t = nd->mtime ? nd->mtime : 1763400000ull;
    memcpy(st + 0, &dev, 8);
    memcpy(st + 8, &nd->ino, 8);
    memcpy(st + 16, &mode, 4);
    memcpy(st + 20, &nlink, 4);
    memcpy(st + 24, &uid, 4);
    memcpy(st + 28, &uid, 4);
    memcpy(st + 48, &nd->size, 8);
    memcpy(st + 56, &blksize, 4);
    memcpy(st + 64, &blocks, 8);
    memcpy(st + 72, &t, 8);
    memcpy(st + 88, &t, 8);
    memcpy(st + 104, &t, 8);
}

int lxfs_stat(const char *path, uint8_t *st) {
    node_t nd;
    int r = resolve(path, &nd);
    if (r) return r;
    fill_stat(st, &nd);
    return 0;
}

int lxfs_fstat(lxfd_t *f, uint8_t *st) {
    node_t nd;
    memset(&nd, 0, sizeof nd);
    switch (f->kind) {
    case FD_PAK: nd.mode = L_S_IFREG | 0644; nd.size = f->size; nd.ino = f->ino; break;
    case FD_MEM: nd.mode = L_S_IFREG | 0644; nd.size = f->mf->size; nd.ino = f->ino; break;
    case FD_SYN: nd.mode = L_S_IFREG | 0444; nd.size = strlen(f->syn); nd.ino = f->ino; break;
    case FD_DIR: if (resolve(f->path, &nd)) { nd.mode = L_S_IFDIR | 0755; } break;
    case FD_SOCK: nd.mode = L_S_IFSOCK | 0777; nd.ino = 0x700000000ull + (uint64_t)f->sock; break;
    default: nd.mode = L_S_IFCHR | 0666; nd.ino = f->ino ? f->ino : 0x900000000ull + f->kind; break;
    }
    fill_stat(st, &nd);
    return 0;
}

int lxfs_access(const char *path) { node_t nd; return resolve(path, &nd); }

int lxfs_readlink(const char *path, char *out, int cap) {
    const char *t = 0;
    char b[32];
    if (!strcmp(path, "/proc/self/exe")) t = lx_exe;
    else if (!strncmp(path, "/proc/self/fd/", 14)) {
        int fd = (int)strtol(path + 14, 0, 10);
        if (fd < 0 || fd >= LX_MAXFD || lx_fd[fd].kind == FD_NONE) return -L_ENOENT;
        t = lx_fd[fd].path[0] ? lx_fd[fd].path : (snprintf(b, sizeof b, "anon:[%d]", fd), b);
    }
    if (!t) { node_t nd; return resolve(path, &nd) ? -L_ENOENT : -L_EINVAL; }
    int n = (int)strlen(t);
    if (n > cap) n = cap;
    memcpy(out, t, (size_t)n);
    return n;
}

/* ---- open, read, write, close -------------------------------------------------------------------------- */
int lxfs_open(const char *path, int flags, int mode, lxfd_t *fd) {
    (void)mode;
    memset(fd, 0, sizeof *fd);
    snprintf(fd->path, sizeof fd->path, "%s", path);
    fd->flags = (uint32_t)flags;
    fd->cloexec = (flags & L_O_CLOEXEC) != 0;
    int acc = flags & L_O_ACCMODE, wr = acc != 0 || (flags & L_O_TRUNC);
    if (lxdev_open(path, fd)) return fd->kind == FD_NONE ? -L_ENOENT : 0;
    node_t nd;
    int r = resolve(path, &nd);
    if (r == 0 && (flags & L_O_CREAT) && (flags & L_O_EXCL)) return -L_EEXIST;
    if (r == 0 && nd.kind == N_SYNFILE) {
        if (wr) return -L_EACCES;
        if (!strcmp(path, "/proc/meminfo"))
            snprintf(meminfo, sizeof meminfo, "MemTotal:       %8lu kB\nMemFree:        %8lu kB\nMemAvailable:   %8lu kB\n",
                     (unsigned long)(lx_mem_total() >> 10), (unsigned long)(lx_mem_free() >> 10), (unsigned long)(lx_mem_free() >> 10));
        fd->kind = FD_SYN; fd->syn = syn_files_tab[nd.idx].text; fd->ino = nd.ino;
        return 0;
    }
    if (r) {
        if (!(flags & L_O_CREAT)) return r;
        int pr = parent_ok(path);
        if (pr) return pr;
        int k = ovl_add(path, 0, 0);
        if (k < 0) return -L_ENOSPC;
        memf_t *m = mf_get(ov[k].path);
        if (!m) return -L_EMFILE;
        m->dirty = 1;
        fd->kind = FD_MEM; fd->mf = m; fd->ino = 0x800000000ull + (uint64_t)k;
        return 0;
    }
    if (is_dir(&nd)) {
        if (wr) return -L_EISDIR;
        fd->kind = FD_DIR; fd->ino = nd.ino;
        return 0;
    }
    if (flags & L_O_DIRECTORY) return -L_ENOTDIR;
    const char *opath = nd.kind == N_OVL ? ov[nd.idx].path : path;
    /* Already open: the same bytes. */
    for (int i = 0; i < MEMF_MAX; i++)
        if (mfs[i].refs && !strcmp(mfs[i].path, opath)) {
            memf_t *m = mf_get(opath);
            if (flags & L_O_TRUNC) { m->size = 0; m->dirty = 1; }
            fd->kind = FD_MEM; fd->mf = m; fd->ino = nd.ino;
            return 0;
        }
    if (nd.kind == N_PAK && !wr) {
        fd->kind = FD_PAK; fd->pack = nd.pack; fd->ent = (uint32_t)nd.idx;
        fd->off = P[nd.pack].ent[nd.idx].a; fd->size = P[nd.pack].ent[nd.idx].b; fd->ino = nd.ino;
        return 0;
    }
    /* Into memory: an overlay file from the card, or a pack file about to change. */
    memf_t *m = 0;
    if (nd.kind == N_OVL) {
        m = mf_get(opath);
        if (!m) return -L_EMFILE;
        if (!(flags & L_O_TRUNC) && nd.size) {
            if (mf_reserve(m, nd.size + 1)) { mf_put(m); return -L_ENOMEM; }
            lx_rq_t rq = { .kind = RQ_FSREAD, .buf = m->data, .len = (uint32_t)m->cap };
            card_path(opath, rq.path);
            int got = lx_request(&rq);
            if (got < 0) { mf_put(m); return got; }
            m->size = (uint64_t)got < m->cap ? (uint64_t)got : m->cap;
        }
    } else if (nd.kind == N_PAK) {
        int k = ovl_add(path, 0, nd.size);
        if (k < 0) return -L_ENOSPC;
        m = mf_get(ov[k].path);
        if (!m) return -L_EMFILE;
        if (!(flags & L_O_TRUNC) && nd.size) {
            if (mf_reserve(m, nd.size)) { mf_put(m); return -L_ENOMEM; }
            int64_t got = pak_read(nd.pack, P[nd.pack].ent[nd.idx].a, m->data, nd.size);
            if (got < 0) { mf_put(m); return (int)got; }
            m->size = (uint64_t)got;
        }
        m->dirty = 1;                                   /* now the overlay's: on the card */
        nd.ino = 0x800000000ull + (uint64_t)k;
    } else return -L_EACCES;
    if (flags & L_O_TRUNC) { m->size = 0; m->dirty = 1; }
    fd->kind = FD_MEM; fd->mf = m; fd->ino = nd.ino;
    return 0;
}

int64_t lxfs_read(lxfd_t *f, void *buf, uint64_t n, uint64_t at) {
    switch (f->kind) {
    case FD_PAK:
        if (at >= f->size) return 0;
        if (n > f->size - at) n = f->size - at;
        return pak_read(f->pack, f->off + at, buf, n);
    case FD_MEM:
        if (at >= f->mf->size) return 0;
        if (n > f->mf->size - at) n = f->mf->size - at;
        memcpy(buf, f->mf->data + at, n);
        return (int64_t)n;
    case FD_SYN: {
        uint64_t len = strlen(f->syn);
        if (at >= len) return 0;
        if (n > len - at) n = len - at;
        memcpy(buf, f->syn + at, n);
        return (int64_t)n;
    }
    case FD_DIR: return -L_EISDIR;
    default: return -L_EINVAL;
    }
}

int64_t lxfs_write(lxfd_t *f, const void *buf, uint64_t n, uint64_t at) {
    if (f->kind != FD_MEM) return f->kind == FD_DIR ? -L_EISDIR : -L_EBADF;
    memf_t *m = f->mf;
    if (at + n > m->cap) { int r = mf_reserve(m, at + n); if (r) return r; }
    if (at > m->size) memset(m->data + m->size, 0, at - m->size);
    memcpy(m->data + at, buf, n);
    if (at + n > m->size) m->size = at + n;
    m->dirty = 1;
    return (int64_t)n;
}

int lxfs_truncate(lxfd_t *f, uint64_t size) {
    if (f->kind != FD_MEM) return -L_EINVAL;
    memf_t *m = f->mf;
    if (size > m->cap) { int r = mf_reserve(m, size); if (r) return r; }
    if (size > m->size) memset(m->data + m->size, 0, size - m->size);
    m->size = size; m->dirty = 1;
    return 0;
}

int lxfs_close(lxfd_t *f) {
    if (f->kind == FD_MEM && f->mf) {
        int k = ovl_find(f->mf->path);
        if (k >= 0) ov[k].size = f->mf->size;
        mf_put(f->mf);
    }
    memset(f, 0, sizeof *f);
    return 0;
}

int lxfs_sync(lxfd_t *f) { return f->kind == FD_MEM ? mf_flush(f->mf) : 0; }

uint64_t lxfs_size(lxfd_t *f) {
    switch (f->kind) {
    case FD_PAK: return f->size;
    case FD_MEM: return f->mf->size;
    case FD_SYN: return strlen(f->syn);
    }
    return 0;
}

void lxfs_dup(const lxfd_t *from, lxfd_t *to) {
    *to = *from;
    if (to->kind == FD_MEM && to->mf) to->mf->refs++;
}

/* ---- folders ---------------------------------------------------------------------------------------------- */
typedef void (*dir_fn)(void *ctx, const char *name, int isdir, uint64_t ino);

static int child_of(const char *dir, const char *path, const char **name) {
    size_t n = strlen(dir);
    if (n == 1) { if (path[0] != '/' || !path[1] || strchr(path + 1, '/')) return 0; *name = path + 1; return 1; }
    if (strncasecmp(path, dir, n) || path[n] != '/' || !path[n + 1] || strchr(path + n + 1, '/')) return 0;
    *name = path + n + 1;
    return 1;
}

typedef struct { const char *dir; dir_fn fn; void *ctx; } syn_ctx;
static void syn_dev_cb(void *c, const char *name, int isdir) {
    syn_ctx *s = c;
    s->fn(s->ctx, name, isdir, 0x900003000ull + hash_s(name, 0) % 4096);
}

static void dir_each(const char *dir, dir_fn fn, void *ctx) {
    char full[512];
    fn(ctx, ".", 1, 1);
    fn(ctx, "..", 1, 1);
    /* Mini's own */
    if (!strcmp(dir, "/")) { fn(ctx, "dev", 1, 3); fn(ctx, "etc", 1, 4); fn(ctx, "proc", 1, 5); fn(ctx, "tmp", 1, 6); }
    if (!strcmp(dir, "/proc")) fn(ctx, "self", 1, 7);
    if (!strcmp(dir, "/proc/self")) { fn(ctx, "exe", 0, 8); fn(ctx, "maps", 0, 9); }
    for (int i = 0; syn_files_tab[i].path; i++) {
        const char *nm;
        if (child_of(dir, syn_files_tab[i].path, &nm) && strcmp(dir, "/proc/self")) fn(ctx, nm, 0, 0x900001000ull + (uint64_t)i);
    }
    syn_ctx sc = { dir, fn, ctx };
    lxdev_list(dir, syn_dev_cb, &sc);
    /* the overlay */
    for (int i = 0; i < nov; i++) {
        const char *nm;
        if (!ov[i].path || ov[i].gone || !child_of(dir, ov[i].path, &nm)) continue;
        snprintf(full, sizeof full, "%s", ov[i].path);
        if (syn_file(full) >= 0 || syn_dir(full) >= 0) continue;
        fn(ctx, nm, ov[i].dir, 0x800000000ull + (uint64_t)i);
    }
    /* the packs, each name once */
    for (int k = 0; k < np; k++) {
        int e = pak_find(&P[k], dir + 1);
        if (e < 0 || (P[k].ent[e].mode & L_S_IFMT) != L_S_IFDIR) continue;
        uint64_t first = P[k].ent[e].a, cnt = P[k].ent[e].b;
        for (uint64_t c = first; c < first + cnt && c < P[k].n; c++) {
            const char *pn = ent_name(&P[k], (uint32_t)c);
            const char *nm = strrchr(pn, '/');
            nm = nm ? nm + 1 : pn;
            snprintf(full, sizeof full, "%s%s%s", dir, dir[1] ? "/" : "", nm);
            if (ovl_find(full) >= 0 || syn_dir(full) >= 0 || syn_file(full) >= 0) continue;
            int dup = 0;
            for (int j = 0; j < k && !dup; j++) dup = pak_find(&P[j], full + 1) >= 0;
            if (dup) continue;
            fn(ctx, nm, (P[k].ent[c].mode & L_S_IFMT) == L_S_IFDIR, ((uint64_t)(k + 1) << 32) | c);
        }
    }
}

typedef struct { uint8_t *buf; uint64_t cap, used; uint64_t skip, idx; int full; } gd_ctx;
static void gd_cb(void *c, const char *name, int isdir, uint64_t ino) {
    gd_ctx *g = c;
    if (g->full) return;
    uint64_t i = g->idx++;
    if (i < g->skip) return;
    size_t nl = strlen(name);
    uint64_t rec = (19 + nl + 1 + 7) & ~7ull;
    if (g->used + rec > g->cap) { g->full = 1; g->idx--; return; }
    uint8_t *d = g->buf + g->used;
    int64_t off = (int64_t)i + 1;
    uint16_t rl = (uint16_t)rec;
    memcpy(d, &ino, 8); memcpy(d + 8, &off, 8); memcpy(d + 16, &rl, 2);
    d[18] = isdir ? 4 : 8;                              /* DT_DIR, DT_REG */
    memcpy(d + 19, name, nl + 1);
    for (uint64_t z = 19 + nl + 1; z < rec; z++) d[z] = 0;
    g->used += rec;
}

int lxfs_getdents(lxfd_t *f, uint8_t *buf, uint64_t cap) {
    if (f->kind != FD_DIR) return -L_ENOTDIR;
    gd_ctx g = { buf, cap, 0, f->pos, 0, 0 };
    dir_each(f->path, gd_cb, &g);
    if (!g.used && g.full) return -L_EINVAL;
    f->pos = g.idx;
    return (int)g.used;
}

int lxfs_mkdir(const char *path) {
    node_t nd;
    if (!resolve(path, &nd)) return -L_EEXIST;
    int pr = parent_ok(path);
    if (pr) return pr;
    if (ovl_add(path, 1, 0) < 0) return -L_ENOSPC;
    lx_rq_t rq = { .kind = RQ_MKDIR };
    card_path(path, rq.path);
    lx_request(&rq);
    return 0;
}

static void count_cb(void *c, const char *name, int isdir, uint64_t ino) {
    (void)isdir; (void)ino;
    if (strcmp(name, ".") && strcmp(name, "..")) (*(int *)c)++;
}

int lxfs_unlink(const char *path, int dir) {
    node_t nd;
    int r = resolve(path, &nd);
    if (r) return r;
    if (nd.kind != N_OVL && nd.kind != N_PAK) return -L_EACCES;
    if (dir && !is_dir(&nd)) return -L_ENOTDIR;
    if (!dir && is_dir(&nd)) return -L_EISDIR;
    if (dir) {
        int cnt = 0;
        dir_each(path, count_cb, &cnt);
        if (cnt) return -L_ENOTEMPTY;
    }
    int in_pack = 0;
    for (int k = 0; k < np; k++) if (pak_find(&P[k], path + 1) >= 0) in_pack = 1;
    if (nd.kind == N_OVL) {
        lx_rq_t rq = { .kind = RQ_REMOVE };
        card_path(ov[nd.idx].path, rq.path);
        lx_request(&rq);
        for (int i = 0; i < MEMF_MAX; i++)
            if (mfs[i].refs && !strcmp(mfs[i].path, ov[nd.idx].path)) mfs[i].dirty = 0;   /* nothing to save any more */
        if (in_pack) ov[nd.idx].gone = 1; else ov[nd.idx].path = 0;
    } else {
        int k = ovl_add(path, is_dir(&nd), 0);
        if (k >= 0) ov[k].gone = 1;
    }
    return 0;
}

int lxfs_rename(const char *from, const char *to) {
    node_t nd;
    int r = resolve(from, &nd);
    if (r) return r;
    if (is_dir(&nd)) return -L_EXDEV;
    if (nd.kind != N_OVL && nd.kind != N_PAK) return -L_EACCES;
    int pr = parent_ok(to);
    if (pr) return pr;
    /* The bytes into memory under the new name, saved now; the old one goes. */
    lxfd_t a, b;
    r = lxfs_open(from, 0, 0, &a);
    if (r) return r;
    node_t tn;
    if (!resolve(to, &tn) && is_dir(&tn)) { lxfs_close(&a); return -L_EISDIR; }
    r = lxfs_open(to, L_O_WRONLY | L_O_CREAT | L_O_TRUNC, 0644, &b);
    if (r) { lxfs_close(&a); return r; }
    uint64_t size = a.kind == FD_PAK ? a.size : a.mf->size;
    if (mf_reserve(b.mf, size + 1)) { lxfs_close(&a); lxfs_close(&b); return -L_ENOSPC; }
    int64_t got = lxfs_read(&a, b.mf->data, size, 0);
    b.mf->size = got > 0 ? (uint64_t)got : 0;
    b.mf->dirty = 1;
    mf_flush(b.mf);
    lxfs_close(&a);
    lxfs_close(&b);
    return lxfs_unlink(from, 0);
}

/* ---- where code came from (crash reports) ------------------------------------------------------------------- */
static struct { char path[96]; uint64_t base, len; } maps[64];
static int nmaps;
void lxfs_note_map(const char *path, uint64_t base, uint64_t len) {
    for (int i = 0; i < nmaps; i++) if (maps[i].base == base) { snprintf(maps[i].path, sizeof maps[i].path, "%s", path); maps[i].len = len; return; }
    if (nmaps >= 64) return;
    snprintf(maps[nmaps].path, sizeof maps[nmaps].path, "%s", path);
    maps[nmaps].base = base; maps[nmaps].len = len;
    nmaps++;
}
const char *lxfs_where(uint64_t pc, uint64_t *off) {
    for (int i = 0; i < nmaps; i++)
        if (pc >= maps[i].base && pc < maps[i].base + maps[i].len) { *off = pc - maps[i].base; return maps[i].path; }
    return 0;
}

/* ---- the boot core: before and after ------------------------------------------------------------------------------ */
lxfd_t *lx_fd;

int lxfs_init(const lx_config_t *c, char *why, int cap) {
    lxfs_free();
    if (store_open(0)) { snprintf(why, (size_t)cap, "the card's store isn't readable: %s", store_why()); return -1; }
    for (int i = 0; i < LX_MAXPACK && c->packs[i]; i++) {
        if (pak_load(&P[np], c->packs[i], why, cap)) { np++; lxfs_free(); return -1; }
        np++;
    }
    if (!np) { snprintf(why, (size_t)cap, "no packs to run from"); return -1; }
    lx_fd = kmalloc(sizeof(lxfd_t) * LX_MAXFD);
    mfs = kmalloc(sizeof(memf_t) * MEMF_MAX);
    ov = kmalloc(sizeof(ovl_t) * OVL_MAX);
    ncb = c->cache_mb * 4;
    cb = kmalloc(sizeof(cblk_t) * (uint64_t)ncb);
    if (!lx_fd || !mfs || !ov || !cb) { snprintf(why, (size_t)cap, "out of memory"); lxfs_free(); return -1; }
    memset(lx_fd, 0, sizeof(lxfd_t) * LX_MAXFD);
    memset(mfs, 0, sizeof(memf_t) * MEMF_MAX);
    memset(ov, 0, sizeof(ovl_t) * OVL_MAX);
    memset(cb, 0, sizeof(cblk_t) * (uint64_t)ncb);
    nov = 0; nmaps = 0; ctick = c_hits = c_miss = c_direct = c_bytes = 0;
    snprintf(rwroot, sizeof rwroot, "%s", c->rw);
    while (rwroot[0] && rwroot[strlen(rwroot) - 1] == '/') rwroot[strlen(rwroot) - 1] = 0;
    snprintf(cwd, sizeof cwd, "%s", c->cwd);
    lx_exe[0] = 0;
    if (fs_ready()) { fs_mkdir(rwroot); scan(rwroot, "", 0); }
    char dns[16] = "1.1.1.1";
    if (inet_dns_server()) net_fmt_ip(inet_dns_server(), dns);
    snprintf(resolv, sizeof resolv, "nameserver %s\n", dns);
    /* This machine's name (uname) is its address on the way out. */
    char me[16] = "127.0.0.1";
    uint32_t mine = net_ip_of(0x01010101u);
    if (mine) net_fmt_ip(mine, me);
    snprintf(hosts, sizeof hosts, "127.0.0.1 localhost\n::1 localhost\n%s boltos-mini\n", me);
    uint64_t midr; __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    snprintf(cpuinfo, sizeof cpuinfo, "processor\t: 0\nBogoMIPS\t: 48.00\nFeatures\t: fp asimd evtstrm crc32 cpuid\n"
             "CPU implementer\t: 0x%02lx\nCPU architecture: 8\nCPU variant\t: 0x%lx\nCPU part\t: 0x%03lx\nCPU revision\t: %lu\n\n",
             (unsigned long)(midr >> 24 & 0xFF), (unsigned long)(midr >> 20 & 15), (unsigned long)(midr >> 4 & 0xFFF),
             (unsigned long)(midr & 15));
    /* stdin, stdout, stderr: the console */
    for (int i = 0; i < 3; i++) { lx_fd[i].kind = FD_CON; lx_fd[i].flags = i ? 1 : 0; snprintf(lx_fd[i].path, sizeof lx_fd[i].path, "/dev/console"); }
    int nfiles = 0;
    for (int k = 0; k < np; k++) nfiles += (int)P[k].n;
    kprintf("[lx] %d pack%s (%d entries), %d of the program's own files on the card in %s\n", np, np == 1 ? "" : "s",
            nfiles, nov, rwroot);
    return 0;
}

void lxfs_free(void) {
    for (int k = 0; k < np; k++) { kfree(P[k].raw); kfree(P[k].hx); kfree(P[k].hl); }
    memset(P, 0, sizeof P);
    np = 0;
    if (lx_fd) { kfree(lx_fd); lx_fd = 0; }
    if (mfs) { kfree(mfs); mfs = 0; }
    if (ov) { kfree(ov); ov = 0; }
    if (cb) { kfree(cb); cb = 0; }
    nov = 0; ncb = 0;
}

int lxfs_debug(char *out, int cap) {
    int n = 0;
    for (int k = 0; k < np && n < cap - 100; k++)
        n += snprintf(out + n, (size_t)(cap - n), "pack %s: %u entries, %lu MiB\n", P[k].name, P[k].n, (unsigned long)(P[k].size >> 20));
    if (cb) n += snprintf(out + n, (size_t)(cap - n), "cache: %d blocks of 256 KiB, %lu hits, %lu misses, %lu direct, %lu MiB read\n",
                          ncb, (unsigned long)c_hits, (unsigned long)c_miss, (unsigned long)c_direct, (unsigned long)(c_bytes >> 20));
    if (ov) {
        int open = 0, dirty = 0;
        for (int i = 0; i < MEMF_MAX; i++) if (mfs[i].refs) { open++; dirty += mfs[i].dirty; }
        n += snprintf(out + n, (size_t)(cap - n), "overlay: %d entries in %s, %d files in memory (%d unsaved)\n", nov, rwroot, open, dirty);
    }
    return n;
}

/* chdir (lx_sys.c) */
int lxfs_chdir(const char *path) {
    node_t nd;
    int r = resolve(path, &nd);
    if (r) return r;
    if (!is_dir(&nd)) return -L_ENOTDIR;
    snprintf(cwd, sizeof cwd, "%s", path);
    return 0;
}
const char *lxfs_cwd(void) { return cwd; }
