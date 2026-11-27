/* ===========================================================================
 *  BoltBoot  -  mini/boot/hw.h
 *
 *  The few things every part of the firmware uses: register access, the
 *  24 MHz counter for delays, GPIO, and a log that goes to UART0 and into
 *  the report in SRAM (boot.h) that pi.py fel reads back.
 * ===========================================================================*/
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "boot.h"

static inline void     w32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t r32(uintptr_t a)             { return *(volatile uint32_t *)a; }
static inline void     set32(uintptr_t a, uint32_t m) { w32(a, r32(a) | m); }
static inline void     clr32(uintptr_t a, uint32_t m) { w32(a, r32(a) & ~m); }
static inline void     clrset32(uintptr_t a, uint32_t clr, uint32_t set) { w32(a, (r32(a) & ~clr) | set); }
#define dsb()  __asm__ volatile("dsb sy" ::: "memory")
#define isb()  __asm__ volatile("isb" ::: "memory")

/* The Generic Timer's counter: 24 MHz from the crystal, running since power-on. */
static inline uint64_t ticks(void) { uint64_t v; __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v)); return v; }
static inline void udelay(uint32_t us) { uint64_t end = ticks() + (uint64_t)us * 24; while (ticks() < end) { } }
static inline void mdelay(uint32_t ms) { udelay(ms * 1000u); }
static inline uint32_t now_us(void)    { return (uint32_t)(ticks() / 24); }

/* ---- the H618's blocks ----------------------------------------------------- */
#define CCU          0x03001000u
#define PIO          0x0300B000u
#define UART0        0x05000000u
#define WDT          0x030090A0u
#define BANK_C 2
#define BANK_F 5
#define BANK_H 7

void gpio_func(int bank, int pin, int func);
void gpio_out(int bank, int pin, int val);
void led_green(int on);
void led_red(int on);

/* ---- log ------------------------------------------------------------------- */
void log_init(void);
void log_putc(char c);
void log_puts(const char *s);
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

void return_to_fel(void) __attribute__((noreturn));
