#pragma once
#include <stdint.h>
#include "mini.h"
#include "gfx.h"
#include "font.h"

/* ===========================================================================
 *  BoltOS Mini  -  mini/ui.h
 *
 *  The shell and its apps: seven built in, and what Packages installs. Every dimension is written for a 600-line
 *  screen and goes through S(), so a 480-line LCD and a 720-line VNC window
 *  get the same layout, smaller or larger.
 *
 *  The look is flat: solid colours, hairline borders, small radii, a bar
 *  along the top, and apps that take everything under it.
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
    /* Esc: 1 if the app used it (stopped something, cleared a line), 0 to
     * go Home as usual. May be NULL. */
    int  (*esc)(void);
} app_t;

extern const app_t app_browser, app_code, app_terminal, app_monitor, app_notes, app_clock, app_settings, app_packages;

enum { G_TERMINAL, G_MONITOR, G_NOTES, G_CLOCK, G_SETTINGS, G_MAC, G_KEYBOARD,
       G_POWER, G_CHECK, G_CHEVRON, G_CLOSE, G_DOT, G_GLOBE, G_WIFI, G_BLUETOOTH, G_LOCK,
       G_PACKAGE, G_CHAT, G_CODE, G_FOLDER, G_FILE, G_PLAY, G_STOP };

/* ---- the theme ---------------------------------------------------------------- */
typedef struct {
    int      wallpaper;               /* the theme: 0 Dark, 1 Light, 2 Black    */
    int      accent_i;
    uint32_t accent;
    uint32_t bg, bar, surface, line;  /* page, top bar, panels, hairlines       */
    uint32_t sel;                     /* a selected row: the accent, faintly    */
    uint32_t text, sub, faint;        /* primary, secondary, tertiary text      */
    uint32_t panel;                   /* = surface (older name)                 */
    int      panel_a;
    int      dark;
} theme_t;
extern theme_t th;
extern const char *const wallpaper_names[3];
extern const char *const accent_names[6];
void theme_set(int wallpaper, int accent);

/* ---- drawing helpers ------------------------------------------------------------ */
/* A panel: the surface colour inside a hairline. `id` is unused (it named
 * the old glass cards); r is capped at a few pixels. */
void ui_card(int id, int x, int y, int w, int h, int r);
void ui_glyph(int id, int cx, int cy, int size, uint32_t color);
void ui_tile(const app_t *a, int x, int y, int size, int on);
/* A row in a list: highlighted when `sel`. Returns its height. */
int  ui_row(int x, int y, int w, const char *label, const char *value, int sel);
void ui_keycap(int x, int cy, const char *label);   /* a small "esc" chip     */
int  ui_hit(int px, int py, int x, int y, int w, int h);

/* ---- the shell, for apps ---------------------------------------------------------- */
void shell_go_home(void);
void shell_open(const app_t *a);
int  shell_add_app(const app_t *a);                 /* an installed package's   */
void shell_remove_app(const app_t *a);
const app_t *shell_current(void);                   /* NULL on Home             */
void shell_request_frame(void);
int  shell_cpu_load(void);                          /* 0..100, last second    */
int  shell_cpu_history(uint8_t *out, int n);        /* oldest first           */
int  shell_mac_connected(void);
extern uint64_t g_idle_us;
void clock_text(char *hm, int hm_cap, char *date, int date_cap);
void shell_settings_save(void);                     /* theme, accent, time zone */
/* The clipboard, shared by the apps and the Mac's (through Screen Sharing). */
void clip_set(const char *s, int n);
const char *clip_get(int *n);
void clip_from_viewer(const char *s, int n);
uint32_t clip_gen(void);
void shell_settings_restore(void);
