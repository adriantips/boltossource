#include <stdint.h>
#include "commands.h"
#include "shell.h"
#include "proc.h"
#include "pit.h"
#include "kprintf.h"
#include "string.h"
#include "fat32.h"
#include "acpi.h"
#include "apic.h"
#include "smp.h"
#include "hpet.h"
#include "audio.h"
#include "dhcp.h"
#include "net.h"
#include "clipboard.h"
#include "fs.h"
#include "ext2.h"
#include "users.h"
#include "gui.h"
#include "framebuffer.h"

/* ===========================================================================
 *  Hardware / platform shell commands: FAT32 volume, power, SMP, ACPI, audio.
 * ===========================================================================*/

int cmd_fat(int argc, char **argv) {
    if (!fat32_mounted()) { kprintf("fat: no FAT32 volume mounted\n"); return 1; }
    const char *sub = argc > 1 ? argv[1] : "ls";

    if (strcmp(sub, "ls") == 0) {
        const char *path = argc > 2 ? argv[2] : "/";
        fat_dirent ents[64];
        int n = fat32_list(path, ents, 64);
        if (n < 0) { kprintf("fat: cannot list %s\n", path); return 1; }
        kprintf("Volume '%s'  %s\n", fat32_label(), path);
        for (int i = 0; i < n; i++) {
            if (ents[i].attr & FAT_ATTR_DIR)
                kprintf("  <DIR>  %s\n", ents[i].name);
            else
                kprintf("  %6u  %s\n", ents[i].size, ents[i].name);
        }
        char human[12]; sh_human(fat32_free_bytes(), human);
        kprintf("%d entries, %s free\n", n, human);
        return 0;
    }
    if (strcmp(sub, "cat") == 0 && argc > 2) {
        static char buf[16384];
        int n = fat32_read(argv[2], buf, sizeof buf - 1);
        if (n < 0) { kprintf("fat: cannot read %s\n", argv[2]); return 1; }
        buf[n] = 0;
        kprintf("%s\n", buf);
        return 0;
    }
    if (strcmp(sub, "write") == 0 && argc > 3) {
        if (fat32_write(argv[2], argv[3], (uint32_t)strlen(argv[3])) != 0) {
            kprintf("fat: write failed\n"); return 1;
        }
        kprintf("wrote %s (%u bytes)\n", argv[2], (unsigned)strlen(argv[3]));
        return 0;
    }
    if (strcmp(sub, "mkdir") == 0 && argc > 2) {
        if (fat32_mkdir(argv[2]) != 0) { kprintf("fat: mkdir failed\n"); return 1; }
        kprintf("created %s\n", argv[2]);
        return 0;
    }
    kprintf("usage: fat ls [path] | cat FILE | write FILE TEXT | mkdir DIR\n");
    return 1;
}

/* `power` predates poweroff/reboot and stays as an alias for them. It used to
 * jump straight at ACPI without flushing, which meant every shutdown through
 * this path left the filesystem dirty and cost the next boot a full fsck. */
int cmd_power(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "";
    if (strcmp(sub, "off") == 0 || strcmp(sub, "shutdown") == 0)
        return cmd_poweroff(argc, argv);
    if (strcmp(sub, "reboot") == 0 || strcmp(sub, "restart") == 0)
        return cmd_reboot(argc, argv);
    kprintf("usage: power off | reboot   (same as the poweroff / reboot commands)\n");
    return 1;
}

int cmd_cpus(int argc, char **argv) {
    (void)argc; (void)argv;
    const struct acpi_info *ai = acpi_get();
    kprintf("Processors (ACPI MADT): %d\n", ai->cpu_count);
    for (int i = 0; i < ai->cpu_count; i++)
        kprintf("  CPU%d  LAPIC id %u%s\n", i, ai->cpu_lapic_id[i],
                ai->cpu_lapic_id[i] == lapic_id() ? "  (BSP)" : "");
    kprintf("Online: %d  |  interrupt mode: %s\n",
            smp_cpu_count(), apic_active() ? "APIC" : "PIC");
    return 0;
}

int cmd_acpiinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    const struct acpi_info *ai = acpi_get();
    if (!ai->present) { kprintf("ACPI: not present\n"); return 1; }
    kprintf("ACPI revision %d\n", ai->revision);
    kprintf("  Local APIC base : 0x%lx\n", ai->lapic_addr);
    kprintf("  IO-APICs        : %d\n", ai->ioapic_count);
    for (int i = 0; i < ai->ioapic_count; i++)
        kprintf("    IOAPIC%d @0x%x gsi-base %u\n", i, ai->ioapic[i].address, ai->ioapic[i].gsi_base);
    kprintf("  HPET            : %s", ai->hpet_addr ? "" : "absent\n");
    if (ai->hpet_addr) kprintf("@0x%lx (%lu us uptime)\n", ai->hpet_addr, hpet_us());
    kprintf("  Override entries: %d\n", ai->iso_count);
    return 0;
}

int cmd_dhcp(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("Requesting DHCP lease...\n");
    if (dhcp_configure(3000) != 0) { kprintf("dhcp: no response (static config kept)\n"); return 1; }
    return 0;
}

int cmd_play(int argc, char **argv) {
    if (!audio_present()) { kprintf("play: no audio device\n"); return 1; }
    int freq = argc > 1 ? atoi(argv[1]) : 440;
    int ms   = argc > 2 ? atoi(argv[2]) : 400;
    kprintf("Playing %d Hz for %d ms on %s\n", freq, ms, audio_name());
    /* Report how long it really took. On a PCM codec a tone lasts as long as the
     * DMA engine takes to consume its samples, so an elapsed time that tracks
     * the length asked for is evidence that audio actually left the machine --
     * rather than that a handful of registers accepted a write. Timed off the
     * HPET, which is read on demand: the PIT can only report ticks it managed
     * to deliver, and this is measured across a tight polling loop. */
    uint64_t t0 = hpet_present() ? hpet_us() : pit_ticks() * 1000;
    audio_tone((uint32_t)freq, (uint32_t)ms);
    uint64_t us = (hpet_present() ? hpet_us() : pit_ticks() * 1000) - t0;
    kprintf("play: %lu ms elapsed on %s\n", us / 1000, audio_name());
    return 0;
}

/* clip: inspect or load the shared system clipboard from the shell, bridging
 * the CLI with the GUI apps that use Ctrl+C / Ctrl+V. */
int cmd_clip(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "show";
    if (strcmp(sub, "show") == 0) {
        /* The clipboard is typed now, so say which kind it holds. "clipboard
         * is empty" for a picture would be a lie, and a paste that then
         * produced an image would be a surprise. */
        if (clip_kind() == CLIP_EMPTY) { kprintf("clipboard is empty\n"); return 0; }
        if (clip_kind() == CLIP_IMAGE) {
            int w = 0, h = 0;
            clip_image(&w, &h);
            kprintf("clipboard: image %dx%d (%d KiB)\n", w, h, (w * h * 4) / 1024);
            return 0;
        }
        if (clip_kind() == CLIP_FILES) {
            int n = clip_file_count();
            kprintf("clipboard: %d file%s\n", n, n == 1 ? "" : "s");
            for (int i = 0; i < n; i++) {
                char p[FS_PATH_MAX];
                if (clip_file_at(i, p, sizeof p)) kprintf("  %s\n", p);
            }
            return 0;
        }
        kprintf("clipboard: text, %d bytes\n%s\n", clip_len(), clip_get());
        return 0;
    }
    if (strcmp(sub, "clear") == 0) { clip_clear(); kprintf("clipboard cleared\n"); return 0; }
    if (strcmp(sub, "set") == 0 && argc > 2) {
        /* join the remaining args with spaces */
        static char buf[CLIP_CAP]; int n = 0;
        for (int i = 2; i < argc && n < CLIP_CAP - 1; i++) {
            if (i > 2 && n < CLIP_CAP - 1) buf[n++] = ' ';
            for (const char *p = argv[i]; *p && n < CLIP_CAP - 1; p++) buf[n++] = *p;
        }
        buf[n] = 0; clip_set(buf, n);
        kprintf("copied %d bytes to clipboard\n", n);
        return 0;
    }
    kprintf("usage: clip show | set TEXT... | clear\n");
    return 1;
}

/* ext2: read files/dirs off a mounted ext2 volume (e.g. a Linux USB stick). */
int cmd_ext2(int argc, char **argv) {
    if (!ext2_mounted()) { kprintf("ext2: no ext2 volume mounted\n"); return 1; }
    const char *sub = argc > 1 ? argv[1] : "ls";
    if (strcmp(sub, "ls") == 0) {
        const char *path = argc > 2 ? argv[2] : "/";
        ext2_dirent ents[128];
        int n = ext2_list(path, ents, 128);
        if (n < 0) { kprintf("ext2: cannot list %s\n", path); return 1; }
        kprintf("ext2 volume '%s'  %s\n", ext2_volume_name(), path);
        for (int i = 0; i < n; i++) {
            if (ents[i].name[0] == '.' && (ents[i].name[1] == 0 ||
                (ents[i].name[1] == '.' && ents[i].name[2] == 0))) continue;
            if (ents[i].is_dir) kprintf("  <DIR>  %s\n", ents[i].name);
            else                kprintf("  %7u  %s\n", ents[i].size, ents[i].name);
        }
        char human[12]; sh_human(ext2_free_bytes(), human);
        kprintf("%d entries, %s free\n", n, human);
        return 0;
    }
    if (strcmp(sub, "cat") == 0 && argc > 2) {
        static char buf[65536];
        int n = ext2_read(argv[2], buf, sizeof buf - 1);
        if (n < 0) { kprintf("ext2: cannot read %s\n", argv[2]); return 1; }
        buf[n] = 0;
        kprintf("%s\n", buf);
        return 0;
    }
    kprintf("usage: ext2 ls [path] | cat FILE\n");
    return 1;
}

/* whoami / users / logout: multi-user account management. */
/* whoami / users and the rest of the identity commands live in
 * kernel/cmd_user.c, alongside sudo, passwd, chmod and chown. */

int cmd_logout(int argc, char **argv) {
    sh_history_save();          /* the session ends here too */
    (void)argc; (void)argv;
    if (!fb_present()) { kprintf("logout: only available in the desktop session\n"); return 1; }
    kprintf("logging out...\n");
    gui_logout();
    return 0;
}

/* crash: deliberately trigger a CPU exception to exercise the panic/crash-dump
 * path (register grid + page-fault decode + backtrace). Debugging aid. */
int cmd_crash(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "";

    /* ring 3: the process dies, the machine does not. This is the demo for
     * per-process fault isolation -- run it and watch the shell come straight
     * back with the desktop and every other program still running. */
    if (strcmp(sub, "ring3") == 0 || strcmp(sub, "user") == 0) {
        const char *mode = argc > 2 ? argv[2] : "null";
        kprintf("running /bin/crashme %s in ring 3; the kernel should reap it\n", mode);
        if (proc_exec_argv("/bin/crashme", mode) < 0) {
            kprintf("crash: could not exec /bin/crashme\n");
            return 1;
        }
        /* Let the scheduler run it and the fault handler finish reaping. */
        uint64_t until = pit_ticks() + 400;
        while (pit_ticks() < until) __asm__ volatile("hlt");
        return 0;
    }

    /* ring 0: a genuine kernel panic. Halts the machine, after saving a crash
     * report to /var/log/crash.log for the next boot to show. */
    if (strcmp(sub, "null") == 0) { volatile int *p = (int *)0; *p = 1; }
    else if (strcmp(sub, "div") == 0) { volatile int z = 0; volatile int x = 1 / z; (void)x; }
    else if (strcmp(sub, "ud")  == 0) { __asm__ volatile("ud2"); }
    else {
        kprintf("usage: crash ring3 [null|div|ud|stack]   fault a ring-3 process\n");
        kprintf("       crash null | div | ud             panic the kernel\n");
    }
    return 0;
}
