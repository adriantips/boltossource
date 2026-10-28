/* ===========================================================================
 *  BoltOS Mini  -  mini/app_settings.c
 *
 *  Five pages: Appearance, Screen Sharing, Date & Time, About, Power. Up and
 *  down move through the list on the left; Right or Return goes into a page,
 *  where Left and Right change a value and Return does a thing.
 * ===========================================================================*/
#include <stdint.h>
#include "ui.h"
#include "kheap.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

enum { P_LOOK, P_SHARE, P_TIME, P_ABOUT, P_POWER, NPAGES };
#ifdef MINI_RELEASE
static const char *const pages[NPAGES] = { "Appearance", "Network", "Date & Time", "About", "Power" };
static const int page_glyph[NPAGES] = { G_DOT, G_GLOBE, G_CLOCK, G_CHECK, G_POWER };
#else
static const char *const pages[NPAGES] = { "Appearance", "Screen Sharing", "Date & Time", "About", "Power" };
static const int page_glyph[NPAGES] = { G_DOT, G_MAC, G_CLOCK, G_CHECK, G_POWER };
#endif
static int page, in_page, item, confirm;
static char pw_edit[9]; static int pw_editing, pw_len;

static int items(void) {
    switch (page) {
    case P_LOOK: return 2;
#ifdef MINI_RELEASE
    case P_SHARE: return 0;
#else
    case P_SHARE: return 1;
#endif
    case P_TIME: return 3;
    case P_POWER: return 2;
    }
    return 0;
}

static void nudge_time(int dh, int dm) {
    uint64_t t = clock_unix();
    if (!clock_known()) t = 1798761600ull + now_ms() / 1000;   /* 2027-01-01, until told */
    t = (uint64_t)((int64_t)t + dh * 3600 + dm * 60);
    clock_set_unix(t);
}

static void change(int dir) {
    if (page == P_LOOK && item == 0) theme_set((th.wallpaper + 3 + dir) % 3, th.accent_i);
    else if (page == P_LOOK && item == 1) theme_set(th.wallpaper, (th.accent_i + 6 + dir) % 6);
    else if (page == P_TIME && item == 0) nudge_time(dir, 0);
    else if (page == P_TIME && item == 1) nudge_time(0, dir);
    else if (page == P_TIME && item == 2) {
        int tz = clock_tz_minutes() + dir * 60;
        if (tz >= -12 * 60 && tz <= 14 * 60) clock_set_tz_minutes(tz);
    }
}

static void activate(void) {
    if (page == P_SHARE && item == 0) { pw_editing = 1; pw_len = 0; pw_edit[0] = 0; }
    else if (page == P_POWER) {
        if (confirm == item + 1) { if (item == 0) board_reboot(); else board_poweroff(); }
        confirm = item + 1;
    } else change(1);
}

static void s_open(void) { in_page = 0; confirm = 0; pw_editing = 0; }

static void s_key(const input_ev_t *e) {
    if (pw_editing) {
        if (e->key == KEY_CHAR && pw_len < 8 && e->ch > 32 && e->ch < 127) { pw_edit[pw_len++] = (char)e->ch; pw_edit[pw_len] = 0; }
        else if (e->key == KEY_BACKSPACE && pw_len) pw_edit[--pw_len] = 0;
        else if (e->key == KEY_ENTER) {
            if (pw_len >= 4) { snprintf(g_vnc_password, sizeof g_vnc_password, "%s", pw_edit); shell_toast("Password changed", "for screen sharing"); }
            pw_editing = 0;
        }
        return;
    }
    if (!in_page) {
        if (e->key == KEY_UP) page = (page + NPAGES - 1) % NPAGES;
        else if (e->key == KEY_DOWN) page = (page + 1) % NPAGES;
        else if ((e->key == KEY_RIGHT || e->key == KEY_ENTER) && items()) { in_page = 1; item = 0; confirm = 0; }
        return;
    }
    switch (e->key) {
    case KEY_UP:    if (item > 0) item--; confirm = 0; break;
    case KEY_DOWN:  if (item < items() - 1) item++; confirm = 0; break;
    case KEY_LEFT:
        if ((page == P_LOOK || page == P_TIME)) change(-1);
        else { in_page = 0; confirm = 0; }
        break;
    case KEY_RIGHT: change(1); break;
    case KEY_ENTER: activate(); break;
    case KEY_BACKSPACE: in_page = 0; confirm = 0; break;
    }
}

static int list_x, list_y, list_w, row_h, pane_x, pane_y, pane_w;

static void s_pointer(const input_ev_t *e, int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
    static int was;
    int click = (e->buttons & 1) && !(was & 1);
    was = e->buttons;
    if (!click) return;
    for (int i = 0; i < NPAGES; i++)
        if (ui_hit(e->x, e->y, list_x, list_y + i * row_h, list_w, row_h)) { page = i; in_page = 0; confirm = 0; return; }
    for (int i = 0; i < items(); i++)
        if (ui_hit(e->x, e->y, pane_x, pane_y + i * S(46), pane_w, S(40))) {
            in_page = 1;
            if (item == i) activate(); else { item = i; confirm = 0; }
            return;
        }
}

static void heading(int x, int y, const char *t, const char *sub) {
    text_l(x, y, t, FONT_SEMIBOLD, S(22), th.text);
    if (sub) text_l(x, y + S(28), sub, FONT_REGULAR, S(13), th.sub);
}

static void s_draw(int x, int y, int w, int h) {
    (void)h;
    char b[96];
    /* the list */
    list_x = x + S(16); list_y = y + S(16); list_w = S(220); row_h = S(44);
    for (int i = 0; i < NPAGES; i++) {
        int ry = list_y + i * row_h;
        if (i == page) gfx_rrect(list_x, ry, list_w, row_h - S(6), S(10), th.accent, in_page ? 40 : 90);
        ui_glyph(page_glyph[i], list_x + S(22), ry + (row_h - S(6)) / 2, S(i == P_LOOK ? 9 : 20), i == page ? th.text : th.sub);
        text_l(list_x + S(42), ry + (row_h - S(6)) / 2, pages[i], i == page ? FONT_SEMIBOLD : FONT_MEDIUM, S(14),
               i == page ? th.text : th.sub);
    }
    gfx_fill(list_x + list_w + S(16), y + S(16), 1, h - S(32), th.faint);

    /* the page */
    pane_x = list_x + list_w + S(40); pane_w = x + w - S(28) - pane_x;
    int hy = y + S(40);
    pane_y = hy + S(64);
    int sel = in_page ? item : -1;
    switch (page) {
    case P_LOOK:
        heading(pane_x, hy, "Appearance", "The wallpaper and the colour that marks what is chosen.");
        ui_row(pane_x, pane_y, pane_w, "Wallpaper", wallpaper_names[th.wallpaper], sel == 0);
        ui_row(pane_x, pane_y + S(46), pane_w, "Accent", accent_names[th.accent_i], sel == 1);
        for (int i = 0; i < 3; i++) {                      /* little previews     */
            int px = pane_x + i * S(94), py = pane_y + S(120);
            static const uint32_t prev[3][2] = { { 0x1B2440, 0x4A3A7A }, { 0xDCE4F5, 0xF5D8E6 }, { 0x07090F, 0x1E2A5A } };
            gfx_rrect_vgrad(px, py, S(80), S(52), S(10), prev[i][0], prev[i][1], 255);
            if (i == th.wallpaper) gfx_rrect_stroke(px - S(3), py - S(3), S(86), S(58), S(12), S(2), th.accent, 255);
            text_c(px + S(40), py + S(66), wallpaper_names[i], FONT_MEDIUM, S(11), th.sub);
        }
        text_l(pane_x, pane_y + S(214), "Left and right change the chosen row.", FONT_REGULAR, S(12), th.faint);
        break;
#ifdef MINI_RELEASE
    case P_SHARE: {                                        /* the release build's Network page */
        netif_t *e = eth_netif();
        heading(pane_x, hy, "Network", e && e->up ? "The Ethernet socket is connected." : "Plug a network cable into the Ethernet socket.");
        char ip[16] = "", gw[16] = "", dns[16] = "";
        if (e && e->ip) { net_fmt_ip(e->ip, ip); net_fmt_ip(e->gw, gw); net_fmt_ip(e->dns, dns); }
        const char *k[] = { "Ethernet", "Address", "Router", "DNS", "Clock" };
        char v[5][48];
        int sp = eth_link();
        snprintf(v[0], 48, "%s", sp ? (sp == 1000 ? "1 Gbit/s" : sp == 100 ? "100 Mbit/s" : "10 Mbit/s") : "No cable");
        snprintf(v[1], 48, "%s", ip[0] ? ip : TXT_ENDASH);
        snprintf(v[2], 48, "%s", gw[0] ? gw : TXT_ENDASH);
        snprintf(v[3], 48, "%s", dns[0] ? dns : TXT_ENDASH);
        snprintf(v[4], 48, "%s", clock_known() ? "Set from the network" : "Not set yet");
        for (int i = 0; i < 5; i++) {
            text_l(pane_x + S(14), pane_y + i * S(30), k[i], FONT_MEDIUM, S(14), th.sub);
            text_l(pane_x + S(130), pane_y + i * S(30), v[i], FONT_REGULAR, S(14), th.text);
        }
        break;
    }
#else
    case P_SHARE: {
        heading(pane_x, hy, "Screen Sharing", "Your Mac sees and drives this screen over the USB-C cable.");
        if (pw_editing) snprintf(b, sizeof b, "%s_", pw_edit);
        ui_row(pane_x, pane_y, pane_w, pw_editing ? "New password (4-8, Return)" : "Password",
               pw_editing ? b : g_vnc_password, sel == 0);
        int ty = pane_y + S(70);
        const char *addr = g_plat == PLAT_QEMU ? "vnc://localhost:5901" : "vnc://10.77.0.1";
        snprintf(b, sizeof b, "Address    %s", addr);
        text_l(pane_x + S(14), ty, b, FONT_REGULAR, S(14), th.text);
        snprintf(b, sizeof b, "Viewers    %d", vnc_clients());
        text_l(pane_x + S(14), ty + S(26), b, FONT_REGULAR, S(14), th.text);
        text_l(pane_x + S(14), ty + S(66), "On the Mac: Finder " TXT_RARROW " Go " TXT_RARROW " Connect to Server,", FONT_REGULAR, S(13), th.sub);
        snprintf(b, sizeof b, "then %s and this password.", addr);
        text_l(pane_x + S(14), ty + S(88), b, FONT_REGULAR, S(13), th.sub);
        break;
    }
#endif
    case P_TIME: {
#ifdef MINI_RELEASE
        heading(pane_x, hy, "Date & Time", clock_known() ? "Set from the network, or by hand here." :
                "Not set yet. It sets itself once the Pi is online.");
#else
        heading(pane_x, hy, "Date & Time", clock_known() ? "Set from your Mac or the network, or by hand here." :
                "Not set yet. Open http://10.77.0.1 on your Mac to copy its time.");
#endif
        int yy, mo, d, hh, mi, s, wd;
        clock_local(&yy, &mo, &d, &hh, &mi, &s, &wd);
        snprintf(b, sizeof b, "%d", hh); ui_row(pane_x, pane_y, pane_w, "Hour", b, sel == 0);
        snprintf(b, sizeof b, "%02d", mi); ui_row(pane_x, pane_y + S(46), pane_w, "Minute", b, sel == 1);
        int tz = clock_tz_minutes();
        snprintf(b, sizeof b, "UTC%s%d", tz < 0 ? TXT_ENDASH : "+", abs(tz) / 60);
        ui_row(pane_x, pane_y + S(92), pane_w, "Time zone", b, sel == 2);
        break;
    }
    case P_ABOUT: {
        heading(pane_x, hy, "BoltOS Mini", mini_version());
        uint64_t used, total; kheap_usage(&used, &total);
        char up[32]; mini_uptime_str(up, sizeof up);
        const char *k[] = { "Board", "Processor", "Memory", "Screen", "Up", "Fonts" };
        char v[6][64];
        snprintf(v[0], 64, "%s", g_board_name);
        if (board_cpu_mhz()) snprintf(v[1], 64, "4 x Cortex-A53 at %u MHz", board_cpu_mhz());
        else snprintf(v[1], 64, "Cortex-A53, emulated");
        snprintf(v[2], 64, "%lu MiB", (unsigned long)(g_ram_size >> 20));
#ifdef MINI_RELEASE
        snprintf(v[3], 64, "%d x %d", scr_w, scr_h);
#else
        snprintf(v[3], 64, "%d x %d, %s", scr_w, scr_h, g_plat == PLAT_QEMU ? "QEMU window and VNC" : "over VNC");
#endif
        snprintf(v[4], 64, "%s, boot #%u", up, klog_boot_count());
        snprintf(v[5], 64, "Inter, DejaVu Sans Mono");
        for (int i = 0; i < 6; i++) {
            text_l(pane_x + S(14), pane_y + i * S(30), k[i], FONT_MEDIUM, S(14), th.sub);
            text_fit_l(pane_x + S(130), pane_y + i * S(30), v[i], pane_w - S(140), FONT_REGULAR, S(14), th.text);
        }
        text_l(pane_x + S(14), pane_y + S(200), "BoltOS is written from scratch. This edition runs on Allwinner H618 boards.",
               FONT_REGULAR, S(12), th.faint);
        break;
    }
    case P_POWER:
#ifdef MINI_RELEASE
        heading(pane_x, hy, "Power", "The Pi runs from its USB-C power.");
#else
        heading(pane_x, hy, "Power", "The Pi takes its power from the Mac's USB port.");
#endif
        ui_row(pane_x, pane_y, pane_w, "Restart", confirm == 1 ? "Return again to restart" : 0, sel == 0);
        ui_row(pane_x, pane_y + S(46), pane_w, "Power off", confirm == 2 ? "Return again to stop" : 0, sel == 1);
        text_l(pane_x + S(14), pane_y + S(110), "Power off stops BoltOS and leaves only the red light on.",
               FONT_REGULAR, S(12), th.faint);
        text_l(pane_x + S(14), pane_y + S(130), "Unplug and replug the cable to start it again.", FONT_REGULAR, S(12), th.faint);
        break;
    }
}

const app_t app_settings = { "Settings", 0x6B7280, G_SETTINGS, s_open, s_key, s_pointer, s_draw, 0 };
