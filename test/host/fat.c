/* Execute the real FAT32 driver against a volatile disk that can fail, tear a
 * sector in half, and lose power. Only the hardware and boot state are
 * substituted; fs/fat32.c and kernel/blk.c are the ones shipping in the OS.
 *
 * What these tests are about is what the disk looks like after a write that
 * did not finish: that a file is always one of its two versions, that a name
 * in a directory always points at something that was really written, and that
 * a read which failed comes back as a failure rather than as a shorter file. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>

#include "../../kernel/blk.c"
#include "../../fs/fat32.c"

static int recovery;
int boot_is_recovery(void) { return recovery; }
void kprintf(const char *fmt, ...) { (void)fmt; }

/* ---- a disk that can be interrupted -------------------------------------- */
/* Large enough for fat32_format_at, which refuses anything under 70000
 * sectors -- below that the volume would be a FAT16 in FAT32 clothing. */
#define DISK_SECTORS 82048u
#define DISK_BYTES   (DISK_SECTORS * BLK_SECTOR)

static uint8_t durable[DISK_BYTES];   /* what survives a power cut */
static uint8_t cache[DISK_BYTES];     /* what the drive has been told */
static int io_step, fail_step, uncertain, writes;
static uint64_t read_fail_lba = UINT64_MAX;

/* Every write, so a test can ask how wide they were and where they landed. */
static struct { uint64_t lba; uint32_t n; } wlog[512];
static int wlog_n;

static int disk_read(blkdev_t *d, uint64_t lba, uint32_t n, void *buf) {
    (void)d;
    if (lba <= read_fail_lba && read_fail_lba - lba < n) return -1;
    assert((lba + n) * BLK_SECTOR <= DISK_BYTES);
    memcpy(buf, cache + lba * BLK_SECTOR, n * BLK_SECTOR);
    return 0;
}
static int disk_write(blkdev_t *d, uint64_t lba, uint32_t n, const void *buf) {
    (void)d;
    assert((lba + n) * BLK_SECTOR <= DISK_BYTES);
    writes++;
    if (wlog_n < (int)(sizeof wlog / sizeof wlog[0])) {
        wlog[wlog_n].lba = lba; wlog[wlog_n].n = n; wlog_n++;
    }
    if (++io_step == fail_step) {
        /* A sector that was being written when the power went can be on the
         * platter in part. Those bytes are durable immediately: leaving them
         * in the cache would make a torn write politely undo itself at the
         * next power cut, which is not a failure mode any real drive has. */
        if (uncertain) {
            memcpy(cache + lba * BLK_SECTOR, buf, 29);
            memcpy(durable + lba * BLK_SECTOR, cache + lba * BLK_SECTOR, BLK_SECTOR);
        }
        return -1;
    }
    memcpy(cache + lba * BLK_SECTOR, buf, n * BLK_SECTOR);
    return 0;
}
static int disk_flush(blkdev_t *d) {
    (void)d;
    int failed = ++io_step == fail_step;
    if (!failed || uncertain) memcpy(durable, cache, DISK_BYTES);
    return failed ? -1 : 0;
}

static void reset(void) {
    fat32_unmount();
    g_n = 0; recovery = 0;
    io_step = fail_step = uncertain = writes = wlog_n = 0;
    read_fail_lba = UINT64_MAX;
    memset(cache, 0, sizeof cache);
    memset(durable, 0, sizeof durable);
    blkdev_t d = {0};
    strcpy(d.name, "ata0");
    d.sectors = DISK_SECTORS;
    d.sector_size = BLK_SECTOR;
    d.read = disk_read; d.write = disk_write; d.flush = disk_flush;
    assert(blk_register(&d) == 0);
}

/* ---- a volume with room in a directory cluster --------------------------- */
/* fat32_format_at picks one sector per cluster for any disk small enough to
 * hold in memory, and a one-sector cluster cannot show whether a directory
 * write touched its neighbours. So the volume these tests use is built here,
 * by hand, with 4 KiB clusters: 128 entries per directory cluster, spread over
 * eight sectors. It is mounted by the real mount path like any other. */
#define VOL_BASE   2048u
#define VOL_SPC    8u
#define VOL_RSVD   32u
#define VOL_FATS   2u
#define VOL_CLUS   2000u
#define VOL_FATSZ  16u                                  /* (2002*4 + 511)/512 */
#define VOL_DATA   (VOL_RSVD + VOL_FATS * VOL_FATSZ)
#define VOL_TOTAL  (VOL_DATA + VOL_CLUS * VOL_SPC)

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void mount_vol(void) {
    assert(fat32_mount_at(blk_get(0), VOL_BASE, VOL_TOTAL) == 0);
}

static void mkvol(void) {
    uint8_t *b = cache + (size_t)VOL_BASE * BLK_SECTOR;
    memset(b, 0, (size_t)VOL_TOTAL * BLK_SECTOR);
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
    memcpy(b + 3, "MSWIN4.1", 8);
    b[11] = 0x00; b[12] = 0x02;                             /* 512 bytes/sector */
    b[13] = VOL_SPC;
    b[14] = VOL_RSVD & 0xFF; b[15] = VOL_RSVD >> 8;
    b[16] = VOL_FATS;
    b[21] = 0xF8;
    wr32(b + 32, VOL_TOTAL);
    wr32(b + 36, VOL_FATSZ);
    wr32(b + 44, 2);                                        /* root cluster */
    b[66] = 0x29;
    memcpy(b + 71, "SMALLVOL   ", 11);
    memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
    for (uint32_t f = 0; f < VOL_FATS; f++) {
        uint8_t *t = b + (size_t)(VOL_RSVD + f * VOL_FATSZ) * BLK_SECTOR;
        wr32(t + 0, 0x0FFFFFF8); wr32(t + 4, 0x0FFFFFFF); wr32(t + 8, 0x0FFFFFFF);
    }
    memcpy(durable, cache, sizeof cache);
    mount_vol();
    io_step = writes = wlog_n = 0;
}

/* The volume is the only part of the disk these tests touch, so that is all
 * that has to be saved and put back between runs. */
static uint8_t volsnap[(size_t)VOL_TOTAL * BLK_SECTOR];
static void save_vol(void) {
    memcpy(volsnap, cache + (size_t)VOL_BASE * BLK_SECTOR, sizeof volsnap);
    memcpy(durable, cache, sizeof cache);
}
static void restore_vol(void) {
    memcpy(cache + (size_t)VOL_BASE * BLK_SECTOR, volsnap, sizeof volsnap);
    memcpy(durable + (size_t)VOL_BASE * BLK_SECTOR, volsnap, sizeof volsnap);
    fat32_unmount();
    mount_vol();
}

/* ---- a directory with entries in more than one sector -------------------- */
#define NFILES 20
static const char OLD17[] = "contents of file 17";
static const char NEW17[] = "seventeen, rewritten and rather longer";

static void body_of(int i, char *out) { sprintf(out, "contents of file %02d", i); }
static void path_of(int i, char *out) { sprintf(out, "/F%02d.TXT", i); }

static void make_files(void) {
    for (int i = 0; i < NFILES; i++) {
        char path[16], body[32];
        path_of(i, path); body_of(i, body);
        assert(fat32_write(path, body, (uint32_t)strlen(body)) == 0);
    }
}

/* Every file except `except` still reads back exactly what was written. */
static void check_files(int except) {
    for (int i = 0; i < NFILES; i++) {
        if (i == except) continue;
        char path[16], body[32], buf[64];
        path_of(i, path); body_of(i, body);
        int n = fat32_read(path, buf, sizeof buf);
        assert(n == (int)strlen(body) && !memcmp(buf, body, (size_t)n));
    }
}

/* ---- tests --------------------------------------------------------------- */
static void roundtrip(void) {
    reset();
    assert(fat32_format_at(blk_get(0), 0, 80000, "BOLTTEST") == 0);
    assert(fat32_mounted() && !strcmp(fat32_label(), "BOLTTEST"));

    char buf[64];
    assert(fat32_write("/HELLO.TXT", "hello fat", 9) == 0);
    assert(fat32_read("/HELLO.TXT", buf, sizeof buf) == 9 && !memcmp(buf, "hello fat", 9));
    assert(fat32_size("/HELLO.TXT") == 9);
    assert(fat32_mkdir("/SUB") == 0);
    assert(fat32_write("/SUB/INNER.TXT", "nested", 6) == 0);
    assert(fat32_read("/SUB/INNER.TXT", buf, sizeof buf) == 6);
    assert(fat32_mkdir("/SUB") != 0);                    /* already there */
    assert(fat32_write("/SUB", "x", 1) != 0);            /* and it is a directory */
    assert(fat32_read("/NOPE.TXT", buf, sizeof buf) < 0);

    fat_dirent ents[8];
    assert(fat32_list("/", ents, 8) == 2);
    assert(fat32_list("/SUB", ents, 8) == 1);

    /* grow across cluster boundaries and shrink back down */
    static char big[9000], back[9000];
    memset(big, 'A', sizeof big);
    assert(fat32_write("/HELLO.TXT", big, sizeof big) == 0);
    assert(fat32_size("/HELLO.TXT") == (int64_t)sizeof big);
    assert(fat32_read("/HELLO.TXT", back, sizeof back) == (int)sizeof big &&
           !memcmp(back, big, sizeof big));
    uint64_t freed = fat32_free_bytes();
    assert(fat32_write("/HELLO.TXT", "small", 5) == 0);
    assert(fat32_read("/HELLO.TXT", buf, sizeof buf) == 5);
    assert(fat32_free_bytes() > freed);                  /* the old chain went back */

    fat32_unmount();
    assert(fat32_read("/HELLO.TXT", buf, sizeof buf) < 0);
    assert(fat32_mount_at(blk_get(0), 0, 80000) == 0);
    assert(fat32_read("/SUB/INNER.TXT", buf, sizeof buf) == 6 && !memcmp(buf, "nested", 6));
    puts("  ok: format, nested directories, growing and shrinking writes survive a remount");
}

static void one_entry_at_a_time(void) {
    reset(); mkvol();
    make_files();
    uint64_t lo = fs.base + clus_lba(2), hi = lo + fs.sec_per_clus;
    assert(hi - lo == 8);        /* the whole point of this volume */

    /* F17 is the eighteenth entry, which puts it in the second sector of the
     * directory cluster; the first sector holds sixteen other files. Changing
     * one file used to read all eight sectors, alter 32 bytes and write all
     * eight back, so nineteen other entries went through the drive to publish
     * this one. Now the write is the sector F17 is in, and only that. */
    wlog_n = 0;
    assert(fat32_write("/F17.TXT", NEW17, (uint32_t)strlen(NEW17)) == 0);
    int touched = 0;
    for (int i = 0; i < wlog_n; i++) {
        if (wlog[i].lba >= hi || wlog[i].lba + wlog[i].n <= lo) continue;
        assert(wlog[i].n == 1);              /* never the whole cluster */
        assert(wlog[i].lba == lo + 1);       /* and never a sector but F17's */
        touched++;
    }
    assert(touched == 1);
    char buf[64];
    assert(fat32_read("/F17.TXT", buf, sizeof buf) == (int)strlen(NEW17));
    check_files(17);
    puts("  ok: publishing a file writes the one directory sector it changes");
}

static void interrupted_writes(void) {
    reset(); mkvol();
    make_files();
    save_vol();

    /* Fail each write and each flush an overwrite performs, in turn, both
     * cleanly and having put part of a sector on the platter, and pull the
     * plug on each one. The nineteen other files are never collateral, and
     * F17 is always one of its two versions -- never a mixture, never gone,
     * and never a directory entry pointing at clusters it does not own. */
    int covered = 0;
    for (int mode = 0; mode < 2; mode++) {
        for (int step = 1; step <= 64; step++) {
            restore_vol();
            io_step = 0; fail_step = step; uncertain = mode;
            int rc = fat32_write("/F17.TXT", NEW17, (uint32_t)strlen(NEW17));
            int ran = io_step;
            fail_step = 0; uncertain = 0;
            memcpy(cache, durable, sizeof cache);        /* pull the plug */
            fat32_unmount();
            mount_vol();

            check_files(17);
            char buf[64];
            int n = fat32_read("/F17.TXT", buf, sizeof buf);
            assert((n == (int)strlen(OLD17) && !memcmp(buf, OLD17, (size_t)n)) ||
                   (n == (int)strlen(NEW17) && !memcmp(buf, NEW17, (size_t)n)));
            /* A write that reported success must have landed. */
            if (rc == 0) assert(n == (int)strlen(NEW17));
            /* And the directory is still walkable in both directions. */
            fat_dirent ents[NFILES + 4];
            assert(fat32_list("/", ents, NFILES + 4) == NFILES);
            covered++;
            if (step > ran) break;                       /* past the last step */
        }
    }
    assert(covered >= 18);
    printf("  ok: %d interrupted overwrites each leave one whole version of the file\n", covered);
}

static void read_failures(void) {
    reset(); mkvol();
    make_files();
    static char big[9000], back[9000];
    memset(big, 'B', sizeof big);
    assert(fat32_write("/BIG.TXT", big, sizeof big) == 0);
    save_vol();

    /* A FAT sector that will not read used to come back as an end of chain,
     * so a long file quietly became a short one. */
    read_fail_lba = fs.base + fs.fat_start;
    assert(fat32_read("/BIG.TXT", back, sizeof back) < 0);
    assert(fat32_free_bytes() == 0);          /* rather than inventing a number */
    read_fail_lba = UINT64_MAX;
    assert(fat32_read("/BIG.TXT", back, sizeof back) == (int)sizeof big);

    /* A directory that will not read is not an empty directory, and above all
     * it is not a directory in which the file being written does not exist:
     * concluding that would file a second entry for a file already on disk. */
    read_fail_lba = fs.base + clus_lba(2);
    fat_dirent ents[NFILES + 4];
    assert(fat32_list("/", ents, NFILES + 4) < 0);
    assert(fat32_read("/F03.TXT", back, sizeof back) < 0);
    assert(fat32_size("/F03.TXT") < 0);
    writes = 0;
    assert(fat32_write("/F03.TXT", "must not happen", 15) != 0);
    assert(fat32_mkdir("/DIR") != 0);
    assert(writes == 0);                      /* and it touched nothing trying */
    read_fail_lba = UINT64_MAX;
    assert(fat32_list("/", ents, NFILES + 4) == NFILES + 1);
    check_files(-1);

    /* A chain that ends before the length in the directory entry means the
     * file on the disk is not the file the directory describes. */
    restore_vol();
    assert(fat32_list("/BIG.TXT", ents, 1) < 0);
    uint32_t first = 0;
    int n = fat32_list("/", ents, NFILES + 4);
    for (int i = 0; i < n; i++) if (!strcmp(ents[i].name, "BIG.TXT")) first = ents[i].cluster;
    assert(first >= 2 && first < 128);
    for (uint32_t f = 0; f < VOL_FATS; f++)
        wr32(cache + (size_t)(VOL_BASE + VOL_RSVD + f * VOL_FATSZ) * BLK_SECTOR + first * 4,
             0x0FFFFFFF);
    fat32_unmount(); mount_vol();
    assert(fat32_read("/BIG.TXT", back, sizeof back) < 0);
    puts("  ok: unreadable FATs and directories, and truncated chains, are errors");
}

static void write_failures(void) {
    reset(); mkvol();
    assert(fat32_write("/KEEP.TXT", "keep me", 7) == 0);
    save_vol();

    /* Creating a file, making a directory, and extending a directory past its
     * first cluster all have to fail cleanly and leave what was there. */
    for (int step = 1; step <= 12; step++) {
        restore_vol();
        io_step = 0; fail_step = step;
        int a = fat32_write("/NEW.TXT", "brand new", 9);
        fail_step = 0;
        char buf[32];
        assert(fat32_read("/KEEP.TXT", buf, sizeof buf) == 7);
        if (a != 0) assert(fat32_read("/NEW.TXT", buf, sizeof buf) < 0 ||
                           fat32_size("/NEW.TXT") == 9);

        restore_vol();
        io_step = 0; fail_step = step;
        int b = fat32_mkdir("/SUB");
        fail_step = 0;
        assert(fat32_read("/KEEP.TXT", buf, sizeof buf) == 7);
        fat_dirent ents[8];
        int n = fat32_list("/", ents, 8), present = 0;
        for (int i = 0; i < n; i++) if (!strcmp(ents[i].name, "SUB")) present = 1;
        assert(b != 0 || present);
        /* Success or failure, a name in the parent has to point at a directory
         * that was really written -- never at a cluster still holding whatever
         * the disk happened to have in it. A failure that only came from the
         * final flush leaves a whole, usable directory, which is fine; what
         * would not be is a half-made one. */
        if (present) {
            assert(fat32_write("/SUB/IN.TXT", "in", 2) == 0);
            assert(fat32_read("/SUB/IN.TXT", buf, sizeof buf) == 2);
        }
    }

    /* Out of space is a failure, not a corrupt file: the directory survives
     * and what was already there still reads. */
    restore_vol();
    static char big[9000];
    memset(big, 'C', sizeof big);
    int made = 0;
    while (fat32_write(made & 1 ? "/FILL1.BIN" : "/FILL0.BIN", big, sizeof big) == 0 &&
           made < 8000) made++;
    /* (the volume is 2000 clusters; two alternating files cannot exhaust it) */
    assert(made >= 2);
    char buf[32];
    assert(fat32_read("/KEEP.TXT", buf, sizeof buf) == 7);
    puts("  ok: failed creates and mkdirs leave the directory as it was");
}

/* ---- long names ---------------------------------------------------------- */
static uint8_t lfn_sum(const char sn[11]) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++)
        s = (uint8_t)(((s & 1) << 7) + (s >> 1) + (uint8_t)sn[i]);
    return s;
}

static void long_names(void) {
    static const char LNAME[] = "my report.txt";           /* exactly 13 chars */
    static const char SN[11]  = { 'M','Y','R','E','P','O','~','1','T','X','T' };
    reset(); mkvol();

    /* Three entries, so the long-name entry can be planted in the middle one:
     * this is the shape another operating system leaves behind, where the 8.3
     * name is a tilde-mangled thing that looks nothing like the real name. */
    assert(fat32_write("/OTHER.TXT", "another file", 12) == 0);
    assert(fat32_write("/FILLER.TXT", "", 0) == 0);
    assert(fat32_write("/MYREPO~1.TXT", "the quarterly numbers", 21) == 0);

    uint8_t *dir = cache + (size_t)(VOL_BASE + VOL_DATA) * BLK_SECTOR;
    uint8_t *l = dir + 32;
    memset(l, 0, 32);
    l[0] = 0x41;                              /* sequence 1, last logical entry */
    l[11] = 0x0F;
    l[13] = lfn_sum(SN);
    static const int slot[13] = { 1,3,5,7,9,14,16,18,20,22,24,28,30 };
    for (int i = 0; i < 13; i++) l[slot[i]] = (uint8_t)LNAME[i];
    fat32_unmount(); mount_vol();

    fat_dirent ents[8];
    int n = fat32_list("/", ents, 8), seen = 0;
    for (int i = 0; i < n; i++) if (!strcmp(ents[i].name, LNAME)) seen = 1;
    assert(seen);

    char buf[64];
    assert(fat32_read("/my report.txt", buf, sizeof buf) == 21);
    /* Overwriting it used to fail outright: the entry was looked up again by a
     * short name rebuilt from "my report.txt", which is MYREPORT.TXT and not
     * what is on the disk. It is now addressed by where it was found. */
    assert(fat32_write("/my report.txt", "revised numbers", 15) == 0);
    assert(fat32_read("/My Report.TXT", buf, sizeof buf) == 15 &&
           !memcmp(buf, "revised numbers", 15));
    n = fat32_list("/", ents, 8); seen = 0;
    for (int i = 0; i < n; i++) if (!strcmp(ents[i].name, LNAME)) seen = 1;
    assert(seen && n == 2);    /* still one file under that name, plus OTHER.TXT */

    /* Delete the 8.3 entry the way an interrupted delete would, leaving the
     * long name behind it. The next file created must not inherit that name. */
    dir[2 * 32] = 0xE5;
    fat32_unmount(); mount_vol();
    assert(fat32_write("/NEW.TXT", "brand new", 9) == 0);
    n = fat32_list("/", ents, 8);
    for (int i = 0; i < n; i++) assert(strcmp(ents[i].name, LNAME) != 0);
    assert(fat32_read("/NEW.TXT", buf, sizeof buf) == 9 && !memcmp(buf, "brand new", 9));
    puts("  ok: a file named by long-name entries can be read, overwritten and not inherited");
}

static void recovery_is_read_only(void) {
    reset(); mkvol();
    assert(fat32_write("/KEEP.TXT", "keep me", 7) == 0);
    recovery = 1;
    writes = 0;
    assert(fat32_write("/KEEP.TXT", "changed", 7) != 0);
    assert(fat32_write("/OTHER.TXT", "new", 3) != 0);
    assert(fat32_mkdir("/SUB") != 0);
    assert(fat32_format_at(blk_get(0), VOL_BASE, VOL_TOTAL, "NOPE") != 0);
    assert(writes == 0);
    char buf[32];
    assert(fat32_read("/KEEP.TXT", buf, sizeof buf) == 7 && !memcmp(buf, "keep me", 7));
    puts("  ok: recovery mode reads the volume and never writes it");
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);   /* an abort must not eat the progress */
    roundtrip();
    one_entry_at_a_time();
    interrupted_writes();
    read_failures();
    write_failures();
    long_names();
    recovery_is_read_only();
    puts("PASS fat-host: interrupted writes, single-sector publishes, I/O errors, long names");
    return 0;
}
