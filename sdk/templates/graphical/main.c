/* @NAME@ -- a BoltOS program that takes the screen.
 *
 * SYS_FBINFO claims the panel and pauses the compositor; SYS_FBPRESENT blits
 * one packed xRGB frame; SYS_FBEND hands the desktop back. There is no window
 * and no toolkit -- a program that asks for the framebuffer gets all of it,
 * the way /bin/browser does. Exiting (or faulting) releases it, so a crash in
 * here does not take the desktop with it.
 *
 * `bolt build`, `bolt ship`, then `run @NAME@` over there. Esc or q to quit.
 */
#include "ulibc.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

/* The 8x8 bitmap font out of kernel/font8x8.c, which the SDK links into every
 * program. One byte per row, bit 0 is the leftmost pixel. */
extern const unsigned char font8x8_basic[128][8];

static struct user_fbinfo FB;
static uint32_t *px;                 /* backbuffer: packed FB.w * FB.h xRGB */

static void clear(uint32_t colour) {
    uint64_t n = (uint64_t)FB.w * FB.h;
    for (uint64_t i = 0; i < n; i++) px[i] = colour;
}

static void rect(int x, int y, int w, int h, uint32_t colour) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)FB.w) w = (int)FB.w - x;
    if (y + h > (int)FB.h) h = (int)FB.h - y;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            px[(uint64_t)(y + r) * FB.w + (x + c)] = colour;
}

/* One glyph, scaled by whole pixels. Anything past ASCII draws as nothing,
 * which is the right amount of effort for a font with 128 entries. */
static void glyph(int x, int y, char ch, uint32_t colour, int scale) {
    unsigned c = (unsigned char)ch;
    if (c > 127) return;
    for (int row = 0; row < 8; row++) {
        unsigned bits = font8x8_basic[c][row];
        for (int col = 0; col < 8; col++)
            if (bits & (1u << col))
                rect(x + col * scale, y + row * scale, scale, scale, colour);
    }
}

static void text(int x, int y, const char *s, uint32_t colour, int scale) {
    for (; *s; s++) {
        if (*s != ' ') glyph(x, y, *s, colour, scale);
        x += 8 * scale;
    }
}

int main(void) {
    if (bolt_abi() != BOLT_ABI_VERSION) {
        printf("@NAME@: built for BoltOS ABI %d, this kernel speaks %d\n",
               BOLT_ABI_VERSION, bolt_abi());
        return 1;
    }

    if (fb_map(&FB) != 0) {
        printf("@NAME@: no framebuffer -- is something else holding it?\n");
        return 1;
    }

    px = (uint32_t *)malloc((uint64_t)FB.w * FB.h * 4);
    if (!px) {
        fb_release();
        printf("@NAME@: %ux%u backbuffer would not fit\n", FB.w, FB.h);
        return 1;
    }

    int bx = (int)FB.w / 2, by = (int)FB.h / 2;
    int dx = 3, dy = 2;
    const int side = 64;

    for (;;) {
        clear(0x00101820);

        text(24, 24, "@NAME@", 0xFFFFFFFF, 3);
        text(24, 64, "esc or q to quit", 0xFF8899AA, 2);

        rect(bx, by, side, side, 0xFF3AA6FF);

        bx += dx; by += dy;
        if (bx < 0 || bx + side > (int)FB.w) { dx = -dx; bx += dx; }
        if (by < 0 || by + side > (int)FB.h) { dy = -dy; by += dy; }

        fb_present(px);

        int k = getkey();
        if (k == 27 || k == 'q') break;

        /* There is no sleep syscall, so this runs as fast as the scheduler
         * lets it and the box moves at whatever speed that turns out to be.
         * yield() at least stops it starving everything else. */
        yield();
    }

    fb_release();
    free(px);
    printf("@NAME@: screen handed back\n");
    return 0;
}
