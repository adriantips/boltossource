/* ===========================================================================
 *  BoltOS Mini  -  mini/klog.c   (kputc, kprintf, panic)
 *
 *  Every line goes two places: the UART (the Pi's three-pin debug header, or
 *  QEMU's -serial), and a 1 MiB window of uncached RAM at MINI_LOG_BASE.
 *
 *  The window is the one that matters on the Pi, where there is usually no
 *  serial cable. It is uncached so each byte is in DRAM the moment it is
 *  written, and it sits where U-Boot never writes: after a crash the
 *  watchdog resets the board, U-Boot comes up, and `pi.py log` reads the
 *  window back over USB as U-Boot's DFU alt "log".
 *
 *      +0  "BLOG"     +4  bytes of text     +8  boot count     +12  0
 *      +16 the text (up to 768 KiB)
 *      +768 KiB  the last 256 KiB of the boot before, kept so a crash that
 *                restarted straight into a new boot can still be read
 *                (/log?prev).
 * ===========================================================================*/
#include <stdint.h>
#include <stdarg.h>
#include "mini.h"
#include "kprintf.h"

#define LOG_MAGIC 0x474F4C42u              /* "BLOG" */
#define PREV_OFF  (768u * 1024)
#define LOG_CAP   (PREV_OFF - 16)
#define PREV_CAP  ((uint32_t)MINI_LOG_SIZE - PREV_OFF - 4)

static volatile uint32_t *hdr = (volatile uint32_t *)(uintptr_t)MINI_LOG_BASE;
static volatile char     *text = (volatile char *)(uintptr_t)(MINI_LOG_BASE + 16);
static uint32_t loglen, boots;
static int ready;

static volatile uint32_t *prev_len = (volatile uint32_t *)(uintptr_t)(MINI_LOG_BASE + PREV_OFF);
static volatile char     *prev = (volatile char *)(uintptr_t)(MINI_LOG_BASE + PREV_OFF + 4);

void klog_init(void) {
    if (hdr[0] == LOG_MAGIC && hdr[1] && hdr[1] <= LOG_CAP) {      /* keep the last boot's tail */
        uint32_t n = hdr[1] < PREV_CAP ? hdr[1] : PREV_CAP, s = hdr[1] - n;
        for (uint32_t i = 0; i < n; i++) prev[i] = text[s + i];
        *prev_len = n;
    } else *prev_len = 0;
    boots = (hdr[0] == LOG_MAGIC) ? hdr[2] + 1 : 1;
    loglen = 0;
    hdr[1] = 0;
    hdr[2] = boots;
    hdr[3] = 0;
    hdr[0] = LOG_MAGIC;
    ready = 1;
}

uint32_t klog_boot_count(void) { return boots; }

void kputc(char c) {
    if (c == '\n') uart_putc('\r');
    uart_putc(c);
    if (ready && loglen < LOG_CAP) {
        text[loglen++] = c;
        hdr[1] = loglen;
    }
}

uint32_t klog_previous(char *out, uint32_t cap) {
    if (!cap) return 0;
    uint32_t len = *prev_len > PREV_CAP ? 0 : *prev_len;
    uint32_t n = len < cap - 1 ? len : cap - 1, start = len - n;
    for (uint32_t i = 0; i < n; i++) out[i] = prev[start + i];
    out[n] = 0;
    return n;
}

uint32_t klog_snapshot(char *out, uint32_t cap) {
    if (!cap) return 0;
    uint32_t n = loglen < cap - 1 ? loglen : cap - 1;
    uint32_t start = loglen - n;
    for (uint32_t i = 0; i < n; i++) out[i] = text[start + i];
    out[n] = 0;
    return n;
}

/* The printf the rest of the kernel uses: libc's vsnprintf into a line
 * buffer, so %f, %lld and the rest work here exactly as in snprintf. */
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

void kprintf(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    for (char *p = line; *p; p++) kputc(*p);
}

void panic(const char *msg) {
    irq_off();
    kprintf("\n*** BoltOS Mini panic: %s ***\n", msg);
    kprintf("    (the watchdog restarts the Pi; `pi.py log` shows this)\n");
    for (int i = 0;; i++) {
        board_led(1, (i / 4) & 1);         /* green blinks fast: a panic       */
        delay_ms(60);
    }
}
