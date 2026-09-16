#include <stdint.h>
#include <stdio.h>
#include "commands.h"
#include "usb.h"
#include "xhci.h"
#include "blk.h"
#include "fat32.h"
#include "ext2.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_usb.c
 *  `usb` -- what is plugged in, and how to get at what is on it.
 *
 *  The mounting half exists because of a limit worth naming rather than
 *  working around badly: fs/fat32.c holds ONE volume at a time. So on a
 *  machine with a FAT partition on its internal disk and a stick in the front,
 *  only one of them is reachable, and which one it is was decided at boot by
 *  the order the drivers happened to probe in. `usb mount` is how you change
 *  your mind about that without rebooting.
 *
 *  The alternative -- a real multi-volume VFS mount table -- is the right
 *  answer and is a bigger job than a USB driver.
 * ===========================================================================*/

static void usage(void) {
    kprintf("usage: usb                       list USB input, storage and mounts\n"
            "       usb mount [NAME]          mount a USB disk's FAT32 volume\n"
            "       usb umount                flush and release the volume\n"
            "       usb format NAME --yes [L] make a fresh FAT32 volume on it\n"
            "  NAME is usb0, usb1, ... as listed; with none, the first one.\n"
            "  `format` needs --yes spelled out, because there is no undo and\n"
            "  the disk it is pointed at is somebody's.\n");
}

static blkdev_t *by_name(const char *name) {
    for (int i = 0; i < blk_count(); i++) {
        blkdev_t *d = blk_get(i);
        if (strcmp(d->name, name) == 0) return d;
    }
    return 0;
}

static int is_usb(const blkdev_t *d) {
    return d->name[0] == 'u' && d->name[1] == 's' && d->name[2] == 'b';
}

/* Input first, because it is the half people come here to check. A machine
 * whose keyboard does nothing is a machine whose owner wants to know whether
 * the kernel ever found a keyboard, and until this line existed the only place
 * that said so was a boot log that had already scrolled away. */
static void list_input(void) {
    int kb = 0, ms = 0;
    xhci_hid_counts(&kb, &ms);
    if (!kb && !ms) {
        kprintf("input: no USB keyboard or mouse\n");
        return;
    }
    kprintf("input: %d USB keyboard%s, %d USB mouse%s (%s)\n",
            kb, kb == 1 ? "" : "s", ms, ms == 1 ? "" : "s",
            xhci_irq_driven() ? "interrupt-driven" : "polled");
    struct xhci_hid h;
    for (int i = 0; xhci_hid_get(i, &h); i++)
        kprintf("  slot %-2u %-8s DCI %-2u %u-byte reports, %lu delivered\n",
                h.slot, h.proto == 2 ? "mouse" : "keyboard", h.dci, h.rptlen,
                (unsigned long)h.reports);
}

static void list(void) {
    list_input();
    int n = usbstor_count();
    if (!n) {
        kprintf("usb: no USB storage attached\n");
        kprintf("     (the controller enumerates at boot; a stick plugged in\n"
                "      afterwards is not noticed -- there is no hot-plug yet)\n");
        return;
    }
    for (int i = 0; i < blk_count(); i++) {
        blkdev_t *d = blk_get(i);
        if (!is_usb(d)) continue;
        char sz[12]; sh_human(d->sectors * d->sector_size, sz);
        kprintf("  %-6s %-8s %s\n", d->name, sz, d->model[0] ? d->model : "(disk)");
    }
    if (fat32_mounted())
        kprintf("mounted: FAT32 '%s', %s\n", fat32_label(),
                "one volume at a time -- `usb mount` to switch");
    else
        kprintf("mounted: no FAT32 volume\n");
    if (ext2_mounted())
        kprintf("         ext2 '%s' is also mounted\n", ext2_volume_name());
}

int cmd_usb(int argc, char **argv) {
    if (argc < 2) { list(); return 0; }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) { usage(); return 0; }
    if (strcmp(argv[1], "list") == 0) { list(); return 0; }

    if (strcmp(argv[1], "umount") == 0) {
        if (!fat32_mounted()) { kprintf("usb: nothing mounted\n"); return 1; }
        fat32_flush();
        fat32_unmount();
        kprintf("usb: unmounted\n");
        return 0;
    }

    if (strcmp(argv[1], "mount") == 0) {
        blkdev_t *d = 0;
        if (argc > 2) {
            d = by_name(argv[2]);
            if (!d)      { kprintf("usb: no device '%s'\n", argv[2]); return 1; }
            if (!is_usb(d)) { kprintf("usb: %s is not USB storage\n", argv[2]); return 1; }
        } else {
            for (int i = 0; i < blk_count() && !d; i++)
                if (is_usb(blk_get(i))) d = blk_get(i);
            if (!d) { kprintf("usb: no USB storage attached\n"); return 1; }
        }

        /* Anything already mounted gets its dirty sectors pushed out first.
         * Dropping a FAT volume without flushing loses whatever is sitting in
         * the driver's cache, and the person who finds out is the one whose
         * file is half written. */
        if (fat32_mounted()) { fat32_flush(); fat32_unmount(); }

        if (fat32_mount(d) != 0) {
            kprintf("usb: %s carries no FAT32 volume this driver can read.\n"
                    "     It is NOT being formatted -- that is your disk. If you\n"
                    "     do want it erased: usb format %s --yes\n", d->name, d->name);
            return 1;
        }
        char sz[12]; sh_human(fat32_free_bytes(), sz);
        kprintf("usb: mounted %s as FAT32 '%s', %s free\n", d->name, fat32_label(), sz);
        return 0;
    }

    if (strcmp(argv[1], "format") == 0) {
        if (argc < 3) { usage(); return 1; }
        blkdev_t *d = by_name(argv[2]);
        if (!d)         { kprintf("usb: no device '%s'\n", argv[2]); return 1; }
        if (!is_usb(d)) { kprintf("usb: %s is not USB storage. This command only\n"
                                  "     formats removable disks; use the installer\n"
                                  "     for a fixed one.\n", argv[2]); return 1; }
        /* The confirmation is a word rather than a prompt on purpose: this has
         * to work over SSH and inside a script, and a y/n prompt in either of
         * those is a command that hangs. */
        int yes = 0;
        for (int i = 3; i < argc; i++) if (strcmp(argv[i], "--yes") == 0) yes = 1;
        if (!yes) {
            char sz[12]; sh_human(d->sectors * d->sector_size, sz);
            kprintf("usb: this would erase %s (%s, %s).\n"
                    "     Everything on it goes. Add --yes if you mean it.\n",
                    d->name, sz, d->model[0] ? d->model : "(disk)");
            return 1;
        }
        const char *label = "BOLTUSB";
        for (int i = 3; i < argc; i++) if (strcmp(argv[i], "--yes") != 0) label = argv[i];

        /* The formatter refuses anything that cannot be a *legal* FAT32 -- a
         * volume under 65525 clusters is FAT16 by definition, and a strict
         * reader (UEFI firmware, Windows) rejects it however it is labelled.
         * Saying that here rather than letting fat32_format return -1 is the
         * difference between a known limit and an evening. */
        if (d->sectors < 70000) {
            kprintf("usb: %s is too small for FAT32. A legal FAT32 volume needs\n"
                    "     more than 65525 clusters -- about 35 MB at the smallest\n"
                    "     cluster size -- and this disk has %lu sectors. Anything\n"
                    "     smaller is FAT16 whatever it is labelled, and a strict\n"
                    "     reader will refuse it.\n",
                    d->name, (unsigned long)d->sectors);
            return 1;
        }
        if (fat32_mounted()) { fat32_flush(); fat32_unmount(); }
        if (fat32_format(d, label) != 0) {
            kprintf("usb: could not write a FAT32 volume to %s\n", d->name);
            return 1;
        }
        /* fat32_format() leaves the volume mounted already; this mounts it a
         * second time on purpose, because a volume that cannot be read back is
         * the failure worth catching here and the only way to catch it is to
         * try. The duplicate line in the log is the check working. */
        if (fat32_mount(d) != 0) {
            kprintf("usb: formatted %s, but it will not mount back. That is a\n"
                    "     bug in the formatter, not in your disk.\n", d->name);
            return 1;
        }
        char sz[12]; sh_human(fat32_free_bytes(), sz);
        kprintf("usb: formatted %s as FAT32 '%s', %s free, and mounted it\n",
                d->name, fat32_label(), sz);
        return 0;
    }

    usage();
    return 1;
}
