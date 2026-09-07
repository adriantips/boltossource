/* ===========================================================================
 *  BoltOS  -  kernel/klog.c
 *  The kernel log ring and the persistent crash log. See include/klog.h.
 *
 *  Everything here has to be callable from a fault handler, so the ring is a
 *  fixed .bss buffer written with plain stores -- no allocation, no recursion
 *  into kprintf, no dependency on a healthy heap. Only the final "write it to
 *  the filesystem" step touches anything that can fail, and it is guarded so a
 *  fault raised inside the reporter cannot loop.
 * ===========================================================================*/
#include <stdint.h>
#include "klog.h"
#include "fs.h"
#include "kprintf.h"
#include "string.h"
#include "pit.h"

/* ------------------------------------------------------------- the ring ----*/
static char     ring[KLOG_CAP];
static uint32_t head;              /* next write position                     */
static uint64_t total;             /* bytes ever written                      */

void klog_putc(char c) {
    ring[head] = c;
    head = (head + 1) % KLOG_CAP;
    total++;
}

uint32_t klog_used(void)  { return total < KLOG_CAP ? (uint32_t)total : KLOG_CAP; }
uint64_t klog_total(void) { return total; }

uint32_t klog_snapshot(char *out, uint32_t cap) {
    uint32_t used = klog_used();
    uint32_t n = used < cap ? used : cap;                 /* newest n bytes */
    uint32_t start = (head + KLOG_CAP - n) % KLOG_CAP;
    for (uint32_t i = 0; i < n; i++) out[i] = ring[(start + i) % KLOG_CAP];
    return n;
}

/* --------------------------------------------------------- formatting ------*/
/* A tiny local formatter. kprintf would work, but it writes through kputc,
 * which feeds this very ring -- so building a crash record with it would echo
 * the record into the log it is about to save. These write into a caller
 * buffer instead and stay allocation-free. */
typedef struct { char *p; char *end; } sb;

static void sb_ch(sb *b, char c) { if (b->p < b->end) *b->p++ = c; }
static void sb_str(sb *b, const char *s) { while (*s) sb_ch(b, *s++); }
static void sb_hex(sb *b, uint64_t v, int digits) {
    static const char *h = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) sb_ch(b, h[(v >> (i * 4)) & 0xF]);
}
static void sb_dec(sb *b, uint64_t v) {
    char t[24]; int i = 0;
    if (!v) { sb_ch(b, '0'); return; }
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) sb_ch(b, t[--i]);
}
static void sb_reg(sb *b, const char *name, uint64_t v) {
    sb_str(b, "  "); sb_str(b, name); sb_str(b, "=0x"); sb_hex(b, v, 16);
}

static const char *exc_name(uint64_t n) {
    static const char *e[32] = {
        "Divide-by-zero", "Debug", "NMI", "Breakpoint", "Overflow",
        "BOUND range", "Invalid opcode", "Device not available", "Double fault",
        "Coproc segment", "Invalid TSS", "Segment not present", "Stack-segment",
        "General protection", "Page fault", "Reserved", "x87 FP",
        "Alignment check", "Machine check", "SIMD FP", "Virtualization",
        "Control protection", "Reserved", "Reserved", "Reserved", "Reserved",
        "Reserved", "Reserved", "Hypervisor", "VMM comm", "Security", "Reserved"
    };
    return n < 32 ? e[n] : "external";
}

static inline uint64_t read_cr2_local(void) {
    uint64_t v; __asm__ volatile("mov %%cr2,%0" : "=r"(v)); return v;
}

/* Same conservative frame-pointer test the panic dumper uses: only walk into
 * memory that is definitely mapped, so the report cannot fault. */
static int rbp_ok(uint64_t p) {
    if (p & 7) return 0;
    if (p >= 0x1000 && p < 0x100000000ull) return 1;
    if (p >= 0xffffffff80000000ull)        return 1;
    return 0;
}

static uint32_t format_record(char *out, uint32_t cap, const char *kind,
                              const char *name, struct registers *r) {
    sb b = { out, out + cap };

    sb_str(&b, "==== BoltOS crash: ");
    sb_str(&b, kind);
    if (name && name[0]) { sb_str(&b, " in "); sb_str(&b, name); }
    sb_str(&b, " ====\n");

    sb_str(&b, "  uptime "); sb_dec(&b, pit_ticks() / (pit_hz() ? pit_hz() : 1000));
    sb_str(&b, "s   vector "); sb_dec(&b, r->int_no);
    sb_str(&b, " ("); sb_str(&b, exc_name(r->int_no)); sb_str(&b, ")");
    sb_str(&b, "   err=0x"); sb_hex(&b, r->err_code, 4);
    sb_str(&b, "   ring "); sb_dec(&b, r->cs & 3);
    sb_ch(&b, '\n');

    if (r->int_no == 14) {
        uint64_t e = r->err_code;
        sb_str(&b, "  page fault at 0x"); sb_hex(&b, read_cr2_local(), 16);
        sb_str(&b, ": ");
        sb_str(&b, (e & 1) ? "protection violation" : "non-present page");
        sb_str(&b, (e & 2) ? " on write" : " on read");
        sb_str(&b, (e & 4) ? ", user mode" : ", kernel mode");
        if (e & 16) sb_str(&b, ", instruction fetch");
        sb_ch(&b, '\n');
    }

    sb_reg(&b, "RIP", r->rip); sb_reg(&b, "RSP", r->rsp); sb_ch(&b, '\n');
    sb_reg(&b, "RAX", r->rax); sb_reg(&b, "RBX", r->rbx); sb_ch(&b, '\n');
    sb_reg(&b, "RCX", r->rcx); sb_reg(&b, "RDX", r->rdx); sb_ch(&b, '\n');
    sb_reg(&b, "RSI", r->rsi); sb_reg(&b, "RDI", r->rdi); sb_ch(&b, '\n');
    sb_reg(&b, "RBP", r->rbp); sb_reg(&b, "R8 ", r->r8);  sb_ch(&b, '\n');
    sb_reg(&b, "R9 ", r->r9);  sb_reg(&b, "R10", r->r10); sb_ch(&b, '\n');
    sb_reg(&b, "R11", r->r11); sb_reg(&b, "R12", r->r12); sb_ch(&b, '\n');
    sb_reg(&b, "R13", r->r13); sb_reg(&b, "R14", r->r14); sb_ch(&b, '\n');
    sb_reg(&b, "R15", r->r15); sb_reg(&b, "CS ", r->cs);  sb_ch(&b, '\n');

    sb_str(&b, "  backtrace: 0x"); sb_hex(&b, r->rip, 16);
    uint64_t rbp = r->rbp;
    for (int i = 0; i < 12 && rbp_ok(rbp); i++) {
        uint64_t next = ((uint64_t *)rbp)[0];
        uint64_t ret  = ((uint64_t *)rbp)[1];
        if (ret < 0x1000) break;
        sb_str(&b, " 0x"); sb_hex(&b, ret, 16);
        if (next <= rbp || next - rbp > 0x40000) break;
        rbp = next;
    }
    sb_str(&b, "\n\n");
    return (uint32_t)(b.p - out);
}

/* ------------------------------------------------------- persistence -------*/
static void ensure_log_dir(void) {
    if (!fs_lookup("/var"))     fs_create("/var", 1);
    if (!fs_lookup("/var/log")) fs_create("/var/log", 1);
}

/* Keep a log file from growing without bound: once it passes `cap`, drop the
 * oldest half. Cheap, and it keeps the newest records, which is what matters. */
static void trim_file(fs_node *f, uint32_t cap) {
    if (!f || f->size <= cap) return;
    uint32_t keep = cap / 2;
    uint32_t from = f->size - keep;
    /* start at the line boundary after `from` so the file never begins mid-record */
    while (from < f->size && f->data[from] != '\n') from++;
    if (from < f->size) from++;
    fs_write(f, f->data + from, f->size - from);
}

static int append_log(const char *path, const char *text, uint32_t len, uint32_t cap) {
    ensure_log_dir();
    fs_node *f = fs_lookup(path);
    if (!f) f = fs_create(path, 0);
    if (!f) return -1;
    trim_file(f, cap);
    return fs_append(f, text, len);
}

int klog_persist(void) {
    static char buf[16 * 1024];
    uint32_t n = klog_snapshot(buf, sizeof buf);
    if (!n) return 0;
    return append_log(KERNEL_LOG_PATH, buf, n, 96 * 1024);
}

/* ------------------------------------------------------------ crashes ------*/
static int in_progress;

int crash_in_progress(void) { return in_progress; }

void crash_record(const char *kind, const char *name, struct registers *r) {
    if (in_progress) return;              /* a fault inside the reporter */
    in_progress = 1;

    /* Static, because a crash handler must not depend on the heap. */
    static char rec[3072];
    uint32_t n = format_record(rec, sizeof rec, kind, name, r);

    /* Straight to serial first: even if everything below fails, the record
     * exists somewhere. Written through kputc so it lands in the ring too. */
    for (uint32_t i = 0; i < n; i++) kputc(rec[i]);

    /* Then to disk, so it is still there after the reboot. Best effort: the
     * machine may be too far gone to reach the filesystem, and that is exactly
     * why the serial copy above happens first. */
    if (fs_persist_active() && !fs_persist_readonly()) {
        if (append_log(CRASH_LOG_PATH, rec, n, 64 * 1024) == 0)
            fs_sync();
    }

    in_progress = 0;
}
