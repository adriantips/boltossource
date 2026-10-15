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
#include "gfx.h"
#include "font.h"
#include "mixer.h"
#include "audio.h"
#include "keyboard.h"
#include "string.h"
#include "stdio.h"

#define PAD      18
#define HEAD_H   72
#define FADER_W  16
#define METER_W  7
#define MUTE_H   26
#define BELOW_H  (10 + MUTE_H + 44)   /* button, label, level -- under a fader */
#define FOOT_H   42

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
    *h = ch - HEAD_H - BELOW_H - FOOT_H;
    if (*h < 40) *h = 40;
}

/* ---------------------------------------------------------------- drawing -- */

static int imin_(int a, int b) { return a < b ? a : b; }

static void draw_meter(int x, int y, int h, int peak, uint32_t col) {
    gfx_rrect(x, y, METER_W, h, 3, THEME_DARK ? 0x000000 : 0x000000, THEME_DARK ? 70 : 18);
    int lit = h * peak / 100;
    if (lit < 1 && peak > 0) lit = 1;
    /* Filled from the bottom, and red over the top fifth -- a meter that is
     * one flat colour tells you a level, not whether you are about to clip. */
    for (int i = 0; i < lit; i++) {
        int yy = y + h - 1 - i;
        int frac = i * 100 / (h > 0 ? h : 1);
        uint32_t c = frac > 85 ? COL_BAD : (frac > 65 ? COL_WARN : col);
        gfx_fill(x + 1, yy, METER_W - 2, 1, c);
    }
}

static void mixer_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    gfx_fill(cx, cy, cw, ch, COL_PANEL);

    text_l(cx + PAD, cy + 28, "Volume mixer", FONT_BOLD, 20, COL_TEXT);
    char sub[72];
    snprintf(sub, sizeof sub, "%s%s", audio_name(),
             audio_pcm_ok() ? "" : "  " TXT_EMDASH " tones only, no PCM codec");
    text_l(cx + PAD, cy + 50, sub, FONT_REGULAR, 12, COL_TEXT_DIM);

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
            gfx_rrect(colx + 4, fy - 10, colw - 8, fh + BELOW_H + 6, 12, THEME_DARK ? 0xFFFFFF : 0x000000,
                      THEME_DARK ? 12 : 9);
        if (c == mixer_active() && !mixer_is_input(c))
            gfx_circle((float)(colx + colw / 2), (float)(fy - 4) - 0.5f, 2.5f, COL_GOOD, 255);

        /* track, then the filled part, then the cap */
        gfx_rrect(fx, fy, fw, fh, FADER_W / 2, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 22 : 16);
        int fill = fh * vol / 100;
        if (fill > 0)
            gfx_rrect(fx, fy + fh - fill, fw, fill, FADER_W / 2, col, 255);
        int capy = fy + fh - fill - 6;
        if (capy < fy - 6) capy = fy - 6;
        gfx_shadow(fx - 5, capy, fw + 10, 12, 6, 4, 1, 0, 70);
        gfx_rrect(fx - 5, capy, fw + 10, 12, 6, muted ? 0x8E8E93 : 0xFFFFFF, 255);

        draw_meter(fx + fw + 6, fy, fh, mixer_peak(c), COL_GOOD);

        /* The button under each fader. On the four output channels it is the
         * mute; on the input it is a monitor, because a microphone's meter
         * only means anything while the ring is actually open and holding it
         * open costs a running DMA engine. */
        int my = fy + fh + 10;
        int monitoring = mixer_is_input(c) && audio_capture_active();
        int lit = monitoring || (!mixer_is_input(c) && muted);
        uint32_t bg = mixer_is_input(c) ? COL_GOOD : COL_BAD;
        const char *ml = mixer_is_input(c) ? (monitoring ? "Listening" : "Listen")
                                           : (muted ? "Muted" : "Mute");
        int bw = colw - 16 < 76 ? colw - 16 : 76;
        if (lit) gfx_rrect(colx + colw / 2 - bw / 2, my, bw, MUTE_H, 8, bg, 255);
        else     gfx_rrect(colx + colw / 2 - bw / 2, my, bw, MUTE_H, 8, THEME_DARK ? 0xFFFFFF : 0x000000,
                           THEME_DARK ? 22 : 14);
        text_c(colx + colw / 2, my + MUTE_H / 2, ml, FONT_MEDIUM, 12, lit ? 0xFFFFFF : COL_TEXT);

        /* label + the number, under everything */
        int ly = my + MUTE_H + 16;
        const char *lab = mixer_label(c);
        text_c(colx + colw / 2, ly, lab, c == MIX_MASTER ? FONT_SEMIBOLD : FONT_MEDIUM, 13,
               c == MIX_MASTER ? COL_TEXT : COL_TEXT_DIM);
        char pct[20];
        if (mixer_is_input(c))
            snprintf(pct, sizeof pct, "%s", audio_capture_ok() ? audio_capture_name() : "none");
        else if (c == MIX_MASTER) snprintf(pct, sizeof pct, "%d%%", vol);
        else snprintf(pct, sizeof pct, "%d%% (%d)", vol, eff);   /* what the codec gets */
        font_draw_fit(FONT_REGULAR, 11, colx + colw / 2 - imin_(font_width(FONT_REGULAR, 11, pct, -1), colw - 8) / 2,
                      ly + 22, pct, colw - 8, muted ? COL_BAD : COL_TEXT_MUTE, 255);
    }

    text_wrap(cx + PAD, cy + ch - FOOT_H + 4, cw - 2 * PAD,
              "Drag a fader or click its track. Arrows move the selected one, M mutes, "
              "R restores the defaults. Levels are kept in /etc/mixer.conf.",
              FONT_REGULAR, 11, 15, 2, COL_TEXT_MUTE);
}

/* ----------------------------------------------------------------- input --- */

/* A level change reaches the hardware by two different routes. Reopening the
 * input is the only way to push a new capture gain at the codec -- the ring is
 * where the driver applies it -- and it is cheap enough to do on a drag. */
static void gain_changed(int c) {
    if (mixer_is_input(c)) {
        if (audio_capture_active()) { audio_capture_close(); audio_capture_open(); }
    } else {
        audio_gain_changed();
    }
}

/* Set a channel from a y inside its fader. Top of the track is 100. */
static void set_from_y(int c, int ly, int cy, int ch, int cw, int cx) {
    int fx, fy, fw, fh;
    fader_rect(c, cx, cy, cw, ch, &fx, &fy, &fw, &fh);
    int v = (fy + fh - ly) * 100 / (fh > 0 ? fh : 1);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    mixer_set_volume(c, v);
    gain_changed(c);
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
    /* Clicks arrive relative to the client area and the layout is in desktop
     * coordinates. This compared one with the other for years, so the faders
     * only answered with the window parked in the top-left corner. */
    lx += cx; ly += cy;
    for (int c = 0; c < mixer_count(); c++) {
        int colx, colw;
        column(c, cx, cw, &colx, &colw);
        if (lx < colx || lx >= colx + colw) continue;
        int fx, fy, fw, fh;
        fader_rect(c, cx, cy, cw, ch, &fx, &fy, &fw, &fh);
        int my = fy + fh + 8;
        sel = c;
        if (ly >= my && ly < my + MUTE_H - 4) {
            if (mixer_is_input(c)) {
                if (audio_capture_active()) audio_capture_close();
                else                        audio_capture_open();
            } else {
                mixer_toggle_mute(c);
                audio_gain_changed();
            }
        } else if (ly >= fy - 8 && ly < fy + fh + 8) {
            grabbed = c;
            set_from_y(c, ly, cy, ch, cw, cx);
        }
        break;
    }
    mixer_save();
    gui_redraw_window(win);
}

static void mixer_drag(window_t *w, int lx, int ly) {
    (void)w; (void)lx;
    if (grabbed < 0) return;
    set_from_y(grabbed, ly + last_cy, last_cy, last_ch, last_cw, last_cx);
    gui_redraw_window(win);
}

/* The wheel moves the selected column in fives -- the step the tray slider and
 * the Media player both use. The scroll callback is given a delta and nothing
 * else, so it cannot know which column the pointer is over; clicking a column
 * selects it, which is the same gesture you were making anyway. */
static void mixer_scroll(window_t *w, int delta) {
    (void)w;
    mixer_set_volume(sel, mixer_volume(sel) + delta * 5);
    gain_changed(sel);
    mixer_save();
    gui_redraw_window(win);
}

static void mixer_key(window_t *w, char c) {
    (void)w;
    switch ((unsigned char)c) {
    case KEY_LEFT:  if (sel > 0) sel--; break;
    case KEY_RIGHT: if (sel < mixer_count() - 1) sel++; break;
    case KEY_UP:   mixer_set_volume(sel, mixer_volume(sel) + 5); break;
    case KEY_DOWN: mixer_set_volume(sel, mixer_volume(sel) - 5); break;
    case ' ':
    case 'm': case 'M':
        if (mixer_is_input(sel)) {
            if (audio_capture_active()) audio_capture_close(); else audio_capture_open();
        } else mixer_toggle_mute(sel);
        break;
    case 'r': case 'R': mixer_reset(); break;
    default: return;
    }
    gain_changed(sel);
    mixer_save();
    gui_redraw_window(win);
}

/* The meters only mean anything if they are repainted while sound is playing,
 * and the window tick is the cheapest place to ask for that. */
static void mixer_tick(window_t *w) {
    if (w && !w->open && audio_capture_active()) audio_capture_close();
    audio_capture_pump();          /* no-op unless the input is open */
    if (mixer_peak(MIX_MASTER) > 0 || audio_capture_active()) gui_redraw_window(w);
}

/* draw() is the only callback given the client rect, so it stashes it. */
static void mixer_draw_wrap(window_t *w, int cx, int cy, int cw, int ch) {
    last_cx = cx; last_cy = cy; last_cw = cw; last_ch = ch;
    mixer_draw(w, cx, cy, cw, ch);
}

void mixer_app_init(void) {
    win = gui_add_window("Mixer", 480, 460, 0x9B6CF2, ICON_MIXER);
    if (!win) return;
    win->draw   = mixer_draw_wrap;
    win->click  = mixer_click;
    win->drag   = mixer_drag;
    win->scroll = mixer_scroll;
    win->key    = mixer_key;
    win->tick   = mixer_tick;
    win->lazy_tick = 1;          /* mixer_tick asks while a meter moves */
    win->min_w  = 400;
    win->min_h  = 400;
    win->x = 300; win->y = 110;
}

/* Opened from the speaker in quick settings, which is where somebody goes when
 * one thing is too loud rather than everything. */
void mixer_app_open(void) {
    if (!win) return;
    gui_open(win);
    gui_request_redraw();
}
