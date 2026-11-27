/* ===========================================================================
 *  BoltOS Mini  -  mini/shell.c
 *
 *  Home, and the frame of every app. A bar runs along the top: BoltOS (Home)
 *  and the apps by number on the left, the network, the processor and
 *  the clock on the right. Home is the time, a few facts about the machine
 *  and a row of the apps; an open app gets everything under the bar.
 *  Arrows and Return, the number keys, F1-F9 or a click open an app; Esc or
 *  the Windows/Command key comes back.
 *
 *  The look is flat on purpose: solid colours, hairlines, small radii, no
 *  blur and no shadows. It is also what keeps a frame cheap on a 1 GHz A53,
 *  and a screen that is mostly one colour is what goes fastest to a viewer.
 *
 *  Frames are drawn only when something changed -- a key, a click, a toast,
 *  the minute turning -- and always whole, into the back buffer;
 *  screen_present copies only the tiles that differ to the front.
 * ===========================================================================*/
#include <stdint.h>
#include "ui.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"
#include "math.h"
#include "update.h"

/* The built-in apps, then whatever Packages has installed (shell_add_app). */
#define MAX_APPS 10
static const app_t *apps[MAX_APPS] = { &app_browser, &app_code, &app_terminal, &app_monitor, &app_notes, &app_clock,
                                       &app_settings, &app_packages };
static int napps = 8;
#define NAPPS napps

theme_t th;
const char *const wallpaper_names[3] = { "Dark", "Light", "Black" };
const char *const accent_names[6] = { "Blue", "Teal", "Green", "Amber", "Red", "Violet" };
static const uint32_t accents[6] = { 0x4C8DF6, 0x26A69A, 0x4CAF6A, 0xE0A030, 0xE5534B, 0x8C7CF0 };

static const app_t *cur;                 /* NULL: home                          */
static int sel;                          /* home: the selected app              */
static int dirty = 1;
static uint64_t last_frame;
static int pointer_x = -1, pointer_y = -1, pointer_btn;
/* An app that has the whole screen (a game): no bar, no arrow, every key
 * but Super is its own, and it draws and presents its frames itself. */
static int fullscreen;
/* What that app asked for (shell_quiet_mode): nothing else running in the
 * background, the screen never going to sleep. It goes with the app: Home,
 * another app or leaving full screen drops it. */
static int hold_quiet, hold_awake;

/* ---- the theme ---------------------------------------------------------------------------------- */
typedef struct { uint32_t bg, bar, surface, line, text, sub, faint; } palette_t;
static const palette_t palettes[3] = {
    { 0x17181B, 0x101113, 0x1E1F23, 0x2C2E33, 0xE8E9EC, 0xA2A5AD, 0x6A6E78 },     /* Dark  */
    { 0xF2F3F5, 0xFFFFFF, 0xFFFFFF, 0xD7DAE0, 0x1B1D21, 0x4F545D, 0x878C96 },     /* Light */
    { 0x000000, 0x0B0B0C, 0x101113, 0x26272B, 0xE6E6E6, 0x9A9CA3, 0x62656C },     /* Black */
};

void theme_set(int wp, int ac) {
    th.wallpaper = wp < 0 ? 0 : wp > 2 ? 2 : wp;
    th.accent_i = ac < 0 ? 0 : ac > 5 ? 5 : ac;
    th.accent = accents[th.accent_i];
    th.dark = th.wallpaper != 1;
    const palette_t *p = &palettes[th.wallpaper];
    th.bg = p->bg; th.bar = p->bar; th.surface = p->surface; th.line = p->line;
    th.text = p->text; th.sub = p->sub; th.faint = p->faint;
    th.panel = p->surface; th.panel_a = 255;
    th.sel = gfx_lerp(p->surface, th.accent, th.dark ? 20 : 14, 100);
    dirty = 1;
}

/* ---- drawing helpers ---------------------------------------------------------------------- */
void ui_card(int id, int x, int y, int w, int h, int r) {
    (void)id;
    if (r > S(6)) r = S(6);
    gfx_rrect(x, y, w, h, r, th.line, 255);
    gfx_rrect(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, th.surface, 255);
}

int ui_hit(int px, int py, int x, int y, int w, int h) { return px >= x && py >= y && px < x + w && py < y + h; }

void ui_keycap(int x, int cy, const char *label) {
    int w = font_width(FONT_MONO, S(11), label, -1) + S(10), h = S(18);
    gfx_rrect(x, cy - h / 2, w, h, S(3), th.line, 255);
    gfx_rrect(x + 1, cy - h / 2 + 1, w - 2, h - 2, S(2), th.surface, 255);
    text_l(x + S(5), cy, label, FONT_MONO, S(11), th.sub);
}

int ui_row(int x, int y, int w, const char *label, const char *value, int is_sel) {
    int h = S(36);
    if (is_sel) {
        gfx_rrect(x, y, w, h, S(4), th.sel, 255);
        gfx_fill(x, y + S(8), S(2), h - S(16), th.accent);
    }
    text_l(x + S(14), y + h / 2, label, FONT_MEDIUM, S(14), th.text);
    if (value) text_r(x + w - S(14), y + h / 2, value, FONT_REGULAR, S(14), is_sel ? th.text : th.sub);
    return h;
}

/* The glyphs are drawn, not bitmaps: lines and rounded shapes, so they are
 * sharp at every size the screen asks for. */
void ui_glyph(int id, int cx, int cy, int sz, uint32_t c) {
    float s = (float)sz, x = (float)cx, y = (float)cy, t = s * 0.085f;
    if (t < 1.4f) t = 1.4f;
    switch (id) {
    case G_TERMINAL:
        gfx_line(x - s * .30f, y - s * .20f, x - s * .08f, y, t, c, 255);
        gfx_line(x - s * .08f, y, x - s * .30f, y + s * .20f, t, c, 255);
        gfx_line(x + s * .02f, y + s * .22f, x + s * .30f, y + s * .22f, t, c, 255);
        break;
    case G_MONITOR: {
        float px[] = { -.36f, -.16f, -.06f, .06f, .16f, .36f }, py[] = { .04f, .04f, -.26f, .26f, .04f, .04f };
        for (int i = 0; i < 5; i++)
            gfx_line(x + s * px[i], y + s * py[i], x + s * px[i + 1], y + s * py[i + 1], t, c, 255);
        break;
    }
    case G_NOTES:
        gfx_rrect_stroke((int)(x - s * .26f), (int)(y - s * .32f), (int)(s * .52f), (int)(s * .64f), (int)(s * .08f), (int)t, c, 255);
        for (int i = 0; i < 3; i++)
            gfx_line(x - s * .13f, y - s * .12f + s * .13f * (float)i, x + (i == 2 ? s * .02f : s * .13f),
                     y - s * .12f + s * .13f * (float)i, t * .85f, c, 255);
        break;
    case G_CLOCK:
        gfx_ring(x, y, s * .32f, t, c, 255);
        gfx_line(x, y, x, y - s * .18f, t, c, 255);
        gfx_line(x, y, x + s * .13f, y + s * .07f, t, c, 255);
        break;
    case G_SETTINGS:
        for (int i = 0; i < 8; i++) {
            float a = (float)i * 0.785398f;
            gfx_line(x + cosf(a) * s * .22f, y + sinf(a) * s * .22f, x + cosf(a) * s * .34f, y + sinf(a) * s * .34f,
                     t * 1.5f, c, 255);
        }
        gfx_ring(x, y, s * .22f, t * 1.2f, c, 255);
        gfx_circle(x, y, s * .07f, c, 255);
        break;
    case G_MAC:
        gfx_rrect_stroke((int)(x - s * .30f), (int)(y - s * .24f), (int)(s * .60f), (int)(s * .38f), (int)(s * .05f), (int)t, c, 255);
        gfx_line(x - s * .40f, y + s * .24f, x + s * .40f, y + s * .24f, t, c, 255);
        break;
    case G_KEYBOARD:
        gfx_rrect_stroke((int)(x - s * .38f), (int)(y - s * .22f), (int)(s * .76f), (int)(s * .44f), (int)(s * .07f), (int)t, c, 255);
        for (int i = 0; i < 4; i++) gfx_circle(x - s * .20f + s * .133f * (float)i, y - s * .06f, t * .7f, c, 255);
        gfx_line(x - s * .16f, y + s * .10f, x + s * .16f, y + s * .10f, t, c, 255);
        break;
    case G_POWER:
        gfx_arc(x, y + s * .03f, s * .28f, t, 40, 320, c, 255);
        gfx_line(x, y - s * .34f, x, y - s * .02f, t, c, 255);
        break;
    case G_CHECK:
        gfx_line(x - s * .26f, y, x - s * .06f, y + s * .20f, t, c, 255);
        gfx_line(x - s * .06f, y + s * .20f, x + s * .28f, y - s * .20f, t, c, 255);
        break;
    case G_CHEVRON:
        gfx_line(x - s * .08f, y - s * .18f, x + s * .10f, y, t, c, 255);
        gfx_line(x + s * .10f, y, x - s * .08f, y + s * .18f, t, c, 255);
        break;
    case G_CLOSE:
        gfx_line(x - s * .20f, y - s * .20f, x + s * .20f, y + s * .20f, t, c, 255);
        gfx_line(x + s * .20f, y - s * .20f, x - s * .20f, y + s * .20f, t, c, 255);
        break;
    case G_DOT:
        gfx_circle(x, y, s * .5f, c, 255);
        break;
    case G_GLOBE:
        gfx_ring(x, y, s * .32f, t, c, 255);
        gfx_arc(x + s * .13f, y, s * .34f, t * .85f, 112, 248, c, 255);     /* a meridian, each side */
        gfx_arc(x - s * .13f, y, s * .34f, t * .85f, -68, 68, c, 255);
        gfx_line(x - s * .32f, y, x + s * .32f, y, t * .85f, c, 255);
        gfx_line(x - s * .26f, y - s * .16f, x + s * .26f, y - s * .16f, t * .7f, c, 255);
        gfx_line(x - s * .26f, y + s * .16f, x + s * .26f, y + s * .16f, t * .7f, c, 255);
        break;
    case G_WIFI:                                            /* three arcs over a dot */
        for (int i = 0; i < 3; i++) gfx_arc(x, y + s * .22f, s * (.14f + .13f * (float)i), t, -45, 45, c, 255);
        gfx_circle(x, y + s * .20f, t * 1.1f, c, 255);
        break;
    case G_BLUETOOTH:                                       /* the rune: a line and two loops */
        gfx_line(x - s * .02f, y - s * .34f, x - s * .02f, y + s * .34f, t, c, 255);
        gfx_line(x - s * .02f, y - s * .34f, x + s * .18f, y - s * .16f, t, c, 255);
        gfx_line(x + s * .18f, y - s * .16f, x - s * .22f, y + s * .18f, t, c, 255);
        gfx_line(x - s * .02f, y + s * .34f, x + s * .18f, y + s * .16f, t, c, 255);
        gfx_line(x + s * .18f, y + s * .16f, x - s * .22f, y - s * .18f, t, c, 255);
        break;
    case G_PACKAGE:                                         /* a box, its lid's seam and a strap */
        gfx_rrect_stroke((int)(x - s * .32f), (int)(y - s * .26f), (int)(s * .64f), (int)(s * .54f), (int)(s * .05f), (int)t, c, 255);
        gfx_line(x - s * .32f, y - s * .08f, x + s * .32f, y - s * .08f, t, c, 255);
        gfx_line(x - s * .08f, y - s * .08f, x - s * .08f, y + s * .06f, t, c, 255);
        gfx_line(x + s * .08f, y - s * .08f, x + s * .08f, y + s * .06f, t, c, 255);
        gfx_line(x - s * .08f, y + s * .06f, x + s * .08f, y + s * .06f, t, c, 255);
        break;
    case G_CHAT:                                            /* a speech bubble with three dots */
        gfx_rrect_stroke((int)(x - s * .34f), (int)(y - s * .28f), (int)(s * .68f), (int)(s * .46f), (int)(s * .10f), (int)t, c, 255);
        gfx_line(x - s * .16f, y + s * .17f, x - s * .22f, y + s * .32f, t, c, 255);
        gfx_line(x - s * .22f, y + s * .32f, x - s * .02f, y + s * .18f, t, c, 255);
        for (int i = -1; i <= 1; i++) gfx_circle(x + s * .15f * (float)i, y - s * .05f, t * .9f, c, 255);
        break;
    case G_CODE:                                            /* </> */
        gfx_line(x - s * .12f, y - s * .20f, x - s * .30f, y, t, c, 255);
        gfx_line(x - s * .30f, y, x - s * .12f, y + s * .20f, t, c, 255);
        gfx_line(x + s * .12f, y - s * .20f, x + s * .30f, y, t, c, 255);
        gfx_line(x + s * .30f, y, x + s * .12f, y + s * .20f, t, c, 255);
        gfx_line(x + s * .05f, y - s * .26f, x - s * .05f, y + s * .26f, t, c, 255);
        break;
    case G_FOLDER:                                          /* a tab and a body */
        gfx_rrect_stroke((int)(x - s * .34f), (int)(y - s * .18f), (int)(s * .68f), (int)(s * .44f), (int)(s * .05f), (int)t, c, 255);
        gfx_line(x - s * .34f, y - s * .18f, x - s * .30f, y - s * .28f, t, c, 255);
        gfx_line(x - s * .30f, y - s * .28f, x - s * .06f, y - s * .28f, t, c, 255);
        gfx_line(x - s * .06f, y - s * .28f, x, y - s * .18f, t, c, 255);
        break;
    case G_FILE:                                            /* a page with lines */
        gfx_rrect_stroke((int)(x - s * .24f), (int)(y - s * .32f), (int)(s * .48f), (int)(s * .64f), (int)(s * .05f), (int)t, c, 255);
        gfx_line(x - s * .12f, y - s * .10f, x + s * .12f, y - s * .10f, t * .8f, c, 255);
        gfx_line(x - s * .12f, y + s * .04f, x + s * .12f, y + s * .04f, t * .8f, c, 255);
        gfx_line(x - s * .12f, y + s * .18f, x + s * .04f, y + s * .18f, t * .8f, c, 255);
        break;
    case G_PLAY:                                            /* a triangle */
        for (int i = 0; i <= (int)(s * .44f); i++) {
            float yy = y - s * .22f + (float)i, half = (s * .22f - fabsf((float)i - s * .22f)) * 1.0f;
            gfx_fill((int)(x - s * .14f), (int)yy, (int)(half * 1.4f) + 1, 1, c);
        }
        break;
    case G_STOP:
        gfx_rrect((int)(x - s * .18f), (int)(y - s * .18f), (int)(s * .36f), (int)(s * .36f), (int)(s * .04f), c, 255);
        break;
    case G_UPDATE:                                          /* an arrow down onto a line */
        gfx_line(x, y - s * .32f, x, y + s * .12f, t, c, 255);
        gfx_line(x - s * .17f, y - s * .04f, x, y + s * .13f, t, c, 255);
        gfx_line(x + s * .17f, y - s * .04f, x, y + s * .13f, t, c, 255);
        gfx_line(x - s * .30f, y + s * .30f, x + s * .30f, y + s * .30f, t, c, 255);
        break;
    case G_LOCK:
        gfx_arc(x, y - s * .06f, s * .16f, t, -90, 90, c, 255);
        gfx_line(x - s * .16f, y - s * .06f, x - s * .16f, y + s * .02f, t, c, 255);
        gfx_line(x + s * .16f, y - s * .06f, x + s * .16f, y + s * .02f, t, c, 255);
        gfx_rrect((int)(x - s * .26f), (int)(y + s * .0f), (int)(s * .52f), (int)(s * .34f), (int)(s * .06f), c, 255);
        break;
    }
}

/* An app's mark: its glyph on a hairline square, no colour of its own. */
void ui_tile(const app_t *a, int x, int y, int size, int on) {
    int r = size > S(30) ? S(5) : S(3);
    gfx_rrect(x, y, size, size, r, on ? th.accent : th.line, 255);
    gfx_rrect(x + 1, y + 1, size - 2, size - 2, r - 1, on ? th.sel : th.surface, 255);
    ui_glyph(a->glyph, x + size / 2, y + size / 2, size * 60 / 100, on ? th.accent : th.sub);
}

/* ---- the clock as text ------------------------------------------------------------------- */
static const char *const days[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const months[12] = { "January", "February", "March", "April", "May", "June", "July",
                                         "August", "September", "October", "November", "December" };
void clock_text(char *hm, int hm_cap, char *date, int date_cap) {
    int y, mo, d, h, mi, s, wd;
    clock_local(&y, &mo, &d, &h, &mi, &s, &wd);
    snprintf(hm, (size_t)hm_cap, "%d:%02d", h, mi);
    if (clock_known()) snprintf(date, (size_t)date_cap, "%s, %d %s", days[wd], d, months[mo - 1]);
#ifdef MINI_RELEASE
    else snprintf(date, (size_t)date_cap, "Clock not set " TXT_ENDASH " plug in the network cable");
#else
    else snprintf(date, (size_t)date_cap, "Clock not set " TXT_ENDASH " open 10.77.0.1 on your Mac");
#endif
}

/* ---- CPU load ------------------------------------------------------------------------------ */
uint64_t g_idle_us;
static uint8_t load_hist[120];
static int load_n, load_now;
static uint64_t load_t0, load_idle0;

static void load_sample(void) {
    uint64_t t = now_us();
    if (t - load_t0 < 1000000) return;
    uint64_t dt = t - load_t0, di = g_idle_us - load_idle0;
    int busy = dt ? (int)(100 - (di * 100 / dt)) : 0;
    load_now = busy < 0 ? 0 : busy > 100 ? 100 : busy;
    memmove(load_hist, load_hist + 1, sizeof load_hist - 1);
    load_hist[sizeof load_hist - 1] = (uint8_t)load_now;
    if (load_n < (int)sizeof load_hist) load_n++;
    load_t0 = t; load_idle0 = g_idle_us;
    if (cur == &app_monitor) dirty = 1;
}
int shell_cpu_load(void) { return load_now; }
int shell_cpu_history(uint8_t *out, int n) {
    if (n > (int)sizeof load_hist) n = (int)sizeof load_hist;
    memcpy(out, load_hist + sizeof load_hist - n, (size_t)n);
    return load_n < n ? load_n : n;
}

int shell_mac_connected(void) {
    netif_t *n = net_if(0);
    return (n && n->up && strcmp(n->name, "usb0") == 0) || vnc_clients() > 0;
}

/* ---- the screen going dark, and screen sleep ------------------------------------------------------
 *  A 1920 x 1080 frame costs tens of ms of the core everything else runs on, so nothing is drawn while
 *  nobody can see it: no picture going out of the HDMI port (the monitor was unplugged, or the screen
 *  is asleep) and no Screen Sharing viewer. Screen sleep turns the HDMI output off -- the monitor goes
 *  to standby -- after the chosen minutes without a key, click, touch or pointer move, and after one
 *  minute when no monitor is detected at all, so a Pi left on its own stops drawing too. The next key,
 *  click or move wakes it and does nothing else. A Screen Sharing viewer keeps it awake. Kept as "slp". */
static const uint8_t sleep_choices[] = { 0, 1, 5, 10, 30, 60 };
#define NSLEEP ((int)sizeof sleep_choices)
static int sleep_min = 10, asleep, was_lit = 1, was_dark;
static uint64_t last_input, woke_at;

int screen_sleep_minutes(void) { return sleep_min; }

void screen_sleep_label(char *out, int cap) {
    if (!sleep_min) snprintf(out, (size_t)cap, "Never");
    else snprintf(out, (size_t)cap, "After %d minute%s", sleep_min, sleep_min == 1 ? "" : "s");
}

void screen_sleep_cycle(int dir) {
    int i = 0;
    while (i < NSLEEP && sleep_choices[i] != sleep_min) i++;
    if (i == NSLEEP) i = 3;
    i = (i + NSLEEP + dir) % NSLEEP;
    sleep_min = sleep_choices[i];
    uint8_t v = (uint8_t)sleep_min;
    keep_put("slp", &v, 1);
    woke_at = now_ms();                                    /* a new choice counts from now */
    dirty = 1;
}

int screen_is_dark(void) { return !display_lit() && vnc_clients() == 0; }

static void screen_wake(const char *why) {
    asleep = 0;
    woke_at = now_ms();
    display_wake();
    dirty = 1;
    kprintf("[screen] awake (%s)\n", why);
}

static void screen_sleep_poll(void) {
    uint64_t now = now_ms();
    int lit = display_lit();
    if (lit && !was_lit) { woke_at = now; asleep = 0; }   /* a monitor came along, or the screen woke */
    was_lit = lit;
    int dark = screen_is_dark();
    if (dark != was_dark) {
        was_dark = dark;
        if (dark) kprintf("[screen] not drawing: %s and no Screen Sharing viewer\n", asleep ? "asleep" : "nothing on the HDMI port");
        else { dirty = 1; kprintf("[screen] drawing again\n"); }
    }
    if (hold_awake && fullscreen && cur) { woke_at = now; return; }   /* a page that stays on */
    if (asleep || !lit || !sleep_min || vnc_clients() > 0) return;
    uint64_t since = now - (last_input > woke_at ? last_input : woke_at);
    int plugged = display_plugged();
    if (since < (plugged ? (uint64_t)sleep_min * 60000u : 60000u)) return;
    if (!display_sleep()) return;                         /* nothing to turn off (QEMU) */
    asleep = 1;
    kprintf("[screen] asleep after %lu min without input%s\n", (unsigned long)(since / 60000),
            plugged ? "" : " (no monitor detected)");
}

/* ---- settings that outlive a restart --------------------------------------------------------------
 *  The theme, the accent and the time zone go into a keep record (klog.c),
 *  which fs.c copies to the card, so they come back after a power cut too. */
typedef struct { int8_t wallpaper, accent; int16_t tz; } look_t;

void shell_settings_save(void) {
    look_t l = { (int8_t)th.wallpaper, (int8_t)th.accent_i, (int16_t)clock_tz_minutes() };
    keep_put("look", &l, (int)sizeof l);
}

void shell_settings_restore(void) {
    look_t l;
    display_restore();
    uint8_t s;
    if (keep_get("slp", &s, 1) == 1)
        for (int i = 0; i < NSLEEP; i++) if (sleep_choices[i] == s) sleep_min = s;
    if (keep_get("look", &l, (int)sizeof l) != (int)sizeof l) return;
    theme_set(l.wallpaper, l.accent);
    if (l.tz >= -12 * 60 && l.tz <= 14 * 60) clock_set_tz_minutes(l.tz);
    dirty = 1;
}

/* ---- the clipboard --------------------------------------------------------------------------------
 *  One for every app. A viewer's clipboard (Screen Sharing sends the Mac's
 *  when it changes) lands here, and what's copied on the Pi goes back to the
 *  viewers (vnc.c asks clip_gen() to see whether there's something new). */
static char *clip; static int clip_n, clip_cap;
static uint32_t clip_g, clip_viewer_g;

static void clip_store(const char *s, int n) {
    if (n < 0) n = 0;
    if (n + 1 > clip_cap) {
        kfree(clip);
        clip_cap = n + 1 < 256 ? 256 : n + 1;
        clip = kmalloc((size_t)clip_cap);
        if (!clip) { clip_cap = clip_n = 0; return; }
    }
    /* \r\n and lone \r become \n */
    int o = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\r') { clip[o++] = '\n'; if (i + 1 < n && s[i + 1] == '\n') i++; }
        else clip[o++] = s[i];
    }
    clip[o] = 0;
    clip_n = o;
}

void clip_set(const char *s, int n) { clip_store(s, n); clip_g++; }
void clip_from_viewer(const char *s, int n) { clip_store(s, n); clip_viewer_g = ++clip_g; }
const char *clip_get(int *n) { if (n) *n = clip_n; return clip ? clip : ""; }
/* what vnc.c sends a viewer: changed since `seen`, and not the viewer's own */
uint32_t clip_gen(void) { return clip_g == clip_viewer_g ? 0 : clip_g; }

/* ---- toasts -------------------------------------------------------------------------------- */
static char toast_t[48], toast_d[64];
static uint64_t toast_at;
#define TOAST_MS 3000
void shell_toast(const char *title, const char *detail) {
    snprintf(toast_t, sizeof toast_t, "%s", title ? title : "");
    snprintf(toast_d, sizeof toast_d, "%s", detail ? detail : "");
    toast_at = now_ms();
    if (!toast_at) toast_at = 1;
    dirty = 1;
}

/* Under the bar, on the right; there for three seconds, no fading. */
static void draw_toast(void) {
    if (!toast_at) return;
    int tw = font_width(FONT_SEMIBOLD, S(12), toast_t, -1), dw = font_width(FONT_REGULAR, S(12), toast_d, -1);
    int w = tw + (toast_d[0] ? dw + S(10) : 0) + S(30), h = S(34);
    int x = scr_w - w - S(12), y = S(28) + S(10);
    gfx_rrect(x, y, w, h, S(4), th.line, 255);
    gfx_rrect(x + 1, y + 1, w - 2, h - 2, S(3), th.surface, 255);
    gfx_fill(x + 1, y + 1, S(3), h - 2, th.accent);
    text_l(x + S(16), y + h / 2, toast_t, FONT_SEMIBOLD, S(12), th.text);
    if (toast_d[0]) text_l(x + S(16) + tw + S(10), y + h / 2, toast_d, FONT_REGULAR, S(12), th.sub);
}

/* ---- the bar ------------------------------------------------------------------------------------ */
#define BAR_H S(28)
#define BAR_MARK S(18)                         /* the mark in front of "BoltOS", Home's item */

/* The bar's left side: "BoltOS" (Home), then "1 Browser", "2 Terminal"...
 * With more apps than the bar has room for, the apps that aren't open show
 * only their key (bar_compact, decided each time the bar is drawn). */
static int bar_compact;
static int bar_item(int i, int *x) {                  /* i = -1 is Home; returns the width */
    int bx = S(14);
    const char *home = "BoltOS";
    int hw = BAR_MARK + S(5) + font_width(FONT_BOLD, S(12), home, -1) + S(16);
    if (i < 0) { *x = bx - S(8); return hw; }
    bx += hw - S(8) + S(10);
    for (int k = 0; k < NAPPS; k++) {
        int named = !bar_compact || apps[k] == cur;
        int w = font_width(FONT_MONO, S(11), "1", -1) + (named ? S(6) + font_width(FONT_MEDIUM, S(12), apps[k]->name, -1) : 0) + S(16);
        if (k == i) { *x = bx; return w; }
        bx += w + S(2);
    }
    *x = bx; return 0;
}

static const netif_t *main_link(void) {
    const netif_t *best = 0;
    for (int i = 0; net_if(i); i++) {
        const netif_t *n = net_if(i);
        if (!n->up || !n->ip) continue;
        if (!best || (!strcmp(best->name, "usb0") && strcmp(n->name, "usb0"))) best = n;
    }
    return best;
}

static int upd_x, upd_w;                             /* the bar's update note, to click */

/* How much the bar's right side needs at the least (the link without its address). */
static int bar_right_min(void) {
    char hm[16], date[96], b[64];
    clock_text(hm, sizeof hm, date, sizeof date);
    int w = S(14);
    if (clock_known()) w += font_width(FONT_SEMIBOLD, S(12), hm, -1) + S(8) + font_width(FONT_MEDIUM, S(12), "Wed 28 Sep", -1) + S(18);
    else w += font_width(FONT_MEDIUM, S(12), "clock not set", -1) + S(18);
    if (update_bar(b, sizeof b) > 0) w += font_width(FONT_MONO, S(11), b, -1) + S(18);
    w += font_width(FONT_MONO, S(11), "cpu 100%", -1) + S(18);
    const netif_t *n = main_link();
    w += font_width(FONT_MONO, S(11), n ? n->name : "offline", -1) + S(16);
    return w;
}

static void draw_bar(void) {
    int h = BAR_H, cy = h / 2;
    int full_end;
    bar_compact = 0;
    bar_item(NAPPS, &full_end);
    bar_compact = full_end + S(12) > scr_w - bar_right_min();
    gfx_fill(0, 0, scr_w, h, th.bar);
    gfx_fill(0, h, scr_w, 1, th.line);
    int x, w = bar_item(-1, &x);
    if (!cur) gfx_fill(x, h - S(2), w, S(2), th.accent);
    {
        int lx, ly;
        boltmark_center(x + S(8) + BAR_MARK / 2, cy, BAR_MARK, &lx, &ly);
        boltmark_draw(lx, ly, BAR_MARK, th.text, 255);
    }
    text_l(x + S(8) + BAR_MARK + S(5), cy, "BoltOS", FONT_BOLD, S(12), th.text);
    for (int i = 0; i < NAPPS; i++) {
        w = bar_item(i, &x);
        int on = cur == apps[i];
        if (on) { gfx_fill(x, 0, w, h, th.sel); gfx_fill(x, h - S(2), w, S(2), th.accent); }
        char n[2] = { (char)('1' + i), 0 };
        int nx = text_l(x + S(8), cy, n, FONT_MONO, S(11), on ? th.accent : th.faint);
        (void)nx;
        if (!bar_compact || on)
            text_l(x + S(8) + font_width(FONT_MONO, S(11), "1", -1) + S(6), cy, apps[i]->name, FONT_MEDIUM, S(12),
                   on ? th.text : th.sub);
    }

    /* right: link, processor, clock -- the link's address is left out when
     * the apps' names reach that far */
    int left_end;
    bar_item(NAPPS, &left_end);
    char hm[16], date[96], b[64];
    clock_text(hm, sizeof hm, date, sizeof date);
    int rx = scr_w - S(14);
    if (clock_known()) {
        int y0, mo, d, hh, mi, s, wd;
        clock_local(&y0, &mo, &d, &hh, &mi, &s, &wd);
        static const char *const dn[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        static const char *const mn[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        snprintf(b, sizeof b, "%s %d %s", dn[wd], d, mn[mo - 1]);
        rx -= text_r(rx, cy, hm, FONT_SEMIBOLD, S(12), th.text) + S(8);
        rx -= text_r(rx, cy, b, FONT_MEDIUM, S(12), th.sub) + S(18);
    } else rx -= text_r(rx, cy, "clock not set", FONT_MEDIUM, S(12), th.faint) + S(18);
    /* a newer BoltOS Mini, or one coming in: a click opens Settings, Updates */
    if (update_bar(b, sizeof b) > 0) {
        int tw = text_r(rx, cy, b, FONT_MONO, S(11), th.accent);
        upd_x = rx - tw - S(8); upd_w = tw + S(16);
        rx -= tw + S(18);
    } else upd_w = 0;
    snprintf(b, sizeof b, "cpu %2d%%", shell_cpu_load());
    rx -= text_r(rx, cy, b, FONT_MONO, S(11), th.sub) + S(18);
    const netif_t *n = main_link();
    if (n) {
        char ip[16]; net_fmt_ip(n->ip, ip);
        snprintf(b, sizeof b, "%s %s", n->name, ip);
        if (rx - font_width(FONT_MONO, S(11), b, -1) - S(10) < left_end + S(12)) snprintf(b, sizeof b, "%s", n->name);
        int tw = text_r(rx, cy, b, FONT_MONO, S(11), th.sub);
        gfx_fill(rx - tw - S(10), cy - S(3), S(6), S(6), 0x4CAF6A);
    } else {
        int tw = text_r(rx, cy, "offline", FONT_MONO, S(11), th.faint);
        gfx_fill(rx - tw - S(10), cy - S(3), S(6), S(6), 0xE0A030);
    }
}

/* ---- home ---------------------------------------------------------------------------------- */
static void tile_rect(int i, int *x, int *y, int *w, int *h) {
    int pad = S(40), gap = S(12);
    *w = (scr_w - 2 * pad - (NAPPS - 1) * gap) / NAPPS;
    *h = S(96);
    *x = pad + i * (*w + gap);
    *y = scr_h - S(44) - *h;
}

static void draw_facts(int x, int y) {
    char v[6][64];
    const char *k[6] = { "host", "kernel", "cpu", "memory", "uptime", "input" };
    snprintf(v[0], sizeof v[0], "%s", g_board_name);
    snprintf(v[1], sizeof v[1], "BoltOS Mini %s", mini_version());
    char *sp = strchr(v[1], ' '); sp = sp ? strchr(sp + 1, ' ') : 0; sp = sp ? strchr(sp + 1, ' ') : 0;
    if (sp) *sp = 0;                                  /* "BoltOS Mini 1.0"     */
    int tc_mc = board_temp_mc();
    if (board_cpu_mhz() && tc_mc > -1000000)
        snprintf(v[2], sizeof v[2], "4 x Cortex-A53, %u MHz, %d\xB0" "C", board_cpu_mhz(), (tc_mc + 500) / 1000);
    else if (board_cpu_mhz()) snprintf(v[2], sizeof v[2], "4 x Cortex-A53, %u MHz", board_cpu_mhz());
    else snprintf(v[2], sizeof v[2], "Cortex-A53 (QEMU)");
    uint64_t used, total; kheap_usage(&used, &total);
    snprintf(v[3], sizeof v[3], "%lu MiB, heap %lu / %lu MiB", (unsigned long)(g_ram_size >> 20),
             (unsigned long)(used >> 20), (unsigned long)(total >> 20));
    mini_uptime_str(v[4], sizeof v[4]);
    int kb = usbh_keyboards(), ms = usbh_mice(), tc = usbh_touchscreens();
    if (kb || ms || tc) snprintf(v[5], sizeof v[5], "%d keyboard%s, %d mouse%s%s", kb, kb == 1 ? "" : "s", ms, ms == 1 ? "" : "s",
                                 tc ? ", touch" : "");
    else snprintf(v[5], sizeof v[5], "%s", shell_mac_connected() ? "the Mac's keyboard" : "none yet");
    int lh = S(20), kw = font_width(FONT_MONO, S(12), "memory  ", -1);
    /* The mark on the left, the way neofetch puts the system's logo next to
     * its facts -- when the screen is wide enough for both. */
    int vw = 0;
    for (int i = 0; i < 6; i++) { int w = font_width(FONT_MONO, S(12), v[i], -1); if (w > vw) vw = w; }
    int lm = S(118);
    if (scr_w - x - S(24) >= lm + S(18) + kw + vw) {
        int lx, ly;
        boltmark_center(x + lm / 2, y + 5 * lh / 2, lm, &lx, &ly);
        boltmark_draw(lx, ly, lm, th.sub, 255);
        x += lm + S(18);
    }
    for (int i = 0; i < 6; i++) {
        text_l(x, y + i * lh, k[i], FONT_MONO, S(12), th.faint);
        text_l(x + kw, y + i * lh, v[i], FONT_MONO, S(12), th.sub);
    }
}

static void draw_home(void) {
    char hm[16], date[96];
    clock_text(hm, sizeof hm, date, sizeof date);
    int x0 = S(40), top = BAR_H + S(40);
    if (clock_known()) {
        font_draw(FONT_DISPLAY, S(72), x0 - S(3), top + font_ascent(FONT_DISPLAY, S(72)), hm, -1, th.text, 255);
        text_l(x0, top + S(72) + S(16), date, FONT_MEDIUM, S(14), th.sub);
    } else {
        font_draw(FONT_DISPLAY, S(44), x0 - S(2), top + font_ascent(FONT_DISPLAY, S(44)), "BoltOS Mini", -1, th.text, 255);
        text_l(x0, top + S(44) + S(18), date, FONT_MEDIUM, S(14), th.sub);
    }
    draw_facts(scr_w / 2 + S(40), top + S(10));

    int tx, ty, tw, tht;
    tile_rect(0, &tx, &ty, &tw, &tht);
    text_l(x0, ty - S(18), "Applications", FONT_SEMIBOLD, S(12), th.faint);
    for (int i = 0; i < NAPPS; i++) {
        tile_rect(i, &tx, &ty, &tw, &tht);
        int on = i == sel;
        gfx_rrect(tx, ty, tw, tht, S(6), on ? th.accent : th.line, 255);
        gfx_rrect(tx + 1, ty + 1, tw - 2, tht - 2, S(5), on ? th.sel : th.surface, 255);
        ui_glyph(apps[i]->glyph, tx + S(30), ty + S(30), S(28), on ? th.accent : th.sub);
        char n[2] = { (char)('1' + i), 0 };
        text_r(tx + tw - S(12), ty + S(18), n, FONT_MONO, S(11), th.faint);
        text_fit_l(tx + S(16), ty + tht - S(20), apps[i]->name, tw - S(24), FONT_MEDIUM, S(13), th.text);
    }
    char hint[96];
    snprintf(hint, sizeof hint, TXT_LARROW " " TXT_RARROW "  choose     Return  open     1-%d  jump     Esc  back to Home", NAPPS);
    text_l(x0, scr_h - S(20), hint, FONT_REGULAR, S(11), th.faint);
}

/* ---- an app's frame ---------------------------------------------------------------------------- */
static void app_area(int *x, int *y, int *w, int *h) {
    *x = 0; *y = BAR_H + 1; *w = scr_w; *h = scr_h - BAR_H - 1;
}

static void draw_app(void) {
    int ax, ay, aw, ah;
    app_area(&ax, &ay, &aw, &ah);
    gfx_clip_push(ax, ay, aw, ah);
    cur->draw(ax, ay, aw, ah);
    gfx_clip_pop();
}

/* ---- input ---------------------------------------------------------------------------------------- */
/* Raw input (a Linux program's keyboard and mouse) belongs to a full-screen
 * app: leaving it, for Home or another app, turns it off. */
void shell_go_home(void) { cur = 0; fullscreen = 0; hold_quiet = hold_awake = 0; input_raw_enable(0); dirty = 1; }

void shell_fullscreen(int on) {
    if (on == fullscreen) return;
    fullscreen = on && cur;
    if (fullscreen) screen_pointer(pointer_x, pointer_y, 0);
    else { input_raw_enable(0); hold_quiet = hold_awake = 0; }
    dirty = 1;
}
int shell_is_fullscreen(void) { return fullscreen; }

/* The full-screen app: quiet, nothing in the background (no package or update
 * checks, no Code or Notes work; main.c and the frame below skip them), and
 * awake, the screen kept on whatever Settings says. */
void shell_quiet_mode(int quiet, int awake) {
    hold_quiet = quiet && fullscreen && cur;
    hold_awake = awake && fullscreen && cur;
    if (hold_awake && asleep) screen_wake("a page that keeps the screen on");
}
int shell_quiet(void) { return hold_quiet && fullscreen && cur; }

/* An installed package's app joins the bar and Home, after the built-in ones. */
int shell_add_app(const app_t *a) {
    for (int i = 0; i < NAPPS; i++) if (apps[i] == a) return 0;
    if (NAPPS >= MAX_APPS) return -1;
    apps[napps++] = a;
    dirty = 1;
    return 0;
}

void shell_remove_app(const app_t *a) {
    for (int i = 0; i < NAPPS; i++) {
        if (apps[i] != a) continue;
        if (cur == a) cur = 0;
        for (int k = i; k + 1 < NAPPS; k++) apps[k] = apps[k + 1];
        napps--;
        if (sel >= NAPPS) sel = NAPPS - 1;
        dirty = 1;
        return;
    }
}

const app_t *shell_current(void) { return cur; }
void shell_open(const app_t *a) {
    cur = a; dirty = 1; fullscreen = 0; hold_quiet = hold_awake = 0; input_raw_enable(0);
    for (int i = 0; i < NAPPS; i++) if (apps[i] == a) sel = i;
    if (a->open) a->open();
}

static void home_key(const input_ev_t *e) {
    switch (e->key) {
    case KEY_LEFT:  sel = (sel + NAPPS - 1) % NAPPS; break;
    case KEY_RIGHT: case KEY_TAB: sel = (sel + 1) % NAPPS; break;
    case KEY_ENTER: shell_open(apps[sel]); break;
    case KEY_CHAR:
        if (e->ch >= '1' && e->ch < '1' + NAPPS) { sel = e->ch - '1'; shell_open(apps[sel]); }
        else if (e->ch == ' ') shell_open(apps[sel]);
        break;
    }
}

static void handle(const input_ev_t *e) {
    if (fullscreen && cur) {
        if (e->type == EV_POINTER) {
            pointer_x = e->x; pointer_y = e->y; pointer_btn = e->buttons;
            if (cur->pointer) cur->pointer(e, 0, 0, scr_w, scr_h);
            return;
        }
        if (e->key == KEY_SUPER) { shell_go_home(); return; }
        cur->key(e);
        return;
    }
    if (e->type == EV_POINTER) {
        int moved = e->x != pointer_x || e->y != pointer_y;
        int click = (e->buttons & 1) && !(pointer_btn & 1);
        pointer_x = e->x; pointer_y = e->y; pointer_btn = e->buttons;
        if (moved || e->source != SRC_USB) screen_pointer(e->x, e->y, e->source == SRC_USB);
        if (e->y < BAR_H) {                           /* the bar: Home and the apps */
            if (!click) return;
            int x, w = bar_item(-1, &x);
            if (ui_hit(e->x, e->y, x, 0, w, BAR_H)) { shell_go_home(); return; }
            if (upd_w && ui_hit(e->x, e->y, upd_x, 0, upd_w, BAR_H)) { settings_show_updates(); return; }
            for (int i = 0; i < NAPPS; i++) {
                w = bar_item(i, &x);
                if (ui_hit(e->x, e->y, x, 0, w, BAR_H)) { if (cur != apps[i]) shell_open(apps[i]); return; }
            }
            return;
        }
        if (!cur) {
            for (int i = 0; i < NAPPS; i++) {
                int x, y, w, h; tile_rect(i, &x, &y, &w, &h);
                if (ui_hit(e->x, e->y, x, y, w, h)) {
                    if (sel != i) { sel = i; dirty = 1; }
                    if (click) shell_open(apps[i]);
                }
            }
        } else {
            int ax, ay, aw, ah; app_area(&ax, &ay, &aw, &ah);
            int acted = click || e->wheel || e->buttons;
            if (cur->pointer) { cur->pointer(e, ax, ay, aw, ah); if (acted) dirty = 1; }
        }
        return;
    }
    /* Keys every screen shares. */
    /* Esc goes home -- except in the Terminal with a half-typed line, where
     * it clears the line first (the Terminal says so by returning 1). */
    int term_ate = 0;
    if (e->key == KEY_ESC && cur == &app_terminal) { int terminal_esc(void); term_ate = terminal_esc(); }
    if (e->key == KEY_ESC && cur == &app_browser) { int browser_esc(void); term_ate = browser_esc(); }
    if (e->key == KEY_ESC && cur == &app_settings) { int settings_esc(void); term_ate = settings_esc(); }
    if (e->key == KEY_ESC && cur && cur->esc) term_ate = cur->esc();
    if (term_ate) { dirty = 1; return; }
    if (e->key == KEY_SUPER || (e->key == KEY_ESC && cur)) {
        if (cur) shell_go_home();
        return;
    }
    /* Code has F2 (rename), F3 (find again) and F5 (run) for itself */
    int code_key = cur == &app_code && (e->key == KEY_F2 || e->key == KEY_F3 || e->key == KEY_F5);
    int browser_wants_key(int key);
    if (cur == &app_browser && browser_wants_key(e->key)) code_key = 1;
    if (!code_key && e->key >= KEY_F1 && e->key < KEY_F1 + NAPPS) { shell_open(apps[e->key - KEY_F1]); return; }
    if (cur) cur->key(e); else home_key(e);
    dirty = 1;
}

/* ---- the frame ------------------------------------------------------------------------------------ */
static int last_minute = -1, last_mac = -1, last_kb = -1, last_load = -1;

void shell_request_frame(void) { dirty = 1; }

void shell_init(void) {
    theme_set(0, 0);
    load_t0 = now_us();
    kprintf("[shell] home: %d apps\n", NAPPS);
}

static void draw_frame(void) {
    gfx_bind(&g_back, 0, 0, 0, 0, scr_w, scr_h);
    gfx_fill(0, 0, scr_w, scr_h, th.bg);
    draw_bar();
    if (cur) draw_app(); else draw_home();
    draw_toast();
}

/* Development (/bench): draw the current screen n times, as if every frame
 * had changed, and say what one costs. */
int shell_bench(int n, char *out, int cap) {
    uint64_t draw = 0, pres = 0, mx = 0;
    for (int i = 0; i < n; i++) {
        uint64_t t0 = now_us();
        draw_frame();
        uint64_t t1 = now_us();
        screen_present(0, 0, scr_w, scr_h);
        uint64_t t2 = now_us();
        draw += t1 - t0; pres += t2 - t1;
        if (t1 - t0 > mx) mx = t1 - t0;
    }
    if (n < 1) n = 1;
    return snprintf(out, (size_t)cap, "%s: draw avg %lu us, max %lu us, present %lu us (%d frames)\n",
                    cur ? cur->name : "Home", (unsigned long)(draw / (uint64_t)n), (unsigned long)mx,
                    (unsigned long)(pres / (uint64_t)n), n);
}

/* Development (/anim): for `ms`, change the screen every frame -- mode 0
 * flips between Home and each app (the whole screen), mode 1 moves Home's
 * selection (a few tiles) -- to measure what reaches a viewer. */
static uint64_t anim_until;
static int anim_mode, anim_step;
void shell_anim(int mode, int ms) { anim_mode = mode; anim_until = now_ms() + (uint64_t)ms; anim_step = 0; dirty = 1; }

int vnc_streaming(void);
void mini_pump(void);

static int was_splash;                  /* the turn before drew the splash */

int shell_frame(void) {
    /* While the splash is up the screen is the splash: input skips it, the
     * machine carries on coming up behind it, and the desktop is drawn on
     * the turn after it ends. */
    if (splash_active()) {
        input_ev_t e;
        while (input_pop(&e)) splash_dismiss();
        display_poll();
        uint64_t now = now_ms();
        if (now - last_frame < 16) return 0;
        last_frame = now;
        dirty = 0;
        splash_draw();
        screen_present(0, 0, scr_w, scr_h);
        g_perf.frames++;
        was_splash = 1;
        return 1;
    }
    if (was_splash) { was_splash = 0; dirty = 1; }
    input_ev_t e;
    while (input_pop(&e)) {
        last_input = now_ms();
        if (asleep) { screen_wake(e.type == EV_KEY ? "a key" : "the pointer"); continue; }   /* waking is all it does */
        handle(&e);
    }
    display_poll();                                    /* a monitor plugged in or out */
    screen_sleep_poll();                               /* nobody looking: go dark */
    if (fullscreen && cur) {                           /* the app draws and presents itself */
        if (cur->tick) cur->tick();
        if (shell_quiet()) return 0;                   /* nothing else, as it asked */
        int code_background(void), notes_background(void);
        code_background();
        notes_background();
        return 0;
    }
    screen_pointer_poll();                             /* an arrow move waiting for the display */
    uint64_t now = now_ms();
    if (anim_until && now - last_frame >= 16) {
        if (now >= anim_until) { anim_until = 0; cur = 0; }
        else if (anim_mode == 0) { anim_step++; cur = (anim_step % 2) ? apps[(anim_step / 2) % NAPPS] : 0; }
        else { cur = 0; sel = (sel + 1) % NAPPS; }
        dirty = 1;
    }
    load_sample();
    int minute = (int)(clock_unix() / 60);
    if (minute != last_minute) { last_minute = minute; dirty = 1; }
    int mac = shell_mac_connected(), kb = usbh_keyboards(), load = shell_cpu_load();
    if (mac != last_mac || kb != last_kb || load != last_load) { last_mac = mac; last_kb = kb; last_load = load; dirty = 1; }
    if (cur && cur->tick && cur->tick()) dirty = 1;
    int code_background(void), notes_background(void);
    code_background();                                 /* a program run from Code keeps printing */
    notes_background();                                /* and Notes saves what was typed last   */
    if (toast_at && now - toast_at >= TOAST_MS) { toast_at = 0; dirty = 1; }
    if (!dirty || now - last_frame < 16) return 0;
    /* Nobody can see it: keep `dirty`, so the frame after it lights up again is drawn whole. */
    if (screen_is_dark()) return 0;
    /* A viewer still taking the last frame can't be sent this one yet: drawing
     * it now would only keep the loop from feeding the network. */
    if (vnc_streaming() && now - last_frame < 100) return 0;
    dirty = 0;
    last_frame = now;

    uint64_t t0 = now_us();
    draw_frame();
    uint64_t t1 = now_us();
    mini_pump();                                       /* the network, between the two halves */
    uint64_t t2 = now_us();
    screen_present(0, 0, scr_w, scr_h);
    g_perf.frames++;
    g_perf.draw_us += t1 - t0;
    if (t1 - t0 > g_perf.draw_max) g_perf.draw_max = t1 - t0;
    g_perf.present_us += now_us() - t2;
    return 1;
}
