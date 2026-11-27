/* ===========================================================================
 *  BoltOS Mini  -  mini/app_settings.c
 *
 *  Nine pages: Network (the Ethernet socket), Bluetooth, Appearance, Display,
 *  Screen Sharing, Date & Time, Updates, About, Power (eight in a release
 *  build, which has no Screen Sharing). Mini is online by Ethernet only: the
 *  Zero3's Wi-Fi antenna is gone, so Wi-Fi has no page and never starts. Up and down move through the list on the left; Right or
 *  Return goes into a page, where Left and Right change a value and Return
 *  does a thing. What takes a moment (starting Bluetooth, pairing) waits for
 *  the next frame, so the page says what it's doing first.
 * ===========================================================================*/
#include <stdint.h>
#include "ui.h"
#include "kheap.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "bt.h"
#include "update.h"

#ifdef MINI_RELEASE
enum { P_NET, P_BT, P_LOOK, P_DISPLAY, P_TIME, P_UPDATE, P_ABOUT, P_POWER, NPAGES };
static const char *const pages[NPAGES] = { "Network", "Bluetooth", "Appearance", "Display", "Date & Time", "Updates", "About", "Power" };
static const int page_glyph[NPAGES] = { G_GLOBE, G_BLUETOOTH, G_DOT, G_MONITOR, G_CLOCK, G_UPDATE, G_CHECK, G_POWER };
#else
enum { P_NET, P_BT, P_LOOK, P_DISPLAY, P_SHARE, P_TIME, P_UPDATE, P_ABOUT, P_POWER, NPAGES };
static const char *const pages[NPAGES] = { "Network", "Bluetooth", "Appearance", "Display", "Screen Sharing", "Date & Time", "Updates", "About", "Power" };
static const int page_glyph[NPAGES] = { G_GLOBE, G_BLUETOOTH, G_DOT, G_MONITOR, G_MAC, G_CLOCK, G_UPDATE, G_CHECK, G_POWER };
#endif
static int page, in_page, item, confirm;
static char pw_edit[9]; static int pw_editing, pw_len;

/* Bluetooth's scroll, and work for the next tick. */
static int btop;
enum { DO_NONE, DO_DISPLAY, DO_BT_ON, DO_BT_OFF, DO_BT_SCAN, DO_BT_PAIR, DO_BT_FORGET, DO_BT_CANCEL, DO_LCD };
static int todo, todo_arg;
static int dpick = -1;                   /* Display: the size Left/Right reached, not yet Return'd */
static uint64_t bt_last_scan;
#define WROW S(42)
static void toggle(int x, int y, int on, int busy);
static int bt_npaired(void);
static int bt_row_y(int i, int ly, int np);

static int items(void) {
    switch (page) {
    case P_NET:  return 0;
    case P_BT:   return 1 + (bt_state() >= BT_READY ? bt_device_count() : 0);
    case P_LOOK: return 2;
    case P_DISPLAY:                          /* the size, screen sleep; on the Pi the SPI screen */
        return g_plat == PLAT_H618 ? 4 + lcd_touch() : 2;
#ifndef MINI_RELEASE
    case P_SHARE: return 1;
#endif
    case P_TIME: return 3;
    case P_UPDATE: return 2;
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
    if (page == P_DISPLAY && item == 1) { screen_sleep_cycle(dir); return; }
    if (page == P_DISPLAY && item == 2) { todo = DO_LCD; todo_arg = (lcd_mode() + 3 + dir) % 3; return; }
    if (page == P_DISPLAY && item == 3) { lcd_set_flip(!lcd_flipped()); return; }
    if (page == P_DISPLAY && item == 4) { if (dir > 0) lcd_calibrate(); return; }
    if (page == P_DISPLAY) {
        int n = display_count();
        if (dpick < 0 || dpick >= n) dpick = display_current();
        dpick = (dpick + n + dir) % n;
        return;
    }
    if (page == P_LOOK && item == 0) theme_set((th.wallpaper + 3 + dir) % 3, th.accent_i);
    else if (page == P_LOOK && item == 1) theme_set(th.wallpaper, (th.accent_i + 6 + dir) % 6);
    else if (page == P_TIME && item == 0) nudge_time(dir, 0);
    else if (page == P_TIME && item == 1) nudge_time(0, dir);
    else if (page == P_TIME && item == 2) {
        int tz = clock_tz_minutes() + dir * 60;
        if (tz >= -12 * 60 && tz <= 14 * 60) clock_set_tz_minutes(tz);
    }
    if (page == P_LOOK || (page == P_TIME && item == 2)) shell_settings_save();
}

static void bt_activate(void) {
    if (item == 0) { todo = bt_state() >= BT_READY ? DO_BT_OFF : DO_BT_ON; return; }
    const bt_device_t *d = bt_device(item - 1);
    if (!d) return;
    if (d->state == BTD_PAIRING) { todo = DO_BT_CANCEL; todo_arg = item - 1; }
    else if (d->state != BTD_CONNECTED) { todo = DO_BT_PAIR; todo_arg = item - 1; }
}

/* Updates: the switch, then one row that does what fits -- check, install,
 * or restart into what's installed. */
static void update_activate(void) {
    const update_t *u = update_state();
    if (item == 0) { update_set_auto(!update_auto()); return; }
    if (u->state == UPD_RESTART) board_reboot();
    if (u->state == UPD_DOWNLOADING || u->state == UPD_INSTALLING || u->state == UPD_CHECKING) return;
    if (u->have) update_install(); else update_check();
}

static void activate(void) {
    if (page == P_BT) { bt_activate(); return; }
    if (page == P_UPDATE) { update_activate(); return; }
    if (page == P_DISPLAY) {
        if (dpick >= 0 && dpick != display_current()) { todo = DO_DISPLAY; todo_arg = dpick; }
        else change(1);
        return;
    }
#ifndef MINI_RELEASE
    if (page == P_SHARE && item == 0) { pw_editing = 1; pw_len = 0; pw_edit[0] = 0; }
    else
#endif
    if (page == P_POWER) {
        if (confirm == item + 1) { if (item == 0) board_reboot(); else board_poweroff(); }
        confirm = item + 1;
    } else change(1);
}

static void s_open(void) { in_page = 0; confirm = 0; pw_editing = 0; dpick = -1; }

void settings_show_updates(void) {
    shell_open(&app_settings);
    page = P_UPDATE; in_page = 1; item = 1; confirm = 0;
}

/* Esc: leave the password being typed (the shell asks this first). */
int settings_esc(void) {
    if (pw_editing) { pw_editing = 0; return 1; }
    return 0;
}

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
        if (page == P_LOOK || page == P_TIME || page == P_DISPLAY) change(-1);
        else if (page == P_UPDATE && item == 0 && update_auto()) update_set_auto(0);
        else { in_page = 0; confirm = 0; }
        break;
    case KEY_RIGHT:
        if (page == P_UPDATE) { if (item == 0) update_set_auto(1); }
        else change(1);
        break;
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
    if (page == P_BT) {
        int np = bt_npaired(), ly = pane_y + S(56), off = bt_row_y(btop, ly, np) - bt_row_y(0, ly, np);
        for (int i = 0; i < items(); i++) {
            int ry = i == 0 ? pane_y : bt_row_y(i - 1, ly, np) - off;
            if (i > 0 && i - 1 < btop) continue;
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

/* The Ethernet socket: Mini's only way online (the Wi-Fi antenna is gone). */
static void draw_net(int hy) {
    netif_t *e = eth_netif();
    int sp = eth_link();
    heading(pane_x, hy, "Network", sp ? "Online by Ethernet." : "Plug a network cable into the Ethernet socket.");
    char ip[16] = "", gw[16] = "", dns[16] = "";
    if (e && e->ip) { net_fmt_ip(e->ip, ip); net_fmt_ip(e->gw, gw); net_fmt_ip(e->dns, dns); }
    const char *k[] = { "Ethernet", "Address", "Router", "DNS", "Clock" };
    char v[5][48];
    snprintf(v[0], 48, "%s", sp ? (sp == 1000 ? "1 Gbit/s" : sp == 100 ? "100 Mbit/s" : "10 Mbit/s") : "No cable");
    snprintf(v[1], 48, "%s", ip[0] ? ip : sp ? "Asking the router" TXT_ELLIPSIS : TXT_ENDASH);
    snprintf(v[2], 48, "%s", gw[0] ? gw : TXT_ENDASH);
    snprintf(v[3], 48, "%s", dns[0] ? dns : TXT_ENDASH);
    snprintf(v[4], 48, "%s", clock_known() ? "Set from the network" : "Not set yet");
    for (int i = 0; i < 5; i++) {
        text_l(pane_x + S(14), pane_y + i * S(30), k[i], FONT_MEDIUM, S(14), th.sub);
        text_l(pane_x + S(130), pane_y + i * S(30), v[i], FONT_REGULAR, S(14), th.text);
    }
}

/* An on/off switch, right-aligned in a row: a flat track and a square knob. */
static void toggle(int x, int y, int on, int busy) {
    int w = S(38), hh = S(20), tx = x - w, ty = y + (S(36) - hh) / 2;
    gfx_rrect(tx, ty, w, hh, S(4), on ? th.accent : th.line, 255);
    int k = hh - S(6), kx = on ? tx + w - k - S(3) : tx + S(3);
    gfx_rrect(kx, ty + S(3), k, k, S(2), on ? 0xFFFFFF : th.faint, busy ? 150 : 255);
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

static void mb(char *out, int cap, uint64_t bytes) {
    snprintf(out, (size_t)cap, "%lu.%lu MB", (unsigned long)(bytes / 1000000), (unsigned long)(bytes % 1000000 / 100000));
}

static void ago(char *out, int cap, uint64_t when) {
    uint64_t m = (now_ms() - when) / 60000;
    if (m < 1) snprintf(out, (size_t)cap, "just now");
    else if (m < 60) snprintf(out, (size_t)cap, "%lu min ago", (unsigned long)m);
    else snprintf(out, (size_t)cap, "%lu h ago", (unsigned long)(m / 60));
}

static void draw_update(int hy, int bottom, int sel) {
    const update_t *u = update_state();
    char sub[160], b[96], v[48];
    if (u->state == UPD_CURRENT) {
        ago(b, sizeof b, u->checked);
        snprintf(sub, sizeof sub, "BoltOS Mini %s is the newest. Checked %s.", update_version(), b);
    } else if (u->state == UPD_DOWNLOADING) {
        char g[24], t[24];
        mb(g, sizeof g, u->got); mb(t, sizeof t, u->size);
        snprintf(sub, sizeof sub, "Downloading BoltOS Mini %s" TXT_ELLIPSIS "  %s of %s", u->version, g, t);
    } else snprintf(sub, sizeof sub, "%s", u->msg);
    heading(pane_x, hy, "Updates", sub);

    ui_row(pane_x, pane_y, pane_w, "Automatic updates", 0, sel == 0);
    toggle(pane_x + pane_w - S(12), pane_y, update_auto(), 0);

    const char *label = "Check for updates";
    v[0] = 0;
    switch (u->state) {
    case UPD_CHECKING:    label = "Checking" TXT_ELLIPSIS; break;
    case UPD_DOWNLOADING: label = "Downloading"; snprintf(v, sizeof v, "%u%%", u->size ? (unsigned)(u->got * 100 / u->size) : 0); break;
    case UPD_INSTALLING:  label = "Installing" TXT_ELLIPSIS; break;
    case UPD_RESTART:     label = "Restart now"; snprintf(v, sizeof v, "to finish"); break;
    default:
        if (u->have) {
            snprintf(b, sizeof b, u->state == UPD_FAILED ? "Try %s again" : "Update to %s", u->version);
            label = b; mb(v, sizeof v, u->size);
        } else if (u->checked) ago(v, sizeof v, u->checked);
    }
    int ry = pane_y + S(46);
    ui_row(pane_x, ry, pane_w, label, v[0] ? v : 0, sel == 1);
    if (u->state == UPD_DOWNLOADING && u->size) {
        int bw = pane_w - S(28), fw = (int)((uint64_t)bw * u->got / u->size);
        gfx_fill(pane_x + S(14), ry + S(40), bw, S(3), th.line);
        gfx_fill(pane_x + S(14), ry + S(40), fw, S(3), th.accent);
    }

    /* what's in it, or what's running */
    int ty = ry + S(70);
    if (u->have) {
        snprintf(b, sizeof b, "BoltOS Mini %s", u->version);
        int tw = text_l(pane_x + S(14), ty, b, FONT_SEMIBOLD, S(14), th.text);
        char sz[24]; mb(sz, sizeof sz, u->size);
        snprintf(b, sizeof b, "%s update  " "\xB7" "  %s%s%s", u->big ? "Big" : "Quick", sz, u->date[0] ? "  " "\xB7" "  " : "", u->date);
        text_l(pane_x + S(14) + tw + S(12), ty, b, FONT_REGULAR, S(13), th.sub);
        ty += S(26);
        for (const char *p = u->notes; *p && ty < bottom - S(70); ) {
            const char *e = strchr(p, '\n');
            int n = e ? (int)(e - p) : (int)strlen(p);
            char line[160];
            snprintf(line, sizeof line, "%.*s", n < 159 ? n : 159, p);
            if (line[0]) {
                gfx_fill(pane_x + S(16), ty - S(1), S(3), S(3), th.faint);
                text_fit_l(pane_x + S(28), ty, line, pane_w - S(40), FONT_REGULAR, S(13), th.sub);
                ty += S(22);
            }
            p += n + (e ? 1 : 0);
        }
    } else {
        text_l(pane_x + S(14), ty, "Installed", FONT_MEDIUM, S(14), th.sub);
        snprintf(b, sizeof b, "BoltOS Mini %s", mini_version());
        text_fit_l(pane_x + S(130), ty, b, pane_w - S(140), FONT_REGULAR, S(14), th.text);
    }
    text_l(pane_x + S(14), bottom - S(40), update_auto() ? "Quick updates install by themselves when the Pi starts. Big ones wait for you."
                                                       : "Updates wait for you: Return on the row above installs one.",
           FONT_REGULAR, S(12), th.faint);
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
        switch (t) {
        case DO_DISPLAY:  display_choose(a); dpick = -1; break;
        case DO_LCD:      lcd_set_mode(a); display_lcd_changed(); break;
        case DO_BT_ON:    if (bt_start() == 0) { bt_scan(); bt_last_scan = now_ms(); } break;
        case DO_BT_OFF:   bt_stop(); break;
        case DO_BT_SCAN:  bt_scan(); bt_last_scan = now_ms(); break;
        case DO_BT_PAIR:  bt_pair(a); break;
        case DO_BT_FORGET: bt_forget(a); break;
        case DO_BT_CANCEL: bt_cancel(a); break;
        }
        return 1;
    }
    if (page == P_BT && cur_is_settings() && bt_state() >= BT_READY && !bt_scanning() && now_ms() - bt_last_scan > 13000) {
        bt_last_scan = now_ms();
        bt_scan();
    }
    netif_t *eth = eth_netif();
    int sig = eth_link() * 1000 + (int)(eth ? eth->ip : 0) + bt_state() * 100000 + bt_device_count() * 7 +
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
        if (i == page) {
            gfx_rrect(list_x, ry, list_w, row_h - S(6), S(4), in_page ? th.surface : th.sel, 255);
            gfx_fill(list_x, ry + S(10), S(2), row_h - S(26), in_page ? th.faint : th.accent);
        }
        ui_glyph(page_glyph[i], list_x + S(22), ry + (row_h - S(6)) / 2, S(i == P_LOOK ? 9 : 20), i == page ? th.text : th.sub);
        text_l(list_x + S(42), ry + (row_h - S(6)) / 2, pages[i], i == page ? FONT_SEMIBOLD : FONT_MEDIUM, S(14),
               i == page ? th.text : th.sub);
    }
    gfx_fill(list_x + list_w + S(16), y, 1, h, th.line);

    /* the page */
    pane_x = list_x + list_w + S(40); pane_w = x + w - S(28) - pane_x;
    int hy = y + S(40);
    pane_y = hy + S(64);
    int sel = in_page ? item : -1;
    switch (page) {
    case P_NET:  draw_net(hy); break;
    case P_BT:   draw_bt(hy, y + h, sel); break;
    case P_UPDATE: draw_update(hy, y + h, sel); break;
    case P_LOOK:
        heading(pane_x, hy, "Appearance", "The theme, and the colour that marks what is chosen.");
        ui_row(pane_x, pane_y, pane_w, "Theme", wallpaper_names[th.wallpaper], sel == 0);
        ui_row(pane_x, pane_y + S(46), pane_w, "Accent", accent_names[th.accent_i], sel == 1);
        for (int i = 0; i < 3; i++) {                      /* little previews     */
            int px = pane_x + i * S(94), py = pane_y + S(120);
            static const uint32_t prev[3][3] = { { 0x17181B, 0x101113, 0x2C2E33 }, { 0xF2F3F5, 0xFFFFFF, 0xD7DAE0 },
                                                 { 0x000000, 0x0B0B0C, 0x26272B } };    /* page, bar, line */
            gfx_rrect(px, py, S(80), S(52), S(4), i == th.wallpaper ? th.accent : th.line, 255);
            gfx_rrect(px + 1, py + 1, S(80) - 2, S(52) - 2, S(3), prev[i][0], 255);
            gfx_fill(px + 1, py + 1, S(80) - 2, S(9), prev[i][1]);
            gfx_fill(px + 1, py + 1 + S(9), S(80) - 2, 1, prev[i][2]);
            gfx_fill(px + S(8), py + S(22), S(26), S(3), th.accent);
            gfx_fill(px + S(8), py + S(30), S(40), S(3), prev[i][2]);
            gfx_fill(px + S(8), py + S(38), S(32), S(3), prev[i][2]);
            text_c(px + S(40), py + S(66), wallpaper_names[i], FONT_MEDIUM, S(11), th.sub);
        }
        text_l(pane_x, pane_y + S(214), "Left and right change the chosen row.", FONT_REGULAR, S(12), th.faint);
        break;
    case P_DISPLAY: {
        char st[128], v[48];
        display_status(st, sizeof st);
        heading(pane_x, hy, "Display", todo == DO_DISPLAY ? "Switching" TXT_ELLIPSIS : st);
        int n = display_count(), cur = display_current();
        if (dpick >= n) dpick = -1;
        int shown_i = dpick >= 0 ? dpick : cur;
        display_label(shown_i, v, sizeof v);
        ui_row(pane_x, pane_y, pane_w, "Resolution", v, sel == 0);
        char sl[32]; screen_sleep_label(sl, sizeof sl);
        ui_row(pane_x, pane_y + S(46), pane_w, "Screen off", sl, sel == 1);
        if (g_plat == PLAT_H618) {                         /* the 3.5" screen on the header */
            static const char *const lm[3] = { "Automatic", "On", "Off" };
            char lv[48];
            if (todo == DO_LCD) snprintf(lv, sizeof lv, "Switching" TXT_ELLIPSIS);
            else if (lcd_mode() == 0) snprintf(lv, sizeof lv, "Automatic  " "\xB7" "  %s", lcd_on() ? "found" : "none found");
            else snprintf(lv, sizeof lv, "%s", lm[lcd_mode() % 3]);
            ui_row(pane_x, pane_y + S(92), pane_w, "SPI screen", lv, sel == 2);
            ui_row(pane_x, pane_y + S(138), pane_w, "Upside down", lcd_flipped() ? "Yes" : "No", sel == 3);
            if (lcd_touch()) ui_row(pane_x, pane_y + S(184), pane_w, "Touch", "Return calibrates", sel == 4);
        }
        int rows_end = pane_y + items() * S(46);
        if (shown_i != cur) text_l(pane_x + S(14), rows_end + S(6), "Return switches to the new size.", FONT_REGULAR, S(13), th.accent);
        int ty = rows_end + S(48);
        const char *mon = hdmi_monitor();
        snprintf(b, sizeof b, "%s", mon ? mon : "None");
        text_l(pane_x + S(14), ty, "Monitor", FONT_MEDIUM, S(14), th.sub);
        text_fit_l(pane_x + S(130), ty, b, pane_w - S(140), FONT_REGULAR, S(14), th.text);
        snprintf(b, sizeof b, "%d x %d", scr_w, scr_h);
        text_l(pane_x + S(14), ty + S(30), "Screen", FONT_MEDIUM, S(14), th.sub);
        text_l(pane_x + S(130), ty + S(30), b, FONT_REGULAR, S(14), th.text);
        const char *touch = usbh_touch_name();
        if (!touch && lcd_touch()) touch = "XPT2046, on the SPI screen";
        text_l(pane_x + S(14), ty + S(60), "Touch", FONT_MEDIUM, S(14), th.sub);
        text_fit_l(pane_x + S(130), ty + S(60), touch ? touch : "None", pane_w - S(140), FONT_REGULAR, S(14), th.text);
        text_l(pane_x + S(14), ty + S(100), mon ? "Automatic uses the size the monitor asks for. Plug one in any time."
                                               : "Plug a monitor or an HDMI LCD into the HDMI port and the screen moves to it.",
               FONT_REGULAR, S(12), th.faint);
        text_l(pane_x + S(14), ty + S(120), touch ? "Tap to click, slide to scroll, hold then move to drag."
                                                  : "An LCD's touch works once its USB cable is in the Pi's USB-A port.",
               FONT_REGULAR, S(12), th.faint);
        text_l(pane_x + S(14), ty + S(140), "Left and right change the chosen row; Return switches the size.", FONT_REGULAR, S(12), th.faint);
        text_l(pane_x + S(14), ty + S(160), "Screen off: no picture after that long without input, and nothing is drawn. Any key or click wakes it.",
               FONT_REGULAR, S(12), th.faint);
        break;
    }
#ifndef MINI_RELEASE
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
        {
            int lx, ly, ms = S(52);
            boltmark_center(pane_x + S(20), hy + S(14), ms, &lx, &ly);
            boltmark_draw(lx, ly, ms, th.text, 255);
        }
        heading(pane_x + S(52), hy, "BoltOS Mini", mini_version());
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
        snprintf(v[3], 64, "%d x %d, %s", scr_w, scr_h, g_plat == PLAT_QEMU ? "QEMU window and VNC" :
                 hdmi_monitor() && lcd_lit() ? "HDMI, SPI screen and VNC" : lcd_lit() ? "SPI screen and VNC" :
                 hdmi_shown() ? "HDMI and VNC" : "over VNC");
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

const app_t app_settings = { "Settings", 0x6B7280, G_SETTINGS, s_open, s_key, s_pointer, s_draw, s_tick, 0 };
