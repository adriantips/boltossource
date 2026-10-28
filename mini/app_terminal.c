/* ===========================================================================
 *  BoltOS Mini  -  mini/app_terminal.c
 *
 *  A terminal with a small shell in it: the commands a little board needs
 *  -- what it is, what it is doing, what is plugged in, its log, its LEDs,
 *  its clock -- and nothing that needs a filesystem. `help` lists them.
 * ===========================================================================*/
#include <stdint.h>
#include "ui.h"
#include "kheap.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include <stdarg.h>

#define NL    400
#define NC    160
enum { C_TEXT, C_DIM, C_ACC, C_OK, C_WARN };

static char lines[NL][NC];
static uint8_t colour[NL];
static int head, count;                  /* ring of lines                         */
static int scroll;                       /* lines scrolled back                   */
static char in[128]; static int in_len, in_cur;
static char hist[16][128]; static int hist_n, hist_i;
static int started;
static int view_cols = 80;

static void put_line(const char *s, int c) {
    int i = (head + count) % NL;
    if (count == NL) head = (head + 1) % NL; else count++;
    snprintf(lines[i], NC, "%s", s);
    colour[i] = (uint8_t)c;
}

/* Print with \n splitting and wrapping at the visible width. */
static void out(int c, const char *fmt, ...) {
    char buf[4096];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    char *p = buf;
    for (;;) {
        char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        while (n > view_cols) {
            char seg[NC]; int k = view_cols < NC - 1 ? view_cols : NC - 1;
            memcpy(seg, p, (size_t)k); seg[k] = 0;
            put_line(seg, c);
            p += k; n -= k;
        }
        char seg[NC]; int k = n < NC - 1 ? n : NC - 1;
        memcpy(seg, p, (size_t)k); seg[k] = 0;
        if (nl || n) put_line(seg, c);
        if (!nl) break;
        p = nl + 1;
        if (!*p) break;
    }
}

/* ---- commands ------------------------------------------------------------------------- */
static void cmd_about(void) {
    char up[32]; mini_uptime_str(up, sizeof up);
    uint64_t used, total; kheap_usage(&used, &total);
    out(C_ACC, "        __        BoltOS Mini %s", mini_version());
    out(C_ACC, "     __/ /        %s", g_board_name);
    out(C_ACC, "    /_  __/       CPU    4x Cortex-A53%s", board_cpu_mhz() ? "" : " (emulated)");
    if (board_cpu_mhz()) out(C_ACC, "     / /_         clock  %u MHz", board_cpu_mhz());
    out(C_ACC, "    /__/          RAM    %lu MiB, heap %lu of %lu KiB in use", (unsigned long)(g_ram_size >> 20),
        (unsigned long)(used >> 10), (unsigned long)(total >> 10));
    out(C_TEXT, "                  up     %s, boot #%u", up, klog_boot_count());
    out(C_TEXT, "                  screen %dx%d, %d viewer%s", scr_w, scr_h, vnc_clients(), vnc_clients() == 1 ? "" : "s");
}

static void cmd_net(void) {
    for (int i = 0; net_if(i); i++) {
        netif_t *n = net_if(i);
        char ip[16]; net_fmt_ip(n->ip, ip);
        out(C_ACC, "%s  %s  %s", n->name, ip, n->up ? "up" : "down");
        out(C_TEXT, "      mac %02x:%02x:%02x:%02x:%02x:%02x", n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
        out(C_TEXT, "      rx %lu packets, %lu KiB   tx %lu packets, %lu KiB, %lu dropped",
            (unsigned long)n->rx_pkts, (unsigned long)(n->rx_bytes >> 10), (unsigned long)n->tx_pkts,
            (unsigned long)(n->tx_bytes >> 10), (unsigned long)n->tx_drops);
    }
    out(C_TEXT, "tcp connections: %d   vnc viewers: %d", tcp_conn_count(), vnc_clients());
    if (g_plat == PLAT_H618) out(C_DIM, "From the Mac: ping 10.77.0.1, or open vnc://10.77.0.1");
}

static void cmd_help(void) {
    static const char *h[][2] = {
        { "about", "what this machine is" }, { "uptime", "how long it has been up" },
        { "date", "the date and time" }, { "time HH:MM", "set the clock by hand" },
        { "mem", "memory in use" }, { "cpu", "processor load" },
        { "net", "network interfaces" }, { "usb", "USB devices" },
        { "log [n]", "the last n lines of the boot log" },
        { "led green|red on|off", "the board's LEDs" },
        { "passwd WORD", "the screen-sharing password" },
        { "theme dusk|dawn|midnight", "the wallpaper" },
        { "echo TEXT", "say something" }, { "clear", "clear the screen" },
        { "reboot", "restart the Pi" }, { "poweroff", "stop the Pi" },
    };
    out(C_ACC, "Commands:");
    for (unsigned i = 0; i < sizeof h / sizeof h[0]; i++) out(C_TEXT, "  %-26s %s", h[i][0], h[i][1]);
    out(C_DIM, "Up and down recall earlier commands. Esc on an empty line goes home.");
}

static void run(char *line) {
    while (*line == ' ') line++;
    char *arg = strchr(line, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; } else arg = line + strlen(line);
    if (!*line) return;
    if (!strcmp(line, "help") || !strcmp(line, "?")) cmd_help();
    else if (!strcmp(line, "clear")) { count = 0; scroll = 0; }
    else if (!strcmp(line, "about") || !strcmp(line, "neofetch") || !strcmp(line, "uname")) cmd_about();
    else if (!strcmp(line, "uptime")) { char up[32]; mini_uptime_str(up, sizeof up); out(C_TEXT, "up %s", up); }
    else if (!strcmp(line, "date")) {
        char hm[16], d[96]; clock_text(hm, sizeof hm, d, sizeof d);
        if (clock_known()) out(C_TEXT, "%s, %s (UTC%+d:%02d)", d, hm, clock_tz_minutes() / 60, abs(clock_tz_minutes()) % 60);
        else out(C_WARN, "The clock has not been set. Open http://10.77.0.1 on the Mac, or: time HH:MM");
    }
    else if (!strcmp(line, "time")) {
        int h, m;
        if (sscanf(arg, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
            uint64_t t = clock_unix() + (uint64_t)((int64_t)clock_tz_minutes() * 60);
            uint64_t day = t / 86400 * 86400;
            if (!clock_known()) day = 1798761600ull;       /* 2027-01-01: some day, at least */
            clock_set_unix(day + (uint64_t)(h * 3600 + m * 60) - (uint64_t)((int64_t)clock_tz_minutes() * 60));
            out(C_OK, "Clock set to %d:%02d.", h, m);
        } else out(C_WARN, "usage: time HH:MM");
    }
    else if (!strcmp(line, "mem")) {
        uint64_t used, total; kheap_usage(&used, &total);
        out(C_TEXT, "RAM   %lu MiB", (unsigned long)(g_ram_size >> 20));
        out(C_TEXT, "heap  %lu KiB used of %lu KiB (%lu%%)", (unsigned long)(used >> 10), (unsigned long)(total >> 10),
            (unsigned long)(total ? used * 100 / total : 0));
    }
    else if (!strcmp(line, "cpu")) out(C_TEXT, "load %d%% over the last second%s", shell_cpu_load(),
                                       board_cpu_mhz() ? "" : " (in QEMU)");
    else if (!strcmp(line, "net") || !strcmp(line, "ifconfig") || !strcmp(line, "ip")) cmd_net();
    else if (!strcmp(line, "usb") || !strcmp(line, "lsusb")) {
        char *b = kmalloc(4096);
        if (b) { usbh_device_list(b, 4096); out(C_TEXT, "%s", b[0] ? b : "No USB devices."); kfree(b); }
    }
    else if (!strcmp(line, "log") || !strcmp(line, "dmesg")) {
        int n = atoi(arg); if (n <= 0) n = 20;
        char *b = kmalloc(64 * 1024);
        if (!b) return;
        klog_snapshot(b, 64 * 1024);
        char *p = b + strlen(b);
        int k = 0;
        while (p > b && k <= n) { p--; if (*p == '\n') k++; }
        if (*p == '\n') p++;
        out(C_DIM, "%s", p);
        kfree(b);
    }
    else if (!strcmp(line, "led")) {
        char which[8] = "", st[8] = "";
        sscanf(arg, "%7s %7s", which, st);
        int green = !strcmp(which, "green"), on = !strcmp(st, "on");
        if ((!green && strcmp(which, "red")) || (!on && strcmp(st, "off"))) out(C_WARN, "usage: led green|red on|off");
        else if (g_plat != PLAT_H618) out(C_WARN, "There are no LEDs on a QEMU machine.");
        else { board_led(green, on); out(C_OK, "%s LED %s.", green ? "Green" : "Red", on ? "on" : "off"); }
    }
    else if (!strcmp(line, "passwd")) {
        if (strlen(arg) < 4 || strlen(arg) > 8) out(C_WARN, "The password is 4 to 8 characters (VNC uses 8 at most).");
        else { snprintf(g_vnc_password, sizeof g_vnc_password, "%s", arg); out(C_OK, "Screen-sharing password changed. It lasts until a restart."); }
    }
    else if (!strcmp(line, "theme")) {
        int w = !strcmp(arg, "dusk") ? 0 : !strcmp(arg, "dawn") ? 1 : !strcmp(arg, "midnight") ? 2 : -1;
        if (w < 0) out(C_WARN, "usage: theme dusk|dawn|midnight");
        else theme_set(w, th.accent_i);
    }
    else if (!strcmp(line, "echo")) out(C_TEXT, "%s", arg);
    else if (!strcmp(line, "reboot") || !strcmp(line, "restart")) { out(C_TEXT, "Restarting..."); board_reboot(); }
    else if (!strcmp(line, "poweroff") || !strcmp(line, "shutdown")) board_poweroff();
    else out(C_WARN, "%s: not a command here. Try help.", line);
}

/* The shell asks before Esc takes us home: a line being typed is cleared
 * instead, and only an empty prompt lets Esc through. */
int terminal_esc(void) {
    if (!in_len) return 0;
    in[0] = 0; in_len = in_cur = 0;
    return 1;
}

/* ---- the app --------------------------------------------------------------------------------- */
static void t_open(void) {
    if (started) return;
    started = 1;
    out(C_ACC, "BoltOS Mini %s", mini_version());
    out(C_DIM, "Type help for the commands.");
}

static void t_key(const input_ev_t *e) {
    switch (e->key) {
    case KEY_CHAR:
        if ((e->mods & MOD_CTRL) && (e->ch == 'l' || e->ch == 'L')) { count = 0; return; }
        if ((e->mods & MOD_CTRL) && (e->ch == 'c' || e->ch == 'C')) { out(C_DIM, "mini$ %s^C", in); in_len = in_cur = 0; in[0] = 0; return; }
        if (in_len < (int)sizeof in - 1 && e->ch >= 32) {
            memmove(in + in_cur + 1, in + in_cur, (size_t)(in_len - in_cur + 1));
            in[in_cur++] = (char)e->ch; in_len++;
        }
        break;
    case KEY_BACKSPACE:
        if (in_cur > 0) { memmove(in + in_cur - 1, in + in_cur, (size_t)(in_len - in_cur + 1)); in_cur--; in_len--; }
        break;
    case KEY_DELETE:
        if (in_cur < in_len) { memmove(in + in_cur, in + in_cur + 1, (size_t)(in_len - in_cur)); in_len--; }
        break;
    case KEY_LEFT:  if (in_cur > 0) in_cur--; break;
    case KEY_RIGHT: if (in_cur < in_len) in_cur++; break;
    case KEY_HOME:  in_cur = 0; break;
    case KEY_END:   in_cur = in_len; break;
    case KEY_PGUP:  scroll += 10; if (scroll > count) scroll = count; break;
    case KEY_PGDN:  scroll -= 10; if (scroll < 0) scroll = 0; break;
    case KEY_UP:
        if (hist_n && hist_i > 0) { hist_i--; snprintf(in, sizeof in, "%s", hist[hist_i % 16]); in_len = in_cur = (int)strlen(in); }
        break;
    case KEY_DOWN:
        if (hist_i < hist_n - 1) { hist_i++; snprintf(in, sizeof in, "%s", hist[hist_i % 16]); }
        else { hist_i = hist_n; in[0] = 0; }
        in_len = in_cur = (int)strlen(in);
        break;
    case KEY_ENTER: {
        out(C_DIM, "mini$ %s", in);
        if (in_len) { snprintf(hist[hist_n % 16], 128, "%s", in); hist_n++; }
        hist_i = hist_n;
        char cmd[128]; snprintf(cmd, sizeof cmd, "%s", in);
        in[0] = 0; in_len = in_cur = 0; scroll = 0;
        run(cmd);
        break;
    }
    }
}

static void t_draw(int x, int y, int w, int h) {
    int px = S(14), lh = S(19), pad = S(20);
    int cw = font_width(FONT_MONO, px, "M", 1);
    if (cw < 1) cw = 7;
    view_cols = (w - 2 * pad) / cw;
    if (view_cols > NC - 1) view_cols = NC - 1;
    int rows = (h - 2 * pad) / lh - 1;
    uint32_t cols[5] = { th.text, th.faint, th.accent, 0x4CC27A, 0xF2B24A };
    if (!th.dark) cols[2] = gfx_shade(th.accent, 80);
    int first = count - rows - scroll; if (first < 0) first = 0;
    int last = count - scroll;
    int yy = y + pad;
    for (int i = first; i < last; i++) {
        int k = (head + i) % NL;
        font_draw(FONT_MONO, px, x + pad, yy + font_ascent(FONT_MONO, px), lines[k], -1, cols[colour[k]], 255);
        yy += lh;
    }
    /* the prompt */
    int base = yy + font_ascent(FONT_MONO, px);
    int pw = font_draw(FONT_MONO_BOLD, px, x + pad, base, "mini$ ", -1, th.accent, 255);
    font_draw(FONT_MONO, px, x + pad + pw, base, in, in_len, th.text, 255);
    int cx = x + pad + pw + font_width(FONT_MONO, px, in, in_cur);
    if ((now_ms() / 530) % 2 == 0) gfx_fill(cx, yy + S(2), S(2), lh - S(4), th.accent);
    if (scroll) text_r(x + w - pad, y + pad / 2 + S(4), "scrolled back " TXT_BULLET " PgDn", FONT_MEDIUM, S(11), th.faint);
}

static int t_tick(void) {
    static uint64_t phase;
    uint64_t p = now_ms() / 530;
    if (p != phase) { phase = p; return 1; }
    return 0;
}

const app_t app_terminal = { "Terminal", 0x3A3F55, G_TERMINAL, t_open, t_key, 0, t_draw, t_tick };
