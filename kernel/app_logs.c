/* ===========================================================================
 *  BoltOS  -  kernel/app_logs.c
 *  Logs: what the machine said, including on the boots that already ended.
 *
 *  Three different logs, and the difference between them is the whole point of
 *  showing them together. The ring is this boot and lives in RAM. kernel.log is
 *  what survived earlier boots. crash.log is faults and panics, kept separately
 *  because it is the one somebody goes looking for on purpose.
 *
 *  Everything here is read-only except Save, which writes the current ring to
 *  disk -- the thing to press before rebooting a machine that has just done
 *  something inexplicable, since otherwise only a *clean* shutdown persists it.
 * ===========================================================================*/
#include <stdint.h>
#include <stdio.h>
#include "gui.h"
#include "keyboard.h"   /* KEY_PGUP and friends */
#include "klog.h"
#include "fs.h"
#include "notify.h"
#include "string.h"

#define LOGBUF   (48 * 1024)
#define MAXLINE  1200
#define LINE_H   14
#define HEAD_H   46

enum { TAB_BOOT = 0, TAB_EARLIER, TAB_CRASH, NTAB };

typedef struct {
    int    tab;
    char   buf[LOGBUF];
    uint32_t len;
    uint32_t line[MAXLINE];          /* offset of each line's first character */
    int    nline;
    int    scroll;
    int    rows, cw, ch;
    uint64_t loaded;                 /* pit tick of the last load             */
} logs_t;

static logs_t st;
static window_t *win;

static const char *TABNAME[NTAB] = { "This boot", "Earlier boots", "Crashes" };

static void index_lines(void) {
    st.nline = 0;
    st.line[st.nline++] = 0;
    for (uint32_t i = 0; i < st.len && st.nline < MAXLINE; i++)
        if (st.buf[i] == '\n' && i + 1 < st.len) st.line[st.nline++] = i + 1;
}

static void load_file(const char *path) {
    st.len = 0;
    fs_node *f = fs_lookup(path);
    if (f && f->data && f->size) {
        /* The tail is the part anybody wants; a log longer than the buffer is
         * shown from its end, not its beginning. */
        uint32_t n = f->size, from = 0;
        if (n > LOGBUF) { from = n - LOGBUF; n = LOGBUF; }
        memcpy(st.buf, f->data + from, n);
        st.len = n;
    }
    index_lines();
}

static void load(int tab) {
    if (tab == TAB_BOOT)         { st.len = klog_snapshot(st.buf, LOGBUF); index_lines(); }
    else if (tab == TAB_EARLIER) load_file(KERNEL_LOG_PATH);
    else                         load_file(CRASH_LOG_PATH);
    if (st.scroll > st.nline - 1) st.scroll = st.nline - 1;
    if (st.scroll < 0) st.scroll = 0;
}

/* ------------------------------------------------------------- drawing ---- */
static void tab_rect(int cx, int cy, int i, int *x, int *y, int *w, int *h) {
    *w = 118; *h = 24;
    *x = cx + 12 + i * (*w + 5);
    *y = cy + 10;
}
static void save_rect(int cx, int cy, int cw, int *x, int *y, int *w, int *h) {
    *w = 74; *h = 24; *x = cx + cw - *w - 12; *y = cy + 10;
}

static void logs_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    st.cw = cw; st.ch = ch;

    for (int t = 0; t < NTAB; t++) {
        int x, y, bw, bh; tab_rect(cx, cy, t, &x, &y, &bw, &bh);
        int on = (st.tab == t);
        g_round(x, y, bw, bh, 6, on ? COL_ACCENT : 0x22222E, 255);
        g_text(x + bw / 2 - g_text_width(TABNAME[t], 1) / 2, y + 6, TABNAME[t],
               on ? 0xFFFFFF : COL_TEXT_DIM, 1);
    }
    if (st.tab == TAB_BOOT) {
        int x, y, bw, bh; save_rect(cx, cy, cw, &x, &y, &bw, &bh);
        g_round(x, y, bw, bh, 6, 0x22222E, 255);
        g_text(x + bw / 2 - g_text_width("Save", 1) / 2, y + 6, "Save", COL_TEXT_DIM, 1);
    }
    g_hline(cx + 10, cy + HEAD_H - 6, cw - 20, 0x2A2A38);

    int top = cy + HEAD_H, bottom = cy + ch - 20;
    st.rows = (bottom - top) / LINE_H;
    if (st.rows < 1) st.rows = 1;

    if (!st.len) {
        const char *why =
            st.tab == TAB_BOOT    ? "Nothing in the ring yet." :
            st.tab == TAB_EARLIER ? "No log from an earlier boot. Press Save, or shut down cleanly."
                                  : "No crashes recorded. Good.";
        g_text(cx + 14, top + 8, why, COL_TEXT_DIM, 1);
        return;
    }

    int cols = (cw - 28) / 8;
    for (int r = 0; r < st.rows; r++) {
        int i = st.scroll + r;
        if (i >= st.nline) break;
        uint32_t s = st.line[i];
        uint32_t e = (i + 1 < st.nline) ? st.line[i + 1] : st.len;
        while (e > s && (st.buf[e - 1] == '\n' || st.buf[e - 1] == '\r')) e--;
        int n = (int)(e - s);
        if (n > cols) n = cols;

        /* Colour by the kernel's own tag, so a page of log can be skimmed. */
        uint32_t col = COL_TEXT_DIM;
        if (n >= 4 && st.buf[s] == '[') {
            if      (!memcmp(st.buf + s, "[ok]", 4)) col = COL_GOOD;
            else if (!memcmp(st.buf + s, "[--]", 4)) col = COL_WARN;
            else                                     col = COL_TEXT;
        }
        if (n >= 4 && !memcmp(st.buf + s, "====", 4)) col = COL_BAD;

        char line[200];
        int k = 0;
        for (; k < n && k < (int)sizeof(line) - 1; k++) {
            char c = st.buf[s + k];
            line[k] = (c >= 32 && c < 127) ? c : ' ';
        }
        line[k] = 0;
        g_text(cx + 14, top + r * LINE_H, line, col, 1);
    }

    char pos[48];
    snprintf(pos, sizeof pos, "%d-%d of %d lines", st.scroll + 1,
             st.scroll + st.rows > st.nline ? st.nline : st.scroll + st.rows, st.nline);
    g_text(cx + cw - g_text_width(pos, 1) - 14, cy + ch - 16, pos, COL_TEXT_DIM, 1);
}

/* --------------------------------------------------------------- input ---- */
static void logs_scroll(window_t *w, int delta) {
    (void)w;
    st.scroll -= delta * 3;
    int last = st.nline - st.rows;
    if (last < 0) last = 0;
    if (st.scroll > last) st.scroll = last;
    if (st.scroll < 0) st.scroll = 0;
    gui_request_redraw();
}

static void logs_click(window_t *w, int lx, int ly) {
    (void)w;
    if (!st.cw) return;
    for (int t = 0; t < NTAB; t++) {
        int x, y, bw, bh; tab_rect(0, 0, t, &x, &y, &bw, &bh);
        if (lx >= x && lx < x + bw && ly >= y && ly < y + bh) {
            st.tab = t; st.scroll = 0; load(t);
            /* Land at the bottom: the newest lines are the ones worth seeing. */
            st.scroll = st.nline > st.rows ? st.nline - st.rows : 0;
            gui_request_redraw();
            return;
        }
    }
    if (st.tab == TAB_BOOT) {
        int x, y, bw, bh; save_rect(0, 0, st.cw, &x, &y, &bw, &bh);
        if (lx >= x && lx < x + bw && ly >= y && ly < y + bh) {
            int rc = klog_persist();
            if (rc == 0) fs_sync();
            gui_notify(rc ? NOTE_ERROR : NOTE_SUCCESS, "Logs",
                       rc ? "Could not write /var/log/kernel.log"
                          : "This boot's log written to /var/log/kernel.log");
            gui_request_redraw();
        }
    }
}

static void logs_key(window_t *w, char c) {
    (void)w;
    unsigned char u = (unsigned char)c;
    if      (u == KEY_PGUP) logs_scroll(0, +5);
    else if (u == KEY_PGDN) logs_scroll(0, -5);
    else if (u == KEY_UP)   logs_scroll(0, +1);
    else if (u == KEY_DOWN) logs_scroll(0, -1);
    else if (u == KEY_HOME) { st.scroll = 0; gui_request_redraw(); }
    else if (u == KEY_END)  { st.scroll = st.nline > st.rows ? st.nline - st.rows : 0;
                              gui_request_redraw(); }
}

/* The ring grows while the window is open; re-read it so "This boot" is live
 * rather than a snapshot of whenever the tab was clicked. */
static void logs_tick(window_t *w) {
    (void)w;
    if (st.tab != TAB_BOOT) return;
    int at_end = st.scroll >= st.nline - st.rows;
    int before = st.nline;
    load(TAB_BOOT);
    if (st.nline != before) {
        if (at_end) st.scroll = st.nline > st.rows ? st.nline - st.rows : 0;
        gui_request_redraw();
    }
}

void logs_app_init(void) {
    win = gui_add_window("Logs", 700, 460, 0xC9A0FF, ICON_SYSINFO);
    win->draw   = logs_draw;
    win->click  = logs_click;
    win->scroll = logs_scroll;
    win->key    = logs_key;
    win->tick   = logs_tick;
    win->st     = &st;
    load(TAB_BOOT);
}
