/* ===========================================================================
 *  BoltOS  -  kernel/app_snip.c
 *  Snipping Tool. Grab the screen, cut a rectangle out of it, keep it.
 *
 *  Print Screen already writes a whole-panel PNG into ~/Pictures and that is
 *  the wrong granularity most of the time: what somebody wants is one window,
 *  one error message, one row of a table. So the crop is the feature and the
 *  capture is the easy part.
 *
 *  Windows dims the live screen and lets you drag on it. Doing that here would
 *  mean a compositor mode that hands every mouse event to one app and paints
 *  over everything else -- real work, and for what you get, not obviously
 *  better than capturing first and cropping in the preview, which is what
 *  Snip & Sketch ends up doing anyway once you press the shutter. So: capture,
 *  then drag on the picture. The delay is there because a menu closes the
 *  moment you click New.
 *
 *  It minimises itself before capturing. Without that the first thing every
 *  snip contains is the Snipping Tool.
 * ===========================================================================*/
#include <stdint.h>
#include <stdio.h>
#include "gui.h"
#include "framebuffer.h"
#include "png_enc.h"
#include "screenshot.h"
#include "clipboard.h"
#include "keyboard.h"
#include "kheap.h"
#include "notify.h"
#include "pit.h"
#include "fs.h"
#include "string.h"

#define HEAD_H   40
#define FOOT_H   34

static struct {
    uint32_t *shot;              /* the captured panel, pw*ph ARGB */
    int       pw, ph;
    int       delay;             /* seconds before the shutter: 0, 3 or 10 */
    /* selection, in captured-image pixels; w/h of 0 means "the whole thing" */
    int       sx, sy, sw, sh;
    int       dragging, dx0, dy0;
    /* the preview rectangle from the last draw, client-local, for hit-testing */
    int       vx, vy, vw, vh;
    int       cw, ch;
    char      status[72];
} S;

static window_t *win;

static void set_status(const char *s) { strlcpy(S.status, s, sizeof S.status); }

/* image pixel <-> preview pixel. The preview is a uniform scale-to-fit, so one
 * ratio does both axes and a selection cannot come out stretched. */
static int img_x(int view_x) { return S.vw ? (view_x - S.vx) * S.pw / S.vw : 0; }
static int img_y(int view_y) { return S.vh ? (view_y - S.vy) * S.ph / S.vh : 0; }

static void clamp_sel(void) {
    if (S.sx < 0) S.sx = 0;
    if (S.sy < 0) S.sy = 0;
    if (S.sx > S.pw) S.sx = S.pw;
    if (S.sy > S.ph) S.sy = S.ph;
    if (S.sx + S.sw > S.pw) S.sw = S.pw - S.sx;
    if (S.sy + S.sh > S.ph) S.sh = S.ph - S.sy;
    if (S.sw < 0) S.sw = 0;
    if (S.sh < 0) S.sh = 0;
}

/* The rectangle a Copy or Save would act on: the drawn selection, or the whole
 * capture when there is not one. Below a few pixels a "selection" is a stray
 * click, and treating it as one loses the picture. */
static void active_rect(int *x, int *y, int *w, int *h) {
    if (S.sw >= 4 && S.sh >= 4) { *x = S.sx; *y = S.sy; *w = S.sw; *h = S.sh; }
    else                        { *x = 0; *y = 0; *w = S.pw; *h = S.ph; }
}

/* ------------------------------------------------------------- capture ---- */
static void capture(void) {
    if (!fb_present()) { set_status("No panel to capture."); return; }

    int was_min = win->minimized;
    win->minimized = 1;
    gui_request_redraw();
    gui_pump();                              /* repaint the desktop without us */

    /* Ten seconds of gui_pump() as fast as it will go is ten seconds of
     * blitting 786k pixels over and over for a screen that is not changing.
     * Ten frames a second is plenty to keep the clock honest, and the hlt
     * hands the machine back to everything else in between. */
    if (S.delay) {
        uint64_t until = pit_ticks() + (uint64_t)S.delay * 1000, next = 0;
        while (pit_ticks() < until) {
            if (pit_ticks() >= next) { gui_pump(); next = pit_ticks() + 100; }
            __asm__ volatile("hlt");
        }
    }

    uint32_t w = fb_width(), h = fb_height();
    if (!w || !h) { win->minimized = was_min; set_status("No panel to capture."); return; }

    uint32_t *px = (uint32_t *)kmalloc((uint64_t)w * h * 4);
    if (!px) { win->minimized = was_min; set_status("Out of memory."); return; }
    if (fb_capture(px) != 0) {
        kfree(px);
        win->minimized = was_min;
        set_status("The panel would not read back.");
        return;
    }

    if (S.shot) kfree(S.shot);
    S.shot = px; S.pw = (int)w; S.ph = (int)h;
    S.sx = S.sy = S.sw = S.sh = 0;

    win->minimized = was_min;
    gui_focus(win);
    snprintf(S.status, sizeof S.status, "Captured %dx%d. Drag to crop.", S.pw, S.ph);
    gui_request_redraw();
}

/* Pack the selected rectangle into a fresh tight buffer. Both Copy and Save
 * need one and neither should be the one that owns it. */
static uint32_t *crop(int *ow, int *oh) {
    int x, y, w, h;
    active_rect(&x, &y, &w, &h);
    if (w <= 0 || h <= 0) return 0;
    uint32_t *out = (uint32_t *)kmalloc((uint64_t)w * h * 4);
    if (!out) return 0;
    for (int r = 0; r < h; r++)
        memcpy(out + (uint64_t)r * w, S.shot + (uint64_t)(y + r) * S.pw + x, (uint32_t)w * 4);
    *ow = w; *oh = h;
    return out;
}

static void do_copy(void) {
    if (!S.shot) { set_status("Nothing captured yet."); return; }
    int w, h;
    uint32_t *px = crop(&w, &h);
    if (!px) { set_status("Out of memory."); return; }
    int rc = clip_set_image(px, w, h);
    kfree(px);
    if (rc == 0) { snprintf(S.status, sizeof S.status, "%dx%d copied to the clipboard.", w, h);
                   gui_notify(NOTE_SUCCESS, "Snipping Tool", "Snip copied to the clipboard"); }
    else set_status("The clipboard refused it.");
}

/* ~/Pictures/snip-N.png for the lowest free N. Same reasoning as the
 * screenshot path next door: the clock may not have been set yet, so a
 * timestamp is not a unique name. */
static void next_path(char *out, uint32_t cap) {
    char dir[FS_PATH_MAX];
    shot_dir(dir, sizeof dir);
    for (int n = 1; n < 10000; n++) {
        snprintf(out, cap, "%s/snip-%d.png", dir, n);
        if (!fs_lookup(out)) return;
    }
    snprintf(out, cap, "%s/snip.png", dir);
}

static void do_save(void) {
    if (!S.shot) { set_status("Nothing captured yet."); return; }
    int w, h;
    uint32_t *px = crop(&w, &h);
    if (!px) { set_status("Out of memory."); return; }

    uint8_t *png = 0; uint32_t len = 0;
    int rc = png_encode(px, w, h, &png, &len);
    kfree(px);
    if (rc != 0 || !png) { set_status("Could not encode the PNG."); return; }

    char path[FS_PATH_MAX];
    next_path(path, sizeof path);
    if (shot_ensure_parent(path) != 0) { kfree(png); set_status("Could not make ~/Pictures."); return; }

    fs_node *f = fs_lookup(path);
    if (!f) {
        char leaf[FS_NAME_MAX];
        fs_node *d = fs_parent_of(path, leaf, sizeof leaf);
        if (!fs_may_create_in(d)) { kfree(png); set_status("No permission to write there."); return; }
        f = fs_create(path, 0);
    }
    if (!f || f->is_dir || fs_write(f, png, len) != 0) {
        kfree(png); set_status("Could not write the file."); return;
    }
    kfree(png);
    fs_sync();

    snprintf(S.status, sizeof S.status, "Saved %s", path);
    gui_notify(NOTE_SUCCESS, "Snipping Tool", S.status);
}

/* ------------------------------------------------------------- drawing ---- */
static void btn(int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg) {
    g_round(x, y, w, h, 6, bg, 255);
    g_text(x + (w - g_text_width(label, 1)) / 2, y + (h - 8) / 2, label, fg, 1);
}

/* toolbar geometry, shared by the draw and the hit test so they cannot drift */
enum { BTN_NEW = 0, BTN_DELAY, BTN_COPY, BTN_SAVE, BTN_CLEAR, NBTN };
static const int BTN_W[NBTN] = { 56, 70, 52, 52, 60 };
static const char *BTN_LABEL[NBTN] = { "New", "", "Copy", "Save", "Reset" };

static int btn_x(int i) {
    int x = 12;
    for (int k = 0; k < i; k++) x += BTN_W[k] + 6;
    return x;
}

static void snip_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    S.cw = cw; S.ch = ch;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    /* toolbar */
    for (int i = 0; i < NBTN; i++) {
        char lab[16];
        if (i == BTN_DELAY) snprintf(lab, sizeof lab, S.delay ? "Delay %ds" : "No delay", S.delay);
        else                strlcpy(lab, BTN_LABEL[i], sizeof lab);
        int live = (i == BTN_NEW || i == BTN_DELAY) ? 1 : (S.shot != 0);
        btn(cx + btn_x(i), cy + 8, BTN_W[i], 24, lab,
            i == BTN_NEW ? COL_ACCENT : COL_PANEL_3,
            i == BTN_NEW ? 0xFFFFFF : (live ? COL_TEXT : COL_TEXT_DIM));
    }
    g_hline(cx + 10, cy + HEAD_H - 6, cw - 20, COL_PANEL_3);

    /* preview area */
    int ax = cx + 10, ay = cy + HEAD_H, aw = cw - 20, ah = ch - HEAD_H - FOOT_H;
    if (aw < 20 || ah < 20) return;
    g_round(ax, ay, aw, ah, 6, COL_PANEL_2, 255);

    if (!S.shot) {
        const char *l1 = "Press New to capture the screen.";
        const char *l2 = "Then drag on the picture to crop it.";
        g_text(ax + (aw - g_text_width(l1, 1)) / 2, ay + ah / 2 - 14, l1, COL_TEXT_DIM, 1);
        g_text(ax + (aw - g_text_width(l2, 1)) / 2, ay + ah / 2 + 2, l2, COL_TEXT_DIM, 1);
        S.vw = S.vh = 0;
        /* Not a return: the footer carries the status line, and "no panel to
         * capture" is exactly the message you need when there is no picture. */
        g_text(cx + 12, cy + ch - 22, S.status, COL_TEXT_DIM, 1);
        return;
    }

    /* scale to fit, integer ratio both axes so nothing is stretched */
    int vw = aw - 8, vh = ah - 8;
    if (S.pw * vh > S.ph * vw) vh = vw * S.ph / S.pw;
    else                       vw = vh * S.pw / S.ph;
    if (vw < 1) vw = 1;
    if (vh < 1) vh = 1;
    S.vx = ax + (aw - vw) / 2; S.vy = ay + (ah - vh) / 2; S.vw = vw; S.vh = vh;

    g_set_clip(ax, ay, aw, ah);
    g_blit(S.vx, S.vy, vw, vh, S.shot, S.pw, S.ph);

    if (S.sw >= 4 && S.sh >= 4) {
        int rx = S.vx + S.sx * vw / S.pw, ry = S.vy + S.sy * vh / S.ph;
        int rw = S.sw * vw / S.pw,        rh = S.sh * vh / S.ph;
        /* dim what is being thrown away, so the crop reads at a glance */
        g_blend(S.vx, S.vy, vw, ry - S.vy, 0x000000, 150);
        g_blend(S.vx, ry + rh, vw, S.vy + vh - ry - rh, 0x000000, 150);
        g_blend(S.vx, ry, rx - S.vx, rh, 0x000000, 150);
        g_blend(rx + rw, ry, S.vx + vw - rx - rw, rh, 0x000000, 150);
        g_rect(rx, ry, rw, rh, COL_ACCENT);
        /* corner pips, because a 1px outline over a screenshot of a desktop
         * disappears into whatever is underneath it */
        g_fill(rx - 1, ry - 1, 6, 2, 0xFFFFFF); g_fill(rx - 1, ry - 1, 2, 6, 0xFFFFFF);
        g_fill(rx + rw - 5, ry - 1, 6, 2, 0xFFFFFF); g_fill(rx + rw - 1, ry - 1, 2, 6, 0xFFFFFF);
        g_fill(rx - 1, ry + rh - 1, 6, 2, 0xFFFFFF); g_fill(rx - 1, ry + rh - 5, 2, 6, 0xFFFFFF);
        g_fill(rx + rw - 5, ry + rh - 1, 6, 2, 0xFFFFFF); g_fill(rx + rw - 1, ry + rh - 5, 2, 6, 0xFFFFFF);
    }
    g_clear_clip();

    /* footer: what will be copied or saved */
    int x, y, sw, sh;
    active_rect(&x, &y, &sw, &sh);
    char dim[48];
    snprintf(dim, sizeof dim, "%d x %d%s", sw, sh,
             (S.sw >= 4 && S.sh >= 4) ? "  selection" : "  whole screen");
    g_text(cx + 12, cy + ch - 22, S.status, COL_TEXT_DIM, 1);
    g_text(cx + cw - 12 - g_text_width(dim, 1), cy + ch - 22, dim, COL_TEXT, 1);
}

/* --------------------------------------------------------------- input ---- */
static void snip_click(window_t *w, int lx, int ly) {
    (void)w;
    if (ly >= 8 && ly < 32) {
        for (int i = 0; i < NBTN; i++) {
            int bx = btn_x(i);
            if (lx < bx || lx >= bx + BTN_W[i]) continue;
            switch (i) {
            case BTN_NEW:   capture(); break;
            case BTN_DELAY: S.delay = S.delay == 0 ? 3 : S.delay == 3 ? 10 : 0; break;
            case BTN_COPY:  do_copy(); break;
            case BTN_SAVE:  do_save(); break;
            case BTN_CLEAR: S.sx = S.sy = S.sw = S.sh = 0; set_status(""); break;
            }
            gui_request_redraw();
            return;
        }
        return;
    }
    /* A click on the picture starts a fresh selection; the drag handler
     * finishes it. A click with no drag clears it, which is how you get the
     * whole screen back. */
    if (!S.shot || !S.vw) return;
    if (lx < S.vx || lx >= S.vx + S.vw || ly < S.vy || ly >= S.vy + S.vh) return;
    S.dx0 = img_x(lx); S.dy0 = img_y(ly);
    S.dragging = 1;
    S.sx = S.dx0; S.sy = S.dy0; S.sw = 0; S.sh = 0;
    gui_request_redraw();
}

static void snip_drag(window_t *w, int lx, int ly) {
    (void)w;
    if (!S.dragging || !S.shot) return;
    int x1 = img_x(lx), y1 = img_y(ly);
    if (x1 < 0) x1 = 0; if (x1 > S.pw) x1 = S.pw;
    if (y1 < 0) y1 = 0; if (y1 > S.ph) y1 = S.ph;
    S.sx = S.dx0 < x1 ? S.dx0 : x1;
    S.sy = S.dy0 < y1 ? S.dy0 : y1;
    S.sw = S.dx0 < x1 ? x1 - S.dx0 : S.dx0 - x1;
    S.sh = S.dy0 < y1 ? y1 - S.dy0 : S.dy0 - y1;
    clamp_sel();
    gui_request_redraw();
}

static void snip_key(window_t *w, char c) {
    (void)w;
    unsigned char u = (unsigned char)c;
    if (u == KEY_COPY)      do_copy();
    else if (u == KEY_SAVE) do_save();
    else if (u == 27)       { S.sx = S.sy = S.sw = S.sh = 0; }
    else if (u == '\n' || u == '\r') capture();
    else return;
    gui_request_redraw();
}

/* The compositor gives no button-up event, so a drag ends when the next click
 * starts. This just stops a stale drag from following the cursor around after
 * the window loses focus. */
static void snip_tick(window_t *w) {
    if (S.dragging && !gui_window_focused(w)) S.dragging = 0;
}

void snip_app_init(void) {
    memset(&S, 0, sizeof S);
    set_status("Ready.");
    win = gui_add_window("Snipping Tool", 560, 440, 0xF06292, ICON_SNIP);
    if (!win) return;
    win->draw  = snip_draw;
    win->click = snip_click;
    win->drag  = snip_drag;
    win->key   = snip_key;
    win->tick  = snip_tick;
    win->min_w = 360; win->min_h = 280;
    win->x = 250; win->y = 140;
}
