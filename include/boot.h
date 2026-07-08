#pragma once
#include <stdint.h>

/* Handed from stage2.asm (physical 0x0500) into kmain via RDI. */
struct bootinfo {
    uint64_t fb_addr;
    uint32_t fb_width, fb_height, fb_pitch, fb_bpp;
    uint32_t e820_count;
    uint32_t e820_addr;
} __attribute__((packed));
