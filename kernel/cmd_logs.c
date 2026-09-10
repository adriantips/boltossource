#include <stdint.h>
#include <stdio.h>
#include "commands.h"
#include "klog.h"
#include "fs.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_logs.c
 *  `logs` -- the kernel log that outlives the boot it was written in.
 *
 *  `dmesg` shows the ring, which is this boot only. klog_persist() has always
 *  written that ring to /var/log/kernel.log on a clean shutdown, and nothing
 *  has ever read it back -- so the log survived the reboot and then had no way
 *  of being seen, which is the same as not surviving it.
 *
 *  There is a second gap this closes. The ring is only persisted at shutdown,
 *  so the boot that goes wrong is exactly the boot whose log is lost. `logs
 *  save` writes it now, which is the thing to run before rebooting a machine
 *  that has just done something inexplicable.
 * ===========================================================================*/

static void usage(void) {
    kprintf("usage: logs [-n LINES]   the kernel log kept across reboots\n"
            "       logs -a           all of it, not just the tail\n"
            "       logs save         write this boot's log to disk now\n"
            "       logs clear        empty the file\n"
            "  `dmesg` shows this boot's ring; this shows what survived earlier\n"
            "  ones. `crashlog` is the separate record of faults and panics.\n");
}

/* Print the last `lines` lines of a buffer. Walking back from the end rather
 * than counting forward: the interesting part of a log is always the bottom. */
static void tail(const uint8_t *d, uint32_t n, int lines) {
    if (!n) return;
    uint32_t start = n;
    int seen = 0;
    while (start > 0) {
        if (d[start - 1] == '\n' && ++seen > lines) break;
        start--;
    }
    for (uint32_t i = start; i < n; i++) kputc((char)d[i]);
    if (n && d[n - 1] != '\n') kputc('\n');
}

int cmd_logs(int argc, char **argv) {
    int lines = 40, all = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { usage(); return 0; }
        if (strcmp(argv[i], "-a") == 0) { all = 1; continue; }
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            lines = 0;
            for (const char *p = argv[++i]; *p; p++) {
                if (*p < '0' || *p > '9') { kprintf("logs: not a number: %s\n", argv[i]); return 1; }
                lines = lines * 10 + (*p - '0');
            }
            if (lines <= 0) lines = 1;
            continue;
        }
        if (strcmp(argv[i], "save") == 0) {
            if (klog_persist() != 0) { kprintf("logs: could not write %s\n", KERNEL_LOG_PATH); return 1; }
            fs_sync();
            fs_node *f = fs_lookup(KERNEL_LOG_PATH);
            kprintf("logs: this boot's log appended to %s (%u bytes total)\n",
                    KERNEL_LOG_PATH, f ? (unsigned)f->size : 0u);
            return 0;
        }
        if (strcmp(argv[i], "clear") == 0) {
            fs_node *f = fs_lookup(KERNEL_LOG_PATH);
            if (!f) { kprintf("logs: nothing to clear\n"); return 0; }
            fs_remove_node(f); fs_sync();
            kprintf("logs: cleared %s\n", KERNEL_LOG_PATH);
            return 0;
        }
        kprintf("logs: unknown option %s\n", argv[i]);
        return 1;
    }

    fs_node *f = fs_lookup(KERNEL_LOG_PATH);
    if (!f || !f->size) {
        kprintf("%s is empty.\n", KERNEL_LOG_PATH);
        kprintf("The kernel log is written there on a clean shutdown, and by\n"
                "'logs save'. 'dmesg' shows this boot's log, which is still in RAM.\n");
        return 0;
    }

    /* How many boots are in there: the banner starts each one. */
    int boots = 0;
    for (uint32_t i = 0; i + 13 < f->size; i++)
        if (memcmp(f->data + i, "BoltOS kernel", 13) == 0) boots++;

    kprintf("%s  (%u bytes, %d boot%s)\n\n", KERNEL_LOG_PATH,
            (unsigned)f->size, boots, boots == 1 ? "" : "s");
    if (all) for (uint32_t i = 0; i < f->size; i++) kputc((char)f->data[i]);
    else     tail(f->data, f->size, lines);
    if (!all && f->size)
        kprintf("\n(last %d lines; 'logs -a' for all of it, 'logs -n N' for more)\n", lines);
    return 0;
}
