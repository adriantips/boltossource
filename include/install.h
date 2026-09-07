#pragma once
#include <stdint.h>
#include "blk.h"
#include "users.h"

/* ===========================================================================
 *  BoltOS installer.
 *
 *  Writes a self-contained, bootable BoltOS onto a target disk from inside a
 *  running BoltOS -- no host toolchain, no dd, no second machine. It lays down
 *  both boot paths so the result boots on legacy BIOS and on UEFI firmware:
 *
 *    LBA 0        stage1 (MBR) with the partition table overlaid
 *    LBA 1..32    stage2
 *    LBA 33       boot header (kernel location + size, see boot.h)
 *    LBA 34..     kernel.bin
 *    partition 1  FAT32 EFI system partition: EFI/BOOT/BOOTX64.EFI + kernel.bin
 *    partition 2  BoltFS, seeded with a copy of the running filesystem
 *
 *  stage1, stage2 and BOOTX64.EFI travel inside the kernel image as blobs
 *  (build.sh embeds them), so the only thing that has to be found at install
 *  time is kernel.bin itself -- read back from whatever medium this system
 *  booted from.
 * ===========================================================================*/

#define INSTALL_STEP_LEN  64
#define INSTALL_ERR_LEN   96

typedef struct {
    /* --- inputs --- */
    char disk[16];                    /* target blkdev name, e.g. "ata1"      */
    char username[USER_NAME_MAX];     /* account created on the target        */
    char password[40];
    uint32_t esp_mb;                  /* EFI system partition size (0 = 64)   */

    /* --- progress, safe to poll from a GUI tick --- */
    int  percent;                     /* 0..100                                */
    char step[INSTALL_STEP_LEN];      /* what it is doing right now            */
    int  done;                        /* finished successfully                 */
    int  failed;
    char error[INSTALL_ERR_LEN];
} install_req;

/* Disks that can legitimately be installed to: everything except the medium
 * this system booted from and the disk its own filesystem is mounted on. */
int        install_target_count(void);
blkdev_t  *install_target_get(int i);

/* Can the kernel image be recovered from the boot medium? Without it there is
 * nothing to install, so the UI can say so before the user commits to anything.
 * Returns the image size in bytes, or 0. */
uint64_t   install_kernel_available(void);

/* Do it. Destroys everything on the target. Fills in req->percent/step as it
 * goes; returns 0 on success, -1 with req->error set on failure. */
int        install_run(install_req *req);

/* Human-readable summary of the layout install_run would write, for a
 * confirmation screen. Returns the number of bytes written to out. */
int        install_describe(const char *disk, char *out, int cap);
