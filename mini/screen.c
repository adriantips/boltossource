/* ===========================================================================
 *  BoltOS Mini  -  mini/screen.c
 *
 *  The screen is two buffers. The shell draws into `back`; screen_present()
 *  copies the rectangle it drew into `front`, which is what is shown: QEMU
 *  scans it out of guest RAM through ramfb, and the VNC server compares it
 *  against what each viewer last saw. With no panel on the Pi yet, front is
 *  the screen as far as anybody can tell.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"

surface_t g_back;
uint32_t *g_front;
int scr_w, scr_h;
volatile uint32_t g_front_gen;

int screen_init(int w, int h) {
    uint64_t bytes = (uint64_t)w * h * 4;
    uint32_t *b = (uint32_t *)kmalloc(bytes);
    uint32_t *f = (uint32_t *)kmalloc(bytes);
    if (!b || !f) { kprintf("[screen] no memory for %dx%d\n", w, h); return -1; }
    memset(b, 0, bytes);
    memset(f, 0, bytes);
    g_back.px = b; g_back.w = w; g_back.h = h; g_back.stride = w;
    g_front = f;
    scr_w = w; scr_h = h;
    if (g_plat == PLAT_QEMU && qemu_ramfb_init(g_front, w, h) == 0)
        kprintf("[screen] %dx%d, shown in QEMU's window (ramfb)\n", w, h);
    else
        kprintf("[screen] %dx%d in RAM (no panel: see it over VNC)\n", w, h);
    return 0;
}

void screen_present(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h) h = scr_h - y;
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; r++)
        memcpy(g_front + (size_t)(y + r) * scr_w + x,
               g_back.px + (size_t)(y + r) * g_back.stride + x, (size_t)w * 4);
    g_front_gen++;
}
