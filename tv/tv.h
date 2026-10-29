#pragma once
#include <stdint.h>
#include "gfx.h"
#include "font.h"

/* ===========================================================================
 *  BoltOS TV  -  tv/tv.h
 *
 *  A television, not a desktop. One full-screen canvas, no windows, no
 *  pointer unless a mouse is actually moved, and every screen reachable with
 *  four arrows, OK and Back -- which is what a remote has.
 *
 *  The canvas is drawn at a logical size (tv_w x tv_h) and copied to the
 *  panel. Every dimension in the shell is written in design pixels and passed
 *  through S(), so the same layout comes out right on a 720p panel, and a 4K
 *  panel gets the 1080p canvas doubled -- which is what the menus of every
 *  television on sale do too.
 *
 *  S() also carries the density: design pixels are drawn at 80%, so a
 *  1080p screen gets the open, unhurried spacing of a 1350-line layout.
 *  Change UI_DENSITY and every screen grows or shrinks together.
 * ===========================================================================*/

extern int tv_w, tv_h;
#define UI_DENSITY 80                      /* percent */
#define S(v)  ((int)(((long)(v) * tv_h * UI_DENSITY + 54000) / 108000))

/* ---- time ---------------------------------------------------------------- */
uint64_t tv_now_ms(void);
/* Wall clock, local == UTC (there is no timezone database on a TV yet;
 * Settings has an hour offset). */
void     tv_clock(int *h, int *m, int *s, int *wday, int *day, int *mon, int *year);
const char *tv_month_name(int mon);      /* 1..12 */
const char *tv_day_name(int wday);       /* 0 = Sunday */

/* ---- input --------------------------------------------------------------- */
enum {
    K_NONE = 0,
    K_UP, K_DOWN, K_LEFT, K_RIGHT, K_OK, K_BACK, K_HOME,
    K_CHUP, K_CHDN, K_VOLUP, K_VOLDN, K_MUTE, K_GUIDE, K_INFO,
    K_CHAR,            /* a typed character, in .ch */
    K_ERASE,           /* backspace inside a text field */
    K_TAB,
};
typedef struct { int k; int ch; } tvkey_t;

/* ---- views --------------------------------------------------------------- *
 *  The shell keeps a stack of these. Home is the bottom of it; an app pushes
 *  itself, Back pops it. `backdrop` says what goes behind the view's draw:  */
enum {
    BG_FILM = 0,       /* the ambient films, full strength (home)            */
    BG_FILM_DIM,       /* the films, darkened (apps drawn over them)          */
    BG_NONE,           /* nothing: the view paints every pixel itself         */
};
typedef struct view {
    const char *name;
    int  backdrop;
    void (*enter)(void);
    void (*leave)(void);
    void (*key)(tvkey_t k);
    /* Pointer in canvas pixels. `click` is 1 on the frame a button went down. */
    void (*pointer)(int x, int y, int click, int wheel);
    void (*draw)(uint64_t now);
    /* Called every frame before draw, for views that animate or poll. */
    void (*tick)(uint64_t now);
} view_t;

void tv_push(const view_t *v);
void tv_redraw(void);              /* compose another frame                     */
float tv_view_t(void);             /* 0..1, eased, since the top view arrived   */
void tv_text_mode(int on);         /* a text field has the keyboard            */
uint32_t tv_log_snapshot(char *out, uint32_t cap);
void tv_pop(void);
uint64_t tv_idle_ms(void);         /* since the last key                        */
void tv_update_init(void);         /* boltos.dev's updates (tv/settings.c)      */
void tv_go_home(void);
const view_t *tv_top(void);
int  tv_pointer_visible(void);

/* ---- overlays ------------------------------------------------------------ */
void tv_toast(const char *title, const char *detail);
void tv_volume_changed(void);
void tv_set_brightness(int pct);        /* 10..100 */
int  tv_brightness(void);

/* ---- the films ----------------------------------------------------------- */
typedef struct {
    const char    *key;        /* "lofoten"                                    */
    const char    *title;      /* "Lofoten Islands"                            */
    const char    *place;      /* "Reine, Norway"                              */
    const char    *channel;    /* the Live TV channel it is                     */
    const char    *blurb;      /* one line for the guide                       */
    const char    *credit;     /* "Clément Proust" (Pexels)                    */
    uint32_t       tint;       /* a colour from the film, for accents          */
    const uint8_t *m1v, *m1v_end;
    const uint8_t *jpg, *jpg_end;
} film_t;
extern const film_t tv_films[];
extern const int    tv_nfilms;

typedef struct plm_video_t plm_video_t;
typedef struct plm_buffer_t plm_buffer_t;
typedef struct {
    plm_video_t  *v;
    plm_buffer_t *buf;
    int        film;           /* -1 when closed                               */
    int        w, h;           /* frame size                                   */
    double     fps;
    uint64_t   t0;             /* ms the current run of frames started at      */
    uint64_t   shown;          /* frames decoded since t0                      */
    uint32_t  *rgb;            /* w*h, the newest frame, 0x00RRGGBB            */
    uint32_t   cap;            /* pixels rgb can hold                          */
    int        have;           /* rgb holds a frame                            */
    uint64_t   decoded;        /* every frame ever decoded, for stats          */
    uint32_t   decode_us;      /* how long the last one took                   */
} player_t;

int  player_open(player_t *p, int film);
void player_close(player_t *p);
/* Decode whatever frame is due at `now`; 1 if rgb changed. Never decodes more
 * than one frame a call: a slow machine plays slowly, it does not stall. */
int  player_tick(player_t *p, uint64_t now);
/* Draw the current frame to cover (x, y, w, h), cropping to fill. */
void player_draw(const player_t *p, int x, int y, int w, int h, int alpha);
/* Average colour of the frame, for tinting glass that sits on it. */
uint32_t player_average(const player_t *p);

/* The shell owns the films: Live TV pins one, home rotates them. */
void shell_film_lock(int film);
void shell_film_unlock(void);
int  shell_film_current(void);
const player_t *shell_player(void);

/* A decoded guide still (premultiplied), cached. */
const uint32_t *film_still(int film, int *w, int *h);

/* ---- art (tv/art.c, drawn by tv/mkart.py) --------------------------------- */
enum {
    ART_LIVE = 0, ART_APPLETV, ART_HDMI, ART_WEB, ART_SHIELD, ART_SETTINGS,
    ART_TILES,
    /* glyphs: white, tinted when drawn */
    GL_BOLT = 0, GL_CHEV_R, GL_CHEV_L, GL_CHEV_U, GL_CHEV_D, GL_VOL, GL_MUTE,
    GL_WIFI, GL_ETHER, GL_GLOBE, GL_SHIELD, GL_SHIELD_OFF, GL_SEARCH, GL_BACK,
    GL_RELOAD, GL_LOCK, GL_CLOSE, GL_POWER, GL_MOON, GL_SUN, GL_IMAGE, GL_INFO,
    GL_TV, GL_FILM, GL_CHECK, GL_PLAY, GL_UNPLUG, GL_CLOCK, GL_HOME, GL_KEYBOARD,
    GL_STAR, GL_LIST, GL_CPU, GL_RESTART,
    GL_COUNT
};
void art_init(void);
/* An app tile at `size` px square, its top-left at (x, y). */
void art_tile(int id, int x, int y, int size, int alpha);
/* A glyph centred on (cx, cy), `size` px, tinted. */
void art_glyph(int id, int cx, int cy, int size, uint32_t color, int alpha);

/* ---- drawing helpers (tv/ui.c) ------------------------------------------- */
typedef struct {
    backdrop_t bd;
    int        x, y, w, h;     /* what was captured                            */
    uint64_t   when;
} glasscache_t;
/* A glass panel over whatever is in the canvas under it right now. `strength`
 * 0..255 is how dense the frosting is. Captures every call: the films move. */
void ui_glass(glasscache_t *gc, int x, int y, int w, int h, int r, int strength, int alpha);
/* The light along a glass edge, without a backdrop -- for small chips. */
void ui_rim(int x, int y, int w, int h, int r, int alpha);
/* A white pill that marks the focused thing, with its glow. */
void ui_focus(int x, int y, int w, int h, int r, int alpha);
void ui_spinner(int cx, int cy, int r, uint64_t now, uint32_t color);
void ui_glow(int cx, int cy, int rx, int ry, uint32_t color, int alpha);   /* soft radial light/shade */
void ui_slider(int x, int y, int w, int pct, int focused);
void ui_switch(int x, int cy, int on, int focused);
void ui_progress(int x, int y, int w, int h, int pct, uint32_t color);
int  ui_hit(int px, int py, int x, int y, int w, int h);
/* Ease `cur` toward `target`: framerate-independent, `ms` to cover ~90%. */
float ui_ease(float cur, float target, uint64_t dt, int ms);
/* Draw a canvas-sized vertical darkening from the top/bottom edges. */
void ui_scrim_top(int h, int alpha);
void ui_scrim_bottom(int h, int alpha);
void ui_scrim_left(int w, int alpha);
int  ui_text(int face, int px, int x, int cy, const char *s, uint32_t color, int alpha, int align);
int  ui_text_shadow(int face, int px, int x, int cy, const char *s, uint32_t color, int alpha, int align);
int  ui_wrap(int face, int px, int x, int top, int w, int line_h, int max_lines,
             const char *s, uint32_t color, int alpha);
void fmt_int(char *out, int cap, long v);          /* 12,345 */
void fmt_time(char *out, int cap, int h, int m);   /* 21:05 or 9:05 PM */

/* ---- the views ----------------------------------------------------------- */
extern const view_t view_home, view_live, view_input, view_web, view_adblock,
                    view_settings;
void settings_open_about(void);
void input_select(int which);        /* 0 HDMI 1, 1 Apple TV (HDMI 2) */
void web_open(const char *url);
void live_tune(int channel);
int  live_channel(void);

/* ---- settings (tv/settings.c) -------------------------------------------- */
typedef struct {
    int volume;            /* 0..100                                           */
    int muted;
    int brightness;        /* 10..100                                          */
    int picture;           /* 0 standard, 1 vivid, 2 cinema, 3 warm            */
    int film;              /* -1 shuffle, else a film index                    */
    int film_info;         /* show what is playing on the home screen          */
    int clock24;
    int utc_offset;        /* hours, -12..14                                   */
    int sounds;            /* UI clicks                                        */
    int sleep_min;         /* 0 = off                                          */
    uint64_t sleep_at;     /* tv_now_ms() when the set turns itself off        */
    int adblock;           /* the blocker is on                                */
} tvsettings_t;
extern tvsettings_t tvs;
void settings_defaults(void);
void settings_apply_picture(void);
/* The picture-mode lookup, applied on the way out; 0 when it is the identity. */
const uint8_t *settings_picture_lut(void);

/* ---- network service (tv/netsvc.c) --------------------------------------- */
enum { NET_DOWN = 0, NET_PROBING, NET_DHCP, NET_UP, NET_NOLINK };
void netsvc_start(void);
/* GET url into buf on the network thread, waiting for it: for a thread of its
 * own (the updater's), never the shell's. The body's length, or -1. */
int  netsvc_get(const char *url, const char *headers, char *buf, uint32_t cap, int *status);
int  netsvc_state(void);
const char *netsvc_nic(void);        /* "Intel e1000", "" if none */
uint32_t netsvc_ip(void);
uint32_t netsvc_gw(void);
uint32_t netsvc_dns(void);
int  netsvc_clock_synced(void);
void fmt_ip(char *out, uint32_t ip);

/* A fetch runs on its own kernel thread so the films keep playing while TLS
 * does its arithmetic. The caller owns the job and polls `state`. */
enum { FETCH_IDLE = 0, FETCH_QUEUED, FETCH_RUNNING, FETCH_DONE, FETCH_FAILED, FETCH_CANCELLED };
typedef struct fetch {
    char      url[1024];
    char     *buf;             /* caller's buffer                              */
    uint32_t  cap;
    volatile int state;
    int       len;             /* body bytes on success                        */
    int       status;          /* HTTP status, or a negative reason            */
    char      location[1024];  /* a redirect's target                          */
    const char *headers;       /* extra request header lines, or NULL           */
} fetch_t;
int  netsvc_fetch(fetch_t *f);       /* 0 queued, -1 queue full */
void netsvc_cancel(fetch_t *f);

/* ---- ad blocker (tv/adblock.c) ------------------------------------------- */
int  adblock_host_blocked(const char *host);     /* counts it if so */
int  adblock_url_blocked(const char *url);       /* host + path patterns */
int  adblock_element_hidden(const char *tag, const char *id, const char *cls);
int  adblock_list_size(void);
uint32_t adblock_blocked_total(void);
uint32_t adblock_blocked_dns(void);
uint32_t adblock_blocked_web(void);
uint32_t adblock_hidden_elements(void);
void adblock_note_hidden(int n);
int  adblock_recent(int i, const char **host, uint32_t *count);   /* newest first */

/* ---- sound (tv/sfx.c) ---------------------------------------------------- */
void sfx_init(void);
void sfx_move(void);
void sfx_select(void);
void sfx_back(void);
void sfx_volume(void);
