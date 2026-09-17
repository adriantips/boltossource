/* ===========================================================================
 *  BoltOS  -  kernel/app_clipboard.c
 *  Clipboard History -- Win+V, and the one Windows feature people miss most
 *  when they sit down at a machine that does not have it.
 *
 *  The store is kernel/clipboard.c; this is only a list of it. Clicking an
 *  entry puts it back on the clipboard, which is the entire interaction: there
 *  is deliberately no "paste into the window behind me", because that would
 *  mean synthesising keystrokes into whatever had focus a moment ago, and the
 *  moment you get that wrong you have typed a paragraph into somebody's shell.
 *  Pick it here, then Ctrl+V where you meant to.
 *
 *  Pinning is the other half. The ring is sixteen entries and a busy session
 *  fills it in a minute; a pinned entry survives that and survives Clear.
 * ===========================================================================*/
#include <stdint.h>
#include <stdio.h>
#include "gui.h"
#include "clipboard.h"
#include "keyboard.h"
#include "notify.h"
#include "string.h"

#define HEAD_H   40
#define ROW_H    54
#define PAD      10

static struct {
    int  scroll;
    int  sel;
    int  rows, cw, ch;
} H;

static window_t *win;

/* ------------------------------------------------------------- drawing ---- */
/* One line of a text entry, with control characters flattened. A clipboard
 * entry is very often a whole file, and rendering the newlines as glyphs makes
 * every row look identical. */
static void preview_line(char *out, int cap, const char *src, int from, int len) {
    int k = 0, i = from;
    while (i < len && k < cap - 1) {
        unsigned char c = (unsigned char)src[i++];
        if (c == '\n' || c == '\r') break;
        out[k++] = (c == '\t') ? ' ' : (c < 32 ? ' ' : (char)c);
    }
    out[k] = 0;
}

static int next_line(const char *src, int from, int len) {
    while (from < len && src[from] != '\n') from++;
    return from < len ? from + 1 : len;
}

static void kind_badge(int x, int y, int kind, int pinned) {
    const char *t = kind == CLIP_IMAGE ? "IMG" : kind == CLIP_FILES ? "FILES" : "TEXT";
    uint32_t c = kind == CLIP_IMAGE ? 0xF06292 : kind == CLIP_FILES ? 0x4FC3F7 : 0x9CCC65;
    int w = g_text_width(t, 1) + 10;
    g_round(x, y, w, 14, 4, c, 200);
    g_text(x + 5, y + 3, t, 0x101018, 1);
    if (pinned) {
        /* a pushpin, four fills: head, shaft, point, and the little crossbar */
        int px = x + w + 8;
        g_fill(px - 3, y + 1, 7, 4, COL_WARN);
        g_fill(px - 1, y + 5, 3, 5, COL_WARN);
        g_fill(px - 5, y + 9, 11, 2, COL_WARN);
        g_fill(px, y + 11, 1, 3, COL_WARN);
    }
}

static void cb_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    H.cw = cw; H.ch = ch;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    int n = clip_hist_count();

    /* header */
    g_text(cx + 12, cy + 14, "Clipboard history", COL_TEXT, 1);
    char cnt[40];
    snprintf(cnt, sizeof cnt, "%d of %d", n, CLIP_HIST);
    g_text(cx + 12 + g_text_width("Clipboard history", 1) + 12, cy + 14, cnt, COL_TEXT_DIM, 1);
    g_round(cx + cw - 70, cy + 8, 58, 24, 6, COL_PANEL_3, 255);
    g_text(cx + cw - 70 + (58 - g_text_width("Clear", 1)) / 2, cy + 16, "Clear",
           n ? COL_TEXT : COL_TEXT_DIM, 1);
    g_hline(cx + 10, cy + HEAD_H - 6, cw - 20, COL_PANEL_3);

    int top = cy + HEAD_H;
    H.rows = (ch - HEAD_H - 6) / ROW_H;
    if (H.rows < 1) H.rows = 1;
    if (H.scroll > n - H.rows) H.scroll = n - H.rows;
    if (H.scroll < 0) H.scroll = 0;

    if (!n) {
        const char *l1 = "Nothing copied yet.";
        const char *l2 = "Ctrl+C anywhere and it turns up here.";
        g_text(cx + (cw - g_text_width(l1, 1)) / 2, top + 30, l1, COL_TEXT_DIM, 1);
        g_text(cx + (cw - g_text_width(l2, 1)) / 2, top + 48, l2, COL_TEXT_DIM, 1);
        return;
    }

    g_set_clip(cx, top, cw, ch - HEAD_H - 6);
    for (int r = 0; r < H.rows; r++) {
        int i = H.scroll + r;
        if (i >= n) break;
        int y = top + r * ROW_H;
        int kind = clip_hist_kind(i);

        g_round(cx + PAD, y + 2, cw - 2 * PAD, ROW_H - 6, 6,
                i == H.sel ? COL_ACCENT_DIM : COL_PANEL_2, 255);
        if (i == 0) g_fill(cx + PAD, y + 2, 3, ROW_H - 6, COL_ACCENT);   /* on the clipboard now */

        int tx = cx + PAD + 10;
        kind_badge(tx, y + 8, kind, clip_hist_pinned(i));

        if (kind == CLIP_IMAGE) {
            int iw, ih;
            const uint32_t *px = clip_hist_image(i, &iw, &ih);
            char dim[32];
            snprintf(dim, sizeof dim, "%d x %d", iw, ih);
            g_text(tx, y + 28, dim, COL_TEXT, 1);
            if (px && iw > 0 && ih > 0) {
                int th = ROW_H - 16, tw2 = th * iw / ih;
                int cap = cw - 2 * PAD - 150;
                if (tw2 > cap && cap > 8) { tw2 = cap; th = tw2 * ih / iw; }
                if (tw2 > 0 && th > 0)
                    g_blit(cx + cw - PAD - 44 - tw2, y + 4 + (ROW_H - 10 - th) / 2,
                           tw2, th, px, iw, ih);
            }
        } else {
            const char *t = clip_hist_text(i);
            int len = clip_hist_len(i);
            char line[128];
            int cols = (cw - 2 * PAD - 110) / 8;
            if (cols > (int)sizeof(line) - 1) cols = (int)sizeof(line) - 1;
            if (cols < 8) cols = 8;

            /* two lines of preview, then how much more there is */
            int p = 0;
            for (int l = 0; l < 2 && p < len; l++) {
                preview_line(line, cols + 1, t, p, len);
                g_text(tx, y + 26 + l * 14, line, l ? COL_TEXT_DIM : COL_TEXT, 1);
                p = next_line(t, p, len);
            }
            char sz[32];
            if (kind == CLIP_FILES) {
                int f = 0;
                for (int k = 0; k < len; k++)
                    if (t[k] != '\n' && (k == 0 || t[k - 1] == '\n')) f++;
                snprintf(sz, sizeof sz, "%d file%s", f, f == 1 ? "" : "s");
            } else {
                snprintf(sz, sizeof sz, "%d chars", len);
            }
            g_text(cx + cw - PAD - 52 - g_text_width(sz, 1), y + 10, sz, COL_TEXT_DIM, 1);
        }

        /* per-row buttons: pin, and remove */
        int bx = cx + cw - PAD - 38;
        g_round(bx, y + 8, 16, 16, 4, clip_hist_pinned(i) ? COL_WARN : COL_PANEL_3, 255);
        g_fill(bx + 5, y + 11, 6, 3, clip_hist_pinned(i) ? 0x101018 : COL_TEXT_DIM);
        g_fill(bx + 7, y + 14, 2, 5, clip_hist_pinned(i) ? 0x101018 : COL_TEXT_DIM);
        g_round(bx, y + 28, 16, 16, 4, COL_PANEL_3, 255);
        g_fill(bx + 4, y + 35, 8, 2, COL_BAD);
    }
    g_clear_clip();

    if (n > H.rows) {
        int track = ch - HEAD_H - 6;
        int bh = track * H.rows / n;
        if (bh < 18) bh = 18;
        int by = top + (track - bh) * H.scroll / (n - H.rows);
        g_fill(cx + cw - 5, top, 3, track, COL_PANEL_2);
        g_round(cx + cw - 5, by, 3, bh, 2, COL_TEXT_DIM, 255);
    }
}

/* --------------------------------------------------------------- input ---- */
static void cb_scroll(window_t *w, int delta) {
    (void)w;
    H.scroll -= delta;
    int last = clip_hist_count() - H.rows;
    if (last < 0) last = 0;
    if (H.scroll > last) H.scroll = last;
    if (H.scroll < 0) H.scroll = 0;
    gui_request_redraw();
}

static void restore(int i) {
    if (clip_hist_restore(i) != 0) { gui_notify(NOTE_ERROR, "Clipboard", "Could not restore that entry"); return; }
    H.sel = 0; H.scroll = 0;
    int kind = clip_kind();
    gui_notify(NOTE_SUCCESS, "Clipboard",
               kind == CLIP_IMAGE ? "Image is on the clipboard. Ctrl+V to paste."
                                  : "On the clipboard. Ctrl+V to paste.");
}

static void cb_click(window_t *w, int lx, int ly) {
    (void)w;
    if (!H.cw) return;

    if (ly >= 8 && ly < 32 && lx >= H.cw - 70 && lx < H.cw - 12) {
        clip_hist_clear();
        H.sel = 0; H.scroll = 0;
        gui_request_redraw();
        return;
    }
    if (ly < HEAD_H) return;

    int r = (ly - HEAD_H) / ROW_H;
    int i = H.scroll + r;
    if (i < 0 || i >= clip_hist_count()) return;
    int ry = (ly - HEAD_H) - r * ROW_H;

    int bx = H.cw - PAD - 38;
    if (lx >= bx && lx < bx + 16) {
        if (ry >= 8 && ry < 24)  { clip_hist_pin(i, !clip_hist_pinned(i)); gui_request_redraw(); return; }
        if (ry >= 28 && ry < 44) { clip_hist_remove(i); if (H.sel >= clip_hist_count()) H.sel = 0;
                                   gui_request_redraw(); return; }
    }

    H.sel = i;
    restore(i);
    gui_request_redraw();
}

static void cb_key(window_t *w, char c) {
    (void)w;
    unsigned char u = (unsigned char)c;
    int n = clip_hist_count();
    switch (u) {
    case KEY_UP:    if (H.sel > 0) H.sel--; break;
    case KEY_DOWN:  if (H.sel < n - 1) H.sel++; break;
    case KEY_HOME:  H.sel = 0; break;
    case KEY_END:   H.sel = n ? n - 1 : 0; break;
    case KEY_DEL:   clip_hist_remove(H.sel); if (H.sel >= clip_hist_count()) H.sel = 0; break;
    case '\n': case '\r': if (n) restore(H.sel); break;
    case 'p': case 'P': clip_hist_pin(H.sel, !clip_hist_pinned(H.sel)); break;
    default: return;
    }
    if (H.sel < H.scroll) H.scroll = H.sel;
    if (H.sel >= H.scroll + H.rows) H.scroll = H.sel - H.rows + 1;
    gui_request_redraw();
}

/* The list changes when anything anywhere copies, and nothing tells us. Two
 * ticks a second is cheap and the alternative is a hook into the clipboard for
 * the sake of one window. */
static void cb_tick(window_t *w) {
    static int last = -1;
    (void)w;
    int n = clip_hist_count();
    if (n != last) { last = n; gui_request_redraw(); }
}

void clipboard_app_open(void) {
    if (!win) return;
    H.sel = 0; H.scroll = 0;
    gui_open(win);
}

void clipboard_app_init(void) {
    memset(&H, 0, sizeof H);
    win = gui_add_window("Clipboard", 420, 420, 0x4DD0E1, ICON_CLIPBOARD);
    if (!win) return;
    win->draw   = cb_draw;
    win->click  = cb_click;
    win->scroll = cb_scroll;
    win->key    = cb_key;
    win->tick   = cb_tick;
    win->min_w  = 320; win->min_h = 240;
    win->x = 560; win->y = 120;
}
