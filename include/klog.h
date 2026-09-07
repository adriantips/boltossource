#pragma once
#include <stdint.h>
#include "interrupts.h"

/* ===========================================================================
 *  Kernel log ring + crash reports.
 *
 *  Every character that goes through kputc() is also copied into a fixed ring
 *  in .bss, so `dmesg` can show the boot log long after the console has
 *  scrolled and the GUI has taken the screen. The ring costs no allocation and
 *  no disk I/O, which is what makes it safe to write to from a fault handler.
 *
 *  On top of that sits the crash log: when a ring-3 process faults, or the
 *  kernel panics, a record is formatted (registers, fault decode, backtrace)
 *  and appended to /var/log/crash.log so it is still there on the next boot.
 *  `crashlog` reads it back. A panic writes on a best-effort basis -- the
 *  machine may be too broken to reach the disk -- but a ring-3 fault is fully
 *  recoverable and always persists.
 * ===========================================================================*/

#define KLOG_CAP  (64 * 1024)
#define CRASH_LOG_PATH "/var/log/crash.log"
#define KERNEL_LOG_PATH "/var/log/kernel.log"

/* Called from kputc for every character. Must never allocate or take a lock. */
void     klog_putc(char c);

/* Copy the most recent bytes of the ring into `out`. Returns bytes copied. */
uint32_t klog_snapshot(char *out, uint32_t cap);
uint32_t klog_used(void);          /* bytes currently held (<= KLOG_CAP)      */
uint64_t klog_total(void);         /* bytes ever written (overflow indicator) */

/* Persist the ring to KERNEL_LOG_PATH, keeping the file under a size cap so it
 * cannot grow without bound across boots. 0 on success. */
int      klog_persist(void);

/* Record a crash. `kind` is a short tag ("ring-3 fault", "kernel panic").
 * `name` names the process, or 0 for the kernel. Formats the registers and a
 * backtrace, appends to CRASH_LOG_PATH, and commits the filesystem.
 * Safe to call from a fault handler; re-entry is ignored. */
void     crash_record(const char *kind, const char *name, struct registers *r);

/* 1 while a panic is being reported -- the fault handler checks this so a
 * fault raised *by* the crash reporter halts instead of recursing. */
int      crash_in_progress(void);
