/* ===========================================================================
 *  BoltOS  -  kernel/app_stopwatch.c
 *  Stopwatch + countdown Timer (two tabs). Timing is derived from the PIT tick
 *  counter (1 kHz), so elapsed/remaining are exact in milliseconds. When the
 *  timer reaches zero it rings the PC speaker for ~1.2s. The window tick drives
 *  the live display and the ring timeout.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "gfx.h"
#include "font.h"
#include "pit.h"
#include "pcspk.h"
#include "string.h"
#include "commands.h"     /* sh_utoa */

enum { TAB_SW = 0, TAB_TIMER };

typedef struct {
    int      tab;
    /* stopwatch */
    int      sw_run;
    uint64_t sw_start;        /* pit tick when last started */
    uint64_t sw_accum;        /* accumulated ms while stopped */
    /* timer */
    int      tm_run;
    uint64_t tm_set;          /* configured duration, ms */
    uint64_t tm_end;          /* pit tick the countdown ends */
    uint64_t tm_remain;       /* frozen remaining while paused, ms */
    int      ringing;
    uint64_t ring_until;
} sw_t;
static sw_t S;

/* hot rects (client-local) */
enum { B_TAB_SW = 1, B_TAB_TM, B_SW_TOGGLE, B_SW_RESET,
       B_TM_TOGGLE, B_TM_RESET, B_TM_M1, B_TM_P1, B_TM_M10, B_TM_P10 };
typedef struct { int x, y, w, h, id; } shot_t;
static shot_t shots[16];
static int    nshot;
static int    org_x, org_y;

static void hot(int x, int y, int w, int h, int id) {
    if (nshot >= 16) return;
    shots[nshot].x = x - org_x; shots[nshot].y = y - org_y;
    shots[nshot].w = w; shots[nshot].h = h; shots[nshot].id = id; nshot++;
}

/* ms -> "MM:SS.T" (T = tenths) */
static void fmt_time(uint64_t ms, char *out, int cap) {
    uint64_t totsec = ms / 1000;
    int mn = (int)(totsec / 60), se = (int)(totsec % 60), tenth = (int)((ms % 1000) / 100);
    char t[8];
    out[0] = 0;
    if (mn < 10) kstrlcat(out, "0", cap);
    sh_utoa((uint64_t)mn, t); kstrlcat(out, t, cap); kstrlcat(out, ":", cap);
    if (se < 10) kstrlcat(out, "0", cap);
    sh_utoa((uint64_t)se, t); kstrlcat(out, t, cap); kstrlcat(out, ".", cap);
    sh_utoa((uint64_t)tenth, t); kstrlcat(out, t, cap);
}

static uint64_t sw_elapsed(void) {
    return S.sw_accum + (S.sw_run ? (pit_ticks() - S.sw_start) : 0);
}
static uint64_t tm_remaining(void) {
    if (!S.tm_run) return S.tm_remain;
    uint64_t now = pit_ticks();
    return now >= S.tm_end ? 0 : S.tm_end - now;
}

static void sw_tick(window_t *w) {
    (void)w;
    if (S.tm_run && pit_ticks() >= S.tm_end) {       /* countdown finished */
        S.tm_run = 0; S.tm_remain = 0;
        S.ringing = 1; S.ring_until = pit_ticks() + 1200;
        pcspk_tone(880);
    }
    if (S.ringing && pit_ticks() >= S.ring_until) { pcspk_off(); S.ringing = 0; }
}

/* ---- input -------------------------------------------------------------- */
static void tm_adjust(int delta_ms) {
    if (S.tm_run) return;
    int64_t v = (int64_t)S.tm_remain + delta_ms;
    if (v < 0) v = 0;
    if (v > 99 * 60000) v = 99 * 60000;
    S.tm_remain = (uint64_t)v; S.tm_set = (uint64_t)v;
}

static void sw_click(window_t *w, int lx, int ly) {
    (void)w;
    for (int i = 0; i < nshot; i++) {
        shot_t *h = &shots[i];
        if (lx < h->x || lx >= h->x + h->w || ly < h->y || ly >= h->y + h->h) continue;
        switch (h->id) {
        case B_TAB_SW:  S.tab = TAB_SW; break;
        case B_TAB_TM:  S.tab = TAB_TIMER; break;
        case B_SW_TOGGLE:
            if (S.sw_run) { S.sw_accum += pit_ticks() - S.sw_start; S.sw_run = 0; }
            else { S.sw_start = pit_ticks(); S.sw_run = 1; }
            break;
        case B_SW_RESET: S.sw_run = 0; S.sw_accum = 0; break;
        case B_TM_TOGGLE:
            if (S.tm_run) { S.tm_remain = tm_remaining(); S.tm_run = 0; }
            else if (S.tm_remain > 0) { S.tm_end = pit_ticks() + S.tm_remain; S.tm_run = 1;
                                        pcspk_off(); S.ringing = 0; }
            break;
        case B_TM_RESET: S.tm_run = 0; S.tm_remain = S.tm_set; pcspk_off(); S.ringing = 0; break;
        case B_TM_M1:  tm_adjust(-60000); break;
        case B_TM_P1:  tm_adjust(+60000); break;
        case B_TM_M10: tm_adjust(-10000); break;
        case B_TM_P10: tm_adjust(+10000); break;
        }
        return;
    }
}

/* ---- draw --------------------------------------------------------------- */
/* bg 0 means the quiet grey every secondary button here is */
static void btn(int x, int y, int w, int h, const char *label, int id, uint32_t bg, uint32_t fg) {
    if (bg) gfx_rrect(x, y, w, h, h / 2, bg, 255);
    else    gfx_rrect(x, y, w, h, h / 2, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 22 : 14);
    text_c(x + w / 2, y + h / 2, label, FONT_MEDIUM, 13, fg);
    hot(x, y, w, h, id);
}

static void sw_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w; (void)ch;
    gfx_fill(cx, cy, cw, ch, COL_PANEL);
    nshot = 0; org_x = cx; org_y = cy;

    /* the two modes, as a segmented control */
    int sx = cx + 16, sw = cw - 32, sy = cy + 14, half = (sw - 4) / 2;
    gfx_rrect(sx, sy, sw, 32, 9, 0x000000, THEME_DARK ? 60 : 14);
    for (int k = 0; k < 2; k++) {
        int on = (k == 0) == (S.tab == TAB_SW);
        int kx = sx + 2 + k * half;
        if (on) {
            gfx_shadow(kx, sy + 2, half, 28, 7, 4, 1, 0, 60);
            gfx_rrect(kx, sy + 2, half, 28, 7, THEME_DARK ? 0x5A5A62 : 0xFFFFFF, 255);
        }
        text_c(kx + half / 2, sy + 16, k ? "Timer" : "Stopwatch", on ? FONT_SEMIBOLD : FONT_REGULAR, 13, COL_TEXT);
        hot(kx, sy, half, 32, k ? B_TAB_TM : B_TAB_SW);
    }

    /* big readout, as large as the window allows */
    char buf[16];
    uint64_t val = (S.tab == TAB_SW) ? sw_elapsed() : tm_remaining();
    fmt_time(val, buf, sizeof(buf));
    int px = 64;
    while (px > 28 && font_width(FONT_DISPLAY, px, buf, -1) > cw - 32) px -= 4;
    int bw = font_width(FONT_DISPLAY, px, buf, -1);
    uint32_t tcol = (S.tab == TAB_TIMER && S.ringing) ? COL_BAD : COL_TEXT;
    int base = cy + 60 + font_capheight(FONT_DISPLAY, px) + (64 - px) / 2 + 12;
    font_draw(FONT_DISPLAY, px, cx + (cw - bw) / 2, base, buf, -1, tcol, 255);

    int by = cy + 152;
    int bh = 40;

    if (S.tab == TAB_SW) {
        int bw2 = (cw - 32 - 12) / 2;
        btn(cx + 16, by, bw2, bh, "Reset", B_SW_RESET, 0, COL_TEXT);
        btn(cx + 16 + bw2 + 12, by, bw2, bh, S.sw_run ? "Stop" : "Start", B_SW_TOGGLE,
            S.sw_run ? COL_BAD : COL_GOOD, 0xFFFFFF);
    } else {
        /* adjust row (dimmed while running) */
        int q = (cw - 32 - 3 * 8) / 4, ax = cx + 16;
        uint32_t fg = S.tm_run ? COL_TEXT_MUTE : COL_TEXT;
        btn(ax, by, q, 34, "-1m",  B_TM_M1,  0, fg); ax += q + 8;
        btn(ax, by, q, 34, "-10s", B_TM_M10, 0, fg); ax += q + 8;
        btn(ax, by, q, 34, "+10s", B_TM_P10, 0, fg); ax += q + 8;
        btn(ax, by, q, 34, "+1m",  B_TM_P1,  0, fg);
        int by2 = by + 34 + 10, bw2 = (cw - 32 - 12) / 2;
        btn(cx + 16, by2, bw2, bh, "Reset", B_TM_RESET, 0, COL_TEXT);
        btn(cx + 16 + bw2 + 12, by2, bw2, bh, S.tm_run ? "Pause" : "Start", B_TM_TOGGLE,
            S.tm_run ? COL_WARN : COL_GOOD, 0xFFFFFF);
    }
}

void stopwatch_app_init(void) {
    memset(&S, 0, sizeof(S));
    S.tm_set = S.tm_remain = 60000;        /* default 1 minute */
    window_t *win = gui_add_window("Stopwatch", 320, 300, 0x4FC3F7, ICON_STOPWATCH);
    if (!win) return;
    win->draw  = sw_draw;
    win->click = sw_click;
    win->tick  = sw_tick;
    win->min_w = 280; win->min_h = 300;
    win->x = 380; win->y = 130;
}
