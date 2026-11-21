/* ===========================================================================
 *  BoltOS Mini  -  mini/screen.c
 *
 *  The screen is two buffers. The shell draws into `back`; screen_present()
 *  copies what changed into `front`, which is what is shown: QEMU scans it
 *  out of guest RAM through ramfb, and the VNC server sends it to the Mac.
 *  With no panel on the Pi yet, front is the screen as far as anybody can
 *  tell.
 *
 *  Present works in 16x16 tiles. A tile whose pixels are the same in both
 *  buffers is left alone; one that changed is copied and stamped with the
 *  new g_front_gen in g_tile_gen. The VNC server reads those stamps to know
 *  what each viewer still needs, so nothing compares whole frames twice.
 *
 *  A USB mouse's arrow lives only in front, composited over back's
 *  pixels: moving it touches a few tiles and never asks the shell for a
 *  frame (see the pointer, below).
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"

surface_t g_back;
uint32_t *g_front;
int scr_w, scr_h, scr_tw, scr_th;
volatile uint32_t g_front_gen;
uint32_t *g_tile_gen;


static void front_flush(int x, int y, int w, int h);

/* ---- the pointer ---------------------------------------------------------------------- *
 *  The arrow is a sprite: drawn once on black and once on white, which
 *  gives each pixel's colour times alpha (spr_p) and how much of what is
 *  under it shows through (spr_t, per channel). On screen a pixel under it
 *  is spr_p + back * spr_t / 255, worked out from `back`, which always holds
 *  the picture without the arrow.
 *
 *  So front never holds the picture without the arrow where the arrow is:
 *  on a monitor the display engine reads front straight out of RAM, and
 *  anything written there, even for a moment, can be shown. And a move is
 *  flushed out of the cache only while the display isn't reading those rows
 *  (hdmi_beam_near), so the arrow isn't torn in half.                      */
#define PTR_MAX 64
static struct { int px, py, x, y, w, h, shown; } ptr;  /* where it is drawn now   */
static int ptr_want, ptr_want_x, ptr_want_y, ptr_pending;
static uint32_t spr_p[PTR_MAX * PTR_MAX], spr_t[PTR_MAX * PTR_MAX];
static int spr_w, spr_h;
static float spr_k;

static void mark(int x, int y, int w, int h, uint32_t gen) {
    if (w <= 0 || h <= 0) return;
    front_flush(x, y, w, h);
    for (int ty = y / SCR_TILE; ty <= (y + h - 1) / SCR_TILE; ty++)
        for (int tx = x / SCR_TILE; tx <= (x + w - 1) / SCR_TILE; tx++)
            g_tile_gen[ty * scr_tw + tx] = gen;
}

static void arrow(surface_t *s, float k) {
    gfx_bind(s, 0, 0, 0, 0, PTR_MAX, PTR_MAX);
    float fx = 2, fy = 2;
    static const float ax[] = { 0, 0, 4.2f, 7.0f, 9.2f, 6.6f, 11.5f }, ay[] = { 0, 16.5f, 12.6f, 18.4f, 17.4f, 11.6f, 11.6f };
    for (int i = 0; i < 7; i++) {
        int j = (i + 1) % 7;
        gfx_line(fx + ax[i] * k, fy + ay[i] * k, fx + ax[j] * k, fy + ay[j] * k, 2.6f * k, 0x000000, 160);
    }
    for (float r = 1; r < 11; r += 1.0f)
        gfx_line(fx + 1.2f * k, fy + (2.2f + r) * k, fx + (1.2f + r * 0.62f) * k, fy + (2.2f + r) * k, 1.2f * k, 0xFFFFFF, 255);
    for (int i = 0; i < 7; i++) {
        int j = (i + 1) % 7;
        gfx_line(fx + ax[i] * k, fy + ay[i] * k, fx + ax[j] * k, fy + ay[j] * k, 1.1f * k, 0xFFFFFF, 255);
    }
}

/* The sprite for this screen's size: bigger on screens taller than 600. */
static void spr_make(void) {
    float k = scr_h > 600 ? (float)scr_h / 600.0f : 1.0f;
    if (k == spr_k) return;
    spr_k = k;
    spr_w = (int)(16 * k) + 4; if (spr_w > PTR_MAX) spr_w = PTR_MAX;
    spr_h = (int)(23 * k) + 4; if (spr_h > PTR_MAX) spr_h = PTR_MAX;
    static uint32_t on_white[PTR_MAX * PTR_MAX];
    surface_t b = { spr_p, PTR_MAX, PTR_MAX, PTR_MAX }, w = { on_white, PTR_MAX, PTR_MAX, PTR_MAX };
    for (int i = 0; i < PTR_MAX * PTR_MAX; i++) { spr_p[i] = 0; on_white[i] = 0xFFFFFF; }
    arrow(&b, k);
    arrow(&w, k);
    gfx_bind(&g_back, 0, 0, 0, 0, scr_w, scr_h);
    for (int i = 0; i < PTR_MAX * PTR_MAX; i++) {
        uint32_t t = 0;
        for (int sh = 0; sh < 24; sh += 8) {
            int d = (int)((on_white[i] >> sh) & 255) - (int)((spr_p[i] >> sh) & 255);
            t |= (uint32_t)(d < 0 ? 0 : d) << sh;
        }
        spr_t[i] = t;
        spr_p[i] &= 0xFFFFFF;
    }
}

/* What the screen shows at a pixel the sprite covers: `under` is back's. */
static inline uint32_t spr_on(uint32_t under, int i) {
    uint32_t t = spr_t[i];
    if (t == 0xFFFFFF) return under;
    uint32_t p = spr_p[i];
    if (!t) return p;
    uint32_t o = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        uint32_t v = ((p >> sh) & 255) + (((under >> sh) & 255) * ((t >> sh) & 255) + 127) / 255;
        o |= (v > 255 ? 255 : v) << sh;
    }
    return o;
}

/* The arrow's rectangle with its tip at (px, py), cut to the screen. */
static void ptr_rect(int px, int py, int *x, int *y, int *w, int *h) {
    *x = px - 2; *y = py - 2; *w = spr_w; *h = spr_h;
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > scr_w) *w = scr_w - *x;
    if (*y + *h > scr_h) *h = scr_h - *y;
}

/* Front over a rectangle: back, with the arrow on top where it is drawn. */
static void ptr_paint(int x, int y, int w, int h) {
    for (int r = y; r < y + h; r++) {
        const uint32_t *s = g_back.px + (size_t)r * g_back.stride;
        uint32_t *d = g_front + (size_t)r * scr_w;
        int sy = r - (ptr.py - 2);
        for (int c = x; c < x + w; c++) {
            int sx = c - (ptr.px - 2);
            int in = ptr.shown && r >= ptr.y && r < ptr.y + ptr.h && c >= ptr.x && c < ptr.x + ptr.w;
            d[c] = in ? spr_on(s[c], sy * PTR_MAX + sx) : s[c];
        }
    }
}

/* Move the arrow to where it is wanted, once the display isn't reading the
 * rows it leaves or lands on. Called every turn of the main loop. */
void screen_pointer_poll(void) {
    if (!ptr_pending || !g_front) return;
    spr_make();
    int nx = 0, ny = 0, nw = 0, nh = 0;
    if (ptr_want) ptr_rect(ptr_want_x, ptr_want_y, &nx, &ny, &nw, &nh);
    int was = ptr.shown, ox = ptr.x, oy = ptr.y, ow = ptr.w, oh = ptr.h;
    int y0 = scr_h, y1 = -1;
    if (was && oh > 0) { y0 = oy; y1 = oy + oh - 1; }
    if (ptr_want && nh > 0) { if (ny < y0) y0 = ny; if (ny + nh - 1 > y1) y1 = ny + nh - 1; }
    if (y1 >= 0 && hdmi_beam_near(y0, y1)) return;     /* the next turn, a moment later */
    ptr_pending = 0;
    ptr.shown = ptr_want && nw > 0 && nh > 0;
    ptr.px = ptr_want_x; ptr.py = ptr_want_y; ptr.x = nx; ptr.y = ny; ptr.w = nw; ptr.h = nh;
    if (was) ptr_paint(ox, oy, ow, oh);
    if (ptr.shown) ptr_paint(ptr.x, ptr.y, ptr.w, ptr.h);
    uint32_t gen = g_front_gen + 1;
    if (was) mark(ox, oy, ow, oh, gen);
    if (ptr.shown) mark(ptr.x, ptr.y, ptr.w, ptr.h, gen);
    if (was || ptr.shown) g_front_gen = gen;
}

void screen_pointer(int x, int y, int shown) {
    ptr_want = shown && x >= 0 && y >= 0;
    ptr_want_x = x; ptr_want_y = y;
    if (!g_front) return;
    if (ptr.shown == ptr_want && (!ptr_want || (ptr.px == x && ptr.py == y))) { ptr_pending = 0; return; }
    ptr_pending = 1;
    screen_pointer_poll();
}

/* ---- the buffers ------------------------------------------------------------------------- *
 *  front starts on a page: the display engine scans it out of RAM on the Pi
 *  (hdmi.c), and while it does, what changed in it is cleaned out of the
 *  data cache after every present so the monitor sees it.                   */
static void *front_raw, *back_raw;
static int scanout;

static void front_flush(int x, int y, int w, int h) {
    if (!scanout || w <= 0 || h <= 0) return;
    for (int r = y; r < y + h; r++) {
        uintptr_t a = (uintptr_t)(g_front + (size_t)r * scr_w + x) & ~(uintptr_t)63;
        uintptr_t e = (uintptr_t)(g_front + (size_t)r * scr_w + x + w);
        if (x == 0 && w == scr_w) {                    /* whole rows: one run       */
            e = (uintptr_t)(g_front + (size_t)(y + h) * scr_w);
            r = y + h;
        }
        for (; a < e; a += 64) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

void screen_scanout(int on) {
    scanout = on;
    front_flush(0, 0, scr_w, scr_h);
}

/* New buffers for w x h. The old ones stay if there's no room for these. */
static int alloc_buffers(int w, int h) {
    uint64_t bytes = (uint64_t)w * h * 4;
    int tw = (w + SCR_TILE - 1) / SCR_TILE, th = (h + SCR_TILE - 1) / SCR_TILE;
    void *b = kmalloc(bytes);
    void *f = kmalloc(bytes + 4096);
    uint32_t *tg = (uint32_t *)kmalloc((uint64_t)tw * th * 4);
    if (!b || !f || !tg) {
        if (b) kfree(b);
        if (f) kfree(f);
        if (tg) kfree(tg);
        kprintf("[screen] no memory for %dx%d\n", w, h);
        return -1;
    }
    uint32_t *fa = (uint32_t *)(((uintptr_t)f + 4095) & ~(uintptr_t)4095);
    memset(b, 0, bytes);
    memset(fa, 0, bytes);
    if (back_raw) kfree(back_raw);
    if (front_raw) kfree(front_raw);
    if (g_tile_gen) kfree(g_tile_gen);
    back_raw = b; front_raw = f;
    g_back.px = (uint32_t *)b; g_back.w = w; g_back.h = h; g_back.stride = w;
    g_front = fa;
    scr_w = w; scr_h = h; scr_tw = tw; scr_th = th;
    /* Every tile is new to every viewer. */
    uint32_t gen = g_front_gen + 1;
    for (int i = 0; i < tw * th; i++) tg[i] = gen;
    g_tile_gen = tg;
    g_front_gen = gen;
    return 0;
}

int screen_init(int w, int h) {
    if (alloc_buffers(w, h) != 0) return -1;
    if (g_plat == PLAT_QEMU && qemu_ramfb_init(g_front, w, h) == 0)
        kprintf("[screen] %dx%d, shown in QEMU's window (ramfb)\n", w, h);
    else
        kprintf("[screen] %dx%d in RAM (see it over VNC or HDMI)\n", w, h);
    return 0;
}

/* A new size while running (Settings, or a monitor plugged in). Viewers
 * that know DesktopSize are told; the shell draws everything again. */
int screen_resize(int w, int h) {
    if (w == scr_w && h == scr_h) return 0;
    ptr.shown = 0;
    if (alloc_buffers(w, h) != 0) return -1;
    ptr_pending = ptr_want;                        /* the arrow, back on the new screen */
    if (g_plat == PLAT_QEMU) qemu_ramfb_init(g_front, w, h);
    kprintf("[screen] now %dx%d\n", w, h);
    return 0;
}

typedef uint64_t u64u __attribute__((aligned(1), may_alias));
typedef uint32_t v4u __attribute__((vector_size(16), aligned(4), may_alias));

/* Do the two runs of n pixels differ? Eight bytes at a time, no early exit
 * inside a run: runs are short and mostly equal. */
static inline int row_differs(const uint32_t *a, const uint32_t *b, int n) {
    if (n == 16) {                                 /* a whole tile's width: four q loads a side */
        const v4u *x = (const v4u *)a, *y = (const v4u *)b;
        v4u d = (x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3]);
        return (d[0] | d[1] | d[2] | d[3]) != 0;
    }
    uint64_t acc = 0;
    int i = 0;
    for (; i + 2 <= n; i += 2) acc |= *(const u64u *)(a + i) ^ *(const u64u *)(b + i);
    if (i < n) acc |= a[i] ^ b[i];
    return acc != 0;
}

void screen_present(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h) h = scr_h - y;
    if (w <= 0 || h <= 0) return;
    uint32_t gen = g_front_gen + 1;
    int changed = 0, cy0 = scr_h, cy1 = -1;
    /* Row by row, not tile by tile: rows are what the A53's prefetcher
     * follows, and a tile's rows are 4 KB apart. Only the 16-pixel pieces
     * of a row that differ are copied. */
    int tx0 = x / SCR_TILE, tx1 = (x + w - 1) / SCR_TILE;
    int px0 = tx0 * SCR_TILE, px1 = (tx1 + 1) * SCR_TILE < scr_w ? (tx1 + 1) * SCR_TILE : scr_w;
    for (int py = y / SCR_TILE * SCR_TILE; py < y + h; py++) {
        const uint32_t *s = g_back.px + (size_t)py * g_back.stride;
        uint32_t *d = g_front + (size_t)py * scr_w;
        uint32_t *tg = g_tile_gen + (py / SCR_TILE) * scr_tw;
        int arow = ptr.shown && py >= ptr.y && py < ptr.y + ptr.h;
        for (int px = px0; px < px1; px += SCR_TILE) {
            int pw = px1 - px < SCR_TILE ? px1 - px : SCR_TILE;
            const uint32_t *src = s + px;
            uint32_t mixed[SCR_TILE];
            if (arow && px < ptr.x + ptr.w && px + pw > ptr.x) {
                /* The arrow is in this piece: what front should hold is
                 * back with the arrow over it, never back alone. */
                int i0 = (py - (ptr.py - 2)) * PTR_MAX - (ptr.px - 2);
                for (int i = 0; i < pw; i++) {
                    int c = px + i;
                    mixed[i] = c >= ptr.x && c < ptr.x + ptr.w ? spr_on(src[i], i0 + c) : src[i];
                }
                src = mixed;
            }
            if (!row_differs(src, d + px, pw)) continue;
            memcpy(d + px, src, (size_t)pw * 4);
            tg[px / SCR_TILE] = gen;
            changed = 1;
            if (py < cy0) cy0 = py;
            cy1 = py;
        }
    }
    if (cy1 >= 0) front_flush(px0, cy0, px1 - px0, cy1 - cy0 + 1);
    if (changed) g_front_gen = gen;
}
