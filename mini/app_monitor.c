/* ===========================================================================
 *  BoltOS Mini  -  mini/app_monitor.c
 *
 *  What the Pi is doing, on one screen: processor load over the last two
 *  minutes, memory, the link to the Mac, and what is on the USB ports.
 * ===========================================================================*/
#include <stdint.h>
#include "ui.h"
#include "kheap.h"
#include "string.h"
#include "stdio.h"

static uint64_t last_rx, last_tx, last_t;
static uint32_t rx_rate, tx_rate;             /* bytes per second              */

static void rates(void) {
    uint64_t t = now_ms();
    if (t - last_t < 1000) return;
    uint64_t rx = 0, tx = 0;
    for (int i = 0; net_if(i); i++) { rx += net_if(i)->rx_bytes; tx += net_if(i)->tx_bytes; }
    if (last_t) {
        rx_rate = (uint32_t)((rx - last_rx) * 1000 / (t - last_t));
        tx_rate = (uint32_t)((tx - last_tx) * 1000 / (t - last_t));
    }
    last_rx = rx; last_tx = tx; last_t = t;
}

static void fmt_rate(char *o, int cap, uint32_t b) {
    if (b >= 1048576) snprintf(o, (size_t)cap, "%u.%u MB/s", b / 1048576, (b % 1048576) * 10 / 1048576);
    else if (b >= 1024) snprintf(o, (size_t)cap, "%u KB/s", b / 1024);
    else snprintf(o, (size_t)cap, "%u B/s", b);
}

static void block(int x, int y, int w, int h, const char *title) {
    ui_card(0, x, y, w, h, S(6));
    text_l(x + S(16), y + S(20), title, FONT_SEMIBOLD, S(12), th.faint);
}

static void m_draw(int x, int y, int w, int h) {
    rates();
    int pad = S(18), gap = S(14);
    int cw = (w - 2 * pad - gap) / 2, ch = (h - 2 * pad - gap) / 2;
    int x0 = x + pad, x1 = x0 + cw + gap, y0 = y + pad, y1 = y0 + ch + gap;
    char b[96];

    /* processor */
    block(x0, y0, cw, ch, "Processor");
    snprintf(b, sizeof b, "%d%%", shell_cpu_load());
    font_draw(FONT_DISPLAY, S(44), x0 + S(16), y0 + S(40) + font_ascent(FONT_DISPLAY, S(44)), b, -1, th.text, 255);
    if (board_cpu_mhz()) snprintf(b, sizeof b, "4 cores, %u MHz", board_cpu_mhz());
    else snprintf(b, sizeof b, "emulated by QEMU");
    text_r(x0 + cw - S(16), y0 + S(20), b, FONT_REGULAR, S(12), th.faint);
    /* The CPU's temperature: Linux starts slowing an H618 down at 60-70 C and 110 C is its limit. */
    int tmc = board_temp_mc();
    if (tmc > -1000000) {
        int t = (tmc + 500) / 1000;
        snprintf(b, sizeof b, "%d\xB0" "C", t);
        text_r(x0 + cw - S(16), y0 + S(58), b, FONT_SEMIBOLD, S(26), t >= 85 ? 0xF0616Du : t >= 70 ? 0xF5A524u : th.text);
        text_r(x0 + cw - S(16), y0 + S(78), t >= 85 ? "Hot: give it air" : t >= 70 ? "Warm" : "CPU temperature",
               FONT_REGULAR, S(12), th.faint);
    }
    uint8_t hist[120];
    int n = shell_cpu_history(hist, 120);
    int gx = x0 + S(16), gw = cw - S(32), gy = y0 + ch - S(16), gh = ch - S(104);
    if (gh > S(20)) {
        gfx_fill(gx, gy, gw, 1, th.line);
        for (int i = 1; i < n; i++) {
            float ax = (float)gx + (float)gw * (float)(120 - n + i - 1) / 119.0f;
            float bx = (float)gx + (float)gw * (float)(120 - n + i) / 119.0f;
            gfx_line(ax, (float)gy - (float)gh * hist[120 - n + i - 1] / 100.0f,
                     bx, (float)gy - (float)gh * hist[120 - n + i] / 100.0f, (float)S(2), th.accent, 255);
        }
    }

    /* memory */
    block(x1, y0, cw, ch, "Memory");
    uint64_t used, total; kheap_usage(&used, &total);
    snprintf(b, sizeof b, "%lu MiB", (unsigned long)(g_ram_size >> 20));
    font_draw(FONT_DISPLAY, S(44), x1 + S(16), y0 + S(40) + font_ascent(FONT_DISPLAY, S(44)), b, -1, th.text, 255);
    int bw = cw - S(32), by = y0 + S(110);
    gfx_fill(x1 + S(16), by, bw, S(6), th.line);
    int fill = total ? (int)((uint64_t)bw * used / total) : 0;
    if (fill < S(8)) fill = S(8);
    gfx_fill(x1 + S(16), by, fill, S(6), th.accent);
    snprintf(b, sizeof b, "Kernel heap: %lu of %lu MiB in use", (unsigned long)(used >> 20), (unsigned long)(total >> 20));
    text_l(x1 + S(16), by + S(26), b, FONT_REGULAR, S(13), th.sub);
    char up[32]; mini_uptime_str(up, sizeof up);
    snprintf(b, sizeof b, "Up %s " TXT_BULLET " boot #%u", up, klog_boot_count());
    text_l(x1 + S(16), by + S(48), b, FONT_REGULAR, S(13), th.sub);

    /* the link */
    block(x0, y1, cw, ch, "Connection");
    netif_t *ni = net_if(0);
    int yy = y1 + S(52);
    if (ni) {
        char ip[16]; net_fmt_ip(ni->ip, ip);
        snprintf(b, sizeof b, "%s " TXT_ENDASH " %s", ni->up ? "Connected" : "Waiting", ip);
        text_l(x0 + S(16), yy, b, FONT_MEDIUM, S(16), th.text); yy += S(30);
        char r1[24], r2[24]; fmt_rate(r1, sizeof r1, tx_rate); fmt_rate(r2, sizeof r2, rx_rate);
        snprintf(b, sizeof b, "To the Mac %s " TXT_BULLET " from it %s", r1, r2);
        text_l(x0 + S(16), yy, b, FONT_REGULAR, S(13), th.sub); yy += S(22);
        snprintf(b, sizeof b, "%d screen viewer%s " TXT_BULLET " %d connection%s", vnc_clients(), vnc_clients() == 1 ? "" : "s",
                 tcp_conn_count(), tcp_conn_count() == 1 ? "" : "s");
        text_l(x0 + S(16), yy, b, FONT_REGULAR, S(13), th.sub);
    } else text_l(x0 + S(16), yy, "No network interface", FONT_MEDIUM, S(15), th.sub);

    /* USB */
    block(x1, y1, cw, ch, "USB");
    char *list = kmalloc(2048);
    if (list) {
        usbh_device_list(list, 2048);
        if (!list[0]) text_l(x1 + S(16), y1 + S(52), "Nothing plugged in", FONT_MEDIUM, S(15), th.sub);
        int ly = y1 + S(48);
        for (char *p = list; *p && ly < y1 + ch - S(10); ) {
            char *e = strchr(p, '\n'); if (e) *e = 0;
            char name[40]; int k = 0;
            while (p[k] && k < 26) { name[k] = p[k]; k++; }
            while (k > 0 && name[k - 1] == ' ') k--;
            name[k] = 0;
            const char *kind = strstr(p, "keyboard") ? "Keyboard" : strstr(p, "mouse") ? "Mouse" :
                               strstr(p, "hub") ? "Hub" : "Device";
            text_fit_l(x1 + S(16), ly, name[0] == ' ' ? name + 2 : name, cw - S(120), FONT_MEDIUM, S(13), th.text);
            text_r(x1 + cw - S(16), ly, kind, FONT_REGULAR, S(13), th.sub);
            ly += S(24);
            if (!e) break;
            p = e + 1;
        }
        kfree(list);
    }
}

static void m_key(const input_ev_t *e) { (void)e; }
static int m_tick(void) {
    static uint64_t t;
    if (now_ms() - t >= 1000) { t = now_ms(); return 1; }
    return 0;
}

const app_t app_monitor = { "Monitor", 0x2E9C7E, G_MONITOR, 0, m_key, 0, m_draw, m_tick, 0 };
