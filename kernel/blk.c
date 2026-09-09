#include <stdint.h>
#include <stddef.h>
#include "blk.h"
#include "string.h"

/* ===========================================================================
 *  Generic block layer: a small registry plus transport-agnostic MBR helpers.
 * ===========================================================================*/

#define BLK_MAX 8
static blkdev_t g_dev[BLK_MAX];
static int      g_n;

int blk_register(const blkdev_t *dev) {
    if (g_n >= BLK_MAX || !dev || !dev->read) return -1;
    g_dev[g_n] = *dev;
    if (g_dev[g_n].sector_size == 0) g_dev[g_n].sector_size = BLK_SECTOR;
    return g_n++;
}
int       blk_count(void)      { return g_n; }
blkdev_t *blk_get(int i)       { return (i >= 0 && i < g_n) ? &g_dev[i] : 0; }

const char *blk_media(const blkdev_t *d) {
    if (!d) return "";
    if (d->name[0] == 'n') return "NVMe";        /* nvme* */
    return d->is_ssd ? "SSD" : "HDD";
}

/* Does this device carry a BoltFS partition? */
int blk_boltfs_partition(blkdev_t *d, blk_part *out) {
    blk_part parts[4];
    if (blk_read_mbr(d, parts) <= 0) return 0;
    int found = -1;
    for (int i = 0; i < 4; i++) {
        if (!parts[i].present) continue;
        if (parts[i].type == 0xEE) return 0;  /* GPT, including a hybrid MBR */
        if (parts[i].type != BLK_PART_BOLTFS) continue;
        if (found >= 0) return 0;             /* ambiguous root */
        found = i;
    }
    if (found < 0) return 0;
    if (out) *out = parts[found];
    return 1;
}

int blk_is_blank_scratch(blkdev_t *d) {
    if (!d || d->is_boot || !d->write || d->sector_size != BLK_SECTOR ||
        d->sectors < 4096 || d->sectors > (64ull << 20) / BLK_SECTOR) return 0;
    /* Bound the scan to the small scratch images shipped by build.sh. A real
     * multi-terabyte disk must not stall boot while we look for its last byte
     * of data. An empty first sector is never evidence of an empty disk. */
    uint8_t buf[32 * BLK_SECTOR];
    for (uint64_t lba = 0; lba < d->sectors; lba += 32) {
        uint32_t n = d->sectors - lba < 32 ? (uint32_t)(d->sectors - lba) : 32;
        if (blk_read(d, lba, n, buf) != 0) return 0;
        for (uint32_t i = 0; i < n * BLK_SECTOR; i++) if (buf[i]) return 0;
    }
    return 1;
}

static int raw_boltfs(blkdev_t *d) {
    uint8_t sec[BLK_SECTOR];
    for (uint64_t lba = 0; lba < 2; lba++) {
        if (blk_read(d, lba, 1, sec) != 0) return 0;
        /* Never reinterpret partitioned media as a whole-disk image, even
         * if stale BoltFS bytes happen to remain in the gap after its MBR. */
        if (!lba && sec[BLK_MBR_SIG_OFF] == 0x55 && sec[BLK_MBR_SIG_OFF + 1] == 0xAA) return 0;
        if (!memcmp(sec, "BOLTFS1", 8) || !memcmp(sec, "BOLTFS2", 8)) return 1;
    }
    return 0;
}

static int media_rank(blkdev_t *d) {
    return d->name[0] == 'n' ? 0 : d->is_ssd ? 1 : 2;
}

blkdev_t *blk_fs_disk(void) {
    /* Existing data wins over a faster empty disk. Never use an unrelated
     * partition just because it happens to be on the first NVMe controller. */
    for (int i = 0; i < g_n; i++) {
        blkdev_t *d = &g_dev[i];
        if (d->sector_size == BLK_SECTOR && d->is_boot &&
            blk_boltfs_partition(d, 0)) return d;
    }
    for (int rank = 0; rank < 3; rank++) {
        for (int i = 0; i < g_n; i++) {
            blkdev_t *d = &g_dev[i];
            if (d->is_boot || d->sector_size != BLK_SECTOR || media_rank(d) != rank) continue;
            if (blk_boltfs_partition(d, 0) || raw_boltfs(d)) return d;
        }
    }
    for (int rank = 0; rank < 3; rank++)
        for (int i = 0; i < g_n; i++)
            if (media_rank(&g_dev[i]) == rank && blk_is_blank_scratch(&g_dev[i])) return &g_dev[i];
    return 0;
}

int blk_read(blkdev_t *d, uint64_t lba, uint32_t count, void *buf) {
    if (!d || !d->read) return -1;
    if (lba > d->sectors || count > d->sectors - lba) return -1;
    return d->read(d, lba, count, buf);
}
int blk_write(blkdev_t *d, uint64_t lba, uint32_t count, const void *buf) {
    if (!d || !d->write) return -1;
    if (lba > d->sectors || count > d->sectors - lba) return -1;
    return d->write(d, lba, count, buf);
}
int blk_flush(blkdev_t *d) {
    if (!d) return -1;
    if (!d->flush) return 0;            /* driver writes are already durable */
    return d->flush(d);
}

/* ---------------------------------------------------------------- MBR ------*/
static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int blk_read_mbr(blkdev_t *d, blk_part out[4]) {
    if (!d || !out || d->sector_size != BLK_SECTOR) return -1;
    memset(out, 0, 4 * sizeof *out);
    uint8_t sec[BLK_SECTOR];
    if (blk_read(d, 0, 1, sec) != 0) return -1;
    if (sec[BLK_MBR_SIG_OFF] != 0x55 || sec[BLK_MBR_SIG_OFF + 1] != 0xAA) return -1;

    int n = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sec + BLK_MBR_PART_OFF + i * 16;
        blk_part *p = &out[i];
        memset(p, 0, sizeof *p);
        p->boot      = e[0];
        p->type      = e[4];
        p->lba_start = rd_le32(e + 8);
        p->sectors   = rd_le32(e + 12);
        if (!p->type && !p->sectors && !p->lba_start) continue;
        if (!p->type || !p->sectors || !p->lba_start ||
            p->lba_start >= d->sectors || p->sectors > d->sectors - p->lba_start) return -1;
        p->present = 1; n++;
        for (int j = 0; j < i; j++) {
            blk_part *q = &out[j];
            if (q->present && p->lba_start < q->lba_start + q->sectors &&
                q->lba_start < p->lba_start + p->sectors) return -1;
        }
    }
    return n;
}

int blk_write_mbr_single(blkdev_t *d, uint8_t type, uint64_t start) {
    if (!d || d->sector_size != BLK_SECTOR || start == 0 ||
        start > 0xFFFFFFFFull || start >= d->sectors) return -1;
    uint8_t sec[BLK_SECTOR];
    memset(sec, 0, sizeof sec);

    uint64_t count = d->sectors - start;
    if (count > 0xFFFFFFFFull) count = 0xFFFFFFFFull;

    uint8_t *e = sec + BLK_MBR_PART_OFF;
    e[0] = 0x00;
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
    e[4] = type;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    wr_le32(e + 8,  (uint32_t)start);
    wr_le32(e + 12, (uint32_t)count);
    sec[BLK_MBR_SIG_OFF]     = 0x55;
    sec[BLK_MBR_SIG_OFF + 1] = 0xAA;

    return blk_write(d, 0, 1, sec);
}
