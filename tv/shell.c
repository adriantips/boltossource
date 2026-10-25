/* ===========================================================================
 *  BoltOS TV  -  tv/shell.c
 *
 *  The loop the television runs from power-on to power-off: read the remote,
 *  advance the film, let the screen on top of the stack draw over it, put the
 *  overlays (volume, toasts, the pointer) on top, and copy the canvas to the
 *  panel. A frame is only composed when something changed -- a new film
 *  frame (24 a second), input, an animation that has not settled -- and the
 *  rest of the time the CPU sleeps in `hlt`, which on a machine that is
 *  decoding 1080p in software is time the decoder gets instead.
 *
 *  The films are owned here, not by any screen. Two players: the one on
 *  screen, and the one being faded in. Home rotates through the set; Live TV
 *  locks it to a channel; either way the change is a cross-fade.
 * ===========================================================================*/
#include <stdint.h>
#include "tv.h"
#include "framebuffer.h"
#include "keyboard.h"
#include "keymap.h"
#include "mouse.h"
#include "xhci.h"
#include "pit.h"
#include "pmm.h"
#include "mm.h"
#include "hw.h"
#include "hw.h"
#include "ntp.h"
#include "audio.h"
#include "acpi.h"
#include "string.h"
#include "stdio.h"
#include "kprintf.h"
#include "timeout.h"

int tv_w, tv_h;
static int pw, ph;                 /* the panel                                */
static int doubled;                /* canvas is half the panel, pixel-doubled  */
static surface_t canvas;
static int dirty = 1;

void tv_redraw(void) { dirty = 1; }

/* ---- time ---------------------------------------------------------------- */
uint64_t tv_now_ms(void) { return pit_ticks(); }   /* the PIT runs at 1 kHz */

static const char *const MONTHS[] = { "", "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December" };
static const char *const DAYS[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
    "Friday", "Saturday" };
const char *tv_month_name(int m) { return (m >= 1 && m <= 12) ? MONTHS[m] : ""; }
const char *tv_day_name(int d)   { return (d >= 0 && d <= 6) ? DAYS[d] : ""; }

void tv_clock(int *h, int *m, int *s, int *wday, int *day, int *mon, int *year) {
    struct rtc_time t;
    ntp_now(&t);                                /* RTC + NTP offset */
    uint32_t u = ntp_unix_from_rtc(&t) + (uint32_t)(tvs.utc_offset * 3600);
    ntp_rtc_from_unix(u, &t);
    if (h) *h = t.hour;
    if (m) *m = t.min;
    if (s) *s = t.sec;
    if (day) *day = t.day;
    if (mon) *mon = t.mon;
    if (year) *year = t.year;
    if (wday) *wday = (int)((u / 86400 + 4) % 7);   /* 1970-01-01 was a Thursday */
}

/* ---- the films ----------------------------------------------------------- */
static player_t pl[2] = { { .film = -1 }, { .film = -1 } };
static int cur;                    /* pl[cur] is on screen                     */
static int fading;                 /* pl[cur ^ 1] is coming in                 */
static uint64_t fade_t0, film_t0;
static int locked = -1;            /* Live TV's channel, or -1                 */
static int order_i;
#define FILM_HOLD_MS  27000        /* three times round a loop, then the next  */
#define FADE_MS       1400

static void film_begin(int film) {
    if (pl[cur].film == film && !fading) return;
    if (fading && pl[cur ^ 1].film == film) return;
    if (!pl[cur].v) {                          /* nothing on screen yet */
        if (player_open(&pl[cur], film) == 0) film_t0 = tv_now_ms();
        return;
    }
    int nx = cur ^ 1;
    if (player_open(&pl[nx], film) != 0) return;
    fading = 1;
    fade_t0 = 0;                               /* starts when its first frame is in */
}

void shell_film_lock(int film) { locked = film; film_begin(film); }
void shell_film_unlock(void)   { locked = -1; film_t0 = tv_now_ms(); }
int  shell_film_current(void)  { return fading && fade_t0 ? pl[cur ^ 1].film : pl[cur].film; }
const player_t *shell_player(void) { return &pl[cur]; }

static int next_film(void) {
    if (tvs.film >= 0 && tvs.film < tv_nfilms) return tvs.film;
    order_i = (order_i + 1) % tv_nfilms;
    return order_i;
}

static int films_tick(uint64_t now) {
    int changed = 0;
    if (locked < 0 && pl[cur].v && !fading) {
        int want = (tvs.film >= 0 && tvs.film < tv_nfilms) ? tvs.film : -1;
        if (want >= 0 && pl[cur].film != want) film_begin(want);
        else if (want < 0 && now - film_t0 > FILM_HOLD_MS) film_begin(next_film());
    }
    if (fading) {
        player_t *in = &pl[cur ^ 1];
        /* Decode both while they overlap if the machine has the time; if it
         * does not, the outgoing film holds its last frame for the second and
         * a half the fade takes, which nobody sees under the fade itself. */
        int both = pl[cur].decode_us + in->decode_us < 30000;
        if (both) changed |= player_tick(&pl[cur], now);
        changed |= player_tick(in, now);
        if (in->have && !fade_t0) fade_t0 = now;
        if (fade_t0 && now - fade_t0 >= FADE_MS) {
            player_close(&pl[cur]);
            cur ^= 1;
            fading = 0;
            film_t0 = now;
            changed = 1;
        }
        if (fade_t0) changed = 1;
    } else {
        changed |= player_tick(&pl[cur], now);
    }
    return changed;
}

static void films_draw(uint64_t now, int dim) {
    if (!pl[cur].have && !(fading && pl[cur ^ 1].have)) {
        gfx_fill(0, 0, tv_w, tv_h, 0x05070B);
        return;
    }
    player_draw(&pl[cur], 0, 0, tv_w, tv_h, 255);
    if (fading && fade_t0) {
        int t = (int)((now - fade_t0) * 256 / FADE_MS);
        if (t > 256) t = 256;
        int e = t * t * (768 - 2 * t) >> 16;          /* smoothstep */
        player_draw(&pl[cur ^ 1], 0, 0, tv_w, tv_h, e * 255 / 256);
    }
    if (dim) gfx_fill_a(0, 0, tv_w, tv_h, 0x04060A, dim);
}

/* ---- the view stack ------------------------------------------------------ */
static const view_t *stack[8];
static int depth;
static uint64_t view_t0;

const view_t *tv_top(void) { return depth ? stack[depth - 1] : 0; }
float tv_view_t(void) {
    uint64_t dt = tv_now_ms() - view_t0;
    if (dt >= 320) return 1.0f;
    float t = (float)dt / 320.0f;
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

void tv_push(const view_t *v) {
    if (!v || depth >= 8) return;
    stack[depth++] = v;
    view_t0 = tv_now_ms();
    if (v->enter) v->enter();
    dirty = 1;
}

void tv_pop(void) {
    if (depth <= 1) return;
    const view_t *v = stack[--depth];
    if (v->leave) v->leave();
    view_t0 = tv_now_ms();
    if (stack[depth - 1]->enter) stack[depth - 1]->enter();
    dirty = 1;
}

void tv_go_home(void) {
    while (depth > 1) {
        const view_t *v = stack[--depth];
        if (v->leave) v->leave();
    }
    view_t0 = tv_now_ms();
    if (stack[0]->enter) stack[0]->enter();
    dirty = 1;
}

/* ---- text entry ---------------------------------------------------------- *
 * While a view has a text field focused, letters are letters: '-' is not
 * volume down and Backspace is not Back. */
static int text_mode;
void tv_text_mode(int on) { text_mode = on; }

/* ---- overlays ------------------------------------------------------------ */
static char toast_title[96], toast_detail[160];
static uint64_t toast_t0;
#define TOAST_MS 3600

void tv_toast(const char *title, const char *detail) {
    strncpy(toast_title, title ? title : "", sizeof toast_title);
    strncpy(toast_detail, detail ? detail : "", sizeof toast_detail);
    toast_t0 = tv_now_ms();
    dirty = 1;
}

static uint64_t vol_t0;
static glasscache_t gc_toast, gc_vol;

void tv_volume_changed(void) {
    audio_set_volume(tvs.muted ? 0 : tvs.volume);
    vol_t0 = tv_now_ms();
    dirty = 1;
}

static void volume_key(int k) {
    if (k == K_MUTE) tvs.muted = !tvs.muted;
    else {
        tvs.muted = 0;
        tvs.volume += k == K_VOLUP ? 5 : -5;
        if (tvs.volume < 0) tvs.volume = 0;
        if (tvs.volume > 100) tvs.volume = 100;
    }
    tv_volume_changed();
    sfx_volume();
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void draw_toast(uint64_t now) {
    if (!toast_t0 || now - toast_t0 > TOAST_MS) return;
    uint64_t dt = now - toast_t0;
    float in = dt < 300 ? (float)dt / 300.0f : 1.0f;
    float out = dt > TOAST_MS - 400 ? (float)(TOAST_MS - dt) / 400.0f : 1.0f;
    float k = in < out ? in : out;
    k = 1.0f - (1.0f - k) * (1.0f - k);
    int alpha = (int)(255 * k);
    int tw = font_width(FONT_SEMIBOLD, S(26), toast_title, -1);
    int dw = toast_detail[0] ? font_width(FONT_REGULAR, S(21), toast_detail, -1) : 0;
    int w = clampi((tw > dw ? tw : dw) + S(64), S(360), tv_w - S(200));
    int h = toast_detail[0] ? S(96) : S(68);
    int x = (tv_w - w) / 2, y = S(36) - (int)((1.0f - k) * S(40));
    ui_glass(&gc_toast, x, y, w, h, h / 2 > S(34) ? S(34) : h / 2, 175, alpha);
    if (toast_detail[0]) {
        text_c(tv_w / 2, y + S(34), toast_title, FONT_SEMIBOLD, S(26), 0xFFFFFF);
        font_draw_fit(FONT_REGULAR, S(21), x + S(32), y + S(74), toast_detail, w - S(64), 0xC9CED8, alpha);
    } else {
        text_c(tv_w / 2, y + h / 2, toast_title, FONT_SEMIBOLD, S(26), 0xFFFFFF);
    }
    dirty = 1;
}

static void draw_volume(uint64_t now) {
    if (!vol_t0 || now - vol_t0 > 2600) return;
    uint64_t dt = now - vol_t0;
    float in = dt < 200 ? (float)dt / 200.0f : 1.0f;
    float out = dt > 2200 ? (float)(2600 - dt) / 400.0f : 1.0f;
    float k = in < out ? in : out;
    int alpha = (int)(255 * k);
    int w = S(96), h = S(400);
    int x = tv_w - S(64) - w + (int)((1.0f - k) * S(60)), y = (tv_h - h) / 2;
    ui_glass(&gc_vol, x, y, w, h, S(48), 165, alpha);
    int bx = x + w / 2 - S(9), by = y + S(40), bh = h - S(150);
    gfx_rrect(bx, by, S(18), bh, S(9), 0xFFFFFF, 45 * alpha / 255);
    int v = tvs.muted ? 0 : tvs.volume;
    int fh = bh * v / 100;
    if (fh > 0) gfx_rrect(bx, by + bh - fh, S(18), fh, S(9), 0xFFFFFF, alpha);
    char num[8];
    snprintf(num, sizeof num, "%d", v);
    font_draw(FONT_SEMIBOLD, S(24), x + (w - font_width(FONT_SEMIBOLD, S(24), num, -1)) / 2,
              y + h - S(70), num, -1, 0xFFFFFF, alpha);
    art_glyph(tvs.muted || !v ? GL_MUTE : GL_VOL, x + w / 2, y + h - S(36), S(32), 0xFFFFFF, alpha);
    dirty = 1;
}

/* ---- the pointer --------------------------------------------------------- */
static int mx, my, mvis;
static uint64_t mlast;
int tv_pointer_visible(void) { return mvis; }

static void draw_pointer(uint64_t now) {
    if (!mvis) return;
    if (now - mlast > 4000) { mvis = 0; dirty = 1; return; }
    int r = S(14);
    gfx_shadow(mx - r, my - r, r * 2, r * 2, r, S(10), S(3), 0, 120);
    gfx_circle((float)mx, (float)my, (float)r, 0xFFFFFF, 235);
    gfx_ring((float)mx, (float)my, (float)r, 2.0f, 0x000000, 60);
}

/* ---- the sleep timer ----------------------------------------------------- */
static void sleep_tick(uint64_t now) {
    if (!tvs.sleep_at) return;
    if (now >= tvs.sleep_at) {
        kprintf("[tv] sleep timer: switching off\n");
        gfx_fill(0, 0, tv_w, tv_h, 0);
        acpi_poweroff();
        tvs.sleep_at = 0;                       /* no ACPI S5: stay on */
        tv_toast("Could not switch off", "This machine did not answer the ACPI power-off.");
        return;
    }
    uint64_t left = tvs.sleep_at - now;
    static uint64_t warned;
    if (left < 60000 && warned != tvs.sleep_at) {
        warned = tvs.sleep_at;
        tv_toast("Sleep timer", "BoltOS TV switches off in a minute. Press any key to keep watching.");
    }
}

/* ---- the panel ----------------------------------------------------------- */
static uint32_t *vram;
static uint32_t vpitch;

static void present(void) {
    const uint8_t *lut = settings_picture_lut();
    for (int y = 0; y < tv_h; y++) {
        const uint32_t *s = canvas.px + (int64_t)y * canvas.stride;
        if (!doubled) {
            uint32_t *d = vram + (uint64_t)y * vpitch;
            if (!lut) gfx_memcpy32(d, s, tv_w);
            else for (int x = 0; x < tv_w; x++) {
                uint32_t c = s[x];
                d[x] = ((uint32_t)lut[(c >> 16) & 0xFF] << 16) | ((uint32_t)lut[256 + ((c >> 8) & 0xFF)] << 8) |
                       lut[512 + (c & 0xFF)];
            }
        } else {
            uint32_t *d0 = vram + (uint64_t)(y * 2) * vpitch, *d1 = d0 + vpitch;
            for (int x = 0; x < tv_w; x++) {
                uint32_t c = s[x];
                if (lut) c = ((uint32_t)lut[(c >> 16) & 0xFF] << 16) | ((uint32_t)lut[256 + ((c >> 8) & 0xFF)] << 8) |
                             lut[512 + (c & 0xFF)];
                d0[x * 2] = d0[x * 2 + 1] = c;
            }
            gfx_memcpy32(d1, d0, tv_w * 2);
        }
    }
}

static void choose_canvas(void) {
    pw = (int)fb_width();
    ph = (int)fb_height();
    /* QEMU's VGA (Bochs DISPI) can be switched straight to 1080p, whatever
     * mode the BIOS offered. Only tried when that card is actually there:
     * the same ports mean nothing, or something else, on real hardware. */
    struct pci_dev d;
    if ((pw != 1920 || ph != 1080) && pci_find_by_id(0x1234, 0x1111, &d) == 0) {
        if (fb_set_mode(1920, 1080) == 0) { pw = 1920; ph = 1080; }
    }
    if (ph >= 2000 && pw >= 3000) { doubled = 1; tv_w = pw / 2; tv_h = ph / 2; }
    else { doubled = 0; tv_w = pw; tv_h = ph; }
    vram = (uint32_t *)P2V(fb_phys());
    vpitch = fb_pitch_px();
    uint64_t pages = ((uint64_t)tv_w * tv_h * 4 + 4095) / 4096;
    uint64_t pa = pmm_alloc_contig(pages);
    if (!pa) { kprintf("[tv] no memory for the canvas\n"); for (;;) __asm__ volatile("hlt"); }
    canvas.px = (uint32_t *)P2V(pa);
    canvas.w = tv_w; canvas.h = tv_h; canvas.stride = tv_w;
    kprintf("[tv] panel %dx%d, canvas %dx%d%s\n", pw, ph, tv_w, tv_h, doubled ? " (doubled)" : "");
}

/* ---- input --------------------------------------------------------------- */
static void deliver(tvkey_t k) {
    const view_t *v = tv_top();
    if (k.k == K_VOLUP || k.k == K_VOLDN || k.k == K_MUTE) { volume_key(k.k); return; }
    if (k.k == K_HOME && v != &view_home) { sfx_back(); tv_go_home(); return; }
    if (tvs.sleep_at && tvs.sleep_at - tv_now_ms() < 60000) {     /* any key keeps it on */
        tvs.sleep_at = tv_now_ms() + (uint64_t)tvs.sleep_min * 60000;
        tv_toast("Sleep timer reset", 0);
    }
    if (v && v->key) v->key(k);
    dirty = 1;
}

static void map_char(int c) {
    tvkey_t k = { K_NONE, 0 };
    switch (c) {
    case KEY_UP:    k.k = K_UP; break;
    case KEY_DOWN:  k.k = K_DOWN; break;
    case KEY_LEFT:  k.k = K_LEFT; break;
    case KEY_RIGHT: k.k = K_RIGHT; break;
    case KEY_PGUP:  k.k = K_CHUP; break;
    case KEY_PGDN:  k.k = K_CHDN; break;
    case KEY_HOME:  k.k = K_HOME; break;
    case '\n': case '\r': k.k = K_OK; break;
    case 27:        k.k = K_BACK; break;
    case '\t':      k.k = K_TAB; break;
    case '\b':      k.k = text_mode ? K_ERASE : K_BACK; break;
    default:
        if (!text_mode) {
            if (c == '+' || c == '=') { k.k = K_VOLUP; break; }
            if (c == '-' || c == '_') { k.k = K_VOLDN; break; }
            if (c == 'm' || c == 'M') { k.k = K_MUTE; break; }
            if (c == 'g' || c == 'G') { k.k = K_GUIDE; break; }
            if (c == 'i' || c == 'I') { k.k = K_INFO; break; }
            if (c == 'h' || c == 'H') { k.k = K_HOME; break; }
        }
        if (kbd_is_text((unsigned char)c)) { k.k = K_CHAR; k.ch = c; }
        break;
    }
    if (k.k != K_NONE) deliver(k);
}

static void poll_input(uint64_t now) {
    xhci_poll();
    int c;
    while ((c = kbd_trygetc()) >= 0) map_char(c);
    int a;
    while ((a = kbd_action_get()) >= 0) {
        if (a == ACT_START_MENU || a == ACT_SHOW_DESKTOP) deliver((tvkey_t){ K_HOME, 0 });
        else if (a == ACT_CLOSE_WINDOW) deliver((tvkey_t){ K_BACK, 0 });
    }
    int wheel = mouse_wheel();
    if (mouse_poll_event() || wheel) {
        uint8_t now_b = mouse_buttons(), latched = mouse_take_pressed();
        int x = mouse_x(), y = mouse_y();
        if (doubled) { x /= 2; y /= 2; }
        int moved = x != mx || y != my;
        mx = clampi(x, 0, tv_w - 1);
        my = clampi(y, 0, tv_h - 1);
        int click = (latched & MOUSE_LEFT) ? 1 : 0;
        (void)now_b;
        if (moved || click || wheel) {
            mvis = 1; mlast = now;
            const view_t *v = tv_top();
            if (v && v->pointer) v->pointer(mx, my, click, wheel);
            if (latched & MOUSE_RIGHT) deliver((tvkey_t){ K_BACK, 0 });
            dirty = 1;
        }
    }
}

/* ---- the loop ------------------------------------------------------------ */
void tv_run(void) {
    choose_canvas();
    mouse_set_bounds(pw, ph);
    gfx_bind(&canvas, 0, 0, 0, 0, tv_w, tv_h);
    settings_defaults();
    font_init();
    art_init();
    sfx_init();
    tv_volume_changed();
    vol_t0 = 0;
    settings_apply_picture();

    order_i = 0;
    film_begin(tvs.film >= 0 ? tvs.film : 0);
    tv_push(&view_home);
    netsvc_start();

    uint64_t last_min = (uint64_t)-1, frames = 0, sec0 = tv_now_ms();
    for (;;) {
        uint64_t now = tv_now_ms();
        poll_input(now);
        const view_t *v = tv_top();
        if (v->backdrop != BG_NONE && films_tick(now)) dirty = 1;
        if (v->tick) v->tick(now);
        sleep_tick(now);
        if (now / 60000 != last_min) { last_min = now / 60000; dirty = 1; }

        if (dirty) {
            dirty = 0;
            gfx_bind(&canvas, 0, 0, 0, 0, tv_w, tv_h);
            if (v->backdrop == BG_FILM) films_draw(now, 0);
            else if (v->backdrop == BG_FILM_DIM) films_draw(now, 120);
            if (v->draw) v->draw(now);
            draw_toast(now);
            draw_volume(now);
            draw_pointer(now);
            present();
            frames++;
        } else {
            __asm__ volatile("hlt");
        }
        if (now - sec0 >= 10000) {
            kprintf("[tv] %lu frames in 10 s, film decode %u us\n",
                    (unsigned long)frames, pl[cur].decode_us);
            frames = 0;
            sec0 = now;
        }
    }
}
