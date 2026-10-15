/* ===========================================================================
 *  BoltOS  -  kernel/gui_shell.c
 *
 *  Everything the desktop draws around the windows: the dock, the tray and
 *  the desktop switcher along the bottom, the Start menu, Quick Settings,
 *  the notification centre and its calendar, toasts, context menus, the
 *  Alt+Tab switcher, the one modal dialog, the icons on the wallpaper, the
 *  drop preview for tiling, and the window frames themselves.
 *
 *  It is all one material: glass. Each panel keeps a blurred, tinted copy of
 *  what is behind it (gfx backdrop), recaptured only when the compositor says
 *  something underneath changed -- a window moved, the wallpaper changed --
 *  and otherwise redrawn from the cache, which is what keeps hovering over
 *  the dock from costing a blur per mouse event.
 *
 *  Layout code computes rectangles; drawing and hit-testing both go through
 *  them. The old taskbar learnt the hard way that two copies of the same
 *  arithmetic disagree by a pixel, and a button you can see but not press is
 *  a special kind of annoying.
 * ===========================================================================*/
#include <stdint.h>
#include <stdio.h>
#include "gui_int.h"
#include "settings.h"
#include "keyboard.h"
#include "mouse.h"
#include "pit.h"
#include "hw.h"
#include "string.h"
#include "fs.h"
#include "users.h"
#include "audio.h"
#include "mixer.h"
#include "netif.h"
#include "net.h"
#include "wifi.h"
#include "notify.h"
#include "record.h"
#include "backlight.h"
#include "power.h"
#include "acpi.h"
#include "kheap.h"
#include "math.h"

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
#define SW gui_screen_w()
#define SH gui_screen_h()
static int in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* ===========================================================================
 *  Materials
 * ===========================================================================*/
static glass_t G_SHELL, G_POPUP, G_MENU;
static uint32_t ink, ink_dim, ink_mute;          /* text on glass */
static uint32_t fill_hot, fill_press;            /* hover/press washes on glass */

static void styles(void) {
    int dark = g_theme.dark;
    uint32_t t = g_theme.glass;
    int a = g_theme.glass_a;
    G_SHELL.tint = t; G_SHELL.tint_a = a;       G_SHELL.sat = 180; G_SHELL.blur = 22; G_SHELL.lift = dark ? 0 : 4;
    G_POPUP.tint = t; G_POPUP.tint_a = imin(a + 38, 235); G_POPUP.sat = 170; G_POPUP.blur = 30; G_POPUP.lift = 0;
    G_MENU.tint  = t; G_MENU.tint_a  = imin(a + 62, 240); G_MENU.sat = 150;  G_MENU.blur = 20;  G_MENU.lift = 0;
    ink = g_theme.text; ink_dim = g_theme.text_dim; ink_mute = g_theme.text_mute;
    fill_hot   = dark ? 0xFFFFFF : 0x000000;
    fill_press = dark ? 0xFFFFFF : 0x000000;
}
#define HOT_A   (g_theme.dark ? 26 : 16)
#define PRESS_A (g_theme.dark ? 40 : 26)

/* A rule across glass. COL_SEP is an opaque colour picked for window
 * backgrounds, and painted over a blurred wallpaper it comes out as a hard
 * black line -- the notification centre had one under its last card. */
static void glass_rule(int x, int y, int w) {
    gfx_fill_a(x, y, w, 1, g_theme.dark ? 0xFFFFFF : 0x000000, g_theme.dark ? 30 : 24);
}

/* A glass panel: where it is, the cache of what is behind it, its layer. */
typedef struct {
    int        layer;
    int        vis;
    int        x, y, w, h, r;
    int        alpha;             /* panel opacity, for fades               */
    int        palpha;
    rect_t     env;               /* what the backdrop covers               */
    backdrop_t bd;
    const glass_t *g;
    int        dirty;
    int        pending;           /* something under it changed; re-blur soon */
    uint64_t   captured;          /* when it last did                       */
    int        px, py, pw, ph, pvis;   /* last frame, for damage           */
    int        shadow;            /* blur of its drop shadow, 0 none        */
} panel_t;

enum { P_PILL, P_DOCK, P_TRAY, P_TIP, P_START, P_QS, P_NC, P_MENU, P_SWITCH,
       P_PROMPT, P_TOAST0, P_TOAST1, P_TOAST2, P_COUNT };
static panel_t P[P_COUNT];

static void panel_place(panel_t *p, int x, int y, int w, int h, int r, int alpha) {
    p->vis = 1; p->x = x; p->y = y; p->w = w; p->h = h; p->r = r; p->alpha = alpha;
    if (!p->env.w) { p->env.x = x; p->env.y = y; p->env.w = w; p->env.h = h; }
}
static int shadow_reach(const panel_t *p) { return p->shadow ? p->shadow + p->shadow / 2 + 12 : 4; }

/* Shadow, blurred backdrop, then the rim that makes it read as glass: a
 * bright line along the top edge where light would catch it, fading down
 * the sides, and a faint dark hairline outside for the busy wallpapers. */
static void panel_glass(panel_t *p) {
    if (!p->vis || p->alpha <= 0) return;
    int a = p->alpha;
    if (p->shadow) gfx_shadow(p->x, p->y, p->w, p->h, p->r, p->shadow, p->shadow / 3, 0,
                              (g_theme.dark ? 100 : 60) * a / 255);
    gfx_backdrop_draw(&p->bd, p->x, p->y, p->w, p->h, p->r, a);
    if (g_theme.dark) {
        gfx_rrect_stroke2(p->x, p->y, p->w, p->h, p->r, 0xFFFFFF, 64 * a / 255, 14 * a / 255);
        gfx_rrect_stroke(p->x - 1, p->y - 1, p->w + 2, p->h + 2, p->r + 1, 1, 0x000000, 70 * a / 255);
    } else {
        gfx_rrect_stroke2(p->x, p->y, p->w, p->h, p->r, 0xFFFFFF, 170 * a / 255, 60 * a / 255);
        gfx_rrect_stroke(p->x - 1, p->y - 1, p->w + 2, p->h + 2, p->r + 1, 1, 0x000000, 34 * a / 255);
    }
}

/* ===========================================================================
 *  State
 * ===========================================================================*/
static int hov = 0;                 /* hover id, see hov_* below */
static int press = 0;               /* element held down */
#define H_DOCK    0x100
#define H_PILL    0x200
#define H_TRAY    0x300
#define H_START   0x400
#define H_QS      0x500
#define H_NC      0x600
#define H_MENU    0x700
#define H_TOAST   0x800
#define H_SWITCH  0x900
#define H_PROMPT  0xA00
#define H_ICON    0xB00
#define HKIND(h)  ((h) & 0xF00)

static int  start_open, qs_open, nc_open;
static uint64_t start_t0, qs_t0, nc_t0;
static int  start_closing, qs_closing, nc_closing;
static int  dnd;                    /* focus: toasts held back */

static void dmg_panel(int k) {
    panel_t *p = &P[k];
    int e = shadow_reach(p);
    if (p->vis) gui_damage(p->x - e, p->y - e, p->w + 2 * e, p->h + 2 * e, p->layer);
    if (p->pvis) gui_damage(p->px - e, p->py - e, p->pw + 2 * e, p->ph + 2 * e, p->layer);
}

/* ===========================================================================
 *  Window chrome
 * ===========================================================================*/
static const uint32_t LIGHT_COL[3] = { 0xFF5F57, 0xFEBC2E, 0x28C840 };
static int light_cx(window_t *w, int k) { return w->x + 18 + k * 20; }

int chrome_button_at(window_t *w, int x, int y) {
    if (y < w->y || y >= w->y + TITLE_H) return -1;
    for (int k = 0; k < 3; k++) {
        int cx = light_cx(w, k);
        if (x >= cx - 10 && x < cx + 10) return k;
    }
    return -1;
}
int chrome_group_at(window_t *w, int x, int y) {
    return y >= w->y && y < w->y + TITLE_H && x >= w->x + 6 && x < w->x + 70;
}

void chrome_draw(window_t *w, int focused, int hot, int maximized) {
    int x = w->x, y = w->y, ww = w->w;
    uint32_t tb = COL_TITLEBAR;
    gfx_vgrad(x, y, ww, TITLE_H, gfx_shade(tb, g_theme.dark ? 106 : 101), tb, 255);
    gfx_fill(x, y + TITLE_H - 1, ww, 1, COL_SEP);
    /* traffic lights */
    int cy = y + TITLE_H / 2;
    int group = hot >= 0;
    for (int k = 0; k < 3; k++) {
        float cx = (float)light_cx(w, k) + 0.5f;
        uint32_t c = focused || group ? LIGHT_COL[k] : (g_theme.dark ? 0x4A4A51 : 0xD1D1D6);
        gfx_circle(cx, (float)cy, 6.5f, c, 255);
        gfx_ring(cx, (float)cy, 6.5f, 1.0f, 0x000000, focused || group ? 38 : 22);
        if (group) {
            uint32_t mk = 0x000000;
            int a = 150;
            if (k == 0) {
                gfx_line(cx - 2.7f, cy - 2.2f, cx + 2.3f, cy + 2.8f, 1.35f, mk, a);
                gfx_line(cx + 2.3f, cy - 2.2f, cx - 2.7f, cy + 2.8f, 1.35f, mk, a);
            } else if (k == 1) {
                gfx_line(cx - 3.0f, cy + 0.5f, cx + 3.0f, cy + 0.5f, 1.5f, mk, a);
            } else {
                /* two corner wedges: the macOS full-screen mark */
                gfx_line(cx - 2.6f, cy - 2.4f, cx + 0.6f, cy - 2.4f, 1.3f, mk, a);
                gfx_line(cx - 2.6f, cy - 2.4f, cx - 2.6f, cy + 0.8f, 1.3f, mk, a);
                gfx_line(cx + 2.6f, cy + 3.0f, cx - 0.6f, cy + 3.0f, 1.3f, mk, a);
                gfx_line(cx + 2.6f, cy + 3.0f, cx + 2.6f, cy - 0.2f, 1.3f, mk, a);
            }
            if (hot == k) gfx_circle(cx, (float)cy, 6.5f, 0x000000, 26);
        }
    }
    /* icon + title, centred on the window */
    int avail = ww - 2 * 78;
    if (avail > 40) {
        uint32_t tc = focused ? COL_TEXT : COL_TEXT_DIM;
        int tw = font_width(FONT_SEMIBOLD, 13, w->title, -1);
        int iw = 22;
        if (tw + iw > avail) tw = avail - iw;
        int sx = x + (ww - tw - iw) / 2;
        icon_draw(w->icon, sx, cy - 8, 16, focused ? 255 : 200);
        font_draw_fit(FONT_SEMIBOLD, 13, sx + iw, cy + 5, w->title, tw, tc, 255);
    }
    (void)maximized;
}

/* The edge every frame gets, drawn after the app so nothing paints over it. */
void chrome_edge(window_t *w, int maximized) {
    if (maximized) return;
    if (g_theme.dark) {
        gfx_rrect_stroke(w->x, w->y, w->w, w->h, WIN_R, 1, 0x000000, 150);
        gfx_rrect_stroke2(w->x + 1, w->y + 1, w->w - 2, w->h - 2, WIN_R - 1, 0xFFFFFF, 34, 10);
    } else {
        gfx_rrect_stroke(w->x, w->y, w->w, w->h, WIN_R, 1, 0x000000, 40);
    }
}

void window_shadow(int x, int y, int w, int h, int r, int focused, int alpha) {
    int op = g_theme.dark ? (focused ? 128 : 84) : (focused ? 74 : 46);
    gfx_shadow(x, y, w, h, r, focused ? 38 : 24, focused ? 16 : 8, 0, op * alpha / 255);
}

/* ===========================================================================
 *  Desktop icons + drag-and-drop
 * ===========================================================================*/
#define MAX_DESKICON 64
#define DI_W 88
#define DI_H 94
typedef struct { int used, x, y, icon; char label[40]; fs_node *node; window_t *win; } deskicon_t;
static deskicon_t dicons[MAX_DESKICON];
static int      desk_sel = -1;
static int      desk_drag = -1, desk_dx, desk_dy, desk_moved;
static uint64_t desk_last_tick;
static int      desk_last_idx = -1;

/* drag payload: a file being dragged out of a window onto the desktop */
static fs_node *dnd_node;
static char     dnd_label[40];
static int      dnd_icon, dnd_armed, dnd_active, dnd_px, dnd_py, press_x, press_y;

static int deskicon_find(fs_node *n) {
    for (int i = 0; i < MAX_DESKICON; i++) if (dicons[i].used && dicons[i].node == n) return i;
    return -1;
}
static void deskicon_autopos(int *ox, int *oy) {
    int used = 0;
    for (int i = 0; i < MAX_DESKICON; i++) if (dicons[i].used) used++;
    int per_col = (SH - SHELF_H - 24) / DI_H; if (per_col < 1) per_col = 1;
    *ox = 14 + (used / per_col) * DI_W;
    *oy = 14 + (used % per_col) * DI_H;
}
static void deskicon_clamp(deskicon_t *d) {
    if (d->x < 0) d->x = 0;
    if (d->x > SW - DI_W) d->x = SW - DI_W;
    if (d->y < 0) d->y = 0;
    if (d->y > SH - SHELF_H - DI_H) d->y = SH - SHELF_H - DI_H;
}
static void dmg_icon(int i) {
    if (i < 0 || i >= MAX_DESKICON || !dicons[i].used) return;
    gui_damage(dicons[i].x - 2, dicons[i].y - 2, DI_W + 4, DI_H + 4, L_DESK);
}

void gui_desktop_add(fs_node *node, const char *label, int icon) {
    if (!node || deskicon_find(node) >= 0) return;
    for (int i = 0; i < MAX_DESKICON; i++) {
        if (dicons[i].used) continue;
        dicons[i].used = 1; dicons[i].node = node; dicons[i].icon = icon;
        strncpy(dicons[i].label, label ? label : "item", sizeof(dicons[i].label) - 1);
        deskicon_autopos(&dicons[i].x, &dicons[i].y);
        dmg_icon(i);
        return;
    }
}

/* Seed an app launcher onto the desktop (double-click opens the window). */
void gui_desktop_add_app(window_t *win, const char *label, int icon) {
    if (!win) return;
    for (int i = 0; i < MAX_DESKICON; i++) {
        if (dicons[i].used) continue;
        dicons[i].used = 1; dicons[i].win = win; dicons[i].icon = icon;
        strncpy(dicons[i].label, label ? label : "app", sizeof(dicons[i].label) - 1);
        deskicon_autopos(&dicons[i].x, &dicons[i].y);
        dmg_icon(i);
        return;
    }
}

static void deskicon_drop(fs_node *n, const char *label, int icon, int x, int y) {
    int idx = deskicon_find(n);
    if (idx < 0) {
        for (int i = 0; i < MAX_DESKICON && idx < 0; i++) if (!dicons[i].used) idx = i;
        if (idx < 0) return;
        dicons[idx].used = 1; dicons[idx].node = n; dicons[idx].icon = icon;
        strncpy(dicons[idx].label, label ? label : "item", sizeof(dicons[idx].label) - 1);
    } else dmg_icon(idx);
    dicons[idx].x = x - DI_W / 2; dicons[idx].y = y - 30;
    deskicon_clamp(&dicons[idx]);
    desk_sel = idx;
    dmg_icon(idx);
}

void gui_begin_item_drag(fs_node *node, const char *label, int icon) {
    if (!node) return;
    dnd_node = node; dnd_icon = icon; dnd_armed = 1; dnd_active = 0;
    strncpy(dnd_label, label ? label : "item", sizeof(dnd_label) - 1);
    press_x = gui_mx(); press_y = gui_my();
}

static void arrange_icons(void) {
    int n = 0;
    int per_col = (SH - SHELF_H - 24) / DI_H; if (per_col < 1) per_col = 1;
    for (int i = 0; i < MAX_DESKICON; i++) {
        if (!dicons[i].used) continue;
        dicons[i].x = 14 + (n / per_col) * DI_W;
        dicons[i].y = 14 + (n % per_col) * DI_H;
        n++;
    }
    gui_damage_all();
}

static int deskicon_hit(int x, int y) {
    for (int i = MAX_DESKICON - 1; i >= 0; i--) {
        if (!dicons[i].used) continue;
        if (in_rect(x, y, dicons[i].x, dicons[i].y, DI_W, DI_H)) return i;
    }
    return -1;
}

static void deskicon_open(int i) {
    if (i < 0 || i >= MAX_DESKICON || !dicons[i].used) return;
    if (dicons[i].win)       gui_open(dicons[i].win);
    else if (dicons[i].node) files_open_node(dicons[i].node);
}

/* text with a soft dark halo under it, for labels sitting on a photo */
static void label_on_photo(int cx, int base, const char *s, int maxw) {
    char tmp[44];
    int n = (int)strlen(s); if (n > 43) n = 43;
    memcpy(tmp, s, (size_t)n); tmp[n] = 0;
    int tw = font_width(FONT_MEDIUM, 12, tmp, -1);
    if (tw > maxw) tw = maxw;
    int x = cx - tw / 2;
    font_draw_fit(FONT_MEDIUM, 12, x, base + 1, tmp, maxw, 0x000000, 150);
    font_draw_fit(FONT_MEDIUM, 12, x + 1, base + 1, tmp, maxw, 0x000000, 60);
    font_draw_fit(FONT_MEDIUM, 12, x, base, tmp, maxw, 0xFFFFFF, 255);
}

void shell_draw_desktop(void) {
    for (int i = 0; i < MAX_DESKICON; i++) {
        if (!dicons[i].used) continue;
        deskicon_t *d = &dicons[i];
        if (!gfx_clip_hits(d->x - 2, d->y - 2, DI_W + 4, DI_H + 4)) continue;
        int hot = hov == (H_ICON | i);
        if (i == desk_sel) {
            gfx_rrect(d->x + 4, d->y + 2, DI_W - 8, DI_H - 4, 10, 0xFFFFFF, 46);
            gfx_rrect_stroke(d->x + 4, d->y + 2, DI_W - 8, DI_H - 4, 10, 1, 0xFFFFFF, 70);
        } else if (hot) {
            gfx_rrect(d->x + 4, d->y + 2, DI_W - 8, DI_H - 4, 10, 0xFFFFFF, 24);
        }
        icon_draw(d->icon, d->x + (DI_W - 48) / 2, d->y + 8, 48, 255);
        label_on_photo(d->x + DI_W / 2, d->y + 74, d->label, DI_W - 10);
    }
    if (dnd_active) {
        int s = 48;
        icon_draw(dnd_icon, dnd_px - s / 2, dnd_py - s / 2, s, 200);
        label_on_photo(dnd_px, dnd_py + s / 2 + 14, dnd_label, 120);
    }
}

/* ===========================================================================
 *  The dock
 * ===========================================================================*/
#define DOCK_ICON 40
#define DOCK_SLOT 50
#define DOCK_PAD  8
enum { DI_START = -1, DI_SEARCH = -2 };

typedef struct { int win; float scale; float target; uint64_t bounce0; } dockitem_t;
static dockitem_t ditems[MAX_WIN + 2];
static int        ndock;
static int        dock_slot = DOCK_SLOT, dock_icon = DOCK_ICON;

static void dock_build(void) {
    dockitem_t old[MAX_WIN + 2];
    int nold = ndock;
    memcpy(old, ditems, sizeof(dockitem_t) * (size_t)ndock);
    ndock = 0;
    ditems[ndock].win = DI_START; ditems[ndock].scale = 1; ditems[ndock].target = 1; ditems[ndock].bounce0 = 0; ndock++;
    ditems[ndock].win = DI_SEARCH; ditems[ndock].scale = 1; ditems[ndock].target = 1; ditems[ndock].bounce0 = 0; ndock++;
    for (int i = 0; i < wm_nwin; i++) {
        window_t *w = &wm_wins[i];
        if (!(w->pinned || (w->open && wm_on_desk(w)))) continue;
        dockitem_t *d = &ditems[ndock++];
        d->win = i; d->scale = 1; d->target = 1; d->bounce0 = 0;
        for (int k = 0; k < nold; k++)
            if (old[k].win == i) { d->scale = old[k].scale; d->target = old[k].target; d->bounce0 = old[k].bounce0; }
    }
    for (int k = 0; k < 2 && k < nold; k++) { ditems[k].scale = old[k].scale; ditems[k].target = old[k].target; }
}

static void tray_layout(int *x, int *y, int *w, int *h);
static void pill_layout(int *x, int *y, int *w, int *h) {
    *w = 4 * 22 + 20; *h = ISLAND_H;
    *x = 12; *y = SH - ISLAND_GAP - ISLAND_H;
}

static void dock_layout(int *x, int *y, int *w, int *h) {
    int tx, ty, tw, th; tray_layout(&tx, &ty, &tw, &th);
    int px, py, pw, ph; pill_layout(&px, &py, &pw, &ph);
    int side = imax(tw, pw) + 12 + 14;
    int maxw = SW - 2 * side;
    dock_slot = DOCK_SLOT; dock_icon = DOCK_ICON;
    while (ndock * dock_slot + 2 * DOCK_PAD > maxw && dock_slot > 34) { dock_slot -= 2; dock_icon = dock_slot - 10; }
    *w = ndock * dock_slot + 2 * DOCK_PAD;
    if (*w > maxw) *w = maxw;
    *h = ISLAND_H;
    *x = (SW - *w) / 2;
    *y = SH - ISLAND_GAP - ISLAND_H;
}
static void dock_slot_rect(int k, int *sx, int *sy) {
    int x, y, w, h; dock_layout(&x, &y, &w, &h);
    *sx = x + DOCK_PAD + k * dock_slot;
    *sy = y;
}
static int dock_hit(int mx, int my) {
    int x, y, w, h; dock_layout(&x, &y, &w, &h);
    if (!in_rect(mx, my, x, y - 8, w, h + 8)) return -1;
    int k = (mx - x - DOCK_PAD) / dock_slot;
    if (mx < x + DOCK_PAD || k < 0 || k >= ndock) return -1;
    return k;
}

int shell_dock_rect(int win, rect_t *r) {
    for (int k = 0; k < ndock; k++) if (ditems[k].win == win) {
        int sx, sy; dock_slot_rect(k, &sx, &sy);
        r->x = sx + (dock_slot - dock_icon) / 2; r->y = sy + (ISLAND_H - dock_icon) / 2;
        r->w = dock_icon; r->h = dock_icon;
        return 1;
    }
    int x, y, w, h; dock_layout(&x, &y, &w, &h);
    r->x = x + w / 2 - 20; r->y = y + 8; r->w = 40; r->h = 40;
    return 0;
}

void shell_launched(int win) {
    for (int k = 0; k < ndock; k++) if (ditems[k].win == win) ditems[k].bounce0 = gui_now();
    gui_animating();
}

static void draw_dock(void) {
    panel_t *p = &P[P_DOCK];
    panel_glass(p);
    int x, y, w, h; dock_layout(&x, &y, &w, &h);
    uint64_t now = gui_now();
    int top = wm_topmost();
    gfx_clip_push(x - 4, y - 40, w + 8, h + 44);
    for (int k = 0; k < ndock; k++) {
        dockitem_t *d = &ditems[k];
        int sx = x + DOCK_PAD + k * dock_slot;
        int cxk = sx + dock_slot / 2;
        int size = (int)((float)dock_icon * d->scale + 0.5f);
        int bounce = 0;
        if (d->bounce0) {
            float t = anim_t(now, d->bounce0, 900);
            if (t < 1.0f) {
                float s = (float)sin((double)(t * 3.14159f * 3.0f));
                bounce = (int)(-(s < 0 ? -s : s) * 18.0f * (1.0f - t));
            }
        }
        int iy = y + ISLAND_H - 8 - size + bounce;
        if (d->win == DI_START) {
            int hot = hov == (H_DOCK | k) || start_open;
            if (hot) gfx_rrect(sx + 3, y + 6, dock_slot - 6, ISLAND_H - 12, 12, fill_hot, start_open ? PRESS_A : HOT_A);
            icon_draw(ICON_START, cxk - size / 2 + 2, iy + 2, size - 4, 255);
            continue;
        }
        if (d->win == DI_SEARCH) {
            int hot = hov == (H_DOCK | k);
            if (hot) gfx_rrect(sx + 3, y + 6, dock_slot - 6, ISLAND_H - 12, 12, fill_hot, HOT_A);
            glyph_draw_c(GLYPH_SEARCH, cxk, y + ISLAND_H / 2, 24, ink, 255);
            continue;
        }
        window_t *wn = &wm_wins[d->win];
        if (press == (H_DOCK | k)) size -= 4;
        icon_draw(wn->icon, cxk - size / 2, iy, size, 255);
        /* running indicator: a dot, a pill for the one in front */
        if (wn->open) {
            int active = d->win == top && !wn->minimized;
            if (active) gfx_rrect(cxk - 8, y + ISLAND_H - 5, 16, 3, 2, COL_ACCENT, 255);
            else        gfx_circle((float)cxk, (float)(y + ISLAND_H - 4), 2.0f, ink_dim, wn->minimized ? 130 : 220);
        }
    }
    gfx_clip_pop();
}

/* the label that floats over a hovered dock icon */
static char tip_text[64];
static int  tip_cx, tip_cy;
static void draw_tip(void) {
    panel_t *p = &P[P_TIP];
    if (!p->vis) return;
    panel_glass(p);
    text_c(p->x + p->w / 2, p->y + p->h / 2, tip_text, FONT_MEDIUM, 12, ink);
}

/* ===========================================================================
 *  The desktop switcher (left)
 * ===========================================================================*/
static int desk_occupied(int d) {
    for (int i = 0; i < wm_nwin; i++) if (wm_wins[i].open && wm_wins[i].desk == d) return 1;
    return 0;
}
static int pill_hit(int mx, int my) {
    int x, y, w, h; pill_layout(&x, &y, &w, &h);
    if (!in_rect(mx, my, x, y, w, h)) return -1;
    int k = (mx - x - 10) / 22;
    return (k >= 0 && k < NDESK) ? k : -1;
}
static void draw_pill(void) {
    panel_t *p = &P[P_PILL];
    panel_glass(p);
    int x, y, w, h; pill_layout(&x, &y, &w, &h);
    int cy = y + h / 2;
    for (int d = 0; d < NDESK; d++) {
        int cx = x + 10 + d * 22 + 11;
        int hot = hov == (H_PILL | d);
        if (hot) gfx_rrect(cx - 10, y + 10, 20, h - 20, 9, fill_hot, HOT_A);
        if (d == wm_desk)            gfx_rrect(cx - 7, cy - 3, 14, 6, 3, COL_ACCENT, 255);
        else if (desk_occupied(d))   gfx_circle((float)cx, (float)cy, 3.2f, ink_dim, 255);
        else                         gfx_ring((float)cx, (float)cy, 3.4f, 1.2f, ink_mute, 255);
    }
}

/* ===========================================================================
 *  The tray (right)
 * ===========================================================================*/
static int net_is_up(void) {
    struct netif *n = netif_default();
    return (n && n->link_up && net_ip != 0);
}
static void clock_strings(char *hm, char *date) {
    static const char *WD[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *MO[13] = { "", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    struct rtc_time t; rtc_now(&t);
    snprintf(hm, 8, "%02d:%02d", t.hour, t.min);
    int y = t.year ? t.year : 2026, m = (t.mon >= 1 && t.mon <= 12) ? t.mon : 1, d = t.day ? t.day : 1;
    int mm = m, yy = y;
    if (mm < 3) { mm += 12; yy--; }
    int K = yy % 100, J = yy / 100;
    int h = (d + (13 * (mm + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    int wd = (h + 6) % 7;
    snprintf(date, 20, "%s %d %s", WD[wd], d, MO[m]);
}

/* group geometry: 0 = recording dot (if any), 1 = status icons, 2 = clock */
static int tray_group(int g, int *gx, int *gw) {
    int x, y, w, h; tray_layout(&x, &y, &w, &h);
    int cur = x + 8;
    int rec = rec_active();
    battery_t b; int bat = power_battery(&b) && b.present;
    char hm[8], dt[20]; clock_strings(hm, dt);
    int clockw = imax(font_width(FONT_SEMIBOLD, 13, hm, -1), font_width(FONT_REGULAR, 11, dt, -1)) + 40;
    int widths[3] = { rec ? 30 : 0, (bat ? 3 : 2) * 26 + 14, clockw };
    for (int k = 0; k < 3; k++) {
        if (k == g) { *gx = cur; *gw = widths[k]; return widths[k] > 0; }
        cur += widths[k] + (widths[k] ? 4 : 0);
    }
    return 0;
}

static void tray_layout(int *x, int *y, int *w, int *h) {
    int rec = rec_active();
    battery_t b; int bat = power_battery(&b) && b.present;
    char hm[8], dt[20]; clock_strings(hm, dt);
    int clockw = imax(font_width(FONT_SEMIBOLD, 13, hm, -1), font_width(FONT_REGULAR, 11, dt, -1)) + 40;
    *w = 16 + (rec ? 34 : 0) + (bat ? 3 : 2) * 26 + 14 + 4 + clockw;
    *h = ISLAND_H;
    *x = SW - 12 - *w;
    *y = SH - ISLAND_GAP - ISLAND_H;
}

static int tray_hit(int mx, int my) {
    int x, y, w, h; tray_layout(&x, &y, &w, &h);
    if (!in_rect(mx, my, x, y, w, h)) return -1;
    for (int g = 0; g < 3; g++) {
        int gx, gw;
        if (tray_group(g, &gx, &gw) && mx >= gx && mx < gx + gw) return g;
    }
    return -1;
}

static int wifi_glyph(void) {
    if (!net_is_up()) return GLYPH_WIFI_OFF;
    const struct wifi_bss *b = wifi_current();
    if (!b) return GLYPH_ETHERNET;
    int s = wifi_signal();
    return s > -55 ? GLYPH_WIFI : s > -67 ? GLYPH_WIFI_2 : s > -78 ? GLYPH_WIFI_1 : GLYPH_WIFI_0;
}
static int vol_glyph(void) {
    if (mixer_muted(MIX_MASTER) || audio_volume() == 0) return GLYPH_VOL_MUTE;
    int v = audio_volume();
    return v > 66 ? GLYPH_VOL_2 : v > 25 ? GLYPH_VOL_1 : GLYPH_VOL_0;
}

static void draw_battery_glyph(int cx, int cy, uint32_t c) {
    battery_t b;
    if (!(power_battery(&b) && b.present)) return;
    glyph_draw_c(b.charging ? GLYPH_BATTERY_CHG : GLYPH_BATTERY, cx, cy, 20, c, 255);
    if (!b.charging) {
        int fw = 10 * b.percent / 100; if (fw < 1) fw = 1;
        gfx_rrect(cx - 7, cy - 3, fw, 6, 1, b.percent <= 15 ? COL_BAD : c, 255);
    }
}

static void draw_tray(void) {
    panel_t *p = &P[P_TRAY];
    panel_glass(p);
    int x, y, w, h; tray_layout(&x, &y, &w, &h);
    int cy = y + h / 2;
    int gx, gw;
    if (tray_group(0, &gx, &gw)) {
        int hot = hov == (H_TRAY | 0);
        if (hot) gfx_rrect(gx, y + 8, gw, h - 16, 12, fill_hot, HOT_A);
        float pulse = 0.5f + 0.5f * (float)sin((double)(gui_now() % 1600) * 0.003927);
        gfx_circle((float)(gx + gw / 2), (float)cy, 7.0f + pulse * 2.0f, COL_BAD, 60);
        gfx_circle((float)(gx + gw / 2), (float)cy, 5.0f, COL_BAD, 255);
    }
    if (tray_group(1, &gx, &gw)) {
        int hot = hov == (H_TRAY | 1) || qs_open;
        if (hot) gfx_rrect(gx, y + 8, gw, h - 16, 12, fill_hot, qs_open ? PRESS_A : HOT_A);
        int ix = gx + 7 + 13;
        glyph_draw_c(wifi_glyph(), ix, cy, 20, net_is_up() ? ink : ink_mute, 255); ix += 26;
        glyph_draw_c(vol_glyph(), ix, cy, 20, ink, 255); ix += 26;
        draw_battery_glyph(ix, cy, ink);
    }
    if (tray_group(2, &gx, &gw)) {
        int hot = hov == (H_TRAY | 2) || nc_open;
        if (hot) gfx_rrect(gx, y + 8, gw, h - 16, 12, fill_hot, nc_open ? PRESS_A : HOT_A);
        char hm[8], dt[20]; clock_strings(hm, dt);
        int rx = gx + gw - 10;
        text_r(rx, cy - 8, hm, FONT_SEMIBOLD, 13, ink);
        text_r(rx, cy + 9, dt, FONT_REGULAR, 11, ink_dim);
        int unread = notify_unread();
        glyph_draw_c(unread ? GLYPH_BELL_DOT : (dnd ? GLYPH_BELL_OFF : GLYPH_BELL), gx + 16, cy, 18,
                     unread ? ink : ink_dim, 255);
        if (unread) gfx_circle((float)(gx + 22), (float)(cy - 6), 3.5f, COL_ACCENT, 255);
    }
}

/* ===========================================================================
 *  Context menus
 * ===========================================================================*/
enum { MF_SEP = 1, MF_DANGER = 2, MF_OFF = 4, MF_CHECK = 8 };
typedef struct { const char *label; int glyph; int act; int flags; } mitem_t;
#define MAXMI 14
static mitem_t mi[MAXMI];
static int     nmi;
static int     menu_open, menu_x, menu_y, menu_up, menu_ctx;
static uint64_t menu_t0;
static char    menu_lbl[MAXMI][40];
#define MI_H   30
#define SEP_H  9

enum {
    A_NONE = 0, A_WALLPAPER, A_DISPLAY, A_TERMINAL, A_FILES, A_ARRANGE,
    A_DOCK_CLOSE, A_DOCK_PIN, A_DOCK_MIN, A_DOCK_OPEN,
    A_WIN_MIN, A_WIN_MAX, A_WIN_LEFT, A_WIN_RIGHT, A_WIN_NEXTDESK, A_WIN_CLOSE,
    A_PWR_LOCK, A_PWR_SIGNOUT, A_PWR_RESTART, A_PWR_OFF, A_SETTINGS,
    A_ICON_OPEN, A_ICON_REMOVE,
};

static void menu_add(const char *label, int glyph, int act, int flags) {
    if (nmi >= MAXMI) return;
    strncpy(menu_lbl[nmi], label ? label : "", sizeof(menu_lbl[nmi]) - 1);
    menu_lbl[nmi][sizeof(menu_lbl[nmi]) - 1] = 0;
    mi[nmi].label = menu_lbl[nmi]; mi[nmi].glyph = glyph; mi[nmi].act = act; mi[nmi].flags = flags;
    nmi++;
}
static void menu_size(int *w, int *h) {
    int mw = 190, mh = 10;
    for (int i = 0; i < nmi; i++) {
        if (mi[i].flags & MF_SEP) { mh += SEP_H; continue; }
        int tw = font_width(FONT_REGULAR, 13, mi[i].label, -1) + 64;
        if (tw > mw) mw = tw;
        mh += MI_H;
    }
    *w = mw; *h = mh;
}
static void menu_rect(int *x, int *y, int *w, int *h) {
    menu_size(w, h);
    int mx = menu_x, my = menu_up ? menu_y - *h - 8 : menu_y;
    if (mx + *w > SW - 8) mx = SW - 8 - *w;
    if (mx < 8) mx = 8;
    if (my + *h > SH - 8) my = SH - 8 - *h;
    if (my < 8) my = 8;
    *x = mx; *y = my;
}
static int menu_item_at(int mx, int my) {
    int x, y, w, h; menu_rect(&x, &y, &w, &h);
    if (!in_rect(mx, my, x, y, w, h)) return -2;
    int cy = y + 5;
    for (int i = 0; i < nmi; i++) {
        int ih = (mi[i].flags & MF_SEP) ? SEP_H : MI_H;
        if (my >= cy && my < cy + ih) return (mi[i].flags & (MF_SEP | MF_OFF)) ? -1 : i;
        cy += ih;
    }
    return -1;
}
static void menu_show(int x, int y, int up, int ctx) {
    menu_open = 1; menu_x = x; menu_y = y; menu_up = up; menu_ctx = ctx;
    menu_t0 = gui_now();
    P[P_MENU].dirty = 1; P[P_MENU].env.w = 0;
    gui_animating();
}
static void menu_close(void) {
    if (!menu_open) return;
    menu_open = 0;
    dmg_panel(P_MENU);
}
static void draw_menu(void) {
    panel_t *p = &P[P_MENU];
    if (!p->vis) return;
    panel_glass(p);
    int x = p->x, y = p->y + 5;
    for (int i = 0; i < nmi; i++) {
        if (mi[i].flags & MF_SEP) {
            glass_rule(x + 12, y + SEP_H / 2, p->w - 24);
            y += SEP_H;
            continue;
        }
        int hot = hov == (H_MENU | i);
        int off = mi[i].flags & MF_OFF;
        uint32_t tc = off ? ink_mute : ((mi[i].flags & MF_DANGER) && hot ? 0xFFFFFF : ink);
        if (hot && !off) gfx_rrect(x + 5, y, p->w - 10, MI_H, 7,
                                   (mi[i].flags & MF_DANGER) ? COL_BAD : COL_ACCENT, 255);
        if (hot && !off && !(mi[i].flags & MF_DANGER)) tc = 0xFFFFFF;
        if (mi[i].glyph >= 0) glyph_draw_c(mi[i].glyph, x + 22, y + MI_H / 2, 16, (mi[i].flags & MF_DANGER) && !hot ? COL_BAD : tc, 255);
        if (mi[i].flags & MF_CHECK) glyph_draw_c(GLYPH_CHECK, x + p->w - 22, y + MI_H / 2, 16, tc, 255);
        text_l(x + 40, y + MI_H / 2, mi[i].label, FONT_REGULAR, 13, (mi[i].flags & MF_DANGER) && !hot ? COL_BAD : tc);
        y += MI_H;
    }
}

/* ===========================================================================
 *  Start
 * ===========================================================================*/
enum { SM_PINNED = 0, SM_ALL, SM_SEARCH };
static int  sm_mode;
static char sm_query[40];
static int  sm_qlen;
static int  sm_scroll;              /* rows, All apps */
static int  sm_sel;                 /* selected search result */
static const char *PINS[] = {
    "File Explorer", "Browser", "Terminal", "Code", "Notepad", "Calculator",
    "Photos", "Media", "Music", "Calendar", "Clock", "Settings",
    "Task Manager", "Sheets", "Paint", "Software", "Solitaire", "Minesweeper",
};
#define NPINS ((int)(sizeof(PINS) / sizeof(PINS[0])))
static int pin_idx[NPINS];
static int npin;
static int all_idx[MAX_WIN];        /* every app, by name */
static int nall;

/* search results: apps first, then files */
#define SR_MAX 9
typedef struct { int app; char path[FS_PATH_MAX]; int dir; } sres_t;
static sres_t sres[SR_MAX];
static int    nsres;

/* "Recommended": the files most recently written under home */
#define REC_MAX 4
static char recent_path[REC_MAX][FS_PATH_MAX];
static int  nrec;

static int ci_contains(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j]) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            j++;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}
static int ci_prefix(const char *hay, const char *needle) {
    for (int j = 0; needle[j]; j++) {
        char a = hay[j], b = needle[j];
        if (!a) return 0;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static void sm_scan(fs_node *d, int depth, int *nf) {
    if (!d || depth > 6) return;
    for (fs_node *c = d->child; c && nsres < SR_MAX && *nf < 5; c = c->next) {
        if (c->name[0] == '.') continue;
        if (ci_contains(c->name, sm_query) && fs_may(c, FS_PERM_READ)) {
            char p[FS_PATH_MAX];
            fs_abspath(c, p, sizeof p);
            int dup = 0;
            for (int i = 0; i < nsres; i++) if (sres[i].app < 0 && strcmp(sres[i].path, p) == 0) dup = 1;
            if (!dup) {
                sres[nsres].app = -1; sres[nsres].dir = c->is_dir;
                snprintf(sres[nsres].path, FS_PATH_MAX, "%s", p);
                nsres++; (*nf)++;
            }
        }
        if (c->is_dir) sm_scan(c, depth + 1, nf);
    }
}

/* Called when the query changes, not per frame: it walks the tree. Apps whose
 * name starts with the query rank above ones that merely contain it, which is
 * what makes "te" mean Terminal and not Device Manager. */
static void sm_search(void) {
    nsres = 0; sm_sel = 0;
    if (!sm_qlen) return;
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < nall && nsres < SR_MAX; k++) {
            window_t *w = &wm_wins[all_idx[k]];
            int pre = ci_prefix(w->title, sm_query);
            if (pass == 0 ? !pre : (pre || !ci_contains(w->title, sm_query))) continue;
            sres[nsres].app = all_idx[k]; sres[nsres].path[0] = 0; sres[nsres].dir = 0;
            nsres++;
        }
    if (sm_qlen >= 2) {
        int nf = 0;
        const char *home = users_home();
        if (home && home[0]) sm_scan(fs_lookup(home), 0, &nf);
        sm_scan(fs_root(), 0, &nf);
    }
}

static void rec_scan(fs_node *d, int depth) {
    if (!d || depth > 4) return;
    for (fs_node *c = d->child; c; c = c->next) {
        if (c->name[0] == '.') continue;
        if (c->is_dir) { rec_scan(c, depth + 1); continue; }
        char p[FS_PATH_MAX];
        fs_abspath(c, p, sizeof p);
        /* insertion into the top REC_MAX by mtime */
        int pos = nrec;
        for (int i = 0; i < nrec; i++) {
            fs_node *o = fs_lookup(recent_path[i]);
            if (!o || c->mtime > o->mtime) { pos = i; break; }
        }
        if (pos >= REC_MAX) continue;
        for (int i = (nrec < REC_MAX ? nrec : REC_MAX - 1); i > pos; i--)
            memcpy(recent_path[i], recent_path[i - 1], FS_PATH_MAX);
        snprintf(recent_path[pos], FS_PATH_MAX, "%s", p);
        if (nrec < REC_MAX) nrec++;
    }
}

static int file_icon(const char *name, int dir) {
    if (dir) return ICON_FOLDER;
    const char *dot = 0;
    for (const char *q = name; *q; q++) if (*q == '.') dot = q;
    if (!dot) return ICON_FILE_TEXT;
    const char *e = dot + 1;
    if (!strcasecmp(e, "png") || !strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg") || !strcasecmp(e, "gif") || !strcasecmp(e, "bmp") || !strcasecmp(e, "tga")) return ICON_FILE_IMAGE;
    if (!strcasecmp(e, "mp3") || !strcasecmp(e, "wav")) return ICON_FILE_AUDIO;
    if (!strcasecmp(e, "c") || !strcasecmp(e, "h") || !strcasecmp(e, "js") || !strcasecmp(e, "py") || !strcasecmp(e, "asm") || !strcasecmp(e, "sh") || !strcasecmp(e, "bolt")) return ICON_FILE_CODE;
    if (!strcasecmp(e, "html") || !strcasecmp(e, "htm") || !strcasecmp(e, "css")) return ICON_FILE_WEB;
    if (!strcasecmp(e, "csv")) return ICON_FILE_SHEET;
    if (!strcasecmp(e, "exe") || !strcasecmp(e, "elf")) return ICON_FILE_EXE;
    if (!strcasecmp(e, "zip") || !strcasecmp(e, "tar") || !strcasecmp(e, "gz") || !strcasecmp(e, "bpk")) return ICON_FILE_ARCHIVE;
    return ICON_FILE_TEXT;
}

#define SM_W_MAX 640
#define SM_H_MAX 620
#define SM_PAD   24
#define SM_COLS  6
static void start_rect(int *x, int *y, int *w, int *h) {
    *w = imin(SM_W_MAX, SW - 32);
    *h = imin(SM_H_MAX, SH - SHELF_H - 24);
    *x = (SW - *w) / 2;
    *y = SH - SHELF_H - *h - 6;
}
/* the rectangles inside Start, relative to its frame */
static void sm_search_rect(int x, int y, int w, int *fx, int *fy, int *fw, int *fh) {
    *fx = x + SM_PAD; *fy = y + 22; *fw = w - 2 * SM_PAD; *fh = 38;
}
static int sm_cell_w(int w) { return (w - 2 * SM_PAD) / SM_COLS; }
#define SM_CELL_H 86
static int sm_grid_top(int y) { return y + 22 + 38 + 52; }
static int sm_footer_h(void) { return 60; }
static void sm_allbtn_rect(int x, int y, int w, int *bx, int *by, int *bw, int *bh) {
    const char *l = sm_mode == SM_ALL ? "Back" : "All apps";
    *bw = font_width(FONT_MEDIUM, 12, l, -1) + 38; *bh = 26;
    *bx = x + w - SM_PAD - *bw; *by = y + 22 + 38 + 14;
}
static void sm_power_rect(int x, int y, int w, int h, int *bx, int *by, int *bw, int *bh) {
    *bw = 36; *bh = 36; *bx = x + w - SM_PAD - 36 + 6; *by = y + h - sm_footer_h() / 2 - 18;
}
static int sm_rows_visible(int y, int h) {
    return (y + h - sm_footer_h() - 8 - sm_grid_top(y)) / SM_CELL_H;
}

static void start_open_now(void) {
    start_open = 1; start_closing = 0; start_t0 = gui_now();
    sm_mode = SM_PINNED; sm_query[0] = 0; sm_qlen = 0; nsres = 0; sm_scroll = 0; sm_sel = 0;
    nrec = 0;
    const char *home = users_home();
    rec_scan(fs_lookup(home && home[0] ? home : "/home"), 0);
    P[P_START].dirty = 1; P[P_START].env.w = 0;
    qs_open = nc_open = 0;
    menu_close();
    gui_animating();
}
static void start_close(void) {
    if (!start_open) return;
    start_open = 0; start_closing = 1; start_t0 = gui_now();
    gui_animating();
}
void shell_toggle_start(void) {
    if (start_open) start_close();
    else start_open_now();
}

static void sm_launch_app(int i) {
    start_close();
    gui_open(&wm_wins[i]);
}
static void sm_launch_path(const char *path) {
    start_close();
    fs_node *n = fs_lookup(path);
    if (n) files_launch(n);
}

/* Where search result k sits, and the section header above it if any:
 * "Best match" over the first, then "Apps" and "Files" where those start. */
static void sr_row(int k, int y0, int *ry, int *rh, const char **hdr) {
    int yy = y0;
    for (int i = 0; i <= k && i < nsres; i++) {
        const char *h = 0;
        if (i == 0) h = "Best match";
        else if (sres[i].app >= 0 && i == 1) h = "Apps";
        else if (sres[i].app < 0 && (i == 1 || sres[i - 1].app >= 0)) h = "Files";
        if (h) yy += 28;
        int hh = i == 0 ? 64 : 40;
        if (i == k) { *ry = yy; *rh = hh; *hdr = h; return; }
        yy += hh + 2;
    }
    *ry = yy; *rh = 40; *hdr = 0;
}

static void draw_app_cell(int i, int cx, int top, int cw, int hot) {
    window_t *w = &wm_wins[i];
    if (hot) gfx_rrect(cx - cw / 2 + 4, top, cw - 8, SM_CELL_H - 6, 12, fill_hot, HOT_A);
    icon_draw(w->icon, cx - 22, top + 9, 44, 255);
    char lbl[40]; snprintf(lbl, sizeof lbl, "%s", w->title);
    int tw = font_width(FONT_REGULAR, 12, lbl, -1);
    if (tw > cw - 10) tw = cw - 10;
    font_draw_fit(FONT_REGULAR, 12, cx - tw / 2, top + 70, lbl, cw - 10, ink, 255);
}

static void draw_start(void) {
    panel_t *p = &P[P_START];
    if (!p->vis) return;
    panel_glass(p);
    int x = p->x, y = p->y, w = p->w, h = p->h;
    int a = p->alpha;
    (void)a;
    /* search field */
    int fx, fy, fw, fh; sm_search_rect(x, y, w, &fx, &fy, &fw, &fh);
    gfx_rrect(fx, fy, fw, fh, fh / 2, g_theme.dark ? 0x000000 : 0xFFFFFF, g_theme.dark ? 70 : 150);
    gfx_rrect_stroke(fx, fy, fw, fh, fh / 2, 1, g_theme.dark ? 0xFFFFFF : 0x000000, sm_qlen ? 60 : 24);
    glyph_draw_c(GLYPH_SEARCH, fx + 22, fy + fh / 2, 16, ink_dim, 255);
    if (sm_qlen) {
        int tw = text_l(fx + 40, fy + fh / 2, sm_query, FONT_REGULAR, 14, ink);
        if ((gui_now() / 530) % 2 == 0) gfx_fill(fx + 41 + tw, fy + 10, 2, fh - 20, COL_ACCENT);
    } else {
        text_l(fx + 40, fy + fh / 2, "Search apps, settings and files", FONT_REGULAR, 14, ink_mute);
    }

    int gy = sm_grid_top(y);
    int cw = sm_cell_w(w);
    if (sm_mode == SM_SEARCH) {
        int y0 = fy + fh + 14;
        if (!nsres) {
            glyph_draw_c(GLYPH_SEARCH, x + w / 2, y0 + 74, 24, ink_mute, 255);
            text_c(x + w / 2, y0 + 108, "Nothing matches that", FONT_MEDIUM, 14, ink_dim);
        }
        for (int k = 0; k < nsres; k++) {
            int big = (k == 0);
            int ry, rh; const char *hdr;
            sr_row(k, y0, &ry, &rh, &hdr);
            if (ry + rh > y + h - sm_footer_h()) break;
            if (hdr) text_l(x + SM_PAD + 2, ry - 13, hdr, FONT_SEMIBOLD, 12, ink_dim);
            int sel = (k == sm_sel), hot = hov == (H_START | 0x80 | k);
            if (sel || hot) gfx_rrect(x + SM_PAD - 8, ry, w - 2 * SM_PAD + 16, rh, 10,
                                      sel ? COL_ACCENT : fill_hot, sel ? (g_theme.dark ? 90 : 60) : HOT_A);
            int isz = big ? 40 : 24;
            const char *name, *sub;
            char leaf[FS_NAME_MAX + 1];
            int icon;
            if (sres[k].app >= 0) {
                name = wm_wins[sres[k].app].title; sub = "App"; icon = wm_wins[sres[k].app].icon;
            } else {
                const char *pp = sres[k].path, *lf = pp;
                for (const char *q = pp; *q; q++) if (*q == '/') lf = q + 1;
                snprintf(leaf, sizeof leaf, "%s", lf);
                name = leaf; sub = sres[k].path; icon = file_icon(leaf, sres[k].dir);
            }
            icon_draw(icon, x + SM_PAD, ry + (rh - isz) / 2, isz, 255);
            int tx = x + SM_PAD + isz + 12;
            if (big) {
                text_fit_l(tx, ry + rh / 2 - 9, name, w - 2 * SM_PAD - isz - 20, FONT_SEMIBOLD, 15, ink);
                text_fit_l(tx, ry + rh / 2 + 11, sub, w - 2 * SM_PAD - isz - 20, FONT_REGULAR, 12, ink_dim);
            } else {
                int nw = text_fit_l(tx, ry + rh / 2, name, (w - 2 * SM_PAD) / 2, FONT_REGULAR, 13, ink);
                int sx2 = tx + nw + 14;
                text_fit_l(sx2, ry + rh / 2, sub, x + w - SM_PAD - sx2, FONT_REGULAR, 12, ink_mute);
            }
        }
    } else {
        int bx, by, bw, bh; sm_allbtn_rect(x, y, w, &bx, &by, &bw, &bh);
        text_l(x + SM_PAD + 4, by + bh / 2, sm_mode == SM_ALL ? "All apps" : "Pinned", FONT_SEMIBOLD, 14, ink);
        int bhot = hov == (H_START | 1);
        gfx_rrect(bx, by, bw, bh, bh / 2, fill_hot, bhot ? HOT_A + 14 : HOT_A - 8);
        if (sm_mode == SM_ALL) {
            glyph_draw_c(GLYPH_CHEV_L, bx + 14, by + bh / 2, 16, ink, 255);
            text_l(bx + 24, by + bh / 2, "Back", FONT_MEDIUM, 12, ink);
        } else {
            text_l(bx + 12, by + bh / 2, "All apps", FONT_MEDIUM, 12, ink);
            glyph_draw_c(GLYPH_CHEV_R, bx + bw - 15, by + bh / 2, 16, ink, 255);
        }
        int rows = sm_rows_visible(y, h);
        if (sm_mode == SM_PINNED) {
            int prow = rows >= 5 ? 3 : (rows > 2 ? rows - 2 : rows);
            for (int k = 0; k < npin && k < prow * SM_COLS; k++) {
                int col = k % SM_COLS, row = k / SM_COLS;
                int cx = x + SM_PAD + col * cw + cw / 2, top = gy + row * SM_CELL_H;
                draw_app_cell(pin_idx[k], cx, top, cw, hov == (H_START | 0x40 | k));
            }
            /* Recommended */
            int ry = gy + prow * SM_CELL_H + 6;
            if (nrec && ry + 90 < y + h - sm_footer_h()) {
                text_l(x + SM_PAD + 4, ry + 10, "Recommended", FONT_SEMIBOLD, 14, ink);
                ry += 28;
                int colw = (w - 2 * SM_PAD) / 2;
                for (int k = 0; k < nrec; k++) {
                    int rx = x + SM_PAD + (k % 2) * colw, ryy = ry + (k / 2) * 52;
                    if (ryy + 48 > y + h - sm_footer_h()) break;
                    int hot = hov == (H_START | 0xA0 | k);
                    if (hot) gfx_rrect(rx, ryy, colw - 8, 48, 10, fill_hot, HOT_A);
                    const char *pp = recent_path[k], *lf = pp;
                    for (const char *q = pp; *q; q++) if (*q == '/') lf = q + 1;
                    icon_draw(file_icon(lf, 0), rx + 8, ryy + 8, 32, 255);
                    text_fit_l(rx + 50, ryy + 17, lf, colw - 66, FONT_MEDIUM, 13, ink);
                    char dir[FS_PATH_MAX];
                    int dl = (int)(lf - pp) - 1; if (dl < 1) dl = 1;
                    if (dl > (int)sizeof(dir) - 1) dl = (int)sizeof(dir) - 1;
                    memcpy(dir, pp, (size_t)dl); dir[dl] = 0;
                    text_fit_l(rx + 50, ryy + 33, dir, colw - 66, FONT_REGULAR, 11, ink_mute);
                }
            }
        } else {
            int maxscroll = (nall + SM_COLS - 1) / SM_COLS - rows;
            if (sm_scroll > maxscroll) sm_scroll = maxscroll > 0 ? maxscroll : 0;
            gfx_clip_push(x, gy, w, rows * SM_CELL_H);
            for (int k = sm_scroll * SM_COLS; k < nall; k++) {
                int col = k % SM_COLS, row = k / SM_COLS - sm_scroll;
                if (row >= rows) break;
                int cx = x + SM_PAD + col * cw + cw / 2, top = gy + row * SM_CELL_H;
                draw_app_cell(all_idx[k], cx, top, cw, hov == (H_START | 0x100000 | k));
            }
            gfx_clip_pop();
            if (maxscroll > 0) {             /* a thin scroll indicator */
                int th = rows * SM_CELL_H - 8, ty = gy + 4;
                int kh = th * rows / (rows + maxscroll);
                int ky = ty + (th - kh) * sm_scroll / maxscroll;
                gfx_rrect(x + w - 10, ky, 4, kh, 2, ink_mute, 160);
            }
        }
    }
    /* footer: who is signed in, and the power button */
    int fy0 = y + h - sm_footer_h();
    gfx_rrect4(x, fy0, w, sm_footer_h(), 0, 0, p->r, p->r, g_theme.dark ? 0x000000 : 0xFFFFFF, g_theme.dark ? 46 : 70);
    gfx_fill_a(x, fy0, w, 1, g_theme.dark ? 0xFFFFFF : 0x000000, g_theme.dark ? 22 : 16);
    const char *who = users_current();
    char init[2] = { (char)(who[0] >= 'a' && who[0] <= 'z' ? who[0] - 32 : (who[0] ? who[0] : 'G')), 0 };
    int ax = x + SM_PAD + 14, ay = fy0 + sm_footer_h() / 2;
    gfx_circle((float)ax, (float)ay, 15.0f, COL_ACCENT, 255);
    gfx_circle((float)ax, (float)ay - 3.0f, 13.0f, 0xFFFFFF, 30);
    text_c(ax, ay, init, FONT_SEMIBOLD, 14, 0xFFFFFF);
    text_l(ax + 24, ay, who[0] ? who : "Guest", FONT_SEMIBOLD, 13, ink);
    int pbx, pby, pbw, pbh; sm_power_rect(x, y, w, h, &pbx, &pby, &pbw, &pbh);
    if (hov == (H_START | 2)) gfx_rrect(pbx, pby, pbw, pbh, 10, fill_hot, HOT_A);
    glyph_draw_c(GLYPH_POWER, pbx + pbw / 2, pby + pbh / 2, 20, ink, 255);
}

/* Start hit test: returns an H_START id or 0 */
static int start_hit(int mx, int my) {
    panel_t *p = &P[P_START];
    if (!start_open || !p->vis || !in_rect(mx, my, p->x, p->y, p->w, p->h)) return 0;
    int x = p->x, y = p->y, w = p->w, h = p->h;
    int fx, fy, fw, fh; sm_search_rect(x, y, w, &fx, &fy, &fw, &fh);
    if (in_rect(mx, my, fx, fy, fw, fh)) return H_START | 3;
    int pbx, pby, pbw, pbh; sm_power_rect(x, y, w, h, &pbx, &pby, &pbw, &pbh);
    if (in_rect(mx, my, pbx, pby, pbw, pbh)) return H_START | 2;
    int gy = sm_grid_top(y), cw = sm_cell_w(w);
    if (sm_mode == SM_SEARCH) {
        for (int k = 0; k < nsres; k++) {
            int ry, rh; const char *hdr;
            sr_row(k, fy + fh + 14, &ry, &rh, &hdr);
            if (in_rect(mx, my, x + SM_PAD - 8, ry, w - 2 * SM_PAD + 16, rh)) return H_START | 0x80 | k;
        }
        return H_START;
    }
    int bx, by, bw, bh; sm_allbtn_rect(x, y, w, &bx, &by, &bw, &bh);
    if (in_rect(mx, my, bx, by, bw, bh)) return H_START | 1;
    int rows = sm_rows_visible(y, h);
    if (sm_mode == SM_PINNED) {
        int prow = rows >= 5 ? 3 : (rows > 2 ? rows - 2 : rows);
        if (my >= gy && my < gy + prow * SM_CELL_H && mx >= x + SM_PAD && mx < x + SM_PAD + SM_COLS * cw) {
            int k = ((my - gy) / SM_CELL_H) * SM_COLS + (mx - x - SM_PAD) / cw;
            if (k < npin) return H_START | 0x40 | k;
        }
        int ry = gy + prow * SM_CELL_H + 6 + 28;
        int colw = (w - 2 * SM_PAD) / 2;
        for (int k = 0; k < nrec; k++) {
            int rx = x + SM_PAD + (k % 2) * colw, ryy = ry + (k / 2) * 52;
            if (in_rect(mx, my, rx, ryy, colw - 8, 48)) return H_START | 0xA0 | k;
        }
    } else {
        if (my >= gy && my < gy + rows * SM_CELL_H && mx >= x + SM_PAD && mx < x + SM_PAD + SM_COLS * cw) {
            int k = ((my - gy) / SM_CELL_H + sm_scroll) * SM_COLS + (mx - x - SM_PAD) / cw;
            if (k < nall) return H_START | 0x100000 | k;
        }
    }
    return H_START;
}

/* ===========================================================================
 *  Quick Settings
 * ===========================================================================*/
#define QS_W 352
enum { QT_WIFI = 0, QT_FOCUS, QT_DARK, QT_MUTE, QT_REC, QT_SHOT, QT_COUNT };
static int qs_slider_drag = -1;     /* 0 brightness, 1 volume */

static void qs_rect(int *x, int *y, int *w, int *h) {
    int tx, ty, tw, th; tray_layout(&tx, &ty, &tw, &th);
    *w = QS_W; *h = 16 + 2 * 74 + 8 + 2 * 46 + 8 + 48;
    *x = tx + tw - *w; if (*x < 8) *x = 8;
    *y = ty - *h - 10;
}
static void qs_tile_rect(int k, int x, int y, int *tx, int *ty, int *tw, int *th) {
    int gap = 10;
    *tw = (QS_W - 32 - 2 * gap) / 3; *th = 66;
    *tx = x + 16 + (k % 3) * (*tw + gap);
    *ty = y + 16 + (k / 3) * (*th + 8);
}
static void qs_slider_rect(int k, int x, int y, int *sx, int *sy, int *sw) {
    *sx = x + 52; *sw = QS_W - 52 - 24;
    *sy = y + 16 + 2 * 74 + 8 + k * 46 + 23;
}
static int qs_tile_on(int k) {
    switch (k) {
    case QT_WIFI:  return net_is_up();
    case QT_FOCUS: return dnd;
    case QT_DARK:  return g_theme.dark;
    case QT_MUTE:  return mixer_muted(MIX_MASTER);
    case QT_REC:   return rec_active();
    default:       return 0;
    }
}
static void draw_qs(void) {
    panel_t *p = &P[P_QS];
    if (!p->vis) return;
    panel_glass(p);
    int x = p->x, y = p->y;
    static const char *NAMES[QT_COUNT] = { "Wi-Fi", "Focus", "Dark mode", "Mute", "Record", "Screenshot" };
    const char *netname = wifi_current() ? "Wi-Fi" : "Network";
    static const int GL[QT_COUNT] = { GLYPH_WIFI, GLYPH_MOON, GLYPH_THEME, GLYPH_VOL_MUTE, GLYPH_RECORD, GLYPH_CAMERA };
    for (int k = 0; k < QT_COUNT; k++) {
        int tx, ty, tw, th; qs_tile_rect(k, x, y, &tx, &ty, &tw, &th);
        int on = qs_tile_on(k), hot = hov == (H_QS | k);
        uint32_t bg = on ? COL_ACCENT : (g_theme.dark ? 0xFFFFFF : 0x000000);
        int ba = on ? 255 : (g_theme.dark ? 22 : 14);
        gfx_rrect(tx, ty, tw, th, 14, bg, ba);
        if (hot) gfx_rrect(tx, ty, tw, th, 14, on ? 0xFFFFFF : fill_hot, on ? 40 : HOT_A);
        uint32_t fc = on ? 0xFFFFFF : ink;
        int g = GL[k];
        if (k == QT_WIFI) g = net_is_up() ? wifi_glyph() : GLYPH_WIFI_OFF;
        if (k == QT_MUTE && !on) g = vol_glyph();
        glyph_draw(g, tx + 12, ty + 11, 20, fc, 255);
        text_l(tx + 12, ty + 44, k == QT_WIFI ? netname : NAMES[k], FONT_SEMIBOLD, 12, fc);
        const char *sub = 0;
        if (k == QT_WIFI) {
            const struct wifi_bss *b = wifi_current();
            sub = net_is_up() ? (b ? b->ssid : "Ethernet") : "Off";
        } else if (k == QT_REC && on) sub = "On";
        if (sub) text_fit_l(tx + 12, ty + 57, sub, tw - 20, FONT_REGULAR, 11, on ? 0xFFFFFF : ink_dim);
    }
    for (int k = 0; k < 2; k++) {
        int sx, sy, sw; qs_slider_rect(k, x, y, &sx, &sy, &sw);
        int v = k == 0 ? backlight_get() : audio_volume();
        int muted = k == 1 && mixer_muted(MIX_MASTER);
        glyph_draw_c(k == 0 ? GLYPH_SUN : vol_glyph(), x + 30, sy, 20, ink, 255);
        int hot = hov == (H_QS | 0x10 | k) || qs_slider_drag == k;
        gfx_rrect(sx, sy - 3, sw, 6, 3, g_theme.dark ? 0xFFFFFF : 0x000000, g_theme.dark ? 36 : 24);
        int fw = sw * v / 100;
        gfx_rrect(sx, sy - 3, fw, 6, 3, muted ? ink_mute : COL_ACCENT, 255);
        int kx = sx + fw;
        gfx_shadow(kx - 10, sy - 10, 20, 20, 10, 6, 2, 0, 90);
        gfx_circle((float)kx, (float)sy, hot ? 11.0f : 10.0f, 0xFFFFFF, 255);
        gfx_ring((float)kx, (float)sy, hot ? 11.0f : 10.0f, 1.0f, 0x000000, 30);
    }
    /* footer */
    int fy0 = y + p->h - 48;
    glass_rule(x + 16, fy0, QS_W - 32);
    battery_t b;
    char bt[40];
    if (power_battery(&b) && b.present) {
        snprintf(bt, sizeof bt, "%d%%%s", b.percent, b.charging ? "  Charging" : "");
        draw_battery_glyph(x + 30, fy0 + 24, ink);
    } else {
        snprintf(bt, sizeof bt, "%s", power_summary());
        glyph_draw_c(GLYPH_POWER, x + 30, fy0 + 24, 18, ink_dim, 255);
    }
    text_fit_l(x + 48, fy0 + 24, bt, 150, FONT_REGULAR, 12, ink_dim);
    int hm = hov == (H_QS | 0x20), hs = hov == (H_QS | 0x21);
    if (hm) gfx_rrect(x + QS_W - 132, fy0 + 9, 72, 30, 10, fill_hot, HOT_A);
    text_c(x + QS_W - 96, fy0 + 24, "Mixer", FONT_MEDIUM, 12, ink);
    if (hs) gfx_rrect(x + QS_W - 54, fy0 + 9, 38, 30, 10, fill_hot, HOT_A);
    glyph_draw_c(GLYPH_SETTINGS, x + QS_W - 35, fy0 + 24, 18, ink, 255);
}
static int qs_hit(int mx, int my) {
    panel_t *p = &P[P_QS];
    if (!qs_open || !p->vis || !in_rect(mx, my, p->x, p->y, p->w, p->h)) return 0;
    int x = p->x, y = p->y;
    for (int k = 0; k < QT_COUNT; k++) {
        int tx, ty, tw, th; qs_tile_rect(k, x, y, &tx, &ty, &tw, &th);
        if (in_rect(mx, my, tx, ty, tw, th)) return H_QS | k;
    }
    for (int k = 0; k < 2; k++) {
        int sx, sy, sw; qs_slider_rect(k, x, y, &sx, &sy, &sw);
        if (my >= sy - 16 && my < sy + 16) return H_QS | 0x10 | k;
    }
    int fy0 = y + p->h - 48;
    if (in_rect(mx, my, x + QS_W - 132, fy0 + 9, 72, 30)) return H_QS | 0x20;
    if (in_rect(mx, my, x + QS_W - 54, fy0 + 9, 38, 30)) return H_QS | 0x21;
    return H_QS | 0xFF;
}
static void qs_slider_set(int k, int mx) {
    panel_t *p = &P[P_QS];
    int sx, sy, sw; qs_slider_rect(k, p->x, p->y, &sx, &sy, &sw);
    int v = (mx - sx) * 100 / (sw > 0 ? sw : 1);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (k == 0) backlight_set(v < 10 ? 10 : v);
    else        audio_set_volume(v);
    dmg_panel(P_QS); dmg_panel(P_TRAY);
}

/* ===========================================================================
 *  Notification centre + calendar
 * ===========================================================================*/
#define NC_W 368
static int nc_scroll;
static int cal_y, cal_m;            /* the month on show; 0 = this one */

static void nc_rect(int *x, int *y, int *w, int *h) {
    *w = NC_W; *x = SW - NC_W - 12; *y = 12; *h = SH - SHELF_H - 18;
}
#define CAL_H 318
static int note_icon(const char *src) {
    if (!src || !src[0]) return ICON_SETTINGS;
    if (!strcmp(src, "Screenshot")) return ICON_SNIP;
    if (!strcmp(src, "Screen recording")) return ICON_MEDIA;
    if (!strcmp(src, "System")) return ICON_SETTINGS;
    for (int i = 0; i < wm_nwin; i++) if (!strcasecmp(wm_wins[i].title, src)) return wm_wins[i].icon;
    if (ci_contains(src, "net") || ci_contains(src, "wifi") || ci_contains(src, "dhcp")) return ICON_NETWORK;
    if (ci_contains(src, "usb") || ci_contains(src, "disk") || ci_contains(src, "storage")) return ICON_DRIVE;
    if (ci_contains(src, "secur") || ci_contains(src, "vault") || ci_contains(src, "fire")) return ICON_SECURITY;
    if (ci_contains(src, "pkg") || ci_contains(src, "software") || ci_contains(src, "package")) return ICON_SOFTWARE;
    return ICON_SYSINFO;
}
static int note_card_h(void) { return 70; }
static int nc_cards_visible(int h) {
    int avail = h - CAL_H - 12 - 52;
    return imax(1, avail / (note_card_h() + 8));
}
static void draw_nc(void) {
    panel_t *p = &P[P_NC];
    if (!p->vis) return;
    panel_glass(p);
    int x = p->x, y = p->y, w = p->w, h = p->h;
    text_l(x + 20, y + 26, "Notifications", FONT_SEMIBOLD, 15, ink);
    if (notify_count()) {
        int cw = font_width(FONT_MEDIUM, 12, "Clear all", -1) + 22;
        int hot = hov == (H_NC | 1);
        gfx_rrect(x + w - 20 - cw, y + 13, cw, 26, 13, fill_hot, hot ? HOT_A + 14 : HOT_A - 8);
        text_c(x + w - 20 - cw / 2, y + 26, "Clear all", FONT_MEDIUM, 12, ink);
    }
    int ly = y + 52;
    int vis = nc_cards_visible(h);
    if (!notify_count()) {
        int cy = y + 52 + (h - CAL_H - 52) / 2 - 20;
        glyph_draw_c(GLYPH_BELL, x + w / 2, cy - 8, 24, ink_mute, 255);
        text_c(x + w / 2, cy + 22, "You're all caught up", FONT_MEDIUM, 13, ink_dim);
    }
    for (int r = 0; r < vis; r++) {
        const notification_t *e = notify_at(nc_scroll + r);
        if (!e) break;
        int cy = ly + r * (note_card_h() + 8);
        int hot = hov == (H_NC | 0x10 | r);
        gfx_rrect(x + 12, cy, w - 24, note_card_h(), 14, g_theme.dark ? 0xFFFFFF : 0xFFFFFF,
                  g_theme.dark ? (hot ? 26 : 16) : (hot ? 200 : 150));
        icon_draw(note_icon(e->source), x + 24, cy + 12, 20, 255);
        text_fit_l(x + 52, cy + 22, e->source, w - 160, FONT_SEMIBOLD, 12, ink);
        char hm[8]; snprintf(hm, sizeof hm, "%02d:%02d", e->hour % 100, e->min % 100);
        if (hot) {
            gfx_circle((float)(x + w - 36), (float)(cy + 22), 10.0f, fill_hot, 40);
            glyph_draw_c(GLYPH_CLOSE, x + w - 36, cy + 22, 14, ink, 255);
        } else {
            text_r(x + w - 26, cy + 22, hm, FONT_REGULAR, 11, ink_mute);
        }
        if (!e->read) gfx_circle((float)(x + 18), (float)(cy + 22), 3.0f, COL_ACCENT, 255);
        text_wrap(x + 24, cy + 33, w - 60, e->text, FONT_REGULAR, 12, 16, 2, ink_dim);
    }
    if (notify_count() > vis) {
        char sc[24]; snprintf(sc, sizeof sc, "%d more", notify_count() - vis - nc_scroll > 0 ? notify_count() - vis - nc_scroll : 0);
        if (notify_count() - vis - nc_scroll > 0)
            text_c(x + w / 2, ly + vis * (note_card_h() + 8) + 2, sc, FONT_REGULAR, 11, ink_mute);
    }
    /* the calendar */
    int cy0 = y + h - CAL_H;
    glass_rule(x + 16, cy0, w - 32);
    static const char *MON[13] = { "", "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December" };
    struct rtc_time t; rtc_now(&t);
    int ty = t.year ? t.year : 2026, tm = (t.mon >= 1 && t.mon <= 12) ? t.mon : 1, td = t.day;
    int yr = cal_y ? cal_y : ty, mo = cal_m ? cal_m : tm;
    char hdr[32]; snprintf(hdr, sizeof hdr, "%s %d", MON[mo], yr);
    text_l(x + 22, cy0 + 28, hdr, FONT_SEMIBOLD, 15, ink);
    for (int k = 0; k < 2; k++) {
        int bx = x + w - 76 + k * 36, hot = hov == (H_NC | 2 | k);
        if (hot) gfx_rrect(bx, cy0 + 12, 30, 30, 9, fill_hot, HOT_A);
        glyph_draw_c(k ? GLYPH_CHEV_D : GLYPH_CHEV_U, bx + 15, cy0 + 27, 18, ink, 255);
    }
    static const char *WD[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    int cw = (w - 40) / 7, gx = x + 20, gy = cy0 + 62;
    for (int i = 0; i < 7; i++) text_c(gx + i * cw + cw / 2, gy, WD[i], FONT_MEDIUM, 11, ink_mute);
    int mm = mo, yy = yr;
    if (mm < 3) { mm += 12; yy--; }
    int K = yy % 100, J = yy / 100;
    int first = ((1 + (13 * (mm + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7 + 6) % 7;
    static const int DIM[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int dim = DIM[mo];
    if (mo == 2 && ((yr % 4 == 0 && yr % 100 != 0) || yr % 400 == 0)) dim = 29;
    for (int d = 1; d <= dim; d++) {
        int slot = first + d - 1, row = slot / 7, col = slot % 7;
        int ccx = gx + col * cw + cw / 2, ccy = gy + 30 + row * 38;
        int today = (d == td && mo == tm && yr == ty);
        char db[4]; snprintf(db, sizeof db, "%d", d);
        if (today) gfx_circle((float)ccx, (float)ccy, 15.0f, COL_ACCENT, 255);
        text_c(ccx, ccy, db, today ? FONT_SEMIBOLD : FONT_REGULAR, 13, today ? 0xFFFFFF : (col == 0 || col == 6 ? ink_dim : ink));
    }
}
static int nc_hit(int mx, int my) {
    panel_t *p = &P[P_NC];
    if (!nc_open || !p->vis || !in_rect(mx, my, p->x, p->y, p->w, p->h)) return 0;
    int x = p->x, y = p->y, w = p->w, h = p->h;
    if (notify_count()) {
        int cw = font_width(FONT_MEDIUM, 12, "Clear all", -1) + 22;
        if (in_rect(mx, my, x + w - 20 - cw, y + 13, cw, 26)) return H_NC | 1;
    }
    int cy0 = y + h - CAL_H;
    for (int k = 0; k < 2; k++) if (in_rect(mx, my, x + w - 76 + k * 36, cy0 + 12, 30, 30)) return H_NC | 2 | k;
    int ly = y + 52, vis = nc_cards_visible(h);
    for (int r = 0; r < vis; r++) {
        if (!notify_at(nc_scroll + r)) break;
        int cy = ly + r * (note_card_h() + 8);
        if (in_rect(mx, my, x + 12, cy, w - 24, note_card_h())) return H_NC | 0x10 | r;
    }
    return H_NC;
}

/* ===========================================================================
 *  Toasts
 * ===========================================================================*/
#define TOAST_MS    4200
#define TOAST_STACK 3
#define TOAST_W     344
#define TOAST_H     74
static uint32_t toast_from = 1;

static int toast_live(int *out) {
    uint64_t now = gui_now();
    int n = 0;
    if (dnd) return 0;
    for (int i = 0; i < notify_count() && n < TOAST_STACK; i++) {
        const notification_t *e = notify_at(i);
        if (!e || e->id < toast_from) break;
        if (now - e->posted >= TOAST_MS) break;
        out[n++] = i;
    }
    return n;
}
static void draw_toast(int k) {
    panel_t *p = &P[P_TOAST0 + k];
    if (!p->vis) return;
    int idx[TOAST_STACK]; int n = toast_live(idx);
    if (k >= n) return;
    const notification_t *e = notify_at(idx[k]);
    if (!e) return;
    panel_glass(p);
    int x = p->x, y = p->y, a = p->alpha;
    icon_draw(note_icon(e->source), x + 14, y + 14, 32, a);
    font_draw_fit(FONT_SEMIBOLD, 13, x + 58, y + 26, e->source, TOAST_W - 120, ink, a);
    char hm[8]; snprintf(hm, sizeof hm, "now");
    int hw = font_width(FONT_REGULAR, 11, hm, -1);
    font_draw(FONT_REGULAR, 11, x + TOAST_W - 16 - hw, y + 25, hm, -1, ink_mute, a);
    text_wrap(x + 58, y + 32, TOAST_W - 74, e->text, FONT_REGULAR, 12, 16, 2, ink_dim);
}

/* ===========================================================================
 *  Alt+Tab
 * ===========================================================================*/
static int sw_open, sw_sel, sw_n;
static int sw_list[MAX_WIN];
static uint64_t sw_t0;
#define SW_TW 184
#define SW_TH 116
typedef struct { uint32_t *px; int w, h; } thumb_t;
static thumb_t thumbs[MAX_WIN];

static void make_thumb(int i) {
    const surface_t *s = wm_surface(i);
    thumb_t *t = &thumbs[i];
    if (!s || s->w <= 0 || s->h <= 0) { t->w = t->h = 0; return; }
    int tw = SW_TW, th = SW_TW * s->h / s->w;
    if (th > SW_TH) { th = SW_TH; tw = SW_TH * s->w / s->h; }
    if (tw < 8 || th < 8) { t->w = t->h = 0; return; }
    if (!t->px) t->px = (uint32_t *)kmalloc((uint64_t)SW_TW * SW_TH * 4);
    if (!t->px) return;
    t->w = tw; t->h = th;
    /* a box filter: every source pixel counts, so text does not shimmer */
    for (int j = 0; j < th; j++) {
        int y0 = j * s->h / th, y1 = (j + 1) * s->h / th; if (y1 <= y0) y1 = y0 + 1;
        for (int i2 = 0; i2 < tw; i2++) {
            int x0 = i2 * s->w / tw, x1 = (i2 + 1) * s->w / tw; if (x1 <= x0) x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy += (y1 - y0 > 4 ? 2 : 1))
                for (int xx = x0; xx < x1; xx += (x1 - x0 > 4 ? 2 : 1)) {
                    uint32_t c = s->px[yy * s->stride + xx];
                    r += c >> 16 & 0xFF; g += c >> 8 & 0xFF; b += c & 0xFF; n++;
                }
            t->px[j * tw + i2] = 0xFF000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
        }
    }
}

static void switcher_rect(int *x, int *y, int *w, int *h) {
    int n = sw_n < 6 ? sw_n : 6;
    *w = n * (SW_TW + 16) + 24; *h = SW_TH + 72;
    *x = (SW - *w) / 2; *y = (SH - SHELF_H - *h) / 2;
}
void shell_switcher(int dir) {
    if (!sw_open) {
        sw_n = 0;
        for (int z = wm_ztop; z >= 0 && sw_n < MAX_WIN; z--)
            for (int i = 0; i < wm_nwin; i++)
                if (wm_wins[i].z == z && wm_wins[i].open && !wm_wins[i].minimized && wm_on_desk(&wm_wins[i]))
                    sw_list[sw_n++] = i;
        if (sw_n < 2) {
            /* nothing to switch to: just focus what there is */
            if (sw_n == 1) gui_focus(&wm_wins[sw_list[0]]);
            return;
        }
        for (int k = 0; k < sw_n && k < 6; k++) make_thumb(sw_list[k]);
        sw_open = 1; sw_sel = 0; sw_t0 = gui_now();
        P[P_SWITCH].dirty = 1; P[P_SWITCH].env.w = 0;
        start_close();
    }
    int lim = sw_n < 6 ? sw_n : 6;
    sw_sel = (sw_sel + (dir > 0 ? 1 : lim - 1)) % lim;
    dmg_panel(P_SWITCH);
    gui_animating();
}
void shell_switcher_poll(void) {
    if (!sw_open) return;
    if (kbd_alt_down()) return;
    sw_open = 0;
    dmg_panel(P_SWITCH);
    if (sw_sel >= 0 && sw_sel < sw_n) gui_focus(&wm_wins[sw_list[sw_sel]]);
}
static void draw_switcher(void) {
    panel_t *p = &P[P_SWITCH];
    if (!p->vis) return;
    panel_glass(p);
    int n = sw_n < 6 ? sw_n : 6;
    for (int k = 0; k < n; k++) {
        int i = sw_list[k];
        int cx = p->x + 12 + k * (SW_TW + 16) + 8;
        int cy = p->y + 14;
        if (k == sw_sel) {
            gfx_rrect(cx - 8, cy - 6, SW_TW + 16, SW_TH + 54, 14, fill_hot, g_theme.dark ? 36 : 26);
            gfx_rrect_stroke(cx - 8, cy - 6, SW_TW + 16, SW_TH + 54, 14, 2, COL_ACCENT, 255);
        }
        thumb_t *t = &thumbs[i];
        if (t->px && t->w) {
            int tx = cx + (SW_TW - t->w) / 2, ty = cy + (SW_TH - t->h) / 2;
            gfx_shadow(tx, ty, t->w, t->h, 6, 12, 4, 0, 90);
            gfx_image_rr(tx, ty, t->w, t->h, t->px, t->w, t->h, 6, 255);
        } else {
            icon_draw(wm_wins[i].icon, cx + SW_TW / 2 - 32, cy + SW_TH / 2 - 32, 64, 255);
        }
        icon_draw(wm_wins[i].icon, cx, cy + SW_TH + 10, 20, 255);
        text_fit_l(cx + 26, cy + SW_TH + 20, wm_wins[i].title, SW_TW - 28, FONT_MEDIUM, 12, ink);
    }
}

/* ===========================================================================
 *  The modal prompt
 * ===========================================================================*/
static int        prompt_active;
static char       prompt_title[40];
static char       prompt_buf[FS_PATH_MAX];
static int        prompt_len;
static void     (*prompt_cb)(const char *);
static uint64_t   prompt_t0;

void gui_prompt(const char *title, const char *initial, void (*cb)(const char *)) {
    prompt_active = 1;
    prompt_cb = cb;
    snprintf(prompt_title, sizeof prompt_title, "%s", title ? title : "");
    prompt_len = 0;
    if (initial) for (; initial[prompt_len] && prompt_len < (int)sizeof(prompt_buf) - 1; prompt_len++)
        prompt_buf[prompt_len] = initial[prompt_len];
    prompt_buf[prompt_len] = 0;
    prompt_t0 = gui_now();
    P[P_PROMPT].dirty = 1; P[P_PROMPT].env.w = 0;
    gui_damage_all();
    gui_animating();
}
static void prompt_close(void) { prompt_active = 0; prompt_cb = 0; gui_damage_all(); }
static void prompt_confirm(void) {
    void (*cb)(const char *) = prompt_cb;
    prompt_close();
    if (cb && prompt_len > 0) cb(prompt_buf);
}
#define PR_W 420
#define PR_H 176
static void prompt_btn(int k, int x, int y, int *bx, int *by, int *bw, int *bh) {
    *bw = 96; *bh = 32; *by = y + PR_H - 20 - 32;
    *bx = k ? x + PR_W - 20 - 96 : x + PR_W - 20 - 96 - 10 - 96;
}
static void draw_prompt(void) {
    panel_t *p = &P[P_PROMPT];
    if (!prompt_active || !p->vis) return;
    gfx_fill_a(0, 0, SW, SH, 0x000000, 70 * p->alpha / 255);
    panel_glass(p);
    int x = p->x, y = p->y;
    text_l(x + 24, y + 34, prompt_title, FONT_SEMIBOLD, 16, ink);
    int fx = x + 24, fy = y + 62, fw = PR_W - 48, fh = 38;
    gfx_rrect(fx, fy, fw, fh, 10, g_theme.dark ? 0x000000 : 0xFFFFFF, g_theme.dark ? 90 : 200);
    gfx_rrect_stroke(fx, fy, fw, fh, 10, 2, COL_ACCENT, 255);
    gfx_clip_push(fx + 4, fy, fw - 8, fh);
    int tw = font_width(FONT_REGULAR, 14, prompt_buf, -1);
    int sx = fx + 12;
    if (tw > fw - 30) sx = fx + fw - 18 - tw;
    font_draw(FONT_REGULAR, 14, sx, fy + 24, prompt_buf, -1, ink, 255);
    if ((gui_now() / 530) % 2 == 0) gfx_fill(sx + tw + 1, fy + 10, 2, fh - 20, COL_ACCENT);
    gfx_clip_pop();
    for (int k = 0; k < 2; k++) {
        int bx, by, bw, bh; prompt_btn(k, x, y, &bx, &by, &bw, &bh);
        int hot = hov == (H_PROMPT | k);
        if (k) gfx_rrect(bx, by, bw, bh, 10, COL_ACCENT, 255);
        else   gfx_rrect(bx, by, bw, bh, 10, fill_hot, hot ? HOT_A + 20 : HOT_A + 6);
        if (k && hot) gfx_rrect(bx, by, bw, bh, 10, 0xFFFFFF, 30);
        text_c(bx + bw / 2, by + bh / 2, k ? "OK" : "Cancel", FONT_SEMIBOLD, 13, k ? 0xFFFFFF : ink);
    }
}
int shell_modal(void) { return prompt_active; }

/* ===========================================================================
 *  Snap preview
 * ===========================================================================*/
static int      snap_shown = SNAP_NONE;
static uint64_t snap_t0;
void shell_draw_snap(void) {
    int z = gui_snap_hint();
    if (z == SNAP_NONE) return;
    rect_t r; gui_snap_rect(z, &r);
    float t = ease_out(anim_t(gui_now(), snap_t0, 180));
    int inset = 8 + (int)((1.0f - t) * 24.0f);
    int x = r.x + inset, y = r.y + inset, w = r.w - 2 * inset, h = r.h - 2 * inset;
    if (w <= 0 || h <= 0) return;
    gfx_rrect(x, y, w, h, 14, COL_ACCENT, (int)(60 * t));
    gfx_rrect(x, y, w, h, 14, 0xFFFFFF, (int)(26 * t));
    gfx_rrect_stroke(x, y, w, h, 14, 2, COL_ACCENT, (int)(200 * t));
    gfx_rrect_stroke2(x + 2, y + 2, w - 4, h - 4, 12, 0xFFFFFF, (int)(90 * t), (int)(20 * t));
}

/* ===========================================================================
 *  Frame bookkeeping: where everything is this frame, what needs repainting
 * ===========================================================================*/
static int panel_alpha_anim(int open, int closing, uint64_t t0, int din, int dout, float *tt) {
    uint64_t now = gui_now();
    if (open) { float t = ease_out(anim_t(now, t0, din)); *tt = t; return (int)(255.0f * t); }
    if (closing) { float t = anim_t(now, t0, dout); *tt = 1.0f - ease_out(t); return (int)(255.0f * (1.0f - t)); }
    *tt = 0; return 0;
}

static void layout_panels(void) {
    uint64_t now = gui_now();
    for (int k = 0; k < P_COUNT; k++) {
        P[k].pvis = P[k].vis; P[k].px = P[k].x; P[k].py = P[k].y; P[k].pw = P[k].w; P[k].ph = P[k].h;
        P[k].palpha = P[k].alpha;
        P[k].vis = 0;
    }
    int x, y, w, h;
    dock_build();
    pill_layout(&x, &y, &w, &h);  panel_place(&P[P_PILL], x, y, w, h, 20, 255);
    dock_layout(&x, &y, &w, &h);  panel_place(&P[P_DOCK], x, y, w, h, 20, 255);
    tray_layout(&x, &y, &w, &h);  panel_place(&P[P_TRAY], x, y, w, h, 20, 255);
    P[P_PILL].env = (rect_t){ P[P_PILL].x, P[P_PILL].y, P[P_PILL].w, P[P_PILL].h };
    P[P_DOCK].env = (rect_t){ P[P_DOCK].x, P[P_DOCK].y, P[P_DOCK].w, P[P_DOCK].h };
    P[P_TRAY].env = (rect_t){ P[P_TRAY].x, P[P_TRAY].y, P[P_TRAY].w, P[P_TRAY].h };

    /* tooltip over the hovered dock icon or desktop dot */
    tip_text[0] = 0;
    if (HKIND(hov) == H_DOCK && !start_open && !menu_open) {
        int k = hov & 0xFF;
        if (k < ndock) {
            const char *t = ditems[k].win == DI_START ? "Start" : ditems[k].win == DI_SEARCH ? "Search" :
                            wm_wins[ditems[k].win].title;
            snprintf(tip_text, sizeof tip_text, "%s", t);
            int sx, sy; dock_slot_rect(k, &sx, &sy);
            tip_cx = sx + dock_slot / 2; tip_cy = sy - 26;
        }
    } else if (HKIND(hov) == H_PILL) {
        int d = hov & 0xFF;
        char names[44];
        int n = gui_desk_windows(d, names, sizeof names);
        if (n) snprintf(tip_text, sizeof tip_text, "Desktop %d " TXT_BULLET " %s", d + 1, names);
        else   snprintf(tip_text, sizeof tip_text, "Desktop %d", d + 1);
        int px, py, pw, ph; pill_layout(&px, &py, &pw, &ph);
        tip_cx = px + 10 + d * 22 + 11; tip_cy = py - 26;
    }
    if (tip_text[0]) {
        int tw = font_width(FONT_MEDIUM, 12, tip_text, -1) + 24;
        int tx = tip_cx - tw / 2;
        if (tx < 8) tx = 8;
        if (tx + tw > SW - 8) tx = SW - 8 - tw;
        panel_place(&P[P_TIP], tx, tip_cy - 15, tw, 30, 15, 255);
        P[P_TIP].env = (rect_t){ P[P_TIP].x, P[P_TIP].y, tw, 30 };
    }

    float t;
    int a = panel_alpha_anim(start_open, start_closing, start_t0, 240, 160, &t);
    if (start_closing && now >= start_t0 + 160) start_closing = 0;
    if (a > 0) {
        start_rect(&x, &y, &w, &h);
        int off = (int)((1.0f - t) * 36.0f);
        panel_place(&P[P_START], x, y + off, w, h, 18, a);
        P[P_START].env = (rect_t){ x, y, w, off ? h + 36 : h };
    }
    a = panel_alpha_anim(qs_open, qs_closing, qs_t0, 220, 150, &t);
    if (qs_closing && now >= qs_t0 + 150) qs_closing = 0;
    if (a > 0) {
        qs_rect(&x, &y, &w, &h);
        int off = (int)((1.0f - t) * 20.0f);
        panel_place(&P[P_QS], x, y + off, w, h, 18, a);
        P[P_QS].env = (rect_t){ x, y, w, off ? h + 20 : h };
    }
    a = panel_alpha_anim(nc_open, nc_closing, nc_t0, 240, 160, &t);
    if (nc_closing && now >= nc_t0 + 160) nc_closing = 0;
    if (a > 0) {
        nc_rect(&x, &y, &w, &h);
        int off = (int)((1.0f - t) * 40.0f);
        panel_place(&P[P_NC], x + off, y, w, h, 18, a);
        P[P_NC].env = (rect_t){ x, y, off ? w + 40 : w, h };
    }
    if (menu_open) {
        menu_rect(&x, &y, &w, &h);
        float mt = ease_out(anim_t(now, menu_t0, 140));
        int off = (int)((1.0f - mt) * (menu_up ? 8.0f : -6.0f));
        panel_place(&P[P_MENU], x, y + off, w, h, 12, (int)(255 * mt));
        P[P_MENU].env = mt < 1.0f ? (rect_t){ x, y - 8, w, h + 16 } : (rect_t){ x, y, w, h };
    }
    if (sw_open) {
        switcher_rect(&x, &y, &w, &h);
        float st = ease_out(anim_t(now, sw_t0, 160));
        panel_place(&P[P_SWITCH], x, y, w, h, 22, (int)(255 * st));
        P[P_SWITCH].env = (rect_t){ x, y, w, h };
    }
    if (prompt_active) {
        float pt = ease_out(anim_t(now, prompt_t0, 180));
        int px = (SW - PR_W) / 2, py = (SH - PR_H) / 2 - 40;
        panel_place(&P[P_PROMPT], px, py + (int)((1.0f - pt) * 16.0f), PR_W, PR_H, 18, (int)(255 * pt));
        P[P_PROMPT].env = pt < 1.0f ? (rect_t){ px, py, PR_W, PR_H + 16 } : (rect_t){ px, py, PR_W, PR_H };
    }
    int idx[TOAST_STACK]; int n = toast_live(idx);
    for (int k = 0; k < n; k++) {
        const notification_t *e = notify_at(idx[k]);
        if (!e) continue;
        float tin = ease_out(anim_t(now, e->posted, 360));
        float tout = anim_t(now, e->posted + TOAST_MS - 260, 260);
        int tx = SW - TOAST_W - 14, ty = 14 + k * (TOAST_H + 10);
        int off = (int)((1.0f - tin) * (TOAST_W + 30));
        int al = (int)(255.0f * (1.0f - tout));
        panel_place(&P[P_TOAST0 + k], tx + off, ty, TOAST_W, TOAST_H, 16, al);
        P[P_TOAST0 + k].env = off ? (rect_t){ tx, ty, SW - tx, TOAST_H } : (rect_t){ tx, ty, TOAST_W, TOAST_H };
        if (tin < 1.0f || tout > 0.0f) gui_animating();
    }
}

static const int layer_of[P_COUNT] = {
    L_SHELL, L_SHELL, L_SHELL, L_POPUP, L_POPUP, L_POPUP, L_POPUP, L_POPUP, L_POPUP,
    L_MODAL, L_TOAST, L_TOAST, L_TOAST,
};
static const glass_t *glass_of(int k) {
    switch (k) {
    case P_PILL: case P_DOCK: case P_TRAY: return &G_SHELL;
    case P_MENU: case P_TIP: return &G_MENU;
    default: return &G_POPUP;
    }
}

int shell_update(uint64_t now) {
    int anim = 0;
    /* dock magnification: ease every icon toward its target */
    for (int k = 0; k < ndock; k++) {
        dockitem_t *d = &ditems[k];
        float tg = 1.0f;
        if (HKIND(hov) == H_DOCK && !start_open) {
            int hk = hov & 0xFF;
            if (k == hk) tg = 1.22f;
            else if (k == hk - 1 || k == hk + 1) tg = 1.08f;
        }
        d->target = tg;
        float diff = d->target - d->scale;
        if (diff > 0.002f || diff < -0.002f) { d->scale += diff * 0.35f; anim = 1; }
        else d->scale = d->target;
        if (d->bounce0 && now < d->bounce0 + 900) anim = 1;
    }
    if (anim) {
        int x, y, w, h; dock_layout(&x, &y, &w, &h);
        gui_damage(x - 8, y - 34, w + 16, h + 34, L_SHELL);
    }
    layout_panels();
    if (start_open || start_closing) { if (now < start_t0 + 260) anim = 1; }
    if (qs_open || qs_closing) { if (now < qs_t0 + 240) anim = 1; }
    if (nc_open || nc_closing) { if (now < nc_t0 + 260) anim = 1; }
    if (menu_open && now < menu_t0 + 160) anim = 1;
    if (sw_open && now < sw_t0 + 180) anim = 1;
    if (prompt_active && now < prompt_t0 + 200) anim = 1;
    if (rec_active()) anim = 1;          /* the recording dot breathes */
    int sh = gui_snap_hint();
    if (sh != snap_shown) { snap_shown = sh; snap_t0 = now; }
    if (sh != SNAP_NONE && now < snap_t0 + 200) {
        rect_t r; gui_snap_rect(sh, &r);
        gui_damage(r.x, r.y, r.w, r.h, L_WIN);
        anim = 1;
    }
    for (int k = 0; k < P_COUNT; k++) {
        panel_t *p = &P[k];
        int changed = p->vis != p->pvis || (p->vis && (p->x != p->px || p->y != p->py || p->w != p->pw ||
                                                       p->h != p->ph || p->alpha != p->palpha));
        if (changed) dmg_panel(k);
    }
    if (rec_active()) dmg_panel(P_TRAY);
    return anim;
}

void shell_prepare(void) {
    for (int k = 0; k < P_COUNT; k++) {
        panel_t *p = &P[k];
        p->layer = layer_of[k];
        p->g = glass_of(k);
        if (!p->vis) continue;
        int m = p->g->blur + 8;
        rect_t e = p->env;
        int stale = p->dirty || !p->bd.valid;
        /* recapture when the envelope moved, or anything beneath it changed */
        if (!stale) {
            int bm = p->g->blur + (p->g->blur > 40 ? 8 : 4);
            if (p->bd.x != e.x - bm || p->bd.y != e.y - bm || p->bd.w != e.w + 2 * bm || p->bd.h != e.h + 2 * bm)
                stale = 1;
        }
        /* What is under a panel changing -- a caret blinking in the window
         * behind Start -- does not need a fresh blur every frame: that blur
         * is the most expensive thing on screen and nobody can read through
         * it anyway. At most four times a second, and always on the frame
         * after things go quiet so the last state is the one left showing. */
        if (!stale && gui_damage_under(e.x - m, e.y - m, e.w + 2 * m, e.h + 2 * m, p->layer)) p->pending = 1;
        if (!stale && p->pending && gui_now() >= p->captured + 250) stale = 1;
        if (p->pending) gui_animating();
        if (stale) {
            p->dirty = 1;
            gui_damage(e.x - m, e.y - m, e.w + 2 * m, e.h + 2 * m, p->layer);
            int r = shadow_reach(p);
            gui_damage(p->x - r, p->y - r, p->w + 2 * r, p->h + 2 * r, p->layer);
        }
    }
}

void shell_capture(int layer) {
    for (int k = 0; k < P_COUNT; k++) {
        panel_t *p = &P[k];
        if (!p->vis || p->layer != layer || !p->dirty) continue;
        gfx_backdrop_capture(&p->bd, p->env.x, p->env.y, p->env.w, p->env.h, p->g);
        p->dirty = 0;
        p->pending = 0;
        p->captured = gui_now();
    }
}

void shell_draw(int layer) {
    switch (layer) {
    case L_SHELL:
        if (P[P_PILL].vis && gfx_clip_hits(P[P_PILL].x - 60, P[P_PILL].y - 60, P[P_PILL].w + 120, P[P_PILL].h + 120)) draw_pill();
        if (P[P_DOCK].vis && gfx_clip_hits(P[P_DOCK].x - 60, P[P_DOCK].y - 70, P[P_DOCK].w + 120, P[P_DOCK].h + 130)) draw_dock();
        if (P[P_TRAY].vis && gfx_clip_hits(P[P_TRAY].x - 60, P[P_TRAY].y - 60, P[P_TRAY].w + 120, P[P_TRAY].h + 120)) draw_tray();
        break;
    case L_POPUP:
        if (P[P_START].vis) draw_start();
        if (P[P_QS].vis) draw_qs();
        if (P[P_NC].vis) draw_nc();
        if (P[P_SWITCH].vis) draw_switcher();
        if (P[P_TIP].vis) draw_tip();
        if (P[P_MENU].vis) draw_menu();
        break;
    case L_TOAST:
        for (int k = 0; k < TOAST_STACK; k++) if (P[P_TOAST0 + k].vis) draw_toast(k);
        break;
    case L_MODAL:
        draw_prompt();
        break;
    default: break;
    }
}

/* ===========================================================================
 *  Input
 * ===========================================================================*/
static int hit_any(int mx, int my) {
    int h;
    if (prompt_active) {
        panel_t *p = &P[P_PROMPT];
        for (int k = 0; k < 2; k++) {
            int bx, by, bw, bh; prompt_btn(k, p->x, p->y, &bx, &by, &bw, &bh);
            if (in_rect(mx, my, bx, by, bw, bh)) return H_PROMPT | k;
        }
        return H_PROMPT | 0xFF;
    }
    if (menu_open) { int it = menu_item_at(mx, my); if (it >= 0) return H_MENU | it; if (it == -1) return H_MENU | 0xFF; }
    { int idx[TOAST_STACK]; int n = toast_live(idx);
      for (int k = 0; k < n; k++) { panel_t *p = &P[P_TOAST0 + k]; if (p->vis && in_rect(mx, my, p->x, p->y, p->w, p->h)) return H_TOAST | k; } }
    if ((h = start_hit(mx, my))) return h;
    if ((h = qs_hit(mx, my))) return h;
    if ((h = nc_hit(mx, my))) return h;
    int k = dock_hit(mx, my);
    if (k >= 0) return H_DOCK | k;
    k = tray_hit(mx, my);
    if (k >= 0) return H_TRAY | k;
    k = pill_hit(mx, my);
    if (k >= 0) return H_PILL | k;
    int x, y, w, h2;
    dock_layout(&x, &y, &w, &h2);
    if (in_rect(mx, my, x, y, w, h2)) return H_DOCK | 0xFF;
    tray_layout(&x, &y, &w, &h2);
    if (in_rect(mx, my, x, y, w, h2)) return H_TRAY | 0xFF;
    pill_layout(&x, &y, &w, &h2);
    if (in_rect(mx, my, x, y, w, h2)) return H_PILL | 0xFF;
    return 0;
}

static void hov_damage(int h) {
    switch (HKIND(h)) {
    case H_DOCK: dmg_panel(P_DOCK); dmg_panel(P_TIP); break;
    case H_PILL: dmg_panel(P_PILL); dmg_panel(P_TIP); break;
    case H_TRAY: dmg_panel(P_TRAY); break;
    case H_START: dmg_panel(P_START); break;
    case H_QS: dmg_panel(P_QS); break;
    case H_NC: dmg_panel(P_NC); break;
    case H_MENU: dmg_panel(P_MENU); break;
    case H_TOAST: dmg_panel(P_TOAST0 + (h & 3)); break;
    case H_PROMPT: dmg_panel(P_PROMPT); break;
    case H_ICON: dmg_icon(h & 0xFF); break;
    default: break;
    }
}

void shell_close_popups(void) {
    start_close();
    if (qs_open) { qs_open = 0; qs_closing = 1; qs_t0 = gui_now(); }
    if (nc_open) { nc_open = 0; nc_closing = 1; nc_t0 = gui_now(); }
    menu_close();
    gui_animating();
}

static void qs_toggle(void) {
    if (qs_open) { qs_open = 0; qs_closing = 1; qs_t0 = gui_now(); }
    else { start_close(); nc_open = 0; menu_close(); qs_open = 1; qs_closing = 0; qs_t0 = gui_now(); P[P_QS].dirty = 1; }
    gui_animating();
}
static void nc_toggle(void) {
    if (nc_open) { nc_open = 0; nc_closing = 1; nc_t0 = gui_now(); }
    else {
        start_close(); qs_open = 0; menu_close();
        nc_open = 1; nc_closing = 0; nc_t0 = gui_now(); nc_scroll = 0; cal_y = cal_m = 0;
        notify_mark_all_read();
        P[P_NC].dirty = 1;
        dmg_panel(P_TRAY);
    }
    gui_animating();
}

static int dock_menu_win = -1;
static int icon_menu_idx = -1;

static void do_action(int act) {
    switch (act) {
    case A_WALLPAPER: case A_DISPLAY: case A_SETTINGS:
        for (int i = 0; i < wm_nwin; i++) if (!strcmp(wm_wins[i].title, "Settings")) gui_open(&wm_wins[i]);
        break;
    case A_TERMINAL:
        for (int i = 0; i < wm_nwin; i++) if (!strcmp(wm_wins[i].title, "Terminal")) gui_open(&wm_wins[i]);
        break;
    case A_FILES:
        for (int i = 0; i < wm_nwin; i++) if (!strcmp(wm_wins[i].title, "File Explorer")) gui_open(&wm_wins[i]);
        break;
    case A_ARRANGE: arrange_icons(); break;
    case A_DOCK_CLOSE: if (dock_menu_win >= 0) wm_close(dock_menu_win); break;
    case A_DOCK_PIN:   if (dock_menu_win >= 0) wm_wins[dock_menu_win].pinned = !wm_wins[dock_menu_win].pinned; break;
    case A_DOCK_MIN:   if (dock_menu_win >= 0) wm_minimize(dock_menu_win); break;
    case A_DOCK_OPEN:  if (dock_menu_win >= 0) wm_toggle_from_dock(dock_menu_win); break;
    case A_WIN_MIN:    if (dock_menu_win >= 0) wm_minimize(dock_menu_win); break;
    case A_WIN_MAX:    if (dock_menu_win >= 0) wm_maximize_toggle(&wm_wins[dock_menu_win]); break;
    case A_WIN_LEFT:   if (dock_menu_win >= 0) wm_snap(&wm_wins[dock_menu_win], SNAP_L); break;
    case A_WIN_RIGHT:  if (dock_menu_win >= 0) wm_snap(&wm_wins[dock_menu_win], SNAP_R); break;
    case A_WIN_NEXTDESK:
        if (dock_menu_win >= 0) { gui_focus(&wm_wins[dock_menu_win]); gui_desk_send((wm_desk + 1) % NDESK); }
        break;
    case A_WIN_CLOSE:  if (dock_menu_win >= 0) wm_close(dock_menu_win); break;
    case A_PWR_LOCK:   gui_lock(); break;
    case A_PWR_SIGNOUT: gui_logout(); break;
    case A_PWR_RESTART: acpi_reboot(); break;
    case A_PWR_OFF:    acpi_poweroff(); break;
    case A_ICON_OPEN:  deskicon_open(icon_menu_idx); break;
    case A_ICON_REMOVE:
        if (icon_menu_idx >= 0 && icon_menu_idx < MAX_DESKICON && dicons[icon_menu_idx].used) {
            dmg_icon(icon_menu_idx);
            dicons[icon_menu_idx].used = 0;
            if (desk_sel == icon_menu_idx) desk_sel = -1;
        }
        break;
    default: break;
    }
    dmg_panel(P_DOCK);
}

void shell_window_menu(int win, int x, int y) {
    window_t *w = &wm_wins[win];
    dock_menu_win = win;
    nmi = 0;
    menu_add("Minimize", GLYPH_MINUS, A_WIN_MIN, 0);
    menu_add(w->maximized || w->snapped ? "Restore" : "Maximize", w->maximized ? GLYPH_RESTORE : GLYPH_MAXIMIZE, A_WIN_MAX, 0);
    menu_add("Snap left", GLYPH_SIDEBAR, A_WIN_LEFT, 0);
    menu_add("Snap right", GLYPH_SIDEBAR, A_WIN_RIGHT, 0);
    menu_add("", -1, 0, MF_SEP);
    menu_add("Move to next desktop", GLYPH_ARROW_R, A_WIN_NEXTDESK, 0);
    menu_add("", -1, 0, MF_SEP);
    menu_add("Close", GLYPH_CLOSE, A_WIN_CLOSE, MF_DANGER);
    menu_show(x, y, 0, 2);
}

static void dock_menu(int k) {
    int win = ditems[k].win;
    if (win < 0) return;
    window_t *w = &wm_wins[win];
    dock_menu_win = win;
    nmi = 0;
    menu_add(w->title, -1, A_NONE, MF_OFF);
    menu_add("", -1, 0, MF_SEP);
    if (w->open) {
        if (w->minimized) menu_add("Show", GLYPH_WINDOW, A_DOCK_OPEN, 0);
        else              menu_add("Minimize", GLYPH_MINUS, A_DOCK_MIN, 0);
    } else menu_add("Open", GLYPH_WINDOW, A_DOCK_OPEN, 0);
    menu_add(w->pinned ? "Unpin from dock" : "Pin to dock", w->pinned ? GLYPH_UNPIN : GLYPH_PIN, A_DOCK_PIN, 0);
    if (w->open) {
        menu_add("", -1, 0, MF_SEP);
        menu_add("Close window", GLYPH_CLOSE, A_DOCK_CLOSE, MF_DANGER);
    }
    int sx, sy; dock_slot_rect(k, &sx, &sy);
    menu_show(sx, sy, 1, 1);
}

static void power_menu(int x, int y) {
    nmi = 0;
    menu_add("Lock", GLYPH_LOCK, A_PWR_LOCK, 0);
    menu_add("Sign out", GLYPH_LOGOUT, A_PWR_SIGNOUT, 0);
    menu_add("", -1, 0, MF_SEP);
    menu_add("Restart", GLYPH_REFRESH, A_PWR_RESTART, 0);
    menu_add("Shut down", GLYPH_POWER, A_PWR_OFF, MF_DANGER);
    menu_show(x, y, 1, 3);
}

int shell_mouse_down(int x, int y, int button) {
    int h = hit_any(x, y);
    if (button == MOUSE_RIGHT) {
        if (menu_open && HKIND(h) != H_MENU) menu_close();
        if (HKIND(h) == H_DOCK && (h & 0xFF) < ndock) { dock_menu(h & 0xFF); return 1; }
        if (HKIND(h) == H_DOCK || HKIND(h) == H_TRAY || HKIND(h) == H_PILL) return 1;
        if (start_open && HKIND(h) == H_START) return 1;
        return 0;
    }
    /* a modal prompt swallows all clicks; only its buttons act */
    if (prompt_active) {
        if (h == (H_PROMPT | 1)) prompt_confirm();
        else if (h == (H_PROMPT | 0)) prompt_close();
        return 1;
    }
    if (menu_open) {
        if (HKIND(h) == H_MENU) {
            int it = h & 0xFF;
            if (it < nmi) { int act = mi[it].act; menu_close(); do_action(act); }
            return 1;
        }
        menu_close();
        /* and the click lands where it was aimed */
    }
    if (HKIND(h) == H_TOAST) {
        int idx[TOAST_STACK]; int n = toast_live(idx);
        int k = h & 3;
        if (k < n) {
            const notification_t *e = notify_at(idx[k]);
            if (e) { toast_from = e->id + 1; notify_mark_read(e->id); }
        }
        for (int j = 0; j < TOAST_STACK; j++) dmg_panel(P_TOAST0 + j);
        if (!nc_open) nc_toggle();
        return 1;
    }
    press = h;
    if (HKIND(h) == H_START) {
        int e = h & ~0xF00;
        if (e == 1) { sm_mode = sm_mode == SM_ALL ? SM_PINNED : SM_ALL; sm_scroll = 0; dmg_panel(P_START); return 1; }
        if (e == 2) {
            panel_t *p = &P[P_START];
            int pbx, pby, pbw, pbh; sm_power_rect(p->x, p->y, p->w, p->h, &pbx, &pby, &pbw, &pbh);
            power_menu(pbx - 150, pby);
            return 1;
        }
        if (e & 0x100000) { int k = e & 0xFF; if (k < nall) sm_launch_app(all_idx[k]); return 1; }
        if ((e & 0xE0) == 0x40) { int k = e & 0x1F; if (k < npin) sm_launch_app(pin_idx[k]); return 1; }
        if ((e & 0xF0) == 0xA0) { int k = e & 0x0F; if (k < nrec) sm_launch_path(recent_path[k]); return 1; }
        if ((e & 0xE0) == 0x80) {
            int k = e & 0x1F;
            if (k < nsres) { if (sres[k].app >= 0) sm_launch_app(sres[k].app); else sm_launch_path(sres[k].path); }
            return 1;
        }
        return 1;
    }
    if (HKIND(h) == H_QS) {
        int e = h & 0xFF;
        if (e < QT_COUNT) {
            switch (e) {
            case QT_WIFI:  qs_toggle(); network_app_open(); break;
            case QT_FOCUS: dnd = !dnd; dmg_panel(P_TRAY); break;
            case QT_DARK:
                g_settings.theme = g_theme.dark ? 1 : 0;
                g_settings.accent = settings_theme_accent(g_settings.theme);
                settings_apply();
                break;
            case QT_MUTE:  mixer_toggle_mute(MIX_MASTER); audio_gain_changed(); dmg_panel(P_TRAY); break;
            case QT_REC:   gui_record_toggle_now(); break;
            case QT_SHOT:  qs_toggle(); gui_screenshot_now(); break;
            }
            dmg_panel(P_QS);
            return 1;
        }
        if (e == 0x10 || e == 0x11) { qs_slider_drag = e & 1; qs_slider_set(qs_slider_drag, x); return 1; }
        if (e == 0x20) { qs_toggle(); mixer_app_open(); return 1; }
        if (e == 0x21) { qs_toggle(); do_action(A_SETTINGS); return 1; }
        return 1;
    }
    if (HKIND(h) == H_NC) {
        int e = h & 0xFF;
        if (e == 1) { notify_clear(); nc_scroll = 0; dmg_panel(P_NC); dmg_panel(P_TRAY); return 1; }
        if (e == 2 || e == 3) {
            struct rtc_time t; rtc_now(&t);
            int yr = cal_y ? cal_y : (t.year ? t.year : 2026), mo = cal_m ? cal_m : (t.mon ? t.mon : 1);
            mo += (e == 3) ? 1 : -1;
            if (mo < 1) { mo = 12; yr--; }
            if (mo > 12) { mo = 1; yr++; }
            cal_y = yr; cal_m = mo;
            dmg_panel(P_NC);
            return 1;
        }
        if ((e & 0xF0) == 0x10) {
            const notification_t *n = notify_at(nc_scroll + (e & 0x0F));
            panel_t *p = &P[P_NC];
            if (n) {
                if (x >= p->x + p->w - 52) {
                    notify_dismiss(n->id);
                    int last = notify_count() - nc_cards_visible(p->h);
                    if (nc_scroll > last) nc_scroll = last > 0 ? last : 0;
                } else notify_mark_read(n->id);
            }
            dmg_panel(P_NC);
            return 1;
        }
        return 1;
    }
    /* outside every open popup: they close, and the click carries on */
    int was_popup = start_open || qs_open || nc_open;
    if (start_open && HKIND(h) != H_DOCK) start_close();
    if (qs_open && !(HKIND(h) == H_TRAY && (h & 0xFF) == 1)) { qs_open = 0; qs_closing = 1; qs_t0 = gui_now(); gui_animating(); }
    if (nc_open && !(HKIND(h) == H_TRAY && (h & 0xFF) == 2)) { nc_open = 0; nc_closing = 1; nc_t0 = gui_now(); gui_animating(); }
    if (HKIND(h) == H_DOCK) {
        int k = h & 0xFF;
        if (k < ndock) {
            if (ditems[k].win == DI_START) shell_toggle_start();
            else if (ditems[k].win == DI_SEARCH) { if (!start_open) start_open_now(); }
            else { if (start_open) start_close(); wm_toggle_from_dock(ditems[k].win); }
        }
        dmg_panel(P_DOCK);
        return 1;
    }
    if (HKIND(h) == H_TRAY) {
        int g = h & 0xFF;
        if (g == 0) gui_record_toggle_now();
        else if (g == 1) qs_toggle();
        else if (g == 2) nc_toggle();
        dmg_panel(P_TRAY);
        return 1;
    }
    if (HKIND(h) == H_PILL) {
        int d = h & 0xFF;
        if (d < NDESK) wm_desk_goto(d);
        return 1;
    }
    (void)was_popup;
    return 0;
}

int shell_mouse_desktop(int x, int y, int button) {
    if (button & MOUSE_RIGHT) {
        int di = deskicon_hit(x, y);
        nmi = 0;
        if (di >= 0) {
            int old = desk_sel;
            desk_sel = di; dmg_icon(old); dmg_icon(di);
            icon_menu_idx = di;
            menu_add("Open", GLYPH_FOLDER_OPEN, A_ICON_OPEN, 0);
            menu_add("", -1, 0, MF_SEP);
            menu_add("Remove from desktop", GLYPH_CLOSE, A_ICON_REMOVE, 0);
            menu_show(x, y, 0, 4);
            return 1;
        }
        menu_add("Change wallpaper", GLYPH_PICTURE, A_WALLPAPER, 0);
        menu_add("Display settings", GLYPH_MONITOR, A_DISPLAY, 0);
        menu_add("", -1, 0, MF_SEP);
        menu_add("Open Terminal", GLYPH_TERMINAL, A_TERMINAL, 0);
        menu_add("File Explorer", GLYPH_FOLDER, A_FILES, 0);
        menu_add("", -1, 0, MF_SEP);
        menu_add("Arrange icons", GLYPH_GRID, A_ARRANGE, 0);
        menu_show(x, y, 0, 0);
        return 1;
    }
    int di = deskicon_hit(x, y);
    if (di >= 0) {
        uint64_t now = gui_now();
        if (desk_last_idx == di && now - desk_last_tick < 400) {   /* double-click */
            desk_sel = di; desk_last_idx = -1; dmg_icon(di); deskicon_open(di); return 1;
        }
        int old = desk_sel;
        desk_sel = di; desk_last_idx = di; desk_last_tick = now;
        desk_drag = di; desk_dx = x - dicons[di].x; desk_dy = y - dicons[di].y; desk_moved = 0;
        dmg_icon(old); dmg_icon(di);
        return 1;
    }
    if (desk_sel >= 0) { dmg_icon(desk_sel); desk_sel = -1; }
    return 0;
}

int shell_dragging(void) { return desk_drag >= 0 || dnd_active || dnd_armed || qs_slider_drag >= 0; }

void shell_mouse_move(int x, int y, int buttons) {
    if (qs_slider_drag >= 0) {
        if (buttons & MOUSE_LEFT) qs_slider_set(qs_slider_drag, x);
        return;
    }
    if (desk_drag >= 0) {
        if (buttons & MOUSE_LEFT) {
            deskicon_t *d = &dicons[desk_drag];
            dmg_icon(desk_drag);
            d->x = x - desk_dx; d->y = y - desk_dy;
            deskicon_clamp(d);
            desk_moved = 1;
            dmg_icon(desk_drag);
        }
        return;
    }
    if (dnd_active) {
        gui_damage(dnd_px - 70, dnd_py - 40, 140, 100, L_DESK);
        dnd_px = x; dnd_py = y;
        gui_damage(dnd_px - 70, dnd_py - 40, 140, 100, L_DESK);
        return;
    }
    if (dnd_armed && (buttons & MOUSE_LEFT)) {
        int dx = x - press_x, dy = y - press_y;
        if (dx * dx + dy * dy > 25) { dnd_active = 1; dnd_px = x; dnd_py = y; }
        return;
    }
    int h = hit_any(x, y);
    if (!h) {
        int di = deskicon_hit(x, y);
        /* only when nothing is on top of the desktop there */
        if (di >= 0) {
            int covered = 0;
            for (int i = 0; i < wm_nwin && !covered; i++) {
                window_t *w = &wm_wins[i];
                if (w->open && !w->minimized && wm_on_desk(w) && in_rect(x, y, w->x, w->y, w->w, w->h)) covered = 1;
            }
            if (!covered) h = H_ICON | di;
        }
    }
    if (h != hov) {
        int old = hov;
        hov = h;
        hov_damage(old);
        hov_damage(h);
    }
}

void shell_mouse_up(int x, int y, int button) {
    (void)button;
    if (qs_slider_drag >= 0) { qs_slider_drag = -1; dmg_panel(P_QS); }
    if (desk_drag >= 0) { desk_drag = -1; }
    if (dnd_active) {
        gui_damage(dnd_px - 70, dnd_py - 40, 140, 100, L_DESK);
        /* dropped on the wallpaper: make a shortcut */
        int over = y >= SH - SHELF_H;
        for (int i = 0; i < wm_nwin && !over; i++) {
            window_t *w = &wm_wins[i];
            if (w->open && !w->minimized && wm_on_desk(w) && in_rect(x, y, w->x, w->y, w->w, w->h)) over = 1;
        }
        if (!over && dnd_node) deskicon_drop(dnd_node, dnd_label, dnd_icon, x, y);
    }
    dnd_active = 0; dnd_armed = 0; dnd_node = 0;
    if (press) { int p2 = press; press = 0; hov_damage(p2); }
}

int shell_wheel(int x, int y, int dz) {
    if (nc_open && in_rect(x, y, P[P_NC].x, P[P_NC].y, P[P_NC].w, P[P_NC].h)) {
        int last = notify_count() - nc_cards_visible(P[P_NC].h);
        if (last < 0) last = 0;
        nc_scroll -= dz;
        if (nc_scroll < 0) nc_scroll = 0;
        if (nc_scroll > last) nc_scroll = last;
        dmg_panel(P_NC);
        return 1;
    }
    if (start_open && in_rect(x, y, P[P_START].x, P[P_START].y, P[P_START].w, P[P_START].h)) {
        if (sm_mode == SM_ALL) { sm_scroll -= dz; if (sm_scroll < 0) sm_scroll = 0; dmg_panel(P_START); }
        return 1;
    }
    int over_tray = tray_hit(x, y) == 1;
    if (over_tray || (qs_open && in_rect(x, y, P[P_QS].x, P[P_QS].y, P[P_QS].w, P[P_QS].h))) {
        /* over the brightness row the wheel means brightness -- a wheel that
         * changes the volume while the pointer is on a brightness slider is the
         * sort of thing that only gets noticed at night */
        if (qs_open) {
            int sx, sy, sw; qs_slider_rect(0, P[P_QS].x, P[P_QS].y, &sx, &sy, &sw);
            if (y >= sy - 16 && y < sy + 16) { backlight_set(backlight_get() + dz * 5); dmg_panel(P_QS); return 1; }
        }
        int v = audio_volume() + dz * 5;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        audio_set_volume(v);
        dmg_panel(P_QS); dmg_panel(P_TRAY);
        return 1;
    }
    int k = dock_hit(x, y);
    if (k >= 0) return 1;
    return 0;
}

int shell_key(char c) {
    unsigned char u = (unsigned char)c;
    if (prompt_active) {
        if (u == 27)                      prompt_close();
        else if (u == '\n' || u == '\r')  prompt_confirm();
        else if (u == '\b') { if (prompt_len > 0) prompt_buf[--prompt_len] = 0; }
        else if (kbd_is_text(u) && prompt_len < (int)sizeof(prompt_buf) - 1) {
            prompt_buf[prompt_len++] = c; prompt_buf[prompt_len] = 0;
        }
        dmg_panel(P_PROMPT);
        return 1;
    }
    if (u == 27) {
        if (menu_open || start_open || qs_open || nc_open || sw_open) {
            sw_open = 0; dmg_panel(P_SWITCH);
            shell_close_popups();
            return 1;
        }
        return 0;
    }
    if (start_open) {
        if (u == '\b') {
            if (sm_qlen) sm_query[--sm_qlen] = 0;
            sm_mode = sm_qlen ? SM_SEARCH : SM_PINNED;
            sm_search();
        } else if (u == '\n') {
            if (sm_mode == SM_SEARCH && nsres) {
                int k = sm_sel < nsres ? sm_sel : 0;
                if (sres[k].app >= 0) sm_launch_app(sres[k].app); else sm_launch_path(sres[k].path);
            }
        } else if (u == KEY_DOWN) {
            if (nsres) sm_sel = (sm_sel + 1) % nsres;
        } else if (u == KEY_UP) {
            if (nsres) sm_sel = (sm_sel + nsres - 1) % nsres;
        } else if (kbd_is_text(u) && sm_qlen < (int)sizeof(sm_query) - 1) {
            sm_query[sm_qlen++] = c; sm_query[sm_qlen] = 0;
            sm_mode = SM_SEARCH;
            sm_search();
        }
        dmg_panel(P_START);
        return 1;
    }
    return 0;
}

int shell_cursor(int x, int y) {
    if (prompt_active) {
        panel_t *p = &P[P_PROMPT];
        if (in_rect(x, y, p->x + 24, p->y + 62, PR_W - 48, 38)) return CURSOR_TEXT;
        return -1;
    }
    if (start_open) {
        panel_t *p = &P[P_START];
        int fx, fy, fw, fh; sm_search_rect(p->x, p->y, p->w, &fx, &fy, &fw, &fh);
        if (in_rect(x, y, fx, fy, fw, fh)) return CURSOR_TEXT;
    }
    return -1;
}

void shell_tick(uint64_t now) {
    static int last_min = -1;
    struct rtc_time t; rtc_now(&t);
    if (t.min != last_min) { last_min = t.min; dmg_panel(P_TRAY); if (nc_open) dmg_panel(P_NC); }
    /* tray state that changes without telling anybody: link, battery, volume */
    static int last_sig;
    int sig = net_is_up() * 1000 + audio_volume() * 2 + mixer_muted(MIX_MASTER) + rec_active() * 4000;
    battery_t b; if (power_battery(&b) && b.present) sig += b.percent * 7 + b.charging * 3000;
    if (sig != last_sig) { last_sig = sig; dmg_panel(P_TRAY); if (qs_open) dmg_panel(P_QS); }
    /* carets */
    if (start_open && sm_qlen) dmg_panel(P_START);
    if (prompt_active) dmg_panel(P_PROMPT);
    /* toasts expiring */
    static int last_toasts = -1;
    int idx[TOAST_STACK]; int n = toast_live(idx);
    if (n != last_toasts) { last_toasts = n; for (int k = 0; k < TOAST_STACK; k++) dmg_panel(P_TOAST0 + k); }
    (void)now;
}

void shell_note(void) {
    for (int k = 0; k < TOAST_STACK; k++) dmg_panel(P_TOAST0 + k);
    dmg_panel(P_TRAY);
    if (nc_open) dmg_panel(P_NC);
    gui_animating();
}

void shell_theme_changed(void) {
    styles();
    for (int k = 0; k < P_COUNT; k++) P[k].dirty = 1;
    gui_all_stale();
    gui_damage_all();
}

/* ---- setup ------------------------------------------------------------------ */
static int title_cmp(int a, int b) { return strcasecmp(wm_wins[a].title, wm_wins[b].title); }

void shell_init(void) {
    styles();
    for (int k = 0; k < P_COUNT; k++) { P[k].shadow = 30; P[k].layer = layer_of[k]; P[k].g = glass_of(k); }
    P[P_DOCK].shadow = P[P_TRAY].shadow = P[P_PILL].shadow = 26;
    P[P_TIP].shadow = 14;
    P[P_MENU].shadow = 24;
    /* what is in the dock before anybody has opened anything */
    static const char *DOCK_PINS[] = { "File Explorer", "Browser", "Terminal", "Code", "Notepad",
                                       "Photos", "Media", "Settings" };
    for (unsigned k = 0; k < sizeof(DOCK_PINS) / sizeof(DOCK_PINS[0]); k++)
        for (int i = 0; i < wm_nwin; i++)
            if (!strcmp(wm_wins[i].title, DOCK_PINS[k])) wm_wins[i].pinned = 1;
    npin = 0;
    for (int k = 0; k < NPINS; k++)
        for (int i = 0; i < wm_nwin; i++)
            if (!strcmp(wm_wins[i].title, PINS[k])) { pin_idx[npin++] = i; break; }
    nall = 0;
    for (int i = 0; i < wm_nwin; i++) all_idx[nall++] = i;
    for (int a = 1; a < nall; a++) {               /* insertion sort by name */
        int v = all_idx[a], b = a - 1;
        while (b >= 0 && title_cmp(all_idx[b], v) > 0) { all_idx[b + 1] = all_idx[b]; b--; }
        all_idx[b + 1] = v;
    }
    dock_build();
}
