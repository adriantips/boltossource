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
 *  A USB mouse's arrow lives only in front, over the picture, with the
 *  pixels it covers saved under it: moving it touches a few tiles and never
 *  asks the shell for a frame.
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

static surface_t front_surf;

/* ---- the pointer ---------------------------------------------------------------------- */
#define PTR_MAX 64
static struct { int px, py, x, y, w, h, shown; } ptr;  /* where it is drawn now   */
static uint32_t ptr_under[PTR_MAX * PTR_MAX];
static int ptr_want, ptr_want_x, ptr_want_y;

static void mark(int x, int y, int w, int h, uint32_t gen) {
    if (w <= 0 || h <= 0) return;
    for (int ty = y / SCR_TILE; ty <= (y + h - 1) / SCR_TILE; ty++)
        for (int tx = x / SCR_TILE; tx <= (x + w - 1) / SCR_TILE; tx++)
            g_tile_gen[ty * scr_tw + tx] = gen;
}

static void ptr_erase(void) {
    if (!ptr.shown) return;
    for (int r = 0; r < ptr.h; r++)
        memcpy(g_front + (size_t)(ptr.y + r) * scr_w + ptr.x, ptr_under + r * PTR_MAX, (size_t)ptr.w * 4);
    ptr.shown = 0;
}

/* Save what is under the arrow at (px, py), then draw it. */
static void ptr_draw(int px, int py) {
    float k = scr_h > 600 ? (float)scr_h / 600.0f : 1.0f;
    int x = px - 2, y = py - 2, w = (int)(16 * k) + 4, h = (int)(23 * k) + 4;
    if (w > PTR_MAX) w = PTR_MAX;
    if (h > PTR_MAX) h = PTR_MAX;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h) h = scr_h - y;
    ptr.px = px; ptr.py = py; ptr.x = x; ptr.y = y; ptr.w = w; ptr.h = h;
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; r++)
        memcpy(ptr_under + r * PTR_MAX, g_front + (size_t)(y + r) * scr_w + x, (size_t)w * 4);
    ptr.shown = 1;
    gfx_bind(&front_surf, 0, 0, x, y, w, h);
    float fx = (float)px, fy = (float)py;
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
    gfx_bind(&g_back, 0, 0, 0, 0, scr_w, scr_h);
}

/* After front was rebuilt under it: draw the arrow where it is wanted, and
 * mark where it was and is if that moved. 1 if anything was marked. */
static int ptr_place(int was_shown, int ox, int oy, int ow, int oh, uint32_t gen) {
    if (ptr_want) ptr_draw(ptr_want_x, ptr_want_y);
    if (was_shown == ptr.shown && (!was_shown || (ox == ptr.x && oy == ptr.y && ow == ptr.w && oh == ptr.h))) return 0;
    if (was_shown) mark(ox, oy, ow, oh, gen);
    if (ptr.shown) mark(ptr.x, ptr.y, ptr.w, ptr.h, gen);
    return 1;
}

void screen_pointer(int x, int y, int shown) {
    ptr_want = shown && x >= 0 && y >= 0;
    ptr_want_x = x; ptr_want_y = y;
    if (!g_front) return;
    if (ptr.shown == ptr_want && (!ptr_want || (ptr.px == x && ptr.py == y))) return;
    int was = ptr.shown, ox = ptr.x, oy = ptr.y, ow = ptr.w, oh = ptr.h;
    ptr_erase();
    if (ptr_place(was, ox, oy, ow, oh, g_front_gen + 1)) g_front_gen++;
}

/* ---- the buffers ------------------------------------------------------------------------- */
int screen_init(int w, int h) {
    uint64_t bytes = (uint64_t)w * h * 4;
    uint32_t *b = (uint32_t *)kmalloc(bytes);
    uint32_t *f = (uint32_t *)kmalloc(bytes);
    int tw = (w + SCR_TILE - 1) / SCR_TILE, th = (h + SCR_TILE - 1) / SCR_TILE;
    uint32_t *tg = (uint32_t *)kmalloc((uint64_t)tw * th * 4);
    if (!b || !f || !tg) { kprintf("[screen] no memory for %dx%d\n", w, h); return -1; }
    memset(b, 0, bytes);
    memset(f, 0, bytes);
    memset(tg, 0, (size_t)tw * th * 4);
    g_back.px = b; g_back.w = w; g_back.h = h; g_back.stride = w;
    g_front = f;
    front_surf.px = f; front_surf.w = w; front_surf.h = h; front_surf.stride = w;
    g_tile_gen = tg;
    scr_w = w; scr_h = h; scr_tw = tw; scr_th = th;
    if (g_plat == PLAT_QEMU && qemu_ramfb_init(g_front, w, h) == 0)
        kprintf("[screen] %dx%d, shown in QEMU's window (ramfb)\n", w, h);
    else
        kprintf("[screen] %dx%d in RAM (no panel: see it over VNC)\n", w, h);
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
    int changed = 0;
    int was = ptr.shown, ox = ptr.x, oy = ptr.y, ow = ptr.w, oh = ptr.h;
    ptr_erase();                                   /* front is the last picture again */
    /* Row by row, not tile by tile: rows are what the A53's prefetcher
     * follows, and a tile's rows are 4 KB apart. Only the 16-pixel pieces
     * of a row that differ are copied. */
    int tx0 = x / SCR_TILE, tx1 = (x + w - 1) / SCR_TILE;
    int px0 = tx0 * SCR_TILE, px1 = (tx1 + 1) * SCR_TILE < scr_w ? (tx1 + 1) * SCR_TILE : scr_w;
    for (int py = y / SCR_TILE * SCR_TILE; py < y + h; py++) {
        const uint32_t *s = g_back.px + (size_t)py * g_back.stride;
        uint32_t *d = g_front + (size_t)py * scr_w;
        uint32_t *tg = g_tile_gen + (py / SCR_TILE) * scr_tw;
        for (int px = px0; px < px1; px += SCR_TILE) {
            int pw = px1 - px < SCR_TILE ? px1 - px : SCR_TILE;
            if (!row_differs(s + px, d + px, pw)) continue;
            memcpy(d + px, s + px, (size_t)pw * 4);
            tg[px / SCR_TILE] = gen;
            changed = 1;
        }
    }
    /* The arrow goes back on top. Tiles under it that changed are marked
     * already; the rest only if it moved. */
    if (ptr_place(was, ox, oy, ow, oh, gen)) changed = 1;
    if (changed) g_front_gen = gen;
}
