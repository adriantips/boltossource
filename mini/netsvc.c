/* ===========================================================================
 *  BoltOS Mini  -  mini/netsvc.c
 *
 *  What runs on the network, so that a Mac at the other end of a USB cable
 *  needs nothing installed to use the Pi:
 *
 *    DHCP server   the Mac's new "BoltOS Mini" network adapter asks for an
 *                  address and gets 10.77.0.2. No router is offered, so the
 *                  Mac keeps using its Wi-Fi for the internet.
 *    mDNS          answers for boltos.local, and advertises the screen as
 *                  _rfb._tcp -- which is what puts "BoltOS Mini" in Finder's
 *                  Network list with a Share Screen button.
 *    VNC (5900)    the screen itself (vnc.c).
 *    HTTP (80)     a page that says how to connect and sets the Pi's clock
 *                  from the Mac's; and the small endpoints pi.py uses:
 *                  /time, /reboot, /log, /screen.bmp.
 *
 *  Under QEMU the same services run on the virtio card, which QEMU's DHCP
 *  makes 10.0.2.15 and its port forwards localhost:5901 and :8080 on the Mac.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kprintf.h"
#include "kheap.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define PI_IP     0x0A4D0001u          /* 10.77.0.1                            */
#define MAC_IP    0x0A4D0002u          /* 10.77.0.2: what the Mac is given     */
#define MASK24    0xFFFFFF00u
#define MDNS_IP   0xE00000FBu          /* 224.0.0.251                          */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }

/* ---- DHCP server --------------------------------------------------------------- */
static int dhcp_leases;

static void dhcp_rx(const udp_meta_t *m, const uint8_t *d, int len) {
    if (len < 240 || d[0] != 1 || d[1] != 1 || d[2] != 6) return;
    if (d[236] != 99 || d[237] != 130 || d[238] != 83 || d[239] != 99) return;
    netif_t *nif = m->nif;
    if (nif->ip != PI_IP) return;              /* only on the USB link         */
    int type = 0;
    for (int o = 240; o + 1 < len && d[o] != 255; ) {
        if (d[o] == 0) { o++; continue; }
        if (d[o] == 53 && d[o + 1] >= 1) type = d[o + 2];
        o += 2 + d[o + 1];
    }
    int reply;
    if (type == 1) reply = 2;                  /* DISCOVER -> OFFER           */
    else if (type == 3) reply = 5;             /* REQUEST  -> ACK             */
    else if (type == 8) reply = 5;             /* INFORM   -> ACK             */
    else return;

    uint8_t r[300];
    memset(r, 0, sizeof r);
    r[0] = 2; r[1] = 1; r[2] = 6;
    memcpy(r + 4, d + 4, 4);                   /* xid                          */
    memcpy(r + 10, d + 10, 2);                 /* flags                        */
    if (type != 8) put32(r + 16, MAC_IP);      /* yiaddr                       */
    else memcpy(r + 12, d + 12, 4);
    put32(r + 20, PI_IP);                      /* siaddr                       */
    memcpy(r + 28, d + 28, 16);                /* chaddr                       */
    r[236] = 99; r[237] = 130; r[238] = 83; r[239] = 99;
    int o = 240;
    r[o++] = 53; r[o++] = 1; r[o++] = (uint8_t)reply;
    r[o++] = 54; r[o++] = 4; put32(r + o, PI_IP); o += 4;
    if (type != 8) { r[o++] = 51; r[o++] = 4; put32(r + o, 86400); o += 4; }
    r[o++] = 1;  r[o++] = 4; put32(r + o, MASK24); o += 4;
    r[o++] = 15; r[o++] = 5; memcpy(r + o, "local", 5); o += 5;
    r[o++] = 255;
    udp_send(nif, PI_IP, 0xFFFFFFFFu, 0, 67, 68, r, o < 300 ? 300 : o);
    if (reply == 5 && type == 3) {
        dhcp_leases++;
        kprintf("[dhcp] the Mac is 10.77.0.2 (lease %d)\n", dhcp_leases);
    }
}

/* ---- mDNS ------------------------------------------------------------------------ */
static const char *host_labels[] = { "boltos", "local", 0 };
static const char *svc_labels[]  = { "_rfb", "_tcp", "local", 0 };
static const char *inst_labels[] = { "BoltOS Mini", "_rfb", "_tcp", "local", 0 };
static const char *http_labels[] = { "_http", "_tcp", "local", 0 };
static const char *web_labels[]  = { "BoltOS Mini", "_http", "_tcp", "local", 0 };

static int put_name(uint8_t *p, const char **labels) {
    int o = 0;
    for (int i = 0; labels[i]; i++) {
        int n = (int)strlen(labels[i]);
        p[o++] = (uint8_t)n; memcpy(p + o, labels[i], (size_t)n); o += n;
    }
    p[o++] = 0;
    return o;
}

/* Compare a (possibly compressed) name in a packet against labels. */
static int name_is(const uint8_t *pkt, int len, int off, const char **labels, int *end) {
    int i = 0, jumped = 0, guard = 0;
    *end = -1;
    while (off < len && guard++ < 32) {
        uint8_t n = pkt[off];
        if ((n & 0xC0) == 0xC0) {
            if (off + 1 >= len) return 0;
            if (!jumped) *end = off + 2;
            off = ((n & 0x3F) << 8) | pkt[off + 1];
            jumped = 1;
            continue;
        }
        if (n == 0) { if (!jumped) *end = off + 1; return labels[i] == 0; }
        if (!labels[i] || off + 1 + n > len || (int)strlen(labels[i]) != n) return 0;
        for (int k = 0; k < n; k++) {
            char a = (char)pkt[off + 1 + k], b = labels[i][k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) return 0;
        }
        i++; off += 1 + n;
    }
    return 0;
}

static int skip_name(const uint8_t *pkt, int len, int off) {
    while (off < len) {
        uint8_t n = pkt[off];
        if ((n & 0xC0) == 0xC0) return off + 2;
        if (n == 0) return off + 1;
        off += 1 + n;
    }
    return len;
}

static int rr_head(uint8_t *p, const char **name, uint16_t type, uint16_t cls, uint32_t ttl) {
    int o = put_name(p, name);
    put16(p + o, type); put16(p + o + 2, cls); put32(p + o + 4, ttl);
    return o + 8;
}

static int rr_a(uint8_t *p, uint32_t ip) {
    int o = rr_head(p, host_labels, 1, 0x8001, 120);
    put16(p + o, 4); put32(p + o + 2, ip);
    return o + 6;
}

static int rr_ptr(uint8_t *p, const char **svc, const char **inst) {
    int o = rr_head(p, svc, 12, 1, 4500);
    int n = put_name(p + o + 2, inst);
    put16(p + o, (uint16_t)n);
    return o + 2 + n;
}

static int rr_srv(uint8_t *p, const char **inst, uint16_t port) {
    int o = rr_head(p, inst, 33, 0x8001, 120);
    uint8_t *r = p + o + 2;
    put16(r, 0); put16(r + 2, 0); put16(r + 4, port);
    int n = put_name(r + 6, host_labels);
    put16(p + o, (uint16_t)(6 + n));
    return o + 2 + 6 + n;
}

static int rr_txt(uint8_t *p, const char **inst) {
    int o = rr_head(p, inst, 16, 0x8001, 4500);
    put16(p + o, 1); p[o + 2] = 0;
    return o + 3;
}

/* Everything we are, in one answer. Small enough that sending all of it
 * whatever was asked is simpler than working out the minimum. */
static int mdns_full(uint8_t *p, uint32_t ip) {
    memset(p, 0, 12);
    put16(p + 2, 0x8400);                      /* response, authoritative      */
    int o = 12, n = 0;
    o += rr_a(p + o, ip); n++;
    o += rr_ptr(p + o, svc_labels, inst_labels); n++;
    o += rr_srv(p + o, inst_labels, 5900); n++;
    o += rr_txt(p + o, inst_labels); n++;
    o += rr_ptr(p + o, http_labels, web_labels); n++;
    o += rr_srv(p + o, web_labels, 80); n++;
    o += rr_txt(p + o, web_labels); n++;
    put16(p + 6, (uint16_t)n);
    return o;
}

static void mdns_rx(const udp_meta_t *m, const uint8_t *d, int len) {
    if (len < 12 || (d[2] & 0x80) || !m->nif->ip) return;     /* queries only */
    int qd = (d[4] << 8) | d[5];
    int off = 12, hit = 0, end;
    for (int q = 0; q < qd && off < len; q++) {
        if (name_is(d, len, off, host_labels, &end) || name_is(d, len, off, svc_labels, &end) ||
            name_is(d, len, off, inst_labels, &end) || name_is(d, len, off, http_labels, &end) ||
            name_is(d, len, off, web_labels, &end)) hit = 1;
        off = skip_name(d, len, off) + 4;
    }
    if (!hit) return;
    uint8_t r[700];
    int n = mdns_full(r, m->nif->ip);
    udp_send(m->nif, m->nif->ip, MDNS_IP, 0, 5353, 5353, r, n);
}

static uint64_t next_announce;
static int announces;
static void mdns_announce(void) {
    for (int i = 0; net_if(i); i++) {
        netif_t *nif = net_if(i);
        if (!nif->ip || !nif->up) continue;
        uint8_t r[700];
        int n = mdns_full(r, nif->ip);
        udp_send(nif, nif->ip, MDNS_IP, 0, 5353, 5353, r, n);
    }
}

/* ---- HTTP -------------------------------------------------------------------------- */
#define HTTP_N 4
static struct { tcb_t *c; char req[2048]; int len; int done; uint64_t t0; } http[HTTP_N];
static uint64_t reboot_at;

static const char page_head[] =
"<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width'>"
"<title>BoltOS Mini</title><style>"
":root{color-scheme:light dark;--bg:#f5f5f7;--fg:#1d1d1f;--mut:#6e6e73;--card:#fff;--acc:#5e5ce6}"
"@media(prefers-color-scheme:dark){:root{--bg:#141418;--fg:#f5f5f7;--mut:#a1a1a6;--card:#1f1f25}}"
"body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 -apple-system,system-ui,sans-serif}"
"main{max-width:640px;margin:0 auto;padding:48px 20px}h1{font-size:32px;margin:0 0 4px;font-weight:650}"
"p{color:var(--mut);margin:0 0 24px}.card{background:var(--card);border-radius:16px;padding:20px 24px;margin:16px 0}"
"a.btn,button{display:inline-block;background:var(--acc);color:#fff;border:0;border-radius:10px;padding:10px 18px;"
"font:inherit;font-weight:600;text-decoration:none;cursor:pointer}code{font:14px ui-monospace,monospace}"
"dl{display:grid;grid-template-columns:max-content 1fr;gap:6px 20px;margin:0}dt{color:var(--mut)}dd{margin:0}"
"</style></head><body><main>";

static void http_page(tcb_t *c) {
    char body[4096];
    char up[32], t[64] = "not set yet";
    mini_uptime_str(up, sizeof up);
    if (clock_known()) {
        int y, mo, d, h, mi, s, wd;
        clock_local(&y, &mo, &d, &h, &mi, &s, &wd);
        snprintf(t, sizeof t, "%04d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
    }
    int n = snprintf(body, sizeof body,
        "%s<h1>BoltOS Mini</h1><p>%s &middot; up %s</p>"
        "<div class=card><b>See the screen</b><p style='margin:8px 0 16px'>Open it in Screen Sharing. "
        "The password is <code>%s</code>.</p><a class=btn href='vnc://%s'>Open the screen</a></div>"
        "<div class=card><b>Clock</b><p id=c style='margin:8px 0 16px'>The Pi's clock: %s</p>"
        "<button onclick=\"fetch('/time?t='+Math.floor(Date.now()/1000)+'&tz='+(-new Date().getTimezoneOffset()),"
        "{method:'POST'}).then(()=>location.reload())\">Set it from this Mac</button></div>"
        "<div class=card><dl><dt>Build</dt><dd>%s</dd><dt>Board</dt><dd>%s</dd>"
        "<dt>Log</dt><dd><a href=/log>/log</a></dd><dt>Screenshot</dt><dd><a href=/screen.bmp>/screen.bmp</a></dd>"
        "</dl></div></main></body></html>",
        page_head, g_board_name, up, g_vnc_password,
        g_plat == PLAT_QEMU ? "localhost:5901" : "10.77.0.1", t, mini_version(), g_board_name);
    char hdr[160];
    int hn = snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                      "Content-Length: %d\r\nConnection: close\r\n\r\n", n);
    tcp_write(c, hdr, hn);
    tcp_write(c, body, n);
}

static void http_text(tcb_t *c, int code, const char *text) {
    char hdr[160];
    int n = (int)strlen(text);
    int hn = snprintf(hdr, sizeof hdr, "HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
                      "Content-Length: %d\r\nConnection: close\r\n\r\n", code, code == 200 ? "OK" : "Not Found", n);
    tcp_write(c, hdr, hn);
    tcp_write(c, text, n);
}

/* /screen.bmp: the front buffer as a 24-bit BMP, bottom-up, streamed out a
 * band at a time as the send ring empties. */
static struct { tcb_t *c; int row; } shot;

static void shot_pump(void) {
    if (!shot.c) return;
    if (!tcp_open(shot.c)) { shot.c = 0; return; }
    int rowbytes = scr_w * 3;
    while (shot.row >= 0 && tcp_space(shot.c) >= rowbytes) {
        uint8_t line[4096 * 3];
        const uint32_t *src = g_front + (size_t)shot.row * scr_w;
        for (int x = 0; x < scr_w; x++) {
            line[3 * x] = (uint8_t)src[x]; line[3 * x + 1] = (uint8_t)(src[x] >> 8);
            line[3 * x + 2] = (uint8_t)(src[x] >> 16);
        }
        tcp_write(shot.c, line, rowbytes);
        shot.row--;
    }
    if (shot.row < 0) { tcp_close(shot.c); shot.c = 0; }
}

static void http_shot(tcb_t *c) {
    if (shot.c) { http_text(c, 503, "busy\n"); return; }
    int img = scr_w * 3 * scr_h;
    char hdr[160];
    int hn = snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\n"
                      "Content-Length: %d\r\nConnection: close\r\n\r\n", 54 + img);
    tcp_write(c, hdr, hn);
    uint8_t b[54] = { 'B', 'M' };
    uint32_t v[] = { (uint32_t)(54 + img), 0, 54, 40, (uint32_t)scr_w, (uint32_t)scr_h };
    for (int i = 0; i < 6; i++) memcpy(b + 2 + 4 * i, &v[i], 4);
    b[26] = 1; b[28] = 24;
    tcp_write(c, b, 54);
    shot.c = c; shot.row = scr_h - 1;
}

static int query_int(const char *q, const char *key, long *out) {
    const char *p = strstr(q, key);
    if (!p) return 0;
    *out = strtol(p + strlen(key), 0, 10);
    return 1;
}

static void http_handle(int i) {
    tcb_t *c = http[i].c;
    char *r = http[i].req;
    char method[8] = "", path[256] = "";
    int k = 0;
    while (r[k] && r[k] != ' ' && k < 7) { method[k] = r[k]; k++; }
    method[k] = 0;
    if (r[k] == ' ') k++;
    int j = 0;
    while (r[k] && r[k] != ' ' && j < 255) path[j++] = r[k++];
    path[j] = 0;

    if (strcmp(path, "/") == 0) http_page(c);
    else if (strncmp(path, "/time", 5) == 0) {
        long t = 0, tz = 0;
        if (query_int(path, "t=", &t) && t > 1600000000L) {
            clock_set_unix((uint64_t)t);
            if (query_int(path, "tz=", &tz) && tz > -900 && tz < 900) clock_set_tz_minutes((int)tz);
            kprintf("[clock] set from the Mac: %ld (UTC%+ld min)\n", t, tz);
            shell_request_frame();
            http_text(c, 200, "ok\n");
        } else http_text(c, 200, "usage: /time?t=<unix seconds>&tz=<minutes east of UTC>\n");
    } else if (strcmp(path, "/reboot") == 0 && strcmp(method, "POST") == 0) {
        http_text(c, 200, "restarting\n");
        reboot_at = now_ms() + 300;
    } else if (strcmp(path, "/log") == 0) {
        char *buf = kmalloc(256 * 1024);
        if (buf) { klog_snapshot(buf, 256 * 1024); http_text(c, 200, buf); kfree(buf); }
    } else if (strcmp(path, "/debug") == 0) {
        int usbh_debug(char *out, int cap);
        char *buf = kmalloc(8192);
        if (buf) { int n = eth_debug(buf, 2048); usbh_debug(buf + n, 8192 - n); http_text(c, 200, buf); kfree(buf); }
    } else if (strcmp(path, "/screen.bmp") == 0) {
        http_shot(c);
        http[i].c = 0;                         /* the pump owns it now         */
        return;
    } else http_text(c, 404, "not here\n");
    tcp_close(c);
    http[i].c = 0;
}

static void http_poll(void) {
    tcb_t *nc;
    while ((nc = tcp_accept(80))) {
        int slot = -1;
        for (int i = 0; i < HTTP_N; i++) if (!http[i].c) { slot = i; break; }
        if (slot < 0) { tcp_abort(nc); continue; }
        http[slot].c = nc; http[slot].len = 0; http[slot].t0 = now_ms();
    }
    for (int i = 0; i < HTTP_N; i++) {
        if (!http[i].c) continue;
        int n = tcp_read(http[i].c, http[i].req + http[i].len, (int)sizeof http[i].req - 1 - http[i].len);
        if (n > 0) http[i].len += n;
        http[i].req[http[i].len] = 0;
        if (strstr(http[i].req, "\r\n\r\n") || http[i].len >= (int)sizeof http[i].req - 1) http_handle(i);
        else if (!tcp_open(http[i].c) || now_ms() - http[i].t0 > 5000) { tcp_abort(http[i].c); http[i].c = 0; }
    }
    shot_pump();
}

/* ---- the services, together --------------------------------------------------------- */
void netsvc_init(void) {
    for (int i = 0; net_if(i); i++) {
        netif_t *nif = net_if(i);
        if (strcmp(nif->name, "usb0") != 0) continue;            /* the others ask DHCP (inet.c) */
        nif->ip = PI_IP; nif->mask = MASK24;
        char ip[16]; net_fmt_ip(nif->ip, ip);
        kprintf("[net] %s is %s\n", nif->name, ip);
    }
    udp_bind(67, dhcp_rx);
    udp_bind(5353, mdns_rx);
    tcp_listen(80);
    vnc_init(5900);
    next_announce = now_ms() + 1000;
}

void netsvc_poll(void) {
    http_poll();
    vnc_poll();
    /* Announce a few times after the link comes up, then only answer. */
    if (announces < 4 && now_ms() >= next_announce) {
        mdns_announce();
        announces++;
        next_announce = now_ms() + 1000u * (1u << announces);
    }
    if (reboot_at && now_ms() >= reboot_at) board_reboot();
}

/* The link went down and up again (the Mac re-enumerated us): start the
 * announcements over, the Mac has forgotten us. */
void netsvc_link_up(void) { announces = 0; next_announce = now_ms() + 500; }
