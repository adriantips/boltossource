#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Generic block-device layer.
 *
 *  Storage drivers (ATA PIO, NVMe, USB mass-storage, ...) register a blkdev
 *  with read/write callbacks; the filesystem and tools talk only to this
 *  interface, so the backing transport is interchangeable. Sectors are the
 *  device's logical block size (512 for everything BoltOS currently mounts).
 * ===========================================================================*/

#define BLK_SECTOR 512u

typedef struct blkdev {
    char     name[16];      /* "ata0", "nvme0n1", ...                        */
    char     model[48];     /* human model string                            */
    uint64_t sectors;       /* total addressable logical blocks              */
    uint32_t sector_size;   /* bytes per logical block (512)                 */
    int      is_ssd;        /* 1 = solid-state media                         */
    int      is_boot;       /* 1 = boot medium; FS avoids persisting here    */
    void    *drv;           /* driver-private handle (ata_dev*, nvme_ns*)    */
    int (*read )(struct blkdev *, uint64_t lba, uint32_t count, void *);
    int (*write)(struct blkdev *, uint64_t lba, uint32_t count, const void *);
    /* Push the device's write cache to stable media. Optional: a driver that
     * already completes writes durably leaves it null. Without this, a disk
     * that reports a write complete while it still sits in DRAM can reorder it
     * past the commit that is supposed to follow it -- which is exactly the
     * ordering BoltFS's atomic commit depends on. */
    int (*flush)(struct blkdev *);
    /* Put the controller back after the machine has slept. S3 keeps the RAM
     * and nothing else: the queues, descriptor rings and buffers a driver
     * allocated are all still where they were, and every register that pointed
     * at them is zero. A driver whose transport genuinely survives (ATA PIO is
     * port I/O and no more) leaves this null and blk_resume_all() treats that
     * as success. Returns 0 if the device can be used again. */
    int (*resume)(struct blkdev *);
} blkdev_t;

/* Register a device (the struct is copied into the table). Returns its index
 * or -1 if the table is full. */
int       blk_register(const blkdev_t *dev);
int       blk_count(void);
blkdev_t *blk_get(int i);

/* Prefer an installed boot disk, then existing BoltFS on a data disk, then a
 * completely zero-filled scratch disk (at most 64 MiB). Unknown disks are
 * never adopted. NULL -> FS stays RAM-only; use the installer for a new disk. */
blkdev_t *blk_fs_disk(void);
/* Read every sector, not just the signature: 1 only for a wholly empty,
 * writable, non-boot disk of at most 64 MiB. Larger disks need `install`. */
int       blk_is_blank_scratch(blkdev_t *d);

const char *blk_media(const blkdev_t *d);   /* "NVMe" / "SSD" / "HDD" */

int  blk_read (blkdev_t *d, uint64_t lba, uint32_t count, void *buf);
int  blk_write(blkdev_t *d, uint64_t lba, uint32_t count, const void *buf);
/* Barrier: returns only once everything already written is on stable media.
 * 0 on success (including "device needs no flush"), -1 if the device reported
 * a failure -- a caller ordering a commit around it must treat that as fatal. */
int  blk_flush(blkdev_t *d);

/* Bring every registered device back after a suspend. Returns the number that
 * could NOT be resumed; those are left in the table and will fail their next
 * read or write rather than pretending. The caller is expected to say so --
 * a filesystem quietly writing into a dead controller is worse than one that
 * has been told it cannot. */
int  blk_resume_all(void);

/* MBR partition table (LBA 0), transport-agnostic (drives blk_read/write). */
#define BLK_MBR_SIG_OFF   510
#define BLK_MBR_PART_OFF  446
#define BLK_PART_BOLTFS   0xBF

typedef struct blk_part {
    int      present;
    uint8_t  boot;
    uint8_t  type;
    uint64_t lba_start;
    uint64_t sectors;
} blk_part;

int blk_read_mbr(blkdev_t *d, blk_part out[4]);
/* One unambiguous, in-bounds BoltFS partition; rejects GPT/hybrid tables. */
int blk_boltfs_partition(blkdev_t *d, blk_part *out);
int blk_write_mbr_single(blkdev_t *d, uint8_t type, uint64_t start);
