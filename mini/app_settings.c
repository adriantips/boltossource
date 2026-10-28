/* ===========================================================================
 *  BoltOS Mini  -  mini/app_settings.c
 *
 *  Seven pages: Wi-Fi, Bluetooth, Appearance, Screen Sharing, Date & Time,
 *  About, Power. Up and down move through the list on the left; Right or
 *  Return goes into a page, where Left and Right change a value and Return
 *  does a thing. What takes a moment (starting the radio, joining) waits for
 *  the next frame, so the page says what it's doing first.
 * ===========================================================================*/
#include <stdint.h>
#include "ui.h"
#include "kheap.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "wlan.h"
#include "bt.h"

enum { P_WIFI, P_BT, P_LOOK, P_SHARE, P_TIME, P_ABOUT, P_POWER, NPAGES };
#ifdef MINI_RELEASE
static const char *const pages[NPAGES] = { "Wi-Fi", "Bluetooth", "Appearance", "Network", "Date & Time", "About", "Power" };
static const int page_glyph[NPAGES] = { G_WIFI, G_BLUETOOTH, G_DOT, G_GLOBE, G_CLOCK, G_CHECK, G_POWER };
#else
static const char *const pages[NPAGES] = { "Wi-Fi", "Bluetooth", "Appearance", "Screen Sharing", "Date & Time", "About", "Power" };
static const int page_glyph[NPAGES] = { G_WIFI, G_BLUETOOTH, G_DOT, G_MAC, G_CLOCK, G_CHECK, G_POWER };
#endif
static int page, in_page, item, confirm;
static char pw_edit[9]; static int pw_editing, pw_len;

/* Wi-Fi: the network being typed a password for, and work for the next tick. */
static int wsel = -1, wtyping, wlen, wtop, btop;
static char wpass[64];
enum { DO_NONE, DO_WIFI_ON, DO_WIFI_OFF, DO_SCAN, DO_JOIN, DO_LEAVE, DO_BT_ON, DO_BT_OFF, DO_BT_SCAN, DO_BT_PAIR, DO_BT_FORGET, DO_BT_CANCEL };
static int todo, todo_arg;
static uint64_t last_scan, bt_last_scan;
#define WROW S(42)
static void toggle(int x, int y, int on, int busy);
static int bt_npaired(void);
static int bt_row_y(int i, int ly, int np);

static int wifi_rows(void) {
    const wifi_net_t *n;
    return wifi_state() >= WIFI_READY ? wifi_nets(&n) : 0;
}

static int items(void) {
    switch (page) {
    case P_WIFI: return 1 + wifi_rows();
    case P_BT:   return 1 + (bt_state() >= BT_READY ? bt_device_count() : 0);
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

static void wifi_activate(void) {
    if (item == 0) { todo = wifi_state() >= WIFI_READY ? DO_WIFI_OFF : DO_WIFI_ON; return; }
    const wifi_net_t *n;
    int k = wifi_nets(&n);
    if (item - 1 >= k) return;
    const wifi_net_t *w = &n[item - 1];
    if (wifi_state() >= WIFI_CONNECTING && !strcmp(wifi_ssid(), w->ssid)) {
        if (confirm == item + 1) { todo = DO_LEAVE; confirm = 0; } else confirm = item + 1;
        return;
    }
    wsel = item - 1;
    if (w->security == WIFI_SEC_WPA2) { wtyping = 1; wlen = 0; wpass[0] = 0; }
    else todo = DO_JOIN;
}

static void bt_activate(void) {
    if (item == 0) { todo = bt_state() >= BT_READY ? DO_BT_OFF : DO_BT_ON; return; }
    const bt_device_t *d = bt_device(item - 1);
    if (!d) return;
    if (d->state == BTD_PAIRING) { todo = DO_BT_CANCEL; todo_arg = item - 1; }
    else if (d->state != BTD_CONNECTED) { todo = DO_BT_PAIR; todo_arg = item - 1; }
}

static void activate(void) {
    if (page == P_WIFI) { wifi_activate(); return; }
    if (page == P_BT) { bt_activate(); return; }
    if (page == P_SHARE && item == 0) { pw_editing = 1; pw_len = 0; pw_edit[0] = 0; }
    else if (page == P_POWER) {
        if (confirm == item + 1) { if (item == 0) board_reboot(); else board_poweroff(); }
        confirm = item + 1;
    } else change(1);
}

static void s_open(void) { in_page = 0; confirm = 0; pw_editing = 0; wtyping = 0; }

/* Esc: leave the password being typed (the shell asks this first). */
int settings_esc(void) {
    if (wtyping) { wtyping = 0; wsel = -1; return 1; }
    if (pw_editing) { pw_editing = 0; return 1; }
    return 0;
}

static void s_key(const input_ev_t *e) {
    if (wtyping) {
        if (e->key == KEY_CHAR && wlen < 63 && e->ch >= 32 && e->ch < 127 && !(e->mods & MOD_CTRL)) { wpass[wlen++] = (char)e->ch; wpass[wlen] = 0; }
        else if (e->key == KEY_BACKSPACE && wlen) wpass[--wlen] = 0;
        else if (e->key == KEY_ENTER && wlen >= 8) { wtyping = 0; todo = DO_JOIN; }
        return;
    }
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
    if (page == P_BT && (e->key == KEY_DELETE || e->key == KEY_BACKSPACE) && item > 0) {
        const bt_device_t *d = bt_device(item - 1);
        if (d && d->state >= BTD_PAIRED) {                  /* forget: press it twice */
            if (confirm == item + 1) { todo = DO_BT_FORGET; todo_arg = item - 1; confirm = 0; } else confirm = item + 1;
            return;
        }
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
    if (page == P_WIFI || page == P_BT) {
        if (wtyping) return;
        int np = bt_npaired(), ly = pane_y + S(56), off = bt_row_y(btop, ly, np) - bt_row_y(0, ly, np);
        for (int i = 0; i < items(); i++) {
            int top = page == P_WIFI ? wtop : btop;
            int ry = i == 0 ? pane_y : page == P_WIFI ? pane_y + S(62) + (i - 1 - top) * WROW : bt_row_y(i - 1, ly, np) - off;
            if (i > 0 && i - 1 < top) continue;
            if (ui_hit(e->x, e->y, pane_x, ry, pane_w, S(38))) {
                in_page = 1;
                if (item == i) activate(); else { item = i; confirm = 0; }
                return;
            }
        }
        return;
    }
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

static void bars(int x, int cy, int rssi, uint32_t c) {
    int n = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        int bh = S(4) + i * S(3);
        gfx_rrect(x + i * S(5), cy + S(7) - bh, S(3), bh, S(1), c, i < n ? 255 : 60);
    }
}

static void draw_wifi(int hy, int bottom, int sel) {
    char b[120], ip[16] = "";
    int st = wifi_state();
    netif_t *wl = wifi_netif();
    if (wl && wl->ip) net_fmt_ip(wl->ip, ip);
    const char *sub = st == WIFI_OFF ? "Off." : st == WIFI_STARTING ? "Starting the radio" TXT_ELLIPSIS : b;
    if (st == WIFI_CONNECTED) snprintf(b, sizeof b, "Connected to %s%s%s", wifi_ssid(), ip[0] ? "  " "\xB7" "  " : "", ip);
    else if (st == WIFI_READY && wifi_scanning() && !wifi_rows()) snprintf(b, sizeof b, "Looking for networks" TXT_ELLIPSIS);
    else snprintf(b, sizeof b, "%s", wifi_why());
    if (todo == DO_JOIN) snprintf(b, sizeof b, "Joining" TXT_ELLIPSIS);
    if (todo == DO_WIFI_ON) sub = "Starting the radio" TXT_ELLIPSIS;
    const wifi_net_t *n;
    int k = wifi_nets(&n);
    if (wtyping && wsel >= 0 && wsel < k) {
        snprintf(b, sizeof b, "The password for %s", n[wsel].ssid);
        heading(pane_x, hy, "Wi-Fi", b);
        char dots[70];
        int d = 0;
        for (int i = 0; i < wlen && d < 60; i++) { dots[d++] = '\x95'; }
        dots[d++] = '_'; dots[d] = 0;
        ui_row(pane_x, pane_y, pane_w, "Password", dots, 1);
        text_l(pane_x + S(14), pane_y + S(64), wlen < 8 ? "At least 8 characters.  Esc goes back." :
               "Return joins.  Esc goes back.", FONT_REGULAR, S(12), th.faint);
        return;
    }
    heading(pane_x, hy, "Wi-Fi", sub);
    ui_row(pane_x, pane_y, pane_w, "Wi-Fi", 0, sel == 0);
    toggle(pane_x + pane_w - S(12), pane_y, st >= WIFI_READY || todo == DO_WIFI_ON, st == WIFI_STARTING || todo == DO_WIFI_ON);
    if (st < WIFI_READY) return;
    int ly = pane_y + S(62), rows = (bottom - S(40) - ly) / WROW;
    if (rows < 1) rows = 1;
    if (sel > 0) {                                          /* keep the chosen one in view */
        if (sel - 1 < wtop) wtop = sel - 1;
        if (sel - 1 >= wtop + rows) wtop = sel - rows;
    }
    if (wtop > k - rows) wtop = k - rows > 0 ? k - rows : 0;
    for (int i = wtop; i < k && i < wtop + rows; i++) {
        int ry = ly + (i - wtop) * WROW;
        int here = st >= WIFI_CONNECTING && !strcmp(wifi_ssid(), n[i].ssid);
        ui_row(pane_x, ry, pane_w, n[i].ssid, 0, sel == i + 1);
        int rx = pane_x + pane_w - S(36), cy = ry + S(19);
        bars(rx, cy, n[i].rssi, th.text);
        if (n[i].security != WIFI_SEC_OPEN) ui_glyph(G_LOCK, rx - S(16), cy - S(1), S(18), th.sub);
        const char *note = here ? (st == WIFI_CONNECTED ? (confirm == i + 2 ? "Return again to leave" : "Connected") : "Joining" TXT_ELLIPSIS)
                         : n[i].security == WIFI_SEC_OTHER ? "Not supported" : 0;
        if (note) text_r(rx - S(32), cy, note, FONT_REGULAR, S(13), here ? th.accent : th.faint);
    }
    if (!k && !wifi_scanning()) text_l(pane_x + S(14), ly + S(10), "No networks in range.", FONT_REGULAR, S(13), th.sub);
}

/* An on/off switch, as on a phone: a pill with a knob, right-aligned in a row. */
static void toggle(int x, int y, int on, int busy) {
    int w = S(44), hh = S(26), tx = x - w, ty = y + (S(38) - hh) / 2;
    gfx_rrect(tx, ty, w, hh, hh / 2, on ? th.accent : th.faint, on ? 255 : 110);
    int kx = on ? tx + w - hh + S(2) : tx + S(2);
    gfx_circle((float)(kx + (hh - S(4)) / 2), (float)(ty + hh / 2), (float)(hh - S(4)) / 2, 0xFFFFFF, busy ? 160 : 255);
}

/* The rows below the switch: "My Devices", then "Other Devices", each with a small
 * heading. Paired ones come first in bt_device()'s order. */
static int bt_npaired(void) {
    int n = 0;
    for (int i = 0; bt_device(i); i++) if (bt_device(i)->state >= BTD_PAIRING) n++;
    return n;
}

static int bt_row_y(int i, int ly, int np) {               /* device i's row, before scrolling */
    int y = ly + i * WROW + S(24);
    if (np && i >= np) y += S(30);
    return y;
}

static void draw_bt(int hy, int bottom, int sel) {
    int st = bt_state();
    const char *sub = st == BT_OFF ? "Off." : st == BT_STARTING ? "Starting" TXT_ELLIPSIS : bt_why();
    if (todo == DO_BT_ON) sub = "Starting" TXT_ELLIPSIS;
    heading(pane_x, hy, "Bluetooth", sub);
    ui_row(pane_x, pane_y, pane_w, "Bluetooth", 0, sel == 0);
    toggle(pane_x + pane_w - S(12), pane_y, st >= BT_READY || todo == DO_BT_ON, st == BT_STARTING || todo == DO_BT_ON);
    if (st < BT_READY) return;
    int ly = pane_y + S(56), k = bt_device_count(), np = bt_npaired();
    int view = bottom - S(56) - ly;
    if (sel > 0) {                                          /* keep the chosen one in view */
        int top = bt_row_y(btop, ly, np) - S(24), y = bt_row_y(sel - 1, ly, np);
        if (sel - 1 < btop) btop = sel - 1;
        else if (y + WROW - top > view) btop = sel - 1 - view / WROW + 1;
        if (btop < 0) btop = 0;
    }
    int off = bt_row_y(btop, ly, np) - bt_row_y(0, ly, np);
    if (np && btop == 0) text_l(pane_x + S(14), ly + S(10), "My Devices", FONT_SEMIBOLD, S(12), th.sub);
    for (int i = btop; i < k; i++) {
        const bt_device_t *d = bt_device(i);
        int ry = bt_row_y(i, ly, np) - off;
        if (ry + S(38) > bottom - S(50)) break;
        if (i == np && (np || btop == 0)) {
            text_l(pane_x + S(14), ry - S(14), "Other Devices", FONT_SEMIBOLD, S(12), th.sub);
            if (bt_scanning()) text_r(pane_x + pane_w - S(14), ry - S(14), "Looking" TXT_ELLIPSIS, FONT_REGULAR, S(12), th.faint);
        }
        ui_row(pane_x, ry, pane_w, d->name[0] ? d->name : d->addr_str, 0, sel == i + 1);
        const char *v = d->state == BTD_CONNECTED ? "Connected" : d->state == BTD_PAIRING ? "Pairing" TXT_ELLIPSIS :
                        d->state == BTD_PAIRED ? "Not Connected" : d->kind;
        if (confirm == i + 2 && d->state >= BTD_PAIRED) v = "Press Backspace again to forget";
        text_r(pane_x + pane_w - S(14), ry + S(19), v, FONT_REGULAR, S(13), d->state == BTD_CONNECTED ? th.accent : th.faint);
    }
    if (k == np) {
        int ry = bt_row_y(np, ly, np) - off;
        if (ry < bottom - S(60)) {
            text_l(pane_x + S(14), ry - S(14), "Other Devices", FONT_SEMIBOLD, S(12), th.sub);
            text_l(pane_x + S(14), ry + S(12), bt_scanning() ? "Looking" TXT_ELLIPSIS : "Put a device in pairing mode to see it here.",
                   FONT_REGULAR, S(13), th.faint);
        }
    }
    const bt_device_t *cur = sel > 0 ? bt_device(sel - 1) : 0;
    const char *hint = !cur ? "Return turns Bluetooth off." : cur->state == BTD_PAIRING ? "Return stops pairing." :
                       cur->state == BTD_CONNECTED ? "Backspace forgets it." : cur->state == BTD_PAIRED ? "Return connects.  Backspace forgets it." :
                       "Return pairs.";
    text_l(pane_x + S(14), bottom - S(40), hint, FONT_REGULAR, S(12), th.faint);
}

static uint64_t drawn_at;                /* Settings drew lately: it's the app on screen */
static int cur_is_settings(void) { return now_ms() - drawn_at < 3000; }

/* Work that takes a while: after the frame that says it's happening. */
static int s_tick(void) {
    static int shown, last_sig;
    if (todo && !shown) { shown = 1; return 1; }
    if (todo) {
        int t = todo, a = todo_arg;
        todo = DO_NONE; shown = 0;
        const wifi_net_t *n;
        int k = wifi_nets(&n);
        switch (t) {
        case DO_WIFI_ON:  if (wifi_start() == 0) { wifi_scan(); last_scan = now_ms(); } break;
        case DO_WIFI_OFF: wifi_stop(); break;
        case DO_SCAN:     wifi_scan(); last_scan = now_ms(); break;
        case DO_JOIN:     if (wsel >= 0 && wsel < k) wifi_connect(n[wsel].ssid, wpass); memset(wpass, 0, sizeof wpass); wlen = 0; wsel = -1; break;
        case DO_LEAVE:    wifi_leave(); break;
        case DO_BT_ON:    if (bt_start() == 0) { bt_scan(); bt_last_scan = now_ms(); } break;
        case DO_BT_OFF:   bt_stop(); break;
        case DO_BT_SCAN:  bt_scan(); bt_last_scan = now_ms(); break;
        case DO_BT_PAIR:  bt_pair(a); break;
        case DO_BT_FORGET: bt_forget(a); break;
        case DO_BT_CANCEL: bt_cancel(a); break;
        }
        return 1;
    }
    /* While the Wi-Fi page is up: look again now and then. */
    if (page == P_WIFI && wifi_state() >= WIFI_READY && !wifi_scanning() && !wtyping && now_ms() - last_scan > 20000) {
        last_scan = now_ms();
        wifi_scan();
    }
    if (page == P_BT && cur_is_settings() && bt_state() >= BT_READY && !bt_scanning() && now_ms() - bt_last_scan > 13000) {
        bt_last_scan = now_ms();
        bt_scan();
    }
    const wifi_net_t *n;
    int sig = wifi_state() * 1000 + wifi_nets(&n) * 10 + wifi_scanning() + bt_state() * 100000 + bt_device_count() * 7 +
              bt_scanning() * 3 + bt_changes() * 13;
    if (sig != last_sig) { last_sig = sig; return 1; }
    return 0;
}

static void s_draw(int x, int y, int w, int h) {
    (void)h;
    drawn_at = now_ms();
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
    case P_WIFI: draw_wifi(hy, y + h, sel); break;
    case P_BT:   draw_bt(hy, y + h, sel); break;
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

const app_t app_settings = { "Settings", 0x6B7280, G_SETTINGS, s_open, s_key, s_pointer, s_draw, s_tick };
