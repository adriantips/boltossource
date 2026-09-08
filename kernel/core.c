#include <stdint.h>
#include "core.h"
#include "proc.h"
#include "vmm.h"
#include "fs.h"
#include "hw.h"
#include "kprintf.h"
#include "string.h"
#include <stdio.h>

/* ===========================================================================
 *  BoltOS  -  kernel/core.c
 *  Writing a core dump on a ring-3 fault.
 *
 *  This runs inside the fault handler, on the faulting thread's kernel stack,
 *  with the dead process's address space still in CR3 -- the one moment its
 *  stack is readable. Two rules follow from that and shape everything here:
 *
 *    * never fault while doing it. Every user page is checked with
 *      vmm_get_phys() before it is read, so a wild RSP produces a short dump
 *      rather than a second fault inside the handler for the first one.
 *    * never make things worse. Every failure path just gives up quietly; a
 *      crash report that cannot be saved must not stop the process from being
 *      killed and the machine from carrying on.
 * ===========================================================================*/

static void ensure_dir(void) {
    if (!fs_lookup("/var"))   fs_create("/var", 1);
    if (!fs_lookup(CORE_DIR)) fs_create(CORE_DIR, 1);
}

/* Keep only the newest CORE_KEEP dumps: a program crashing in a loop should
 * not be able to fill the disk with the same report. */
static void prune(void) {
    fs_node *d = fs_lookup(CORE_DIR);
    if (!d || !d->is_dir) return;
    for (;;) {
        int n = 0;
        fs_node *oldest = 0;
        for (fs_node *c = d->child; c; c = c->next) {
            if (c->is_dir) continue;
            n++;
            if (!oldest || c->mtime < oldest->mtime) oldest = c;
        }
        if (n <= CORE_KEEP || !oldest) return;
        fs_remove_node(oldest);
    }
}

/* The last path component, without its directories. */
static const char *base_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/' && q[1]) b = q + 1;
    return b;
}

void core_write(int pid, const char *exe, struct registers *r) {
    proc_t *p = proc_current();
    if (!p || !r) return;

    /* Capture from the faulting stack pointer upwards: everything below RSP is
     * dead, and everything above is the frames that led here. */
    uint64_t base = r->rsp & ~15ull;
    if (base >= USER_STACK_TOP) return;              /* nonsense RSP */
    uint64_t want = USER_STACK_TOP - base;
    if (want > CORE_STACK_MAX) want = CORE_STACK_MAX;

    /* Stop at the first unmapped page rather than trusting the register. */
    uint64_t len = 0;
    while (len < want) {
        uint64_t page = (base + len) & ~0xFFFull;
        if (!vmm_get_phys(p->cr3, page)) break;
        uint64_t step = 0x1000 - ((base + len) & 0xFFF);
        if (len + step > want) step = want - len;
        len += step;
    }
    if (len < 64) len = 0;                           /* not worth a file */

    core_hdr_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, CORE_MAGIC, 8);
    h.version = CORE_VERSION;
    h.pid = (uint32_t)pid;
    strncpy(h.exe, exe ? exe : "?", sizeof h.exe - 1);
    struct rtc_time t;
    rtc_now(&t);
    snprintf(h.when, sizeof h.when, "%04d-%02d-%02d %02d:%02d:%02d",
             t.year, t.mon, t.day, t.hour, t.min, t.sec);
    if (r->int_no == 14) __asm__ volatile("mov %%cr2,%0" : "=r"(h.cr2));
    h.stack_base = base;
    h.stack_len  = (uint32_t)len;
    h.regs       = *r;

    ensure_dir();
    char path[128];
    snprintf(path, sizeof path, CORE_DIR "/%s.%d.core", base_of(h.exe), pid);

    fs_node *f = fs_lookup(path);
    if (!f) f = fs_create(path, 0);
    if (!f) return;
    if (fs_write(f, &h, (uint32_t)sizeof h) != 0) return;
    if (len && fs_append(f, (const void *)base, (uint32_t)len) != 0) return;

    prune();
    kprintf("[core] %s (%u bytes of stack)\n", path, (unsigned)len);
}
