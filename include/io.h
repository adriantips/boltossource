#pragma once
#include <stdint.h>

static inline void    outb(uint16_t p, uint8_t v)  { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb (uint16_t p)             { uint8_t r;  __asm__ volatile("inb %1, %0"  : "=a"(r) : "Nd"(p)); return r; }
static inline void    outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint16_t inw(uint16_t p)             { uint16_t r; __asm__ volatile("inw %1, %0"  : "=a"(r) : "Nd"(p)); return r; }
static inline void    outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p)             { uint32_t r; __asm__ volatile("inl %1, %0"  : "=a"(r) : "Nd"(p)); return r; }

static inline void io_wait(void) { outb(0x80, 0); }
static inline void cli(void)     { __asm__ volatile("cli"); }
static inline void sti(void)     { __asm__ volatile("sti"); }

/* Disable interrupts and say whether they were on, so the caller can put them
 * back the way it found them. A blind cli/sti pair turns every nested critical
 * section into one that ends early. */
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");   /* IF */
}
static inline void hlt(void)     { __asm__ volatile("hlt"); }

static inline uint64_t read_cr2(void){ uint64_t v; __asm__ volatile("mov %%cr2,%0":"=r"(v)); return v; }
