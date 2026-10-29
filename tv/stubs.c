/* ===========================================================================
 *  BoltOS TV  -  tv/stubs.c
 *
 *  The shared kernel code was written for the desktop, and in a handful of
 *  places it reaches for something the TV kernel does not have: a filesystem
 *  to keep a config file in, a process table, a crash log on disk, the
 *  compositor. Each of those is answered here, once, with what a television
 *  should do instead -- usually "nothing, and say so on the serial port".
 *
 *  The list is short because build-tv.sh's source list was chosen by link
 *  closure: start from what the TV needs, and cut every edge that leads back
 *  into the desktop. If a new shared file grows one more of these, the link
 *  fails and names it, which is the point.
 * ===========================================================================*/
#include <stdint.h>
#include "kprintf.h"
#include "serial.h"
#include "fs.h"
#include "proc.h"
#include "interrupts.h"
#include "usb.h"
#include "tv.h"

/* ---- the console --------------------------------------------------------- *
 * Every kprintf goes to COM1 and into a ring the About page can show. Nothing
 * is ever painted: a television that scrolls a boot log across the picture
 * looks broken even when it is not. */
#define LOGCAP (32 * 1024)
static char     logbuf[LOGCAP];
static uint32_t loghead;
static uint64_t logtotal;

void kputc(char c) {
    serial_putc(c);
    logbuf[loghead] = c;
    loghead = (loghead + 1) % LOGCAP;
    logtotal++;
}

uint32_t tv_log_snapshot(char *out, uint32_t cap) {
    if (!cap) return 0;
    uint32_t have = logtotal < LOGCAP ? (uint32_t)logtotal : LOGCAP;
    if (have > cap - 1) have = cap - 1;
    uint32_t start = (loghead + LOGCAP - have) % LOGCAP;
    for (uint32_t i = 0; i < have; i++) out[i] = logbuf[(start + i) % LOGCAP];
    out[have] = 0;
    return have;
}

/* ---- crashes ------------------------------------------------------------- *
 * The desktop persists a crash report to /var/log; with no disk the report is
 * what interrupts.c already printed to serial. */
static int crashing;
void crash_record(const char *kind, const char *name, struct registers *r) {
    (void)r;
    crashing = 1;
    kprintf("[tv] %s%s%s -- no disk to keep a crash log on; the above is all of it\n",
            kind ? kind : "crash", name ? ": " : "", name ? name : "");
}
int crash_in_progress(void) { return crashing; }

/* ---- processes ----------------------------------------------------------- *
 * The TV kernel runs no ring-3 code, so the scheduler only ever holds kernel
 * threads and none of these can be reached with a real process. */
uint64_t syscall_kstack_top;
int  proc_pid_of(proc_t *p) { (void)p; return -1; }
void proc_release(proc_t *p) { (void)p; }
void proc_note_exit(int pid, int code) { (void)pid; (void)code; }
struct registers *proc_fault_kill(struct registers *r) { return r; }

/* ---- the filesystem ------------------------------------------------------ *
 * The keyboard layout, the shortcut table and the mixer each try to load and
 * save a file under /etc. There is no /etc: lookups find nothing, so each of
 * them keeps its built-in defaults, and saves are refused. */
fs_node *fs_lookup(const char *path) { (void)path; return 0; }
fs_node *fs_create(const char *path, int is_dir) { (void)path; (void)is_dir; return 0; }
int      fs_write(fs_node *n, const void *data, uint32_t len) { (void)n; (void)data; (void)len; return -1; }

/* /etc/hosts has nowhere to live either; dns.c asks it first. */
uint32_t netcfg_hosts_lookup(const char *name) { (void)name; return 0; }

/* ---- the compositor ------------------------------------------------------ *
 * font.c asks for a repaint when a face finishes loading; the TV shell
 * repaints every frame anyway. */
void gui_request_redraw(void) { }

/* ---- DNS ----------------------------------------------------------------- *
 * The one hook that is not a stub: dns.c asks before it resolves anything,
 * and the answer comes from the ad blocker. That is what makes it apply to
 * the whole set, not just the browser. */
int dns_blocked(const char *host) {
    return tvs.adblock && adblock_host_blocked(host);
}
