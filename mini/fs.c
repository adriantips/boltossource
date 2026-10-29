/* ===========================================================================
 *  BoltOS Mini  -  mini/fs.c
 *
 *  Files and folders on the microSD card: the FAT32 partition U-Boot loads
 *  BOLTOS.BIN from, which is also what a Mac shows when the card is in its
 *  reader. The Code editor keeps its files in /Code, Notes in /Notes, and
 *  what has to outlive a power cut (the Wi-Fi network, Bluetooth pairings,
 *  the theme) in /System.
 *
 *  It's FAT32 as every other system writes it: long names (made with an 8.3
 *  alias and checksum, as Windows does), both FATs kept the same, FSInfo's
 *  hints kept up. A file is written into new clusters first and only then
 *  pointed to, and its old clusters freed last, so a power cut mid-save
 *  leaves the old file whole (and, at worst, some clusters nobody uses).
 *
 *  BOLTOS.BIN is the one file nothing here will remove, rename or replace:
 *  without it the Pi doesn't start. pi.py install replaces it through U-Boot.
 *
 *  A directory is read whole into memory, changed there, and only the
 *  sectors that changed are written back. FAT sectors go through a small
 *  write-back cache that each operation flushes before it returns.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "fs.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

int      sd_init(void);
uint64_t sd_sectors(void);
int      sd_read(uint64_t lba, void *buf, uint32_t n);
int      sd_write(uint64_t lba, const void *buf, uint32_t n);

#define SEC        512u
#define EOC        0x0FFFFFF8u
#define A_RO       0x01
#define A_HIDDEN   0x02
#define A_SYSTEM   0x04
#define A_VOLID    0x08
#define A_DIR      0x10
#define A_ARCH     0x20
#define A_LFN      0x0F

static struct {
    int      state;                        /* 0 not tried, 1 mounted, -1 failed  */
    uint64_t base;                         /* the partition's first sector       */
    uint32_t spc, fat_start, fat_size, nfats, data_start, root, nclus, cbytes;
    uint32_t hint;                         /* where to look for a free cluster   */
    uint32_t fsinfo;                       /* its sector, 0 if none              */
    int64_t  nfree;                        /* -1 until counted                   */
    char     label[12];
    char     why[96];
    uint32_t reads, writes;
} V;

static int fail(const char *fmt, const char *arg) {
    snprintf(V.why, sizeof V.why, fmt, arg ? arg : "");
    return -1;
}

static int rd(uint32_t lba, void *buf, uint32_t n) { V.reads += n; return sd_read(V.base + lba, buf, n); }
static int wr(uint32_t lba, const void *buf, uint32_t n) { V.writes += n; return sd_write(V.base + lba, buf, n); }
static uint32_t clus_sec(uint32_t c) { return V.data_start + (c - 2) * V.spc; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

/* Sector I/O must be 4-byte aligned on the Pi: files go through this. */
#define BOUNCE (64 * 1024)
static uint8_t *bounce;
static uint8_t *bounce_buf(void) {
    if (!bounce) bounce = kmalloc(BOUNCE);
    return bounce;
}

/* ---- the FAT, through a small write-back cache -------------------------------------------- */
#define FC 8
static struct { uint32_t sec; int dirty; uint32_t used; uint8_t b[SEC] __attribute__((aligned(8))); } fc[FC];
static uint32_t fc_clock;

static uint8_t *fat_sector(uint32_t sec) {
    int lru = 0;
    for (int i = 0; i < FC; i++) {
        if (fc[i].used && fc[i].sec == sec) { fc[i].used = ++fc_clock; return fc[i].b; }
        if (fc[i].used < fc[lru].used) lru = i;
    }
    if (fc[lru].used && fc[lru].dirty) {
        for (uint32_t f = 0; f < V.nfats; f++)
            if (wr(V.fat_start + f * V.fat_size + fc[lru].sec, fc[lru].b, 1)) return 0;
        fc[lru].dirty = 0;
    }
    fc[lru].used = 0;
    if (rd(V.fat_start + sec, fc[lru].b, 1)) return 0;
    fc[lru].sec = sec; fc[lru].dirty = 0; fc[lru].used = ++fc_clock;
    return fc[lru].b;
}

static int fat_get(uint32_t c, uint32_t *v) {
    if (c < 2 || c >= V.nclus + 2) return -1;
    uint8_t *b = fat_sector(c / 128);
    if (!b) return -1;
    *v = le32(b + (c % 128) * 4) & 0x0FFFFFFFu;
    return 0;
}

static int fat_set(uint32_t c, uint32_t v) {
    if (c < 2 || c >= V.nclus + 2) return -1;
    uint8_t *b = fat_sector(c / 128);
    if (!b) return -1;
    uint32_t old = le32(b + (c % 128) * 4);
    put32(b + (c % 128) * 4, (old & 0xF0000000u) | (v & 0x0FFFFFFFu));
    for (int i = 0; i < FC; i++) if (fc[i].b == b) fc[i].dirty = 1;
    if (V.nfree >= 0) {
        if ((old & 0x0FFFFFFFu) == 0 && v) V.nfree--;
        else if ((old & 0x0FFFFFFFu) && !v) V.nfree++;
    }
    return 0;
}

static int flush_cache(void) {
    int err = 0;
    for (int i = 0; i < FC; i++) {
        if (!fc[i].used || !fc[i].dirty) continue;
        for (uint32_t f = 0; f < V.nfats; f++)
            if (wr(V.fat_start + f * V.fat_size + fc[i].sec, fc[i].b, 1)) err = -1;
        if (!err) fc[i].dirty = 0;
    }
    return err;
}

static void count_free(void);

static int fat_flush(void) {
    int err = flush_cache();
    if (V.nfree < 0) count_free();               /* FSInfo should say, as fsck expects */
    /* FSInfo: the next-free hint, and the free count once it's known. */
    if (V.fsinfo) {
        static uint8_t s[SEC] __attribute__((aligned(8)));
        if (rd(V.fsinfo, s, 1) == 0 && le32(s) == 0x41615252u && le32(s + 484) == 0x61417272u) {
            uint32_t want = V.nfree >= 0 ? (uint32_t)V.nfree : 0xFFFFFFFFu;
            if (le32(s + 488) != want || le32(s + 492) != V.hint) {
                put32(s + 488, want); put32(s + 492, V.hint);
                wr(V.fsinfo, s, 1);
            }
        }
    }
    return err;
}

/* A free cluster, marked as the end of a chain and linked after `prev`. */
static uint32_t alloc_after(uint32_t prev) {
    uint32_t last = V.nclus + 2, start = V.hint >= 2 && V.hint < last ? V.hint : 2;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t from = pass ? 2 : start, to = pass ? start : last;
        for (uint32_t c = from; c < to; c++) {
            uint32_t v;
            if (fat_get(c, &v)) return 0;
            if (v) continue;
            if (fat_set(c, 0x0FFFFFFFu)) return 0;
            if (prev && fat_set(prev, c)) return 0;
            V.hint = c + 1;
            return c;
        }
    }
    return 0;
}

static void free_chain(uint32_t c) {
    for (int guard = 0; c >= 2 && c < EOC && guard < (int)V.nclus; guard++) {
        uint32_t next;
        if (fat_get(c, &next)) return;
        fat_set(c, 0);
        if (c < V.hint) V.hint = c;
        c = next;
    }
}

/* ---- mounting ------------------------------------------------------------------------------ */
int fs_ready(void) { return V.state == 1; }
const char *fs_why(void) { return V.why; }

int fs_mount(void) {
    if (V.state == 1) return 0;
    if (V.state < 0) return -1;
    V.state = -1;
    if (sd_init() != 0) return fail("The SD card isn't answering", 0);
    static uint8_t s[SEC] __attribute__((aligned(8)));
    if (sd_read(0, s, 1) || s[510] != 0x55 || s[511] != 0xAA) return fail("The SD card has no partition table", 0);
    uint64_t base = 0;
    for (int i = 0; i < 4 && !base; i++) {
        const uint8_t *p = s + 446 + 16 * i;
        if (p[4] == 0x0B || p[4] == 0x0C || p[4] == 0xEF) base = le32(p + 8);
    }
    if (!base) return fail("The SD card has no FAT32 partition", 0);
    if (sd_read(base, s, 1) || s[510] != 0x55 || s[511] != 0xAA) return fail("The FAT partition can't be read", 0);
    if (le16(s + 11) != SEC || !s[13] || le16(s + 17) != 0 || !le32(s + 36))
        return fail("The card's first partition isn't FAT32", 0);
    V.base = base;
    V.spc = s[13];
    V.nfats = s[16] ? s[16] : 1;
    V.fat_start = le16(s + 14);
    V.fat_size = le32(s + 36);
    V.root = le32(s + 44);
    uint32_t total = le16(s + 19) ? le16(s + 19) : le32(s + 32);
    V.data_start = V.fat_start + V.nfats * V.fat_size;
    V.nclus = (total - V.data_start) / V.spc;
    V.cbytes = V.spc * SEC;
    V.fsinfo = le16(s + 48);
    if (V.fsinfo == 0 || V.fsinfo == 0xFFFF) V.fsinfo = 0;
    memcpy(V.label, s + 71, 11); V.label[11] = 0;
    for (int i = 10; i >= 0 && V.label[i] == ' '; i--) V.label[i] = 0;
    if (V.nclus < 65525 || V.root < 2 || V.cbytes > 65536) return fail("The card's first partition isn't FAT32", 0);
    V.hint = 2; V.nfree = -1;
    if (V.fsinfo && sd_read(base + V.fsinfo, s, 1) == 0 && le32(s) == 0x41615252u) {
        uint32_t h = le32(s + 492);
        if (h >= 2 && h < V.nclus + 2) V.hint = h;
    }
    V.state = 1;
    kprintf("[fs] the card's FAT32 \"%s\": %lu MiB, %u-byte clusters\n", V.label,
            (unsigned long)((uint64_t)V.nclus * V.cbytes >> 20), V.cbytes);
    return 0;
}

/* ---- names ---------------------------------------------------------------------------------- */
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int name_eq(const char *a, const char *b) {
    while (*a && *b) if (lower((unsigned char)*a++) != lower((unsigned char)*b++)) return 0;
    return !*a && !*b;
}

static int name_ok(const char *n) {
    int len = (int)strlen(n);
    if (!len || len >= FS_NAME || !strcmp(n, ".") || !strcmp(n, "..")) return 0;
    for (const char *p = n; *p; p++)
        if ((unsigned char)*p < 32 || strchr("\\/:*?\"<>|", *p)) return 0;
    return n[len - 1] != ' ' && n[len - 1] != '.';
}

static void short_to_str(const uint8_t *e, char *out) {
    int o = 0;
    int lb = e[12] & 0x08, le = e[12] & 0x10;           /* Windows' all-lowercase flags */
    for (int i = 0; i < 8 && e[i] != ' '; i++) out[o++] = (char)(lb ? lower(e[i]) : e[i]);
    if (e[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) out[o++] = (char)(le ? lower(e[i]) : e[i]);
    }
    out[o] = 0;
    if ((uint8_t)out[0] == 0x05) out[0] = (char)0xE5;
}

static int short_char(int c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("!#$%&'()-@^_`{}~", c)) return c;
    return c >= 128 ? c : '_';
}

/* Is the name its own 8.3 alias (upper case, 8 and 3)? Then it needs no long entries. */
static int is_short(const char *n, uint8_t out[11]) {
    memset(out, ' ', 11);
    const char *dot = strrchr(n, '.');
    int bl = dot ? (int)(dot - n) : (int)strlen(n), el = dot ? (int)strlen(dot + 1) : 0;
    if (bl < 1 || bl > 8 || el > 3 || (dot && !el)) return 0;
    for (int i = 0; i < bl; i++) { if (n[i] == '.' || short_char((unsigned char)n[i]) != (unsigned char)n[i]) return 0; out[i] = (uint8_t)n[i]; }
    for (int i = 0; i < el; i++) { if (short_char((unsigned char)dot[1 + i]) != (unsigned char)dot[1 + i]) return 0; out[8 + i] = (uint8_t)dot[1 + i]; }
    return 1;
}

static uint8_t lfn_sum(const uint8_t *s) {
    uint8_t c = 0;
    for (int i = 0; i < 11; i++) c = (uint8_t)(((c & 1) << 7) + (c >> 1) + s[i]);
    return c;
}

static const int lfn_off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

/* ---- directories, read whole ------------------------------------------------------------------ */
typedef struct {
    uint32_t first;
    uint32_t *clus; int nclus;
    uint8_t *buf; uint32_t bytes;           /* every entry, in order          */
} dir_t;

static void dir_free(dir_t *d) { kfree(d->clus); kfree(d->buf); memset(d, 0, sizeof *d); }

static int dir_load(uint32_t first, dir_t *d) {
    memset(d, 0, sizeof *d);
    d->first = first;
    int cap = 8;
    d->clus = kmalloc(sizeof(uint32_t) * (size_t)cap);
    if (!d->clus) return fail("Out of memory", 0);
    for (uint32_t c = first; c >= 2 && c < EOC; ) {
        if (d->nclus == cap) {
            uint32_t *n = kmalloc(sizeof(uint32_t) * (size_t)cap * 2);
            if (!n) { dir_free(d); return fail("Out of memory", 0); }
            memcpy(n, d->clus, sizeof(uint32_t) * (size_t)cap); kfree(d->clus); d->clus = n; cap *= 2;
        }
        d->clus[d->nclus++] = c;
        if (d->nclus > 65536 / 32 * 8) { dir_free(d); return fail("A folder's chain loops", 0); }
        if (fat_get(c, &c)) { dir_free(d); return fail("The card can't be read", 0); }
    }
    d->bytes = (uint32_t)d->nclus * V.cbytes;
    d->buf = kmalloc(d->bytes ? d->bytes : 1);
    if (!d->buf) { dir_free(d); return fail("Out of memory", 0); }
    for (int i = 0; i < d->nclus; i++)
        if (rd(clus_sec(d->clus[i]), d->buf + (uint32_t)i * V.cbytes, V.spc)) { dir_free(d); return fail("The card can't be read", 0); }
    return 0;
}

/* Write back the sectors holding entries [from, from + n). */
static int dir_put(dir_t *d, int from, int n) {
    uint32_t a = (uint32_t)from * 32 / SEC, b = ((uint32_t)(from + n) * 32 + SEC - 1) / SEC;
    for (uint32_t s = a; s < b; s++) {
        uint32_t ci = s / V.spc, si = s % V.spc;
        if ((int)ci >= d->nclus) return -1;
        if (wr(clus_sec(d->clus[ci]) + si, d->buf + s * SEC, 1)) return fail("The SD card won't take a write", 0);
    }
    return 0;
}

/* One more cluster, zeroed, on the end. */
static int dir_grow(dir_t *d) {
    uint32_t c = alloc_after(d->nclus ? d->clus[d->nclus - 1] : 0);
    if (!c) return fail("The card is full", 0);
    uint8_t *nb = kmalloc(d->bytes + V.cbytes);
    uint32_t *nc = kmalloc(sizeof(uint32_t) * (size_t)(d->nclus + 1));
    if (!nb || !nc) { kfree(nb); kfree(nc); return fail("Out of memory", 0); }
    memcpy(nb, d->buf, d->bytes); memset(nb + d->bytes, 0, V.cbytes);
    memcpy(nc, d->clus, sizeof(uint32_t) * (size_t)d->nclus); nc[d->nclus] = c;
    kfree(d->buf); kfree(d->clus);
    d->buf = nb; d->clus = nc; d->nclus++; d->bytes += V.cbytes;
    for (uint32_t s = 0; s < V.spc; s++)
        if (wr(clus_sec(c) + s, nb + d->bytes - V.cbytes + s * SEC, 1)) return fail("The SD card won't take a write", 0);
    return 0;
}

/* An entry found in a directory: its short entry's index, and the first of
 * its long-name entries (== idx when it has none). */
typedef struct { int idx, lfn; char name[FS_NAME]; } found_t;

/* Walk a loaded directory. cb returns nonzero to stop; that value is returned. */
typedef int (*walk_cb)(dir_t *d, const found_t *f, void *ctx);
static int dir_walk(dir_t *d, walk_cb cb, void *ctx) {
    int n = (int)(d->bytes / 32);
    char lfn[FS_NAME + 16]; int lfn_ok = 0, lfn_first = -1, lfn_next = 0; uint8_t lfn_ck = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *e = d->buf + i * 32;
        if (e[0] == 0) break;
        if (e[0] == 0xE5) { lfn_ok = 0; continue; }
        if ((e[11] & 0x3F) == A_LFN) {
            int seq = e[0] & 0x1F;
            if (e[0] & 0x40) {
                if (seq < 1 || seq * 13 > FS_NAME + 12) { lfn_ok = 0; continue; }
                memset(lfn, 0, sizeof lfn); lfn_ok = 1; lfn_first = i; lfn_next = seq; lfn_ck = e[13];
            } else if (!lfn_ok || seq != lfn_next || e[13] != lfn_ck) { lfn_ok = 0; continue; }
            if (lfn_ok) {
                for (int k = 0; k < 13; k++) {
                    uint16_t ch = le16(e + lfn_off[k]);
                    if (ch == 0 || ch == 0xFFFF) break;
                    lfn[(seq - 1) * 13 + k] = (char)(ch < 256 ? ch : '?');
                }
                lfn_next = seq - 1;
            }
            continue;
        }
        if (e[11] & A_VOLID) { lfn_ok = 0; continue; }
        found_t f;
        f.idx = i;
        if (lfn_ok && lfn_next == 0 && lfn_ck == lfn_sum(e) && lfn[0]) {
            f.lfn = lfn_first;
            snprintf(f.name, sizeof f.name, "%s", lfn);
        } else {
            f.lfn = i;
            short_to_str(e, f.name);
        }
        lfn_ok = 0;
        int r = cb(d, &f, ctx);
        if (r) return r;
    }
    return 0;
}

typedef struct { const char *want; found_t got; } find_ctx;
static int find_cb(dir_t *d, const found_t *f, void *ctx) {
    (void)d;
    find_ctx *c = ctx;
    if (!name_eq(f->name, c->want)) return 0;
    c->got = *f;
    return 1;
}
static int dir_find(dir_t *d, const char *name, found_t *out) {
    find_ctx c = { name, { 0 } };
    if (dir_walk(d, find_cb, &c) != 1) return -1;
    if (out) *out = c.got;
    return 0;
}

static uint32_t ent_clus(const uint8_t *e) { return (uint32_t)le16(e + 20) << 16 | le16(e + 26); }
static void set_clus(uint8_t *e, uint32_t c) { put16(e + 20, c >> 16); put16(e + 26, c); }

static void stamp(uint8_t *e, int created) {
    uint16_t date = (1 << 5) | 1, time = 0;                /* 1980-01-01, the clock unknown */
    if (clock_known()) {
        int y, mo, d, h, mi, s, wd;
        clock_local(&y, &mo, &d, &h, &mi, &s, &wd);
        if (y >= 1980) { date = (uint16_t)((y - 1980) << 9 | mo << 5 | d); time = (uint16_t)(h << 11 | mi << 5 | s / 2); }
    }
    put16(e + 22, time); put16(e + 24, date); put16(e + 18, date);
    if (created) { e[13] = 0; put16(e + 14, time); put16(e + 16, date); }
}

/* ---- paths ------------------------------------------------------------------------------------ */
/* Split "/a/b/c" into the folder holding c (loaded into d) and "c". The root
 * itself has no parent: -1. */
static int walk_to(const char *path, dir_t *d, char *leaf) {
    if (fs_mount()) return -1;
    if (path[0] != '/') return fail("Paths start with /", 0);
    if (dir_load(V.root, d)) return -1;
    const char *p = path;
    for (;;) {
        while (*p == '/') p++;
        const char *q = p;
        while (*q && *q != '/') q++;
        int n = (int)(q - p);
        if (!n) { dir_free(d); return fail("That's the top folder", 0); }
        if (n >= FS_NAME) { dir_free(d); return fail("A name is too long", 0); }
        char part[FS_NAME];
        memcpy(part, p, (size_t)n); part[n] = 0;
        const char *rest = q; while (*rest == '/') rest++;
        if (!*rest) { strcpy(leaf, part); return 0; }
        found_t f;
        if (dir_find(d, part, &f)) { dir_free(d); return fail("There's no folder called %s", part); }
        uint8_t *e = d->buf + f.idx * 32;
        if (!(e[11] & A_DIR)) { dir_free(d); return fail("%s is a file, not a folder", part); }
        uint32_t c = ent_clus(e);
        dir_free(d);
        if (dir_load(c ? c : V.root, d)) return -1;
        p = q;
    }
}

static int is_root(const char *path) {
    for (const char *p = path; *p; p++) if (*p != '/') return 0;
    return 1;
}

static int protected_path(const char *path) {
    while (*path == '/') path++;
    return name_eq(path, "BOLTOS.BIN");
}

/* Free slots for `n` entries in a row, growing the folder if it has none. */
static int dir_slots(dir_t *d, int n) {
    for (;;) {
        int total = (int)(d->bytes / 32), run = 0;
        for (int i = 0; i < total; i++) {
            uint8_t c = d->buf[i * 32];
            if (c == 0 || c == 0xE5) { if (++run == n) return i - n + 1; }
            else run = 0;
        }
        if (dir_grow(d)) return -1;
    }
}

typedef struct { const uint8_t *s; int hit; } alias_ctx;
static int alias_cb(dir_t *d, const found_t *f, void *ctx) {
    alias_ctx *c = ctx;
    if (!memcmp(d->buf + f->idx * 32, c->s, 11)) { c->hit = 1; return 1; }
    return 0;
}

/* Write a new entry for `name` into d: long-name entries if it needs them,
 * then the short one, a copy of `tmpl` with the alias in front. Its index. */
static int dir_add(dir_t *d, const char *name, const uint8_t *tmpl) {
    uint8_t sn[11];
    int need_lfn = !is_short(name, sn);
    if (need_lfn) {
        /* BASE~N.EXT, the lowest N not taken. */
        char base[9] = { 0 }, ext[4] = { 0 };
        const char *dot = strrchr(name, '.');
        int bl = 0, el = 0;
        for (const char *p = name; *p && p != dot && bl < 6; p++) if (*p != ' ' && *p != '.') base[bl++] = (char)short_char((unsigned char)*p);
        if (dot) for (const char *p = dot + 1; *p && el < 3; p++) if (*p != ' ') ext[el++] = (char)short_char((unsigned char)*p);
        if (!bl) base[bl++] = '_';
        for (int k = 1; k < 1000000; k++) {
            char tail[10]; int tl = snprintf(tail, sizeof tail, "~%d", k);
            int keep = bl + tl > 8 ? 8 - tl : bl;
            memset(sn, ' ', 11);
            memcpy(sn, base, (size_t)keep); memcpy(sn + keep, tail, (size_t)tl); memcpy(sn + 8, ext, (size_t)el);
            alias_ctx c = { sn, 0 };
            dir_walk(d, alias_cb, &c);
            if (!c.hit) break;
        }
    }
    int len = (int)strlen(name), nl = need_lfn ? (len + 12) / 13 : 0;
    int at = dir_slots(d, nl + 1);
    if (at < 0) return -1;
    uint8_t ck = lfn_sum(sn);
    for (int k = 0; k < nl; k++) {
        int seq = nl - k;                                  /* last part first */
        uint8_t *e = d->buf + (at + k) * 32;
        memset(e, 0, 32);
        e[0] = (uint8_t)(seq | (k == 0 ? 0x40 : 0));
        e[11] = A_LFN; e[13] = ck;
        for (int j = 0; j < 13; j++) {
            int ci = (seq - 1) * 13 + j;
            uint16_t ch = ci < len ? (uint8_t)name[ci] : ci == len ? 0 : 0xFFFF;
            put16(e + lfn_off[j], ch);
        }
    }
    uint8_t *e = d->buf + (at + nl) * 32;
    memcpy(e, tmpl, 32);
    memcpy(e, sn, 11);
    e[12] = 0;
    if (dir_put(d, at, nl + 1)) return -1;
    return at + nl;
}

static int dir_del(dir_t *d, const found_t *f) {
    for (int i = f->lfn; i <= f->idx; i++) d->buf[i * 32] = 0xE5;
    return dir_put(d, f->lfn, f->idx - f->lfn + 1);
}

/* ---- the calls -------------------------------------------------------------------------------- */
static void fill_ent(const found_t *f, const uint8_t *e, fs_ent_t *o) {
    snprintf(o->name, sizeof o->name, "%s", f->name);
    o->dir = (e[11] & A_DIR) != 0;
    o->size = o->dir ? 0 : le32(e + 28);
    o->date = le16(e + 24); o->time = le16(e + 22);
}

/* The folder at `path` (the root too), loaded. */
static int open_dir(const char *path, dir_t *d) {
    if (fs_mount()) return -1;
    if (is_root(path)) return dir_load(V.root, d);
    char leaf[FS_NAME];
    dir_t p;
    if (walk_to(path, &p, leaf)) return -1;
    found_t f;
    if (dir_find(&p, leaf, &f)) { dir_free(&p); return fail("There's no folder called %s", leaf); }
    uint8_t *e = p.buf + f.idx * 32;
    if (!(e[11] & A_DIR)) { dir_free(&p); return fail("%s is a file, not a folder", leaf); }
    uint32_t c = ent_clus(e);
    dir_free(&p);
    return dir_load(c ? c : V.root, d);
}

typedef struct { fs_ent_t *out; int max, n; } list_ctx;
static int list_cb(dir_t *d, const found_t *f, void *ctx) {
    list_ctx *c = ctx;
    const uint8_t *e = d->buf + f->idx * 32;
    if (f->name[0] == '.' || (e[11] & (A_HIDDEN | A_SYSTEM))) return 0;
    if (c->n < c->max) fill_ent(f, e, &c->out[c->n++]);
    return 0;
}

static int ent_cmp(const fs_ent_t *a, const fs_ent_t *b) {
    if (a->dir != b->dir) return a->dir ? -1 : 1;
    for (int i = 0;; i++) {
        int x = lower((unsigned char)a->name[i]), y = lower((unsigned char)b->name[i]);
        if (x != y || !x) return x - y;
    }
}

int fs_list(const char *path, fs_ent_t *out, int max) {
    dir_t d;
    if (open_dir(path, &d)) return -1;
    list_ctx c = { out, max, 0 };
    dir_walk(&d, list_cb, &c);
    dir_free(&d);
    for (int i = 1; i < c.n; i++)                          /* a few dozen: insertion sort */
        for (int k = i; k > 0 && ent_cmp(&out[k - 1], &out[k]) > 0; k--) {
            fs_ent_t t = out[k]; out[k] = out[k - 1]; out[k - 1] = t;
        }
    return c.n;
}

int fs_stat(const char *path, fs_ent_t *out) {
    if (fs_mount()) return -1;
    if (is_root(path)) { if (out) { memset(out, 0, sizeof *out); out->dir = 1; strcpy(out->name, "/"); } return 0; }
    dir_t d; char leaf[FS_NAME];
    if (walk_to(path, &d, leaf)) return -1;
    found_t f;
    int r = dir_find(&d, leaf, &f);
    if (r) fail("There's nothing called %s", leaf);
    else if (out) fill_ent(&f, d.buf + f.idx * 32, out);
    dir_free(&d);
    return r;
}

int fs_read(const char *path, char **out, uint32_t *len) {
    *out = 0; if (len) *len = 0;
    dir_t d; char leaf[FS_NAME];
    if (walk_to(path, &d, leaf)) return -1;
    found_t f;
    if (dir_find(&d, leaf, &f)) { dir_free(&d); return fail("There's no file called %s", leaf); }
    uint8_t *e = d.buf + f.idx * 32;
    if (e[11] & A_DIR) { dir_free(&d); return fail("%s is a folder", leaf); }
    uint32_t size = le32(e + 28), c = ent_clus(e);
    dir_free(&d);
    char *buf = kmalloc(size + 1);
    uint8_t *bb = bounce_buf();
    if (!buf || !bb) { kfree(buf); return fail("Out of memory", 0); }
    uint32_t got = 0;
    while (got < size) {
        if (c < 2 || c >= EOC) { kfree(buf); return fail("%s is shorter than it says", leaf); }
        /* A run of clusters that follow each other, read in one go. */
        uint32_t run = 1, next, last = c;
        for (;;) {
            if (fat_get(last, &next)) { kfree(buf); return fail("The card can't be read", 0); }
            if (next != last + 1 || (run + 1) * V.cbytes > BOUNCE || got + run * V.cbytes >= size) break;
            run++; last = next;
        }
        if (rd(clus_sec(c), bb, run * V.spc)) { kfree(buf); return fail("The card can't be read", 0); }
        uint32_t n = run * V.cbytes;
        if (n > size - got) n = size - got;
        memcpy(buf + got, bb, n);
        got += n;
        c = next;
    }
    buf[size] = 0;
    *out = buf;
    if (len) *len = size;
    return 0;
}

/* The data into a new chain of clusters: its first, or 0 for an empty file (-1 on failure). */
static int64_t write_chain(const uint8_t *data, uint32_t len) {
    if (!len) return 0;
    uint8_t *bb = bounce_buf();
    if (!bb) return fail("Out of memory", 0);
    uint32_t first = 0, prev = 0, done = 0;
    while (done < len) {
        /* Allocate a run, then write it at once. */
        uint32_t run_first = 0, run = 0, want = (len - done + V.cbytes - 1) / V.cbytes, maxrun = BOUNCE / V.cbytes;
        while (run < want && run < maxrun) {
            uint32_t c = alloc_after(prev);
            if (!c) { if (first) free_chain(first); fat_flush(); return fail("The card is full", 0); }
            if (!first) first = c;
            if (run && c != prev + 1) { fat_set(prev, 0x0FFFFFFFu); fat_set(c, 0); V.hint = c; break; }
            if (!run) run_first = c;
            prev = c; run++;
        }
        uint32_t n = run * V.cbytes, take = len - done < n ? len - done : n;
        memcpy(bb, data + done, take);
        if (take < n) memset(bb + take, 0, n - take);
        if (wr(clus_sec(run_first), bb, run * V.spc)) { free_chain(first); fat_flush(); return fail("The SD card won't take a write", 0); }
        done += take;
    }
    return first;
}

int fs_write(const char *path, const void *data, uint32_t len) {
    if (protected_path(path)) return fail("BOLTOS.BIN is the system itself: pi.py install replaces it", 0);
    dir_t d; char leaf[FS_NAME];
    if (walk_to(path, &d, leaf)) return -1;
    if (!name_ok(leaf)) { dir_free(&d); return fail("\"%s\" can't be a file name", leaf); }
    found_t f;
    int have = dir_find(&d, leaf, &f) == 0;
    if (have && (d.buf[f.idx * 32 + 11] & A_DIR)) { dir_free(&d); return fail("%s is a folder", leaf); }
    int64_t first = write_chain(data, len);
    if (first < 0) { dir_free(&d); return -1; }
    if (fat_flush()) { free_chain((uint32_t)first); dir_free(&d); return fail("The SD card won't take a write", 0); }
    uint32_t old = 0;
    int err;
    if (have) {
        uint8_t *e = d.buf + f.idx * 32;
        old = ent_clus(e);
        set_clus(e, (uint32_t)first); put32(e + 28, len); e[11] |= A_ARCH;
        stamp(e, 0);
        err = dir_put(&d, f.idx, 1);
    } else {
        uint8_t t[32] = { 0 };
        t[11] = A_ARCH; set_clus(t, (uint32_t)first); put32(t + 28, len); stamp(t, 1);
        err = dir_add(&d, leaf, t) < 0;
    }
    dir_free(&d);
    if (err) { free_chain((uint32_t)first); fat_flush(); return -1; }
    if (old) free_chain(old);
    return fat_flush();
}

int fs_mkdir(const char *path) {
    if (fs_mount()) return -1;
    if (is_root(path)) return 0;
    /* The folders above it first. */
    char up[FS_PATH];
    snprintf(up, sizeof up, "%s", path);
    char *slash = strrchr(up, '/');
    if (slash && slash != up) { *slash = 0; if (fs_mkdir(up)) return -1; }
    dir_t d; char leaf[FS_NAME];
    if (walk_to(path, &d, leaf)) return -1;
    found_t f;
    if (dir_find(&d, leaf, &f) == 0) {
        int isdir = (d.buf[f.idx * 32 + 11] & A_DIR) != 0;
        dir_free(&d);
        return isdir ? 0 : fail("%s is a file", leaf);
    }
    if (!name_ok(leaf)) { dir_free(&d); return fail("\"%s\" can't be a folder name", leaf); }
    uint32_t c = alloc_after(0);
    if (!c) { dir_free(&d); return fail("The card is full", 0); }
    uint8_t *z = kmalloc(V.cbytes);
    if (!z) { free_chain(c); dir_free(&d); return fail("Out of memory", 0); }
    memset(z, 0, V.cbytes);
    memset(z, ' ', 11); z[0] = '.'; z[11] = A_DIR; set_clus(z, c); stamp(z, 1);
    memset(z + 32, ' ', 11); z[32] = z[33] = '.'; z[43] = A_DIR; set_clus(z + 32, d.first == V.root ? 0 : d.first); stamp(z + 32, 1);
    int err = 0;
    for (uint32_t s = 0; s < V.spc && !err; s++) err = wr(clus_sec(c) + s, z + s * SEC, 1);
    kfree(z);
    if (err || fat_flush()) { free_chain(c); fat_flush(); dir_free(&d); return fail("The SD card won't take a write", 0); }
    uint8_t t[32] = { 0 };
    t[11] = A_DIR; set_clus(t, c); stamp(t, 1);
    err = dir_add(&d, leaf, t) < 0;
    dir_free(&d);
    if (err) { free_chain(c); fat_flush(); return -1; }
    return fat_flush();
}

/* Everything in the folder whose first cluster is c, then the folder's own clusters. */
static int remove_tree(uint32_t c, int depth) {
    if (depth > 32) return fail("Folders are nested too deep", 0);
    dir_t d;
    if (dir_load(c, &d)) return -1;
    int n = (int)(d.bytes / 32);
    for (int i = 0; i < n; i++) {
        uint8_t *e = d.buf + i * 32;
        if (e[0] == 0) break;
        if (e[0] == 0xE5 || (e[11] & 0x3F) == A_LFN || (e[11] & A_VOLID) || e[0] == '.') continue;
        uint32_t ec = ent_clus(e);
        if (e[11] & A_DIR) { if (ec && ec != c && remove_tree(ec, depth + 1)) { dir_free(&d); return -1; } }
        else if (ec) free_chain(ec);
    }
    dir_free(&d);
    free_chain(c);
    return 0;
}

int fs_remove(const char *path) {
    if (is_root(path)) return fail("The top folder can't be removed", 0);
    if (protected_path(path)) return fail("BOLTOS.BIN is the system itself: it stays", 0);
    dir_t d; char leaf[FS_NAME];
    if (walk_to(path, &d, leaf)) return -1;
    found_t f;
    if (dir_find(&d, leaf, &f)) { dir_free(&d); return fail("There's nothing called %s", leaf); }
    uint8_t e[32];
    memcpy(e, d.buf + f.idx * 32, 32);
    int err = dir_del(&d, &f);
    dir_free(&d);
    if (err) return -1;
    uint32_t c = ent_clus(e);
    if (e[11] & A_DIR) { if (c) remove_tree(c, 0); }
    else if (c) free_chain(c);
    return fat_flush();
}

int fs_rename(const char *from, const char *to) {
    if (protected_path(from) || protected_path(to)) return fail("BOLTOS.BIN is the system itself: it stays as it is", 0);
    if (is_root(from) || is_root(to)) return fail("The top folder can't be moved", 0);
    /* A folder can't go inside itself. */
    int fl = (int)strlen(from);
    if (!strncasecmp(to, from, (size_t)fl) && to[fl] == '/') return fail("A folder can't go inside itself", 0);
    dir_t a, b; char la[FS_NAME], lb[FS_NAME];
    if (walk_to(from, &a, la)) return -1;
    found_t f;
    if (dir_find(&a, la, &f)) { dir_free(&a); return fail("There's nothing called %s", la); }
    if (walk_to(to, &b, lb)) { dir_free(&a); return -1; }
    if (!name_ok(lb)) { dir_free(&a); dir_free(&b); return fail("\"%s\" can't be a name", lb); }
    found_t g;
    int same_dir = a.first == b.first;
    if (dir_find(&b, lb, &g) == 0 && !(same_dir && g.idx == f.idx)) {
        dir_free(&a); dir_free(&b);
        return fail("There's already something called %s", lb);
    }
    uint8_t e[32];
    memcpy(e, a.buf + f.idx * 32, 32);
    int err;
    if (same_dir) {
        /* One folder: take the old entries out, then add the new ones to the same copy. */
        dir_free(&b);
        err = dir_del(&a, &f) || dir_add(&a, lb, e) < 0;
        dir_free(&a);
    } else {
        err = dir_add(&b, lb, e) < 0;
        if (!err) err = dir_del(&a, &f);
        /* A folder that moved has a new parent: its ".." says so. */
        if (!err && (e[11] & A_DIR) && ent_clus(e)) {
            dir_t m;
            if (dir_load(ent_clus(e), &m) == 0) {
                if (m.bytes >= 64 && m.buf[32] == '.' && m.buf[33] == '.') {
                    set_clus(m.buf + 32, b.first == V.root ? 0 : b.first);
                    dir_put(&m, 1, 1);
                }
                dir_free(&m);
            }
        }
        dir_free(&a); dir_free(&b);
    }
    fat_flush();
    return err ? -1 : 0;
}


/* Counted once, from the first FAT read in big pieces; kept up after (fat_set). */
static void count_free(void) {
    uint8_t *bb = bounce_buf();
    if (!bb || flush_cache()) return;
    int64_t n = 0;
    uint32_t per = BOUNCE / SEC, last = V.nclus + 2;
    for (uint32_t s = 0; s < V.fat_size && s * 128 < last; s += per) {
        uint32_t k = V.fat_size - s < per ? V.fat_size - s : per;
        if (rd(V.fat_start + s, bb, k)) return;
        for (uint32_t i = 0; i < k * 128; i++) {
            uint32_t c = s * 128 + i;
            if (c >= 2 && c < last && !(le32(bb + i * 4) & 0x0FFFFFFFu)) n++;
        }
    }
    V.nfree = n;
}

int fs_space(uint64_t *free, uint64_t *total) {
    if (fs_mount()) return -1;
    if (V.nfree < 0) count_free();
    if (V.nfree < 0) return fail("The card can't be read", 0);
    if (free) *free = (uint64_t)V.nfree * V.cbytes;
    if (total) *total = (uint64_t)V.nclus * V.cbytes;
    return 0;
}

/* ---- path text ----------------------------------------------------------------------------------- */
int fs_join(const char *cwd, const char *rel, char *out, int cap) {
    char tmp[FS_PATH * 2];
    if (rel[0] == '/') snprintf(tmp, sizeof tmp, "%s", rel);
    else snprintf(tmp, sizeof tmp, "%s/%s", cwd && cwd[0] ? cwd : "/", rel);
    /* Walk the parts, dropping "." and stepping back for "..". */
    int n = 0;
    out[0] = 0;
    for (char *p = tmp; *p; ) {
        while (*p == '/') p++;
        if (!*p) break;
        char *q = p;
        while (*q && *q != '/') q++;
        int len = (int)(q - p);
        if (len == 1 && p[0] == '.') { }
        else if (len == 2 && p[0] == '.' && p[1] == '.') { while (n > 0 && out[n - 1] != '/') n--; if (n > 0) n--; out[n] = 0; }
        else {
            if (n + 1 + len >= cap) return -1;
            out[n++] = '/'; memcpy(out + n, p, (size_t)len); n += len; out[n] = 0;
        }
        p = q;
    }
    if (!n) { if (cap < 2) return -1; out[0] = '/'; out[1] = 0; }
    return 0;
}

const char *fs_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void fs_date_str(uint16_t date, uint16_t time, char *out, int cap) {
    snprintf(out, (size_t)cap, "%04d-%02d-%02d %02d:%02d", 1980 + (date >> 9), (date >> 5) & 15, date & 31,
             time >> 11, (time >> 5) & 63);
}

int fs_debug(char *out, int cap) {
    if (V.state != 1) return snprintf(out, (size_t)cap, "fs: not mounted (%s)\n", V.why);
    return snprintf(out, (size_t)cap, "fs: FAT32 \"%s\" at sector %lu, %u clusters of %u bytes, hint %u, free %ld, "
                    "%u sectors read, %u written\n", V.label, (unsigned long)V.base, V.nclus, V.cbytes, V.hint,
                    (long)V.nfree, V.reads, V.writes);
}

/* ---- the keep records, on the card ------------------------------------------------------------- *
 *  klog.c keeps a few small records (the Wi-Fi network, Bluetooth pairings, the
 *  theme) in a corner of RAM a restart doesn't clear. A power cut does, so a
 *  copy goes to /System/keep.bin a second after each change, and comes back
 *  from there at boot when RAM has nothing.                                      */
int  keep_blob(uint8_t *out, int cap);
int  keep_restore_blob(const uint8_t *blob, int len);
static uint64_t keep_due;

void fs_keep_changed(void) { keep_due = now_ms() + 1000; if (!keep_due) keep_due = 1; }

void fs_keep_restore(void) {
    if (fs_mount()) return;
    char *b; uint32_t n;
    if (fs_read("/System/keep.bin", &b, &n)) return;
    if (keep_restore_blob((const uint8_t *)b, (int)n) > 0) kprintf("[fs] the kept settings came back from the card\n");
    kfree(b);
}

void fs_poll(void) {
    if (!keep_due || now_ms() < keep_due) return;
    keep_due = 0;
    if (fs_mount()) return;
    static uint8_t blob[4096];
    int n = keep_blob(blob, sizeof blob);
    if (n <= 0) return;
    char *old; uint32_t on;
    if (fs_read("/System/keep.bin", &old, &on) == 0) {
        int same = on == (uint32_t)n && !memcmp(old, blob, (size_t)n);
        kfree(old);
        if (same) return;
    }
    if (fs_mkdir("/System") || fs_write("/System/keep.bin", blob, (uint32_t)n))
        kprintf("[fs] couldn't save the kept settings: %s\n", V.why);
}
