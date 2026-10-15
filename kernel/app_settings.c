/* ===========================================================================
 *  BoltOS  -  kernel/app_settings.c
 *
 *  Settings: a sidebar of pages, the way every OS since about 2015 has done
 *  it -- Personalization, Display, Sound, Text, About. Each control edits
 *  g_settings and calls settings_apply(), which retints the theme and asks the
 *  desktop to relayout and repaint; nothing here knows how any of that works.
 *
 *  Built on the control kit (ui.h), so every row is drawn and hit-tested from
 *  the same rectangle, and it follows the theme it is changing -- switch to
 *  Light here and this window goes light with everything else.
 * ===========================================================================*/
#include <stdint.h>
#include <stdio.h>
#include "gui.h"
#include "ui.h"
#include "gfx.h"
#include "settings.h"
#include "string.h"
#include "pcspk.h"
#include "pit.h"
#include "hw.h"
#include "pmm.h"
#include "smp.h"
#include "audio.h"
#include "backlight.h"
#include "users.h"
#include "fs.h"
#include "image.h"
#include "kheap.h"
#include "buildid.h"
#include "keyboard.h"

enum { PG_PERSONAL = 0, PG_DISPLAY, PG_SOUND, PG_TEXT, PG_ABOUT, PG_COUNT };
static const char *PAGE_NAME[PG_COUNT] = { "Personalization", "Display", "Sound", "Text & fonts", "About" };
static const int   PAGE_GLYPH[PG_COUNT] = { GLYPH_PALETTE, GLYPH_MONITOR, GLYPH_SPEAKER, GLYPH_TYPE, GLYPH_INFO };

static int      page;
static window_t *self;
static ui_ctx_t U;
static uint64_t beep_off_at;      /* pit tick to silence the test tone, 0 = idle */
static int      drag_id = -1;     /* slider being dragged */
static int      sl_x[4], sl_w[4]; /* slider geometry, desktop coords, by id-SL_BASE */

/* ids */
#define ID_PAGE      100          /* + page                       */
#define ID_WALL      200          /* + wallpaper choice            */
#define ID_THEME     300          /* + preset                     */
#define ID_ACCENT    400          /* + accent                     */
#define ID_WCOLOR    500          /* + wallpaper base colour       */
#define ID_RES       600          /* + resolution                  */
#define ID_SCALE     700          /* + scale                       */
#define ID_AUDIO     800          /* + device                      */
#define ID_FONT      900          /* + face                        */
#define ID_TEST      1000
#define ID_MIXER     1001
#define SL_BASE      1100         /* sliders: + 0 brightness, 1 volume */
#define ID_MOTION    1200         /* + MOTION_*                    */

/* ---- wallpapers ------------------------------------------------------------- */
typedef struct { const char *name; const char *stem; int style; } wpick_t;
static const wpick_t WP[] = {
    { "Aurora",       "aurora",       -1 },
    { "Aurora Light", "aurora-light", -1 },
    { "Mountain",     "mountain",     -1 },
    { "Gradient",     0,              WALL_GRADIENT },
    { "Glow",         0,              WALL_GLOW },
    { "Solid",        0,              WALL_SOLID },
};
#define NWP ((int)(sizeof(WP) / sizeof(WP[0])))
#define TH_W 120
#define TH_H 72
static uint32_t *thumb[NWP];
static int       thumb_tried[NWP];

static void box_scale(const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh) {
    /* centre-crop to the thumbnail's shape, then average boxes */
    int cw = sw, ch = sw * dh / dw;
    if (ch > sh) { ch = sh; cw = sh * dw / dh; }
    int ox = (sw - cw) / 2, oy = (sh - ch) / 2;
    for (int j = 0; j < dh; j++) {
        int y0 = oy + j * ch / dh, y1 = oy + (j + 1) * ch / dh; if (y1 <= y0) y1 = y0 + 1;
        for (int i = 0; i < dw; i++) {
            int x0 = ox + i * cw / dw, x1 = ox + (i + 1) * cw / dw; if (x1 <= x0) x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    uint32_t c = src[y * sw + x];
                    r += c >> 16 & 0xFF; g += c >> 8 & 0xFF; b += c & 0xFF; n++;
                }
            dst[j * dw + i] = 0xFF000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
        }
    }
}

/* One thumbnail per call: decoding a JPEG is the slowest thing this window
 * ever does, so it happens a tick at a time instead of all at once on open. */
static void thumbs_step(void) {
    for (int i = 0; i < NWP; i++) {
        if (thumb[i] || thumb_tried[i]) continue;
        thumb_tried[i] = 1;
        uint32_t *t = (uint32_t *)kmalloc(TH_W * TH_H * 4);
        if (!t) return;
        if (WP[i].stem) {
            char path[96];
            snprintf(path, sizeof path, "/usr/share/wallpapers/%s-1024x768.jpg", WP[i].stem);
            fs_node *n = fs_lookup(path);
            image_t *im = (n && n->data) ? image_decode(n->data, n->size) : 0;
            if (!im) { kfree(t); return; }
            box_scale(im->px, im->w, im->h, t, TH_W, TH_H);
            image_free(im);
        } else {
            /* procedural styles: a miniature of what render_wallpaper paints */
            uint32_t base = g_settings.wall_color;
            uint32_t glow = gfx_shade(base, 170), glow2 = gfx_lerp(base, g_theme.accent, 45, 100);
            for (int y = 0; y < TH_H; y++)
                for (int x = 0; x < TH_W; x++) {
                    uint32_t c = base;
                    if (WP[i].style != WALL_SOLID) {
                        int dx = x - TH_W / 3, dy = y - TH_H / 4;
                        int d = dx * dx + dy * dy, r = TH_W * TH_W / 2;
                        if (d < r) c = gfx_lerp(glow, base, d, r);
                        int ex = x - TH_W * 4 / 5, ey = y - TH_H * 4 / 5;
                        int d2 = ex * ex + ey * ey, r2 = TH_W * TH_W / 4;
                        if (WP[i].style == WALL_GRADIENT && d2 < r2) c = gfx_lerp(gfx_lerp(c, glow2, 55, 100), c, d2, r2);
                    }
                    t[y * TH_W + x] = 0xFF000000u | c;
                }
        }
        thumb[i] = t;
        if (self) gui_redraw_window(self);
        return;
    }
}

static int wall_current(void) {
    const char *p = gui_wallpaper_path();
    if (!p || !p[0]) {
        for (int i = 0; i < NWP; i++) if (!WP[i].stem && WP[i].style == g_settings.wall_style) return i;
        return -1;
    }
    for (int i = 0; i < NWP; i++) {
        if (!WP[i].stem) continue;
        char pre[64]; snprintf(pre, sizeof pre, "/usr/share/wallpapers/%s-", WP[i].stem);
        int n = (int)strlen(pre);
        if (strncmp(p, pre, (size_t)n) == 0 && p[n] >= '0' && p[n] <= '9') return i;
    }
    return -1;
}

/* the live desktop, shrunk: rebuilt when the wallpaper changes */
static uint32_t *preview;
static int       preview_ver = -1;
#define PV_W 232
#define PV_H 140
static void preview_update(void) {
    int v = gui_wallpaper_version();
    if (preview && v == preview_ver) return;
    if (!preview) preview = (uint32_t *)kmalloc(PV_W * PV_H * 4);
    uint32_t *wp = gui_wallpaper_px();
    if (!preview || !wp) return;
    /* sample, not average: the wallpaper is big and this runs on the UI path */
    int sw = gui_screen_w(), sh = gui_screen_h();
    for (int j = 0; j < PV_H; j++)
        for (int i = 0; i < PV_W; i++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) {
                int sx = (i * 2 + (k & 1)) * sw / (PV_W * 2), sy = (j * 2 + (k >> 1)) * sh / (PV_H * 2);
                uint32_t c = wp[sy * sw + sx];
                r += c >> 16 & 0xFF; g += c >> 8 & 0xFF; b += c & 0xFF;
            }
            preview[j * PV_W + i] = 0xFF000000u | (r / 4) << 16 | (g / 4) << 8 | (b / 4);
        }
    preview_ver = v;
}

/* ---- pages --------------------------------------------------------------------- */
static void row_label(int x, int cy, const char *t, const char *sub) { ui_label2(x, cy, t, sub); }

static int draw_personal(int x, int y, int w) {
    preview_update();
    /* the preview, with a little window and a dock on it */
    int ch = PV_H + 32;
    ui_card(x, y, w, ch);
    int px = x + 16, py = y + 16;
    if (preview) {
        gfx_shadow(px, py, PV_W, PV_H, 8, 14, 4, 0, 70);
        gfx_image_rr(px, py, PV_W, PV_H, preview, PV_W, PV_H, 8, 255);
        gfx_shadow(px + 52, py + 26, 110, 70, 5, 10, 3, 0, 80);
        gfx_rrect(px + 52, py + 26, 110, 70, 5, COL_PANEL, 255);
        gfx_rrect4(px + 52, py + 26, 110, 12, 5, 5, 0, 0, COL_TITLEBAR, 255);
        gfx_circle((float)px + 58.0f, (float)py + 32.0f, 2.0f, 0xFF5F57, 255);
        gfx_circle((float)px + 64.0f, (float)py + 32.0f, 2.0f, 0xFEBC2E, 255);
        gfx_circle((float)px + 70.0f, (float)py + 32.0f, 2.0f, 0x28C840, 255);
        gfx_rrect(px + 64, py + 46, 50, 5, 2, COL_TEXT_DIM, 120);
        gfx_rrect(px + 64, py + 56, 76, 5, 2, COL_TEXT_DIM, 90);
        gfx_rrect(px + 64, py + 72, 34, 12, 4, COL_ACCENT, 255);
        gfx_rrect(px + PV_W / 2 - 46, py + PV_H - 18, 92, 13, 6, 0xFFFFFF, THEME_DARK ? 40 : 120);
        for (int k = 0; k < 6; k++)
            gfx_rrect(px + PV_W / 2 - 40 + k * 14, py + PV_H - 15, 9, 9, 3,
                      k == 0 ? COL_ACCENT : 0xFFFFFF, k == 0 ? 255 : 180);
    }
    /* the choices beside it */
    int gx = px + PV_W + 24;
    text_l(gx, py + 8, "Background", FONT_SEMIBOLD, 14, COL_TEXT);
    text_l(gx, py + 28, "Pick a picture, or a colour field", FONT_REGULAR, 12, COL_TEXT_DIM);
    int cur = wall_current();
    int cols = (x + w - 16 - gx + 10) / (TH_W / 2 + 14);
    if (cols < 1) cols = 1;
    int tw = TH_W / 2 + 4, th = TH_H / 2 + 4;
    for (int i = 0; i < NWP; i++) {
        int tx = gx + (i % cols) * (tw + 10), ty = py + 46 + (i / cols) * (th + 10);
        if (ty + th > y + ch - 8) break;
        if (i == cur) gfx_rrect_stroke(tx - 3, ty - 3, tw + 6, th + 6, 9, 2, COL_ACCENT, 255);
        else if (ui_hover(&U, tx, ty, tw, th)) gfx_rrect_stroke(tx - 3, ty - 3, tw + 6, th + 6, 9, 2, COL_TEXT_MUTE, 160);
        if (thumb[i]) gfx_image_rr(tx, ty, tw, th, thumb[i], TH_W, TH_H, 6, 255);
        else gfx_rrect(tx, ty, tw, th, 6, COL_PANEL_3, 255);
        ui_hot(&U, tx - 3, ty - 3, tw + 6, th + 6, ID_WALL + i);
    }
    y += ch + 14;

    /* appearance */
    ui_section(x, y, "Appearance");
    y += 26;
    int n = settings_theme_count();
    int cw = (w - (n - 1) * 10) / n; if (cw > 132) cw = 132;
    int cardh = 96;
    for (int i = 0; i < n; i++) {
        const theme_t *t = settings_theme_preset(i);
        int cx = x + i * (cw + 10);
        int sel = (g_settings.theme == i);
        if (sel) gfx_rrect_stroke(cx - 3, y - 3, cw + 6, cardh - 20 + 6, 12, 2, COL_ACCENT, 255);
        else if (ui_hover(&U, cx, y, cw, cardh)) gfx_rrect_stroke(cx - 3, y - 3, cw + 6, cardh - 20 + 6, 12, 2, COL_TEXT_MUTE, 140);
        gfx_rrect(cx, y, cw, cardh - 20, 10, t->panel, 255);
        gfx_rrect4(cx, y, cw, 16, 10, 10, 0, 0, t->titlebar, 255);
        gfx_circle((float)cx + 9.0f, (float)y + 8.0f, 2.5f, 0xFF5F57, 255);
        gfx_circle((float)cx + 16.0f, (float)y + 8.0f, 2.5f, 0xFEBC2E, 255);
        gfx_circle((float)cx + 23.0f, (float)y + 8.0f, 2.5f, 0x28C840, 255);
        gfx_rrect(cx + 10, y + 26, cw - 40, 6, 3, t->text, 160);
        gfx_rrect(cx + 10, y + 38, cw - 56, 6, 3, t->text_dim, 140);
        gfx_rrect(cx + 10, y + 54, 30, 12, 6, t->accent, 255);
        gfx_rrect_stroke(cx, y, cw, cardh - 20, 10, 1, 0x000000, 50);
        text_c(cx + cw / 2, y + cardh - 8, settings_theme_name(i), sel ? FONT_SEMIBOLD : FONT_REGULAR, 12, COL_TEXT);
        ui_hot(&U, cx, y, cw, cardh, ID_THEME + i);
    }
    y += cardh + 14;

    /* accent */
    ui_card(x, y, w, 56);
    row_label(x + 16, y + 28, "Accent colour", "Buttons, selection, the dock's active app");
    int na = settings_accent_count();
    int sx = x + w - 16 - na * 30 + 15;
    for (int i = 0; i < na; i++)
        ui_swatch(&U, sx + i * 30, y + 28, 11, settings_accent_color(i),
                  settings_accent_color(i) == g_settings.accent, ID_ACCENT + i);
    y += 56 + 10;

    /* base colour for the colour-field wallpapers */
    if (cur < 0 || !WP[cur].stem) {
        ui_card(x, y, w, 56);
        row_label(x + 16, y + 28, "Background colour", "For the gradient, glow and solid fields");
        int nw = settings_wallcolor_count();
        int bx = x + w - 16 - nw * 30 + 15;
        for (int i = 0; i < nw; i++)
            ui_swatch(&U, bx + i * 30, y + 28, 11, settings_wallcolor(i),
                      settings_wallcolor(i) == g_settings.wall_color, ID_WCOLOR + i);
        y += 56 + 10;
    }
    return y;
}

static int draw_display(int x, int y, int w) {
    char line[80];
    ui_card(x, y, w, 116);
    row_label(x + 16, y + 30, "Resolution", "The panel itself is reprogrammed: no stretching");
    const char *res[8]; int nr = settings_res_count(); if (nr > 8) nr = 8;
    for (int i = 0; i < nr; i++) res[i] = settings_res_name(i);
    ui_segmented(&U, x + 16, y + 62, w - 32, res, nr, g_settings.res_index, ID_RES);
    snprintf(line, sizeof line, "Panel %d " "\xD7" " %d", gui_panel_w(), gui_panel_h());
    text_r(x + w - 16, y + 30, line, FONT_REGULAR, 12, COL_TEXT_DIM);
    y += 116 + 10;

    ui_card(x, y, w, 116);
    row_label(x + 16, y + 30, "Scale", "Makes everything bigger, every app included");
    const char *sc[8]; int ns = settings_scale_count(); if (ns > 8) ns = 8;
    for (int i = 0; i < ns; i++) sc[i] = settings_scale_name(i);
    ui_segmented(&U, x + 16, y + 62, w - 32, sc, ns, g_settings.scale_index, ID_SCALE);
    snprintf(line, sizeof line, "Desktop %d " "\xD7" " %d", gui_screen_w(), gui_screen_h());
    text_r(x + w - 16, y + 30, line, FONT_REGULAR, 12, COL_TEXT_DIM);
    y += 116 + 10;

    ui_card(x, y, w, 60);
    glyph_draw_c(GLYPH_SUN, x + 28, y + 30, 20, COL_TEXT_DIM, 255);
    row_label(x + 48, y + 30, "Brightness", 0);
    sl_x[0] = x + 170; sl_w[0] = w - 170 - 60;
    ui_slider(&U, sl_x[0], y + 30, sl_w[0], backlight_get(), SL_BASE + 0);
    snprintf(line, sizeof line, "%d%%", backlight_get());
    text_r(x + w - 16, y + 30, line, FONT_REGULAR, 12, COL_TEXT_DIM);
    y += 70;

    ui_card(x, y, w, 116);
    row_label(x + 16, y + 30, "Animations", "Auto turns them off on a machine that draws slowly");
    const char *mo[MOTION_COUNT];
    for (int i = 0; i < MOTION_COUNT; i++) mo[i] = settings_motion_name(i);
    ui_segmented(&U, x + 16, y + 62, w - 32, mo, MOTION_COUNT, g_settings.motion, ID_MOTION);
    if (g_settings.motion == MOTION_AUTO) {
        int us = gui_frame_copy_us();
        snprintf(line, sizeof line, "%s " TXT_EMDASH " a frame copy takes %d.%d ms",
                 gui_motion_reduced() ? "Off" : "On", us / 1000, us / 100 % 10);
    } else {
        snprintf(line, sizeof line, "%s", gui_motion_reduced() ? "Off" : "On");
    }
    text_r(x + w - 16, y + 30, line, FONT_REGULAR, 12, COL_TEXT_DIM);
    return y + 126;
}

static int draw_sound(int x, int y, int w) {
    char line[32];
    ui_card(x, y, w, 60);
    glyph_draw_c(GLYPH_SPEAKER, x + 28, y + 30, 20, COL_TEXT_DIM, 255);
    row_label(x + 48, y + 30, "Volume", 0);
    sl_x[1] = x + 170; sl_w[1] = w - 170 - 60;
    ui_slider(&U, sl_x[1], y + 30, sl_w[1], audio_volume(), SL_BASE + 1);
    snprintf(line, sizeof line, "%d%%", audio_volume());
    text_r(x + w - 16, y + 30, line, FONT_REGULAR, 12, COL_TEXT_DIM);
    y += 70;

    ui_card(x, y, w, 116);
    row_label(x + 16, y + 30, "System sounds", "Beeps and alerts go to this device");
    const char *dev[4]; int nd = settings_audio_count(); if (nd > 4) nd = 4;
    for (int i = 0; i < nd; i++) dev[i] = settings_audio_name(i);
    ui_segmented(&U, x + 16, y + 62, w - 32 - 110, dev, nd, g_settings.audio_device, ID_AUDIO);
    ui_button(&U, x + w - 16 - 96, y + 62, 96, 30, "Test", UI_BTN_SECONDARY, ID_TEST);
    y += 126;

    ui_card(x, y, w, 60);
    row_label(x + 16, y + 30, "Mixer", "Every source, with its own level and mute");
    ui_button(&U, x + w - 16 - 120, y + 15, 120, 30, "Open Mixer", UI_BTN_SECONDARY, ID_MIXER);
    return y + 70;
}

static int draw_text(int x, int y, int w) {
    int n = settings_font_count();
    int cw = (w - 10) / 2, chh = 150;
    for (int i = 0; i < n && i < 2; i++) {
        int cx = x + i * (cw + 10);
        int sel = g_settings.font == i;
        ui_card(cx, y, cw, chh);
        if (sel) gfx_rrect_stroke(cx - 2, y - 2, cw + 4, chh + 4, 12, 2, COL_ACCENT, 255);
        if (i == 1) {
            font_draw(FONT_SEMIBOLD, 44, cx + 24, y + 70, "Aa", -1, COL_TEXT, 255);
            text_l(cx + 24, y + 100, "Inter", FONT_SEMIBOLD, 14, COL_TEXT);
            text_l(cx + 24, y + 120, "Sharp at every size. The default.", FONT_REGULAR, 12, COL_TEXT_DIM);
        } else {
            /* the old face, in its own pixels */
            g_text_retro(cx + 24, y + 34, "Aa", COL_TEXT, 4);
            text_l(cx + 24, y + 100, "Retro 8" "\xD7" "8", FONT_SEMIBOLD, 14, COL_TEXT);
            text_l(cx + 24, y + 120, "The bitmap face BoltOS started with", FONT_REGULAR, 12, COL_TEXT_DIM);
        }
        ui_hot(&U, cx, y, cw, chh, ID_FONT + i);
    }
    y += chh + 14;
    ui_card(x, y, w, 120);
    text_l(x + 16, y + 26, "Preview", FONT_SEMIBOLD, 13, COL_TEXT_DIM);
    font_draw(FONT_BOLD, 22, x + 16, y + 64, "The quick brown fox", -1, COL_TEXT, 255);
    font_draw(FONT_REGULAR, 13, x + 16, y + 88, "jumps over the lazy dog. 0123456789 " TXT_EMDASH " AV To Ty", -1, COL_TEXT_DIM, 255);
    font_draw(FONT_MONO, 13, x + 16, y + 108, "bolt:/home/user> echo terminal text", -1, COL_TEXT_DIM, 255);
    return y + 130;
}

static int draw_about(int x, int y, int w) {
    ui_card(x, y, w, 150);
    icon_draw(ICON_START, x + 24, y + 27, 96, 255);
    font_draw(FONT_BOLD, 30, x + 140, y + 62, "BoltOS", -1, COL_TEXT, 255);
    text_l(x + 140, y + 86, "Built " BOLT_BUILD_ID, FONT_REGULAR, 12, COL_TEXT_DIM);
    text_l(x + 140, y + 108, "A 64-bit operating system, written from the boot sector up", FONT_REGULAR, 12, COL_TEXT_DIM);
    y += 164;
    char cpu[49]; hw_cpu_brand(cpu);
    char *c = cpu; while (*c == ' ') c++;
    char mem[40], disp[40], cores[16], who[48];
    uint64_t mb = pmm_total_count() * 4096 / (1024 * 1024);
    snprintf(mem, sizeof mem, "%lu MB", (unsigned long)mb);
    snprintf(disp, sizeof disp, "%d " "\xD7" " %d", gui_panel_w(), gui_panel_h());
    snprintf(cores, sizeof cores, "%d", smp_cpu_count());
    snprintf(who, sizeof who, "%s", users_current()[0] ? users_current() : "Nobody");
    const char *K[5] = { "Processor", "Cores", "Memory", "Display", "Signed in as" };
    const char *V[5] = { c[0] ? c : "Unknown", cores, mem, disp, who };
    ui_card(x, y, w, 5 * 40 + 8);
    for (int i = 0; i < 5; i++) {
        int ry = y + 4 + i * 40;
        if (i) ui_card_row_sep(x + 16, ry, w - 32);
        text_l(x + 16, ry + 20, K[i], FONT_REGULAR, 13, COL_TEXT);
        text_fit_l(x + 150, ry + 20, V[i], w - 166, FONT_REGULAR, 13, COL_TEXT_DIM);
    }
    return y + 5 * 40 + 18;
}

/* ---- draw ---------------------------------------------------------------------- */
#define SIDE_W 220
static void settings_draw(window_t *w, int cx, int cy, int cw, int ch) {
    ui_begin(&U, w, cx, cy);
    /* sidebar: a step away from the page colour, like every modern settings app */
    uint32_t side = THEME_DARK ? gfx_shade(COL_PANEL, 82) : gfx_shade(COL_PANEL, 98);
    gfx_fill(cx, cy, SIDE_W, ch, side);
    gfx_fill(cx + SIDE_W, cy, 1, ch, COL_SEP);
    /* who */
    const char *who = users_current();
    char init[2] = { (char)(who[0] >= 'a' && who[0] <= 'z' ? who[0] - 32 : (who[0] ? who[0] : 'G')), 0 };
    gfx_circle((float)cx + 38.0f, (float)cy + 42.0f, 20.0f, COL_ACCENT, 255);
    text_c(cx + 38, cy + 42, init, FONT_SEMIBOLD, 17, 0xFFFFFF);
    text_l(cx + 68, cy + 34, who[0] ? who : "Guest", FONT_SEMIBOLD, 14, COL_TEXT);
    text_l(cx + 68, cy + 52, users_is_admin() ? "Administrator" : "Local account", FONT_REGULAR, 12, COL_TEXT_DIM);
    for (int i = 0; i < PG_COUNT; i++)
        ui_sidebar_item(&U, cx + 10, cy + 84 + i * 38, SIDE_W - 20, PAGE_GLYPH[i], PAGE_NAME[i], page == i, ID_PAGE + i);
    /* the page */
    int px = cx + SIDE_W + 28, pw = cw - SIDE_W - 56;
    gfx_clip_push(cx + SIDE_W + 1, cy, cw - SIDE_W - 1, ch);
    ui_title(px, cy + 20, PAGE_NAME[page]);
    int y = cy + 66;
    switch (page) {
    case PG_PERSONAL: draw_personal(px, y, pw); break;
    case PG_DISPLAY:  draw_display(px, y, pw); break;
    case PG_SOUND:    draw_sound(px, y, pw); break;
    case PG_TEXT:     draw_text(px, y, pw); break;
    default:          draw_about(px, y, pw); break;
    }
    gfx_clip_pop();
}

/* ---- input --------------------------------------------------------------------- */
static void set_wallpaper(int i) {
    if (i < 0 || i >= NWP) return;
    if (WP[i].stem) {
        char path[96];
        snprintf(path, sizeof path, "/usr/share/wallpapers/%s-%dx%d.jpg", WP[i].stem, gui_screen_w(), gui_screen_h());
        if (!fs_lookup(path)) snprintf(path, sizeof path, "/usr/share/wallpapers/%s-1024x768.jpg", WP[i].stem);
        gui_set_wallpaper_image(path);
    } else {
        g_settings.wall_style = WP[i].style;
        gui_clear_wallpaper_image();
    }
}

static void act(int id, int lx_desktop) {
    if (id >= ID_PAGE && id < ID_PAGE + PG_COUNT) { page = id - ID_PAGE; return; }
    if (id >= ID_WALL && id < ID_WALL + NWP) { set_wallpaper(id - ID_WALL); return; }
    if (id >= ID_THEME && id < ID_THEME + settings_theme_count()) {
        g_settings.theme = id - ID_THEME;
        g_settings.accent = settings_theme_accent(g_settings.theme);
        settings_apply();
        return;
    }
    if (id >= ID_ACCENT && id < ID_ACCENT + settings_accent_count()) {
        g_settings.accent = settings_accent_color(id - ID_ACCENT); settings_apply(); return;
    }
    if (id >= ID_WCOLOR && id < ID_WCOLOR + settings_wallcolor_count()) {
        g_settings.wall_color = settings_wallcolor(id - ID_WCOLOR);
        for (int i = 0; i < NWP; i++) if (!WP[i].stem && thumb[i]) { kfree(thumb[i]); thumb[i] = 0; thumb_tried[i] = 0; }
        gui_clear_wallpaper_image();
        return;
    }
    if (id >= ID_RES && id < ID_RES + settings_res_count()) { g_settings.res_index = id - ID_RES; settings_apply(); return; }
    if (id >= ID_SCALE && id < ID_SCALE + settings_scale_count()) { g_settings.scale_index = id - ID_SCALE; settings_apply(); return; }
    if (id >= ID_AUDIO && id < ID_AUDIO + settings_audio_count()) { g_settings.audio_device = id - ID_AUDIO; settings_apply(); return; }
    if (id >= ID_FONT && id < ID_FONT + settings_font_count()) { g_settings.font = id - ID_FONT; settings_apply(); return; }
    if (id >= ID_MOTION && id < ID_MOTION + MOTION_COUNT) { g_settings.motion = id - ID_MOTION; gui_motion_apply(); return; }
    if (id == ID_TEST) {
        /* 880 Hz for ~200 ms; the tick silences it. Honours Muted. */
        pcspk_tone(880);
        beep_off_at = pit_ticks() + 200;
        return;
    }
    if (id == ID_MIXER) { mixer_app_open(); return; }
    if (id == SL_BASE + 0) { backlight_set(ui_slider_value(sl_x[0], sl_w[0], lx_desktop) < 10 ? 10 : ui_slider_value(sl_x[0], sl_w[0], lx_desktop)); return; }
    if (id == SL_BASE + 1) { audio_set_volume(ui_slider_value(sl_x[1], sl_w[1], lx_desktop)); return; }
}

static void settings_click(window_t *w, int lx, int ly) {
    int id = ui_hit(&U, lx, ly);
    drag_id = (id == SL_BASE + 0 || id == SL_BASE + 1) ? id : -1;
    if (id >= 0) act(id, w->x + lx);
}

static void settings_drag(window_t *w, int lx, int ly) {
    (void)ly;
    if (drag_id >= 0) act(drag_id, w->x + lx);
}

static void settings_tick(window_t *w) {
    if (beep_off_at && pit_ticks() >= beep_off_at) { pcspk_off(); beep_off_at = 0; }
    if (page == PG_PERSONAL) thumbs_step();
    static int last_bright = -1, last_vol = -1;
    if (backlight_get() != last_bright || audio_volume() != last_vol) {
        last_bright = backlight_get(); last_vol = audio_volume();
        gui_redraw_window(w);
    }
}

static void settings_key(window_t *w, char c) {
    (void)w;
    unsigned char u = (unsigned char)c;
    if (u == KEY_UP   && page > 0)            page--;
    if (u == KEY_DOWN && page < PG_COUNT - 1) page++;
}

void settings_app_init(void) {
    window_t *win = gui_add_window("Settings", 860, 600, COL_ACCENT, ICON_SETTINGS);
    if (!win) return;
    self = win;
    win->draw  = settings_draw;
    win->click = settings_click;
    win->drag  = settings_drag;
    win->tick  = settings_tick;
    win->key   = settings_key;
    win->track_hover = 1;
    win->lazy_tick = 1;
    win->min_w = 640; win->min_h = 440;
    win->x = 90; win->y = 50;
}
