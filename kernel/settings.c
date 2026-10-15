/* ===========================================================================
 *  BoltOS  -  kernel/settings.c
 *  The live desktop theme (g_theme) and the editable settings model
 *  (g_settings), plus the option tables the Settings app renders. Applying a
 *  change retints the theme and hands off to the GUI to relayout + repaint.
 * ===========================================================================*/
#include <stdint.h>
#include "settings.h"
#include "gui.h"
#include "pcspk.h"

/* Live palette. Starts as the Dark preset so any early COL_* read (before
 * settings_init) still produces a sane desktop. */
theme_t g_theme = {
    .accent = 0x0A84FF, .accent_dim = 0x0A5BB8,
    .text   = 0xF5F5F7, .text_dim   = 0xA1A1AA,
    .panel  = 0x1E1E21, .panel2     = 0x2C2C30, .panel3 = 0x3C3C42,
    .good   = 0x30D158, .warn       = 0xFFD60A, .bad    = 0xFF453A,
    .dark = 1, .titlebar = 0x2A2A2E, .text_mute = 0x6E6E76, .sep = 0x0E0E10,
    .glass = 0x1C1C22, .glass_a = 135, .sel = 0x0A84FF,
};

settings_t g_settings;

/* ---- theme presets ------------------------------------------------------ *
 * Neutral greys first: a tinted window frame fights every app's own colours,
 * and the glass picks the wallpaper's tint up on its own anyway. The window
 * body is darkest, the title bar a step lighter, controls a step above that
 * -- the same ladder macOS climbs, which is why it reads as depth. */
static const theme_t presets[] = {
    /* Dark */
    { .accent = 0x0A84FF, .accent_dim = 0x0A5BB8, .text = 0xF5F5F7, .text_dim = 0xA1A1AA,
      .panel = 0x1E1E21, .panel2 = 0x2C2C30, .panel3 = 0x3C3C42,
      .good = 0x30D158, .warn = 0xFFD60A, .bad = 0xFF453A,
      .dark = 1, .titlebar = 0x2A2A2E, .text_mute = 0x6E6E76, .sep = 0x0E0E10,
      .glass = 0x1C1C22, .glass_a = 135, .sel = 0x0A84FF },
    /* Light */
    { .accent = 0x007AFF, .accent_dim = 0x7DB7FF, .text = 0x1D1D1F, .text_dim = 0x6E6E73,
      .panel = 0xF6F6F8, .panel2 = 0xFFFFFF, .panel3 = 0xE2E2E7,
      .good = 0x248A3D, .warn = 0xC77C02, .bad = 0xE0352B,
      .dark = 0, .titlebar = 0xECECEF, .text_mute = 0xA1A1A6, .sep = 0xD4D4D9,
      .glass = 0xF2F2F6, .glass_a = 150, .sel = 0x007AFF },
    /* Midnight: the dark ladder, pushed toward blue */
    { .accent = 0x5E8BFF, .accent_dim = 0x3A5BB0, .text = 0xEEF1FA, .text_dim = 0x98A2BD,
      .panel = 0x161A28, .panel2 = 0x1F2436, .panel3 = 0x2B3248,
      .good = 0x4FD18A, .warn = 0xF2C14E, .bad = 0xFF5C6C,
      .dark = 1, .titlebar = 0x1D2233, .text_mute = 0x5E6782, .sep = 0x0A0D16,
      .glass = 0x141A2C, .glass_a = 140, .sel = 0x5E8BFF },
    /* Graphite: no colour at all, accent included */
    { .accent = 0x98989D, .accent_dim = 0x6E6E73, .text = 0xF2F2F2, .text_dim = 0xA0A0A3,
      .panel = 0x232325, .panel2 = 0x2F2F32, .panel3 = 0x3F3F43,
      .good = 0x30D158, .warn = 0xFFD60A, .bad = 0xFF453A,
      .dark = 1, .titlebar = 0x303033, .text_mute = 0x6A6A6F, .sep = 0x111112,
      .glass = 0x202022, .glass_a = 140, .sel = 0x98989D },
    /* Sand: a warm light one */
    { .accent = 0xD9822B, .accent_dim = 0xEBB27A, .text = 0x2B2620, .text_dim = 0x7A7064,
      .panel = 0xF7F3EE, .panel2 = 0xFFFDFB, .panel3 = 0xE8E1D8,
      .good = 0x3E8E41, .warn = 0xB7791F, .bad = 0xC8463B,
      .dark = 0, .titlebar = 0xEFE8E0, .text_mute = 0xA89E92, .sep = 0xDDD3C7,
      .glass = 0xF6F0E8, .glass_a = 150, .sel = 0xD9822B },
};
static const char *preset_names[] = { "Dark", "Light", "Midnight", "Graphite", "Sand" };
#define NPRESET ((int)(sizeof(presets) / sizeof(presets[0])))

/* ---- accent palette ----------------------------------------------------- */
static const uint32_t accents[] = {
    0x0A84FF, 0x5E5CE6, 0xBF5AF2, 0xFF375F, 0xFF453A,
    0xFF9F0A, 0xFFCC00, 0x30D158, 0x40C8E0, 0x98989D,
};
#define NACCENT ((int)(sizeof(accents) / sizeof(accents[0])))

/* ---- wallpaper base colours --------------------------------------------- */
static const uint32_t wallcolors[] = {
    0x0B1A3A, 0x1A1440, 0x2A1236, 0x10283A, 0x0E2A22,
    0x3A1E12, 0x16181D, 0x26262B, 0x0C1414, 0x000000,
};
#define NWALLC ((int)(sizeof(wallcolors) / sizeof(wallcolors[0])))

static const char *wallstyle_names[] = { "Gradient", "Solid", "Glow", "Grid" };

/* ---- resolution table (REAL physical panel size via DISPI) -------------- *
 * These reprogram the GPU to a true native mode -- the framebuffer becomes the
 * picked size with no upscaling and no letterbox bars, so the picture stays
 * crisp at every step. 1024x768 is the boot default. */
static const struct { const char *name; int w, h; } res_tab[] = {
    { "1024x768", 1024, 768 },
    { "720p",     1280, 720  },
    { "1080p",    1920, 1080 },
    { "1440p",    2560, 1440 },
    { "4K",       3840, 2160 },
};
#define NRES ((int)(sizeof(res_tab) / sizeof(res_tab[0])))

/* ---- display scaling ---------------------------------------------------- *
 * Integer factors are exact: at 200% one logical pixel is a 2x2 block of real
 * ones and nothing softens. 125% and 150% are not -- the upscale picks nearest,
 * so some rows double and their neighbours do not. They are here because a
 * desktop that is slightly too small is worse than one that is slightly uneven,
 * and the Settings page says which is which. */
static const struct { const char *name; int pct; } scale_tab[] = {
    { "Auto", 0 },
    { "100%", 100 },
    { "125%", 125 },
    { "150%", 150 },
    { "200%", 200 },
};
#define NSCALE ((int)(sizeof(scale_tab) / sizeof(scale_tab[0])))

/* ---- audio output device ------------------------------------------------ */
static const char *audio_names[] = { "PC Speaker", "Muted" };
#define NAUDIO ((int)(sizeof(audio_names) / sizeof(audio_names[0])))

/* ---- UI font face ------------------------------------------------------- */
static const char *font_names[] = { "Retro 8x8", "Arial" };
#define NFONT ((int)(sizeof(font_names) / sizeof(font_names[0])))

/* ---- aspect table (output rectangle shape) ------------------------------ */
static const struct { const char *name; int num, den; } asp_tab[] = {
    { "Auto", 0, 0 },          /* 0,0 -> preserve logical aspect */
    { "16:9",  16, 9 },
    { "16:10", 16, 10 },
    { "4:3",   4,  3 },
    { "1:1",   1,  1 },
    { "21:9",  21, 9 },
};
#define NASP ((int)(sizeof(asp_tab) / sizeof(asp_tab[0])))

/* ---- table accessors ---------------------------------------------------- */
int         settings_theme_count(void)       { return NPRESET; }
const char *settings_theme_name(int i)       { return (i >= 0 && i < NPRESET) ? preset_names[i] : ""; }
uint32_t    settings_theme_accent(int i)     { return (i >= 0 && i < NPRESET) ? presets[i].accent : 0x0A84FF; }
uint32_t    settings_theme_swatch(int i)     { return (i >= 0 && i < NPRESET) ? presets[i].panel : 0x1E1E21; }
const theme_t *settings_theme_preset(int i)  { return &presets[(i >= 0 && i < NPRESET) ? i : 0]; }

int         settings_accent_count(void)      { return NACCENT; }
uint32_t    settings_accent_color(int i)     { return (i >= 0 && i < NACCENT) ? accents[i] : 0x0A84FF; }

int         settings_wallcolor_count(void)   { return NWALLC; }
uint32_t    settings_wallcolor(int i)        { return (i >= 0 && i < NWALLC) ? wallcolors[i] : 0x0B1A3A; }

const char *settings_wallstyle_name(int i)   { return (i >= 0 && i < WALL_COUNT) ? wallstyle_names[i] : ""; }

int         settings_res_count(void)         { return NRES; }
const char *settings_res_name(int i)         { return (i >= 0 && i < NRES) ? res_tab[i].name : ""; }
void settings_res_dims(int i, int *w, int *h) {
    int rw = (i >= 0 && i < NRES) ? res_tab[i].w : 1024;
    int rh = (i >= 0 && i < NRES) ? res_tab[i].h : 768;
    if (w) *w = rw;
    if (h) *h = rh;
}

int         settings_scale_count(void)       { return NSCALE; }
const char *settings_scale_name(int i)       { return (i >= 0 && i < NSCALE) ? scale_tab[i].name : ""; }

/* Auto has one thing to go on: how many pixels the panel has. Nothing reports
 * its physical size, so this cannot be real DPI and does not pretend to be --
 * it is the point past which an unscaled desktop is uncomfortable on anything
 * laptop-sized, and it only ever picks an exact factor. */
int settings_scale_percent(int i, int panel_w, int panel_h) {
    (void)panel_h;
    if (i <= 0 || i >= NSCALE) return panel_w >= 2560 ? 200 : 100;
    return scale_tab[i].pct;
}

int         settings_audio_count(void)       { return NAUDIO; }
const char *settings_audio_name(int i)       { return (i >= 0 && i < NAUDIO) ? audio_names[i] : ""; }

int         settings_font_count(void)        { return NFONT; }
const char *settings_font_name(int i)        { return (i >= 0 && i < NFONT) ? font_names[i] : ""; }

static const char *motion_names[MOTION_COUNT] = { "Auto", "Full", "Reduced" };
const char *settings_motion_name(int i)      { return (i >= 0 && i < MOTION_COUNT) ? motion_names[i] : ""; }

int         settings_aspect_count(void)      { return NASP; }
const char *settings_aspect_name(int i)      { return (i >= 0 && i < NASP) ? asp_tab[i].name : ""; }
void settings_aspect_ratio(int i, int *num, int *den) {
    if (num) *num = (i >= 0 && i < NASP) ? asp_tab[i].num : 0;
    if (den) *den = (i >= 0 && i < NASP) ? asp_tab[i].den : 0;
}

/* ---- apply -------------------------------------------------------------- */
/* scale each RGB channel of c by n/255 */
static uint32_t shade(uint32_t c, int n) {
    int r = ((c >> 16 & 0xFF) * n) / 255;
    int g = ((c >> 8  & 0xFF) * n) / 255;
    int b = ((c       & 0xFF) * n) / 255;
    return (uint32_t)(r << 16 | g << 8 | b);
}

void settings_init(void) {
    g_settings.theme        = 0;
    g_settings.accent       = settings_theme_accent(0);
    g_settings.wall_style   = WALL_GRADIENT;
    g_settings.wall_color   = 0x0B1A3A;
    g_settings.res_index    = 0;     /* 1024x768 boot default */
    g_settings.aspect_index = 0;     /* unused (kept for ABI) */
    g_settings.audio_device = 0;     /* PC Speaker */
    g_settings.font         = 1;     /* Arial (8x16) is the default face */
    g_settings.scale_index  = 0;     /* Auto: 100% unless the panel is huge */
    g_settings.motion       = MOTION_AUTO;
    settings_apply();
}

void settings_apply(void) {
    int t = g_settings.theme;
    if (t < 0 || t >= NPRESET) t = 0;
    g_theme = presets[t];
    /* user accent overrides the preset's; derive a dim variant for it */
    g_theme.accent     = g_settings.accent;
    g_theme.accent_dim = shade(g_settings.accent, 158);
    g_theme.sel        = g_settings.accent;
    pcspk_set_enabled(g_settings.audio_device == 0);   /* PC Speaker vs Muted */
    g_set_font(g_settings.font);                        /* swap the GUI font face */
    gui_motion_apply();
    gui_apply_display();             /* re-mode the panel + repaint if changed */
}
