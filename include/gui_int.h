#pragma once
#include <stdint.h>
#include "gui.h"
#include "gfx.h"
#include "font.h"
#include "icons.h"

/* ===========================================================================
 *  BoltOS  -  include/gui_int.h
 *
 *  The desktop's insides, shared by the three files it is split across and
 *  by nothing else -- apps talk to include/gui.h.
 *
 *    kernel/gui.c        compositor, windows, input, the main loop
 *    kernel/gui_shell.c  everything drawn around the windows: dock, tray,
 *                        Start, flyouts, menus, toasts, desktop icons
 *    kernel/gui_login.c  sign-in, lock screen, the splash before them
 * ===========================================================================*/

#define TITLE_H     32
#define WIN_R       10            /* window corner radius                       */
#define MAX_WIN     48
#define NDESK       4
#define ISLAND_H    56            /* the dock and its neighbours                */
#define ISLAND_GAP  6             /* between them and the bottom of the screen  */
#define SHELF_H     (ISLAND_H + 2 * ISLAND_GAP)   /* what maximise leaves free  */

/* Layers, bottom to top. Damage is tagged with the layer that caused it, so a
 * glass panel knows whether what is under it changed (re-blur) or only
 * something on top of it did (redraw from the cached blur). */
enum { L_DESK = 0, L_WIN, L_SHELL, L_POPUP, L_TOAST, L_MODAL, L_NLAYERS };

/* ---- window manager state (gui.c) ----------------------------------------- */
extern window_t wm_wins[MAX_WIN];
extern int      wm_nwin;
extern int      wm_focus;          /* index, -1 = none                         */
extern int      wm_desk;           /* current virtual desktop                  */
extern int      wm_ztop;
extern int      wm_drag;           /* window being moved by its title, or -1   */

int  wm_on_desk(const window_t *w);
int  wm_topmost(void);
int  wm_index(const window_t *w);
void wm_desk_goto(int d);
void wm_close(int i);
void wm_minimize(int i);
void wm_toggle_from_dock(int i);
void wm_snap(window_t *w, int zone);          /* SNAP_*; SNAP_NONE restores  */
void wm_maximize_toggle(window_t *w);
int  wm_visual(int i, rect_t *r, int *alpha); /* where window i is drawn now; 0 = not drawn */
const surface_t *wm_surface(int i);           /* its rendered surface, or 0  */

/* ---- damage --------------------------------------------------------------- */
void gui_damage(int x, int y, int w, int h, int layer);
void gui_damage_all(void);
void gui_window_stale(int i);                 /* re-render window i next frame */
void gui_all_stale(void);
void gui_animating(void);                     /* keep frames coming this tick  */
uint64_t gui_now(void);                       /* milliseconds                  */

/* ---- the screen ----------------------------------------------------------- */
int  gui_mx(void);                            /* pointer, desktop coords       */
int  gui_my(void);
uint32_t *gui_wallpaper_px(void);             /* W*H, or 0                     */
int  gui_wallpaper_version(void);             /* bumps on every change         */
int  gui_wallpaper_is_image(void);
const char *gui_wallpaper_path(void);
void gui_set_wallpaper_style(void);           /* re-render after a settings change */
int  gui_snap_hint(void);                     /* SNAP_* the drag is over       */
void gui_snap_rect(int zone, rect_t *r);      /* where that zone would put it  */
int  gui_damage_under(int x, int y, int w, int h, int layer);
surface_t *gui_bb(void);                      /* the backbuffer, for login      */
void gui_present_all(void);
void gui_present_rect(int x, int y, int w, int h);
void gui_cursor_show(int on);
void gui_cursor_update(void);
extern uint64_t gui_last_input_tick;          /* idle lock; kept by gui_login.c */
void gui_screenshot_now(void);
void gui_record_toggle_now(void);

/* ---- easing --------------------------------------------------------------- */
static inline float ease_out(float t)   { t = 1.0f - t; return 1.0f - t * t * t; }
static inline float ease_in(float t)    { return t * t * t; }
static inline float ease_inout(float t) {
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) / 2.0f;
}
/* A spring that overshoots once and settles: dock bounces, toggles. */
static inline float ease_back(float t)  {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}
static inline float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
/* Reduce motion. With it on, every animation is over the moment it starts:
 * a window appears where it is going, a panel opens already opaque. Auto
 * turns it on for a machine that copies a frame too slowly for a fade to be
 * anything but a slideshow -- QEMU without acceleration, mostly. */
extern int gui_motion_off;
static inline int motion_ms(int ms) { return gui_motion_off ? 0 : ms; }

/* progress of an animation that started at t0 and lasts dur ms, 0..1.
 * One that has not started yet is at 0 even with motion off: a toast's
 * fade-out is timed this way, and it must not fade out on arrival. */
static inline float anim_t(uint64_t now, uint64_t t0, int dur) {
    if (now < t0) return 0.0f;
    if (dur <= 0 || gui_motion_off || now >= t0 + (uint64_t)dur) return 1.0f;
    return (float)(now - t0) / (float)dur;
}

/* ---- the shell (gui_shell.c) ---------------------------------------------- */
void shell_init(void);
void shell_tick(uint64_t now);                 /* ~6 Hz: clock, tray, carets   */
int  shell_update(uint64_t now);               /* per frame; 1 while animating */
void shell_prepare(void);                      /* glass invalidation -> damage */
void shell_capture(int layer);                 /* re-blur what changed         */
void shell_draw(int layer);                    /* everything of that layer     */
void shell_draw_desktop(void);                 /* icons on the wallpaper       */
void shell_draw_snap(void);                    /* the drop preview             */
int  shell_mouse_down(int x, int y, int button);     /* 1 = the shell took it  */
int  shell_mouse_desktop(int x, int y, int button);  /* below every window     */
void shell_window_menu(int win, int x, int y);       /* right-click on a title */
void shell_mouse_move(int x, int y, int buttons);
void shell_mouse_up(int x, int y, int button);
int  shell_dragging(void);                     /* a desktop icon or a dnd item */
int  shell_wheel(int x, int y, int dz);
int  shell_key(char c);                        /* 1 = consumed                 */
int  shell_modal(void);                        /* a dialog has the keyboard    */
int  shell_cursor(int x, int y);               /* CURSOR_* override, -1 = none */
void shell_toggle_start(void);
void shell_close_popups(void);
void shell_switcher(int dir);                  /* Alt+Tab                      */
void shell_switcher_poll(void);                /* commit when Alt goes up      */
int  shell_dock_rect(int win, rect_t *r);      /* where a window minimises to  */
void shell_launched(int win);                  /* bounce its dock icon         */
void shell_theme_changed(void);
void shell_note(void);                         /* a notification arrived       */

/* Window chrome, drawn into the window's own surface. hot: which traffic
 * light the pointer is over (-1 none, 3 = over the group but no button). */
void chrome_draw(window_t *w, int focused, int hot, int maximized);
void chrome_edge(window_t *w, int maximized);       /* after the client */
int  chrome_button_at(window_t *w, int x, int y);   /* 0 close, 1 min, 2 max, -1 */
int  chrome_group_at(window_t *w, int x, int y);
void window_shadow(int x, int y, int w, int h, int r, int focused, int alpha);

/* ---- sign-in (gui_login.c) ------------------------------------------------ */
extern int login_active;
void login_begin(void);
void login_lock(void);
int  login_frame(uint64_t now);       /* draw if needed; 1 while animating */
void login_key(char c);
void login_mouse(int x, int y, int down);
int  login_transition(uint64_t now, int *alpha);  /* fading out over the desktop */
void login_draw_over(int alpha);      /* the lock screen on top, faded           */
