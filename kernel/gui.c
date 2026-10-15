/* ===========================================================================
 *  BoltOS  -  kernel/gui.c
 *
 *  The compositor and window manager: windows, focus and stacking, the four
 *  desktops, dragging, resizing and tiling, input routing, and the loop that
 *  turns all of it into frames.
 *
 *  Every window is rendered into a surface of its own and composited from
 *  there. That is what makes the animations possible -- a window opening
 *  grows out of its surface, a minimised one flies to the dock as a scaled
 *  copy of itself -- and it is also what makes the desktop cheap: moving a
 *  window no longer asks the app to draw anything, and hovering the dock no
 *  longer redraws the terminal behind it.
 *
 *  Frames are built from damage. Whatever changed says which rectangle of
 *  the screen and which layer; only those rectangles are recomposited and
 *  copied to the panel, and the pointer is drawn straight onto the panel on
 *  top, so moving the mouse costs two small copies and nothing else. Under
 *  QEMU's TCG -- where a full-screen copy is most of 100 ms -- that is the
 *  difference between a desktop and a slideshow.
 *
 *  The things drawn around the windows (dock, tray, Start, flyouts, menus,
 *  toasts, desktop icons) live in gui_shell.c; sign-in in gui_login.c.
 * ===========================================================================*/
#include <stdint.h>
#include <stdio.h>
#include "gui.h"
#include "gui_int.h"
#include "xhci.h"
#include "settings.h"
#include "framebuffer.h"
#include "console.h"
#include "keyboard.h"
#include "kprintf.h"
#include "mouse.h"
#include "kheap.h"
#include "pmm.h"
#include "mm.h"
#include "pit.h"
#include "hw.h"
#include "shell.h"
#include "string.h"
#include "fs.h"
#include "image.h"
#include "users.h"
#include "audio.h"
#include "notify.h"
#include "screenshot.h"
#include "record.h"
#include "keymap.h"
#include "timeout.h"

#define MAX_PW     3840            /* largest panel we allocate backbuffers for */
#define MAX_PH     2160            /* (4K) -- res changes reuse these buffers   */
#define RS_BORDER  6               /* window edge resize grab band, in px       */
enum { RS_L = 1, RS_R = 2, RS_T = 4, RS_B = 8 };

static int hires_ok;               /* big backbuffers allocated -> res switch allowed */

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iabs(int v) { return v < 0 ? -v : v; }
/* sqrtss. The procedural wallpaper used to count its way up to the root, a
 * thousand iterations a pixel at the far corner of a 4K panel, and two of
 * those per pixel kept the desktop off the screen for four seconds at boot. */
static int isqrt(int v) {
    float f = (float)v, r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(f));
    return (int)r;
}

/* ---- the screen ------------------------------------------------------------ */
static uint32_t *BB;            /* compositor backbuffer (alloc MAX, use W*H)   */
static uint32_t *BG;            /* the wallpaper, prerendered at W*H            */
static surface_t BBS;           /* BB as a surface                              */
static image_t  *wall_img;      /* user-chosen image wallpaper, or 0            */
static char      wall_img_path[FS_PATH_MAX];
static int       PW, PH;        /* physical panel size                          */
static int       W, H;          /* logical desktop size                         */
static int       out_x, out_y, out_w, out_h;

int  gui_screen_w(void) { return W; }
int  gui_screen_h(void) { return H; }
int  gui_panel_w(void)  { return PW; }
int  gui_panel_h(void)  { return PH; }
uint64_t gui_now(void)  { return pit_ticks(); }

static int cpu_load;
int gui_cpu_load(void) { return cpu_load; }

/* The pointer reports physical panel pixels; map it into the logical desktop so
 * every hit-test and the cursor stay aligned when the desktop is scaled. */
static int mlx(void) {
    int v = mouse_x() - out_x;
    if (out_w > 0) v = v * W / out_w;
    if (v < 0) v = 0;
    if (v >= W) v = W - 1;
    return v;
}
static int mly(void) {
    int v = mouse_y() - out_y;
    if (out_h > 0) v = v * H / out_h;
    if (v < 0) v = 0;
    if (v >= H) v = H - 1;
    return v;
}
int gui_mx(void) { return mlx(); }
int gui_my(void) { return mly(); }
int gui_mouse_x(void) { return mlx(); }
int gui_mouse_y(void) { return mly(); }

/* ===========================================================================
 *  Damage
 * ===========================================================================*/
#define MAX_DMG 32
typedef struct { int x, y, w, h, layer; } dmg_t;
static dmg_t dmg[MAX_DMG];
static int   ndmg;
static int   any_anim;            /* something animated this frame */

void gui_animating(void) { any_anim = 1; }

static int rect_hit(const dmg_t *a, int x, int y, int w, int h) {
    return a->x < x + w && x < a->x + a->w && a->y < y + h && y < a->y + a->h;
}

void gui_damage(int x, int y, int w, int h, int layer) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    /* absorbed by, or absorbing, one we already have */
    for (int i = 0; i < ndmg; i++) {
        dmg_t *d = &dmg[i];
        if (x >= d->x && y >= d->y && x + w <= d->x + d->w && y + h <= d->y + d->h) {
            if (layer < d->layer) d->layer = layer;
            return;
        }
        if (d->x >= x && d->y >= y && d->x + d->w <= x + w && d->y + d->h <= y + h) {
            d->x = x; d->y = y; d->w = w; d->h = h;
            if (layer < d->layer) d->layer = layer;
            return;
        }
    }
    if (ndmg == MAX_DMG) {
        /* full: fold everything into one box. Rare, and always correct. */
        int x0 = x, y0 = y, x1 = x + w, y1 = y + h, l = layer;
        for (int i = 0; i < ndmg; i++) {
            x0 = imin(x0, dmg[i].x); y0 = imin(y0, dmg[i].y);
            x1 = imax(x1, dmg[i].x + dmg[i].w); y1 = imax(y1, dmg[i].y + dmg[i].h);
            l = imin(l, dmg[i].layer);
        }
        ndmg = 1;
        dmg[0].x = x0; dmg[0].y = y0; dmg[0].w = x1 - x0; dmg[0].h = y1 - y0; dmg[0].layer = l;
        return;
    }
    dmg[ndmg].x = x; dmg[ndmg].y = y; dmg[ndmg].w = w; dmg[ndmg].h = h; dmg[ndmg].layer = layer;
    ndmg++;
}
void gui_damage_all(void) { gui_damage(0, 0, W, H, L_DESK); }

/* Overlapping rectangles become their union when that is not much bigger
 * than the two were -- the classic damage-region tradeoff, kept simple. */
static void dmg_coalesce(void) {
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < ndmg && !changed; i++)
            for (int j = i + 1; j < ndmg; j++) {
                dmg_t *a = &dmg[i], *b = &dmg[j];
                int x0 = imin(a->x, b->x), y0 = imin(a->y, b->y);
                int x1 = imax(a->x + a->w, b->x + b->w), y1 = imax(a->y + a->h, b->y + b->h);
                int64_t ua = (int64_t)(x1 - x0) * (y1 - y0);
                int64_t sa = (int64_t)a->w * a->h + (int64_t)b->w * b->h;
                int touch = rect_hit(a, b->x - 1, b->y - 1, b->w + 2, b->h + 2);
                if (touch && ua <= sa + sa / 3) {
                    a->x = x0; a->y = y0; a->w = x1 - x0; a->h = y1 - y0;
                    a->layer = imin(a->layer, b->layer);
                    dmg[j] = dmg[--ndmg];
                    changed = 1;
                    break;
                }
            }
    }
}

/* Is any damage below `layer` touching this rectangle? (Glass invalidation.) */
int gui_damage_under(int x, int y, int w, int h, int layer) {
    for (int i = 0; i < ndmg; i++)
        if (dmg[i].layer < layer && rect_hit(&dmg[i], x, y, w, h)) return 1;
    return 0;
}

/* ===========================================================================
 *  Desktop wallpaper
 * ===========================================================================*/
static uint32_t add_clamp(uint32_t c, int dr, int dg, int db) {
    int r = (int)(c >> 16 & 0xFF) + dr, g = (int)(c >> 8 & 0xFF) + dg, b = (int)(c & 0xFF) + db;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return (uint32_t)(r << 16 | g << 8 | b);
}

/* Render the desktop background per the current wallpaper style + base colour.
 * dst is a packed W*H image. */
static void render_wallpaper(uint32_t *dst) {
    if (wall_img && wall_img->px && wall_img->w > 0 && wall_img->h > 0) {
        int iw = wall_img->w, ih = wall_img->h;        /* stretch image to fill */
        for (int y = 0; y < H; y++) {
            const uint32_t *srow = &wall_img->px[(y * ih / H) * iw];
            if (iw == W) { for (int x = 0; x < W; x++) dst[x] = srow[x] & 0x00FFFFFF; }
            else for (int x = 0; x < W; x++) dst[x] = srow[x * iw / W] & 0x00FFFFFF;
            dst += W;
        }
        return;
    }
    /* The procedural styles: a deep field with two soft lights in it. They are
     * the fallback when no image is set, so they had better look deliberate. */
    uint32_t base   = g_settings.wall_color;
    uint32_t bottom = add_clamp(base, 10, 16, 30);
    uint32_t glow   = add_clamp(base, 40, 58, 104);
    uint32_t glow2  = gfx_lerp(base, g_theme.accent, 45, 100);
    uint32_t grid   = gfx_lerp(base, g_theme.accent, 22, 100);
    int gx = W / 3, gy = H / 4, gr = (W > H ? W : H) * 2 / 3;
    int g2x = W * 4 / 5, g2y = H * 4 / 5, g2r = (W > H ? W : H) / 2;
    int style = g_settings.wall_style;
    for (int y = 0; y < H; y++) {
        uint32_t grad = gfx_lerp(base, bottom, y, H);
        for (int x = 0; x < W; x++) {
            uint32_t c;
            if (style == WALL_SOLID) {
                c = base;
            } else if (style == WALL_GRID) {
                c = ((x % 32) == 0 || (y % 32) == 0) ? grid : grad;
            } else {
                int dx = x - gx, dy = y - gy;
                int dist = isqrt(dx * dx + dy * dy);
                uint32_t fld = (style == WALL_GLOW) ? base : grad;
                c = (dist < gr) ? gfx_lerp(glow, fld, dist, gr) : fld;
                int ex = x - g2x, ey = y - g2y;
                int d2 = isqrt(ex * ex + ey * ey);
                if (d2 < g2r) c = gfx_lerp(gfx_lerp(c, glow2, 55, 100), c, d2, g2r);
            }
            dst[x] = c;
        }
        dst += W;
    }
}

#define WALL_PREF "/home/.wallpaper"   /* remembers the image path across reboots */
/* Until the glass desktop, the boot restore wrote the default back to the
 * preference file on every boot, so every machine that ever booted "chose"
 * the mountain. A preference naming the old default set is therefore not a
 * choice anybody made, and is not honoured over the new default. One the
 * user makes in Settings from now on goes through the same file, so picking
 * the mountain deliberately still sticks: it is only the *boot* restore that
 * no longer writes anything back. */
#define WALL_DIR  "/usr/share/wallpapers"
#define WALL_DEF  "aurora"              /* the default set kernel/main.c seeds */

static int wall_version;
int gui_wallpaper_version(void) { return wall_version; }

static void wallpaper_changed(void) {
    if (BG) render_wallpaper(BG);
    wall_version++;
    gui_damage_all();
    shell_theme_changed();
}

/* Load `path` as the wallpaper. `announce` is off for the loads the user did
 * not ask for -- the boot restore, and the swap that follows a resolution
 * change -- because a toast for something nobody clicked is just noise. */
static void wallpaper_load(const char *path, int announce) {
    if (wall_img) { image_free(wall_img); wall_img = 0; }
    wall_img_path[0] = 0;
    if (path && path[0]) {
        fs_node *n = fs_lookup(path);
        if (n && !n->is_dir && n->data && n->size) {
            /* Decode at panel size, not the browser's 640px: these renders are
             * cut for this exact mode and shrinking one here would undo that. */
            int fit = W > H ? W : H;
            wall_img = image_decode_upto(n->data, n->size, fit);
            if (wall_img) {
                strncpy(wall_img_path, path, sizeof(wall_img_path) - 1);
                wall_img_path[sizeof(wall_img_path) - 1] = 0;
            }
        }
    }
    if (announce) {                     /* a choice: remember it */
        fs_node *w = fs_lookup(WALL_PREF);
        if (!w) w = fs_create(WALL_PREF, 0);
        if (w) fs_write(w, wall_img_path, (uint32_t)strlen(wall_img_path));
        /* and mark that it was one, so the migration below leaves it alone */
        fs_node *m = fs_lookup(WALL_PREF ".chosen");
        if (!m) m = fs_create(WALL_PREF ".chosen", 0);
        if (m) fs_write(m, "1", 1);
    }
    wallpaper_changed();
    if (announce && wall_img_path[0]) gui_toast("Wallpaper updated");
}

void gui_set_wallpaper_image(const char *path) { wallpaper_load(path, 1); }
void gui_clear_wallpaper_image(void) { wallpaper_load(0, 1); }
void gui_set_wallpaper_style(void) { wallpaper_changed(); }
uint32_t *gui_wallpaper_px(void) { return BG; }
int  gui_wallpaper_is_image(void) { return wall_img_path[0] != 0; }
const char *gui_wallpaper_path(void) { return wall_img_path; }

/* Build WALL_DIR/<stem>-<W>x<H>.jpg. A wallpaper shipped as a set has one file
 * per mode in the resolution table, and the name is how they are tied together. */
static void wallpaper_variant(char *dst, uint32_t cap, const char *stem, int w, int h) {
    snprintf(dst, cap, "%s/%s-%dx%d.jpg", WALL_DIR, stem, w, h);
}

/* If the live wallpaper came from a per-resolution set, pull out its stem --
 * "/usr/share/wallpapers/mountain-1920x1080.jpg" -> "mountain". Returns 0 when
 * the current wallpaper is a one-off the user picked themselves, which must be
 * left exactly where it is. */
static int wallpaper_stem(char *dst, uint32_t cap) {
    const char *p = wall_img_path;
    uint32_t dlen = (uint32_t)strlen(WALL_DIR "/");
    if (strncmp(p, WALL_DIR "/", dlen) != 0) return 0;
    p += dlen;
    const char *dash = 0;
    for (const char *q = p; *q; q++) if (*q == '-') dash = q;   /* last dash */
    if (!dash || dash == p) return 0;
    uint32_t n = (uint32_t)(dash - p);
    if (n >= cap) return 0;
    memcpy(dst, p, n); dst[n] = 0;
    return 1;
}

/* After a mode change, swap to the render cut for the new panel. The compositor
 * stretches whatever it is given with a nearest-neighbour sampler, and that is
 * brutal on a photograph -- 1024x768 blown up to 4K is a mess of stair-stepped
 * ridgelines. Swapping the file keeps the stretch loop a straight 1:1 copy. */
static void wallpaper_follow_res(void) {
    char stem[64], path[FS_PATH_MAX];
    if (!wall_img_path[0] || !wallpaper_stem(stem, sizeof(stem))) return;
    wallpaper_variant(path, sizeof(path), stem, W, H);
    if (strcmp(path, wall_img_path) == 0) return;      /* already the right one */
    fs_node *n = fs_lookup(path);
    if (!n || n->is_dir || !n->data || !n->size) return;   /* no sibling: keep */
    wallpaper_load(path, 0);
}

/* On boot, restore a previously chosen image wallpaper. A machine that has
 * never had one gets the shipped default at whatever mode it came up in --
 * BoltOS renders one per resolution, so there is always a match. */
static void wallpaper_restore(void) {
    char path[FS_PATH_MAX];
    fs_node *n = fs_lookup(WALL_PREF);
    if (n && !n->is_dir && n->data && n->size) {
        uint32_t len = n->size; if (len > sizeof(path) - 1) len = sizeof(path) - 1;
        memcpy(path, n->data, len); path[len] = 0;
        /* An empty pref file is a deliberate "no image" -- the user cleared it
         * in Settings -- and must not be second-guessed back to the default. */
        int chosen = fs_lookup(WALL_PREF ".chosen") != 0;
        int olddef = strncmp(path, "/usr/share/wallpapers/mountain-", 31) == 0;
        if (chosen || !olddef) {
            if (path[0]) { wallpaper_load(path, 0); wallpaper_follow_res(); }
            return;
        }
    }
    wallpaper_variant(path, sizeof(path), WALL_DEF, W, H);
    if (fs_lookup(path)) wallpaper_load(path, 0);
}

/* ===========================================================================
 *  Window registry + stacking
 * ===========================================================================*/
window_t wm_wins[MAX_WIN];
int      wm_nwin;
int      wm_ztop;
int      wm_focus = -1;
int      wm_desk;
int      wm_drag = -1;
static int drag_dx, drag_dy;
static int snap_hint;
static int title_last_id = -1; static uint64_t title_last_tick;
static int resize_id = -1, resize_edge;
static int rs_x0, rs_y0, rs_w0, rs_h0, rs_mx, rs_my;
static int client_drag_id = -1;
static uint8_t prev_btns;

#define wins wm_wins
#define nwin wm_nwin

int gui_snap_hint(void) { return snap_hint; }

/* gui_focus sits above the surface bookkeeping in this file; it reaches the
 * title-bar invalidation through this pointer, set once the state exists. */
static void (*ws_stale_title)(int i);
static void ws_reset_hot(int i);

window_t *gui_add_window(const char *title, int w, int h, uint32_t accent, int icon) {
    if (nwin >= MAX_WIN) return 0;
    window_t *win = &wins[nwin];
    memset(win, 0, sizeof(*win));
    strncpy(win->title, title, sizeof(win->title) - 1);
    win->w = w; win->h = h;
    win->min_w = 260; win->min_h = 160;
    win->accent = accent; win->icon = icon;
    /* Cascade initial placement so windows don't perfectly overlap -- and wrap
     * it back to the top left when it runs off the panel. Forty-odd apps at a
     * step of 48 by 40 put everything after the fifteenth off the side of a
     * 1024x768 screen, where clicking its launcher appeared to do nothing. */
    win->x = 80 + nwin * 48;
    win->y = 50 + nwin * 40;
    if (W > 0 && win->x + w > W) win->x = 40 + (win->x % 200);
    if (H > 0 && win->y + h > H - SHELF_H) win->y = 30 + (win->y % 140);
    win->z = ++wm_ztop;
    ws_reset_hot(nwin);
    nwin++;
    return win;
}

int wm_index(const window_t *w) {
    for (int i = 0; i < nwin; i++) if (&wins[i] == w) return i;
    return -1;
}

/* ---- virtual desktops ------------------------------------------------------ *
 *  Four of them. A window belongs to the desktop it was opened on and is
 *  invisible (and unclickable) from the others; closed-but-pinned dock
 *  launchers appear on all of them.                                           */
int wm_on_desk(const window_t *w) { return w->desk == wm_desk; }
#define on_desk wm_on_desk

/* The slide between desktops: windows of the old one leave to one side while
 * the new one's come in from the other. */
static int      desk_from = -1, desk_dir;
static uint64_t desk_t0;
#define DESK_MS 320

/* ---- reduce motion ---------------------------------------------------------- */
int gui_motion_off;
static int copy_us;             /* one 1024x768 frame copied, measured at boot */
static int blend_us;            /* a 128x128 alpha fill, the same            */

/* Time a frame-sized copy and a small alpha blend: the cheapest thing a
 * frame is made of, and the thing most of one is made of. Best of three,
 * because the first can land on a host that is busy doing something else.
 *
 * Measured here, on the same host, at this point in boot:
 *
 *                     copy 1024x768    blend 128x128
 *     QEMU, TCG           7.3 ms           0.7 ms
 *     QEMU, WHPX          0.3 ms          <0.05 ms
 *
 * `display bench` gets four or five times the TCG figures for the same work
 * and I have not pinned down why; the thresholds are set against these,
 * since these are what this function sees. A window fade at TCG speed is
 * three or four frames of a half-transparent window, so Auto turns it off. */
static void motion_calibrate(void) {
    if (!BB || !BG) return;
    uint64_t best = ~0ull, bbest = ~0ull;
    for (int k = 0; k < 3; k++) {
        uint64_t t = now_us();
        for (int y = 0; y < 256; y++) gfx_memcpy32(BB + y * 1024, BG + y * 1024, 1024);
        t = now_us() - t;
        if (t < best) best = t;
        t = now_us();
        gfx_fill_a(0, 0, 128, 128, 0x808080, 128);
        t = now_us() - t;
        if (t < bbest) bbest = t;
    }
    copy_us = (int)(best * 3);
    blend_us = (int)bbest;
}

#define MOTION_SLOW_COPY_US  2500    /* WHPX 300, TCG 7300 */
#define MOTION_SLOW_BLEND_US  250    /* WHPX <50, TCG  700 */
void gui_motion_apply(void) {
    int m = g_settings.motion;
    int slow = copy_us > MOTION_SLOW_COPY_US || blend_us > MOTION_SLOW_BLEND_US;
    int off = m == MOTION_REDUCED || (m == MOTION_AUTO && slow);
    if (off != gui_motion_off) { gui_motion_off = off; gui_damage_all(); }
}
int gui_motion_reduced(void) { return gui_motion_off; }
int gui_frame_copy_us(void)  { return copy_us; }
int gui_blend_us(void)       { return blend_us; }

int gui_window_focused(window_t *win) { return wm_focus >= 0 && &wins[wm_focus] == win; }

/* topmost open, non-minimised window index on this desktop, or -1 */
int wm_topmost(void) {
    int best = -1, bz = -1;
    for (int i = 0; i < nwin; i++)
        if (wins[i].open && !wins[i].minimized && on_desk(&wins[i]) && wins[i].z > bz)
            { bz = wins[i].z; best = i; }
    return best;
}

void gui_focus(window_t *win) {
    int i = wm_index(win);
    if (i < 0) return;
    int old = wm_focus;
    win->z = ++wm_ztop;
    wm_focus = i;
    if (old != i) {
        if (old >= 0 && ws_stale_title) ws_stale_title(old);
        if (ws_stale_title) ws_stale_title(i);
    }
    gui_damage(win->x, win->y, win->w, win->h, L_WIN);
}

/* Show a window without taking the keyboard from whatever has it. The `media`
 * command opens the player from a Terminal you are still typing in, and a
 * window that takes the caret out from under a shell prompt eats your next
 * command -- in the player's case, reading the space in it as Pause.
 *
 * Which is also why this does not raise the window: keys go to the topmost
 * window here, so raising one *is* focusing it. */
void gui_show_unfocused(window_t *win) {
    if (!win) return;
    if (!win->open) { win->desk = wm_desk; win->open = 1; }
    win->minimized = 0;
    int i = wm_index(win);
    if (i >= 0) gui_window_stale(i);
}

void wm_desk_goto(int d) {
    if (d < 0 || d >= NDESK || d == wm_desk) return;
    desk_from = wm_desk;
    desk_dir = d > wm_desk ? 1 : -1;
    desk_t0 = gui_now();
    wm_desk = d;
    /* Focus follows: whatever was frontmost over there is frontmost again. */
    wm_focus = -1;
    int best = -1, bz = -1;
    for (int i = 0; i < nwin; i++)
        if (wins[i].open && !wins[i].minimized && on_desk(&wins[i]) && wins[i].z > bz)
            { bz = wins[i].z; best = i; }
    if (best >= 0) wm_focus = best;
    gui_all_stale();
    gui_damage_all();
}

void gui_open(window_t *win) {
    /* Already open somewhere else? Go to it rather than dragging it here: the
     * window is where the user left it, and they asked to see it, not move it. */
    if (win->open && win->desk != wm_desk) wm_desk_goto(win->desk);
    else if (!win->open) win->desk = wm_desk;
    int was_open = win->open;
    win->open = 1; win->minimized = 0;
    gui_focus(win);
    if (!was_open) shell_launched(wm_index(win));
}

/* Send the focused window to another desktop and follow it -- following is the
 * point, otherwise you have thrown a window somewhere you cannot see. */
static void desk_send(int d) {
    if (d < 0 || d >= NDESK || d == wm_desk) return;
    int t = wm_focus;
    if (t < 0 || !wins[t].open || !on_desk(&wins[t])) return;
    wins[t].desk = d;
    wm_desk_goto(d);
    gui_focus(&wins[t]);
}

/* Reachable from the shell (`desk`), so the desktops can be driven from a
 * terminal, over SSH, and by a test that cannot press Win+2. */
int gui_desk_count(void)   { return NDESK; }
int gui_desk_current(void) { return wm_desk; }

int gui_desk_goto(int d) {
    if (d < 0 || d >= NDESK) return -1;
    wm_desk_goto(d);
    return 0;
}

int gui_desk_windows(int d, char *out, uint32_t cap) {
    uint32_t n = 0; int count = 0;
    if (cap) out[0] = 0;
    for (int i = 0; i < nwin; i++) {
        if (!wins[i].open || wins[i].desk != d) continue;
        count++;
        if (n && n + 2 < cap) { out[n++] = ','; out[n++] = ' '; }
        for (const char *p = wins[i].title; *p && n + 1 < cap; p++) out[n++] = *p;
        if (n < cap) out[n] = 0;
    }
    return count;
}

int gui_desk_send(int d) {
    if (d < 0 || d >= NDESK || d == wm_desk) return -1;
    int t = wm_focus;
    if (t < 0 || !wins[t].open || !on_desk(&wins[t])) return -2;
    desk_send(d);
    return 0;
}

void wm_close(int i) {
    if (i < 0 || i >= nwin) return;
    wins[i].open = 0;
    if (wm_focus == i) {
        wm_focus = -1;
        int t = wm_topmost();
        if (t >= 0) gui_focus(&wins[t]);
    }
}

void wm_minimize(int i) {
    if (i < 0 || i >= nwin) return;
    wins[i].minimized = 1;
    if (wm_focus == i) {
        wm_focus = -1;
        int t = wm_topmost();
        if (t >= 0) gui_focus(&wins[t]);
    }
}

void wm_toggle_from_dock(int i) {
    window_t *w = &wins[i];
    int top = wm_topmost();
    if (!w->open)            gui_open(w);
    else if (w->desk != wm_desk) { wm_desk_goto(w->desk); w->minimized = 0; gui_focus(w); }
    else if (w->minimized) { w->minimized = 0; gui_focus(w); }
    else if (i == top)       wm_minimize(i);          /* focused -> minimise */
    else                     gui_focus(w);
}

/* Win+D: minimise every open window; press again to restore the ones we hid.
 * Per desktop, so showing the desktop on one does not disturb another. */
static void gui_show_desktop(void) {
    static uint8_t hidden[NDESK][MAX_WIN];
    uint8_t *hid = hidden[wm_desk];
    int any_visible = 0;
    for (int i = 0; i < nwin; i++)
        if (wins[i].open && !wins[i].minimized && on_desk(&wins[i])) { any_visible = 1; break; }
    if (any_visible) {
        for (int i = 0; i < nwin; i++) {
            hid[i] = (wins[i].open && !wins[i].minimized && on_desk(&wins[i])) ? 1 : 0;
            if (hid[i]) wins[i].minimized = 1;
        }
        wm_focus = -1;
    } else {
        for (int i = 0; i < nwin; i++)
            if (hid[i] && wins[i].open) { wins[i].minimized = 0; hid[i] = 0; }
        int t = wm_topmost();
        if (t >= 0) gui_focus(&wins[t]);
    }
}

/* ===========================================================================
 *  Window surfaces and animation
 * ===========================================================================*/
enum { WA_NONE = 0, WA_OPEN, WA_CLOSE, WA_MIN, WA_RESTORE, WA_GEOM };
typedef struct {
    surface_t s;
    int       cap;                 /* pixels allocated                         */
    int       stale;               /* 0 fresh, 1 title bar only, 2 everything  */
    int       open_prev, min_prev, focus_prev, desk_prev;
    int       gx, gy, gw, gh;      /* geometry as of last frame                */
    int       anim;
    uint64_t  t0;
    int       dur;
    rect_t    from, to;
    rect_t    foot;                /* screen footprint drawn last frame         */
    int       foot_ok;
    int       hot;                 /* traffic light under the pointer           */
} wstate_t;
static wstate_t ws[MAX_WIN];

void gui_window_stale(int i) {
    if (i < 0 || i >= nwin) return;
    ws[i].stale = 2;
}
/* 0 would mean "over the close button" */
static void ws_reset_hot(int i) { if (i >= 0 && i < MAX_WIN) ws[i].hot = -1; }
static void stale_title(int i) {
    if (i >= 0 && i < nwin && ws[i].stale < 1) ws[i].stale = 1;
}
void gui_all_stale(void) {
    for (int i = 0; i < nwin; i++) if (wins[i].open) ws[i].stale = 2;
}
void gui_request_redraw(void) {
    for (int i = 0; i < nwin; i++)
        if (wins[i].open && !wins[i].minimized && on_desk(&wins[i])) ws[i].stale = 2;
}
void gui_redraw_window(window_t *win) {
    int i = wm_index(win);
    if (i >= 0) ws[i].stale = 2;
}

const surface_t *wm_surface(int i) {
    if (i < 0 || i >= nwin || !ws[i].s.px) return 0;
    return &ws[i].s;
}

static int surface_fit(wstate_t *s, int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    if (w * h > s->cap) {
        if (s->s.px) kfree(s->s.px);
        int cap = w * h + (w * h) / 4;
        s->s.px = (uint32_t *)kmalloc((uint64_t)cap * 4);
        if (!s->s.px) {
            s->s.px = (uint32_t *)kmalloc((uint64_t)w * h * 4);
            cap = s->s.px ? w * h : 0;
        }
        s->cap = cap;
        if (!s->s.px) { s->cap = 0; return 0; }
    }
    s->s.w = w; s->s.h = h; s->s.stride = w;
    return 1;
}

static void surface_drop(wstate_t *s) {
    if (s->s.px) kfree(s->s.px);
    s->s.px = 0; s->cap = 0; s->s.w = s->s.h = 0;
}

static int win_tiled(const window_t *w) { return w->maximized || w->snapped; }

/* Draw window i into its surface: the chrome, then the app's client area,
 * then the edge highlight over both. `full` 0 redraws only the title bar --
 * the traffic lights lighting up should not cost the app a redraw. */
static void render_window(int i, int full) {
    window_t *w = &wins[i];
    wstate_t *s = &ws[i];
    if (!surface_fit(s, w->w, w->h)) { s->stale = 0; return; }
    if (s->s.w != w->w || s->s.h != w->h) full = 1;
    int focused = (i == wm_focus);
    gfx_bind(&s->s, w->x, w->y, w->x, w->y, w->w, full ? w->h : TITLE_H);
    chrome_draw(w, focused, s->hot, win_tiled(w));
    if (full) {
        int cx = w->x, cy = w->y + TITLE_H, cw = w->w, ch = w->h - TITLE_H;
        gfx_bind(&s->s, w->x, w->y, cx, cy, cw, ch);
        gfx_fill(cx, cy, cw, ch, COL_PANEL);
        if (w->draw) w->draw(w, cx, cy, cw, ch);
    }
    gfx_bind(&s->s, w->x, w->y, w->x, w->y, w->w, full ? w->h : TITLE_H);
    chrome_edge(w, win_tiled(w));
    s->stale = 0;
}

/* Where window i is drawn this frame -- its own rect, or wherever its
 * animation has it -- and how opaque. 0: not drawn at all. */
int wm_visual(int i, rect_t *r, int *alpha) {
    window_t *w = &wins[i];
    wstate_t *s = &ws[i];
    uint64_t now = gui_now();
    rect_t wr = { w->x, w->y, w->w, w->h };
    int a = 255;
    int shown = w->open && !w->minimized;
    int deskok = on_desk(w);
    int dx = 0;
    if (desk_from >= 0) {
        float t = ease_inout(anim_t(now, desk_t0, DESK_MS));
        if (w->desk == desk_from) { deskok = 1; dx = (int)(-desk_dir * W * t); }
        else if (w->desk == wm_desk) dx = (int)(desk_dir * W * (1.0f - t));
    }
    if (s->anim != WA_NONE) {
        float p = anim_t(now, s->t0, s->dur);
        switch (s->anim) {
        case WA_OPEN: {
            float t = ease_out(p), sc = 0.90f + 0.10f * t;
            int nw = (int)(wr.w * sc), nh = (int)(wr.h * sc);
            r->x = wr.x + (wr.w - nw) / 2; r->y = wr.y + (wr.h - nh) / 2 + (int)((1.0f - t) * 14.0f);
            r->w = nw; r->h = nh; a = (int)(255.0f * clamp01(p * 1.6f));
            break;
        }
        case WA_CLOSE: {
            float t = ease_out(p), sc = 1.0f - 0.07f * t;
            int nw = (int)(wr.w * sc), nh = (int)(wr.h * sc);
            r->x = wr.x + (wr.w - nw) / 2; r->y = wr.y + (wr.h - nh) / 2;
            r->w = nw; r->h = nh; a = (int)(255.0f * (1.0f - t));
            break;
        }
        case WA_MIN: case WA_RESTORE: {
            float t = ease_inout(p);
            if (s->anim == WA_RESTORE) t = 1.0f - t;
            rect_t d = s->to;
            r->x = wr.x + (int)((d.x - wr.x) * t); r->y = wr.y + (int)((d.y - wr.y) * t);
            r->w = wr.w + (int)((d.w - wr.w) * t); r->h = wr.h + (int)((d.h - wr.h) * t);
            a = (int)(255.0f * (1.0f - t * 0.85f));
            break;
        }
        case WA_GEOM: {
            float t = ease_out(p);
            r->x = s->from.x + (int)((wr.x - s->from.x) * t); r->y = s->from.y + (int)((wr.y - s->from.y) * t);
            r->w = s->from.w + (int)((wr.w - s->from.w) * t); r->h = s->from.h + (int)((wr.h - s->from.h) * t);
            break;
        }
        }
        if (r->w < 8) r->w = 8;
        if (r->h < 8) r->h = 8;
        r->x += dx;
        *alpha = a < 0 ? 0 : a;
        return deskok || s->anim == WA_CLOSE;
    }
    if (!shown || !deskok) return 0;
    *r = wr;
    r->x += dx;
    *alpha = 255;
    return 1;
}

#define SH_EXT 64       /* how far a window's shadow reaches past its edges */
static void foot_of(const rect_t *r, rect_t *f) {
    f->x = r->x - SH_EXT; f->y = r->y - SH_EXT + 8;
    f->w = r->w + 2 * SH_EXT; f->h = r->h + 2 * SH_EXT + 8;
}

/* Watch every window for the transitions the animations hang off: opened,
 * closed, minimised, restored, moved by something other than a drag. Doing
 * it by comparing with last frame means none of the forty places that set
 * win->open or win->minimized had to learn about animation. */
static void wm_track(uint64_t now) {
    for (int i = 0; i < nwin; i++) {
        window_t *w = &wins[i];
        wstate_t *s = &ws[i];
        int visible_desk = on_desk(w);
        if (w->open && !s->open_prev) {
            if (visible_desk && !w->minimized) {
                s->anim = WA_OPEN; s->t0 = now; s->dur = motion_ms(230);
            }
            s->stale = 2;
        } else if (!w->open && s->open_prev) {
            if (visible_desk && !s->min_prev && s->s.px) {
                s->anim = WA_CLOSE; s->t0 = now; s->dur = motion_ms(170);
            }
        } else if (w->open && w->minimized && !s->min_prev) {
            if (visible_desk && s->s.px) {
                rect_t d;
                shell_dock_rect(i, &d);
                s->anim = WA_MIN; s->t0 = now; s->dur = motion_ms(300); s->to = d;
            }
        } else if (w->open && !w->minimized && s->min_prev) {
            if (visible_desk) {
                rect_t d;
                shell_dock_rect(i, &d);
                s->anim = WA_RESTORE; s->t0 = now; s->dur = motion_ms(300); s->to = d;
            }
            s->stale = 2;
        } else if (w->open && !w->minimized &&
                   (w->x != s->gx || w->y != s->gy || w->w != s->gw || w->h != s->gh)) {
            int direct = (i == wm_drag || i == resize_id);
            if (w->w != s->gw || w->h != s->gh) s->stale = 2;
            if (!direct && visible_desk && s->anim == WA_NONE && s->gw > 0) {
                s->anim = WA_GEOM; s->t0 = now; s->dur = motion_ms(220);
                s->from.x = s->gx; s->from.y = s->gy; s->from.w = s->gw; s->from.h = s->gh;
            }
        }
        int focused = (i == wm_focus);
        if (focused != s->focus_prev && s->stale < 1) s->stale = 1;
        s->open_prev = w->open; s->min_prev = w->minimized; s->focus_prev = focused;
        s->gx = w->x; s->gy = w->y; s->gw = w->w; s->gh = w->h;
        if (s->anim != WA_NONE) {
            any_anim = 1;
            if (now >= s->t0 + (uint64_t)s->dur) {
                if (s->anim == WA_CLOSE) surface_drop(s);
                s->anim = WA_NONE;
            }
        }
        /* damage: where it was drawn last frame and where it is now */
        rect_t r; int a;
        int vis = wm_visual(i, &r, &a);
        rect_t f;
        if (vis) foot_of(&r, &f);
        int moved = vis != s->foot_ok || (vis && (f.x != s->foot.x || f.y != s->foot.y ||
                                                  f.w != s->foot.w || f.h != s->foot.h));
        if (moved || (vis && s->anim != WA_NONE)) {
            if (s->foot_ok) gui_damage(s->foot.x, s->foot.y, s->foot.w, s->foot.h, L_WIN);
            if (vis) gui_damage(f.x, f.y, f.w, f.h, L_WIN);
        } else if (vis && s->stale) {
            gui_damage(r.x, r.y, r.w, s->stale == 1 ? TITLE_H : r.h, L_WIN);
        }
        s->foot_ok = vis;
        if (vis) s->foot = f;
        (void)a;
    }
    if (desk_from >= 0) {
        any_anim = 1;
        if (now >= desk_t0 + (uint64_t)motion_ms(DESK_MS)) { desk_from = -1; gui_damage_all(); }
    }
}

/* ===========================================================================
 *  Tiling: halves, quarters and maximise, from the keyboard or a drag to an
 *  edge. One rect function serves all of it, so the preview you see while
 *  dragging is computed by the same code that places the window on release.
 * ===========================================================================*/
static void snap_rect(int zone, int *rx, int *ry, int *rw, int *rh) {
    int avail = H - SHELF_H;
    int hw = W / 2, hh = avail / 2;
    int x = 0, y = 0, w = W, h = avail;
    switch (zone) {
    case SNAP_L:  w = hw; break;
    case SNAP_R:  x = hw; w = W - hw; break;
    case SNAP_TL: w = hw; h = hh; break;
    case SNAP_TR: x = hw; w = W - hw; h = hh; break;
    case SNAP_BL: w = hw; y = hh; h = avail - hh; break;
    case SNAP_BR: x = hw; w = W - hw; y = hh; h = avail - hh; break;
    default: break;                                    /* SNAP_MAX: all of it */
    }
    *rx = x; *ry = y; *rw = w; *rh = h;
}
void gui_snap_rect(int zone, rect_t *r) { snap_rect(zone, &r->x, &r->y, &r->w, &r->h); }

static int snap_zone_of(const window_t *w) { return w->maximized ? SNAP_MAX : w->snapped; }

static void snap_apply(window_t *w, int zone) {
    if (!w->maximized && !w->snapped) {                /* remember the float rect */
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
    }
    if (zone == SNAP_NONE) {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
        w->maximized = 0; w->snapped = SNAP_NONE;
        /* The desktop can have shrunk since that rect was saved -- a scale
         * change does exactly that -- and a window restored wider than the
         * screen puts its own controls past the edge. */
        if (w->w > W)             w->w = W;
        if (w->h > H - SHELF_H)   w->h = H - SHELF_H;
        if (w->x + w->w > W)      w->x = W - w->w;
        if (w->x < 0)             w->x = 0;
        if (w->y < 0)             w->y = 0;
        return;
    }
    snap_rect(zone, &w->x, &w->y, &w->w, &w->h);
    w->maximized = (zone == SNAP_MAX);
    w->snapped   = (zone == SNAP_MAX) ? SNAP_NONE : zone;
}

void wm_snap(window_t *w, int zone) { snap_apply(w, zone); }
void wm_maximize_toggle(window_t *w) {
    snap_apply(w, (w->maximized || w->snapped) ? SNAP_NONE : SNAP_MAX);
}

/* Where the next arrow takes a window that is already in `cur`. This is the
 * Windows table and it is a table on purpose: every attempt to derive it from
 * the current rect got the two-step cases (half, then quarter, then back out)
 * subtly wrong. -1 means minimise. */
static int snap_next(int cur, int dir) {
    switch (dir) {
    case 0:  /* left  */
        switch (cur) {
        case SNAP_R: case SNAP_TL: case SNAP_BL: return SNAP_NONE;
        case SNAP_TR: return SNAP_TL;
        case SNAP_BR: return SNAP_BL;
        default:      return SNAP_L;
        }
    case 1:  /* right */
        switch (cur) {
        case SNAP_L: case SNAP_TR: case SNAP_BR: return SNAP_NONE;
        case SNAP_TL: return SNAP_TR;
        case SNAP_BL: return SNAP_BR;
        default:      return SNAP_R;
        }
    case 2:  /* up    */
        switch (cur) {
        case SNAP_L:  return SNAP_TL;
        case SNAP_R:  return SNAP_TR;
        case SNAP_BL: return SNAP_L;
        case SNAP_BR: return SNAP_R;
        default:      return SNAP_MAX;
        }
    default: /* down  */
        switch (cur) {
        case SNAP_MAX: case SNAP_BL: case SNAP_BR: return SNAP_NONE;
        case SNAP_L:  return SNAP_BL;
        case SNAP_R:  return SNAP_BR;
        case SNAP_TL: return SNAP_L;
        case SNAP_TR: return SNAP_R;
        default:      return -1;
        }
    }
}

static void gui_keysnap(window_t *w, char c) {
    int dir = (unsigned char)c == KEY_LEFT ? 0 : (unsigned char)c == KEY_RIGHT ? 1 :
              (unsigned char)c == KEY_UP   ? 2 : 3;
    int zone = snap_next(snap_zone_of(w), dir);
    if (zone < 0) wm_minimize(wm_index(w));
    else          snap_apply(w, zone);
}

/* Which zone the pointer is asking for, or SNAP_NONE. The top edge maximises;
 * a side edge gives that half, or a quarter when the pointer is also in the top
 * or bottom quarter of the screen. */
#define SNAP_EDGE 2
static int snap_zone_at(int mx, int my) {
    int avail = H - SHELF_H;
    int band  = avail / 4;
    int left  = mx <= SNAP_EDGE, right = mx >= W - 1 - SNAP_EDGE;
    if (left || right) {
        if (my <= band)         return left ? SNAP_TL : SNAP_TR;
        if (my >= avail - band) return left ? SNAP_BL : SNAP_BR;
        return left ? SNAP_L : SNAP_R;
    }
    if (my <= SNAP_EDGE) return SNAP_MAX;
    return SNAP_NONE;
}

int gui_snap_focused(int zone) {
    int t = wm_topmost();
    if (t < 0 || zone < SNAP_NONE || zone > SNAP_MAX) return -1;
    snap_apply(&wins[t], zone);
    return 0;
}

int gui_focused_rect(char *title, uint32_t cap, int *x, int *y, int *w, int *h, int *zone) {
    int t = wm_topmost();
    if (t < 0) return -1;
    window_t *win = &wins[t];
    if (title && cap) snprintf(title, cap, "%s", win->title);
    if (x) *x = win->x;
    if (y) *y = win->y;
    if (w) *w = win->w;
    if (h) *h = win->h;
    if (zone) *zone = snap_zone_of(win);
    return 0;
}

/* ===========================================================================
 *  Compositing
 * ===========================================================================*/
static void draw_windows(void) {
    /* back to front; the snap preview goes under the window being dragged */
    int top_drawn = (wm_drag >= 0 && snap_hint != SNAP_NONE) ? wm_drag : -1;
    for (int pass = 0; pass <= wm_ztop; pass++) {
        for (int i = 0; i < nwin; i++) {
            if (wins[i].z != pass) continue;
            if (i == top_drawn) shell_draw_snap();
            rect_t r; int a;
            if (!wm_visual(i, &r, &a) || a <= 0) continue;
            if (!gfx_clip_hits(r.x - SH_EXT, r.y - SH_EXT, r.w + 2 * SH_EXT, r.h + 2 * SH_EXT + 8)) continue;
            window_t *w = &wins[i];
            int radius = win_tiled(w) && ws[i].anim != WA_MIN && ws[i].anim != WA_RESTORE ? 0 : WIN_R;
            if (r.w != w->w) radius = radius * r.w / (w->w > 0 ? w->w : 1);
            window_shadow(r.x, r.y, r.w, r.h, radius, i == wm_focus, a);
            if (!gfx_clip_hits(r.x, r.y, r.w, r.h)) continue;
            if (ws[i].s.px) {
                gfx_surface_scaled(r.x, r.y, r.w, r.h, &ws[i].s, radius, a);
            } else {
                /* no surface (out of memory): draw straight into the frame */
                int cx0, cy0, cw0, ch0;
                gfx_clip_get(&cx0, &cy0, &cw0, &ch0);
                gfx_clip_push(w->x, w->y, w->w, w->h);
                chrome_draw(w, i == wm_focus, ws[i].hot, win_tiled(w));
                gfx_clip_push(w->x, w->y + TITLE_H, w->w, w->h - TITLE_H);
                gfx_fill(w->x, w->y + TITLE_H, w->w, w->h - TITLE_H, COL_PANEL);
                if (w->draw) w->draw(w, w->x, w->y + TITLE_H, w->w, w->h - TITLE_H);
                gfx_clip_pop();
                chrome_edge(w, win_tiled(w));
                gfx_clip_pop();
            }
        }
    }
    if (top_drawn < 0 && snap_hint != SNAP_NONE) shell_draw_snap();
}

static void draw_desktop_layer(const dmg_t *d) {
    /* wallpaper */
    if (BG) {
        for (int y = d->y; y < d->y + d->h; y++)
            gfx_memcpy32(BB + y * W + d->x, BG + y * W + d->x, d->w);
    } else {
        gfx_fill(d->x, d->y, d->w, d->h, g_settings.wall_color);
    }
    shell_draw_desktop();
}

/* ---- the pointer ------------------------------------------------------------ */
static int  cur_px = -1000, cur_py = -1000, cur_kind = -1;
static uint32_t curbuf[32 * 32];
static int  cur_hide;           /* the login screen draws its own world */

static void present(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0 || g_fb_exclusive) return;
    fb_present_rect(BB + y * W + x, W, x, y, w, h, W, H, out_x, out_y, out_w, out_h);
}

static const int cur_hot[CURSOR_COUNT][2] = {
    { 6, 3 }, { 16, 16 }, { 16, 16 }, { 16, 16 }, { 16, 16 }, { 16, 16 }, { 14, 5 },
};

static int resize_zone(window_t *w, int x, int y);
static int cursor_resize(int x, int y, int *out_i);

static int cursor_kind_now(int mx, int my) {
    int e = 0, wi;
    if (resize_id >= 0) e = resize_edge;
    else if (wm_drag < 0 && client_drag_id < 0) e = cursor_resize(mx, my, &wi);
    if (e) {
        if ((e & (RS_L | RS_R)) && (e & (RS_T | RS_B)))
            return (((e & RS_L) && (e & RS_T)) || ((e & RS_R) && (e & RS_B))) ? CURSOR_RESIZE_NWSE : CURSOR_RESIZE_NESW;
        return (e & (RS_L | RS_R)) ? CURSOR_RESIZE_EW : CURSOR_RESIZE_NS;
    }
    int k = shell_cursor(mx, my);
    return k >= 0 ? k : CURSOR_ARROW;
}

/* Draw the pointer straight onto the panel, over a copy of the backbuffer
 * underneath it. The backbuffer never contains the pointer, so taking it
 * away again is a copy of 32x32 pixels and not a recomposite. */
static void cursor_present(int force) {
    if (cur_hide) return;
    int mx = mlx(), my = mly();
    int kind = cursor_kind_now(mx, my);
    int x = mx - cur_hot[kind][0], y = my - cur_hot[kind][1];
    if (!force && x == cur_px && y == cur_py && kind == cur_kind) return;
    if (cur_px > -1000) present(cur_px, cur_py, 32, 32);
    const uint32_t *img; int stride;
    surface_t cs = { curbuf, 32, 32, 32 };
    surface_t *ot; int oox, ooy;
    ot = gfx_target(&oox, &ooy);
    for (int j = 0; j < 32; j++)
        for (int i = 0; i < 32; i++) {
            int sx = x + i, sy = y + j;
            curbuf[j * 32 + i] = (sx >= 0 && sy >= 0 && sx < W && sy < H) ? BB[sy * W + sx] : 0;
        }
    gfx_bind(&cs, x, y, x, y, 32, 32);
    if (cursor_image(kind, &img, &stride)) gfx_image_sub(x, y, 32, 32, img, 32, 32, stride, 255);
    else { gfx_fill(x, y, 2, 14, 0xFFFFFF); gfx_fill(x, y, 10, 2, 0xFFFFFF); }
    if (ot) gfx_bind(ot, oox, ooy, 0, 0, W, H);
    /* present the composed square; clip it to the desktop */
    int px0 = x < 0 ? -x : 0, py0 = y < 0 ? -y : 0;
    int pw = 32 - px0, ph = 32 - py0;
    if (x + 32 > W) pw = W - x - px0;
    if (y + 32 > H) ph = H - y - py0;
    if (pw > 0 && ph > 0 && !g_fb_exclusive)
        fb_present_rect(curbuf + py0 * 32 + px0, 32, x + px0, y + py0, pw, ph, W, H, out_x, out_y, out_w, out_h);
    cur_px = x; cur_py = y; cur_kind = kind;
}

/* One frame: track, decide what to redraw, redraw it, put it on the panel. */
static uint64_t last_frame;
static int      frames_this_sec;
static uint32_t st_frames;
static uint64_t st_last_us, st_sum_us, st_max_us;
/* where the time of the slowest frame went: render, base, shell, present */
static uint64_t st_stage[4], st_worst_stage[4];
static int      st_worst_dmg, st_dmg_now;

void gui_frame_stages(int *ms10, int *ndmg_worst) {
    for (int i = 0; i < 4; i++) ms10[i] = (int)(st_worst_stage[i] / 100);
    if (ndmg_worst) *ndmg_worst = st_worst_dmg;
    for (int i = 0; i < 4; i++) st_worst_stage[i] = 0;
    st_worst_dmg = 0;
}

void gui_frame_stats(int *frames, int *last_ms10, int *avg_ms10, int *max_ms10) {
    if (frames)    *frames = (int)st_frames;
    if (last_ms10) *last_ms10 = (int)(st_last_us / 100);
    if (avg_ms10)  *avg_ms10 = st_frames ? (int)(st_sum_us / st_frames / 100) : 0;
    if (max_ms10)  *max_ms10 = (int)(st_max_us / 100);
    st_sum_us = 0; st_frames = 0; st_max_us = 0;
}

static void frame(void) {
    uint64_t now = gui_now();
    uint64_t fus0 = now_us();
    any_anim = 0;
    wm_track(now);
    if (shell_update(now)) any_anim = 1;
    int lt_alpha = 0;
    int login_fading = login_transition(now, &lt_alpha);
    if (login_fading) { gui_damage_all(); any_anim = 1; }
    shell_prepare();
    uint64_t ts0 = now_us();
    /* windows whose surface is out of date, if anyone can see them */
    for (int i = 0; i < nwin; i++) {
        if (!ws[i].stale || !wins[i].open) continue;
        rect_t r; int a;
        if (!wm_visual(i, &r, &a)) continue;
        render_window(i, ws[i].stale == 2 || !ws[i].s.px);
    }
    uint64_t ts1 = now_us();
    if (!ndmg) { cursor_present(0); return; }
    dmg_coalesce();
    st_dmg_now = ndmg;
    /* desktop and windows, rectangle by rectangle */
    for (int k = 0; k < ndmg; k++) {
        dmg_t *d = &dmg[k];
        gfx_bind(&BBS, 0, 0, d->x, d->y, d->w, d->h);
        draw_desktop_layer(d);
        draw_windows();
    }
    uint64_t ts2 = now_us();
    /* the shell, a layer at a time, re-blurring its glass as it goes */
    for (int layer = L_SHELL; layer < L_NLAYERS; layer++) {
        gfx_bind(&BBS, 0, 0, 0, 0, W, H);
        shell_capture(layer);
        for (int k = 0; k < ndmg; k++) {
            gfx_bind(&BBS, 0, 0, dmg[k].x, dmg[k].y, dmg[k].w, dmg[k].h);
            shell_draw(layer);
        }
    }
    if (login_fading) {
        gfx_bind(&BBS, 0, 0, 0, 0, W, H);
        login_draw_over(lt_alpha);
    }
    uint64_t ts3 = now_us();
    int cursor_hit = 0;
    for (int k = 0; k < ndmg; k++) {
        present(dmg[k].x, dmg[k].y, dmg[k].w, dmg[k].h);
        if (dmg[k].x < cur_px + 32 && cur_px < dmg[k].x + dmg[k].w &&
            dmg[k].y < cur_py + 32 && cur_py < dmg[k].y + dmg[k].h) cursor_hit = 1;
    }
    ndmg = 0;
    cursor_present(cursor_hit);
    last_frame = now;
    frames_this_sec++;
    uint64_t ts4 = now_us();
    uint64_t fus = ts4 - fus0;
    st_last_us = fus; st_sum_us += fus; st_frames++;
    st_stage[0] = ts1 - ts0; st_stage[1] = ts2 - ts1; st_stage[2] = ts3 - ts2; st_stage[3] = ts4 - ts3;
    if (fus > st_max_us) {
        st_max_us = fus;
        for (int i = 0; i < 4; i++) st_worst_stage[i] = st_stage[i];
        st_worst_dmg = st_dmg_now;
    }
}

/* ===========================================================================
 *  Input
 * ===========================================================================*/
static void clamp_window(window_t *w) {
    if (w->x > W - 80)        w->x = W - 80;
    if (w->x < 80 - w->w)     w->x = 80 - w->w;
    if (w->y < 0)             w->y = 0;
    if (w->y > H - SHELF_H - TITLE_H) w->y = H - SHELF_H - TITLE_H;
}

/* ---- window edge resize: hit-testing ---------------------------------------- *
 * Which edges of window w is (x,y) within the grab band of? Returns an RS_*
 * bitmask (a corner sets two bits). The band straddles each edge by RS_BORDER
 * px, inside and out, so the very rim grabs like every other OS. Maximised
 * windows don't resize. */
static int resize_zone(window_t *w, int x, int y) {
    if (w->maximized) return 0;
    int x0 = w->x, y0 = w->y, x1 = w->x + w->w, y1 = w->y + w->h;
    if (x < x0 - RS_BORDER || x >= x1 + RS_BORDER ||
        y < y0 - RS_BORDER || y >= y1 + RS_BORDER) return 0;
    int e = 0;
    if (iabs(x - x0) <= RS_BORDER) e |= RS_L;
    if (iabs(x - x1) <= RS_BORDER) e |= RS_R;
    if (iabs(y - y0) <= RS_BORDER) e |= RS_T;
    if (iabs(y - y1) <= RS_BORDER) e |= RS_B;
    return e;
}

/* Resize edge mask for the cursor at (x,y): the topmost window whose grab band
 * the point lands in wins. 0 if over an interior, a traffic light or nothing. */
static int cursor_resize(int x, int y, int *out_i) {
    int best = -1, bz = -1;
    for (int i = 0; i < nwin; i++) {
        window_t *w = &wins[i];
        if (!w->open || w->minimized || w->maximized || !on_desk(w)) continue;
        if (x < w->x - RS_BORDER || x >= w->x + w->w + RS_BORDER ||
            y < w->y - RS_BORDER || y >= w->y + w->h + RS_BORDER) continue;
        if (w->z > bz) { bz = w->z; best = i; }
    }
    *out_i = best;
    if (best < 0) return 0;
    window_t *w = &wins[best];
    int e = resize_zone(w, x, y);
    if (e && chrome_group_at(w, x, y)) e = 0;        /* buttons win over the rim */
    return e;
}

/* The window under (x, y), front to back; -1 for the desktop. */
static int window_at(int x, int y) {
    for (int z = wm_ztop; z >= 0; z--)
        for (int i = 0; i < nwin; i++) {
            window_t *w = &wins[i];
            if (!w->open || w->minimized || !on_desk(w) || w->z != z) continue;
            if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) return i;
        }
    return -1;
}

static void on_left_down(int x, int y) {
    /* the shell's overlays first: dialogs, menus, flyouts, the dock */
    if (shell_mouse_down(x, y, MOUSE_LEFT)) return;

    /* window edge -> start an edge/corner resize (topmost window wins) */
    {
        int rwin, re = cursor_resize(x, y, &rwin);
        if (re && rwin >= 0) {
            gui_focus(&wins[rwin]);
            resize_id = rwin; resize_edge = re;
            rs_x0 = wins[rwin].x; rs_y0 = wins[rwin].y;
            rs_w0 = wins[rwin].w; rs_h0 = wins[rwin].h;
            rs_mx = x; rs_my = y;
            return;
        }
    }

    int i = window_at(x, y);
    if (i >= 0) {
        window_t *w = &wins[i];
        gui_focus(w);
        if (y < w->y + TITLE_H) {                   /* title bar */
            int b = chrome_button_at(w, x, y);
            if (b == 0) { wm_close(i); return; }
            if (b == 1) { wm_minimize(i); return; }
            if (b == 2) { wm_maximize_toggle(w); return; }
            /* double-click the title bar toggles maximise */
            uint64_t now = pit_ticks();
            if (title_last_id == i && now - title_last_tick < 400) {
                wm_maximize_toggle(w); title_last_id = -1; return;
            }
            title_last_id = i; title_last_tick = now;
            /* drag a maximised/snapped window: restore its size first and
             * recenter it under the cursor so it follows naturally */
            if (w->maximized || w->snapped) {
                int gw = w->rw > 0 ? w->rw : w->w;
                w->maximized = 0; w->snapped = 0;
                w->w = gw; w->h = w->rh > 0 ? w->rh : w->h;
                w->x = x - gw / 2; w->y = y - TITLE_H / 2;
                clamp_window(w);
                ws[i].gx = w->x; ws[i].gy = w->y; ws[i].gw = w->w; ws[i].gh = w->h;
                ws[i].stale = 2;
            }
            wm_drag = i; drag_dx = x - w->x; drag_dy = y - w->y;
        } else {                                     /* client area */
            if (w->click) w->click(w, x - w->x, y - (w->y + TITLE_H));
            if (w->drag) {                           /* begin a held-button drag stream */
                client_drag_id = i;
                w->drag(w, x - w->x, y - (w->y + TITLE_H));
            }
            ws[i].stale = 2;
        }
        return;
    }
    /* nothing else: the desktop and its icons */
    shell_mouse_desktop(x, y, MOUSE_LEFT);
}

static void on_right_down(int x, int y) {
    if (shell_mouse_down(x, y, MOUSE_RIGHT)) return;
    int i = window_at(x, y);
    if (i >= 0) {
        window_t *w = &wins[i];
        if (y >= w->y + TITLE_H && w->rclick) {
            gui_focus(w);
            w->rclick(w, x - w->x, y - (w->y + TITLE_H));
            ws[i].stale = 2;
        } else if (y < w->y + TITLE_H) {
            gui_focus(w);
            shell_window_menu(i, x, y);                   /* the title bar's menu */
        }
        return;
    }
    shell_mouse_desktop(x, y, MOUSE_RIGHT);
}

static void handle_wheel(int dz) {
    int x = mlx(), y = mly();
    if (shell_wheel(x, y, dz)) return;
    int i = window_at(x, y);
    if (i >= 0) {
        window_t *w = &wins[i];
        if (y >= w->y + TITLE_H && w->scroll) { w->scroll(w, dz); ws[i].stale = 2; }
    }
}

static int last_mx = -1, last_my = -1;

static void handle_mouse(uint8_t b) {
    int x = mlx(), y = mly();
    uint8_t down = b & ~prev_btns;
    uint8_t up   = prev_btns & ~b;
    int moved = (x != last_mx || y != last_my);
    last_mx = x; last_my = y;

    if (resize_id >= 0) {                            /* resizing a window by an edge/corner */
        window_t *w = &wins[resize_id];
        if (b & MOUSE_LEFT) {
            int dx = x - rs_mx, dy = y - rs_my;
            int r_edge = rs_x0 + rs_w0, b_edge = rs_y0 + rs_h0;   /* fixed opposite edges */
            int nx = rs_x0, ny = rs_y0, nw = rs_w0, nh = rs_h0;
            if (resize_edge & RS_R) nw = rs_w0 + dx;
            if (resize_edge & RS_L) nw = rs_w0 - dx;
            if (resize_edge & RS_B) nh = rs_h0 + dy;
            if (resize_edge & RS_T) nh = rs_h0 - dy;
            if (nw < w->min_w) nw = w->min_w;
            if (nh < w->min_h) nh = w->min_h;
            if (nw > W)            nw = W;
            if (nh > H - TITLE_H)  nh = H - TITLE_H;
            if (resize_edge & RS_L) nx = r_edge - nw;            /* grow left: keep right edge */
            if (resize_edge & RS_T) ny = b_edge - nh;            /* grow up: keep bottom edge   */
            if (ny < 0) { ny = 0; if (resize_edge & RS_T) nh = b_edge; }
            w->x = nx; w->y = ny; w->w = nw; w->h = nh;
            clamp_window(w);
        }
        if (up & MOUSE_LEFT) resize_id = -1;
    } else if (wm_drag >= 0) {                       /* dragging a window */
        if (b & MOUSE_LEFT) {
            wins[wm_drag].x = x - drag_dx; wins[wm_drag].y = y - drag_dy;
            clamp_window(&wins[wm_drag]);
            int nh = snap_zone_at(x, y);
            if (nh != snap_hint) {
                rect_t r;
                if (snap_hint != SNAP_NONE) { snap_rect(snap_hint, &r.x, &r.y, &r.w, &r.h); gui_damage(r.x, r.y, r.w, r.h, L_WIN); }
                if (nh != SNAP_NONE)        { snap_rect(nh, &r.x, &r.y, &r.w, &r.h); gui_damage(r.x, r.y, r.w, r.h, L_WIN); }
                snap_hint = nh;
            }
        }
        if (up & MOUSE_LEFT) {
            int zone = snap_zone_at(x, y);
            int id = wm_drag;
            wm_drag = -1;
            if (zone != SNAP_NONE) {
                rect_t r; snap_rect(zone, &r.x, &r.y, &r.w, &r.h);
                gui_damage(r.x, r.y, r.w, r.h, L_WIN);
                snap_apply(&wins[id], zone);
            }
            snap_hint = SNAP_NONE;
        }
    } else if (client_drag_id >= 0) {                /* held-button drag inside an app */
        window_t *w = &wins[client_drag_id];
        if ((b & MOUSE_LEFT) && w->drag && moved) {
            w->drag(w, x - w->x, y - (w->y + TITLE_H));
            ws[client_drag_id].stale = 2;
        }
        if (up & MOUSE_LEFT) client_drag_id = -1;
    } else if (shell_dragging()) {
        shell_mouse_move(x, y, b);
        if (up & MOUSE_LEFT) shell_mouse_up(x, y, MOUSE_LEFT);
    } else {
        if (down & MOUSE_LEFT) on_left_down(x, y);
        else if (up & MOUSE_LEFT) shell_mouse_up(x, y, MOUSE_LEFT);
        if (moved) shell_mouse_move(x, y, b);
    }
    if (down & MOUSE_RIGHT) on_right_down(x, y);

    /* hover: the traffic lights of the window under the pointer, and the
     * client of an app that asked to follow the mouse */
    if (moved) {
        int hi = window_at(x, y);
        for (int i = 0; i < nwin; i++) {
            int hot = (i == hi) ? chrome_button_at(&wins[i], x, y) : -1;
            if (hot < 0 && i == hi && chrome_group_at(&wins[i], x, y)) hot = 3;
            if (hot != ws[i].hot) {
                ws[i].hot = hot;
                if (ws[i].stale < 1) ws[i].stale = 1;
            }
        }
        if (hi >= 0 && wins[hi].track_hover && y >= wins[hi].y + TITLE_H) ws[hi].stale = 2;
    }
    prev_btns = b;
}

/* Print Screen / F12. The capture, the PNG and the file naming all live in
 * kernel/screenshot.c so that `screenshot` at a prompt and this key produce
 * the same file in the same place. */
void gui_screenshot_now(void) {
    char saved[FS_PATH_MAX];
    int rc = shot_take(0, saved, sizeof saved);
    if (rc != 0) {
        char msg[NOTE_TEXT_MAX];
        snprintf(msg, sizeof msg, "Screenshot failed: %s", shot_strerror(rc));
        notify_error("Screenshot", msg);
        return;
    }
    char msg[NOTE_TEXT_MAX];
    snprintf(msg, sizeof msg, "Saved %s", saved);
    notify_success("Screenshot", msg);
    imgview_open_path(saved);      /* and show it, which also proves it decodes */
}

/* Ctrl+Print Screen: start recording, or stop the one that is running. One
 * key for both because a recording you cannot stop from the keyboard means
 * every recording ends with a trip to a terminal in the last frame. */
void gui_record_toggle_now(void) {
    char msg[NOTE_TEXT_MAX];
    if (rec_active()) {
        char saved[FS_PATH_MAX];
        int rc = rec_stop(saved, sizeof saved);
        if (rc != 0) { snprintf(msg, sizeof msg, "Recording failed: %s", rec_strerror(rc));
                       notify_error("Screen recording", msg); return; }
        snprintf(msg, sizeof msg, "Saved %s", saved);
        notify_success("Screen recording", msg);
        return;
    }
    int rc = rec_start(0, 0);
    if (rc != 0) { snprintf(msg, sizeof msg, "Could not start: %s", rec_strerror(rc));
                   notify_error("Screen recording", msg); return; }
    snprintf(msg, sizeof msg, "Recording to %s -- Ctrl+PrtSc to stop", rec_path());
    notify_info("Screen recording", msg);
}

/* One global shortcut, already resolved from its chord by kernel/keymap.c. */
static void gui_do_action(int a) {
    switch (a) {
    case ACT_START_MENU:        shell_toggle_start(); break;
    case ACT_CYCLE_WINDOW:      shell_switcher(+1); break;
    case ACT_CYCLE_WINDOW_BACK: shell_switcher(-1); break;
    case ACT_CLOSE_WINDOW: {
        int t = wm_topmost(); if (t >= 0) wm_close(t);
        shell_close_popups(); break;
    }
    case ACT_SHOW_DESKTOP: gui_show_desktop(); break;
    case ACT_LOCK:         gui_lock(); break;
    case ACT_SCREENSHOT:   gui_screenshot_now(); break;
    case ACT_RECORD:       gui_record_toggle_now(); break;
    case ACT_CLIPBOARD:    clipboard_app_open(); shell_close_popups(); break;
    case ACT_SNAP_LEFT:    case ACT_SNAP_RIGHT:
    case ACT_SNAP_MAX:     case ACT_SNAP_RESTORE: {
        int t = wm_topmost();
        if (t >= 0) gui_keysnap(&wins[t],
            a == ACT_SNAP_LEFT  ? (char)KEY_LEFT  :
            a == ACT_SNAP_RIGHT ? (char)KEY_RIGHT :
            a == ACT_SNAP_MAX   ? (char)KEY_UP    : (char)KEY_DOWN);
        break;
    }
    case ACT_DESK_1: case ACT_DESK_2: case ACT_DESK_3: case ACT_DESK_4:
        wm_desk_goto(a - ACT_DESK_1); break;
    case ACT_DESK_NEXT: wm_desk_goto((wm_desk + 1) % NDESK); break;
    case ACT_DESK_PREV: wm_desk_goto((wm_desk + NDESK - 1) % NDESK); break;
    case ACT_DESK_MOVE_NEXT: desk_send((wm_desk + 1) % NDESK); break;
    case ACT_DESK_MOVE_PREV: desk_send((wm_desk + NDESK - 1) % NDESK); break;
    default: break;
    }
}

/* Shortcuts arrive on their own ring, so they are drained separately and
 * before the characters. Lock and Screenshot outrank a modal dialog: a lock an
 * app (or the OS's own Save As box) could swallow would not be a lock. The
 * window-management ones do not, because a modal dialog really is modal. */
static void handle_actions(void) {
    int a;
    while ((a = kbd_action_get()) >= 0) {
        if (shell_modal() && a != ACT_LOCK && a != ACT_SCREENSHOT) continue;
        gui_do_action(a);
    }
}

static void handle_key(char c) {
    if (shell_key(c)) return;
    int f = wm_topmost();
    if (f >= 0 && wins[f].key) { wins[f].key(&wins[f], c); ws[f].stale = 2; }
}

/* ===========================================================================
 *  Display configuration  (resolution, scaling, wallpaper)
 * ===========================================================================*/
void gui_apply_display(void) {
    /* Real native resolution: reprogram the GPU to the picked mode so the panel
     * itself becomes that size. */
    int lw, lh; settings_res_dims(g_settings.res_index, &lw, &lh);
    if (lw > MAX_PW) lw = MAX_PW;
    if (lh > MAX_PH) lh = MAX_PH;
    if (hires_ok && (lw != PW || lh != PH)) {
        if (fb_set_mode((uint32_t)lw, (uint32_t)lh) == 0) {
            PW = lw; PH = lh;
            mouse_set_bounds(PW, PH);
        }
    }
    /* Display scaling. The panel keeps its native mode and the *desktop* gets
     * smaller: at 200% one logical pixel is a 2x2 block of real ones, so the
     * chrome, the text and every app's fixed pixel layout all double without a
     * single app being told about it. A desktop smaller than 640x480 is not a
     * desktop, so a scale that would produce one is declined. */
    int pct = settings_scale_percent(g_settings.scale_index, PW, PH);
    int lw2 = PW * 100 / pct, lh2 = PH * 100 / pct;
    if (pct == 100 || lw2 < 640 || lh2 < 480) { W = PW; H = PH; }
    else                                      { W = lw2; H = lh2; }
    out_x = 0; out_y = 0; out_w = PW; out_h = PH;
    BBS.px = BB; BBS.w = W; BBS.h = H; BBS.stride = W;
    if (BB) gfx_bind(&BBS, 0, 0, 0, 0, W, H);

    /* keep every window inside the (possibly smaller) logical desktop */
    for (int i = 0; i < nwin; i++) {
        window_t *w = &wins[i];
        if (w->maximized) { w->x = 0; w->y = 0; w->w = W; w->h = H - SHELF_H; }
        else if (w->snapped) snap_apply(w, w->snapped);
        else {
            if (w->w > W)             w->w = W;
            if (w->h > H - SHELF_H)   w->h = H - SHELF_H;
            clamp_window(w);
        }
        if (w->rw > W)             w->rw = W;
        if (w->rh > H - SHELF_H)   w->rh = H - SHELF_H;
        if (w->rx + w->rw > W)     w->rx = W - w->rw;
        if (w->rx < 0)             w->rx = 0;
        if (w->ry < 0)             w->ry = 0;
        ws[i].gx = w->x; ws[i].gy = w->y; ws[i].gw = w->w; ws[i].gh = w->h;
        ws[i].stale = 2;
        ws[i].foot_ok = 0;
    }
    cur_px = -1000;
    wallpaper_follow_res();      /* re-load the render cut for this panel size */
    if (BG) render_wallpaper(BG);
    wall_version++;
    ndmg = 0;
    gui_damage_all();
    shell_theme_changed();
}

/* Force one composite + blit right now. Lets a blocking command (the python
 * REPL waiting on keyboard input) keep the desktop on screen. */
void gui_pump(void) {
    if (!BB) return;
    if (login_active) { login_frame(gui_now()); return; }
    frame();
}

/* `display bench`: time the primitives a frame is made of, in the machine
 * itself, because the cost model under QEMU's TCG is nothing like the one on
 * hardware and guessing which part of a frame is slow has been wrong twice. */
void gui_bench(void) {
    if (!BB) { kprintf("bench: no desktop\n"); return; }
    surface_t s = BBS;
    uint64_t t;
    #define B(name, n, body) do { gfx_bind(&s, 0, 0, 0, 0, W, H); t = now_us(); \
        for (int _i = 0; _i < (n); _i++) { body; } t = now_us() - t; \
        kprintf("  %-26s %5lu.%01lu ms\n", name, (unsigned long)(t / (n) / 1000), (unsigned long)(t / (n) / 100 % 10)); } while (0)
    kprintf("per call, %d x %d:\n", W, H);
    B("memcpy full screen", 4, for (int y = 0; y < H; y++) gfx_memcpy32(BB + y * W, BG + y * W, W));
    B("fill full screen", 4, gfx_fill(0, 0, W, H, 0x202020));
    B("blend 400x400", 4, gfx_fill_a(100, 100, 400, 400, 0x808080, 128));
    B("rrect 640x620 r18", 4, gfx_rrect(100, 50, 640, 620, 18, 0x333333, 255));
    B("shadow 720x460 b38", 4, gfx_shadow(100, 100, 720, 460, 10, 38, 16, 0, 120));
    backdrop_t bd = {0};
    glass_t g = { 0x1C1C22, 170, 170, 30, 0 };
    B("glass capture 640x620", 2, gfx_backdrop_capture(&bd, 190, 60, 640, 620, &g));
    B("glass draw 640x620", 4, gfx_backdrop_draw(&bd, 190, 60, 640, 620, 18, 255));
    B("glass draw 640x620 a128", 2, gfx_backdrop_draw(&bd, 190, 60, 640, 620, 18, 128));
    gfx_backdrop_free(&bd);
    B("icon 48 native x18", 2, for (int k = 0; k < 18; k++) icon_draw(ICON_FILES, 100 + k * 50, 300, 48, 255));
    B("icon 44 scaled x18", 2, for (int k = 0; k < 18; k++) icon_draw(ICON_FILES, 100 + k * 50, 300, 44, 255));
    B("text 13px 60 chars x20", 2, for (int k = 0; k < 20; k++)
        font_draw(FONT_REGULAR, 13, 100, 100 + k * 18, "The quick brown fox jumps over the lazy dog 0123456789 abc", -1, 0xFFFFFF, 255));
    B("mono 80x29 cells", 1, for (int r = 0; r < 29; r++) for (int c = 0; c < 80; c++) g_char(20 + c * 8, 20 + r * 16, 'a' + (c % 26), 0xE0E0E0, 1));
    if (wm_nwin > 0) {
        int i = wm_topmost();
        if (i >= 0 && ws[i].s.px) {
            B("window copy_rr", 4, gfx_copy_rr(wins[i].x, wins[i].y, &ws[i].s, WIN_R, 255));
            B("window scaled 90%", 2, gfx_surface_scaled(wins[i].x + 20, wins[i].y + 20, wins[i].w * 9 / 10, wins[i].h * 9 / 10, &ws[i].s, WIN_R, 200));
            B("window render", 2, render_window(i, 1));
        }
    }
    B("present full screen", 4, present(0, 0, W, H));
    #undef B
    gui_damage_all();
}

/* Shown at the sign-in screen and the lock screen through the same calls. */
surface_t *gui_bb(void) { return &BBS; }
void gui_present_all(void) { present(0, 0, W, H); cur_px = -1000; }
void gui_present_rect(int x, int y, int w, int h) { present(x, y, w, h); }
void gui_cursor_show(int on) { cur_hide = !on; if (on) { cur_px = -1000; cursor_present(1); } }
void gui_cursor_update(void) { cursor_present(0); }

void gui_toast(const char *msg) {
    if (!msg) return;
    notify_post(NOTE_INFO, "System", msg);
}
void gui_notify(int level, const char *source, const char *text) {
    notify_post(level, source, text);
}
static void gui_note_hook(const notification_t *n) { (void)n; shell_note(); }

/* Handed to drivers/audio.c: stop the two players that stream through the
 * codec, so a blocking play gets a device to itself. */
static void gui_audio_preempt(void) { media_halt(); music_halt(); }

/* ===========================================================================
 *  Boot + main loop
 * ===========================================================================*/
void gui_run(void) {
    PW = (int)fb_width();
    PH = (int)fb_height();
    /* The backbuffers are sized MAX_PW x MAX_PH once and never resized, so a
     * panel larger than that would have the compositor drawing past their end
     * -- on exactly the machine nobody tests on. Say so and fall back. */
    if (PW > MAX_PW || PH > MAX_PH) {
        kprintf("\nThe display is %dx%d and the desktop is built for at most "
                "%dx%d.\nStarting BoltShell instead.\n", PW, PH, MAX_PW, MAX_PH);
        shell_run();
        return;
    }
    W = PW; H = PH;
    out_x = out_y = 0; out_w = PW; out_h = PH;

    /* Backbuffers at the largest panel we support (4K), so a later resolution
     * change just reuses them. From the PMM (direct-mapped), since the kheap
     * can't hold two 33 MiB buffers next to everything else. */
    uint64_t maxpx    = (uint64_t)MAX_PW * MAX_PH;
    uint64_t maxframes = (maxpx * 4 + 4095) / 4096;
    uint64_t bbp = pmm_alloc_contig(maxframes);
    uint64_t bgp = bbp ? pmm_alloc_contig(maxframes) : 0;
    if (bbp && bgp) {
        BB = (uint32_t *)P2V(bbp);
        BG = (uint32_t *)P2V(bgp);
        hires_ok = 1;
    } else {
        if (bbp) pmm_free_contig(bbp, maxframes);
        BB = (uint32_t *)kmalloc((uint64_t)PW * PH * 4);
        BG = BB ? (uint32_t *)kmalloc((uint64_t)PW * PH * 4) : 0;
        /* Both or neither: the compositor reads BG every frame. */
        if (!BG) { kfree(BB); console_detach(); shell_run(); return; }
        hires_ok = 0;
    }
    BBS.px = BB; BBS.w = W; BBS.h = H; BBS.stride = W;
    gfx_bind(&BBS, 0, 0, 0, 0, W, H);
    ws_stale_title = stale_title;
    motion_calibrate();        /* before settings_init applies Auto */

    font_init();
    settings_init();           /* theme + logical size + wallpaper */

    /* A notification posted from a driver or the network stack has to reach
     * the screen without that code knowing the compositor exists. */
    notify_set_hook(gui_note_hook);
    audio_set_preempt(gui_audio_preempt);

    console_detach();                                /* GUI owns the framebuffer now */
    login_begin();                                   /* the splash, straight away */
    login_frame(gui_now());

    terminal_app_init();
    files_app_init();
    browser_app_init();
    oldbrowser_app_init();
    ide_app_init();
    taskmgr_app_init();
    calc_app_init();
    clock_app_init();
    notes_app_init();
    tasks_app_init();
    contacts_app_init();
    sheets_app_init();
    imgview_app_init();
    music_app_init();
    media_app_init();
    calendar_app_init();
    piano_app_init();
    paint_app_init();
    mines_app_init();
    snake_app_init();
    g2048_app_init();
    stopwatch_app_init();
    charmap_app_init();
    clipboard_app_init();
    snip_app_init();
    devmgr_app_init();
    solitaire_app_init();
    sysinfo_app_init();
    life_app_init();
    ttt_app_init();
    colorpick_app_init();
    memory_app_init();
    matrix_app_init();
    doom_app_init();
    strike_app_init();
    gameboy_app_init();
    boltcraft_app_init();
    installer_app_init();
    software_app_init();
    logs_app_init();
    security_app_init();
    network_app_init();
    mixer_app_init();
    settings_app_init();
    shell_init();
    icons_preload();

    uint64_t tick_pit = pit_ticks();
    uint64_t sec_pit  = pit_ticks();
    uint64_t busy = 0, total = 0;
    prev_btns = 0; wm_drag = -1;

    wallpaper_restore();       /* restore a saved image wallpaper, if any */
    gui_damage_all();

    gui_last_input_tick = pit_ticks();

    for (;;) {
        uint64_t t0 = rdtsc();

        /* Feed the media player's codec first, and unconditionally: above
         * every early-out below, so a track keeps playing while its window is
         * minimised, while the session is locked, and while a ring-3 program
         * has the panel. Returns at once when nothing is playing. */
        media_pump();
        /* The screen recorder too: it only reads VRAM. */
        rec_pump();

        /* A ring-3 program has grabbed the panel: don't touch VRAM or drain
         * the keyboard. Repaint the desktop once it lets go. */
        if (g_fb_exclusive) {
            __asm__ volatile("hlt");
            gui_damage_all();
            cur_px = -1000;
            continue;
        }

        if (login_active || login_transition(gui_now(), 0)) {
            xhci_poll();
            /* Drained and dropped: a shortcut must not act on a locked
             * session, and leaving them in the ring would fire the lot the
             * moment somebody signs in. */
            if (login_active) while (kbd_action_get() >= 0) { }
            int ci;
            while ((ci = kbd_trygetc()) >= 0) {
                gui_last_input_tick = pit_ticks();
                if (login_active) login_key((char)ci);
            }
            if (mouse_poll_event()) {
                uint8_t now = mouse_buttons(), latched = mouse_take_pressed();
                if (login_active) login_mouse(mlx(), mly(), (now | latched) & MOUSE_LEFT);
                gui_last_input_tick = pit_ticks();
            }
            (void)mouse_wheel();
            if (login_active) {
                int busy_anim = login_frame(gui_now());
                if (!busy_anim) __asm__ volatile("hlt");
                continue;
            }
            /* the fade out of the sign-in screen runs through frame() */
        }

        xhci_poll();
        if (mouse_poll_event()) {
            /* A click that began and ended between two frames is only in the
             * latch: replay it as a press, then let the real (released) state
             * deliver the matching release, so nothing is left half-dragged. */
            uint8_t now = mouse_buttons(), latched = mouse_take_pressed();
            if (latched & ~now) handle_mouse((uint8_t)(now | latched));
            handle_mouse(now);
            gui_last_input_tick = pit_ticks();
        }
        { int dz = mouse_wheel(); if (dz) { handle_wheel(dz); gui_last_input_tick = pit_ticks(); } }
        handle_actions();
        shell_switcher_poll();
        int ci;
        while ((ci = kbd_trygetc()) >= 0) {
            gui_last_input_tick = pit_ticks();
            handle_key((char)ci);
        }

        /* Idle auto-lock, measured from the last real input. */
        if (gui_idle_lock() > 0 && !login_active) {
            uint64_t idle = pit_ticks() - gui_last_input_tick;
            if (idle > (uint64_t)gui_idle_lock() * 60000ull) gui_lock();
        }

        doom_pump();        /* DOOM self-throttles to ~35 fps and only when focused */
        strike_pump();      /* same arrangement: caps itself, idles unless focused  */
        gameboy_pump();     /* one emulated frame per 16 ms, focused only           */

        uint64_t now = pit_ticks();
        if (now - tick_pit >= 150) {                 /* ~6.6 Hz: app ticks */
            tick_pit = now;
            /* A ticking window that can be seen redraws -- it might be a
             * clock. So does the focused one, but only when a caret would
             * blink in it: every 150 ms for a terminal nobody is typing in
             * was most of what the desktop spent its time on. */
            int caret = (int)((now / 500) % 2);
            static int last_caret = -1;
            for (int i = 0; i < nwin; i++) {
                if (!wins[i].open) continue;
                if (wins[i].tick) wins[i].tick(&wins[i]);
                int vis = !wins[i].minimized && on_desk(&wins[i]);
                if (!vis) continue;
                if (wins[i].tick && !wins[i].lazy_tick) ws[i].stale = 2;
                else if (i == wm_focus && caret != last_caret) ws[i].stale = 2;
            }
            last_caret = caret;
            shell_tick(now);
        }

        /* Brightness is applied on the way to VRAM, so only what gets
         * presented picks a new level up -- and with frames built from damage
         * that was the Terminal you typed `display brightness 40` into and
         * nothing else (t_quick: "40% made the screen 89% as bright"). The
         * backbuffer is never dimmed, so a change is one full present and no
         * recomposite. */
        static int dim_seen = 100;
        if (fb_dim() != dim_seen) { dim_seen = fb_dim(); gui_present_all(); }

        /* A frame when there is something to draw -- and while animating, at
         * most every 8 ms: 125 fps is more than any panel here shows. A stale
         * window nobody can see does not count, or the loop would spin on
         * it for as long as it sat minimised. */
        int pending = ndmg != 0 || any_anim;
        for (int i = 0; i < nwin && !pending; i++)
            if (ws[i].stale && wins[i].open && !wins[i].minimized && on_desk(&wins[i])) pending = 1;
        int due = !any_anim || (now - last_frame >= 8);
        if (pending && due) frame();
        else cursor_present(0);

        uint64_t t1 = rdtsc();
        busy += t1 - t0;
        __asm__ volatile("hlt");                     /* sleep until the next IRQ: <= 1 ms */
        total += rdtsc() - t0;

        if (now - sec_pit >= 1000) {                 /* recompute CPU load each second */
            sec_pit = now;
            cpu_load = total ? (int)((busy * 100) / total) : 0;
            if (cpu_load > 100) cpu_load = 100;
            busy = total = 0;
            frames_this_sec = 0;
        }
    }
}
