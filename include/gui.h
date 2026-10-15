#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS GUI  -  a compositing window manager over the framebuffer.
 *
 *  Apps are windows: they register a draw callback and optional input
 *  callbacks, and the manager handles framing, focus, dragging, snapping,
 *  minimise/maximise/close, the dock, the tray and the pointer. Each window
 *  is drawn into a surface of its own and composited from there, which is
 *  what lets windows fade, scale and fly to the dock without the app being
 *  asked to draw at a size it never planned for. Only the parts of the
 *  screen that changed are composited and copied out.
 *
 *  Drawing primitives live in gfx.h (anti-aliased shapes, shadows, glass),
 *  text in font.h, icons in icons.h. The g_* calls below are the original
 *  API, kept so the apps written against it draw unchanged.
 * ===========================================================================*/

/* ---- theme -------------------------------------------------------------- *
 *  Colours are runtime now (the Settings app retints the whole desktop). The
 *  COL_* names are kept as accessors into the live theme so the ~50 existing
 *  call sites compile unchanged; they just read the current palette each use.
 *  NOTE: because these expand to a struct read they are NOT compile-time
 *  constants -- never use a COL_* in a static/global initialiser.            */
typedef struct {
    uint32_t accent, accent_dim;
    uint32_t text, text_dim;
    uint32_t panel, panel2, panel3;
    uint32_t good, warn, bad;
    /* The glass desktop's additions. Old code never reads these, new code
     * should: a hard-coded grey is a grey that is wrong in light mode. */
    int      dark;          /* 1 = dark appearance                          */
    uint32_t titlebar;      /* window title bar                             */
    uint32_t text_mute;     /* third tier of text: hints, disabled, captions */
    uint32_t sep;           /* hairline separators                          */
    uint32_t glass;         /* tint over the blur behind the shell          */
    int      glass_a;       /* how much of it, 0..255                        */
    uint32_t sel;           /* selection fill behind a highlighted row      */
} theme_t;
extern theme_t g_theme;

#define COL_ACCENT      (g_theme.accent)      /* signature accent          */
#define COL_ACCENT_DIM  (g_theme.accent_dim)
#define COL_TEXT        (g_theme.text)
#define COL_TEXT_DIM    (g_theme.text_dim)
#define COL_PANEL       (g_theme.panel)       /* window client background  */
#define COL_PANEL_2     (g_theme.panel2)      /* raised: cards, controls   */
#define COL_PANEL_3     (g_theme.panel3)      /* hover, borders            */
#define COL_GOOD        (g_theme.good)
#define COL_WARN        (g_theme.warn)
#define COL_BAD         (g_theme.bad)
#define COL_TITLEBAR    (g_theme.titlebar)
#define COL_TEXT_MUTE   (g_theme.text_mute)
#define COL_SEP         (g_theme.sep)
#define COL_SEL         (g_theme.sel)
#define THEME_DARK      (g_theme.dark)

/* ---- graphics primitives (operate on the compositor backbuffer) -------- */
void g_fill (int x, int y, int w, int h, uint32_t color);
void g_blend(int x, int y, int w, int h, uint32_t color, uint8_t alpha);   /* alpha 0..255 */
void g_rect (int x, int y, int w, int h, uint32_t color);                  /* 1px outline   */
void g_hline(int x, int y, int w, uint32_t color);
void g_vline(int x, int y, int h, uint32_t color);
void g_round(int x, int y, int w, int h, int r, uint32_t color, uint8_t alpha); /* rounded rect */
void g_char (int x, int y, char c, uint32_t color, int scale);
void g_text (int x, int y, const char *s, uint32_t color, int scale);
int  g_text_width(const char *s, int scale);
/* Font face: 0 = Retro 8x8, 1 = the UI face (default). g_text is Inter now and
 * proportional -- measure with g_text_width, not strlen*8. g_char stays on an
 * 8 px monospace grid for the terminal and the editor. */
enum { FONT_RETRO = 0, FONT_ARIAL = 1 };
void g_set_font(int face);
int  g_get_font(void);
/* The 8x8 bitmap face whatever the setting says: Settings shows it as a sample. */
void g_text_retro(int x, int y, const char *s, uint32_t color, int scale);
int  g_font_height(int scale);   /* glyph box height for the active face */
int  g_underline_y(int scale);   /* where an underline goes, from the line top */
int  g_glyph_ink(char c);        /* 1 if the active face draws anything for c */
/* proportional (variable-width) text: glyphs are trimmed to their ink box plus a
 * 1px gap, so prose reads like a real font instead of a monospace grid. italic
 * shears each row. The UI/terminal keep the monospace g_* calls above. */
int  g_glyph_adv(char c, int scale);                                   /* advance width of one glyph */
void g_char_p(int x, int y, char c, uint32_t color, int scale, int italic);
int  g_text_width_pn(const char *s, int len, int scale);
int  g_text_pn(int x, int y, const char *s, int len, uint32_t color, int scale, int italic); /* -> end x */
void g_blit(int dx, int dy, int dw, int dh, const uint32_t *src, int sw, int sh); /* ARGB, scaled */
void g_set_clip(int x, int y, int w, int h);
void g_clear_clip(void);
void gui_icon(int id, int x, int y, int scale, uint32_t color);   /* ICON_* at 16*scale px */

/* ---- windows ----------------------------------------------------------- */
typedef struct window window_t;
struct window {
    char     title[40];
    int      x, y, w, h;            /* outer rectangle (includes title bar)   */
    int      min_w, min_h;
    int      open;                  /* registered + visible                   */
    int      pinned;                /* kept on the taskbar even when closed   */
    int      minimized, maximized;
    int      snapped;               /* SNAP_* zone, 0 = floating; restores on drag */
    int      desk;                  /* which virtual desktop it lives on (0-based) */
    int      rx, ry, rw, rh;        /* saved rect for un-maximise / un-snap   */
    int      z;                     /* stacking order (higher = nearer front) */
    uint32_t accent;                /* per-app accent / icon colour           */
    int      icon;                  /* taskbar/title icon id (see ICON_*)      */
    /* client area is (x, y+TITLE_H, w, h-TITLE_H); callbacks get it as cx..ch */
    void (*draw) (window_t *, int cx, int cy, int cw, int ch);
    void (*key)  (window_t *, char c);
    void (*click) (window_t *, int lx, int ly);  /* client-local left click   */
    void (*rclick)(window_t *, int lx, int ly);  /* client-local right click  */
    void (*drag) (window_t *, int lx, int ly);   /* held-button motion over client (client-local) */
    void (*scroll)(window_t *, int delta);       /* mouse wheel over client: +up / -down */
    void (*tick) (window_t *);                    /* periodic update (~6 Hz)   */
    void  *st;                       /* app private state                      */
    /* Redraw on every pointer move over the client, for apps that light up
     * what is under the mouse. Off by default: each window is rendered into
     * its own surface now and only redrawn when something asks, so an app
     * that wants hover has to say so. */
    int    track_hover;
    /* Set by an app whose tick only sometimes changes what it shows: the
     * window is then redrawn when the app says so (gui_redraw_window), not
     * after every tick as a matter of course. */
    int    lazy_tick;
};

/* Tiling zones. A window's `snapped` holds one of these (SNAP_NONE when it is
 * floating); SNAP_MAX is the odd one out and lives in `maximized` instead,
 * because the maximise button and the double-click predate any of this. */
enum { SNAP_NONE = 0, SNAP_L, SNAP_R, SNAP_TL, SNAP_TR, SNAP_BL, SNAP_BR, SNAP_MAX };

/* The order is the order of the icon atlases: mkicons.py reads this enum and
 * draws them in it, so a new icon goes at the end, with a drawing there. */
enum { ICON_NONE = 0, ICON_TERMINAL, ICON_TASKMGR, ICON_START, ICON_FILES, ICON_SETTINGS,
       ICON_BROWSER, ICON_FOLDER, ICON_FILE, ICON_TRASH, ICON_CALC, ICON_CLOCK, ICON_NOTES, ICON_CALENDAR, ICON_PIANO, ICON_PAINT, ICON_MINES, ICON_SNAKE, ICON_2048, ICON_STOPWATCH, ICON_SYSINFO, ICON_LIFE, ICON_TTT, ICON_COLOR, ICON_MEMORY, ICON_MATRIX, ICON_CODE, ICON_DOOM, ICON_OLDBROWSER, ICON_SHEETS, ICON_IMAGE, ICON_MUSIC, ICON_TASKS, ICON_CONTACTS, ICON_INSTALL, ICON_SECURITY, ICON_SOFTWARE, ICON_MEDIA, ICON_NETWORK, ICON_MIXER, ICON_DEVICE, ICON_CHARMAP, ICON_SNIP, ICON_CARDS, ICON_CLIPBOARD, ICON_STRIKE, ICON_GAMEBOY, ICON_CRAFT,
       ICON_LOGS, ICON_PC, ICON_DRIVE, ICON_USB, ICON_HOME, ICON_DESKTOPF, ICON_DOCS, ICON_DOWNLOADS,
       ICON_PICTURES, ICON_MUSICF, ICON_VIDEOS, ICON_TRASH_FULL, ICON_FILE_TEXT, ICON_FILE_CODE,
       ICON_FILE_IMAGE, ICON_FILE_AUDIO, ICON_FILE_VIDEO, ICON_FILE_ARCHIVE, ICON_FILE_EXE,
       ICON_FILE_WEB, ICON_FILE_SHEET, ICON_USER, ICON_COUNT };

window_t *gui_add_window(const char *title, int w, int h, uint32_t accent, int icon);
void      gui_open(window_t *win);          /* show + focus + raise           */
void      gui_focus(window_t *win);
void      gui_show_unfocused(window_t *win);  /* show it; the keyboard stays put */
void      gui_request_redraw(void);        /* every visible window redraws  */
void      gui_redraw_window(window_t *win);/* just this one: much cheaper   */
int       gui_mouse_x(void);               /* pointer in desktop coordinates */
int       gui_mouse_y(void);
int       gui_cpu_load(void);               /* 0..100, real busy/idle fraction*/
/* Frames composited since the last call and how long they took, in tenths of
 * a millisecond. Reading resets the counters; `display` prints them. */
void      gui_frame_stats(int *frames, int *last_ms10, int *avg_ms10, int *max_ms10);
/* The slowest frame's time by stage (render windows, desktop + windows,
 * shell, copy out) and how many damage rectangles it had. Resets. */
void      gui_frame_stages(int ms10[4], int *ndmg_worst);
void      gui_bench(void);                  /* `display bench`: time the primitives */
void      gui_motion_apply(void);           /* g_settings.motion -> live; cheap      */
int       gui_motion_reduced(void);         /* 1 if animations are off right now     */
int       gui_frame_copy_us(void);          /* boot calibration: one 1024x768 copy   */
int       gui_blend_us(void);               /* ...and one 128x128 alpha fill         */
int       gui_screen_w(void);               /* logical desktop width           */
int       gui_screen_h(void);               /* logical desktop height          */
int       gui_panel_w(void);                /* physical panel width (fixed)    */
int       gui_panel_h(void);                /* physical panel height (fixed)   */
int       gui_window_focused(window_t *win);
void      gui_run(void);                    /* enter the desktop; never returns*/
void      gui_pump(void);                   /* force one composite+blit (re-entrant)*/
void      gui_logout(void);                 /* return to the sign-in screen */

/* ---- virtual desktops --------------------------------------------------- *
 *  Four of them. A window belongs to the desktop it was opened on and is
 *  invisible (and unclickable) from the others; closed-but-pinned taskbar
 *  launchers appear on all of them. Win+1..4 switches, Ctrl+Alt+Left/Right
 *  walks, Win+Shift+Left/Right takes the focused window along, and the pips at
 *  the left of the taskbar do the same with the mouse. `desk` is the shell
 *  front end, so this works over SSH too.                                     */
int  gui_desk_count(void);
int  gui_desk_current(void);
int  gui_desk_goto(int d);                            /* 0, or -1 out of range */
int  gui_desk_send(int d);                            /* move the focused window */
int  gui_desk_windows(int d, char *out, uint32_t cap); /* count; names into out */

/* ---- tiling -------------------------------------------------------------- *
 *  Halves, quarters and maximise. The keyboard (Win+arrow) and a drag to an
 *  edge are how anyone actually tiles a window; these exist so `snap` can do it
 *  from a prompt -- over SSH there is no Win key to press, and a test that
 *  synthesises one is mostly testing the keyboard driver.                      */
int  gui_snap_focused(int zone);        /* SNAP_*; -1 if nothing is focused    */
int  gui_focused_rect(char *title, uint32_t cap,
                      int *x, int *y, int *w, int *h, int *zone);

/* Lock the running session behind a password prompt. The desktop is left
 * exactly as it was; only the current user's password gets back in. Armed by
 * Ctrl+Alt+L, by the idle timer, and by the `lock` shell command. */
void      gui_lock(void);
int       gui_locked(void);
void      gui_set_idle_lock(int minutes);   /* 0 disables the idle auto-lock */
int       gui_idle_lock(void);

/* Re-read the live settings: relayout the logical desktop / letterbox, retint
 * the theme and re-render the wallpaper. Called by the Settings app on change. */
void      gui_apply_display(void);

/* ---- desktop icons + drag-and-drop ------------------------------------- *
 *  The desktop hosts a layer of shortcut icons (folders/files/trash). Apps may
 *  drop an item onto the desktop by arming a drag while handling a click; the
 *  compositor follows the cursor and, on release over the wallpaper, creates a
 *  shortcut. Double-clicking a desktop icon opens it via files_open_node().    */
struct fs_node;
void gui_desktop_add(struct fs_node *node, const char *label, int icon); /* seed a shortcut */
void gui_desktop_add_app(window_t *win, const char *label, int icon);    /* seed an app launcher */
void gui_begin_item_drag(struct fs_node *node, const char *label, int icon); /* arm a drag */

/* ---- apps -------------------------------------------------------------- */
void terminal_app_init(void);
void oldbrowser_app_init(void);
void taskmgr_app_init(void);
void settings_app_init(void);
void installer_app_init(void);
void software_app_init(void);
void logs_app_init(void);
void security_app_init(void);
void network_app_init(void);
void network_app_open(void);   /* from the tray's network button */
void mixer_app_init(void);
void mixer_app_open(void);     /* from the tray's speaker button */
void browser_app_init(void);
void files_app_init(void);
void ide_app_init(void);
void calc_app_init(void);
void clock_app_init(void);
void notes_app_init(void);
void tasks_app_init(void);
void contacts_app_init(void);
void notes_open_path(const char *path);   /* load `path` into Notepad + show it */
void sheets_app_init(void);
void sheets_open_path(const char *path);  /* load `path` into Sheets + show it  */
void imgview_app_init(void);
void imgview_open_path(const char *path); /* decode + show an image in Photos    */
void music_app_init(void);
void music_halt(void);                    /* give the codec back to whoever asked */

/* ---- Media player -------------------------------------------------------- *
 *  The window is the smaller half of this. Playback is fed from the desktop's
 *  main loop (media_pump), not from the window's tick, so a track keeps playing
 *  while its window is minimised or on another desktop -- and so the gap
 *  between one buffer and the next is a millisecond rather than a tick. The
 *  `media` command is the same transport from a prompt, for a session over SSH
 *  and for a test that would otherwise have to synthesise clicks on a seek bar
 *  to check anything.                                                          */
void media_app_init(void);
void media_open_path(const char *path);   /* load, show and play an audio file  */
void media_open_path_bg(const char *path);/* the same, without taking the keyboard */
void media_pump(void);                    /* keep the codec fed (desktop loop)  */
void media_halt(void);                    /* stop; release the codec            */
void media_toggle(void);                  /* play / pause                       */
void media_stop(void);
int  media_skip(int delta);               /* +1 next, -1 previous; -1 if no list */
int  media_select(int i);                 /* play playlist entry i              */
void media_seek_ms(uint32_t ms);
uint32_t media_position_ms(void);         /* what is audible now, not decoded   */
uint32_t media_duration_ms(void);
int  media_playing(void);
int  media_loaded(void);
const char *media_track(void);            /* file name, or "" */
const char *media_format(void);           /* "MPEG-1 Layer III, 44.1 kHz ...", or "" */
const char *media_status(void);           /* why it will not play, or "" */
const char *media_folder(void);           /* the folder the playlist came from */
int  media_repeat(void);
void media_set_repeat(int on);
int  media_index(void);                   /* playlist position, or -1 */
int  media_list(int i, const char **name);/* count; the i'th name via *name */
void gui_set_wallpaper_image(const char *path); /* use an image as the wallpaper  */
const char *gui_wallpaper_path(void);           /* the image in use, "" for none   */
uint32_t   *gui_wallpaper_px(void);             /* the wallpaper as drawn, W*H     */
int         gui_wallpaper_version(void);        /* changes whenever it does        */
void gui_clear_wallpaper_image(void);           /* revert to the procedural style */
/* Post a notification: a toast bottom-right now, and a row in the notification
 * centre afterwards. gui_toast() is the plain-information case. The levels and
 * the store itself are include/notify.h -- code with nothing to do with the
 * desktop can post too, and does. */
void gui_toast(const char *msg);                /* NOTE_INFO from "System"        */
void gui_notify(int level, const char *source, const char *text);
/* Modal text prompt; `cb` gets the entered string on OK/Enter (not on Cancel). */
void gui_prompt(const char *title, const char *initial, void (*cb)(const char *));
void calendar_app_init(void);
void piano_app_init(void);
void paint_app_init(void);
void mines_app_init(void);
void snake_app_init(void);
void g2048_app_init(void);
void stopwatch_app_init(void);
void devmgr_app_init(void);
void charmap_app_init(void);
void snip_app_init(void);
void solitaire_app_init(void);
void clipboard_app_init(void);
void clipboard_app_open(void);   /* Win+V: show the history and take the keyboard */
void sysinfo_app_init(void);
void life_app_init(void);
void ttt_app_init(void);
void colorpick_app_init(void);
void memory_app_init(void);
void matrix_app_init(void);
void doom_app_init(void);
void doom_pump(void);    /* advance DOOM one frame slice; called from the desktop loop */
void strike_app_init(void);
void strike_pump(void);  /* BoltStrike: one frame slice, same deal as DOOM */
void gameboy_app_init(void);
void gameboy_pump(void); /* the Game Boy emulator, likewise */
void boltcraft_app_init(void); /* a launcher for the ring-3 voxel sandbox in /bin */
void files_open_node(struct fs_node *n);   /* open a folder in the explorer / a file in its viewer */
void files_launch(struct fs_node *n);      /* the same, but honouring file associations */
