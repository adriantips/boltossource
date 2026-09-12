/* ===========================================================================
 *  BoltOS  -  kernel/app_network.c
 *  The Network window: the list of networks in range, and what it takes to
 *  join one.
 *
 *  Every control drives net/wifi_sta.c and net/netcfg.c -- the same code the
 *  `wifi` command drives -- so what the desktop believes about the radio and
 *  what a terminal believes cannot come apart. Nothing is decided here; the
 *  window only shows and asks.
 *
 *  Scanning takes a second and a half and joining takes several, and both of
 *  them block. So a click queues the work and the next tick performs it, with
 *  the window already painted saying what it is doing -- the same shape
 *  kernel/app_software.c uses for the package manager, for the same reason.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "wifi.h"
#include "netcfg.h"
#include "netif.h"
#include "net.h"
#include "dhcp.h"
#include "notify.h"
#include "string.h"
#include "stdio.h"

enum { JOB_NONE = 0, JOB_SCAN, JOB_CONNECT, JOB_DISCONNECT, JOB_FORGET, JOB_RADIO };

#define HEAD_H  98
#define ROW_H   34
#define PAD     16

static window_t *win;

static struct {
    int  job;
    int  sel;                 /* the highlighted scan row, -1 for none      */
    int  rows, cw, ch;
    int  asking;              /* the passphrase field has the keyboard      */
    char pass[64];
    int  passlen;
    char status[96];
    char job_ssid[33];
} st = { .sel = -1 };

/* --------------------------------------------------------------- drawing -- */

static const char *sec_name(const struct wifi_bss *b) {
    if (b->akm == WIFI_AKM_SAE) return "WPA3";
    if (b->rsn && b->ccmp)      return "WPA2";
    if (b->rsn)                 return "WPA2 (TKIP)";
    if (b->privacy)             return "WEP";
    return "open";
}

/* Four bars, filled by signal. -50 dBm and better is full; below -85 is one
 * bar, because a network you cannot actually use should not look like one you
 * can. */
static void bars(int x, int y, int rssi, uint32_t col) {
    int level = 1;
    if      (rssi >= -55) level = 4;
    else if (rssi >= -67) level = 3;
    else if (rssi >= -78) level = 2;
    for (int i = 0; i < 4; i++) {
        int h = 4 + i * 3;
        g_fill(x + i * 5, y + 15 - h, 3, h, i < level ? col : COL_PANEL_3);
    }
}

static void btn(int x, int y, int w, const char *label, int on) {
    g_round(x, y, w, 26, 7, on ? COL_ACCENT : COL_PANEL_3, 255);
    g_text(x + (w - g_text_width(label, 1)) / 2, y + 9, label, on ? 0xFFFFFF : COL_TEXT, 1);
}

/* The buttons sit in one row under the header; laid out once here so the draw
 * and the click agree without either of them owning the numbers. */
static void btn_rect(int cx, int cw, int i, int *bx, int *by, int *bw) {
    int w = (cw - 2 * PAD - 3 * 8) / 4;
    if (w < 70) w = 70;
    *bw = w;
    *bx = cx + PAD + i * (w + 8);
    *by = HEAD_H - 36;
    (void)cw;
}

static void draw_rows(int cx, int cy, int cw, int ch) {
    int n = wifi_scan_count();
    int top = cy + HEAD_H;
    st.rows = (ch - HEAD_H - 46) / ROW_H;
    if (st.rows < 1) st.rows = 1;

    if (n == 0) {
        g_text(cx + PAD, top + 10,
               wifi_radio_get() ? "No networks found. Scan again, or move closer."
                                : "The radio is off.", COL_TEXT_DIM, 1);
        return;
    }

    for (int r = 0; r < st.rows && r < n; r++) {
        const struct wifi_bss *b = wifi_scan_get(r);
        int y = top + r * ROW_H;
        int on = r == st.sel;
        if (on) g_round(cx + PAD - 6, y - 2, cw - 2 * PAD + 12, ROW_H - 2, 6, COL_PANEL_3, 255);

        const struct wifi_bss *cur = wifi_current();
        int joined = cur && strcmp(cur->ssid, b->ssid) == 0;

        bars(cx + PAD, y + 6, b->rssi, joined ? COL_GOOD : COL_ACCENT);
        g_text(cx + PAD + 30, y + 8, b->ssid[0] ? b->ssid : "(hidden)",
               joined ? COL_GOOD : COL_TEXT, 1);

        const char *sec = sec_name(b);
        int sx = cx + cw - PAD - g_text_width(sec, 1);
        g_text(sx, y + 8, sec, COL_TEXT_DIM, 1);

        if (joined)
            g_text(sx - 90, y + 8, "connected", COL_GOOD, 1);
        else if (netcfg_wifi_find(b->ssid, 0, 0))
            g_text(sx - 90, y + 8, "saved", COL_TEXT_DIM, 1);
    }
}

static void net_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    st.cw = cw; st.ch = ch;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    struct wifi_radio *r = wifi_radio_get();
    const struct wifi_bss *cur = wifi_current();

    g_fill(cx + PAD, cy + 14, 3, 12, COL_ACCENT);
    g_text(cx + PAD + 10, cy + 12, "Wi-Fi", COL_TEXT, 1);

    char line[120];
    if (!r) {
        snprintf(line, sizeof line, "%s", "No radio. Turn one on to look for networks.");
    } else if (cur) {
        char ip[16];
        netcfg_format_ip(net_ip, ip);
        snprintf(line, sizeof line, "%s  -  %s, channel %d, %d dBm%s%s",
                 cur->ssid, sec_name(cur), (int)cur->channel, wifi_signal(),
                 net_ip ? "  -  " : "", net_ip ? ip : "");
    } else {
        snprintf(line, sizeof line, "%s", wifi_state_name(wifi_get_state()));
    }
    g_text(cx + PAD, cy + 34, line, cur ? COL_GOOD : COL_TEXT_DIM, 1);

    /* buttons */
    int bx, by, bw;
    btn_rect(cx, cw, 0, &bx, &by, &bw);
    btn(bx, cy + by, bw, "Scan", 0);
    btn_rect(cx, cw, 1, &bx, &by, &bw);
    btn(bx, cy + by, bw, cur ? "Disconnect" : "Connect", st.sel >= 0 || cur);
    btn_rect(cx, cw, 2, &bx, &by, &bw);
    btn(bx, cy + by, bw, "Forget", st.sel >= 0);
    btn_rect(cx, cw, 3, &bx, &by, &bw);
    btn(bx, cy + by, bw, r ? "Radio off" : "Radio on", 0);

    draw_rows(cx, cy, cw, ch);

    /* The passphrase field replaces the status line while it is being typed. */
    int fy = cy + ch - 34;
    if (st.asking) {
        const struct wifi_bss *b = st.sel >= 0 ? wifi_scan_get(st.sel) : 0;
        snprintf(line, sizeof line, "Passphrase for %s:", b ? b->ssid : "?");
        g_text(cx + PAD, fy, line, COL_TEXT, 1);
        int fx = cx + PAD + g_text_width(line, 1) + 10;
        g_round(fx, fy - 5, cw - (fx - cx) - PAD, 24, 6, COL_PANEL_3, 255);
        char dots[33];
        int n = st.passlen < 32 ? st.passlen : 32;
        for (int i = 0; i < n; i++) dots[i] = '*';
        dots[n] = 0;
        g_text(fx + 8, fy, dots, COL_TEXT, 1);
        g_text(cx + cw - PAD - g_text_width("Enter to join", 1), fy - 22,
               "Enter to join", COL_TEXT_DIM, 1);
    } else if (st.status[0]) {
        g_text(cx + PAD, fy, st.status, COL_TEXT_DIM, 1);
    }
}

/* ----------------------------------------------------------------- input -- */

static void queue(int job, const char *ssid, const char *saying) {
    st.job = job;
    snprintf(st.job_ssid, sizeof st.job_ssid, "%s", ssid ? ssid : "");
    snprintf(st.status, sizeof st.status, "%s", saying);
    /* The desktop loop checks the tick timer before it composites, so without
     * this the job could finish before the frame that says it started was ever
     * drawn -- and a five-second join would look like a frozen window. */
    gui_pump();
}

static void begin_connect(void) {
    if (st.sel < 0) return;
    const struct wifi_bss *b = wifi_scan_get(st.sel);
    if (!b) return;

    char saved[80];
    if (!b->rsn && !b->privacy) {                     /* open network */
        st.pass[0] = 0; st.passlen = 0;
        queue(JOB_CONNECT, b->ssid, "Connecting...");
        return;
    }
    if (netcfg_wifi_find(b->ssid, saved, sizeof saved)) {
        snprintf(st.pass, sizeof st.pass, "%s", saved);
        st.passlen = (int)strlen(st.pass);
        memset(saved, 0, sizeof saved);
        queue(JOB_CONNECT, b->ssid, "Connecting...");
        return;
    }
    st.asking  = 1;
    st.passlen = 0;
    st.pass[0] = 0;
    gui_request_redraw();
}

static void net_click(window_t *w, int lx, int ly) {
    (void)w;
    if (st.job) return;                                /* one thing at a time */
    int cw = st.cw;
    if (!cw) return;

    for (int i = 0; i < 4; i++) {
        int bx, by, bw;
        btn_rect(0, cw, i, &bx, &by, &bw);
        if (lx < bx || lx >= bx + bw || ly < by || ly >= by + 26) continue;
        struct wifi_radio *r = wifi_radio_get();
        st.asking = 0;
        switch (i) {
        case 0:
            /* Scan with no radio bound turns one on first. Making somebody
             * press "Radio on" and then "Scan" is asking them to know the
             * difference, and the answer to "what is in range" is the same
             * either way. */
            if (!r) queue(JOB_RADIO, "on", "Starting the radio...");
            else    queue(JOB_SCAN, 0, "Scanning...");
            break;
        case 1:
            if (wifi_current()) queue(JOB_DISCONNECT, 0, "Disconnecting...");
            else                begin_connect();
            break;
        case 2:
            if (st.sel >= 0) {
                const struct wifi_bss *b = wifi_scan_get(st.sel);
                if (b) queue(JOB_FORGET, b->ssid, "");
            }
            break;
        case 3:
            queue(JOB_RADIO, r ? "off" : "on",
                  r ? "Turning the radio off..." : "Starting the radio...");
            break;
        }
        gui_request_redraw();
        return;
    }

    int top = HEAD_H;
    for (int r = 0; r < st.rows; r++) {
        int ry = top + r * ROW_H;
        if (ly < ry || ly >= ry + ROW_H) continue;
        if (r >= wifi_scan_count()) break;
        st.sel = r;
        st.asking = 0;
        gui_request_redraw();
        return;
    }
}

static void net_key(window_t *w, char c) {
    (void)w;
    if (!st.asking) return;
    if (c == '\n') {
        st.asking = 0;
        const struct wifi_bss *b = st.sel >= 0 ? wifi_scan_get(st.sel) : 0;
        if (b) queue(JOB_CONNECT, b->ssid, "Connecting...");
        return;
    }
    if (c == 27) { st.asking = 0; st.passlen = 0; st.pass[0] = 0; gui_request_redraw(); return; }
    if (c == '\b') {
        if (st.passlen) st.pass[--st.passlen] = 0;
        gui_request_redraw();
        return;
    }
    if ((unsigned char)c >= 32 && (unsigned char)c < 127 && st.passlen < (int)sizeof st.pass - 1) {
        st.pass[st.passlen++] = c;
        st.pass[st.passlen] = 0;
        gui_request_redraw();
    }
}

static void net_tick(window_t *w) {
    (void)w;
    if (!st.job) return;
    int job = st.job;
    char ssid[33];
    snprintf(ssid, sizeof ssid, "%s", st.job_ssid);
    st.job = 0;

    if (job == JOB_RADIO) {
        if (ssid[0] == 'o' && ssid[1] == 'n') {
            int rc = wifi_sim_start(0, 0);
            if (rc == 0) {
                g_netcfg.sim_radio = 1;
                netcfg_save();
                snprintf(st.status, sizeof st.status, "Radio on");
                wifi_scan(1500);
            } else {
                snprintf(st.status, sizeof st.status,
                         rc == -2 ? "The built-in radio needs a wired interface to bridge"
                                  : "The radio would not start");
            }
        } else {
            wifi_disconnect();
            wifi_sim_stop();
            g_netcfg.sim_radio = 0;
            netcfg_save();
            st.sel = -1;
            snprintf(st.status, sizeof st.status, "Radio off");
        }
    } else if (job == JOB_SCAN) {
        int n = wifi_scan(1500);
        if (n < 0) n = 0;
        if (st.sel >= n) st.sel = n ? 0 : -1;
        snprintf(st.status, sizeof st.status, n == 1 ? "1 network in range"
                                                     : "%d networks in range", n);
    } else if (job == JOB_DISCONNECT) {
        wifi_disconnect();
        snprintf(st.status, sizeof st.status, "Disconnected");
        gui_notify(NOTE_INFO, "Network", "Wi-Fi disconnected");
    } else if (job == JOB_FORGET) {
        netcfg_wifi_forget(ssid);
        snprintf(st.status, sizeof st.status, "Forgot %s", ssid);
    } else if (job == JOB_CONNECT) {
        int rc = wifi_connect(ssid, st.pass, 8000);
        if (rc == 0) {
            if (st.pass[0]) netcfg_wifi_save(ssid, st.pass);
            char msg[NOTE_TEXT_MAX];
            if (dhcp_configure(3000) == 0) {
                char ip[16];
                netcfg_format_ip(net_ip, ip);
                snprintf(msg, sizeof msg, "Connected to %s (%s)", ssid, ip);
            } else {
                snprintf(msg, sizeof msg, "Joined %s, but no address was offered", ssid);
            }
            snprintf(st.status, sizeof st.status, "%s", msg);
            gui_notify(NOTE_SUCCESS, "Network", msg);
        } else {
            snprintf(st.status, sizeof st.status, "%s: %s", ssid, wifi_err_str(rc));
            gui_notify(NOTE_ERROR, "Network", st.status);
            /* A passphrase that did not work is not one to keep offering. */
            if (rc == WIFI_EKEY) netcfg_wifi_forget(ssid);
        }
        memset(st.pass, 0, sizeof st.pass);
        st.passlen = 0;
    }
    gui_request_redraw();
}

void network_app_init(void) {
    win = gui_add_window("Network", 560, 420, 0x4FC3F7, ICON_NETWORK);
    if (!win) return;
    win->draw  = net_draw;
    win->click = net_click;
    win->key   = net_key;
    win->tick  = net_tick;
    win->min_w = 460;
    win->min_h = 300;
    win->x = 250; win->y = 90;
    snprintf(st.status, sizeof st.status, "%s", "");
}

/* Opened from the tray's network button, which is where somebody actually
 * looks when the Wi-Fi is not working. */
void network_app_open(void) {
    if (!win) return;
    gui_open(win);
    if (wifi_radio_get() && wifi_scan_count() == 0)
        queue(JOB_SCAN, 0, "Scanning...");
}
