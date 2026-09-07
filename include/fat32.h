#pragma once
#include <stdint.h>
#include "blk.h"

/* ===========================================================================
 *  FAT32 filesystem: mount/format a block device and read/write files.
 *
 *  Standard on-disk format, so volumes are interchangeable with Windows/Linux
 *  and USB sticks. Long File Names are parsed on read; writes emit 8.3 short
 *  names (uppercased/truncated). One volume mounted at a time.
 * ===========================================================================*/

#define FAT_ATTR_DIR    0x10
#define FAT_ATTR_VOLID  0x08

typedef struct {
    char     name[256];      /* long name if present, else 8.3 */
    uint32_t size;
    uint8_t  attr;
    uint32_t cluster;
} fat_dirent;

int  fat32_mount(blkdev_t *dev);          /* whole device; 0 on success */
int  fat32_mounted(void);
void fat32_unmount(void);
int  fat32_format(blkdev_t *dev, const char *label);

/* Partition-relative variants. Every LBA inside the driver is volume-relative,
 * so a FAT32 volume can live in a partition -- which is what an EFI system
 * partition is, and therefore what the installer needs to create one.
 * `span` is the volume length in sectors (0 = to the end of the device). */
int  fat32_mount_at (blkdev_t *dev, uint64_t base, uint64_t span);
int  fat32_format_at(blkdev_t *dev, uint64_t base, uint64_t span, const char *label);

/* List directory at absolute path ("/", "/sub"). Returns count, fills up to max. */
int  fat32_list(const char *path, fat_dirent *out, int max);

/* Read whole file into buf (up to max). Returns bytes read, <0 on error. */
int  fat32_read(const char *path, void *buf, uint32_t max);

/* File size without reading it; -1 if missing or a directory. */
int64_t fat32_size(const char *path);

/* Create/overwrite a file with the given bytes. Returns 0 on success. */
int  fat32_write(const char *path, const void *data, uint32_t len);

/* Create a directory. Returns 0 on success. */
int  fat32_mkdir(const char *path);

/* Barrier: return only once every write to the mounted volume is on media.
 * Called on the shutdown path so a FAT volume is safe to unplug. */
int  fat32_flush(void);

const char *fat32_label(void);
uint64_t    fat32_free_bytes(void);
uint64_t    fat32_total_bytes(void);
