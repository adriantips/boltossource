/* ===========================================================================
 *  BoltOS  -  kernel/app_software.c
 *  Software: the package manager, for somebody who has never opened a terminal.
 *
 *  Installing software is the one administrative task a person using a machine
 *  cannot avoid, and until now the only way to do it was `pkg install` at a
 *  prompt. That is a reasonable thing to ask of a developer and an unreasonable
 *  thing to ask of anybody else.
 *
 *  Everything here goes through include/pkg.h -- the same index parser and the
 *  same install path the command uses. A GUI with its own copy of the format
 *  would be a second reader to keep in step, and the two would drift.
 *
 *  Installing blocks: it reads a repository that may be across a network, and
 *  the compositor is single-threaded. So a click does not install anything. It
 *  records what to do, asks for a repaint, and the work happens on the next
 *  tick -- by which time the window is already saying which package it is
 *  working on. A frozen desktop with no explanation is how you get a machine
 *  people distrust.
 * ===========================================================================*/
#include <stdint.h>
#include <stdio.h>
#include "gui.h"
#include "gfx.h"
#include "font.h"
#include "pkg.h"
#include "notify.h"
#include "string.h"

#define ROW_H     54
#define HEAD_H    76
#define BTN_W     78
#define BTN_H     26

enum { TAB_AVAILABLE = 0, TAB_INSTALLED };
enum { JOB_NONE = 0, JOB_INSTALL, JOB_REMOVE, JOB_REFRESH };

typedef struct {
    pkg_entry list[PKG_LIST_MAX];
    int       n;
    int       tab;
    int       scroll;
    int       sel;
    int       no_index;              /* the repository has no index to read   */
    int       job;                   /* queued work, run on the next tick     */
    char      job_name[40];
    char      status[96];
    int       rows, cw, ch;          /* client geometry, from the last draw   */
} soft_t;

static soft_t st;
static window_t *win;

static void reload(void) {
    st.n = (st.tab == TAB_AVAILABLE) ? pkg_catalogue(st.list, PKG_LIST_MAX)
                                     : pkg_installed_list(st.list, PKG_LIST_MAX);
    st.no_index = (st.n < 0);
    if (st.n < 0) st.n = 0;
    if (st.sel >= st.n) st.sel = st.n - 1;
    if (st.scroll > st.n - 1) st.scroll = st.n > 0 ? st.n - 1 : 0;
    if (st.scroll < 0) st.scroll = 0;
}

/* ------------------------------------------------------------- drawing ---- */
static void tab_rect(int cx, int cy, int which, int *x, int *y, int *w, int *h) {
    *w = 104; *h = 26;
    *x = cx + 14 + which * (*w + 6);
    *y = cy + 12;
}

static void refresh_rect(int cx, int cy, int cw, int *x, int *y, int *w, int *h) {
    *w = 86; *h = 26;
    *x = cx + cw - *w - 14; *y = cy + 12;
}

static void row_btn_rect(int cx, int cw, int ry, int *x, int *y, int *w, int *h) {
    *w = BTN_W; *h = BTN_H;
    *x = cx + cw - BTN_W - 16;
    *y = ry + (ROW_H - BTN_H) / 2;
}

static void soft_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    /* The click handler works in client-local coordinates and needs the same
     * rectangles this draws. Recording the size here keeps it from having to
     * know the title bar's height, which is the compositor's business. */
    st.cw = cw; st.ch = ch;

    /* tabs */
    for (int t = 0; t < 2; t++) {
        int x, y, bw, bh; tab_rect(cx, cy, t, &x, &y, &bw, &bh);
        int on = (st.tab == t);
        g_round(x, y, bw, bh, 7, on ? COL_ACCENT : COL_PANEL_2, 255);
        const char *label = t == TAB_AVAILABLE ? "Available" : "Installed";
        text_c(x + bw / 2, y + bh / 2, label, on ? FONT_SEMIBOLD : FONT_MEDIUM, 12, on ? 0xFFFFFF : COL_TEXT_DIM);
    }
    { int x, y, bw, bh; refresh_rect(cx, cy, cw, &x, &y, &bw, &bh);
      g_round(x, y, bw, bh, 7, COL_PANEL_2, 255);
      text_c(x + bw / 2, y + bh / 2, "Refresh", FONT_MEDIUM, 12, COL_TEXT); }

    /* the repository this is all coming from: worth seeing, because it is the
     * difference between installing off the disk and off the network. */
    char sub[140];
    snprintf(sub, sizeof sub, "%s", pkg_repo_in_use());
    g_text(cx + 14, cy + 48, sub, COL_TEXT_DIM, 1);
    g_hline(cx + 12, cy + HEAD_H - 8, cw - 24, COL_SEP);

    if (st.status[0])
        g_text(cx + 14, cy + ch - 20, st.status,
               st.job ? COL_WARN : COL_TEXT_DIM, 1);

    int top = cy + HEAD_H, bottom = cy + ch - 26;
    st.rows = (bottom - top) / ROW_H;
    if (st.rows < 1) st.rows = 1;

    if (st.no_index) {
        g_text(cx + 16, top + 12, "No package index yet.", COL_TEXT, 1);
        g_text(cx + 16, top + 32, "Press Refresh to read the repository.", COL_TEXT_DIM, 1);
        return;
    }
    if (!st.n) {
        g_text(cx + 16, top + 12,
               st.tab == TAB_AVAILABLE ? "The repository is empty."
                                       : "Nothing is installed yet.", COL_TEXT_DIM, 1);
        return;
    }

    for (int r = 0; r < st.rows; r++) {
        int i = st.scroll + r;
        if (i >= st.n) break;
        pkg_entry *e = &st.list[i];
        int ry = top + r * ROW_H;
        if (i == st.sel) gfx_rrect(cx + 8, ry + 2, cw - 16, ROW_H - 4, 9, THEME_DARK ? 0xFFFFFF : 0x000000,
                                   THEME_DARK ? 12 : 8);

        int nx = cx + 18;
        nx += font_draw(FONT_SEMIBOLD, 13, nx, ry + 20, e->name, -1, COL_TEXT, 255) + 8;
        nx += font_draw(FONT_REGULAR, 12, nx, ry + 20, e->version, -1, COL_TEXT_DIM, 255) + 8;
        int kw = font_width(FONT_MEDIUM, 11, e->kind, -1) + 12;
        gfx_rrect(nx, ry + 8, kw, 17, 8, COL_ACCENT, THEME_DARK ? 40 : 30);
        text_c(nx + kw / 2, ry + 16, e->kind, FONT_MEDIUM, 11, COL_ACCENT);

        /* the summary, trimmed to what is left after the button */
        text_fit_l(cx + 18, ry + 37, e->summary, cw - BTN_W - 44 - 18, FONT_REGULAR, 12, COL_TEXT_DIM);

        int bx, by, bw, bh; row_btn_rect(cx, cw, ry, &bx, &by, &bw, &bh);
        int busy = st.job && strcmp(st.job_name, e->name) == 0;
        const char *label = busy ? "Working" TXT_ELLIPSIS : e->installed ? "Remove" : "Install";
        /* Install is the thing you came for, so it is the filled one */
        if (!busy && !e->installed) {
            gfx_rrect(bx, by, bw, bh, bh / 2, COL_ACCENT, 255);
            text_c(bx + bw / 2, by + bh / 2, label, FONT_SEMIBOLD, 12, 0xFFFFFF);
        } else {
            gfx_rrect(bx, by, bw, bh, bh / 2, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 22 : 14);
            text_c(bx + bw / 2, by + bh / 2, label, FONT_SEMIBOLD, 12, busy ? COL_WARN : COL_BAD);
        }
    }

    if (st.n > st.rows) {
        char sc[32];
        snprintf(sc, sizeof sc, "%d-%d of %d", st.scroll + 1,
                 st.scroll + st.rows > st.n ? st.n : st.scroll + st.rows, st.n);
        g_text(cx + cw - g_text_width(sc, 1) - 16, cy + ch - 20, sc, COL_TEXT_DIM, 1);
    }
}

/* --------------------------------------------------------------- input ---- */
static void queue(int job, const char *name, const char *saying) {
    st.job = job;
    snprintf(st.job_name, sizeof st.job_name, "%s", name ? name : "");
    snprintf(st.status, sizeof st.status, "%s", saying);
    /* Not just a redraw request: the desktop loop checks the tick timer before
     * it composites, so the job could run and finish before the frame that
     * says it started was ever drawn. Paint it now, then let the tick block. */
    gui_pump();
}

static void soft_click(window_t *w, int lx, int ly) {
    (void)w;
    if (st.job) return;                       /* one thing at a time */
    int cx = 0, cy = 0, cw = st.cw;           /* the draw's own coordinates */
    int x = lx, y = ly;
    if (!cw) return;                          /* not drawn yet */

    for (int t = 0; t < 2; t++) {
        int bx, by, bw, bh; tab_rect(cx, cy, t, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            st.tab = t; st.scroll = 0; st.sel = 0; reload();
            gui_request_redraw();
            return;
        }
    }
    { int bx, by, bw, bh; refresh_rect(cx, cy, cw, &bx, &by, &bw, &bh);
      if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
          queue(JOB_REFRESH, 0, "Reading the repository...");
          return;
      } }

    int top = cy + HEAD_H;
    for (int r = 0; r < st.rows; r++) {
        int i = st.scroll + r;
        if (i >= st.n) break;
        int ry = top + r * ROW_H;
        if (y < ry || y >= ry + ROW_H) continue;
        st.sel = i;
        int bx, by, bw, bh; row_btn_rect(cx, cw, ry, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            char saying[96];
            if (st.list[i].installed) {
                snprintf(saying, sizeof saying, "Removing %s...", st.list[i].name);
                queue(JOB_REMOVE, st.list[i].name, saying);
            } else {
                snprintf(saying, sizeof saying, "Installing %s...", st.list[i].name);
                queue(JOB_INSTALL, st.list[i].name, saying);
            }
        }
        gui_request_redraw();
        return;
    }
}

static void soft_scroll(window_t *w, int delta) {
    (void)w;
    st.scroll -= delta;
    int last = st.n - st.rows;
    if (last < 0) last = 0;
    if (st.scroll > last) st.scroll = last;
    if (st.scroll < 0) st.scroll = 0;
    gui_request_redraw();
}

/* The queued job runs here, one tick after the click, so the window has
 * already painted "Installing x..." before anything blocks. */
static void soft_tick(window_t *w) {
    (void)w;
    if (!st.job) return;
    int job = st.job, rc = 0;
    char name[40];
    snprintf(name, sizeof name, "%s", st.job_name);

    if      (job == JOB_INSTALL) rc = pkg_action_install(name);
    else if (job == JOB_REMOVE)  rc = pkg_action_remove(name);
    else                         rc = pkg_action_update();

    st.job = 0; st.job_name[0] = 0;
    reload();

    char msg[NOTE_TEXT_MAX];
    if (job == JOB_REFRESH) {
        snprintf(msg, sizeof msg, rc ? "Could not read the repository"
                                     : "Package list updated");
        snprintf(st.status, sizeof st.status, "%s", msg);
        gui_notify(rc ? NOTE_WARN : NOTE_INFO, "Software", msg);
    } else {
        const char *verb = job == JOB_INSTALL ? "install" : "remove";
        if (rc) snprintf(msg, sizeof msg, "Could not %s %s -- see the Terminal for why", verb, name);
        else    snprintf(msg, sizeof msg, "%s %s", job == JOB_INSTALL ? "Installed" : "Removed", name);
        snprintf(st.status, sizeof st.status, "%s", msg);
        gui_notify(rc ? NOTE_ERROR : NOTE_SUCCESS, "Software", msg);
    }
    gui_request_redraw();
}

void software_app_init(void) {
    win = gui_add_window("Software", 620, 440, 0x6FD08C, ICON_SOFTWARE);
    win->draw   = soft_draw;
    win->click  = soft_click;
    win->scroll = soft_scroll;
    win->tick   = soft_tick;
    win->st     = &st;
    st.tab = TAB_AVAILABLE;
    snprintf(st.status, sizeof st.status, "%s", "");
    reload();
}
