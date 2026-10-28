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
 *      +768 KiB  the last 252 KiB of the boot before, kept so a crash that
 *                restarted straight into a new boot can still be read
 *                (/log?prev).
 *      +1020 KiB  "kept": a few records that outlive a restart (not a power
 *                cut) -- the Wi-Fi network joined last, for one.
 * ===========================================================================*/
#include <stdint.h>
#include <stdarg.h>
#include "mini.h"
#include "kprintf.h"

#define LOG_MAGIC 0x474F4C42u              /* "BLOG" */
#define PREV_OFF  (768u * 1024)
#define LOG_CAP   (PREV_OFF - 16)
#define KEEP_SIZE 4096u
#define PREV_CAP  ((uint32_t)MINI_LOG_SIZE - PREV_OFF - 4 - KEEP_SIZE)

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

/* ---- kept across a restart -------------------------------------------------------------------
 *  "KEEP", a length, a sum, then records: an 8-byte tag, a 16-bit length, the bytes. A
 *  power cut loses it all (DRAM), and so does a bad sum; nothing is trusted unchecked. */
#define KEEP_MAGIC 0x5045454Bu                 /* "KEEP" */
static volatile uint8_t *keep = (volatile uint8_t *)(uintptr_t)(MINI_LOG_BASE + MINI_LOG_SIZE - KEEP_SIZE);

static uint32_t keep_sum(const uint8_t *p, uint32_t n) {
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static int keep_load(uint8_t *buf) {           /* the records, or 0 bytes */
    for (uint32_t i = 0; i < KEEP_SIZE; i++) buf[i] = keep[i];
    uint32_t magic, len, sum;
    __builtin_memcpy(&magic, buf, 4); __builtin_memcpy(&len, buf + 4, 4); __builtin_memcpy(&sum, buf + 8, 4);
    if (magic != KEEP_MAGIC || len > KEEP_SIZE - 12 || keep_sum(buf + 12, len) != sum) return 0;
    return (int)len;
}

int keep_get(const char *tag, void *out, int cap) {
    static uint8_t buf[KEEP_SIZE];
    int len = keep_load(buf);
    for (int o = 0; o + 10 <= len; ) {
        int n = buf[12 + o + 8] | buf[12 + o + 9] << 8;
        if (o + 10 + n > len) break;
        if (!__builtin_strncmp((const char *)buf + 12 + o, tag, 8)) {
            if (n > cap) n = cap;
            __builtin_memcpy(out, buf + 12 + o + 10, (size_t)n);
            return n;
        }
        o += 10 + n;
    }
    return -1;
}

/* Replace (or with len < 0, remove) the record under `tag`. */
void keep_put(const char *tag, const void *data, int len) {
    static uint8_t buf[KEEP_SIZE], out[KEEP_SIZE];
    int have = keep_load(buf), n = 0;
    for (int o = 0; o + 10 <= have; ) {
        int l = buf[12 + o + 8] | buf[12 + o + 9] << 8;
        if (o + 10 + l > have) break;
        if (__builtin_strncmp((const char *)buf + 12 + o, tag, 8) && n + 10 + l <= (int)KEEP_SIZE - 12) {
            __builtin_memcpy(out + 12 + n, buf + 12 + o, (size_t)(10 + l));
            n += 10 + l;
        }
        o += 10 + l;
    }
    if (len >= 0 && n + 10 + len <= (int)KEEP_SIZE - 12) {
        __builtin_memset(out + 12 + n, 0, 8);
        for (int i = 0; i < 8 && tag[i]; i++) out[12 + n + i] = (uint8_t)tag[i];
        out[12 + n + 8] = (uint8_t)len; out[12 + n + 9] = (uint8_t)(len >> 8);
        __builtin_memcpy(out + 12 + n + 10, data, (size_t)len);
        n += 10 + len;
    }
    uint32_t magic = KEEP_MAGIC, ln = (uint32_t)n, sum = keep_sum(out + 12, (uint32_t)n);
    __builtin_memcpy(out, &magic, 4); __builtin_memcpy(out + 4, &ln, 4); __builtin_memcpy(out + 8, &sum, 4);
    for (int i = 0; i < 12 + n; i++) keep[i] = out[i];
}
