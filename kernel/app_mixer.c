/* ===========================================================================
 *  BoltOS  -  kernel/app_mixer.c
 *  The Mixer window: one fader per channel, a mute button under each, and a
 *  meter beside each that shows what is actually coming out.
 *
 *  Vertical faders rather than the horizontal sliders everywhere else in this
 *  desktop, because a mixer with five channels side by side is the one place
 *  the shape earns itself: you can see at a glance that Media is up and System
 *  is down, which is the whole question you open a mixer to answer.
 *
 *  Every control is kernel/mixer.c, which is also what `mixer` drives, so the
 *  window and the prompt cannot disagree. Nothing is cached here.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "mixer.h"
#include "audio.h"
#include "keyboard.h"
#include "string.h"
#include "stdio.h"

#define PAD      18
#define HEAD_H   58
#define FADER_W  16
#define METER_W  7
#define MUTE_H   22
#define FOOT_H   46

static window_t *win;
static int grabbed = -1;       /* the fader the mouse is dragging, -1 for none */
static int sel = MIX_MASTER;   /* what the wheel and the arrow keys move       */

/* Where channel `c`'s column starts, and how wide it is. Computed from the
 * client rect every frame so resizing the window spreads the faders instead of
 * clipping them. */
static void column(int c, int cx, int cw, int *x, int *w) {
    int n = mixer_count();
    int avail = cw - 2 * PAD;
    int each = avail / n;
    *x = cx + PAD + c * each;
    *w = each;
}

static void fader_rect(int c, int cx, int cy, int cw, int ch, int *x, int *y, int *w, int *h) {
    int colx, colw;
    column(c, cx, cw, &colx, &colw);
    *w = FADER_W;
    *x = colx + colw / 2 - (FADER_W + 6 + METER_W) / 2;
    *y = cy + HEAD_H;
    *h = ch - HEAD_H - MUTE_H - FOOT_H;
    if (*h < 40) *h = 40;
}

/* ---------------------------------------------------------------- drawing -- */

static void draw_meter(int x, int y, int h, int peak, uint32_t col) {
    g_round(x, y, METER_W, h, 3, 0x1A1A24, 255);
    int lit = h * peak / 100;
    if (lit < 1 && peak > 0) lit = 1;
    /* Filled from the bottom, and red over the top fifth -- a meter that is
     * one flat colour tells you a level, not whether you are about to clip. */
    for (int i = 0; i < lit; i++) {
        int yy = y + h - 1 - i;
        int frac = i * 100 / (h > 0 ? h : 1);
        uint32_t c = frac > 85 ? COL_BAD : (frac > 65 ? COL_WARN : col);
        g_fill(x + 1, yy, METER_W - 2, 1, c);
    }
}

static void mixer_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    g_text(cx + PAD, cy + 14, "Volume mixer", COL_TEXT, 2);
    char sub[72];
    snprintf(sub, sizeof sub, "%s%s", audio_name(),
             audio_pcm_ok() ? "" : "  (tones only -- no PCM codec)");
    g_text(cx + PAD, cy + 36, sub, COL_TEXT_DIM, 1);

    for (int c = 0; c < mixer_count(); c++) {
        int colx, colw;
        column(c, cx, cw, &colx, &colw);
        int fx, fy, fw, fh;
        fader_rect(c, cx, cy, cw, ch, &fx, &fy, &fw, &fh);

        int muted = mixer_muted(c);
        int vol   = mixer_volume(c);
        int eff   = mixer_effective(c);
        uint32_t col = muted ? COL_TEXT_DIM
                     : (c == MIX_MASTER ? COL_ACCENT
                     : (mixer_is_input(c) ? COL_WARN : COL_ACCENT_DIM));

        /* Two different things worth seeing: which fader the keyboard moves
         * (a ring) and which channel currently owns the codec (a dot over its
         * label). They are usually not the same channel. */
        if (c == sel)
            g_round(colx + 2, fy - 6, colw - 4, fh + MUTE_H + 14, 8, 0x22222E, 255);
        if (c == mixer_active() && !mixer_is_input(c))
            g_round(colx + colw / 2 - 2, fy - 12, 4, 4, 2, COL_GOOD, 255);

        /* track, then the filled part, then the cap */
        g_round(fx, fy, fw, fh, FADER_W / 2, 0x2C2C38, 255);
        int fill = fh * vol / 100;
        if (fill > 0)
            g_round(fx, fy + fh - fill, fw, fill, FADER_W / 2, col, 255);
        int capy = fy + fh - fill - 5;
        if (capy < fy - 5) capy = fy - 5;
        g_round(fx - 4, capy, fw + 8, 11, 5, muted ? 0x6A6A78 : COL_TEXT, 255);

        draw_meter(fx + fw + 6, fy, fh, mixer_peak(c), COL_GOOD);

        /* mute button */
        int my = fy + fh + 8;
        g_round(colx + 6, my, colw - 12, MUTE_H - 4, 6,
                muted ? COL_BAD : COL_PANEL_3, 255);
        const char *ml = muted ? "Muted" : "Mute";
        g_text(colx + colw / 2 - g_text_width(ml, 1) / 2, my + 5, ml,
               muted ? 0xFFFFFF : COL_TEXT_DIM, 1);

        /* label + the number, under everything */
        int ly = my + MUTE_H + 4;
        const char *lab = mixer_label(c);
        g_text(colx + colw / 2 - g_text_width(lab, 1) / 2, ly, lab,
               c == MIX_MASTER ? COL_TEXT : COL_TEXT_DIM, 1);
        char pct[16];
        if (mixer_is_input(c) || c == MIX_MASTER) snprintf(pct, sizeof pct, "%d%%", vol);
        else snprintf(pct, sizeof pct, "%d%% (%d)", vol, eff);   /* what the codec gets */
        g_text(colx + colw / 2 - g_text_width(pct, 1) / 2, ly + 14, pct,
               muted ? COL_BAD : COL_TEXT_DIM, 1);
    }

    g_text(cx + PAD, cy + ch - 28,
           "Drag a fader or click the track. Arrows move the selected one, M mutes,",
           COL_TEXT_DIM, 1);
    g_text(cx + PAD, cy + ch - 14,
           "R restores the defaults. Levels are kept in /etc/mixer.conf.",
           COL_TEXT_DIM, 1);
}

/* ----------------------------------------------------------------- input --- */

/* Set a channel from a y inside its fader. Top of the track is 100. */
static void set_from_y(int c, int ly, int cy, int ch, int cw, int cx) {
    int fx, fy, fw, fh;
    fader_rect(c, cx, cy, cw, ch, &fx, &fy, &fw, &fh);
    int v = (fy + fh - ly) * 100 / (fh > 0 ? fh : 1);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    mixer_set_volume(c, v);
    audio_gain_changed();
}

/* The client rect is handed to draw() but not to click()/drag(), so the last
 * one drawn is remembered. Every app here that needs geometry in its input
 * handlers does this; the alternative is recomputing a layout in two places
 * and watching them drift apart. */
static int last_cx, last_cy, last_cw, last_ch;

static void mixer_click(window_t *w, int lx, int ly) {
    (void)w;
    grabbed = -1;
    int cx = last_cx, cy = last_cy, cw = last_cw, ch = last_ch;
    for (int c = 0; c < mixer_count(); c++) {
        int colx, colw;
        column(c, cx, cw, &colx, &colw);
        if (lx < colx || lx >= colx + colw) continue;
        int fx, fy, fw, fh;
        fader_rect(c, cx, cy, cw, ch, &fx, &fy, &fw, &fh);
        int my = fy + fh + 8;
        sel = c;
        if (ly >= my && ly < my + MUTE_H - 4) {
            mixer_toggle_mute(c);
            audio_gain_changed();
        } else if (ly >= fy - 8 && ly < fy + fh + 8) {
            grabbed = c;
            set_from_y(c, ly, cy, ch, cw, cx);
        }
        break;
    }
    mixer_save();
    gui_request_redraw();
}

static void mixer_drag(window_t *w, int lx, int ly) {
    (void)w; (void)lx;
    if (grabbed < 0) return;
    set_from_y(grabbed, ly, last_cy, last_ch, last_cw, last_cx);
    gui_request_redraw();
}

/* The wheel moves the selected column in fives -- the step the tray slider and
 * the Media player both use. The scroll callback is given a delta and nothing
 * else, so it cannot know which column the pointer is over; clicking a column
 * selects it, which is the same gesture you were making anyway. */
static void mixer_scroll(window_t *w, int delta) {
    (void)w;
    mixer_set_volume(sel, mixer_volume(sel) + delta * 5);
    audio_gain_changed();
    mixer_save();
    gui_request_redraw();
}

static void mixer_key(window_t *w, char c) {
    (void)w;
    switch ((unsigned char)c) {
    case KEY_LEFT:  if (sel > 0) sel--; break;
    case KEY_RIGHT: if (sel < mixer_count() - 1) sel++; break;
    case KEY_UP:   mixer_set_volume(sel, mixer_volume(sel) + 5); break;
    case KEY_DOWN: mixer_set_volume(sel, mixer_volume(sel) - 5); break;
    case ' ':
    case 'm': case 'M': mixer_toggle_mute(sel); break;
    case 'r': case 'R': mixer_reset(); break;
    default: return;
    }
    audio_gain_changed();
    mixer_save();
    gui_request_redraw();
}

/* The meters only mean anything if they are repainted while sound is playing,
 * and the window tick is the cheapest place to ask for that. */
static void mixer_tick(window_t *w) {
    (void)w;
    if (mixer_peak(MIX_MASTER) > 0) gui_request_redraw();
}

/* draw() is the only callback given the client rect, so it stashes it. */
static void mixer_draw_wrap(window_t *w, int cx, int cy, int cw, int ch) {
    last_cx = cx; last_cy = cy; last_cw = cw; last_ch = ch;
    mixer_draw(w, cx, cy, cw, ch);
}

void mixer_app_init(void) {
    win = gui_add_window("Mixer", 460, 400, 0x9B6CF2, ICON_MIXER);
    if (!win) return;
    win->draw   = mixer_draw_wrap;
    win->click  = mixer_click;
    win->drag   = mixer_drag;
    win->scroll = mixer_scroll;
    win->key    = mixer_key;
    win->tick   = mixer_tick;
    win->min_w  = 400;
    win->min_h  = 320;
    win->x = 300; win->y = 110;
}

/* Opened from the speaker in quick settings, which is where somebody goes when
 * one thing is too loud rather than everything. */
void mixer_app_open(void) {
    if (!win) return;
    gui_open(win);
    gui_request_redraw();
}
