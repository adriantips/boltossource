/* Execute the real block layer and BoltFS against a volatile disk cache.
 * Only hardware, the allocator and boot/account state are substituted. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>

#include "../../kernel/blk.c"
#include "../../fs/ramfs.c"

static int alloc_fail = -1, allocations, live_allocations;
static int recovery;
static uint64_t ticks;

void *kmalloc(uint64_t n) {
    if (alloc_fail >= 0 && allocations++ == alloc_fail) return 0;
    void *p = malloc((size_t)n);
    if (p) live_allocations++;
    return p;
}
void kfree(void *p) { if (p) { live_allocations--; free(p); } }
uint64_t pit_ticks(void) { return ++ticks; }
int users_current_uid(void) { return 0; }
int users_current_gid(void) { return 0; }
int boot_is_recovery(void) { return recovery; }
uint32_t boot_flags(void) { return recovery ? BOOT_RECOVERY : 0; }
void kprintf(const char *fmt, ...) { (void)fmt; }
/* fs_init() panics if the root node cannot be allocated; a test that
 * reaches it has found something worth stopping for. */
void panic(const char *msg) { fprintf(stderr, "PANIC: %s\n", msg); abort(); }

/* Protecting a disk read-only is a decision the person using the machine has
 * to be told about, so it is part of what these tests check, not a stub that
 * merely satisfies the linker. */
static int notifications;
uint32_t notify_post(int level, const char *source, const char *text) {
    (void)level; (void)source; (void)text; return ++notifications;
}
uint32_t notify_error(const char *source, const char *text) {
    return notify_post(3, source, text);
}

#define DISK_BYTES (4u * 1024 * 1024)
static uint8_t durable[DISK_BYTES], cache[DISK_BYTES];
static int io_step, fail_step, uncertain, writes;
static uint64_t read_fail_lba = UINT64_MAX;

static int disk_read(blkdev_t *d, uint64_t lba, uint32_t n, void *buf) {
    (void)d;
    if (lba <= read_fail_lba && read_fail_lba - lba < n) return -1;
    assert(lba * BLK_SECTOR + n * BLK_SECTOR <= DISK_BYTES);
    memcpy(buf, cache + lba * BLK_SECTOR, n * BLK_SECTOR);
    return 0;
}
static int disk_write(blkdev_t *d, uint64_t lba, uint32_t n, const void *buf) {
    (void)d;
    assert(lba * BLK_SECTOR + n * BLK_SECTOR <= DISK_BYTES);
    writes++;
    if (++io_step == fail_step) {
        /* A failed sector write can still have transferred some of its bytes. */
        if (uncertain) memcpy(cache + lba * BLK_SECTOR, buf, 29);
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
    alloc_fail = -1; allocations = 0;
    if (root_) node_free_tree(root_);
    root_ = cwd_ = 0;
    assert(live_allocations == 0);
    g_disk = 0; g_base = g_cap = g_slot_sectors = g_gen = g_mounts = 0;
    g_slot = g_autosave = g_readonly = g_unclean = 0;
    g_n = recovery = io_step = fail_step = writes = uncertain = 0;
    read_fail_lba = UINT64_MAX;
    memset(cache, 0, sizeof cache); memset(durable, 0, sizeof durable);
    blkdev_t d = {0};
    strcpy(d.name, "ata1");
    d.sectors = DISK_BYTES / BLK_SECTOR;
    d.sector_size = BLK_SECTOR;
    d.read = disk_read; d.write = disk_write; d.flush = disk_flush;
    assert(blk_register(&d) == 0);
    root_ = cwd_ = node_new("/", 1, 0);
    assert(root_);
    fs_node *f = fs_create("/keep", 0);
    assert(f && fs_write(f, "old contents", 12) == 0);
}
static void attach(void) {
    g_disk = blk_get(0); g_base = 2048;
    g_cap = g_disk->sectors - g_base;
    g_slot_sectors = (g_cap - BFS_SLOT0) / 2;
    assert(blk_write_mbr_single(g_disk, BLK_PART_BOLTFS, g_base) == 0);
    assert(fs_sync() == 0);
    io_step = writes = 0;
}
static void assert_contents(const char *text) {
    fs_node *f = fs_lookup("/keep");
    assert(f && f->size == strlen(text) && !memcmp(f->data, text, f->size));
}
static void publish_blob(const uint8_t *blob, uint64_t size, uint32_t count) {
    struct bfs_super sb;
    sb_fill(&sb, 0, 3, count, size, fnv32(blob, size), 1);
    memcpy(cache + slot_lba(0) * BLK_SECTOR, blob, (size_t)size);
    memcpy(cache + g_base * BLK_SECTOR, &sb, sizeof sb);
    memcpy(cache + (g_base + 1) * BLK_SECTOR, &sb, sizeof sb);
    memcpy(durable, cache, sizeof cache);
}

static void allocation_failures(void) {
    reset(); attach();
    uint32_t cnt; uint64_t bytes, sectors;
    uint8_t *blob = serialise(&cnt, &bytes, &sectors);
    assert(blob);
    int baseline = live_allocations;
    fs_node *original = root_;
    int failures = 0;
    for (int point = 0; point < 20; point++) {
        alloc_fail = point; allocations = 0;
        int rc = rebuild(blob, bytes, cnt, 3);
        alloc_fail = -1;
        assert(live_allocations == baseline);
        assert_contents("old contents");
        if (rc == 0) break;
        assert(rc == -2 && root_ == original);
        failures++;
    }
    assert(failures == 4); /* index, root, file node, payload */
    kfree(blob);

    /* A transient OOM at any mount allocation must not select an older
     * generation or autoformat. Even a seeded tree must remain untouched. */
    for (int point = 0; point < 5; point++) {
        reset(); attach();
        assert(fs_write(fs_lookup("/keep"), "unsaved seed", 12) == 0);
        fs_node *seed = root_;
        baseline = live_allocations;
        g_disk = 0; writes = 0; alloc_fail = point; allocations = 0;
        fs_persist_init();
        alloc_fail = -1;
        assert(root_ == seed && live_allocations == baseline && writes == 0);
        assert(fs_persist_readonly() && fs_sync() != 0);
        assert_contents("unsaved seed");
        assert(!memcmp(cache, durable, DISK_BYTES));
    }
    puts("  ok: every load allocation can fail without losing the live tree or writing the disk");
}

static void malformed_images(void) {
    for (int kind = 0; kind < 11; kind++) {
        reset(); attach();
        struct bfs_rec r[3] = {0};
        r[0].parent = BFS_NOPAR; r[0].is_dir = 1; strcpy(r[0].name, "/");
        r[1].parent = 0; r[1].is_dir = 1; strcpy(r[1].name, "dir");
        r[2].parent = 1; strcpy(r[2].name, "file");
        uint64_t bytes = sizeof r; uint32_t cnt = 3;
        switch (kind) {
        case 0: r[0].parent = 0; break;
        case 1: r[0].is_dir = 0; break;
        case 2: r[1].is_dir = 0; break; /* a file cannot be a parent */
        case 3: r[2].parent = 2; break;
        case 4: memset(r[2].name, 'x', FS_NAME_MAX); break;
        case 5: strcpy(r[2].name, ".."); break;
        case 6: strcpy(r[2].name, "a/b"); break;
        case 7: r[2].is_dir = 2; break;
        case 8: r[1].size = 1; break;
        case 9: cnt = 2; break; /* trailing record not counted */
        case 10: r[2].parent = 0; strcpy(r[2].name, "dir"); break;
        }
        publish_blob((uint8_t *)r, bytes, cnt);
        fs_node *seed = root_;
        int baseline = live_allocations;
        assert(rebuild((uint8_t *)r, bytes, cnt, 3) != 0);
        assert(root_ == seed && live_allocations == baseline);
        fs_check_report report;
        assert(fs_check(0, &report) < 0 && !report.structure_ok);
        assert(live_allocations == baseline);
        writes = 0; g_disk = 0;
        fs_persist_init();
        assert(root_ == seed && writes == 0 && fs_persist_readonly());
        assert(fs_check(1, &report) < 0 && writes == 0);
        assert_contents("old contents");
    }
    reset(); attach();
    struct bfs_super sb;
    assert(sb_read(g_base, &sb) == 0);
    sb.blob_bytes = UINT64_MAX;
    sb.hdr_sum = fnv32((uint8_t *)&sb, sizeof sb - 8);
    memcpy(cache + g_base * BLK_SECTOR, &sb, sizeof sb);
    assert(sb_read(g_base, &sb) != 0);
    puts("  ok: mount and fsck reject corrupt records and overflowing image bounds");
}

static void commit_failures(void) {
    for (int mode = 0; mode < 2; mode++) {
        for (int point = 1; point <= 6; point++) {
            reset(); attach();
            assert(fs_write(fs_lookup("/keep"), "new contents", 12) == 0);
            io_step = 0; fail_step = point; uncertain = mode; notifications = 0;
            assert(fs_sync() < 0 && fs_persist_readonly());
            assert(notifications == 1);   /* and the user is told, exactly once */
            int steps = io_step;
            assert(fs_sync() < 0 && fs_shutdown() < 0 && io_step == steps);
            /* Pull the plug: discard everything not made durable by a flush. */
            memcpy(cache, durable, sizeof cache);
            fail_step = 0; io_step = 0; g_readonly = 0; g_disk = 0;
            fs_persist_init();
            fs_node *f = fs_lookup("/keep");
            assert(f && f->size == 12);
            assert(!memcmp(f->data, "old contents", 12) || !memcmp(f->data, "new contents", 12));
            assert(!fs_persist_readonly());
            fs_check_report rep;
            assert(fs_check(0, &rep) == 0 && rep.checksum_ok && rep.structure_ok);
        }
    }
    puts("  ok: all six commit write/flush failures fence retries and survive power loss");
}

static void selection(void) {
    reset();
    assert(blk_is_blank_scratch(blk_get(0)));
    cache[DISK_BYTES - 1] = 1;
    assert(!blk_is_blank_scratch(blk_get(0)) && !blk_fs_disk());
    cache[DISK_BYTES - 1] = 0;
    read_fail_lba = DISK_BYTES / BLK_SECTOR - 1;
    assert(!blk_is_blank_scratch(blk_get(0)));
    read_fail_lba = UINT64_MAX;
    assert(blk_write_mbr_single(blk_get(0), 0x83, 2048) == 0);
    memcpy(cache + BLK_SECTOR, "BOLTFS2", 8); /* stale raw header behind foreign MBR */
    assert(!blk_fs_disk());
    attach();
    /* A malformed or overlapping table cannot authorise use of the disk. */
    wr_le32(cache + BLK_MBR_PART_OFF + 12, 0xffffffffu);
    assert(!blk_boltfs_partition(blk_get(0), 0));
    assert(blk_write_mbr_single(blk_get(0), BLK_PART_BOLTFS, 2048) == 0);
    memcpy(cache + BLK_MBR_PART_OFF + 16, cache + BLK_MBR_PART_OFF, 16);
    assert(!blk_boltfs_partition(blk_get(0), 0));
    assert(blk_read(blk_get(0), UINT64_MAX, 2, cache) < 0);
    assert(blk_write(blk_get(0), UINT64_MAX, 2, cache) < 0);
    reset(); recovery = 1;
    fs_persist_init();
    assert(writes == 0 && !fs_persist_active());
    puts("  ok: foreign data, failed reads, bad partitions, and recovery never authorise formatting");
}

static void migration(void) {
    reset(); attach();
    struct bfs_rec2 r[2] = {0};
    r[0].parent = BFS_NOPAR; r[0].is_dir = 1; strcpy(r[0].name, "/");
    r[1].parent = 0; strcpy(r[1].name, "legacy");
    struct bfs_super1 sb = {0};
    memcpy(sb.magic, BFS_MAGIC1, 8);
    sb.version = 1; sb.node_count = 2; sb.blob_bytes = sizeof r;
    for (unsigned i = 0; i < sizeof r; i++) sb.checksum += ((uint8_t *)r)[i];
    memset(cache + g_base * BLK_SECTOR, 0, BLK_SECTOR * 2);
    memcpy(cache + g_base * BLK_SECTOR, &sb, sizeof sb);
    memcpy(cache + (g_base + 1) * BLK_SECTOR, r, sizeof r);
    memcpy(durable, cache, sizeof cache);
    g_disk = 0;
    fs_persist_init();
    assert(fs_lookup("/legacy") && g_slot == 1 && g_gen == 1);
    fs_check_report rep;
    assert(fs_check(0, &rep) == 0);
    puts("  ok: legacy v1 migrates into the opposite half of the disk");
}

static void mirror_and_limits(void) {
    reset(); attach();
    struct bfs_super older, newer;
    assert(sb_read(g_base, &older) == 0);
    assert(fs_write(fs_lookup("/keep"), "new contents", 12) == 0);
    assert(fs_sync() == 0 && sb_read(g_base, &newer) == 0);
    uint8_t *blob = cache + slot_lba(newer.slot) * BLK_SECTOR;
    wr_le32(blob + sizeof(struct bfs_rec), 1); /* corrupt child's parent */
    newer.blob_sum = fnv32(blob, newer.blob_bytes);
    newer.hdr_sum = fnv32((uint8_t *)&newer, sizeof newer - 8);
    memcpy(cache + g_base * BLK_SECTOR, &newer, sizeof newer);
    memcpy(cache + (g_base + 1) * BLK_SECTOR, &older, sizeof older);
    g_disk = 0;
    fs_persist_init();
    assert_contents("old contents");
    fs_check_report rep;
    assert(!fs_persist_readonly() && fs_check(0, &rep) == 0);

    reset();
    fs_node *d = root_;
    for (int i = 0; i < FS_MAX_DEPTH; i++) {
        fs_set_cwd(d);
        d = fs_create("d", 1);
        assert(d);
    }
    fs_set_cwd(d);
    assert(!fs_create("too-deep", 1));
    assert(fs_move(fs_lookup("/keep"), d, "keep") != 0);
    assert(!fs_create("..", 1));
    assert(!fs_create("this-file-name-is-too-long-to-store", 0));
    attach();
    assert(fs_check(0, &rep) == 0);
    puts("  ok: malformed newest tree falls back cleanly; mutations cannot exceed loader limits");
}

static void read_and_save_failures(void) {
    reset(); attach();
    fs_node *seed = root_;
    /* A successful signature read followed by a failed blob read must also
     * protect the original disk. Both superblocks point at this same blob. */
    read_fail_lba = slot_lba(g_slot);
    writes = 0; g_disk = 0;
    fs_persist_init();
    assert(root_ == seed && fs_persist_readonly() && !writes);
    assert_contents("old contents");
    read_fail_lba = UINT64_MAX;
    assert(fs_sync() < 0 && !writes);

    reset(); attach();
    alloc_fail = 0; allocations = 0; writes = 0;
    assert(fs_sync() < 0 && !writes && !fs_persist_readonly());
    alloc_fail = -1;
    assert(!fs_sync()); /* no I/O occurred, so retrying after OOM is safe */
    puts("  ok: disk read errors protect the original; pre-I/O allocation failures can be retried");
}

int main(void) {
    allocation_failures(); malformed_images(); commit_failures(); selection(); migration(); mirror_and_limits();
    read_and_save_failures();
    alloc_fail = -1; node_free_tree(root_); root_ = 0;
    assert(live_allocations == 0);
    puts("PASS storage-host: allocation failures, corrupt images, disk selection, interrupted commits");
    return 0;
}
