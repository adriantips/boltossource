/* ===========================================================================
 *  BoltOS Mini  -  mini/shell.c
 *
 *  Home, and the frame of every app. Home is a clock and a row of six apps
 *  on a soft wallpaper: arrows and Return, the number keys, or a click open
 *  one; Esc or the Windows/Command key comes back. An open app gets the whole
 *  screen with a slim title above it.
 *
 *  Frames are drawn only when something changed -- a key, a pointer move,
 *  a toast fading, the minute turning -- and always whole, into the back
 *  buffer, which screen_present then copies to the front in one go. The
 *  wallpaper and its blurred twin (what glass shows) are made once.
 * ===========================================================================*/
#include <stdint.h>
#include "ui.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"
#include "math.h"

static const app_t *const apps[] = { &app_browser, &app_terminal, &app_monitor, &app_notes, &app_clock, &app_settings };
#define NAPPS 6

theme_t th;
const char *const wallpaper_names[3] = { "Dusk", "Dawn", "Midnight" };
const char *const accent_names[6] = { "Violet", "Blue", "Teal", "Green", "Orange", "Pink" };
static const uint32_t accents[6] = { 0x8B7CF6, 0x4F8DF7, 0x2BB5B0, 0x4CC27A, 0xF29A4A, 0xEC6FA8 };

static surface_t wall, wall_blur;
static int theme_gen;                    /* bumped by theme_set: glass recaptures */
static const app_t *cur;                 /* NULL: home                          */
static int sel;                          /* home: the selected tile             */
static int dirty = 1;
static uint64_t last_frame;
static int pointer_x = -1, pointer_y = -1, pointer_src, pointer_btn;

/* ---- the wallpaper ------------------------------------------------------------------
 *  Computed, not stored: a vertical gradient with four soft lights in it,
 *  worked out at a quarter of the resolution (it has no detail to lose) and
 *  stretched up. Then a blurred copy, for glass to show.                     */
typedef struct { float x, y, r; uint32_t c; float k; } light_t;
typedef struct { uint32_t top, bot; light_t l[4]; } paper_t;
static const paper_t papers[3] = {
    { 0x101A2E, 0x2A1E44, { { .18f, .30f, .55f, 0x5B4BC4, .55f }, { .82f, .22f, .45f, 0x2C6FB8, .45f },
                            { .70f, .95f, .60f, 0xB0507A, .35f }, { .30f, .90f, .40f, 0x2A8C9A, .25f } } },
    { 0xE9EEF7, 0xF6E6EE, { { .20f, .25f, .55f, 0xB9C8F5, .70f }, { .85f, .30f, .50f, 0xF4C9DE, .65f },
                            { .55f, .95f, .60f, 0xC9E8E0, .55f }, { .10f, .90f, .40f, 0xF7DCC0, .45f } } },
    { 0x07090F, 0x0D1020, { { .25f, .20f, .50f, 0x1E2A5A, .55f }, { .80f, .75f, .55f, 0x2A1B4A, .50f },
                            { .55f, .40f, .35f, 0x123A4A, .30f }, { .10f, .95f, .30f, 0x1A1A3A, .30f } } },
};

static void make_wallpaper(void) {
    int w = scr_w, h = scr_h, qw = (w + 3) / 4, qh = (h + 3) / 4;
    uint32_t *q = kmalloc((uint64_t)qw * qh * 4);
    if (!wall.px || wall.w != w || wall.h != h) {
        if (wall.px) kfree(wall.px);
        if (wall_blur.px) kfree(wall_blur.px);
        wall.px = kmalloc((uint64_t)w * h * 4); wall.w = w; wall.h = h; wall.stride = w;
        wall_blur.px = kmalloc((uint64_t)w * h * 4); wall_blur.w = w; wall_blur.h = h; wall_blur.stride = w;
    }
    const paper_t *p = &papers[th.wallpaper];
    float aspect = (float)w / (float)h;
    for (int y = 0; y < qh; y++) {
        float fy = (float)y / (float)(qh - 1);
        float r = (float)((p->top >> 16) & 255) * (1 - fy) + (float)((p->bot >> 16) & 255) * fy;
        float g = (float)((p->top >> 8) & 255) * (1 - fy) + (float)((p->bot >> 8) & 255) * fy;
        float b = (float)(p->top & 255) * (1 - fy) + (float)(p->bot & 255) * fy;
        for (int x = 0; x < qw; x++) {
            float fx = (float)x / (float)(qw - 1);
            float rr = r, gg = g, bb = b;
            for (int i = 0; i < 4; i++) {
                const light_t *l = &p->l[i];
                float dx = (fx - l->x) * aspect, dy = fy - l->y;
                float d2 = (dx * dx + dy * dy) / (l->r * l->r);
                float a = l->k * expf(-d2 * 2.2f);
                rr += ((float)((l->c >> 16) & 255) - rr) * a;
                gg += ((float)((l->c >> 8) & 255) - gg) * a;
                bb += ((float)(l->c & 255) - bb) * a;
            }
            int R = (int)rr, G = (int)gg, B = (int)bb;
            R = R < 0 ? 0 : R > 255 ? 255 : R; G = G < 0 ? 0 : G > 255 ? 255 : G; B = B < 0 ? 0 : B > 255 ? 255 : B;
            q[y * qw + x] = 0xFF000000u | (uint32_t)R << 16 | (uint32_t)G << 8 | (uint32_t)B;
        }
    }
    gfx_bind(&wall, 0, 0, 0, 0, w, h);
    gfx_image(0, 0, w, h, q, qw, qh, 255);
    kfree(q);
    glass_t g = { .tint = th.dark ? 0x101018 : 0xFFFFFF, .tint_a = th.dark ? 70 : 90, .sat = 120,
                  .blur = S(28), .lift = th.dark ? 6 : 0 };
    gfx_blur_surface(&wall, &wall_blur, &g);
}

void theme_set(int wp, int ac) {
    th.wallpaper = wp < 0 ? 0 : wp > 2 ? 2 : wp;
    th.accent_i = ac < 0 ? 0 : ac > 5 ? 5 : ac;
    th.accent = accents[th.accent_i];
    th.dark = th.wallpaper != 1;
    if (th.dark) { th.text = 0xF4F4F8; th.sub = 0xB4B6C4; th.faint = 0x7C7F92; th.panel = 0x1A1C28; th.panel_a = 110; }
    else         { th.text = 0x1C1C24; th.sub = 0x55586A; th.faint = 0x8A8D9C; th.panel = 0xFFFFFF; th.panel_a = 120; }
    if (scr_w) make_wallpaper();
    theme_gen++;
    dirty = 1;
}

/* ---- drawing helpers ---------------------------------------------------------------- */
/* Nothing that moves ever sits under a card -- only the wallpaper -- so each
 * card's frosting is captured once per place and theme, then just drawn. */
static backdrop_t cards[4];
static int card_key[4][5];

void ui_card(int id, int x, int y, int w, int h, int r) {
    if (id < 0 || id > 3) id = 3;
    int k[5] = { x, y, w, h, theme_gen };
    gfx_shadow(x, y, w, h, r, S(24), S(6), 0, th.dark ? 60 : 30);
    if (!cards[id].valid || memcmp(k, card_key[id], sizeof k)) {
        glass_t g = { .tint = th.panel, .tint_a = th.panel_a, .sat = 125, .blur = S(24), .lift = th.dark ? 4 : 6 };
        gfx_backdrop_capture(&cards[id], x, y, w, h, &g);
        memcpy(card_key[id], k, sizeof k);
    }
    gfx_backdrop_draw(&cards[id], x, y, w, h, r, 255);
    gfx_rrect_stroke2(x, y, w, h, r, 0xFFFFFF, th.dark ? 46 : 150, th.dark ? 10 : 40);
}

int ui_hit(int px, int py, int x, int y, int w, int h) { return px >= x && py >= y && px < x + w && py < y + h; }

void ui_keycap(int x, int cy, const char *label) {
    int w = font_width(FONT_MEDIUM, S(11), label, -1) + S(12), h = S(18);
    gfx_rrect(x, cy - h / 2, w, h, S(5), th.dark ? 0xFFFFFF : 0x000000, th.dark ? 26 : 18);
    text_l(x + S(6), cy, label, FONT_MEDIUM, S(11), th.sub);
}

int ui_row(int x, int y, int w, const char *label, const char *value, int is_sel) {
    int h = S(38);
    if (is_sel) gfx_rrect(x, y, w, h, S(9), th.accent, th.dark ? 70 : 60);
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
    }
}

void ui_tile(const app_t *a, int x, int y, int size, int lift) {
    int r = size * 23 / 100;
    gfx_shadow(x, y, size, size, r, S(14) + lift, S(5) + lift / 2, 0, th.dark ? 90 : 50);
    uint32_t top = gfx_shade(a->tint, 118), bot = gfx_shade(a->tint, 82);
    gfx_rrect_vgrad(x, y, size, size, r, top, bot, 255);
    gfx_rrect_stroke2(x, y, size, size, r, 0xFFFFFF, 70, 0);
    ui_glyph(a->glyph, x + size / 2, y + size / 2, size * 62 / 100, 0xFFFFFF);
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
    else snprintf(date, (size_t)date_cap, "Clock not set " TXT_ENDASH " open 10.77.0.1 on your Mac");
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

/* ---- toasts -------------------------------------------------------------------------------- */
static char toast_t[48], toast_d[64];
static uint64_t toast_at;
void shell_toast(const char *title, const char *detail) {
    snprintf(toast_t, sizeof toast_t, "%s", title ? title : "");
    snprintf(toast_d, sizeof toast_d, "%s", detail ? detail : "");
    toast_at = now_ms();
    dirty = 1;
}

static int draw_toast(uint64_t now) {
    if (!toast_at) return 0;
    uint64_t age = now - toast_at;
    if (age > 3200) { toast_at = 0; return 0; }
    int a = age < 200 ? (int)(age * 255 / 200) : age > 2800 ? (int)((3200 - age) * 255 / 400) : 255;
    int tw = font_width(FONT_SEMIBOLD, S(13), toast_t, -1), dw = font_width(FONT_REGULAR, S(13), toast_d, -1);
    int w = tw + (toast_d[0] ? dw + S(10) : 0) + S(36), h = S(36);
    int x = (scr_w - w) / 2, y = S(18) - (age < 200 ? (int)(S(10) * (200 - age) / 200) : 0);
    gfx_shadow(x, y, w, h, h / 2, S(18), S(4), 0, a * 60 / 255);
    gfx_rrect(x, y, w, h, h / 2, th.dark ? 0x22242E : 0xFFFFFF, a * 235 / 255);
    gfx_circle((float)(x + S(16)), (float)(y + h / 2), (float)S(3), th.accent, a);
    font_draw(FONT_SEMIBOLD, S(13), x + S(26), y + h / 2 + S(5), toast_t, -1, th.text, a);
    if (toast_d[0]) font_draw(FONT_REGULAR, S(13), x + S(26) + tw + S(10), y + h / 2 + S(5), toast_d, -1, th.sub, a);
    return 1;
}

/* ---- home ---------------------------------------------------------------------------------- */
static int tile_size(void) { return S(74); }
static void tile_pos(int i, int *x, int *y) {
    int ts = tile_size(), gap = S(46);
    int total = NAPPS * ts + (NAPPS - 1) * gap;
    *x = (scr_w - total) / 2 + i * (ts + gap);
    *y = scr_h - S(196);
}

static void draw_status(void) {
    const char *s; uint32_t dot;
    netif_t *n = net_if(0);
    if (vnc_clients() > 0) { s = "Screen shared with your Mac"; dot = 0x4CC27A; }
    else if (n && n->up && !strcmp(n->name, "usb0")) { s = "Mac connected " TXT_ENDASH " 10.77.0.1"; dot = 0x4CC27A; }
    else if (g_plat == PLAT_QEMU) { s = "Running in QEMU on your Mac"; dot = th.accent; }
    else { s = "Waiting for the Mac"; dot = 0xF2B24A; }
    int w = font_width(FONT_MEDIUM, S(12), s, -1);
    int x = scr_w - S(48) - w;
    gfx_circle((float)(x - S(10)), (float)S(58), (float)S(3), dot, 255);
    text_l(x, S(58), s, FONT_MEDIUM, S(12), th.sub);
    int kb = usbh_keyboards();
    if (kb) {
        const char *k = kb == 1 ? "Keyboard" : "Keyboards";
        int kw = font_width(FONT_MEDIUM, S(12), k, -1);
        text_l(scr_w - S(48) - kw, S(80), k, FONT_MEDIUM, S(12), th.faint);
    }
}

static void draw_home(uint64_t now) {
    char hm[16], date[96];
    clock_text(hm, sizeof hm, date, sizeof date);
    if (clock_known()) font_draw(FONT_DISPLAY, S(92), S(48), S(48) + font_ascent(FONT_DISPLAY, S(92)), hm, -1, th.text, 255);
    else font_draw(FONT_DISPLAY, S(64), S(48), S(48) + font_ascent(FONT_DISPLAY, S(64)), "BoltOS Mini", -1, th.text, 255);
    int dy = clock_known() ? S(48) + S(92) + S(18) : S(48) + S(64) + S(20);
    text_l(S(52), dy, date, FONT_MEDIUM, S(15), th.sub);
    draw_status();

    /* A shelf of glass under the tiles, barely there. */
    int x0, y0; tile_pos(0, &x0, &y0);
    int ts = tile_size(), pad = S(26);
    int sw = NAPPS * ts + (NAPPS - 1) * S(46) + 2 * pad, sh = ts + S(64);
    ui_card(0, x0 - pad, y0 - S(22), sw, sh, S(28));
    for (int i = 0; i < NAPPS; i++) {
        int x, y; tile_pos(i, &x, &y);
        int lift = (i == sel) ? S(4) : 0;
        if (i == sel) gfx_rrect(x - S(5), y - S(5) - lift, ts + S(10), ts + S(10), (ts + S(10)) * 25 / 100,
                                th.dark ? 0xFFFFFF : 0x000000, th.dark ? 34 : 20);
        ui_tile(apps[i], x, y - lift, ts, lift);
        text_c(x + ts / 2, y + ts + S(16), apps[i]->name, i == sel ? FONT_SEMIBOLD : FONT_MEDIUM, S(12),
               i == sel ? th.text : th.sub);
    }
    (void)now;
    const char *hint = "Arrows and Return to open " TXT_BULLET " Esc comes back here";
    text_c(scr_w / 2, scr_h - S(34), hint, FONT_REGULAR, S(11), th.faint);
}

/* ---- an app's frame ---------------------------------------------------------------------------- */
static void app_area(int *x, int *y, int *w, int *h) {
    *x = S(24); *y = S(64); *w = scr_w - S(48); *h = scr_h - S(64) - S(24);
}

static void draw_app(uint64_t now) {
    int ax, ay, aw, ah;
    app_area(&ax, &ay, &aw, &ah);
    int isz = S(22);
    ui_tile(cur, S(28), S(32) - isz / 2, isz, 0);
    text_l(S(28) + isz + S(10), S(32), cur->name, FONT_SEMIBOLD, S(15), th.text);
    char hm[16], date[96];
    clock_text(hm, sizeof hm, date, sizeof date);
    int rx = scr_w - S(28);
    int kw = font_width(FONT_MEDIUM, S(11), "esc", -1) + S(12);
    ui_keycap(rx - kw, S(32), "esc");
    if (clock_known()) text_r(rx - kw - S(14), S(32), hm, FONT_MEDIUM, S(13), th.sub);
    (void)now;
    ui_card(1, ax, ay, aw, ah, S(20));
    gfx_clip_push(ax, ay, aw, ah);
    cur->draw(ax, ay, aw, ah);
    gfx_clip_pop();
}

/* ---- the pointer, when it is a USB mouse (VNC and QEMU show their own) ---------------------------- */
static void draw_pointer(void) {
    if (pointer_src != SRC_USB || pointer_x < 0) return;
    float x = (float)pointer_x, y = (float)pointer_y, k = (float)S(1);
    float px[] = { 0, 0, 4.2f, 7.0f, 9.2f, 6.6f, 11.5f }, py[] = { 0, 16.5f, 12.6f, 18.4f, 17.4f, 11.6f, 11.6f };
    for (int i = 0; i < 7; i++) {
        int j = (i + 1) % 7;
        gfx_line(x + px[i] * k, y + py[i] * k, x + px[j] * k, y + py[j] * k, 2.6f * k, 0x000000, 160);
    }
    for (float r = 1; r < 11; r += 1.0f)
        gfx_line(x + 1.2f * k, y + (2.2f + r) * k, x + (1.2f + r * 0.62f) * k, y + (2.2f + r) * k, 1.2f * k, 0xFFFFFF, 255);
    for (int i = 0; i < 7; i++) {
        int j = (i + 1) % 7;
        gfx_line(x + px[i] * k, y + py[i] * k, x + px[j] * k, y + py[j] * k, 1.1f * k, 0xFFFFFF, 255);
    }
}

/* ---- input ---------------------------------------------------------------------------------------- */
void shell_go_home(void) { cur = 0; dirty = 1; }
void shell_open(const app_t *a) {
    cur = a; dirty = 1;
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
    if (e->type == EV_POINTER) {
        int moved = e->x != pointer_x || e->y != pointer_y;
        int click = (e->buttons & 1) && !(pointer_btn & 1);
        pointer_x = e->x; pointer_y = e->y; pointer_src = e->source; pointer_btn = e->buttons;
        if (pointer_src == SRC_USB && moved) dirty = 1;
        if (!cur) {
            for (int i = 0; i < NAPPS; i++) {
                int x, y; tile_pos(i, &x, &y);
                if (ui_hit(e->x, e->y, x - S(10), y - S(10), tile_size() + S(20), tile_size() + S(40))) {
                    if (sel != i) { sel = i; dirty = 1; }
                    if (click) shell_open(apps[i]);
                }
            }
        } else {
            int kw = font_width(FONT_MEDIUM, S(11), "esc", -1) + S(12);
            if (click && ui_hit(e->x, e->y, scr_w - S(28) - kw - S(6), S(18), kw + S(12), S(28))) { shell_go_home(); return; }
            int ax, ay, aw, ah; app_area(&ax, &ay, &aw, &ah);
            if (cur->pointer) { cur->pointer(e, ax, ay, aw, ah); dirty = 1; }
        }
        return;
    }
    /* Keys every screen shares. */
    /* Esc goes home -- except in the Terminal with a half-typed line, where
     * it clears the line first (the Terminal says so by returning 1). */
    int term_ate = 0;
    if (e->key == KEY_ESC && cur == &app_terminal) { int terminal_esc(void); term_ate = terminal_esc(); }
    if (e->key == KEY_ESC && cur == &app_browser) { int browser_esc(void); term_ate = browser_esc(); }
    if (term_ate) { dirty = 1; return; }
    if (e->key == KEY_SUPER || (e->key == KEY_ESC && cur)) {
        if (cur) shell_go_home();
        return;
    }
    if (e->key >= KEY_F1 && e->key < KEY_F1 + NAPPS) { shell_open(apps[e->key - KEY_F1]); return; }
    if (cur) cur->key(e); else home_key(e);
    dirty = 1;
}

/* ---- the frame ------------------------------------------------------------------------------------ */
static int last_minute = -1, last_mac = -1, last_kb = -1;

void shell_request_frame(void) { dirty = 1; }

void shell_init(void) {
    theme_set(0, 0);
    load_t0 = now_us();
    kprintf("[shell] home: %d apps\n", NAPPS);
}

int shell_frame(void) {
    input_ev_t e;
    while (input_pop(&e)) handle(&e);
    load_sample();
    uint64_t now = now_ms();
    int minute = (int)(clock_unix() / 60);
    if (minute != last_minute) { last_minute = minute; dirty = 1; }
    int mac = shell_mac_connected(), kb = usbh_keyboards();
    if (mac != last_mac || kb != last_kb) { last_mac = mac; last_kb = kb; dirty = 1; }
    if (cur && cur->tick && cur->tick()) dirty = 1;
    if (toast_at) dirty = 1;
    if (!dirty || now - last_frame < 16) return 0;
    dirty = 0;
    last_frame = now;

    gfx_bind(&g_back, 0, 0, 0, 0, scr_w, scr_h);
    if (cur) gfx_copy(0, 0, &wall_blur, 0, 0, scr_w, scr_h);
    else     gfx_copy(0, 0, &wall, 0, 0, scr_w, scr_h);
    if (cur) draw_app(now); else draw_home(now);
    draw_toast(now);
    draw_pointer();
    screen_present(0, 0, scr_w, scr_h);
    return 1;
}
