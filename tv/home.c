/* ===========================================================================
 *  BoltOS TV  -  tv/home.c
 *
 *  What you see when the set comes on: a film, edge to edge, and very little
 *  else. A slim pane of liquid glass down the left holds the apps -- each
 *  one its icon and its name -- and the name of the thing and the time sit
 *  quietly in the top corner. The film is the point; everything else should
 *  stay out of its way. Leave the remote alone for a while and even the pane
 *  fades, leaving a window rather than a menu.
 * ===========================================================================*/
#include <stdint.h>
#include "tv.h"
#include "string.h"
#include "stdio.h"

enum { A_LIVE, A_YOUTUBE, A_NETFLIX, A_PRIME, A_DISNEY, A_HBOMAX, A_APPLETV, A_HDMI, A_WEB, A_ADBLOCK, A_SETTINGS };

typedef struct {
    int         tile;
    const char *name;
    const char *line;          /* the hero's description */
} app_t;

static const app_t APPS[] = {
    { ART_LIVE,     "Live TV",     "Six channels of nature, filmed in 4K and on all the time." },
    { ART_YOUTUBE,  "YouTube",     "Search and watch, played by the TV itself. No account needed." },
    { ART_NETFLIX,  "Netflix",     "Plays in Firefox on a computer on your network, through the BoltOS Web Engine." },
    { ART_PRIME,    "Prime Video", "Plays in Firefox on a computer on your network, through the BoltOS Web Engine." },
    { ART_DISNEY,   "Disney+",     "Plays in Firefox on a computer on your network, through the BoltOS Web Engine." },
    { ART_HBOMAX,   "HBO Max",     "Plays in Firefox on a computer on your network, through the BoltOS Web Engine." },
    { ART_APPLETV,  "Apple TV",    "Switch over to the Apple TV on HDMI 2." },
    { ART_HDMI,     "HDMI 1",      "A games console, a Blu-ray player or a set-top box." },
    { ART_WEB,      "Browser",     "The web, with the ads taken out before they load." },
    { ART_SHIELD,   "Ad Blocker",  0 },
    { ART_SETTINGS, "Settings",    "Picture, sound, network, background and the sleep timer." },
};
#define NAPPS ((int)(sizeof APPS / sizeof APPS[0]))

static int   focus;
static float fy = -1;                      /* focus pill, animated, design px */
static float hero_a = 1, ui_a;             /* hero fade, whole-UI fade        */
static int   hero_shown = -1, hero_prev = -1;
static uint64_t hero_t0, last_input, intro_t0, last_tick;
static int   ambient;
static glasscache_t gc_side;

/* geometry, in design pixels (see S() in tv.h) */
#define SX 60                              /* the pane's left edge            */
#define SY 60                              /* and its top and bottom margin   */
#define SW 400                             /* its width                       */
#define PAD 20                             /* inside the pane, above the rows */
#define ROW_H 84
#define GROUP_GAP 22                       /* between watching, inputs and apps */
#define AMBIENT_MS 45000

/* The pane runs the height of the screen, 1080 / density design px. */
static int pane_y(void) { return SY; }
static int pane_h(void) { return 1080 * 100 / UI_DENSITY - 2 * SY; }
static int row_y(int i) {
    return pane_y() + PAD + i * ROW_H + (i >= A_APPLETV ? GROUP_GAP : 0) + (i >= A_WEB ? GROUP_GAP : 0);
}

static void activate(int i) {
    sfx_select();
    switch (i) {
    case A_LIVE:     tv_push(&view_live); break;
    case A_YOUTUBE:  tv_push(&view_youtube); break;
    case A_NETFLIX: case A_PRIME: case A_DISNEY: case A_HBOMAX:
        stream_select(SVC_NETFLIX + (i - A_NETFLIX)); tv_push(&view_stream); break;
    case A_APPLETV:  input_select(1); tv_push(&view_input); break;
    case A_HDMI:     input_select(0); tv_push(&view_input); break;
    case A_WEB:      tv_push(&view_web); break;
    case A_ADBLOCK:  tv_push(&view_adblock); break;
    case A_SETTINGS: tv_push(&view_settings); break;
    }
}

static void home_enter(void) {
    shell_film_unlock();
    last_input = tv_now_ms();
    if (!intro_t0) intro_t0 = tv_now_ms();
}

static int wake(void) {
    last_input = tv_now_ms();
    if (ambient) { ambient = 0; return 1; }
    return 0;
}

static void home_key(tvkey_t k) {
    if (wake()) return;                    /* the first press just wakes it */
    int old = focus;
    switch (k.k) {
    case K_UP:   if (focus > 0) focus--; break;
    case K_DOWN: if (focus < NAPPS - 1) focus++; break;
    case K_OK: case K_RIGHT: activate(focus); return;
    case K_CHUP: case K_CHDN: tv_push(&view_live); return;
    case K_GUIDE: tv_push(&view_live); live_tune(live_channel()); return;
    case K_CHAR:
        if (k.ch >= '1' && k.ch <= '9') { live_tune(k.ch - '1'); tv_push(&view_live); }
        return;
    case K_BACK: ambient = 1; return;      /* Back on home: just the film */
    default: return;
    }
    if (focus != old) sfx_move();
}

static void home_pointer(int x, int y, int click, int wheel) {
    if (wake()) return;
    (void)wheel;
    for (int i = 0; i < NAPPS; i++) {
        if (ui_hit(x, y, S(SX + 12), S(row_y(i)), S(SW - 24), S(ROW_H))) {
            if (focus != i) { focus = i; sfx_move(); }
            if (click) activate(i);
            return;
        }
    }
}

static void home_tick(uint64_t now) {
    uint64_t dt = last_tick ? now - last_tick : 16;
    last_tick = now;
    if (!ambient && now - last_input > AMBIENT_MS && tv_top() == &view_home) ambient = 1;

    /* the intro: a moment of logo, then the glass slides in */
    float target_ui = ambient ? 0.0f : 1.0f;
    if (now - intro_t0 < 1500) target_ui = 0.0f;
    float nu = ui_ease(ui_a, target_ui, dt, ambient ? 1600 : 650);
    float ty = (float)row_y(focus);
    if (fy < 0) fy = ty;
    float nfy = ui_ease(fy, ty, dt, 170);
    if (focus != hero_shown) {
        hero_prev = hero_shown;
        hero_shown = focus;
        hero_t0 = now;
    }
    float nh = (now - hero_t0) < 420 ? (float)(now - hero_t0) / 420.0f : 1.0f;
    if (nu != ui_a || nfy != fy || nh != hero_a || now - intro_t0 < 3200) tv_redraw();
    ui_a = nu; fy = nfy; hero_a = nh;
}

/* ---- drawing ------------------------------------------------------------- */

/* Top right, small: the name, and under it the day and the time. */
static void draw_corner(int alpha) {
    int right = tv_w - S(72), cy = S(70);
    /* a soft shade under it, so small type stays readable on snow and sky */
    ui_glow(tv_w - S(60), S(40), S(560), S(200), 0x000000, alpha * 150 / 255);
    int wt = font_width(FONT_REGULAR, S(26), " TV", -1);
    int wb = font_width(FONT_SEMIBOLD, S(26), "BoltOS", -1);
    ui_text_shadow(FONT_SEMIBOLD, S(26), right - wt - wb, cy, "BoltOS", 0xFFFFFF, alpha, 0);
    ui_text_shadow(FONT_REGULAR, S(26), right - wt, cy, " TV", 0xFFFFFF, alpha, 0);

    int h, m, wd, day, mon;
    tv_clock(&h, &m, 0, &wd, &day, &mon, 0);
    char t[16], line[80];
    fmt_time(t, sizeof t, h, m);
    snprintf(line, sizeof line, "%s %d %s  %s", tv_day_name(wd), day, tv_month_name(mon), t);
    ui_text_shadow(FONT_REGULAR, S(20), right, cy + S(34), line, 0xE8EBF0, alpha * 210 / 255, 2);
}

static void draw_hero(int i, int alpha, int dy) {
    if (i < 0 || alpha <= 0) return;
    int x = S(SX + SW + 88), w = S(720);
    int top = tv_h - S(250) + dy;
    char line[160];
    const char *desc = APPS[i].line;
    if (i == A_ADBLOCK) {
        uint32_t n = adblock_blocked_total();
        char num[24]; fmt_int(num, sizeof num, (long)n);
        if (!tvs.adblock) snprintf(line, sizeof line, "Off. Turn it on to block ads and trackers for the whole TV.");
        else if (n) snprintf(line, sizeof line, "%s ads and trackers blocked so far, for every app on this TV.", num);
        else snprintf(line, sizeof line, "On for every app on this TV. %d ad and tracking networks on the list.",
                      adblock_list_size());
        desc = line;
    }
    ui_text_shadow(FONT_DISPLAY, S(68), x, top + S(30), APPS[i].name, 0xFFFFFF, alpha, 0);
    ui_wrap(FONT_REGULAR, S(26), x, top + S(78) + S(2), w, S(38), 2, desc, 0x000000, alpha * 70 / 255);
    ui_wrap(FONT_REGULAR, S(26), x, top + S(78), w, S(38), 2, desc, 0xEEF1F6, alpha * 225 / 255);
}

static void draw_sidebar(int alpha, int slide) {
    int x = S(SX) - slide, y = S(pane_y()), w = S(SW), h = S(pane_h());
    ui_glass(&gc_side, x, y, w, h, S(34), 140, alpha);

    /* the focus pill slides between rows */
    ui_focus(x + S(12), S((int)fy), w - S(24), S(ROW_H), S(26), alpha);

    for (int i = 0; i < NAPPS; i++) {
        int cy = S(row_y(i)) + S(ROW_H) / 2;
        int on = i == focus;
        int isz = S(on ? 56 : 52);
        art_tile(APPS[i].tile, x + S(30) + (S(56) - isz) / 2, cy - isz / 2, isz, alpha);
        ui_text(FONT_MEDIUM, S(28), x + S(30) + S(56) + S(22), cy, APPS[i].name,
                on ? 0x101318 : 0xFFFFFF, on ? alpha : alpha * 220 / 255, 0);
    }
    /* hairlines between watching, the inputs and doing */
    gfx_fill_a(x + S(34), S(row_y(A_APPLETV)) - S(GROUP_GAP) / 2 - 1, w - S(68), 1, 0xFFFFFF, alpha * 34 / 255);
    gfx_fill_a(x + S(34), S(row_y(A_WEB)) - S(GROUP_GAP) / 2 - 1, w - S(68), 1, 0xFFFFFF, alpha * 34 / 255);
}

static void draw_intro(uint64_t now) {
    uint64_t dt = now - intro_t0;
    if (dt > 2600) return;
    int a = dt < 300 ? (int)(dt * 255 / 300) : dt > 1700 ? (int)((2600 - dt) * 255 / 900) : 255;
    if (a < 0) a = 0;
    /* black under the logo until the first frame of film is in */
    int black = dt < 1300 ? 255 : dt < 2300 ? (int)((2300 - dt) * 255 / 1000) : 0;
    if (black > 0) gfx_fill_a(0, 0, tv_w, tv_h, 0x000000, black);
    int cx = tv_w / 2, cy = tv_h / 2;
    float s = 1.0f + (float)dt / 26000.0f;
    int px = (int)(S(80) * s);
    int wb = font_width(FONT_SEMIBOLD, px, "BoltOS", -1), wt = font_width(FONT_DISPLAY, px, " TV", -1);
    int x0 = cx - (wb + wt) / 2;
    ui_text(FONT_SEMIBOLD, px, x0, cy, "BoltOS", 0xFFFFFF, a, 0);
    ui_text(FONT_DISPLAY, px, x0 + wb, cy, " TV", 0xFFFFFF, a, 0);
    tv_redraw();
}

static void home_draw(uint64_t now) {
    int ua = (int)(ui_a * 255);
    /* A soft darkening behind the pane and the corner, scaled with the UI so
     * ambient mode is the film at full strength. */
    if (ua > 0) ui_scrim_left(S(820), 90 * ua / 255);
    ui_scrim_top(S(200), 60);

    if (ua > 0) {
        int slide = (int)((1.0f - ui_a) * S(80));
        draw_sidebar(ua, slide);
        int ha = (int)(hero_a * 255) * ua / 255;
        int dy = (int)((1.0f - hero_a) * S(20));
        if (hero_prev >= 0 && hero_a < 1.0f) draw_hero(hero_prev, (255 - (int)(hero_a * 255)) * ua / 255, -S(8));
        draw_hero(hero_shown, ha, dy);
    }
    int ia = now - intro_t0 < 1500 ? 0 : 255;
    draw_corner(ambient ? 190 : ia);
    draw_intro(now);
}

const view_t view_home = {
    .name = "home", .backdrop = BG_FILM,
    .enter = home_enter, .key = home_key, .pointer = home_pointer,
    .draw = home_draw, .tick = home_tick,
};
