/* ===========================================================================
 *  BoltOS Mini  -  mini/store.c
 *
 *  Files on the microSD card: what Packages installs (an app, a language
 *  model) and keeps across restarts and power cuts. The card's partitions
 *  -- U-Boot in front, the FAT partition with BOLTOS.BIN -- are left alone:
 *  the store starts past the last of them, at 256 MiB or later, in space a
 *  card image never uses.
 *
 *      base + 0      directory, copy A (a header sector, 32 sectors of entries)
 *      base + 64     directory, copy B
 *      base + 2048   files, each one run of sectors, 1 MiB aligned
 *
 *  A change writes the copy that isn't current, with the generation one
 *  higher, so a power cut mid-write leaves the other copy to fall back on.
 *  A file is written in order from its start; `written` says how far it
 *  got, so an interrupted download carries on from there.
 *
 *  The store is made the first time something is installed, not at boot.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "store.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

int      sd_init(void);
uint64_t sd_sectors(void);
int      sd_read(uint64_t lba, void *buf, uint32_t n);
int      sd_write(uint64_t lba, const void *buf, uint32_t n);

#define MAGIC      "BOLTSTOR"
#define VERSION    1
#define MIN_BASE   (256ull * 2048)                /* 256 MiB, in sectors           */
#define DIR_A      0
#define DIR_B      64
#define DATA_OFF   2048
#define DIR_SECT   33
#define EXTENT     2048ull                         /* 1 MiB                         */
_Static_assert(sizeof(store_entry_t) == 256, "a store entry is 256 bytes");
_Static_assert(sizeof(store_entry_t) * STORE_MAX == (DIR_SECT - 1) * 512, "the entries fill a copy");

typedef struct {
    char     magic[8];
    uint32_t version, entries;
    uint64_t gen;
    uint64_t base, end;                            /* sectors: the store's span     */
    uint32_t crc;                                  /* of the entries                */
    uint8_t  pad[512 - 44];
} dir_head_t;

static struct {
    int      card;                                 /* 1 the card answered, -1 it didn't */
    int      open;
    int      copy;                                 /* the current copy: 0 A, 1 B    */
    uint64_t base, end, gen;
    store_entry_t e[STORE_MAX];
    char     why[96];
} T;

static uint32_t crc32(const void *p, uint32_t n) {
    const uint8_t *b = p;
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *b++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

static int card_up(void) {
    if (!T.card) T.card = sd_init() == 0 ? 1 : -1;
    return T.card > 0;
}

/* Where the store may start: past every partition the MBR lists. */
static uint64_t first_free_sector(void) {
    static uint8_t mbr[512] __attribute__((aligned(4)));
    if (sd_read(0, mbr, 1)) return 0;
    uint64_t end = 0;
    if (mbr[510] == 0x55 && mbr[511] == 0xAA)
        for (int i = 0; i < 4; i++) {
            const uint8_t *p = mbr + 446 + 16 * i;
            if (!p[4]) continue;
            uint32_t lba, n;
            memcpy(&lba, p + 8, 4); memcpy(&n, p + 12, 4);
            if ((uint64_t)lba + n > end) end = (uint64_t)lba + n;
        }
    end = (end + EXTENT - 1) / EXTENT * EXTENT;
    return end > MIN_BASE ? end : MIN_BASE;
}

static int read_copy(int which, dir_head_t *h, store_entry_t *e) {
    uint8_t *buf = kmalloc(DIR_SECT * 512);
    if (!buf) return -1;
    int ok = sd_read(T.base + (which ? DIR_B : DIR_A), buf, DIR_SECT) == 0;
    memcpy(h, buf, sizeof *h);
    memcpy(e, buf + 512, sizeof(store_entry_t) * STORE_MAX);
    kfree(buf);
    if (!ok || memcmp(h->magic, MAGIC, 8) || h->version != VERSION || h->entries != STORE_MAX) return -1;
    if (crc32(e, sizeof(store_entry_t) * STORE_MAX) != h->crc) return -1;
    return 0;
}

static int write_dir(void) {
    uint8_t *buf = kmalloc(DIR_SECT * 512);
    if (!buf) return -1;
    memset(buf, 0, DIR_SECT * 512);
    dir_head_t *h = (dir_head_t *)buf;
    memcpy(h->magic, MAGIC, 8);
    h->version = VERSION; h->entries = STORE_MAX;
    h->gen = T.gen + 1; h->base = T.base; h->end = T.end;
    memcpy(buf + 512, T.e, sizeof T.e);
    h->crc = crc32(T.e, sizeof T.e);
    int other = !T.copy;
    int err = sd_write(T.base + (other ? DIR_B : DIR_A), buf, DIR_SECT);
    kfree(buf);
    if (err) return -1;
    T.copy = other; T.gen++;
    return 0;
}

/* Find the store; with `create`, make one if there isn't. 0 if it's open. */
int store_open(int create) {
    if (T.open) return 0;
    if (!card_up()) { snprintf(T.why, sizeof T.why, "The SD card isn't answering"); return -1; }
    uint64_t sectors = sd_sectors();
    T.base = first_free_sector();
    if (!T.base) { snprintf(T.why, sizeof T.why, "The SD card can't be read"); return -1; }
    T.end = sectors;
    if (T.base + DATA_OFF + 64 * EXTENT > T.end) {
        snprintf(T.why, sizeof T.why, "The SD card has no room past its partitions");
        return -1;
    }
    static dir_head_t ha, hb;
    static store_entry_t eb[STORE_MAX];
    int a = read_copy(0, &ha, T.e) == 0;
    int b = read_copy(1, &hb, eb) == 0;
    if (a || b) {
        if (b && (!a || hb.gen > ha.gen)) { memcpy(T.e, eb, sizeof T.e); T.copy = 1; T.gen = hb.gen; }
        else { if (!a) memset(T.e, 0, sizeof T.e); T.copy = 0; T.gen = ha.gen; }
        T.open = 1;
        kprintf("[store] on the card at %lu MiB: %d files, %lu MiB free\n", (unsigned long)(T.base >> 11),
                store_count(), (unsigned long)(store_free() >> 20));
        return 0;
    }
    if (!create) { snprintf(T.why, sizeof T.why, "Nothing has been installed yet"); return -1; }
    memset(T.e, 0, sizeof T.e);
    T.gen = 0; T.copy = 1;                        /* so the first write goes to A */
    if (write_dir()) { snprintf(T.why, sizeof T.why, "The SD card won't take a write"); return -1; }
    T.open = 1;
    kprintf("[store] made a new one on the card at %lu MiB (%lu MiB)\n", (unsigned long)(T.base >> 11),
            (unsigned long)((T.end - T.base) >> 11));
    return 0;
}

const char *store_why(void) { return T.why; }
int store_is_open(void) { return T.open; }

int store_count(void) {
    int n = 0;
    for (int i = 0; i < STORE_MAX; i++) n += T.e[i].flags != 0;
    return n;
}

const store_entry_t *store_at(int i) {
    if (!T.open || i < 0 || i >= STORE_MAX || !T.e[i].flags) return 0;
    return &T.e[i];
}

static int find(const char *name) {
    for (int i = 0; i < STORE_MAX; i++)
        if (T.e[i].flags && !strncmp(T.e[i].name, name, sizeof T.e[i].name)) return i;
    return -1;
}

const store_entry_t *store_find(const char *name) {
    if (!T.open) return 0;
    int i = find(name);
    return i < 0 ? 0 : &T.e[i];
}

uint64_t store_total(void) { return T.open ? (T.end - T.base - DATA_OFF) * 512 : 0; }
uint64_t store_free(void) {
    if (!T.open) return 0;
    uint64_t used = 0;
    for (int i = 0; i < STORE_MAX; i++) if (T.e[i].flags) used += T.e[i].sectors * 512;
    return store_total() - used;
}

/* First fit, between the files in order of where they are. */
static uint64_t place(uint64_t sectors) {
    uint64_t at = T.base + DATA_OFF;
    for (;;) {
        int moved = 0;
        for (int i = 0; i < STORE_MAX; i++) {
            const store_entry_t *e = &T.e[i];
            if (!e->flags) continue;
            if (at < e->lba + e->sectors && e->lba < at + sectors) { at = e->lba + e->sectors; moved = 1; }
        }
        if (!moved) break;
    }
    return at + sectors <= T.end ? at : 0;
}

/* Room for `size` bytes under `name`, replacing a file of that name. The
 * file starts out unfinished, with nothing written. */
const store_entry_t *store_create(const char *name, uint64_t size) {
    if (!T.open || strlen(name) >= sizeof T.e[0].name) return 0;
    int i = find(name);
    if (i >= 0) memset(&T.e[i], 0, sizeof T.e[i]);
    else for (i = 0; i < STORE_MAX && T.e[i].flags; i++) { }
    if (i >= STORE_MAX) { snprintf(T.why, sizeof T.why, "The store is full (%d files)", STORE_MAX); return 0; }
    uint64_t sectors = (size + 511) / 512;
    sectors = (sectors + EXTENT - 1) / EXTENT * EXTENT;
    uint64_t lba = place(sectors);
    if (!lba) { snprintf(T.why, sizeof T.why, "There isn't room on the SD card"); return 0; }
    store_entry_t *e = &T.e[i];
    strncpy(e->name, name, sizeof e->name - 1);
    e->lba = lba; e->sectors = sectors; e->size = size; e->written = 0;
    e->flags = STORE_USED;
    if (write_dir()) { memset(e, 0, sizeof *e); snprintf(T.why, sizeof T.why, "The SD card won't take a write"); return 0; }
    return e;
}

/* Bytes at `off` (a multiple of 512) into the file; a length that isn't a
 * multiple of 512 is the file's end, and the last sector is padded. */
int store_write(const store_entry_t *e, uint64_t off, const void *buf, uint32_t len) {
    if (!e || (off & 511) || off + len > e->sectors * 512) return -1;
    uint32_t whole = len & ~511u;
    if (whole && sd_write(e->lba + off / 512, buf, whole / 512)) return -1;
    if (len > whole) {
        static uint8_t last[512] __attribute__((aligned(4)));
        memset(last, 0, sizeof last);
        memcpy(last, (const uint8_t *)buf + whole, len - whole);
        if (sd_write(e->lba + (off + whole) / 512, last, 1)) return -1;
    }
    return 0;
}

int store_read(const store_entry_t *e, uint64_t off, void *buf, uint32_t len) {
    if (!e || (off & 511) || (len & 511) || off + len > e->sectors * 512) return -1;
    return sd_read(e->lba + off / 512, buf, len / 512);
}

/* How far the file has been written; `done` marks it whole, with its hash. */
int store_progress(const char *name, uint64_t written, int done, const uint8_t *sha256) {
    int i = find(name);
    if (i < 0) return -1;
    T.e[i].written = written;
    if (done) {
        T.e[i].flags |= STORE_DONE;
        T.e[i].size = written;
        if (sha256) memcpy(T.e[i].sha256, sha256, 32);
    }
    return write_dir();
}

int store_set_meta(const char *name, const char *meta) {
    int i = find(name);
    if (i < 0) return -1;
    memset(T.e[i].meta, 0, sizeof T.e[i].meta);
    strncpy(T.e[i].meta, meta, sizeof T.e[i].meta - 1);
    return write_dir();
}

int store_remove(const char *name) {
    int i = find(name);
    if (i < 0) return -1;
    memset(&T.e[i], 0, sizeof T.e[i]);
    return write_dir();
}

int store_debug(char *out, int cap) {
    int sd_debug(char *out, int cap);
    int n = sd_debug(out, cap);
    if (!T.open) return n + snprintf(out + n, (size_t)(cap - n), "store: not open (%s)\n", T.why);
    n += snprintf(out + n, (size_t)(cap - n), "store: at %lu MiB, gen %lu, copy %c, %lu of %lu MiB free\n",
                  (unsigned long)(T.base >> 11), (unsigned long)T.gen, T.copy ? 'B' : 'A',
                  (unsigned long)(store_free() >> 20), (unsigned long)(store_total() >> 20));
    for (int i = 0; i < STORE_MAX && n < cap - 160; i++) {
        const store_entry_t *e = &T.e[i];
        if (!e->flags) continue;
        n += snprintf(out + n, (size_t)(cap - n), "  %-40s %10lu / %10lu bytes %s at %lu MiB  %s\n", e->name,
                      (unsigned long)e->written, (unsigned long)e->size, (e->flags & STORE_DONE) ? "done" : "part",
                      (unsigned long)(e->lba >> 11), e->meta);
    }
    return n;
}
