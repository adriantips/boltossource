#pragma once
#include <stdint.h>

/* Handed from stage2.asm (physical 0x0500) into kmain via RDI. */
struct bootinfo {
    uint64_t fb_addr;
    uint32_t fb_width, fb_height, fb_pitch, fb_bpp;
    uint32_t e820_count;
    uint32_t e820_addr;
    uint32_t boot_flags;      /* BOOT_* below, chosen in the boot menu     */
    uint32_t boot_drive;      /* BIOS drive number (legacy), 0xFF for UEFI */
} __attribute__((packed));

/* ---------------------------------------------------------------------------
 *  Boot-menu selection. stage2.asm / uefi_boot.c write these into
 *  bootinfo.boot_flags; kmain reads them back through boot_flags().
 *
 *  Safe mode is the "my desktop won't come up" escape hatch: the kernel boots
 *  to the text shell with the network and the compositor left down, so a bad
 *  wallpaper, a wedged NIC or a crashing app can be undone from a prompt.
 *  Recovery goes further and also skips the persistent filesystem's autosave,
 *  so a damaged image can be inspected without being rewritten.
 * ------------------------------------------------------------------------- */
#define BOOT_SAFE      0x00000001u   /* no GUI, no network bring-up          */
#define BOOT_RECOVERY  0x00000002u   /* + read-only FS, minimal drivers      */
#define BOOT_FSCK      0x00000004u   /* force a filesystem check on mount    */
#define BOOT_UEFI      0x00000008u   /* booted through BOOTX64.EFI           */

/* ---------------------------------------------------------------------------
 *  BoltOS boot header  -  one sector, written by build.sh and by the installer
 *  at BOOT_HDR_LBA on every bootable BoltOS disk.
 *
 *  It exists so stage2 does not have to be rebuilt whenever the kernel changes
 *  size: stage2 reads the header and learns where the kernel lives and how many
 *  sectors to pull in. That in turn lets the kernel carry a *prebuilt* stage2
 *  blob, which is what makes `install` able to lay down a working boot chain on
 *  another disk from inside BoltOS itself.
 * ------------------------------------------------------------------------- */
#define BOOT_HDR_LBA     33u          /* stage1 @0, stage2 @1..32, header @33 */
#define BOOT_KERNEL_LBA  34u          /* kernel image starts here             */
#define BOOT_HDR_MAGIC   "BOLTBHDR"   /* 8 bytes, no NUL                      */

struct boot_header {
    char     magic[8];                /* BOOT_HDR_MAGIC                       */
    uint32_t version;                 /* BOOT_HDR_VERSION                     */
    uint32_t kernel_lba;              /* first sector of kernel.bin           */
    uint32_t kernel_sectors;          /* sectors to load                      */
    uint32_t kernel_bytes;            /* exact image length                   */
    uint32_t flags;                   /* reserved                             */
    uint32_t kernel_sum;              /* v2: sum32 of the padded kernel image */
    uint32_t checksum;                /* sum32 of the first 32 bytes          */
} __attribute__((packed));

/* v1 headers exist on disks written by an older installer and still boot; they
 * simply cannot be checked. v2 adds kernel_sum: the 32-bit sum of every dword
 * of kernel_sectors * 512 bytes starting at kernel_lba, i.e. of the image as
 * padded out to a whole sector. stage2 recomputes it over what actually landed
 * at 1 MiB and refuses to jump into a mismatch, which is what turns a silently
 * truncated USB read into a message instead of a hang. */
#define BOOT_HDR_VERSION 2u

/* Boot-time state, published by kmain (kernel/main.c). */
uint32_t    boot_flags(void);         /* BOOT_* bitmask for this boot         */
int         boot_is_safe(void);       /* BOOT_SAFE or BOOT_RECOVERY           */
int         boot_is_recovery(void);
const char *boot_mode_name(void);     /* "normal" / "safe" / "recovery"       */
