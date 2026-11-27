/* ===========================================================================
 *  BoltBoot  -  mini/boot/hw.c
 *
 *  GPIO, the LEDs, UART0 and the report log (hw.h).
 *
 *  The report sits in SRAM at BOOT0_REPORT: "BRPT", the length, then the
 *  text. Over FEL the Mac reads it back after boot0 returns; on a card boot
 *  nobody reads it, but UART0 (the 3-pin header by the Ethernet jack,
 *  115200 8N1) gets the same lines.
 * ===========================================================================*/
#include <stdarg.h>
#include "hw.h"

/* ---- GPIO ------------------------------------------------------------------ */
#define PIO_CFG(b, p) (PIO + (b) * 0x24u + ((p) / 8) * 4)
#define PIO_DAT(b)    (PIO + (b) * 0x24u + 0x10)

void gpio_func(int bank, int pin, int func) {
    int sh = (pin % 8) * 4;
    clrset32(PIO_CFG(bank, pin), 0xFu << sh, (uint32_t)func << sh);
}

void gpio_out(int bank, int pin, int val) {
    gpio_func(bank, pin, 1);
    if (val) set32(PIO_DAT(bank), 1u << pin);
    else     clr32(PIO_DAT(bank), 1u << pin);
}

/* The Zero3's two LEDs: red PC12 (power), green PC13 (status). */
void led_red(int on)   { gpio_out(BANK_C, 12, on); }
void led_green(int on) { gpio_out(BANK_C, 13, on); }

/* ---- UART0 + report -------------------------------------------------------- */
static int uart_up;

void log_init(void) {
    w32(BOOT0_REPORT, 0x54505242u);               /* "BRPT"                       */
    w32(BOOT0_REPORT + 4, 0);
    /* UART0: PH0 TX / PH1 RX, function 2; its bus clock gate and reset in the
     * CCU (UART_BGR, bit 0 and bit 16); APB2 runs from the 24 MHz crystal
     * after the ROM, so 115200 baud is a divisor of 13. */
    gpio_func(BANK_H, 0, 2);
    gpio_func(BANK_H, 1, 2);
    set32(CCU + 0x90C, 1u << 16);
    set32(CCU + 0x90C, 1u << 0);
    w32(UART0 + 0x04, 0);                         /* IER: none                    */
    w32(UART0 + 0x0C, 0x83);                      /* LCR: 8N1, divisor latch      */
    w32(UART0 + 0x00, 13);
    w32(UART0 + 0x04, 0);
    w32(UART0 + 0x0C, 0x03);
    w32(UART0 + 0x08, 0x07);                      /* FIFOs on, cleared            */
    uart_up = 1;
}

static void uart_tx(char c) {
    for (int i = 0; i < 20000 && !(r32(UART0 + 0x14) & 0x20); i++) { }  /* THRE */
    w32(UART0, (uint8_t)c);
}

void log_putc(char c) {
    uint32_t n = r32(BOOT0_REPORT + 4);
    if (n < BOOT0_REPORT_SIZE - 8) {
        *(volatile uint8_t *)(uintptr_t)(BOOT0_REPORT + 8 + n) = (uint8_t)c;
        w32(BOOT0_REPORT + 4, n + 1);
    }
    if (uart_up) {
        if (c == '\n') uart_tx('\r');
        uart_tx(c);
    }
}

void log_puts(const char *s) { while (*s) log_putc(*s++); }

static void put_num(uint64_t v, int base, int width, char pad, int neg) {
    char b[24];
    int n = 0;
    do { b[n++] = "0123456789abcdef"[v % (unsigned)base]; v /= (unsigned)base; } while (v);
    if (neg) b[n++] = '-';
    while (n < width) b[n++] = pad;
    while (n) log_putc(b[--n]);
}

/* %s %c %d %u %x %p, with a width and 0, and l / ll ignored (all 64-bit). */
void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { log_putc(*p); continue; }
        p++;
        char pad = ' ';
        int width = 0, lng = 0;
        if (*p == '0') { pad = '0'; p++; }
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        while (*p == 'l') { lng = 1; p++; }
        switch (*p) {
        case 's': { const char *s = va_arg(ap, const char *); log_puts(s ? s : "(null)"); break; }
        case 'c': log_putc((char)va_arg(ap, int)); break;
        case 'd': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            put_num(v < 0 ? (uint64_t)-v : (uint64_t)v, 10, width, pad, v < 0);
            break;
        }
        case 'u': put_num(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, width, pad, 0); break;
        case 'x': put_num(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, width, pad, 0); break;
        case 'p': log_puts("0x"); put_num((uintptr_t)va_arg(ap, void *), 16, width, pad, 0); break;
        case '%': log_putc('%'); break;
        default:  log_putc('%'); log_putc(*p); break;
        }
    }
    va_end(ap);
}

/* ---- the two the compiler may call on its own ------------------------------ */
void *memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *a = d;
    const uint8_t *b = s;
    while (n--) *a++ = *b++;
    return d;
}
