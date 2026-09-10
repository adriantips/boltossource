/* ===========================================================================
 *  BoltOS  -  kernel/install.c
 *  The installer: write a bootable BoltOS onto a target disk from inside a
 *  running BoltOS. See include/install.h for the on-disk layout it produces.
 *
 *  The boot chain travels inside the kernel image: build.sh embeds stage1.bin,
 *  stage2.bin and BOOTX64.EFI as blobs, so a running system carries its own
 *  bootloader for both firmware paths. The only piece that cannot be embedded
 *  is kernel.bin -- a kernel cannot contain a copy of itself -- so it is read
 *  back from the medium this machine booted from, which is also what makes the
 *  copy byte-exact rather than a snapshot of mutated .data.
 * ===========================================================================*/
#include <stdint.h>
#include "install.h"
#include "blk.h"
#include "boot.h"
#include "fs.h"
#include "fat32.h"
#include "users.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "commands.h"    /* sh_utoa */

/* Boot-chain blobs, embedded by build.sh (see [1/6] in build.sh). */
extern const uint8_t _binary_stage1_bin_start[];
extern const uint8_t _binary_stage1_bin_end[];
extern const uint8_t _binary_stage2_bin_start[];
extern const uint8_t _binary_stage2_bin_end[];
extern const uint8_t _binary_BOOTX64_EFI_start[];
extern const uint8_t _binary_BOOTX64_EFI_end[];

#define MIB_SECTORS      2048u             /* 1 MiB at 512 B/sector          */
#define DEFAULT_ESP_MB   64u
#define MIN_TARGET_MB    192u              /* boot chain + ESP + a usable FS  */

#define PART_TYPE_ESP    0x0C              /* FAT32 LBA (what firmware wants) */

static void step(install_req *r, int pct, const char *what) {
    if (!r) return;
    r->percent = pct;
    strncpy(r->step, what, INSTALL_STEP_LEN - 1);
    r->step[INSTALL_STEP_LEN - 1] = 0;
    kprintf("[install] %3d%%  %s\n", pct, what);
}

static int fail(install_req *r, const char *why) {
    if (r) {
        r->failed = 1;
        strncpy(r->error, why, INSTALL_ERR_LEN - 1);
        r->error[INSTALL_ERR_LEN - 1] = 0;
    }
    kprintf("[install] FAILED: %s\n", why);
    return -1;
}

/* --------------------------------------------------------------- targets ---*/
/* Any writable disk except the one we booted from. The disk this system is
 * persisting to IS allowed: installing from live media with a single disk
 * attached is the normal case, and BoltFS auto-mounts that disk. install_run
 * detaches persistence from it first (fs_persist_detach), so the tree stays
 * live in RAM while the disk is repartitioned underneath. Erasing the boot
 * medium, by contrast, would pull the floor out from under the running
 * install, so it stays excluded. */
static int is_target(blkdev_t *d) {
    return d && !d->is_boot && d->write;
}

int install_target_count(void) {
    int n = 0;
    for (int i = 0; i < blk_count(); i++) if (is_target(blk_get(i))) n++;
    return n;
}

blkdev_t *install_target_get(int i) {
    for (int k = 0; k < blk_count(); k++) {
        blkdev_t *d = blk_get(k);
        if (!is_target(d)) continue;
        if (i-- == 0) return d;
    }
    return 0;
}

static blkdev_t *find_disk(const char *name) {
    for (int i = 0; i < blk_count(); i++) {
        blkdev_t *d = blk_get(i);
        if (d && strcmp(d->name, name) == 0) return d;
    }
    return 0;
}

/* ------------------------------------------------- locating kernel.bin -----*/
/* Source 1: a BoltOS boot header at LBA 33 of any block device. Covers booting
 * from a BoltOS image (os.img) and from an already-installed BoltOS disk. */
static int header_on(blkdev_t *d, struct boot_header *out) {
    uint8_t sec[BLK_SECTOR];
    if (!d || d->sectors <= BOOT_HDR_LBA) return 0;
    if (blk_read(d, BOOT_HDR_LBA, 1, sec) != 0) return 0;
    struct boot_header *h = (struct boot_header *)sec;
    if (memcmp(h->magic, BOOT_HDR_MAGIC, 8) != 0) return 0;
    if (h->version != 1 || h->kernel_sectors == 0 || h->kernel_bytes == 0) return 0;
    if (h->kernel_lba + h->kernel_sectors > d->sectors) return 0;
    *out = *h;
    return 1;
}

/* Source 2: kernel.bin on a FAT32 volume -- the EFI system partition we booted
 * from under UEFI, where there is no boot header anywhere on the disk. */
static uint8_t *kernel_from_fat(uint64_t *len_out) {
    static const char *paths[] = { "/kernel.bin", "/EFI/BOLTOS/kernel.bin",
                                   "/EFI/BOOT/kernel.bin" };
    uint8_t *img = 0;

    for (int i = 0; i < blk_count() && !img; i++) {
        blkdev_t *d = blk_get(i);
        if (!d) continue;

        /* Try the whole device, then each partition that looks like an ESP.
         * QEMU's virtual-FAT drive presents a partition table; a real ESP is
         * always a partition; a plain formatted stick may be neither. */
        uint64_t bases[5]; uint64_t spans[5]; int nb = 0;
        bases[nb] = 0; spans[nb] = d->sectors; nb++;
        blk_part parts[4];
        if (blk_read_mbr(d, parts) > 0) {
            for (int p = 0; p < 4 && nb < 5; p++) {
                if (!parts[p].present) continue;
                if (parts[p].type != 0x0C && parts[p].type != 0x0B &&
                    parts[p].type != 0xEF && parts[p].type != 0x06) continue;
                bases[nb] = parts[p].lba_start; spans[nb] = parts[p].sectors; nb++;
            }
        }

        for (int b = 0; b < nb && !img; b++) {
            if (fat32_mount_at(d, bases[b], spans[b]) != 0) continue;
            for (unsigned p = 0; p < sizeof(paths) / sizeof(paths[0]) && !img; p++) {
                int64_t sz = fat32_size(paths[p]);
                if (sz <= 0) continue;
                uint8_t *buf = (uint8_t *)kmalloc((uint32_t)sz);
                if (!buf) continue;
                int got = fat32_read(paths[p], buf, (uint32_t)sz);
                if (got == (int)sz) { img = buf; *len_out = (uint64_t)got; }
                else kfree(buf);
            }
        }
    }
    return img;
}

/* Read the kernel image this machine booted. Caller frees. */
static uint8_t *find_kernel_image(uint64_t *len_out) {
    *len_out = 0;

    for (int i = 0; i < blk_count(); i++) {
        blkdev_t *d = blk_get(i);
        struct boot_header h;
        if (!header_on(d, &h)) continue;
        uint64_t sectors = h.kernel_sectors;
        uint8_t *buf = (uint8_t *)kmalloc(sectors * BLK_SECTOR);
        if (!buf) continue;
        if (blk_read(d, h.kernel_lba, (uint32_t)sectors, buf) == 0) {
            *len_out = h.kernel_bytes;
            return buf;
        }
        kfree(buf);
    }
    return kernel_from_fat(len_out);
}

/* How big is the kernel we could install -- without reading it.
 *
 * This used to call find_kernel_image() and immediately free the result: 7.5 MB
 * of PIO to learn a number the boot header carries in its own field, done once
 * for the summary line and then again by install_describe() for every disk in
 * the list. `install` sat silent for twenty-odd seconds before printing
 * anything, which under load was long enough for the test suite to give up on
 * it -- and long enough for a person to assume it had hung.
 *
 * The header answers it in one sector. Under UEFI there is no header, so the
 * ESP's directory entry answers it without reading the file. */
static uint64_t kernel_size_from_fat(void) {
    static const char *paths[] = { "/kernel.bin", "/EFI/BOLTOS/kernel.bin",
                                   "/EFI/BOOT/kernel.bin" };
    for (int i = 0; i < blk_count(); i++) {
        blkdev_t *d = blk_get(i);
        if (!d) continue;
        uint64_t bases[5], spans[5]; int nb = 0;
        bases[nb] = 0; spans[nb] = d->sectors; nb++;
        blk_part parts[4];
        if (blk_read_mbr(d, parts) > 0) {
            for (int p = 0; p < 4 && nb < 5; p++) {
                if (!parts[p].present) continue;
                if (parts[p].type != 0x0C && parts[p].type != 0x0B &&
                    parts[p].type != 0xEF && parts[p].type != 0x06) continue;
                bases[nb] = parts[p].lba_start; spans[nb] = parts[p].sectors; nb++;
            }
        }
        for (int b = 0; b < nb; b++) {
            if (fat32_mount_at(d, bases[b], spans[b]) != 0) continue;
            for (unsigned p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
                int64_t sz = fat32_size(paths[p]);
                if (sz > 0) return (uint64_t)sz;
            }
        }
    }
    return 0;
}

uint64_t install_kernel_available(void) {
    for (int i = 0; i < blk_count(); i++) {
        struct boot_header h;
        if (header_on(blk_get(i), &h)) return h.kernel_bytes;
    }
    return kernel_size_from_fat();
}

/* ------------------------------------------------------ partition table ----*/
static void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void part_entry(uint8_t *e, uint8_t boot, uint8_t type,
                       uint64_t start, uint64_t count) {
    e[0] = boot;
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;      /* CHS start: unused/max */
    e[4] = type;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;      /* CHS end:   unused/max */
    if (count > 0xFFFFFFFFull) count = 0xFFFFFFFFull;
    wr_le32(e + 8,  (uint32_t)start);
    wr_le32(e + 12, (uint32_t)count);
}

/* Round up to the next 1 MiB boundary; partitions that start there keep SSDs
 * and 4Kn drives from doing read-modify-write on every single access. */
static uint64_t align_mib(uint64_t lba) {
    return ((lba + MIB_SECTORS - 1) / MIB_SECTORS) * MIB_SECTORS;
}

/* --------------------------------------------------------------- layout ----*/
struct layout {
    uint64_t kernel_lba, kernel_sectors, kernel_bytes;
    uint64_t esp_lba, esp_sectors;
    uint64_t fs_lba, fs_sectors;
};

static int plan(blkdev_t *d, uint64_t kbytes, uint32_t esp_mb, struct layout *L) {
    L->kernel_bytes   = kbytes;
    L->kernel_sectors = (kbytes + BLK_SECTOR - 1) / BLK_SECTOR;
    L->kernel_lba     = BOOT_KERNEL_LBA;

    /* The boot chain lives in the gap before the first partition, the way a
     * classic MBR loader embeds its core image there. It is a big gap -- the
     * kernel is several MiB -- but this is our own partition table, so the
     * first partition simply starts after it. */
    L->esp_lba     = align_mib(L->kernel_lba + L->kernel_sectors);
    L->esp_sectors = (uint64_t)(esp_mb ? esp_mb : DEFAULT_ESP_MB) * MIB_SECTORS;

    L->fs_lba = align_mib(L->esp_lba + L->esp_sectors);
    if (L->fs_lba + MIB_SECTORS * 16 > d->sectors) return -1;   /* need >=16 MiB */
    L->fs_sectors = d->sectors - L->fs_lba;
    return 0;
}

int install_describe(const char *disk, char *out, int cap) {
    blkdev_t *d = find_disk(disk);
    out[0] = 0;
    if (!d) { strncpy(out, "no such disk", cap - 1); out[cap - 1] = 0; return (int)strlen(out); }

    uint64_t kb = install_kernel_available();
    struct layout L;
    if (!kb || plan(d, kb, DEFAULT_ESP_MB, &L) != 0) {
        strncpy(out, kb ? "disk too small for an install" :
                          "kernel image not found on the boot medium", cap - 1);
        out[cap - 1] = 0;
        return (int)strlen(out);
    }
    /* Sizes in MiB; the shell/GUI just print this back to the user. */
    char n1[16], n2[16], n3[16];
    sh_utoa((L.kernel_bytes + 1048575) / 1048576, n1);
    sh_utoa((L.esp_sectors * BLK_SECTOR) / 1048576, n2);
    sh_utoa((L.fs_sectors * BLK_SECTOR) / 1048576, n3);
    out[0] = 0;
    kstrlcat(out, "boot chain + kernel ", cap); kstrlcat(out, n1, cap);
    kstrlcat(out, " MiB, EFI partition ", cap);  kstrlcat(out, n2, cap);
    kstrlcat(out, " MiB, BoltFS ", cap);         kstrlcat(out, n3, cap);
    kstrlcat(out, " MiB", cap);
    return (int)strlen(out);
}

/* ------------------------------------------------------------- the work ----*/
int install_run(install_req *r) {
    if (!r) return -1;
    r->done = r->failed = 0; r->error[0] = 0;

    blkdev_t *d = find_disk(r->disk);
    if (!d)            return fail(r, "no such disk");
    if (!is_target(d)) return fail(r, "that disk is the boot or system disk");
    if (d->sectors * BLK_SECTOR < (uint64_t)MIN_TARGET_MB * 1048576ull)
        return fail(r, "target disk is too small (need 192 MiB)");
    if (!r->username[0]) return fail(r, "a username is required");

    /* If we are about to erase the disk this system persists to, let go of it
     * first. The in-RAM tree is untouched -- and it is what gets exported to the
     * target below, so nothing is lost. */
    if (d == fs_persist_disk()) {
        kprintf("[install] %s currently backs this system's filesystem;\n"
                "[install] detaching it (the live tree stays in RAM and is what\n"
                "[install] gets copied to the target)\n", d->name);
        fs_persist_detach();
    }

    step(r, 2, "reading the kernel from the boot medium");
    uint64_t kbytes = 0;
    uint8_t *kimg = find_kernel_image(&kbytes);
    if (!kimg) return fail(r, "kernel image not found on the boot medium");

    struct layout L;
    if (plan(d, kbytes, r->esp_mb, &L) != 0) { kfree(kimg); return fail(r, "disk too small"); }

    /* --- 1. boot chain ----------------------------------------------------- */
    step(r, 10, "writing stage1 and the partition table");
    uint8_t mbr[BLK_SECTOR];
    memset(mbr, 0, sizeof mbr);
    uint64_t s1len = (uint64_t)(_binary_stage1_bin_end - _binary_stage1_bin_start);
    if (s1len > BLK_SECTOR) s1len = BLK_SECTOR;
    memcpy(mbr, _binary_stage1_bin_start, s1len);
    /* Overlay the partition table on top of stage1's code+data. stage1 is well
     * under 446 bytes, so the table and the 0x55AA signature sit in space it
     * does not use -- the same arrangement every MBR bootloader relies on. */
    part_entry(mbr + BLK_MBR_PART_OFF +  0, 0x80, PART_TYPE_ESP,  L.esp_lba, L.esp_sectors);
    part_entry(mbr + BLK_MBR_PART_OFF + 16, 0x00, BLK_PART_BOLTFS, L.fs_lba,  L.fs_sectors);
    mbr[BLK_MBR_SIG_OFF] = 0x55; mbr[BLK_MBR_SIG_OFF + 1] = 0xAA;
    if (blk_write(d, 0, 1, mbr) != 0) { kfree(kimg); return fail(r, "MBR write failed"); }

    step(r, 15, "writing stage2");
    uint64_t s2len = (uint64_t)(_binary_stage2_bin_end - _binary_stage2_bin_start);
    uint64_t s2sec = (s2len + BLK_SECTOR - 1) / BLK_SECTOR;
    if (s2sec > 32) { kfree(kimg); return fail(r, "stage2 blob exceeds its 32-sector slot"); }
    {
        uint8_t pad[32 * BLK_SECTOR];
        memset(pad, 0, sizeof pad);
        memcpy(pad, _binary_stage2_bin_start, s2len);
        if (blk_write(d, 1, (uint32_t)s2sec, pad) != 0) {
            kfree(kimg); return fail(r, "stage2 write failed");
        }
    }

    step(r, 20, "writing the kernel");
    {
        /* Pad the tail to a whole sector so the last partial sector is written. */
        uint64_t bytes = L.kernel_sectors * BLK_SECTOR;
        uint8_t *padded = (uint8_t *)kmalloc(bytes);
        if (!padded) { kfree(kimg); return fail(r, "out of memory"); }
        memset(padded, 0, bytes);
        memcpy(padded, kimg, kbytes);
        int rc = blk_write(d, L.kernel_lba, (uint32_t)L.kernel_sectors, padded);
        kfree(padded);
        if (rc != 0) { kfree(kimg); return fail(r, "kernel write failed"); }
    }
    kfree(kimg);

    step(r, 40, "writing the boot header");
    {
        /* Must match struct boot_header and stage2.asm's read_bhdr byte for byte. */
        uint8_t sec[BLK_SECTOR];
        memset(sec, 0, sizeof sec);
        struct boot_header *h = (struct boot_header *)sec;
        memcpy(h->magic, BOOT_HDR_MAGIC, 8);
        h->version        = 1;
        h->kernel_lba     = (uint32_t)L.kernel_lba;
        h->kernel_sectors = (uint32_t)L.kernel_sectors;
        h->kernel_bytes   = (uint32_t)L.kernel_bytes;
        h->flags          = 0;
        uint32_t sum = 0;
        for (unsigned i = 0; i < sizeof(struct boot_header) - sizeof(uint32_t); i++)
            sum += sec[i];
        h->checksum = sum;
        if (blk_write(d, BOOT_HDR_LBA, 1, sec) != 0) return fail(r, "boot header write failed");
    }
    blk_flush(d);

    /* --- 2. EFI system partition ------------------------------------------- */
    step(r, 50, "formatting the EFI system partition");
    /* Remember what was mounted so the running system keeps its FAT volume. */
    int had_fat = fat32_mounted();
    if (fat32_format_at(d, L.esp_lba, L.esp_sectors, "BOLTESP") != 0)
        return fail(r, "could not format the EFI system partition");

    step(r, 60, "installing BOOTX64.EFI");
    if (fat32_mkdir("/EFI") != 0 || fat32_mkdir("/EFI/BOOT") != 0)
        return fail(r, "could not create /EFI/BOOT on the ESP");
    uint64_t eflen = (uint64_t)(_binary_BOOTX64_EFI_end - _binary_BOOTX64_EFI_start);
    if (fat32_write("/EFI/BOOT/BOOTX64.EFI", _binary_BOOTX64_EFI_start, (uint32_t)eflen) != 0)
        return fail(r, "could not write BOOTX64.EFI");

    step(r, 70, "copying the kernel to the ESP");
    {
        /* The UEFI loader reads kernel.bin from the ESP root, so it needs its
         * own copy -- firmware cannot see the raw sectors in the MBR gap. */
        uint64_t klen = 0;
        uint8_t *k2 = find_kernel_image(&klen);
        if (!k2) return fail(r, "kernel image vanished mid-install");
        int rc = fat32_write("/kernel.bin", k2, (uint32_t)klen);
        kfree(k2);
        if (rc != 0) return fail(r, "could not copy kernel.bin to the ESP");
    }
    fat32_flush();

    /* --- 3. the filesystem, with the new account --------------------------- */
    step(r, 85, "creating the user account");
    {
        /* The account has to exist in the tree that gets exported, so it is
         * added here and the export below carries it to the target. */
        int uid = 1000;
        for (int i = 0; i < users_count(); i++) {
            const user_t *u = users_get(i);
            if (u && u->uid >= uid) uid = u->uid + 1;
        }
        if (users_auth(r->username, r->password) < 0)
            users_add(r->username, r->password, uid);
        else
            users_set_password(r->username, r->password);
    }

    step(r, 90, "copying the filesystem to the target");
    if (fs_export_image(d, L.fs_lba, L.fs_sectors) != 0)
        return fail(r, "could not write the filesystem image");
    blk_flush(d);

    /* Restore the running system's FAT mount, which formatting the ESP stole. */
    if (had_fat) {
        fat32_unmount();
        for (int i = 0; i < blk_count(); i++) {
            blkdev_t *o = blk_get(i);
            if (o && o != d && fat32_mount(o) == 0) break;
        }
    }

    step(r, 100, "done");
    r->done = 1;
    kprintf("[install] BoltOS installed on %s (%s '%s')\n",
            d->name, blk_media(d), d->model[0] ? d->model : "disk");
    kprintf("[install]   legacy: stage1@0 stage2@1 header@%u kernel@%lu (%lu sectors)\n",
            BOOT_HDR_LBA, (unsigned long)L.kernel_lba, (unsigned long)L.kernel_sectors);
    kprintf("[install]   UEFI:   ESP @LBA %lu (%lu MiB) with EFI/BOOT/BOOTX64.EFI\n",
            (unsigned long)L.esp_lba, (unsigned long)(L.esp_sectors / MIB_SECTORS));
    kprintf("[install]   BoltFS: @LBA %lu (%lu MiB), account '%s'\n",
            (unsigned long)L.fs_lba, (unsigned long)(L.fs_sectors / MIB_SECTORS),
            r->username);
    return 0;
}
