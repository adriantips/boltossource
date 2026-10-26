/* libc/support.c -- odds and ends: the global errno, assert handler, and the
 * C-locale localeconv()/setlocale() that number formatting consults. */
#include "errno.h"
#include "locale.h"
#include "kprintf.h"

/* Where a dead end parks the CPU: hlt on the kernel's x86-64, a trap on
 * WebAssembly (BoltOS TV for Samsung), where there is nothing to halt. */
#if defined(__x86_64__)
#define LIBC_HALT() __asm__ volatile("hlt")
#else
#define LIBC_HALT() __builtin_trap()
#endif

int errno = 0;

void __assert_fail(const char *expr, const char *file, int line, const char *func) {
    kprintf("\n[assert] %s:%d: %s: assertion failed: %s\n", file, line, func, expr);
    panic("assertion failed");
    for (;;) LIBC_HALT();
}

static struct lconv c_locale = {
    ".", "", "", "", "", ".", "", "", "", "",
    127, 127, 127, 127, 127, 127, 127, 127
};
struct lconv *localeconv(void) { return &c_locale; }
char *setlocale(int category, const char *locale) { (void)category; (void)locale; return "C"; }
