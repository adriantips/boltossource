#include <stdint.h>
#include <stddef.h>
#include "fat32.h"
#include "blk.h"
#include "string.h"
#include "kprintf.h"
#include "boot.h"

/* ===========================================================================
 *  FAT32 implementation. Synchronous, single-volume, one open operation at a
 *  time. Cluster I/O goes through the generic block layer (blk_read/write).
 * ===========================================================================*/

#define SECSZ 512u
#define MAX_CLUSTER_BYTES (64 * 1024)
#define EOC 0x0FFFFFF8u

static struct {
    int       mounted;
    blkdev_t *dev;
    uint64_t  base;           /* first LBA of the volume within the device     */
    uint64_t  span;           /* volume length in sectors                      */
    uint32_t  bytes_per_sec;
    uint32_t  sec_per_clus;
    uint32_t  reserved;
    uint32_t  num_fats;
    uint32_t  fat_size;       /* sectors per FAT */
    uint32_t  root_clus;
    uint32_t  fat_start;      /* LBA of FAT #0 */
    uint32_t  data_start;     /* LBA of cluster 2 */
    uint32_t  total_clus;
    uint32_t  clus_bytes;
    uint32_t  next_free;      /* allocation hint: where to start looking      */
    char      label[12];
} fs;

/* One-sector FAT read cache. A FAT32 sector holds 128 entries, so walking or
 * allocating a run of consecutive clusters otherwise re-reads the same sector
 * 128 times over. Invalidated on every FAT write. */
static uint8_t  fatcache[SECSZ];
static uint32_t fatcache_sec = 0xFFFFFFFFu;

static uint8_t  secbuf[SECSZ];
static uint8_t  clusbuf[MAX_CLUSTER_BYTES];

int fat32_mounted(void) { return fs.mounted; }
const char *fat32_label(void) { return fs.label; }

/* ---- low level ---------------------------------------------------------- */
/* Every LBA in this driver is volume-relative; the partition base is added
 * here and nowhere else, so a FAT32 volume can sit inside a partition (an EFI
 * system partition, say) as easily as on a whole device. */
static int rd_sec(uint32_t lba, void *b) { return blk_read(fs.dev, fs.base + lba, 1, b); }
static int wr_sec(uint32_t lba, const void *b) { return blk_write(fs.dev, fs.base + lba, 1, b); }

static uint32_t clus_lba(uint32_t c) { return fs.data_start + (c - 2) * fs.sec_per_clus; }
/* Cluster I/O is multi-sector, so it adds the base itself. */
static uint64_t clus_dev_lba(uint32_t c) { return fs.base + clus_lba(c); }

/* Follow one link of a cluster chain. The old fat_get() reported a failed FAT
 * read as EOC, which is indistinguishable from a real end of chain: a read
 * error in the middle of a file truncated it and still returned success, and
 * free_chain() stopped halfway and leaked the rest of the clusters. Every walk
 * goes through this now, and a failure is an error the caller has to handle. */
static int fat_next(uint32_t clus, uint32_t *out) {
    uint32_t off = clus * 4;
    uint32_t sec = fs.fat_start + off / SECSZ;
    if (sec != fatcache_sec) {
        /* A failed read may have left part of the sector in the buffer, so the
         * cache is marked invalid rather than trusted on the next call. */
        if (rd_sec(sec, fatcache) != 0) { fatcache_sec = 0xFFFFFFFFu; return -1; }
        fatcache_sec = sec;
    }
    *out = (*(uint32_t *)(fatcache + off % SECSZ)) & 0x0FFFFFFFu;
    return 0;
}
static int fat_set(uint32_t clus, uint32_t val) {
    uint32_t off = clus * 4;
    uint32_t idx = off % SECSZ;
    fatcache_sec = 0xFFFFFFFFu;                    /* the cached copy is stale */
    for (uint32_t f = 0; f < fs.num_fats; f++) {
        uint32_t sec = fs.fat_start + f * fs.fat_size + off / SECSZ;
        if (rd_sec(sec, secbuf) != 0) return -1;
        uint32_t *e = (uint32_t *)(secbuf + idx);
        *e = (*e & 0xF0000000u) | (val & 0x0FFFFFFFu);
        if (wr_sec(sec, secbuf) != 0) return -1;
    }
    return 0;
}

/* allocate one free cluster, mark EOC. returns cluster# or 0. */
/* Allocate one cluster, resuming from where the last allocation stopped and
 * wrapping once. The old version restarted at cluster 2 every call, so writing
 * an N-cluster file cost O(N^2) FAT reads -- copying a 7 MiB kernel onto an ESP
 * took over a million sector reads and looked like a hang. */
static uint32_t fat_alloc(void) {
    uint32_t last = fs.total_clus + 2;
    uint32_t start = (fs.next_free >= 2 && fs.next_free < last) ? fs.next_free : 2;

    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t from = pass ? 2 : start;
        uint32_t to   = pass ? start : last;
        for (uint32_t c = from; c < to; c++) {
            uint32_t v;
            if (fat_next(c, &v) != 0) return 0;        /* the FAT is unreadable */
            if (v != 0) continue;
            if (fat_set(c, EOC) != 0) return 0;
            fs.next_free = c + 1;
            return c;
        }
    }
    return 0;
}

static int read_cluster(uint32_t c, void *buf) {
    return blk_read(fs.dev, clus_dev_lba(c), fs.sec_per_clus, buf);
}
static int write_cluster(uint32_t c, const void *buf) {
    return blk_write(fs.dev, clus_dev_lba(c), fs.sec_per_clus, buf);
}

/* ---- name helpers ------------------------------------------------------- */
static void to_short(const char *name, char out[11]) {
    memset(out, ' ', 11);
    int i = 0, o = 0;
    /* base name */
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    while (name[i] && (dot ? &name[i] != dot : 1) && o < 8) {
        char c = name[i++];
        if (c == '.') break;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == ' ') continue;
        out[o++] = c;
    }
    if (dot) {
        const char *e = dot + 1;
        o = 8;
        for (int k = 0; e[k] && o < 11; k++) {
            char c = e[k];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out[o++] = c;
        }
    }
}

/* assemble LFN fragment chars (UTF-16 low byte) into dst at position */
static void lfn_chars(const uint8_t *e, char *dst, int *n) {
    const int idx[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    for (int i = 0; i < 13; i++) {
        uint16_t c = e[idx[i]] | (e[idx[i] + 1] << 8);
        if (c == 0 || c == 0xFFFF) return;
        if (*n < 255) dst[(*n)++] = (char)(c & 0xFF);
    }
}

static void short_to_name(const uint8_t *e, char *out) {
    int o = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) out[o++] = (char)e[i];
    int has_ext = e[8] != ' ';
    if (has_ext) {
        out[o++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) out[o++] = (char)e[i];
    }
    out[o] = 0;
}

/* ---- directory iteration ------------------------------------------------ */
/* Where a directory entry lives. Everything that changes an entry addresses it
 * this way, so the change is one 512-byte sector write rather than a
 * read-modify-write of the cluster it happens to share with other files. */
typedef struct { uint32_t clus; uint32_t idx; } dir_loc;

/* Walk every entry of the directory whose first cluster is dir_clus, invoking
 * cb for each live file/subdir, with where that entry sits. Returns 0 at the
 * end of the directory, the callback's value if it asked to stop, or -1 if the
 * directory could not be read. */
typedef int (*dir_cb)(const uint8_t *ent, const char *name,
                      const dir_loc *at, void *ctx);

static int walk_dir(uint32_t dir_clus, dir_cb cb, void *ctx) {
    char lfn[256]; int lfn_len = 0; int have_lfn = 0;
    uint32_t c = dir_clus;
    while (c >= 2 && c < EOC) {
        if (read_cluster(c, clusbuf) != 0) return -1;
        uint32_t entries = fs.clus_bytes / 32;
        for (uint32_t i = 0; i < entries; i++) {
            uint8_t *e = clusbuf + i * 32;
            if (e[0] == 0x00) return 0;              /* end of directory */
            if (e[0] == 0xE5) { have_lfn = 0; continue; }  /* deleted */
            if (e[11] == 0x0F) {                      /* LFN entry */
                if (e[0] & 0x40) { lfn_len = 0; }     /* last logical -> reset */
                /* LFN pieces come last-first; prepend by rebuilding */
                char frag[16]; int fn = 0;
                lfn_chars(e, frag, &fn);
                /* shift existing right and place frag at front */
                char tmp[256];
                int seq = (e[0] & 0x3F);
                int pos = (seq - 1) * 13;
                for (int k = 0; k < fn && pos + k < 255; k++) lfn[pos + k] = frag[k];
                if (pos + fn > lfn_len) lfn_len = pos + fn;
                (void)tmp;
                have_lfn = 1;
                continue;
            }
            if (e[11] & FAT_ATTR_VOLID) { have_lfn = 0; continue; }
            char name[256];
            if (have_lfn) { for (int k = 0; k < lfn_len; k++) name[k] = lfn[k]; name[lfn_len] = 0; }
            else short_to_name(e, name);
            have_lfn = 0; lfn_len = 0;
            dir_loc at = { c, i };
            int r = cb(e, name, &at, ctx);
            if (r) return r;                          /* cb requested stop */
        }
        if (fat_next(c, &c) != 0) return -1;
    }
    return 0;
}

/* ---- path resolution ---------------------------------------------------- */
struct find_ctx { const char *want; uint32_t clus; uint32_t size; uint8_t attr;
                  int found; dir_loc at; };
static int find_cb(const uint8_t *e, const char *name, const dir_loc *at, void *vctx) {
    struct find_ctx *fc = vctx;
    /* case-insensitive compare */
    const char *a = name, *b = fc->want;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    if (*a || *b) return 0;
    fc->clus = ((uint32_t)(e[20] | (e[21] << 8)) << 16) | (e[26] | (e[27] << 8));
    fc->size = *(uint32_t *)(e + 28);
    fc->attr = e[11];
    fc->at = *at;
    fc->found = 1;
    return 1;
}

/* Split path; resolve to (cluster,size,attr). Returns 0 if found, -1 if the
 * name is not there, and -2 if a directory along the way could not be read --
 * a distinction that matters, because treating an I/O error as "not found" is
 * how a write ends up creating a second directory entry for a file that is
 * already on the disk. Also returns the parent directory cluster in *parent
 * (for create operations) and the leaf's own entry location in *at. */
static int resolve(const char *path, uint32_t *clus, uint32_t *size, uint8_t *attr,
                   uint32_t *parent, char *leaf, dir_loc *at) {
    uint32_t cur = fs.root_clus;
    uint32_t par = fs.root_clus;
    char comp[256];
    const char *p = path;
    if (*p == '/') p++;
    int is_dir = 1;
    if (leaf) leaf[0] = 0;
    uint32_t csize = 0; uint8_t cattr = FAT_ATTR_DIR;
    while (*p) {
        int n = 0;
        while (*p && *p != '/') { if (n < 255) comp[n++] = *p; p++; }
        comp[n] = 0;
        if (*p == '/') p++;
        if (n == 0) break;
        if (leaf) strcpy(leaf, comp);
        struct find_ctx fc = { comp, 0, 0, 0, 0, { 0, 0 } };
        par = cur;
        int wr = walk_dir(cur, find_cb, &fc);
        if (wr < 0) return -2;                        /* unreadable directory */
        if (wr == 0 || !fc.found) {
            if (parent) *parent = cur;
            return -1;                                /* not found */
        }
        cur = fc.clus ? fc.clus : fs.root_clus;
        csize = fc.size; cattr = fc.attr;
        if (at) *at = fc.at;
        is_dir = (fc.attr & FAT_ATTR_DIR) != 0;
    }
    (void)is_dir;
    if (clus) *clus = cur;
    if (size) *size = csize;
    if (attr) *attr = cattr;
    if (parent) *parent = par;
    return 0;
}

/* ---- public: list ------------------------------------------------------- */
struct list_ctx { fat_dirent *out; int max; int n; };
static int list_cb(const uint8_t *e, const char *name, const dir_loc *at, void *vctx) {
    struct list_ctx *lc = vctx;
    (void)at;
    if (lc->n >= lc->max) return 1;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) return 0;
    fat_dirent *d = &lc->out[lc->n++];
    strncpy(d->name, name, sizeof d->name - 1);
    d->name[sizeof d->name - 1] = 0;
    d->size = *(uint32_t *)(e + 28);
    d->attr = e[11];
    d->cluster = ((uint32_t)(e[20] | (e[21] << 8)) << 16) | (e[26] | (e[27] << 8));
    return 0;
}
int fat32_list(const char *path, fat_dirent *out, int max) {
    if (!fs.mounted) return -1;
    uint32_t clus; uint8_t attr;
    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) clus = fs.root_clus;
    else if (resolve(path, &clus, 0, &attr, 0, 0, 0) != 0) return -1;
    else if (!(attr & FAT_ATTR_DIR)) return -1;       /* a file's data is not a
                                                       * directory, however
                                                       * willingly it parses as
                                                       * one */
    struct list_ctx lc = { out, max, 0 };
    if (walk_dir(clus, list_cb, &lc) < 0) return -1;   /* a partial listing is a
                                                        * wrong answer, not a
                                                        * shorter one */
    return lc.n;
}

/* ---- public: read ------------------------------------------------------- */
/* Size of a file without reading it, so a caller can allocate exactly once.
 * -1 if the path is missing or is a directory. */
int64_t fat32_size(const char *path) {
    if (!fs.mounted) return -1;
    uint32_t clus, size; uint8_t attr;
    if (resolve(path, &clus, &size, &attr, 0, 0, 0) != 0) return -1;
    if (attr & FAT_ATTR_DIR) return -1;
    return (int64_t)size;
}

int fat32_read(const char *path, void *buf, uint32_t max) {
    if (!fs.mounted) return -1;
    uint32_t clus, size; uint8_t attr;
    if (resolve(path, &clus, &size, &attr, 0, 0, 0) != 0) return -1;
    if (attr & FAT_ATTR_DIR) return -1;
    uint8_t *out = buf;
    uint32_t got = 0;
    uint32_t c = clus;
    while (c >= 2 && c < EOC && got < size && got < max) {
        if (read_cluster(c, clusbuf) != 0) return -1;
        uint32_t chunk = fs.clus_bytes;
        if (got + chunk > size) chunk = size - got;
        if (got + chunk > max)  chunk = max - got;
        memcpy(out + got, clusbuf, chunk);
        got += chunk;
        if (fat_next(c, &c) != 0) return -1;
    }
    /* Stopping short of the length in the directory entry means the chain ran
     * out early: the file on disk is not the file the directory describes, and
     * handing back a short count as success would let a caller install a
     * truncated kernel and call it done. */
    if (got < size && got < max) return -1;
    return (int)got;
}

/* ---- public: write (create/overwrite) ----------------------------------- */
/* The device sector holding a given entry. An entry is 32 bytes and a sector
 * is a whole number of entries, so an entry never straddles two of them. */
static uint32_t dir_ent_lba(const dir_loc *at) {
    return clus_lba(at->clus) + (at->idx * 32) / SECSZ;
}

/* Read the sector an entry lives in, hand it to the caller to amend, write it
 * back -- and touch nothing else.
 *
 * Publishing a file used to read the whole cluster, change 32 bytes of it and
 * write all of it back. That put every other entry in the cluster through the
 * drive on every single write -- on a 32 KiB cluster, 1023 other files, 64 KiB
 * of I/O to change four fields -- and left the claim that a file switches over
 * atomically resting on the driver having read those neighbours correctly and
 * written them back byte for byte. Changing the one sector that actually
 * differs makes the claim structural: nothing else is in flight to be got
 * wrong. */
static uint8_t *dir_ent_begin(const dir_loc *at) {
    if (rd_sec(dir_ent_lba(at), secbuf) != 0) return 0;
    return secbuf + (at->idx * 32) % SECSZ;
}
static int dir_ent_commit(const dir_loc *at) { return wr_sec(dir_ent_lba(at), secbuf); }

/* Find a free 32-byte slot in the directory's cluster chain, extending it by a
 * cluster if every slot is taken. */
static int dir_put_entry(uint32_t dir_clus, const uint8_t ent[32]) {
    uint32_t c = dir_clus, last = dir_clus;
    int prev_lfn = 0;
    while (c >= 2 && c < EOC) {
        if (read_cluster(c, clusbuf) != 0) return -1;
        uint32_t entries = fs.clus_bytes / 32;
        for (uint32_t i = 0; i < entries; i++) {
            uint8_t *e = clusbuf + i * 32;
            /* A deleted 8.3 slot can be the tail of a long-name run whose own
             * entries another system left behind. Dropping a new file into it
             * would give that file the deleted one's long name, so a slot with
             * an orphaned name entry in front of it is passed over. */
            if (e[0] == 0xE5 && e[11] != 0x0F && prev_lfn) { prev_lfn = 0; continue; }
            if (e[0] == 0x00 || e[0] == 0xE5) {
                dir_loc at = { c, i };
                uint8_t *slot = dir_ent_begin(&at);
                if (!slot) return -1;
                memcpy(slot, ent, 32);
                return dir_ent_commit(&at);
            }
            prev_lfn = (e[11] == 0x0F);
        }
        last = c;
        if (fat_next(c, &c) != 0) return -1;
    }
    /* Extend. The new cluster has to hold the entry, and be on the disk,
     * before the FAT links it into the directory -- linking first left a crash
     * in that window showing whatever the disk happened to have there as
     * directory entries. */
    uint32_t nc = fat_alloc();
    if (!nc) return -1;
    memset(clusbuf, 0, fs.clus_bytes);
    memcpy(clusbuf, ent, 32);
    if (write_cluster(nc, clusbuf) != 0 || blk_flush(fs.dev) != 0 ||
        fat_set(last, nc) != 0) { fat_set(nc, 0); return -1; }
    return 0;
}

static int free_chain(uint32_t c) {
    while (c >= 2 && c < EOC) {
        uint32_t nx;
        if (fat_next(c, &nx) != 0) return -1;
        if (fat_set(c, 0) != 0) return -1;
        c = nx;
    }
    return 0;
}

/* Repoint an existing entry at a new cluster chain and length, in place. That
 * one sector write is the moment the file becomes the new one, which is what
 * makes overwriting power-loss safe rather than a delete-then-create race.
 *
 * The entry is addressed by where the lookup found it, not by matching a
 * reconstructed 8.3 name: a file another system created is named by its
 * long-name entries and its short name is something like README~1.TXT, which
 * never matched, so every overwrite of such a file failed. */
static int dir_repoint(const dir_loc *at, uint32_t first, uint32_t len) {
    uint8_t *e = dir_ent_begin(at);
    if (!e) return -1;
    e[20] = (uint8_t)(first >> 16); e[21] = (uint8_t)(first >> 24);
    e[26] = (uint8_t)(first);       e[27] = (uint8_t)(first >> 8);
    *(uint32_t *)(e + 28) = len;
    return dir_ent_commit(at);
}

/* TODO: there is still no journal here. The ordering below means a crash
 * leaves either the old file or the new one, which is the property that
 * matters, but a crash between the switch and the free leaks clusters and
 * nothing ever reclaims them. fsck would have to grow a chain walk. */

/* Barrier for the mounted volume: everything written so far is on media. */
int fat32_flush(void) {
    if (!fs.mounted) return 0;
    return blk_flush(fs.dev);
}

int fat32_write(const char *path, const void *data, uint32_t len) {
    if (!fs.mounted || boot_is_recovery()) return -1;
    uint32_t parent; char leaf[256]; dir_loc at = { 0, 0 };
    uint32_t old_clus, old_size; uint8_t old_attr;
    int rr = resolve(path, &old_clus, &old_size, &old_attr, &parent, leaf, &at);
    if (rr == -2) return -1;      /* the directory could not be read; carrying on
                                   * as if the file were absent would file a
                                   * second entry for one already on the disk */
    int exists = (rr == 0);
    if (exists && (old_attr & FAT_ATTR_DIR)) return -1;
    if (leaf[0] == 0) return -1;

    /* Order matters for power-loss safety. This used to free the old cluster
     * chain and delete the directory entry FIRST, so a crash anywhere in the
     * rest of the function destroyed the file outright and left the FAT
     * pointing at freed clusters. Now the new copy is written to freshly
     * allocated clusters while the old one is still entirely intact, the
     * directory entry is switched over in a single sector write, and only then
     * is the old chain released. A crash before the switch leaves the previous
     * contents; a crash after it leaves the new contents; a crash between the
     * switch and the free leaks some clusters, which is recoverable and
     * infinitely preferable to losing the file. */

    /* 1. allocate + write the new data clusters (old file untouched) */
    uint32_t first = 0, prev = 0, written = 0;
    const uint8_t *src = data;
    while (written < len) {
        uint32_t nc = fat_alloc();
        if (!nc) { if (first) free_chain(first); return -1; }
        if (!first) first = nc;
        if (prev && fat_set(prev, nc) != 0) { free_chain(first); return -1; }
        memset(clusbuf, 0, fs.clus_bytes);
        uint32_t chunk = len - written;
        if (chunk > fs.clus_bytes) chunk = fs.clus_bytes;
        memcpy(clusbuf, src + written, chunk);
        if (write_cluster(nc, clusbuf) != 0) { free_chain(first); return -1; }
        written += chunk;
        prev = nc;
    }
    /* The data and its FAT chain must be durable before any entry names them. */
    if (blk_flush(fs.dev) != 0) { if (first) free_chain(first); return -1; }

    /* 2. publish it: repoint the existing entry, or add a new one */
    int rc;
    if (exists) {
        rc = dir_repoint(&at, first, len);
    } else {
        char sn[11]; to_short(leaf, sn);
        uint8_t ent[32]; memset(ent, 0, 32);
        memcpy(ent, sn, 11);
        ent[11] = 0x20;                               /* archive */
        ent[20] = (uint8_t)(first >> 16); ent[21] = (uint8_t)(first >> 24);
        ent[26] = (uint8_t)(first);       ent[27] = (uint8_t)(first >> 8);
        *(uint32_t *)(ent + 28) = len;
        rc = dir_put_entry(parent, ent);
    }
    if (rc != 0) { if (first) free_chain(first); return -1; }
    /* A failed flush here leaves the switch in doubt, and both answers -- the
     * old file or the new one -- are a consistent filesystem. What must not
     * happen is freeing the old chain on the strength of a switch that may not
     * have reached the disk, so this returns before step 3 rather than
     * continuing. The leaked new chain is fsck's problem, not the file's. */
    if (blk_flush(fs.dev) != 0) return -1;

    /* 3. the switch is durable, so the old chain is finally safe to release */
    if (exists && old_clus) { free_chain(old_clus); blk_flush(fs.dev); }
    return 0;
}

int fat32_mkdir(const char *path) {
    if (!fs.mounted || boot_is_recovery()) return -1;
    uint32_t parent; char leaf[256];
    uint32_t cc, cs; uint8_t ca;
    /* Anything but a clean "not found" -- it already exists, or a directory on
     * the way could not be read -- is a reason not to create it. */
    if (resolve(path, &cc, &cs, &ca, &parent, leaf, 0) != -1) return -1;
    if (leaf[0] == 0) return -1;

    uint32_t nc = fat_alloc();
    if (!nc) return -1;
    /* init new dir cluster with . and .. */
    memset(clusbuf, 0, fs.clus_bytes);
    uint8_t *dot = clusbuf, *dotdot = clusbuf + 32;
    memset(dot, ' ', 11); dot[0] = '.'; dot[11] = FAT_ATTR_DIR;
    dot[20] = (uint8_t)(nc >> 16); dot[21] = (uint8_t)(nc >> 24);
    dot[26] = (uint8_t)(nc); dot[27] = (uint8_t)(nc >> 8);
    memset(dotdot, ' ', 11); dotdot[0] = '.'; dotdot[1] = '.'; dotdot[11] = FAT_ATTR_DIR;
    uint32_t pc = (parent == fs.root_clus) ? 0 : parent;
    dotdot[20] = (uint8_t)(pc >> 16); dotdot[21] = (uint8_t)(pc >> 24);
    dotdot[26] = (uint8_t)(pc); dotdot[27] = (uint8_t)(pc >> 8);
    /* The . and .. entries have to be durable before the parent names this
     * cluster as a directory, or a crash in between leaves a directory whose
     * contents are whatever the disk had there. */
    if (write_cluster(nc, clusbuf) != 0 || blk_flush(fs.dev) != 0) {
        fat_set(nc, 0);
        return -1;
    }

    uint8_t ent[32]; memset(ent, 0, 32);
    to_short(leaf, (char *)ent);
    ent[11] = FAT_ATTR_DIR;
    ent[20] = (uint8_t)(nc >> 16); ent[21] = (uint8_t)(nc >> 24);
    ent[26] = (uint8_t)(nc); ent[27] = (uint8_t)(nc >> 8);
    if (dir_put_entry(parent, ent) != 0) { fat_set(nc, 0); return -1; }
    return blk_flush(fs.dev);
}

uint64_t fat32_total_bytes(void) { return (uint64_t)fs.total_clus * fs.clus_bytes; }
uint64_t fat32_free_bytes(void) {
    if (!fs.mounted) return 0;
    uint64_t freec = 0;
    for (uint32_t c = 2; c < fs.total_clus + 2; c++) {
        uint32_t v;
        if (fat_next(c, &v) != 0) return 0;   /* unreadable: claim no free space
                                               * rather than invent some */
        if (v == 0) freec++;
    }
    return freec * fs.clus_bytes;
}

/* ---- mount -------------------------------------------------------------- */
int fat32_mount(blkdev_t *dev) {
    return dev ? fat32_mount_at(dev, 0, dev->sectors) : -1;
}

int fat32_mount_at(blkdev_t *dev, uint64_t base, uint64_t span) {
    if (!dev || base + 1 > dev->sectors) return -1;
    if (blk_read(dev, base, 1, secbuf) != 0) return -1;
    if (secbuf[510] != 0x55 || secbuf[511] != 0xAA) return -1;

    uint32_t bps = secbuf[11] | (secbuf[12] << 8);
    if (bps != SECSZ) return -1;
    uint32_t spc = secbuf[13];
    uint32_t fatsz = *(uint32_t *)(secbuf + 36);
    if (spc == 0 || spc * SECSZ > MAX_CLUSTER_BYTES || fatsz == 0) return -1;

    fs.dev           = dev;
    fs.base          = base;
    fs.span          = span ? span : dev->sectors - base;
    fs.bytes_per_sec = bps;
    fs.sec_per_clus  = spc;
    fs.reserved      = secbuf[14] | (secbuf[15] << 8);
    fs.num_fats      = secbuf[16];
    fs.fat_size      = fatsz;
    fs.root_clus     = *(uint32_t *)(secbuf + 44);
    fs.fat_start     = fs.reserved;
    fs.data_start    = fs.reserved + fs.num_fats * fs.fat_size;
    fs.clus_bytes    = spc * SECSZ;

    uint32_t total_sec = *(uint32_t *)(secbuf + 32);
    if (total_sec == 0) total_sec = (uint32_t)fs.span;
    uint32_t data_sec = total_sec - fs.data_start;
    fs.total_clus = data_sec / spc;

    memcpy(fs.label, secbuf + 71, 11); fs.label[11] = 0;
    for (int i = 10; i >= 0 && fs.label[i] == ' '; i--) fs.label[i] = 0;

    fs.next_free = 2;
    fatcache_sec = 0xFFFFFFFFu;
    fs.mounted = 1;
    if (base) kprintf("[ok] FAT32 mounted on %s@LBA%lu: '%s', %u clusters x %u B\n",
                      dev->name, (unsigned long)base, fs.label, fs.total_clus, fs.clus_bytes);
    else      kprintf("[ok] FAT32 mounted on %s: '%s', %u clusters x %u B\n",
                      dev->name, fs.label, fs.total_clus, fs.clus_bytes);
    return 0;
}

void fat32_unmount(void) { fs.mounted = 0; fs.dev = 0; fs.base = 0; }

/* ---- format ------------------------------------------------------------- */
int fat32_format(blkdev_t *dev, const char *label) {
    return dev ? fat32_format_at(dev, 0, dev->sectors, label) : -1;
}

int fat32_format_at(blkdev_t *dev, uint64_t base, uint64_t span, const char *label) {
    if (!dev || boot_is_recovery() || base > dev->sectors ||
        span > dev->sectors - base) return -1;
    uint32_t total = (uint32_t)span;
    /* FAT32 needs >= 65525 clusters to be a legal FAT32 (below that a strict
     * driver -- UEFI firmware included -- reads it as FAT16 and rejects it). */
    if (total < 70000) return -1;                     /* too small for FAT32 */

    /* FAT32 is only legal above 65525 clusters -- below that a strict reader
     * (UEFI firmware, Windows) sees FAT16 and rejects the volume. The old fixed
     * 4 KiB cluster made a 64 MiB EFI system partition come out at ~16k
     * clusters: a FAT16 in FAT32 clothing. Pick the smallest power-of-two
     * cluster whose count stays under a sane FAT size, then verify the lower
     * bound. Bounded by clusbuf (MAX_CLUSTER_BYTES). */
    uint32_t reserved = 32;
    uint32_t num_fats = 2;
    uint32_t spc = 1;
    while (spc * SECSZ < MAX_CLUSTER_BYTES &&
           (total - reserved) / spc > 4194304u)      /* keep each FAT <= 16 MiB */
        spc *= 2;
    if ((total - reserved) / spc < 65525u) return -1;  /* too small for FAT32 */

    /* iteratively size the FAT: clusters = (total - reserved - 2*fatsz)/spc */
    uint32_t fatsz = 0;
    for (int it = 0; it < 8; it++) {
        uint32_t datasec = total - reserved - num_fats * fatsz;
        uint32_t clusters = datasec / spc;
        uint32_t needed = ((clusters + 2) * 4 + SECSZ - 1) / SECSZ;
        if (needed == fatsz) break;
        fatsz = needed;
    }

    uint32_t data_start = reserved + num_fats * fatsz;
    uint32_t root_clus = 2;

    /* boot sector */
    memset(secbuf, 0, SECSZ);
    secbuf[0] = 0xEB; secbuf[1] = 0x58; secbuf[2] = 0x90;
    memcpy(secbuf + 3, "MSWIN4.1", 8);
    secbuf[11] = SECSZ & 0xFF; secbuf[12] = SECSZ >> 8;
    secbuf[13] = (uint8_t)spc;
    secbuf[14] = reserved & 0xFF; secbuf[15] = reserved >> 8;
    secbuf[16] = (uint8_t)num_fats;
    secbuf[21] = 0xF8;                                  /* media */
    secbuf[24] = 63; secbuf[26] = 255;                 /* sectors/track, heads */
    *(uint32_t *)(secbuf + 32) = total;                /* total sectors 32 */
    *(uint32_t *)(secbuf + 36) = fatsz;                /* FAT size 32 */
    *(uint32_t *)(secbuf + 44) = root_clus;
    secbuf[48] = 1;                                    /* FSInfo sector */
    secbuf[50] = 6;                                    /* backup boot sector */
    secbuf[66] = 0x29;                                 /* ext boot sig */
    *(uint32_t *)(secbuf + 67) = 0x12345678;           /* volume id */
    memset(secbuf + 71, ' ', 11);
    for (int i = 0; label && label[i] && i < 11; i++) secbuf[71 + i] = (uint8_t)label[i];
    memcpy(secbuf + 82, "FAT32   ", 8);
    secbuf[510] = 0x55; secbuf[511] = 0xAA;
    if (blk_write(dev, base + 0, 1, secbuf) != 0) return -1;
    if (blk_write(dev, base + 6, 1, secbuf) != 0) return -1;   /* backup */

    /* FSInfo */
    memset(secbuf, 0, SECSZ);
    *(uint32_t *)(secbuf + 0)   = 0x41615252;
    *(uint32_t *)(secbuf + 484) = 0x61417272;
    *(uint32_t *)(secbuf + 488) = 0xFFFFFFFF;          /* free count unknown */
    *(uint32_t *)(secbuf + 492) = 0xFFFFFFFF;          /* next free unknown */
    secbuf[510] = 0x55; secbuf[511] = 0xAA;
    if (blk_write(dev, base + 1, 1, secbuf) != 0) return -1;

    /* Every write below is checked. A FAT sector that silently failed to zero
     * leaves clusters that read as allocated to nothing, and the first file
     * written to the volume is handed a cluster some other file already owns:
     * a mount that succeeds onto a volume that was never really formatted. */
    memset(secbuf, 0, SECSZ);
    for (uint32_t f = 0; f < num_fats; f++)
        for (uint32_t s = 0; s < fatsz; s++)
            if (blk_write(dev, base + reserved + f * fatsz + s, 1, secbuf) != 0) return -1;

    /* FAT[0]=media|EOC, FAT[1]=EOC, FAT[2]=EOC (root) */
    *(uint32_t *)(secbuf + 0) = 0x0FFFFFF8;
    *(uint32_t *)(secbuf + 4) = 0x0FFFFFFF;
    *(uint32_t *)(secbuf + 8) = 0x0FFFFFFF;
    for (uint32_t f = 0; f < num_fats; f++)
        if (blk_write(dev, base + reserved + f * fatsz, 1, secbuf) != 0) return -1;

    /* zero the root directory cluster */
    memset(secbuf, 0, SECSZ);
    for (uint32_t s = 0; s < spc; s++)
        if (blk_write(dev, base + data_start + s, 1, secbuf) != 0) return -1;
    if (blk_flush(dev) != 0) return -1;

    kprintf("[fat32] formatted %s@LBA%lu: %u sectors, FAT %u sec, %u clusters\n",
            dev->name, (unsigned long)base, total, fatsz, (total - data_start) / spc);
    return fat32_mount_at(dev, base, span);
}
