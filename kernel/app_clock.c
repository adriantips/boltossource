/* ===========================================================================
 *  BoltOS  -  kernel/app_clock.c
 *  Clock: an analog face, the time in big numerals under it, and the date.
 *  Time comes from the CMOS RTC.
 *
 *  This used to place its hands from a 60-entry integer sine table because
 *  the kernel had no floating point. It has had SSE2 for a long time now, and
 *  a hand drawn as a chain of square dots looked exactly like one.
 * ===========================================================================*/
#include <stdint.h>
#include <stdio.h>
#include "gui.h"
#include "gfx.h"
#include "font.h"
#include "hw.h"
#include "math.h"

static int last_sec = -1;

static void clk_tick(window_t *w) {
    struct rtc_time t; rtc_now(&t);
    if (t.sec == last_sec) return;
    last_sec = t.sec;
    gui_redraw_window(w);
}

static const char *MONTH[13] = { "", "January", "February", "March", "April", "May", "June",
                                 "July", "August", "September", "October", "November", "December" };
static const char *WDAY[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

static int weekday(int y, int m, int d) {          /* Zeller, 0 = Sunday */
    if (m < 3) { m += 12; y--; }
    int K = y % 100, J = y / 100;
    int h = (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    return (h + 6) % 7;
}

/* a hand: from a little behind the centre out to len, at angle a (0 = 12) */
static void hand(float cx, float cy, float a, float len, float tail, float width, uint32_t c) {
    float s = (float)sin((double)a), k = (float)cos((double)a);
    gfx_line(cx - s * tail, cy + k * tail, cx + s * len, cy - k * len, width, c, 255);
}

static void clk_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    struct rtc_time t; rtc_now(&t);
    int dark = THEME_DARK;

    /* the face takes what is left above the readout */
    int below = 92;
    int d = (cw < ch - below ? cw : ch - below) - 36;
    if (d < 100) d = 100;
    float r = d / 2.0f;
    float fx = cx + cw / 2.0f, fy = cy + 18 + r;

    gfx_shadow((int)(fx - r), (int)(fy - r), d, d, d / 2, 18, 6, 0, dark ? 120 : 50);
    gfx_circle(fx, fy, r, dark ? 0x1C1C1E : 0xFFFFFF, 255);
    gfx_ring(fx, fy, r, 1.0f, dark ? 0xFFFFFF : 0x000000, dark ? 26 : 22);

    for (int i = 0; i < 60; i++) {
        float a = (float)(i * M_PI / 30.0), s = (float)sin((double)a), k = (float)cos((double)a);
        int major = i % 5 == 0;
        float r0 = r - (major ? 11.0f : 7.0f), r1 = r - 4.0f;
        gfx_line(fx + s * r0, fy - k * r0, fx + s * r1, fy - k * r1, major ? 2.2f : 1.0f,
                 COL_TEXT, major ? 230 : 90);
    }
    int npx = d > 200 ? 16 : 13;
    for (int n = 1; n <= 12; n++) {
        float a = (float)(n * M_PI / 6.0);
        float rr = r - 26.0f - (d > 200 ? 4.0f : 0.0f);
        char num[4]; snprintf(num, sizeof num, "%d", n);
        text_c((int)(fx + (float)sin((double)a) * rr), (int)(fy - (float)cos((double)a) * rr),
               num, FONT_MEDIUM, npx, COL_TEXT);
    }

    float sec = (float)(t.sec % 60), mn = (float)(t.min % 60) + sec / 60.0f;
    float hr = (float)(t.hour % 12) + mn / 60.0f;
    hand(fx, fy, (float)(hr * M_PI / 6.0), r * 0.50f, 0.0f, 5.0f, COL_TEXT);
    hand(fx, fy, (float)(mn * M_PI / 30.0), r * 0.74f, 0.0f, 3.5f, COL_TEXT);
    gfx_circle(fx, fy, 5.0f, COL_TEXT, 255);
    hand(fx, fy, (float)(sec * M_PI / 30.0), r * 0.82f, r * 0.16f, 1.5f, 0xFF9F0A);
    gfx_circle(fx, fy, 3.0f, 0xFF9F0A, 255);
    gfx_circle(fx, fy, 1.2f, dark ? 0x1C1C1E : 0xFFFFFF, 255);

    /* the readout */
    char dig[12], date[48];
    snprintf(dig, sizeof dig, "%02d:%02d:%02d", t.hour % 24, t.min % 60, t.sec % 60);
    int mo = t.mon >= 1 && t.mon <= 12 ? t.mon : 1;
    int yr = t.year ? t.year : 2026;
    snprintf(date, sizeof date, "%s, %d %s", WDAY[weekday(yr, mo, t.day ? t.day : 1)], t.day, MONTH[mo]);
    int ty = (int)(fy + r) + 22;
    int big = cw >= 260 ? 40 : 32;
    int tw = font_width(FONT_DISPLAY, big, dig, -1);
    font_draw(FONT_DISPLAY, big, cx + cw / 2 - tw / 2, ty + font_ascent(FONT_DISPLAY, big) - 4,
              dig, -1, COL_TEXT, 255);
    text_c(cx + cw / 2, ty + big + 12, date, FONT_REGULAR, 13, COL_TEXT_DIM);
}

void clock_app_init(void) {
    window_t *win = gui_add_window("Clock", 300, 400, 0x4FC3F7, ICON_CLOCK);
    if (!win) return;
    win->draw = clk_draw;
    win->tick = clk_tick;
    win->lazy_tick = 1;          /* clk_tick asks once a second */
    win->min_w = 240; win->min_h = 320;
    win->x = 660; win->y = 120;
}
