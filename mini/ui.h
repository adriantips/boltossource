#pragma once
#include <stdint.h>
#include "mini.h"
#include "gfx.h"
#include "font.h"

/* ===========================================================================
 *  BoltOS Mini  -  mini/ui.h
 *
 *  The shell and its six apps. Every dimension is written for a 600-line
 *  screen and goes through S(), so a 480-line LCD and a 720-line VNC window
 *  get the same layout, smaller or larger.
 *
 *  The look is the BoltOS glass, kept quiet: a soft wallpaper, a large thin
 *  clock, one row of apps, and apps that take the whole screen with a slim
 *  title and nothing else around them.
 * ===========================================================================*/
#define S(v) ((int)(((long)(v) * scr_h + 300) / 600))

typedef struct app {
    const char *name;
    uint32_t    tint;                  /* the tile's colour                     */
    int         glyph;                 /* G_*                                   */
    void (*open)(void);
    void (*key)(const input_ev_t *e);
    void (*pointer)(const input_ev_t *e, int x, int y, int w, int h);
    void (*draw)(int x, int y, int w, int h);
    /* Called every loop while the app is open: 1 asks for another frame
     * (a clock that ticks, a stopwatch). May be NULL. */
    int  (*tick)(void);
} app_t;

extern const app_t app_browser, app_terminal, app_monitor, app_notes, app_clock, app_settings;

enum { G_TERMINAL, G_MONITOR, G_NOTES, G_CLOCK, G_SETTINGS, G_MAC, G_KEYBOARD,
       G_POWER, G_CHECK, G_CHEVRON, G_CLOSE, G_DOT, G_GLOBE, G_WIFI, G_BLUETOOTH, G_LOCK };

/* ---- the theme ---------------------------------------------------------------- */
typedef struct {
    int      wallpaper;               /* 0 Dusk, 1 Dawn, 2 Midnight             */
    int      accent_i;
    uint32_t accent;
    uint32_t text, sub, faint;        /* primary, secondary, tertiary text      */
    uint32_t panel;                   /* glass tint                             */
    int      panel_a;
    int      dark;
} theme_t;
extern theme_t th;
extern const char *const wallpaper_names[3];
extern const char *const accent_names[6];
void theme_set(int wallpaper, int accent);

/* ---- drawing helpers ------------------------------------------------------------ */
/* A glass card: the blurred wallpaper under it, tinted, with a hairline rim.
 * `id` names the card so its blur is captured once, not every frame. */
void ui_card(int id, int x, int y, int w, int h, int r);
void ui_glyph(int id, int cx, int cy, int size, uint32_t color);
void ui_tile(const app_t *a, int x, int y, int size, int lift);
/* A row in a list: highlighted when `sel`. Returns its height. */
int  ui_row(int x, int y, int w, const char *label, const char *value, int sel);
void ui_keycap(int x, int cy, const char *label);   /* a small "esc" chip     */
int  ui_hit(int px, int py, int x, int y, int w, int h);

/* ---- the shell, for apps ---------------------------------------------------------- */
void shell_go_home(void);
void shell_open(const app_t *a);
void shell_request_frame(void);
int  shell_cpu_load(void);                          /* 0..100, last second    */
int  shell_cpu_history(uint8_t *out, int n);        /* oldest first           */
int  shell_mac_connected(void);
extern uint64_t g_idle_us;
void clock_text(char *hm, int hm_cap, char *date, int date_cap);
