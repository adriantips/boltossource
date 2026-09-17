#include <stdint.h>
#include "commands.h"
#include "shell.h"
#include "proc.h"
#include "pit.h"
#include "kprintf.h"
#include "power.h"
#include "backlight.h"
#include "string.h"
#include "fat32.h"
#include "acpi.h"
#include "apic.h"
#include "smp.h"
#include "hpet.h"
#include "audio.h"
#include "mixer.h"
#include "wav.h"
#include "kheap.h"
#include "dhcp.h"
#include "net.h"
#include "clipboard.h"
#include "fs.h"
#include "ext2.h"
#include "users.h"
#include "gui.h"
#include "sleep.h"
#include "fs.h"
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
/* What the machine can say about its power source, which is not much and says
 * so. See include/power.h: a battery is an AML control method and there is no
 * AML interpreter here, so the honest answers are "on AC", "this is a laptop
 * and I cannot read its battery", and whatever a real driver reports once one
 * exists. */
static void power_status(void) {
    kprintf("power: %s\n", power_summary());
    kprintf("       firmware profile: %s\n",
            power_is_mobile() ? "portable" : "not portable");
    kprintf("       brightness %d%% via the %s\n", backlight_get(), backlight_name());
    if (sleep_s3_ok())
        kprintf("       suspend to RAM (S3): available\n");
    else
        kprintf("       suspend to RAM (S3): no -- %s\n", sleep_why());
    if (sleep_count())
        kprintf("       resumed from S3 %u time%s this boot\n",
                (unsigned)sleep_count(), sleep_count() == 1 ? "" : "s");
}

/* `suspend` -- stop, keep the RAM, come back where you were.
 *
 * The commit before sleeping is not belt-and-braces. A suspended machine is
 * one power cut away from a machine that never woke, and the filesystem tree
 * lives in RAM: an S3 that loses power is exactly as lossy as pulling the plug
 * on a running machine, so the disk is brought up to date first, the same way
 * `poweroff` does it. What is different from poweroff is that the filesystem
 * is NOT marked clean -- the machine is still, as far as the next boot is
 * concerned, running. */
int cmd_suspend(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!sleep_s3_ok()) {
        kprintf("suspend: this machine cannot suspend to RAM.\n"
                "         %s\n", sleep_why());
        return 1;
    }
    kprintf("suspend: committing the filesystem first -- a sleeping machine\n"
            "         that loses power has lost whatever was only in RAM.\n");
    if (fs_persist_active() && !fs_persist_readonly() && fs_sync() != 0)
        kprintf("suspend: WARNING - the commit failed; changes may be only in RAM.\n");
    fat32_flush();
    kprintf("suspend: going to sleep. Wake it with a key, the power button,\n"
            "         or whatever your firmware calls a wake event.\n");

    if (sleep_suspend() != 0) {
        kprintf("suspend: did not sleep. %s\n", sleep_why());
        return 1;
    }
    kprintf("suspend: awake again.\n");
    return 0;
}

int cmd_power(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "";
    if (strcmp(sub, "off") == 0 || strcmp(sub, "shutdown") == 0)
        return cmd_poweroff(argc, argv);
    if (strcmp(sub, "reboot") == 0 || strcmp(sub, "restart") == 0)
        return cmd_reboot(argc, argv);
    if (strcmp(sub, "status") == 0) { power_status(); return 0; }
    if (strcmp(sub, "suspend") == 0 || strcmp(sub, "sleep") == 0)
        return cmd_suspend(argc, argv);
    power_status();
    kprintf("usage: power status | off | reboot | suspend\n");
    return argc > 1 ? 1 : 0;
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

/* Stream a WAV file to the codec, converting as it goes. One chunk is decoded,
 * handed to the device and waited on before the next is decoded, so a long track
 * never needs to exist twice: once as the file, once as 48 kHz stereo. */
static int play_file(const char *path) {
    wav_t w;
    uint8_t *file = 0;
    const char *err = "unknown error";
    if (wav_load(path, &w, &file, &err) != 0) {
        kprintf("play: %s: %s\n", path, err);
        return 1;
    }
    kprintf("%s: %u Hz, %u ch, %u-bit, %u.%02u s -> %u Hz stereo on %s\n",
            path, w.rate, w.channels, w.bits,
            wav_duration_ms(&w) / 1000, (wav_duration_ms(&w) % 1000) / 10,
            AUDIO_RATE, audio_name());

    if (!audio_pcm_ok()) {
        kprintf("play: %s has no PCM device to play through\n", audio_name());
        kfree(file);
        return 1;
    }

    uint32_t chunk = AUDIO_RATE / 4;                  /* a quarter second */
    int16_t *pcm = (int16_t *)kmalloc((uint64_t)chunk * 4);
    if (!pcm) { kprintf("play: out of memory\n"); kfree(file); return 1; }

    uint64_t t0 = hpet_present() ? hpet_us() : pit_ticks() * 1000;
    for (;;) {
        uint32_t got = wav_read(&w, pcm, chunk);
        if (!got) break;
        audio_play_pcm_ch(MIX_MEDIA, pcm, got);
    }
    uint64_t us = (hpet_present() ? hpet_us() : pit_ticks() * 1000) - t0;
    kprintf("play: %lu ms elapsed on %s\n", us / 1000, audio_name());

    kfree(pcm);
    kfree(file);
    return 0;
}

int cmd_play(int argc, char **argv) {
    if (!audio_present()) { kprintf("play: no audio device\n"); return 1; }
    /* `play FILE` or `play [HZ] [MS]`. A first argument that is not a number is
     * a path -- which does mean a file called "440" would be heard as a tone,
     * and that is the trade for not having two commands for one idea. */
    if (argc > 1 && (argv[1][0] < '0' || argv[1][0] > '9'))
        return play_file(argv[1]);
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

    /* The history is a desktop feature with a window, so it needs a prompt
     * front end too: over SSH there is no Win+V to press, and a test that
     * drives the ring by clicking rows is mostly testing the compositor. */
    if (strcmp(sub, "history") == 0 || strcmp(sub, "hist") == 0) {
        if (argc > 2 && strcmp(argv[2], "clear") == 0) {
            clip_hist_clear();
            kprintf("history cleared (%d pinned kept)\n", clip_hist_count());
            return 0;
        }
        int n = clip_hist_count();
        if (!n) { kprintf("clipboard history is empty\n"); return 0; }
        kprintf("clipboard history: %d of %d\n", n, CLIP_HIST);
        for (int i = 0; i < n; i++) {
            int k = clip_hist_kind(i);
            char mark = clip_hist_pinned(i) ? '*' : ' ';
            if (k == CLIP_IMAGE) {
                int w = 0, h = 0;
                clip_hist_image(i, &w, &h);
                kprintf(" %c%2d image  %dx%d\n", mark, i, w, h);
                continue;
            }
            /* One line, truncated: an entry is frequently a whole file and
             * printing it would push the rest of the list off the screen. */
            const char *t = clip_hist_text(i);
            char line[61];
            int j = 0;
            for (; t[j] && j < (int)sizeof(line) - 1; j++) {
                if (t[j] == '\n' || t[j] == '\r' || t[j] == '\t') { line[j] = ' '; continue; }
                line[j] = t[j];
            }
            line[j] = 0;
            kprintf(" %c%2d %-6s %s%s\n", mark, i, k == CLIP_FILES ? "files" : "text",
                    line, clip_hist_len(i) > j ? "..." : "");
        }
        return 0;
    }
    if (strcmp(sub, "restore") == 0 && argc > 2) {
        int i = atoi(argv[2]);
        if (clip_hist_restore(i) != 0) { kprintf("clip: no history entry %d\n", i); return 1; }
        kprintf("clipboard: %s\n", clip_kind_name());
        return 0;
    }
    if (strcmp(sub, "pin") == 0 && argc > 2) {
        int i = atoi(argv[2]);
        if (i < 0 || i >= clip_hist_count()) { kprintf("clip: no history entry %d\n", i); return 1; }
        clip_hist_pin(i, !clip_hist_pinned(i));
        kprintf("entry %d %s\n", i, clip_hist_pinned(i) ? "pinned" : "unpinned");
        return 0;
    }
    if (strcmp(sub, "forget") == 0 && argc > 2) {
        int i = atoi(argv[2]);
        if (i < 0 || i >= clip_hist_count()) { kprintf("clip: no history entry %d\n", i); return 1; }
        clip_hist_remove(i);
        kprintf("entry %d dropped\n", i);
        return 0;
    }
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
    kprintf("usage: clip show | set TEXT... | clear\n"
            "       clip history [clear] | restore N | pin N | forget N\n");
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
