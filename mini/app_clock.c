/* ===========================================================================
 *  BoltOS Mini  -  mini/app_clock.c
 *
 *  The time, large, and a stopwatch under it.
 * ===========================================================================*/
#include <stdint.h>
#include "ui.h"
#include "stdio.h"

static uint64_t sw_start, sw_acc;
static int sw_running;

static uint64_t sw_ms(void) { return sw_acc + (sw_running ? now_ms() - sw_start : 0); }

static void c_key(const input_ev_t *e) {
    if ((e->key == KEY_CHAR && e->ch == ' ') || e->key == KEY_ENTER) {
        if (sw_running) { sw_acc += now_ms() - sw_start; sw_running = 0; }
        else { sw_start = now_ms(); sw_running = 1; }
    } else if (e->key == KEY_CHAR && (e->ch == 'r' || e->ch == 'R')) {
        sw_running = 0; sw_acc = 0;
    }
}

static void c_draw(int x, int y, int w, int h) {
    char hm[16], date[96], b[48];
    clock_text(hm, sizeof hm, date, sizeof date);
    int y0, mo, d, hh, mi, s, wd;
    clock_local(&y0, &mo, &d, &hh, &mi, &s, &wd);
    int cx = x + w / 2;
    if (clock_known()) {
        snprintf(b, sizeof b, "%d:%02d", hh, mi);
        int tw = font_width(FONT_DISPLAY, S(120), b, -1);
        int base = y + h * 38 / 100 + font_capheight(FONT_DISPLAY, S(120)) / 2;
        font_draw(FONT_DISPLAY, S(120), cx - tw / 2, base, b, -1, th.text, 255);
        snprintf(b, sizeof b, ":%02d", s);
        font_draw(FONT_DISPLAY, S(36), cx + tw / 2 + S(8), base, b, -1, th.faint, 255);
        text_c(cx, base + S(40), date, FONT_MEDIUM, S(16), th.sub);
    } else {
        text_c(cx, y + h * 30 / 100, "The clock is not set", FONT_SEMIBOLD, S(26), th.text);
        text_c(cx, y + h * 30 / 100 + S(40), "Open http://10.77.0.1 on your Mac and press \"Set it from this Mac\",", FONT_REGULAR, S(14), th.sub);
        text_c(cx, y + h * 30 / 100 + S(62), "or type  time HH:MM  in the Terminal.", FONT_REGULAR, S(14), th.sub);
    }
    /* stopwatch */
    uint64_t ms = sw_ms();
    snprintf(b, sizeof b, "%02lu:%02lu.%01lu", (unsigned long)(ms / 60000), (unsigned long)(ms / 1000 % 60),
             (unsigned long)(ms / 100 % 10));
    int sy = y + h - S(110);
    gfx_fill(x + S(60), sy - S(34), w - S(120), 1, th.faint);
    text_l(x + S(60), sy, "Stopwatch", FONT_SEMIBOLD, S(13), th.sub);
    font_draw(FONT_MONO, S(30), x + S(60), sy + S(46), b, -1, sw_running ? th.accent : th.text, 255);
    text_r(x + w - S(60), sy + S(36), sw_running ? "Space stops" : (ms ? "Space goes on " TXT_BULLET " R resets" : "Space starts"),
           FONT_REGULAR, S(12), th.faint);
}

static int c_tick(void) {
    static uint64_t last;
    uint64_t step = sw_running ? 100 : 1000;
    if (now_ms() / step != last) { last = now_ms() / step; return 1; }
    return 0;
}

const app_t app_clock = { "Clock", 0xD9534F, G_CLOCK, 0, c_key, 0, c_draw, c_tick };
