#pragma once
#include <stdint.h>
#include "boot.h"

void     fb_init(struct bootinfo *bi);
int      fb_present(void);
/* Switch the physical panel to w*h 32bpp via the Bochs DISPI interface (QEMU
 * std VGA). Returns 0 on success, -1 if unsupported -- caller keeps old mode. */
int      fb_set_mode(uint32_t w, uint32_t h);
uint32_t fb_width(void);
uint32_t fb_height(void);
uint64_t fb_phys(void);      /* physical base of the linear framebuffer */
uint32_t fb_pitch_px(void);  /* scanline stride in pixels (>= width) */

/* Set while a ring-3 program owns the panel (maps the fb via SYS_FBINFO); the
 * GUI compositor skips its flip so it doesn't fight the userland renderer. */
extern volatile int g_fb_exclusive;
void     fb_pixel(uint32_t x, uint32_t y, uint32_t color);  /* color = 0x00RRGGBB */
void     fb_fill(uint32_t color);
void     fb_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void     fb_blit(const uint32_t *src);   /* copy a packed fw*fh xRGB image to VRAM */
/* Read the panel back into a packed fb_width()*fb_height() ARGB buffer (alpha
 * forced opaque). This is what a screenshot is: what is actually on screen,
 * including a ring-3 program that owns the panel and the text console, neither
 * of which passes through the compositor. 0 on success, -1 with no panel. */
int      fb_capture(uint32_t *dst);

/* Scale every pixel on its way to the panel by pct/100. This is the software
 * half of include/backlight.h; 100 is the fast path and costs nothing.
 * fb_capture() undoes it, because a screenshot taken at 30% brightness that
 * comes back nearly black is a bug report waiting to happen -- the dim belongs
 * to the panel, not to the picture. */
void     fb_set_dim(int pct);
int      fb_dim(void);
/* nearest-neighbour scale a packed sw*sh xRGB image into panel rect (dx,dy,dw,dh);
 * the rest of the panel is cleared to black (letterbox bars). */
void     fb_blit_scaled(const uint32_t *src, uint32_t sw, uint32_t sh,
                        uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh);
