#include <stdint.h>
#include "commands.h"
#include "shell.h"
#include "hw.h"
#include "fs.h"
#include "fat32.h"
#include "acpi.h"
#include "install.h"
#include "klog.h"
#include "blk.h"
#include "pmm.h"
#include "kheap.h"
#include "pit.h"
#include "kprintf.h"
#include "string.h"
#include "framebuffer.h"
#include "ata.h"

#define BOLT_VERSION "BoltOS 0.2"

static void p2(uint8_t v) { kprintf("%s%u", v < 10 ? "0" : "", (unsigned)v); }

static void print_datetime(void) {
    struct rtc_time t; rtc_now(&t);
    kprintf("%u-", (unsigned)t.year); p2(t.mon); kprintf("-"); p2(t.day);
    kprintf(" "); p2(t.hour); kprintf(":"); p2(t.min); kprintf(":"); p2(t.sec);
}

static void print_uptime(void) {
    uint64_t ms = sh_uptime_ms();
    uint64_t s = ms / 1000;
    uint64_t d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60, ss = s % 60;
    if (d) kprintf("%lud ", (unsigned long)d);
    kprintf("%luh %lum %lus", (unsigned long)h, (unsigned long)m, (unsigned long)ss);
}

/* ------------------------------ cpuinfo ---------------------------------- */
int cmd_cpuinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    char vendor[13], brand[49];
    hw_cpu_vendor(vendor);
    hw_cpu_brand(brand);
    uint32_t a, b, c, d;
    cpuidx(1, 0, &a, &b, &c, &d);

    uint32_t stepping = a & 0xF;
    uint32_t model    = (a >> 4) & 0xF;
    uint32_t family   = (a >> 8) & 0xF;
    if (family == 0xF) family += (a >> 20) & 0xFF;
    if (family == 0x6 || family == 0xF) model += ((a >> 16) & 0xF) << 4;

    kprintf("vendor   : %s\n", vendor);
    kprintf("brand    : %s\n", brand[0] ? brand : "(unknown)");
    kprintf("family   : %u  model %u  stepping %u\n",
            (unsigned)family, (unsigned)model, (unsigned)stepping);
    kprintf("max leaf : 0x%x\n", hw_cpu_max_leaf());
    kprintf("features :");
    if (d & (1u << 0))  kprintf(" fpu");
    if (d & (1u << 5))  kprintf(" msr");
    if (d & (1u << 6))  kprintf(" pae");
    if (d & (1u << 9))  kprintf(" apic");
    if (d & (1u << 23)) kprintf(" mmx");
    if (d & (1u << 25)) kprintf(" sse");
    if (d & (1u << 26)) kprintf(" sse2");
    if (c & (1u << 0))  kprintf(" sse3");
    if (c & (1u << 9))  kprintf(" ssse3");
    if (c & (1u << 19)) kprintf(" sse4.1");
    if (c & (1u << 20)) kprintf(" sse4.2");
    if (c & (1u << 28)) kprintf(" avx");
    if (c & (1u << 31)) kprintf(" hypervisor");
    kprintf("\n");
    return 0;
}

/* ------------------------------ meminfo ---------------------------------- */
int cmd_meminfo(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t freep = pmm_free_count(), totp = pmm_total_count();
    uint64_t usedp = totp - freep;
    kprintf("physical : total %lu MiB  used %lu MiB  free %lu MiB\n",
            (unsigned long)((totp * 4096ull) >> 20),
            (unsigned long)((usedp * 4096ull) >> 20),
            (unsigned long)((freep * 4096ull) >> 20));
    kprintf("frames   : %lu / %lu free  (4 KiB each)\n",
            (unsigned long)freep, (unsigned long)totp);
    kheap_stats h;
    kheap_get_stats(&h);
    kprintf("kheap    : %lu KiB used, %lu KiB free of %lu KiB\n",
            (unsigned long)(h.used >> 10), (unsigned long)(h.free_bytes >> 10),
            (unsigned long)(h.total >> 10));
    /* The largest free block, not just the total, because that is the number
     * that decides whether the next big allocation succeeds. */
    kprintf("           %lu blocks (%lu free), largest free %lu KiB\n",
            (unsigned long)h.blocks, (unsigned long)h.free_blocks,
            (unsigned long)(h.largest_free >> 10));
    if (h.failures || h.bad_frees)
        kprintf("           %lu failed allocation%s, %lu bad free%s\n",
                (unsigned long)h.failures, h.failures == 1 ? "" : "s",
                (unsigned long)h.bad_frees, h.bad_frees == 1 ? "" : "s");
    /* Walking the heap's own structure costs one pass over the block list,
     * which is worth paying when somebody has asked how memory is doing. */
    int hc = kheap_check();
    if (hc == 0) kprintf("           structure ok\n");
    else         kprintf("           structure INCONSISTENT (%d)\n", hc);
    char sz[12]; sh_human(fs_total_bytes(), sz);
    kprintf("ramfs    : %d nodes, %s in files\n", fs_count_nodes(), sz);
    return 0;
}
int cmd_mem(int argc, char **argv) { return cmd_meminfo(argc, argv); }

/* ------------------------------ uptime ----------------------------------- */
int cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("up "); print_uptime();
    kprintf("   (%lu ticks @ %u Hz)\n",
            (unsigned long)pit_ticks(), (unsigned)pit_hz());
    return 0;
}

/* ------------------------------ version ---------------------------------- */
int cmd_version(int argc, char **argv) {
    (void)argc; (void)argv;
    char brand[49]; hw_cpu_brand(brand);
    kprintf("%s  (64-bit long mode)\n", BOLT_VERSION);
    kprintf("arch     : x86-64\n");
    kprintf("cpu      : %s\n", brand[0] ? brand : "x86-64");
    kprintf("clock    : "); print_datetime(); kprintf("\n");
    return 0;
}

/* ------------------------------ devices ---------------------------------- */
int cmd_devices(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("platform devices:\n");
    kprintf("  i8253  PIT        timer       @0x40\n");
    kprintf("  i8259  PIC        irq ctrl    @0x20/0xA0\n");
    kprintf("  i8042  PS/2 kbd   input       @0x60\n");
    kprintf("  16550  COM1       serial      @0x3F8\n");
    kprintf("  CMOS   RTC        clock       @0x70\n");
    if (fb_present())
        kprintf("  VBE    framebuffer %ux%u 32bpp\n", fb_width(), fb_height());

    struct pci_dev pds[32];
    int n = pci_scan(pds, 32);
    kprintf("PCI devices: %d\n", n);
    for (int i = 0; i < n; i++) {
        struct pci_dev *p = &pds[i];
        kprintf("  %x:%x.%x  %x:%x  ",
                p->bus, p->slot, p->func, p->vendor, p->device);
        sh_pad(pci_class_name(p->class), 14);
        kprintf("\n");
    }
    return 0;
}

/* ------------------------------ diskinfo --------------------------------- */
int cmd_diskinfo(int argc, char **argv) {
    (void)argc; (void)argv;

    struct pci_dev pds[32];
    int n = pci_scan(pds, 32), found = 0;
    for (int i = 0; i < n; i++) {
        if (pds[i].class == 0x01) {
            kprintf("ctrl   : PCI %x:%x.%x  storage  %x:%x\n",
                    pds[i].bus, pds[i].slot, pds[i].func, pds[i].vendor, pds[i].device);
            found = 1;
        }
    }
    if (!found) kprintf("ctrl   : (no PCI storage controller reported)\n");

    int dn = blk_count();
    kprintf("block devices: %d\n", dn);
    for (int i = 0; i < dn; i++) {
        blkdev_t *d = blk_get(i);
        char sz[12]; sh_human(d->sectors * d->sector_size, sz);
        kprintf("  [%d] %-8s %s  %s  %s%s\n", i, d->name, blk_media(d), sz,
                d->is_boot ? "boot " : "",
                d->model[0] ? d->model : "(disk)");
    }

    char sz[12]; sh_human(fs_total_bytes(), sz);
    if (fs_persist_active())
        kprintf("mounted: BoltFS on %s '%s'  %d nodes  %s (%s)\n",
                fs_persist_media(), fs_persist_model(), fs_count_nodes(), sz,
                fs_persist_readonly() ? "disk READ-ONLY; changes are RAM-only" : "persistent");
    else
        kprintf("mounted: ramfs (in-RAM, volatile)  %d nodes  %s\n",
                fs_count_nodes(), sz);
    return 0;
}

/* ------------------------------ sync ------------------------------------- */
int cmd_sync(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!fs_persist_active()) { kprintf("sync: no persistent disk; FS is RAM-only\n"); return 1; }
    if (fs_persist_readonly()) { kprintf("sync: disk protected read-only (recovery or storage failure)\n"); return 1; }
    int rc = fs_sync();
    kprintf(rc == 0 ? "sync: committed FS to %s '%s' (generation %lu)\n" : "sync: FAILED\n",
            fs_persist_media(), fs_persist_model(), (unsigned long)fs_generation());
    return rc == 0 ? 0 : 1;
}

/* ------------------------------- fsck ------------------------------------ */
int cmd_fsck(int argc, char **argv) {
    int repair = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--repair") == 0) repair = 1;

    if (!fs_persist_active()) {
        kprintf("fsck: no persistent disk; the FS is RAM-only and always consistent\n");
        return 0;
    }
    if (repair && fs_persist_readonly()) {
        kprintf("fsck: cannot repair a protected read-only disk; original contents preserved\n");
        return 1;
    }

    fs_check_report r;
    int rc = fs_check(repair, &r);

    kprintf("BoltFS on %s '%s'\n", fs_persist_media(), fs_persist_model());
    kprintf("  superblocks : %d of 2 valid\n", r.sb_ok);
    kprintf("  generation  : %lu   slot %u   mount #%lu\n",
            (unsigned long)r.generation, r.slot, (unsigned long)r.mounts);
    kprintf("  shutdown    : %s\n", r.clean ? "clean" : "UNCLEAN (crash or power loss)");
    kprintf("  checksum    : %s\n", r.checksum_ok ? "ok" : "FAILED");
    kprintf("  structure   : %s\n", r.structure_ok ? "ok" : "FAILED");
    kprintf("  contents    : %d dirs, %d files, %lu bytes in %lu image bytes\n",
            r.dirs, r.files, (unsigned long)r.bytes, (unsigned long)r.blob_bytes);

    if (r.nissue == 0) kprintf("  no issues found.\n");
    for (int i = 0; i < r.nissue && i < FS_CHECK_MAX_ISSUES; i++)
        kprintf("  issue: %s\n", r.issue[i]);
    if (r.nissue > FS_CHECK_MAX_ISSUES)
        kprintf("  ... and %d more\n", r.nissue - FS_CHECK_MAX_ISSUES);

    if (r.repaired) kprintf("  repaired: rewrote both superblocks and marked the image clean.\n");
    else if (r.nissue && !repair && !fs_persist_readonly())
        kprintf("  run 'fsck -r' to rewrite the image from the live tree.\n");

    return rc < 0 ? 2 : (rc > 0 ? 1 : 0);
}

/* --------------------------- shutdown / reboot ---------------------------- *
 *  The one path a user is guaranteed to take every single day, so it is the
 *  one that must never lose a file. Both commands commit the filesystem and
 *  mark the image clean BEFORE touching ACPI -- an image left dirty is what
 *  makes the next boot run a full fsck.
 * -------------------------------------------------------------------------- */
static void shutdown_prepare(const char *what) {
    /* The shell keeps its history in memory and writes it here, so a day's
     * commands survive the reboot without paying a disk commit per command. */
    sh_history_save();
    /* And the kernel log, before the final commit so it lands in the image.
     * klog_persist() has existed for as long as the ring has, and the only
     * thing that ever called it was `dmesg save` -- so the log survived a
     * reboot exactly when somebody had thought to ask for it in advance, which
     * is not when anybody needs it. A clean shutdown writes it now, and `logs`
     * reads it back on the next boot. */
    klog_persist();
    kprintf("%s: flushing filesystems...\n", what);
    if (fs_persist_active() && !fs_persist_readonly()) {
        if (fs_shutdown() == 0)
            kprintf("%s: BoltFS committed and marked clean (generation %lu)\n",
                    what, (unsigned long)fs_generation());
        else
            kprintf("%s: WARNING - the final commit failed; recent changes may be lost\n", what);
    }
    fat32_flush();
}

int cmd_poweroff(int argc, char **argv) {
    (void)argc; (void)argv;
    shutdown_prepare("poweroff");
    kprintf("poweroff: powering off via ACPI (S5).\n");
    acpi_poweroff();
    return 0;
}

int cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    shutdown_prepare("reboot");
    kprintf("reboot: restarting.\n");
    acpi_reboot();
    return 0;
}

/* ------------------------------ battery ---------------------------------- */
int cmd_battery(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("no battery detected.\n");
    kprintf("ACPI power management is not implemented; on a desktop/VM this is\n");
    kprintf("expected (AC power, no smart battery).\n");
    return 0;
}

/* ------------------------------ sensors ---------------------------------- */
int cmd_sensors(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("no hardware sensors available.\n");
    kprintf("thermal/voltage/fan readouts need ACPI or MSR/SMBus drivers,\n");
    kprintf("which are not implemented.\n");
    kprintf("live counters:\n");
    kprintf("  pit ticks : %lu @ %u Hz\n", (unsigned long)pit_ticks(), (unsigned)pit_hz());
    kprintf("  rtc clock : "); print_datetime(); kprintf("\n");
    return 0;
}

/* ------------------------------ sysinfo ---------------------------------- */
int cmd_sysinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    char brand[49]; hw_cpu_brand(brand);
    uint64_t freep = pmm_free_count(), totp = pmm_total_count();
    struct pci_dev pds[32];
    int npci = pci_scan(pds, 32);

    kprintf("=============== BoltOS system summary ===============\n");
    kprintf(" os      : %s (x86-64 long mode)\n", BOLT_VERSION);
    kprintf(" cpu     : %s\n", brand[0] ? brand : "x86-64");
    kprintf(" memory  : %lu MiB total, %lu MiB free\n",
            (unsigned long)((totp * 4096ull) >> 20),
            (unsigned long)((freep * 4096ull) >> 20));
    if (fb_present()) kprintf(" display : %ux%u 32bpp framebuffer\n", fb_width(), fb_height());
    else              kprintf(" display : serial only\n");
    kprintf(" pci     : %d devices\n", npci);
    kprintf(" ramfs   : %d nodes\n", fs_count_nodes());
    kprintf(" uptime  : "); print_uptime(); kprintf("\n");
    kprintf(" clock   : "); print_datetime(); kprintf("\n");
    kprintf("=====================================================\n");
    return 0;
}

/* ------------------------ health / doctor -------------------------------- */
/* Runs real self-checks against live subsystems. Returns problem count. */
static int run_diagnostics(int suggest) {
    int problems = 0;

    /* heap round-trip */
    void *a = kmalloc(256), *b = kmalloc(4096);
    if (a && b) { kfree(a); kfree(b); kprintf("[ok]   kheap   alloc/free round-trip\n"); }
    else        { kprintf("[FAIL] kheap   allocation returned NULL\n"); problems++;
                  if (suggest) kprintf("       -> heap exhausted; free objects or grow kheap\n"); }

    /* physical memory */
    uint64_t freep = pmm_free_count(), totp = pmm_total_count();
    if (totp && freep) kprintf("[ok]   pmm     %lu/%lu frames free\n",
                               (unsigned long)freep, (unsigned long)totp);
    else { kprintf("[WARN] pmm     no free frames\n"); problems++; }

    /* timer advancing (IRQ0 must be firing) */
    uint64_t t0 = pit_ticks();
    for (volatile uint64_t i = 0; i < 60000000ull && pit_ticks() == t0; i++) { }
    if (pit_ticks() != t0) kprintf("[ok]   pit     timer interrupts firing\n");
    else { kprintf("[FAIL] pit     no timer ticks (IRQ0 stalled?)\n"); problems++;
           if (suggest) kprintf("       -> check PIC mask / sti / IDT vector 0x20\n"); }

    /* filesystem writable */
    fs_node *t = fs_create("/.health", 0);
    if (t && fs_write(t, "ok", 2) == 0) { fs_remove_node(t); kprintf("[ok]   ramfs   create/write/remove\n"); }
    else { kprintf("[FAIL] ramfs   cannot create temp file\n"); problems++; }

    return problems;
}

int cmd_health(int argc, char **argv) {
    (void)argc; (void)argv;
    int p = run_diagnostics(0);
    kprintf(p ? "health: %d problem(s) found\n" : "health: all systems nominal\n", p);
    return p ? 1 : 0;
}

int cmd_doctor(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("BoltDoctor: running diagnostics...\n");
    int p = run_diagnostics(1);
    if (!p) kprintf("diagnosis: healthy. No action needed.\n");
    else    kprintf("diagnosis: %d issue(s). See [FAIL]/[WARN] hints above.\n", p);
    return 0;
}

/* ===========================================================================
 *  install  -  write a bootable BoltOS onto another disk, from inside BoltOS.
 *
 *    install                              list the disks that can be installed to
 *    install DISK                         interactive: asks for the account, confirms
 *    install DISK USER PASS [ESP_MB]      unattended (what the test suite drives)
 * ===========================================================================*/
int cmd_install(int argc, char **argv) {
    if (argc < 2) {
        int n = install_target_count();
        uint64_t kb = install_kernel_available();

        kprintf("BoltOS installer\n\n");
        if (!kb) {
            kprintf("  The kernel image could not be found on the medium this system\n");
            kprintf("  booted from, so there is nothing to install. Boot from the BoltOS\n");
            kprintf("  disk image or an installed BoltOS disk and try again.\n");
            return 1;
        }
        char sz[16]; sh_human(kb, sz);
        kprintf("  kernel image: %s (read back from the boot medium)\n\n", sz);

        if (!n) {
            kprintf("  No installable disks. The boot disk and the disk holding this\n");
            kprintf("  system's filesystem are both excluded; attach another one.\n");
            return 1;
        }
        kprintf("  Installable disks:\n");
        for (int i = 0; i < n; i++) {
            blkdev_t *d = install_target_get(i);
            char cap[16]; sh_human(d->sectors * d->sector_size, cap);
            char plan[128]; install_describe(d->name, plan, sizeof plan);
            kprintf("    %-8s %-5s %-8s %s\n", d->name, blk_media(d), cap,
                    d->model[0] ? d->model : "(disk)");
            kprintf("             -> %s\n", plan);
        }
        kprintf("\n  install DISK                       (asks for an account)\n");
        kprintf("  install DISK USER PASS [ESP_MB]    (unattended)\n");
        return 0;
    }

    install_req req;
    memset(&req, 0, sizeof req);
    strncpy(req.disk, argv[1], sizeof req.disk - 1);

    if (argc >= 4) {
        strncpy(req.username, argv[2], sizeof req.username - 1);
        strncpy(req.password, argv[3], sizeof req.password - 1);
        if (argc >= 5) req.esp_mb = (uint32_t)atoi(argv[4]);
    } else {
        kprintf("Creating the account you will sign in with on the new system.\n");
        kprintf("username: ");
        sh_readline(req.username, sizeof req.username);
        kprintf("password: ");
        sh_readline(req.password, sizeof req.password);
        if (!req.username[0]) { kprintf("install: a username is required\n"); return 1; }

        char plan[128];
        install_describe(req.disk, plan, sizeof plan);
        kprintf("\nTarget %s will be COMPLETELY ERASED and laid out as:\n  %s\n",
                req.disk, plan);
        if (!sh_yesno("Erase it and install")) { kprintf("install: cancelled\n"); return 1; }
    }

    if (install_run(&req) != 0) {
        kprintf("install: %s\n", req.error);
        return 1;
    }
    kprintf("\nInstalled. Shut down, remove the install medium, and boot %s.\n"
            "Sign in as '%s'.\n", req.disk, req.username);
    return 0;
}

/* ============================ dmesg / crashlog ============================ *
 *  dmesg    -- the kernel message ring (kernel/klog.c). Everything kputc has
 *              printed since boot, so the boot log is still readable after the
 *              GUI has taken the screen and the console has scrolled away.
 *  crashlog -- crash reports persisted to /var/log/crash.log. Survives reboots,
 *              which is the whole point: the report you need is the one from
 *              the boot that failed, not the one you are in.
 * ========================================================================== */
int cmd_dmesg(int argc, char **argv) {
    int tail = 0;
    int save = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') tail = atoi(argv[i] + 1);
        else if (strcmp(argv[i], "save") == 0) save = 1;
    }

    if (save) {
        int rc = klog_persist();
        kprintf(rc == 0 ? "dmesg: kernel log appended to %s\n"
                        : "dmesg: could not write %s\n", KERNEL_LOG_PATH);
        return rc == 0 ? 0 : 1;
    }

    uint32_t used = klog_used();
    char *buf = (char *)kmalloc(used + 1);
    if (!buf) { kprintf("dmesg: out of memory\n"); return 1; }
    uint32_t n = klog_snapshot(buf, used);
    buf[n] = 0;

    uint32_t from = 0;
    if (tail > 0) {                       /* -N: only the last N lines */
        int want = tail;
        for (uint32_t i = n; i > 0; i--) {
            if (buf[i - 1] != '\n') continue;
            if (--want <= 0) { from = i; break; }
        }
    }
    for (uint32_t i = from; i < n; i++) kputc(buf[i]);
    if (klog_total() > KLOG_CAP)
        kprintf("\n[dmesg: ring holds the newest %u of %lu bytes]\n",
                used, (unsigned long)klog_total());
    kfree(buf);
    return 0;
}

int cmd_crashlog(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "clear") == 0) {
        fs_node *f = fs_lookup(CRASH_LOG_PATH);
        if (f) { fs_remove_node(f); fs_sync(); kprintf("crashlog: cleared\n"); }
        else kprintf("crashlog: nothing to clear\n");
        return 0;
    }

    fs_node *f = fs_lookup(CRASH_LOG_PATH);
    if (!f || !f->size) {
        kprintf("No crashes recorded. (%s is empty.)\n", CRASH_LOG_PATH);
        kprintf("Reports land here when a ring-3 program faults or the kernel\n");
        kprintf("panics, and survive the reboot. 'crash null' triggers one.\n");
        return 0;
    }

    int crashes = 0;
    for (uint32_t i = 0; i + 4 < f->size; i++)
        if (memcmp(f->data + i, "====", 4) == 0 &&
            (i == 0 || f->data[i - 1] == '\n')) crashes++;
    kprintf("%s  (%u bytes, %d record%s)\n\n", CRASH_LOG_PATH,
            (unsigned)f->size, crashes / 2, crashes / 2 == 1 ? "" : "s");
    for (uint32_t i = 0; i < f->size; i++) kputc((char)f->data[i]);
    return 0;
}
