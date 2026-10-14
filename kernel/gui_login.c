/* ===========================================================================
 *  BoltOS  -  kernel/gui_login.c
 *
 *  Sign-in and lock screen, and the splash in front of them at boot.
 *
 *  The same screen serves both, because they ask the same question and must
 *  be equally hard to get past. The difference is only what happens behind
 *  it: signing in starts a session, unlocking resumes one, so the lock screen
 *  keeps the account fixed and never offers to switch it.
 *
 *  The lock arms on Ctrl+Alt+L and on idle. Idle is measured from the last
 *  key or mouse event, so a machine left alone in an office does not stay
 *  open.
 *
 *  It draws straight into the backbuffer, full screen, only when something
 *  on it changed: a key, the minute, a shake. Unlocking fades it out over the
 *  live desktop, which gui.c composites underneath while that runs.
 * ===========================================================================*/
#include <stdint.h>
#include <stdio.h>
#include "gui_int.h"
#include "kprintf.h"
#include "keyboard.h"
#include "users.h"
#include "vault.h"
#include "hw.h"
#include "string.h"
#include "acpi.h"
#include "math.h"

int login_active;
static int  login_locked;         /* 1 = locking a live session, not signing in */
static char login_user[USER_NAME_MAX];
static int  login_ulen;
static char login_pass[40];
static int  login_plen;
static int  login_field;          /* 0 = username, 1 = password */
static int  login_err;
static char login_msg[64];
uint64_t    gui_last_input_tick;

/* Minutes of idleness before the session locks itself. 0 disables it. */
static int lock_idle_minutes = 10;

static uint64_t t_splash;         /* boot splash start, 0 once it is over   */
static uint64_t t_reveal;         /* the sign-in UI fading in                */
static uint64_t t_shake;          /* a wrong password                        */
static uint64_t t_out;            /* fading out over the desktop             */
static uint64_t t_in;             /* fading in over the desktop (locking)    */
static int      need;             /* redraw wanted                           */
static int      last_min = -1;
static int      hot_btn = -1;     /* 0 submit, 1 restart, 2 shut down        */
static backdrop_t bgblur;
static int      bg_for_w, bg_for_h, bg_for_ver = -1;
static int      caret_phase = -1;

#define SPLASH_MS 1100
#define REVEAL_MS 420
#define OUT_MS    360
#define IN_MS     320

void gui_lock(void);
int  gui_locked(void) { return login_active && login_locked; }
void gui_set_idle_lock(int minutes) { lock_idle_minutes = minutes; }
int  gui_idle_lock(void) { return lock_idle_minutes; }

static void reset_fields(void) {
    login_ulen = login_plen = 0; login_field = 0; login_err = 0;
    login_user[0] = login_pass[0] = 0; login_msg[0] = 0;
}

void login_begin(void) {
    login_active = 1; login_locked = 0;
    reset_fields();
    uint64_t now = gui_now();
    static int booted;
    if (!booted) { booted = 1; t_splash = now ? now : 1; gui_cursor_show(0); }  /* from boot */
    else t_in = now;                                                         /* sign out */
    t_out = 0;
    need = 1;
}

/* Lock the running session: the desktop stays exactly as it is behind the
 * panel, and the password of the *current* user unlocks it. */
void gui_lock(void) {
    if (login_active) return;
    const char *who = users_current();
    if (!who[0]) return;                     /* nobody signed in; nothing to lock */
    /* Locking the screen is the moment somebody walks away from the machine,
     * which is exactly the moment an open vault should stop being open. */
    vault_lock_all();
    login_active = 1; login_locked = 1;
    strncpy(login_user, who, sizeof login_user - 1);
    login_user[sizeof login_user - 1] = 0;
    login_ulen = (int)strlen(login_user);
    login_plen = 0; login_pass[0] = 0;
    login_field = 1;                          /* the username is not in question */
    login_err = 0; login_msg[0] = 0;
    t_in = gui_now();
    t_out = 0;
    need = 1;
    shell_close_popups();
}

void gui_logout(void) {
    users_logout();
    shell_close_popups();
    login_begin();
}

static void login_submit(void) {
    login_user[login_ulen] = 0; login_pass[login_plen] = 0;
    /* Guessing is throttled here exactly as it is at the shell: a wrong
     * password costs an escalating delay, so an unattended machine cannot be
     * brute-forced at keyboard speed. */
    uint64_t wait = users_lockout_ms();
    if (wait) {
        login_err = 1;
        snprintf(login_msg, sizeof login_msg, "Too many attempts. Wait a moment.");
        login_plen = 0; login_pass[0] = 0;
        t_shake = gui_now();
        return;
    }
    int uid = users_auth(login_user, login_pass);
    if (uid >= 0) {
        users_note_success();
        /* Signing in at the desktop is logged the way sshd logs its own: so
         * `logs` can say who was on the machine, and so a test can tell a
         * login that worked from one still waiting to be typed. */
        kprintf("[gui] %s signed in%s\n", login_user,
                login_locked ? " (session unlocked)" : "");
        if (!login_locked) users_login(uid);   /* unlocking resumes the session */
        login_active = 0; login_locked = 0;
        login_msg[0] = 0;
        login_plen = 0; login_pass[0] = 0;
        gui_last_input_tick = gui_now();
        t_out = gui_now();
        t_in = 0;
        gui_all_stale();
        gui_damage_all();
        gui_cursor_show(1);
    } else {
        users_note_failure();
        kprintf("[gui] failed sign-in for '%s'\n", login_user);
        login_err = 1;
        snprintf(login_msg, sizeof login_msg, "Incorrect password. Try again.");
        login_plen = 0; login_pass[0] = 0;
        login_field = 1;
        t_shake = gui_now();
    }
}

void login_key(char c) {
    unsigned char u = (unsigned char)c;
    need = 1;
    /* On the lock screen the account is fixed, so Tab has nothing to move to. */
    if (u == '\t') { if (!login_locked) login_field ^= 1; login_err = 0; return; }
    if (u == '\n') { if (login_field == 0 && login_ulen > 0) login_field = 1; else login_submit(); return; }
    if (u == '\b') {
        if (login_field == 0) { if (login_ulen) login_user[--login_ulen] = 0; }
        else                  { if (login_plen) login_pass[--login_plen] = 0; }
        return;
    }
    if (u == 27) { if (login_field == 1) { login_plen = 0; login_pass[0] = 0; } return; }
    if (kbd_is_text(u)) {
        login_err = 0;
        if (login_field == 0) { if (login_ulen < USER_NAME_MAX - 1) { login_user[login_ulen++] = c; login_user[login_ulen] = 0; } }
        else                  { if (login_plen < 39) { login_pass[login_plen++] = c; login_pass[login_plen] = 0; } }
    }
}

/* ---- layout ---------------------------------------------------------------- */
#define FIELD_W 300
#define FIELD_H 42
static int ui_cy(void) { int H = gui_screen_h(); return H * 54 / 100; }
static void field_rect(int k, int *x, int *y) {
    int W = gui_screen_w();
    *x = (W - FIELD_W) / 2;
    int base = ui_cy() + 92;
    if (login_locked) *y = base;
    else              *y = base + (k == 0 ? 0 : FIELD_H + 12);
}
static void submit_rect(int *cx, int *cy) {
    int fx, fy; field_rect(1, &fx, &fy);
    *cx = fx + FIELD_W - FIELD_H / 2; *cy = fy + FIELD_H / 2;
}
static void power_rect(int k, int *x, int *y, int *w, int *h) {
    int W = gui_screen_w(), H = gui_screen_h();
    *w = 44; *h = 44;
    *x = W - 24 - 44 - (1 - k) * 54; *y = H - 24 - 44;
}

void login_mouse(int x, int y, int down) {
    int nh = -1;
    int sx, sy; submit_rect(&sx, &sy);
    if ((x - sx) * (x - sx) + (y - sy) * (y - sy) <= 17 * 17) nh = 0;
    for (int k = 0; k < 2; k++) {
        int bx, by, bw, bh; power_rect(k, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) nh = 1 + k;
    }
    if (nh != hot_btn) { hot_btn = nh; need = 1; }
    gui_cursor_update();
    if (!down) return;
    if (nh == 0) { login_submit(); need = 1; return; }
    if (nh == 1) { acpi_reboot(); return; }
    if (nh == 2) { acpi_poweroff(); return; }
    for (int k = 0; k < 2; k++) {
        if (login_locked && k == 0) continue;
        int fx, fy; field_rect(k, &fx, &fy);
        if (x >= fx && x < fx + FIELD_W && y >= fy && y < fy + FIELD_H) {
            if (login_field != k) { login_field = k; need = 1; }
        }
    }
}

/* ---- drawing ---------------------------------------------------------------- */
static void ensure_bg(void) {
    uint32_t *wp = gui_wallpaper_px();
    int W = gui_screen_w(), H = gui_screen_h();
    int ver = gui_wallpaper_version();
    if (bgblur.valid && bg_for_ver == ver && bg_for_w == W && bg_for_h == H) return;
    if (!wp) return;
    surface_t s = { wp, W, H, W };
    surface_t *bb = gui_bb();
    glass_t g = { 0x0B0D14, 120, 130, 56, -6 };
    gfx_bind(&s, 0, 0, 0, 0, W, H);
    gfx_backdrop_capture(&bgblur, 0, 0, W, H, &g);
    gfx_bind(bb, 0, 0, 0, 0, W, H);
    bg_for_ver = ver; bg_for_w = W; bg_for_h = H;
}

static void shadowed(int face, int px, int cx, int base, const char *s, int a) {
    int w = font_width(face, px, s, -1);
    font_draw(face, px, cx - w / 2, base + 2, s, -1, 0x000000, a * 50 / 255);
    font_draw(face, px, cx - w / 2, base, s, -1, 0xFFFFFF, a);
}

static void draw_field(int k, int a, int dx) {
    int fx, fy; field_rect(k, &fx, &fy);
    fx += dx;
    int active = (login_field == k);
    gfx_rrect(fx, fy, FIELD_W, FIELD_H, FIELD_H / 2, 0xFFFFFF, (active ? 46 : 28) * a / 255);
    gfx_rrect_stroke2(fx, fy, FIELD_W, FIELD_H, FIELD_H / 2, 0xFFFFFF, (active ? 110 : 60) * a / 255, (active ? 40 : 20) * a / 255);
    int cy = fy + FIELD_H / 2;
    int tx = fx + 22;
    if (k == 0) {
        if (login_ulen) {
            int w = font_draw(FONT_MEDIUM, 15, tx, cy + 5, login_user, -1, 0xFFFFFF, a);
            if (active && (gui_now() / 530) % 2 == 0) gfx_fill_a(tx + w + 2, cy - 9, 2, 18, 0xFFFFFF, a);
        } else {
            font_draw(FONT_REGULAR, 15, tx, cy + 5, "Username", -1, 0xFFFFFF, 140 * a / 255);
            if (active && (gui_now() / 530) % 2 == 0) gfx_fill_a(tx, cy - 9, 2, 18, 0xFFFFFF, a);
        }
        return;
    }
    if (login_plen) {
        for (int i = 0; i < login_plen && i < 22; i++)
            gfx_circle((float)(tx + 4 + i * 12), (float)cy, 4.0f, 0xFFFFFF, a);
        if (active && (gui_now() / 530) % 2 == 0) gfx_fill_a(tx + 4 + (login_plen < 22 ? login_plen : 22) * 12 - 4, cy - 9, 2, 18, 0xFFFFFF, a);
    } else {
        font_draw(FONT_REGULAR, 15, tx, cy + 5, login_locked ? "Enter Password" : "Password", -1, 0xFFFFFF, 140 * a / 255);
        if (active && (gui_now() / 530) % 2 == 0) gfx_fill_a(tx, cy - 9, 2, 18, 0xFFFFFF, a);
    }
    /* the arrow that submits */
    int sx = fx + FIELD_W - FIELD_H / 2, sy = cy;
    int on = login_plen > 0 || hot_btn == 0;
    gfx_circle((float)sx, (float)sy, 15.0f, 0xFFFFFF, (on ? 220 : 60) * a / 255);
    glyph_draw_c(GLYPH_ARROW_R, sx, sy, 16, on ? 0x1C1C24 : 0xFFFFFF, a);
}

/* The whole sign-in screen at opacity `a`, over whatever is in the target. */
static void draw_screen(int a, int ui_a) {
    int W = gui_screen_w(), H = gui_screen_h();
    ensure_bg();
    if (bgblur.valid) gfx_backdrop_draw(&bgblur, 0, 0, W, H, 0, a);
    else              gfx_fill_a(0, 0, W, H, 0x0B0D14, a);
    /* a soft vignette, darker toward the bottom where the text sits */
    for (int i = 0; i < 6; i++) gfx_fill_a(0, H - (6 - i) * H / 12, W, H / 12, 0x000000, (10 + i * 6) * a / 255);
    int ua = ui_a * a / 255;
    if (ua <= 0) return;
    /* clock and date */
    struct rtc_time t; rtc_now(&t);
    static const char *WD[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *MO[13] = { "", "January", "February", "March", "April", "May", "June", "July",
                                  "August", "September", "October", "November", "December" };
    int y = t.year ? t.year : 2026, m = (t.mon >= 1 && t.mon <= 12) ? t.mon : 1, d = t.day ? t.day : 1;
    int mm = m, yy = y;
    if (mm < 3) { mm += 12; yy--; }
    int K = yy % 100, J = yy / 100;
    int wd = ((d + (13 * (mm + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7 + 6) % 7;
    char date[48], hm[8];
    snprintf(date, sizeof date, "%s, %d %s", WD[wd], d, MO[m]);
    snprintf(hm, sizeof hm, "%d:%02d", t.hour, t.min);
    int clock_px = H < 700 ? 88 : (H < 1000 ? 112 : 136);
    int cbase = H * 26 / 100;
    shadowed(FONT_SEMIBOLD, 20, W / 2, cbase - clock_px + 2, date, ua);
    shadowed(FONT_DISPLAY, clock_px, W / 2, cbase + 6, hm, ua);

    /* who */
    int dx = 0;
    if (t_shake) {
        float st = anim_t(gui_now(), t_shake, 460);
        if (st < 1.0f) dx = (int)(sin((double)st * 3.14159 * 6.0) * 14.0 * (1.0 - st));
        else t_shake = 0;
    }
    int cy = ui_cy();
    const char *who = login_locked ? login_user : (login_ulen ? login_user : "");
    gfx_shadow(W / 2 - 48, cy - 48, 96, 96, 48, 26, 8, 0, 90 * ua / 255);
    if (who[0]) {
        uint32_t c0 = g_theme.accent, c1 = gfx_shade(g_theme.accent, 70);
        gfx_circle((float)(W / 2), (float)cy, 48.0f, c1, ua);
        gfx_circle((float)(W / 2), (float)(cy - 4), 45.0f, c0, ua);
        char init[2] = { (char)(who[0] >= 'a' && who[0] <= 'z' ? who[0] - 32 : who[0]), 0 };
        int iw = font_width(FONT_SEMIBOLD, 40, init, -1);
        font_draw(FONT_SEMIBOLD, 40, W / 2 - iw / 2, cy + 14, init, -1, 0xFFFFFF, ua);
    } else {
        icon_draw(ICON_USER, W / 2 - 48, cy - 48, 96, ua);
    }
    gfx_ring((float)(W / 2), (float)cy, 49.0f, 1.5f, 0xFFFFFF, 70 * ua / 255);
    shadowed(FONT_SEMIBOLD, 20, W / 2, cy + 78, login_locked ? login_user : (login_ulen ? login_user : "Welcome"), ua);

    if (!login_locked) draw_field(0, ua, dx);
    draw_field(1, ua, dx);

    int fx, fy; field_rect(1, &fx, &fy);
    if (login_err) {
        const char *msg = login_msg[0] ? login_msg : "Incorrect password";
        int mw = font_width(FONT_MEDIUM, 13, msg, -1);
        gfx_rrect(W / 2 - mw / 2 - 14, fy + FIELD_H + 14, mw + 28, 28, 14, 0xFF453A, 200 * ua / 255);
        font_draw(FONT_MEDIUM, 13, W / 2 - mw / 2, fy + FIELD_H + 32, msg, -1, 0xFFFFFF, ua);
    } else {
        const char *hint = login_locked ? "Locked. Enter your password to unlock." :
                                          "Tab switches field " TXT_BULLET " default account: user / bolt";
        int hw = font_width(FONT_REGULAR, 12, hint, -1);
        font_draw(FONT_REGULAR, 12, W / 2 - hw / 2, fy + FIELD_H + 30, hint, -1, 0xFFFFFF, 150 * ua / 255);
    }
    /* restart / shut down */
    for (int k = 0; k < 2; k++) {
        int bx, by, bw, bh; power_rect(k, &bx, &by, &bw, &bh);
        int hot = hot_btn == 1 + k;
        gfx_circle((float)(bx + bw / 2), (float)(by + bh / 2), 21.0f, 0xFFFFFF, (hot ? 60 : 26) * ua / 255);
        glyph_draw_c(k ? GLYPH_POWER : GLYPH_REFRESH, bx + bw / 2, by + bh / 2, 20, 0xFFFFFF, ua);
    }
    /* bottom left: the build */
    font_draw(FONT_SEMIBOLD, 13, 26, H - 34, "BoltOS", -1, 0xFFFFFF, 200 * ua / 255);
}

static void draw_splash(uint64_t now) {
    int W = gui_screen_w(), H = gui_screen_h();
    float t = anim_t(now, t_splash, SPLASH_MS);
    gfx_fill(0, 0, W, H, 0x05060A);
    int a = (int)(255.0f * ease_out(clamp01(t * 3.0f)));
    /* a faint glow behind the mark */
    for (int r = 6; r >= 1; r--)
        gfx_circle((float)(W / 2), (float)(H / 2 - 30), 40.0f + r * 18.0f, 0x3D5BFF, (6 + (6 - r) * 3) * a / 255);
    icon_draw(ICON_START, W / 2 - 56, H / 2 - 86, 112, a);
    int ww = font_width(FONT_SEMIBOLD, 28, "BoltOS", -1);
    font_draw(FONT_SEMIBOLD, 28, W / 2 - ww / 2, H / 2 + 62, "BoltOS", -1, 0xFFFFFF, a);
    /* the spinner */
    float spin = (float)(now % 1000) * 0.36f;
    gfx_ring((float)(W / 2), (float)(H / 2 + 118), 11.0f, 2.5f, 0xFFFFFF, 30 * a / 255);
    gfx_arc((float)(W / 2), (float)(H / 2 + 118), 11.0f, 2.5f, spin, spin + 100.0f, 0xFFFFFF, 220 * a / 255);
}

int login_frame(uint64_t now) {
    surface_t *bb = gui_bb();
    int W = gui_screen_w(), H = gui_screen_h();
    int anim = 0;
    struct rtc_time t; rtc_now(&t);
    if (t.min != last_min) { last_min = t.min; need = 1; }
    int cp = (int)((now / 530) % 2);
    if (cp != caret_phase) { caret_phase = cp; need = 1; }          /* caret */
    gfx_bind(bb, 0, 0, 0, 0, W, H);
    if (t_splash) {
        if (now < t_splash + SPLASH_MS) {
            draw_splash(now);
            gui_present_all();
            return 1;
        }
        t_splash = 0;
        t_reveal = now;
    }
    if (t_reveal && now < t_reveal + REVEAL_MS) {
        float r = ease_out(anim_t(now, t_reveal, REVEAL_MS));
        draw_splash(now);
        draw_screen((int)(255 * r), (int)(255 * r));
        gui_present_all();
        return 1;
    }
    if (t_reveal) { t_reveal = 0; need = 1; gui_cursor_show(1); }
    if (t_in) {
        /* fading in over the desktop: blend a little more of the lock screen
         * over what is already there each frame. Converges, and needs no copy
         * of the desktop to fade from. */
        float r = anim_t(now, t_in, IN_MS);
        if (r < 1.0f) {
            draw_screen(70, 255);
            gui_present_all();
            gui_cursor_update();
            return 1;
        }
        t_in = 0;
        need = 1;
    }
    if (t_shake) { need = 1; anim = 1; }
    if (!need) { gui_cursor_update(); return anim; }
    need = 0;
    draw_screen(255, 255);
    gui_present_all();
    gui_cursor_show(1);
    return anim;
}

int login_transition(uint64_t now, int *alpha) {
    if (!t_out) return 0;
    float r = anim_t(now, t_out, OUT_MS);
    if (r >= 1.0f) {
        /* The last frame of a fade still had some of the lock screen in it;
         * one clean repaint, or whatever nothing else redraws keeps it. */
        t_out = 0;
        gui_damage_all();
        return 0;
    }
    if (alpha) *alpha = (int)(255.0f * (1.0f - ease_out(r)));
    return 1;
}

void login_draw_over(int alpha) {
    if (alpha <= 0) return;
    draw_screen(alpha, alpha);
}
