/* ===========================================================================
 *  BoltOS Mini  -  mini/app_terminal.c
 *
 *  A terminal with a small shell in it: the commands a little board needs
 *  -- what it is, what it is doing, what is plugged in, its log, its LEDs,
 *  its clock -- and the files on the SD card (ls, cd, cat, mkdir, rm, mv,
 *  cp, df), with `edit` and `run` handing a file to the Code app. `help`
 *  lists them; Tab completes a file's name.
 * ===========================================================================*/
#include <stdint.h>
#include "ui.h"
#include "fs.h"
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
static char cwd[FS_PATH] = "/";

static int lower_eq(char a, char b) { return (a | 32) == (b | 32) && ((a >= 'A' && a <= 'Z') || (a >= 'a' && a <= 'z')) ? 1 : a == b; }
static void complete(void);
static void prompt(char *out, int cap) { snprintf(out, (size_t)cap, "mini:%s$ ", cwd); }
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
        { "theme dark|light|black", "the colours" },
        { "ls [folder]", "what's in a folder on the SD card" }, { "cd folder", "go into a folder" },
        { "cat file", "show a file" }, { "mkdir folder", "make a folder" },
        { "rm file|folder", "remove one (a folder with what's in it)" },
        { "mv from to", "rename or move" }, { "cp from to", "copy a file" },
        { "touch file", "make an empty file" }, { "df", "room on the card" },
        { "edit file", "open it in Code" }, { "run file.py|.c|.cpp", "run it in Code" },
        { "echo TEXT", "say something" }, { "clear", "clear the screen" },
        { "reboot", "restart the Pi" }, { "poweroff", "stop the Pi" },
    };
    out(C_ACC, "Commands:");
    for (unsigned i = 0; i < sizeof h / sizeof h[0]; i++) out(C_TEXT, "  %-26s %s", h[i][0], h[i][1]);
    out(C_DIM, "Up and down recall earlier commands. Esc on an empty line goes home.");
}

/* ---- the SD card's files -------------------------------------------------------------------- */
static int path_of(const char *arg, char *dst) {
    if (!*arg) { out(C_WARN, "Which one?"); return -1; }
    if (fs_join(cwd, arg, dst, FS_PATH)) { out(C_WARN, "That path is too long."); return -1; }
    return 0;
}

static void cmd_ls(const char *arg) {
    char p[FS_PATH];
    if (fs_join(cwd, *arg ? arg : ".", p, sizeof p)) return;
    fs_ent_t st;
    if (fs_stat(p, &st)) { out(C_WARN, "ls: %s", fs_why()); return; }
    if (!st.dir) { out(C_TEXT, "%s  %u bytes", st.name, st.size); return; }
    fs_ent_t *e = kmalloc(sizeof(fs_ent_t) * 256);
    if (!e) return;
    int n = fs_list(p, e, 256);
    if (n < 0) out(C_WARN, "ls: %s", fs_why());
    else if (!n) out(C_DIM, "(empty)");
    for (int i = 0; i < n; i++) {
        char d[24]; fs_date_str(e[i].date, e[i].time, d, sizeof d);
        if (e[i].dir) out(C_ACC, "  %s  %8s  %s/", d, "", e[i].name);
        else out(C_TEXT, "  %s  %8u  %s", d, e[i].size, e[i].name);
    }
    kfree(e);
}

static void cmd_cat(const char *arg) {
    char p[FS_PATH], *b; uint32_t n;
    if (path_of(arg, p)) return;
    if (fs_read(p, &b, &n)) { out(C_WARN, "cat: %s", fs_why()); return; }
    int bin = 0;
    for (uint32_t i = 0; i < n && i < 4096; i++) if (!b[i]) bin = 1;
    if (bin) out(C_WARN, "%s isn't text (%u bytes).", fs_basename(p), n);
    else if (n > 64 * 1024) { b[64 * 1024] = 0; out(C_TEXT, "%s", b); out(C_DIM, "... the first 64 KiB of %u bytes", n); }
    else out(C_TEXT, "%s", b);
    kfree(b);
}

static int two_paths(const char *arg, char *a, char *b) {
    char x[FS_PATH], y[FS_PATH];
    if (sscanf(arg, "%255s %255s", x, y) != 2) { out(C_WARN, "Two names, please: the one there and the new one."); return -1; }
    if (fs_join(cwd, x, a, FS_PATH) || fs_join(cwd, y, b, FS_PATH)) return -1;
    /* Into a folder that's there: keep the name. */
    fs_ent_t st;
    if (fs_stat(b, &st) == 0 && st.dir) {
        int n = (int)strlen(b);
        snprintf(b + n, (size_t)(FS_PATH - n), "%s%s", n > 1 ? "/" : "", fs_basename(a));
    }
    return 0;
}

static int files_cmd(const char *line, const char *arg) {
    char p[FS_PATH], q[FS_PATH];
    if (!strcmp(line, "pwd")) out(C_TEXT, "%s", cwd);
    else if (!strcmp(line, "ls") || !strcmp(line, "dir")) cmd_ls(arg);
    else if (!strcmp(line, "cd")) {
        if (fs_join(cwd, *arg ? arg : "/", p, sizeof p)) return 1;
        fs_ent_t st;
        if (fs_stat(p, &st)) out(C_WARN, "cd: %s", fs_why());
        else if (!st.dir) out(C_WARN, "cd: %s is a file", st.name);
        else snprintf(cwd, sizeof cwd, "%s", p);
    }
    else if (!strcmp(line, "cat") || !strcmp(line, "type")) cmd_cat(arg);
    else if (!strcmp(line, "mkdir")) { if (!path_of(arg, p) && fs_mkdir(p)) out(C_WARN, "mkdir: %s", fs_why()); }
    else if (!strcmp(line, "rm") || !strcmp(line, "rmdir") || !strcmp(line, "del")) {
        if (!path_of(arg, p)) {
            if (!strncasecmp(cwd, p, strlen(p)) && (cwd[strlen(p)] == '/' || !cwd[strlen(p)])) out(C_WARN, "rm: you're in there; cd out first");
            else if (fs_remove(p)) out(C_WARN, "rm: %s", fs_why());
        }
    }
    else if (!strcmp(line, "mv") || !strcmp(line, "ren")) { if (!two_paths(arg, p, q) && fs_rename(p, q)) out(C_WARN, "mv: %s", fs_why()); }
    else if (!strcmp(line, "cp") || !strcmp(line, "copy")) {
        char *b; uint32_t n;
        if (two_paths(arg, p, q)) return 1;
        if (fs_read(p, &b, &n)) { out(C_WARN, "cp: %s", fs_why()); return 1; }
        if (fs_write(q, b, n)) out(C_WARN, "cp: %s", fs_why());
        kfree(b);
    }
    else if (!strcmp(line, "touch")) {
        fs_ent_t st;
        if (!path_of(arg, p) && fs_stat(p, &st) != 0 && fs_write(p, "", 0)) out(C_WARN, "touch: %s", fs_why());
    }
    else if (!strcmp(line, "df")) {
        uint64_t fr, tot;
        if (fs_space(&fr, &tot)) out(C_WARN, "df: %s", fs_why());
        else out(C_TEXT, "SD card, BOLTOS partition: %lu MiB free of %lu MiB", (unsigned long)(fr >> 20), (unsigned long)(tot >> 20));
    }
    else if (!strcmp(line, "edit") || !strcmp(line, "code") || !strcmp(line, "run")) {
        if (!*arg) { shell_open(&app_code); return 1; }
        if (path_of(arg, p)) return 1;
        int code_open_path(const char *path, int run);
        if (code_open_path(p, !strcmp(line, "run"))) out(C_WARN, "%s: %s", line, fs_why());
    }
    else return 0;
    return 1;
}

/* Tab: the word under the cursor, completed from the names in its folder. */
static void complete(void) {
    int s = in_cur;
    while (s > 0 && in[s - 1] != ' ') s--;
    char word[FS_PATH]; int wl = in_cur - s;
    if (wl >= (int)sizeof word) return;
    memcpy(word, in + s, (size_t)wl); word[wl] = 0;
    char *slash = strrchr(word, '/');
    char dir[FS_PATH], stem[FS_NAME];
    if (slash) { *slash = 0; if (fs_join(cwd, word[0] ? word : "/", dir, sizeof dir)) return; snprintf(stem, sizeof stem, "%s", slash + 1); *slash = '/'; }
    else { snprintf(dir, sizeof dir, "%s", cwd); snprintf(stem, sizeof stem, "%s", word); }
    fs_ent_t *e = kmalloc(sizeof(fs_ent_t) * 256);
    if (!e) return;
    int n = fs_list(dir, e, 256), hit = -1, hits = 0, sl = (int)strlen(stem), common = 0;
    for (int i = 0; i < n; i++) {
        if (strncasecmp(e[i].name, stem, (size_t)sl)) continue;
        if (hit < 0) { hit = i; common = (int)strlen(e[i].name); }
        else { int k = 0; while (k < common && lower_eq(e[hit].name[k], e[i].name[k])) k++; common = k; }
        hits++;
    }
    if (hit >= 0 && common > sl) {
        char add[FS_NAME + 2]; int al = 0;
        for (int k = sl; k < common && al < FS_NAME; k++) add[al++] = e[hit].name[k];
        if (hits == 1 && e[hit].dir) add[al++] = '/';
        else if (hits == 1) add[al++] = ' ';
        if (in_len + al < (int)sizeof in - 1) {
            memmove(in + in_cur + al, in + in_cur, (size_t)(in_len - in_cur + 1));
            memcpy(in + in_cur, add, (size_t)al);
            in_cur += al; in_len += al;
        }
    } else if (hits > 1) {
        char list[1024]; int ln = 0;
        for (int i = 0; i < n && ln < 900; i++)
            if (!strncasecmp(e[i].name, stem, (size_t)sl)) ln += snprintf(list + ln, sizeof list - (size_t)ln, "%s%s  ", e[i].name, e[i].dir ? "/" : "");
        out(C_DIM, "%s", list);
    }
    kfree(e);
}

static void run(char *line) {
    while (*line == ' ') line++;
    char *arg = strchr(line, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; } else arg = line + strlen(line);
    if (!*line) return;
    if (!strcmp(line, "help") || !strcmp(line, "?")) cmd_help();
    else if (files_cmd(line, arg)) { }
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
        int w = !strcmp(arg, "dark") || !strcmp(arg, "dusk") ? 0 : !strcmp(arg, "light") || !strcmp(arg, "dawn") ? 1 :
                !strcmp(arg, "black") || !strcmp(arg, "midnight") ? 2 : -1;
        if (w < 0) out(C_WARN, "usage: theme dark|light|black");
        else { theme_set(w, th.accent_i); shell_settings_save(); }
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
        if ((e->mods & MOD_CTRL) && (e->ch == 'c' || e->ch == 'C')) { char p[FS_PATH + 8]; prompt(p, sizeof p); out(C_DIM, "%s%s^C", p, in); in_len = in_cur = 0; in[0] = 0; return; }
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
    case KEY_TAB: complete(); break;
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
        { char p[FS_PATH + 8]; prompt(p, sizeof p); out(C_DIM, "%s%s", p, in); }
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
    char pr[FS_PATH + 8]; prompt(pr, sizeof pr);
    int pw = font_draw(FONT_MONO_BOLD, px, x + pad, base, pr, -1, th.accent, 255);
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

const app_t app_terminal = { "Terminal", 0x3A3F55, G_TERMINAL, t_open, t_key, 0, t_draw, t_tick, 0 };
