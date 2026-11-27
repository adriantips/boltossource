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
 *                  /time, /reboot, /log, /screen.bmp; and for measuring
 *                  speed, /perf, /bench and /anim (MINI.md, "How fast").
 *
 *  Under QEMU the same services run on the virtio card, which QEMU's DHCP
 *  makes 10.0.2.15 and its port forwards localhost:5901 and :8080 on the Mac.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "fs.h"
#include "rt.h"
#include "wlan.h"
#include "bt.h"
#include "pkg.h"
#include "store.h"
#include "crypto.h"
#include "kprintf.h"
#include "kheap.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "update.h"

#define PI_IP     0x0A4D0001u          /* 10.77.0.1                            */
#define MAC_IP    0x0A4D0002u          /* 10.77.0.2: what the Mac is given     */
#define MASK24    0xFFFFFF00u
#define MDNS_IP   0xE00000FBu          /* 224.0.0.251                          */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }

/* ---- DHCP server --------------------------------------------------------------- */
static int dhcp_leases;
static char job_url[300];

static int job_wifi;                     /* 1 start, 2 scan */
void netsvc_jobs(void) {
    if (job_wifi) {
        int j = job_wifi;
        job_wifi = 0;
        if (j == 1) wifi_start();
        else if (j == 2) wifi_scan();
        else if (j == 3) bt_start();
        else if (j == 4) bt_scan();
        else if (j >= 100) bt_pair(j - 100);
    }
    if (!job_url[0]) return;
    char u[300];
    snprintf(u, sizeof u, "%s", job_url);
    job_url[0] = 0;
    void web_fetch_test(const char *url);
    web_fetch_test(u);
}

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
static struct { tcb_t *c; int row, w, h; } shot;

static void shot_pump(void) {
    if (!shot.c) return;
    if (!tcp_open(shot.c)) { shot.c = 0; return; }
    if (shot.w != scr_w || shot.h != scr_h) { tcp_close(shot.c); shot.c = 0; return; }   /* resized under it */
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
    shot.c = c; shot.row = scr_h - 1; shot.w = scr_w; shot.h = scr_h;
}

/* ---- uploads (development) --------------------------------------------------------
 *  POST /pkg: a .bpk, installed as if it had come from boltos.dev.
 *  POST /store?name=<name>: a file straight into the SD card's store (a
 *  language model, say, over the Mac's link instead of Wi-Fi).
 *  The body comes over many polls; the reply says how it went.            */
static struct {
    tcb_t *c; int kind;                   /* 1 a package, 2 a store file, 3 a file on the card,
                                             4 boot firmware for the card's boot area */
    uint64_t len, got, written;
    uint8_t *buf; uint32_t blen, cap;
    const store_entry_t *e; char name[48];
    sha256_ctx sha; uint64_t t0, last;    /* last: when bytes last came */
    char fpath[256];
} up;

/* %xx and + in a query */
static void url_decode(const char *q, char *out, int cap) {
    int k = 0;
    for (; *q && *q != '&' && k < cap - 1; q++) {
        if (*q == '+') out[k++] = ' ';
        else if (*q == '%' && q[1] && q[2]) { char hx[3] = { q[1], q[2], 0 }; out[k++] = (char)strtol(hx, 0, 16); q += 2; }
        else out[k++] = *q;
    }
    out[k] = 0;
}

static void up_finish(const char *msg) {
    char b[256];
    snprintf(b, sizeof b, "%s\n", msg);
    http_text(up.c, 200, b);
    tcp_close(up.c);
    if (up.buf) kfree(up.buf);
    memset(&up, 0, sizeof up);
}

/* ---- the card's boot area (POST /firmware, pi.py firmware) -----------------
 *  What the H618's boot ROM runs sits 8 KiB into the card, in front of the
 *  FAT partition at 1 MiB: BoltBoot (mini/boot/), or U-Boot before it. This
 *  writes a new one there -- only an image the ROM would accept (eGON magic,
 *  a length that is a multiple of 512, the right sum), only sectors 16-2047,
 *  then reads it all back. The files on the card are never touched.        */
int sd_read(uint64_t lba, void *buf, uint32_t n);
int sd_write(uint64_t lba, const void *buf, uint32_t n);

static void firmware_write(const uint8_t *img, uint32_t len, char *msg, int cap) {
    if (len < 512 || memcmp(img + 4, "eGON.BT0", 8)) { snprintf(msg, cap, "not an eGON boot image"); return; }
    uint32_t n = *(const uint32_t *)(img + 0x10), want = *(const uint32_t *)(img + 0x0C), sum = 0;
    if (!n || (n & 511) || n > 0xC000 || n > len) { snprintf(msg, cap, "bad eGON length %u", n); return; }
    for (uint32_t o = 0; o < n; o += 4) sum += o == 0x0C ? 0x5F0A6C39u : *(const uint32_t *)(img + o);
    if (sum != want) { snprintf(msg, cap, "eGON sum is %08x, the header says %08x", sum, want); return; }
    uint32_t secs = (len + 511) / 512;
    if (16 + secs > 2048) { snprintf(msg, cap, "%u bytes: more than fits before the partition", len); return; }
    uint8_t *pad = kmalloc(secs * 512), *back = kmalloc(secs * 512);
    if (!pad || !back) { if (pad) kfree(pad); if (back) kfree(back); snprintf(msg, cap, "out of memory"); return; }
    memset(pad, 0, secs * 512);
    memcpy(pad, img, len);
    int bad = sd_write(16, pad, secs) || sd_read(16, back, secs) || memcmp(pad, back, secs * 512);
    kprintf("[fw] boot area: %u bytes (boot0 %u) %s\n", len, n, bad ? "FAILED" : "written and read back");
    if (bad) snprintf(msg, cap, "the card did not take it: the boot area may be damaged, rewrite it now");
    else snprintf(msg, cap, "wrote %u bytes to the boot area (boot0 %u), read back the same; "
                  "it runs at the next power-on", len, n);
    kfree(pad); kfree(back);
}

static int up_flush(void) {
    if (!up.blen) return 0;
    if (store_write(up.e, up.written, up.buf, up.blen)) return -1;
    up.written += up.blen; up.blen = 0;
    return 0;
}

static void up_take(const uint8_t *p, uint32_t n) {
    while (n && up.got < up.len) {
        uint32_t k = up.cap - up.blen;
        if (k > n) k = n;
        if (k > up.len - up.got) k = (uint32_t)(up.len - up.got);
        memcpy(up.buf + up.blen, p, k);
        if (up.kind == 2) sha256_update(&up.sha, p, k);
        up.blen += k; up.got += k; p += k; n -= k;
        if (up.kind == 2 && up.blen == up.cap && up_flush()) { up_finish("the card won't take a write"); return; }
    }
}

static void up_pump(void) {
    if (!up.c) return;
    uint8_t b[16384];
    if (!up.last) up.last = now_ms();
    for (int i = 0; i < 64 && up.got < up.len; i++) {
        int n = tcp_read(up.c, b, sizeof b);
        if (n <= 0) break;
        up.last = now_ms();
        up_take(b, (uint32_t)n);
        if (!up.c) return;
    }
    /* A link that went away mid-upload (a cable out) never says so: after 30 s
     * of nothing, the upload is given up, so the next one isn't "busy". */
    if (up.got < up.len && now_ms() - up.last > 30000) {
        kprintf("[http] upload stalled at %lu of %lu bytes: given up\n", (unsigned long)up.got, (unsigned long)up.len);
        tcp_abort(up.c);
        if (up.buf) kfree(up.buf);
        memset(&up, 0, sizeof up);
        return;
    }
    if (up.got >= up.len) {
        char msg[200];
        if (up.kind == 1) {
            pkg_install_file(up.buf, (uint32_t)up.len, msg, sizeof msg);
        } else if (up.kind == 4) {
            firmware_write(up.buf, (uint32_t)up.len, msg, sizeof msg);
        } else if (up.kind == 3) {
            char dirp[256]; snprintf(dirp, sizeof dirp, "%s", up.fpath);
            char *sl = strrchr(dirp, '/');
            if (sl && sl != dirp) { *sl = 0; fs_mkdir(dirp); }
            if (fs_write(up.fpath, up.buf, (uint32_t)up.len)) snprintf(msg, sizeof msg, "can't write %s: %s", up.fpath, fs_why());
            else snprintf(msg, sizeof msg, "wrote %s, %lu bytes", up.fpath, (unsigned long)up.len);
        } else {
            uint8_t sum[32];
            sha256_final(&up.sha, sum);
            if (up_flush() || store_progress(up.name, up.len, 1, sum)) snprintf(msg, sizeof msg, "the card won't take a write");
            else snprintf(msg, sizeof msg, "stored %s, %lu bytes in %lu ms", up.name, (unsigned long)up.len,
                          (unsigned long)(now_ms() - up.t0));
        }
        up_finish(msg);
        return;
    }
    if (!tcp_open(up.c)) { kprintf("[http] upload cut off at %lu bytes\n", (unsigned long)up.got); tcp_abort(up.c); if (up.buf) kfree(up.buf); memset(&up, 0, sizeof up); }
}

/* The request's headers are in; what's after them is the body's start. */
static int up_begin(int i, const char *path) {
    char *r = http[i].req;
    const char *cl = strcasestr(r, "\r\nContent-Length:");
    char *body = strstr(r, "\r\n\r\n");
    if (up.c || !cl || !body) return -1;
    memset(&up, 0, sizeof up);
    up.len = strtoull(cl + 17, 0, 10);
    up.c = http[i].c; up.t0 = now_ms();
    if (!strncmp(path, "/fs?path=", 9)) {
        up.kind = 3; up.cap = (uint32_t)up.len;
        url_decode(path + 9, up.fpath, sizeof up.fpath);
        if (up.len > (8u << 20) || !(up.buf = kmalloc(up.cap + 1))) { up_finish("too big"); return 0; }
    } else if (!strcmp(path, "/firmware")) {
        up.kind = 4; up.cap = (uint32_t)up.len;
        if (up.len > (1016u << 10) || !(up.buf = kmalloc(up.cap + 1))) { up_finish("too big"); return 0; }
    } else if (!strcmp(path, "/pkg")) {
        up.kind = 1; up.cap = (uint32_t)up.len;
        if (up.len > (16u << 20) || !(up.buf = kmalloc(up.cap + 1))) { up_finish("too big"); return 0; }
    } else {
        up.kind = 2; up.cap = 256u << 10;              /* a short write: the link keeps up */
        snprintf(up.name, sizeof up.name, "%s", path + 12);
        if (store_open(1) != 0) { up_finish(store_why()); return 0; }
        up.e = store_create(up.name, up.len);
        if (!up.e || !(up.buf = kmalloc(up.cap))) { up_finish(up.e ? "out of memory" : store_why()); return 0; }
        sha256_init(&up.sha);
    }
    body += 4;
    up_take((const uint8_t *)body, (uint32_t)(http[i].len - (body - r)));
    http[i].c = 0;
    return 0;
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

    if (!strcmp(method, "POST") && (!strcmp(path, "/pkg") || !strncmp(path, "/store?name=", 12) || !strncmp(path, "/fs?path=", 9))) {
        if (up_begin(i, path) == 0) return;
        http_text(c, 503, "busy, or no Content-Length\n");
    }
    else if (strcmp(path, "/") == 0) http_page(c);
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
    } else if (!strcmp(method, "POST") && !strcmp(path, "/firmware")) {
        if (up_begin(i, path) == 0) return;
        http_text(c, 503, "busy, or no Content-Length\n");
    } else if (!strncmp(path, "/regs?", 6)) {
        /* Development: read-only MMIO words, /regs?a=0x3001000&n=64 -- to see
         * what the firmware left in a block. The SoC's register space only,
         * 4 KiB at most; a block with its bus clock off can still hang. */
        const char *pa = strstr(path, "a="), *pn = strstr(path, "n=");
        uint64_t a = pa ? strtoull(pa + 2, 0, 0) : 0;
        long n = pn ? strtol(pn + 2, 0, 0) : 16;
        if (n < 1) n = 1;
        if (n > 1024) n = 1024;
        a &= ~3ull;
        if (a < 0x01000000ull || a + (uint64_t)n * 4 > 0x09100000ull) http_text(c, 400, "usage: /regs?a=ADDR&n=WORDS, ADDR in 0x01000000..0x090FFFFF\n");
        else {
            char *buf = kmalloc(16384);
            if (buf) {
                int k = 0;
                for (long w = 0; w < n && k < 16000; w++) {
                    if (w % 4 == 0) k += snprintf(buf + k, 16384 - k, "%s%08lx:", w ? "\n" : "", (unsigned long)(a + w * 4));
                    k += snprintf(buf + k, 16384 - k, " %08x", r32((uintptr_t)(a + w * 4)));
                }
                snprintf(buf + k, 16384 - k, "\n");
                http_text(c, 200, buf);
                kfree(buf);
            }
        }
    } else if (strncmp(path, "/keys?", 6) == 0) {
        /* Development: type at the shell. Text as it is, and {enter} {tab} {esc}
         * {bs} {up} {down} {left} {right} {pgdn} {pgup} {home} {end} {s-tab}
         * {s-left} {s-right} {s-up} {s-down} {del} {c-x} {c-s-x} {a-up} {f1}..{f8},
         * %XX and + as in a URL. */
        char t[300]; int n = 0;
        for (const char *p = path + 6; *p && n < (int)sizeof t - 1; p++) {
            if (*p == '%' && p[1] && p[2]) { char h[3] = { p[1], p[2], 0 }; t[n++] = (char)strtol(h, 0, 16); p += 2; }
            else t[n++] = *p == '+' ? ' ' : *p;
        }
        t[n] = 0;
        static const struct { const char *name; int key, mods; } named[] = {
            { "enter", KEY_ENTER, 0 }, { "tab", KEY_TAB, 0 }, { "s-tab", KEY_TAB, MOD_SHIFT }, { "esc", KEY_ESC, 0 },
            { "bs", KEY_BACKSPACE, 0 }, { "up", KEY_UP, 0 }, { "down", KEY_DOWN, 0 }, { "left", KEY_LEFT, 0 },
            { "right", KEY_RIGHT, 0 }, { "s-left", KEY_LEFT, MOD_SHIFT }, { "s-right", KEY_RIGHT, MOD_SHIFT },
            { "pgdn", KEY_PGDN, 0 }, { "pgup", KEY_PGUP, 0 }, { "home", KEY_HOME, 0 }, { "end", KEY_END, 0 },
            { "f1", KEY_F1, 0 }, { "f2", KEY_F2, 0 }, { "f3", KEY_F3, 0 }, { "f4", KEY_F4, 0 }, { "f5", KEY_F5, 0 }, { "f6", KEY_F6, 0 },
            { "f7", KEY_F7, 0 }, { "f8", KEY_F8, 0 }, { "s-f5", KEY_F5, MOD_SHIFT }, { "del", KEY_DELETE, 0 },
            { "s-up", KEY_UP, MOD_SHIFT }, { "s-down", KEY_DOWN, MOD_SHIFT }, { "s-home", KEY_HOME, MOD_SHIFT },
            { "s-end", KEY_END, MOD_SHIFT }, { "c-home", KEY_HOME, MOD_CTRL }, { "c-end", KEY_END, MOD_CTRL },
            { "super", KEY_SUPER, 0 },
        };
        for (char *p = t; *p; p++) {
            if (*p == '{') {
                char *e = strchr(p, '}');
                if (e) {
                    *e = 0;
                    /* {c-x}, {c-s-x}: a letter with Ctrl (and Shift); {s-up} and friends */
                    if (!strncmp(p + 1, "c-s-", 4) && p[5] && !p[6]) input_key(KEY_CHAR, (unsigned char)p[5], MOD_CTRL | MOD_SHIFT, SRC_SERIAL);
                    else if (!strncmp(p + 1, "c-", 2) && p[3] && !p[4]) input_key(KEY_CHAR, (unsigned char)p[3], MOD_CTRL, SRC_SERIAL);
                    else if (!strncmp(p + 1, "a-", 2) && !strcmp(p + 3, "up")) input_key(KEY_UP, 0, MOD_ALT, SRC_SERIAL);
                    else if (!strncmp(p + 1, "a-", 2) && !strcmp(p + 3, "down")) input_key(KEY_DOWN, 0, MOD_ALT, SRC_SERIAL);
                    for (unsigned k = 0; k < sizeof named / sizeof named[0]; k++)
                        if (!strcmp(p + 1, named[k].name)) input_key(named[k].key, 0, named[k].mods, SRC_SERIAL);
                    p = e;
                    continue;
                }
            }
            input_key(KEY_CHAR, (unsigned char)*p, 0, SRC_SERIAL);
        }
        http_text(c, 200, "ok\n");
    } else if (strncmp(path, "/fetch?", 7) == 0) {
        /* Development: fetch a URL with the browser's HTTP stack and log what came
         * back. Done from the main loop (netsvc_jobs), not in here: a fetch waits,
         * and this handler is part of what keeps turning while it does. */
        snprintf(job_url, sizeof job_url, "%s", path + 7);
        http_text(c, 200, "queued: see /log\n");
    } else if (strncmp(path, "/bt", 3) == 0) {
        /* Development: Bluetooth. /bt?up, /bt?scan, /bt?pair=N; /bt lists what it found. */
        if (!strcmp(path, "/bt?up")) job_wifi = 3;
        else if (!strcmp(path, "/bt?scan")) job_wifi = 4;
        else if (!strncmp(path, "/bt?pair=", 9)) job_wifi = 100 + atoi(path + 9);
        char *buf = kmalloc(8192);
        if (buf) {
            int n = bt_debug(buf, 1024);
            for (int i = 0; bt_device(i) && n < 7900; i++) {
                const bt_device_t *d = bt_device(i);
                n += snprintf(buf + n, (size_t)(8192 - n), "%2d %s %-30s %-20s %4d dBm state %d cod %06x\n", i, d->addr_str,
                              d->name, d->kind, d->rssi, d->state, (unsigned)d->cod);
            }
            http_text(c, 200, buf);
            kfree(buf);
        }
    } else if (strncmp(path, "/wifi", 5) == 0) {
        /* Development: Wi-Fi. /wifi?up starts it, /wifi?scan scans (both from the main
         * loop, like /fetch); /wifi says where it is and what it found. */
        if (!strcmp(path, "/wifi?up")) job_wifi = 1;
        else if (!strcmp(path, "/wifi?scan")) job_wifi = 2;
        char *buf = kmalloc(8192);
        if (buf) {
            int n = wifi_debug(buf, 1024);
            const wifi_net_t *nets;
            int k = wifi_nets(&nets);
            static const char *sec[] = { "open", "WPA2", "other" };
            for (int i = 0; i < k && n < 7900; i++)
                n += snprintf(buf + n, (size_t)(8192 - n), "%-32s %02x:%02x:%02x:%02x:%02x:%02x ch %3d %4d dBm %s\n", nets[i].ssid,
                              nets[i].bssid[0], nets[i].bssid[1], nets[i].bssid[2], nets[i].bssid[3], nets[i].bssid[4],
                              nets[i].bssid[5], nets[i].channel, nets[i].rssi, sec[nets[i].security % 3]);
            http_text(c, 200, buf);
            kfree(buf);
        }
    } else if (strncmp(path, "/bench", 6) == 0) {
        int shell_bench(int n, char *out, int cap);
        long nf = 30; query_int(path, "n=", &nf);
        char buf[256];
        shell_bench((int)nf, buf, sizeof buf);
        http_text(c, 200, buf);
    } else if (strncmp(path, "/anim", 5) == 0) {
        void shell_anim(int mode, int ms);
        long mode = 0, ms = 5000;
        query_int(path, "mode=", &mode); query_int(path, "ms=", &ms);
        shell_anim((int)mode, (int)ms);
        http_text(c, 200, "ok\n");
    } else if (!strcmp(path, "/pkg") || !strcmp(path, "/store")) {
        char *buf = kmalloc(16384);
        if (buf) { pkg_debug(buf, 16384); http_text(c, 200, buf); kfree(buf); }
    } else if (!strncmp(path, "/vm", 3)) {
        /* the Firefox machine (vm.c): ?start, ?stop, ?con for its console */
        int vm_dev(const char *q, char *out, int cap);
        char *buf = kmalloc(8192);
        if (buf) { vm_dev(path + 3, buf, 8192); http_text(c, 200, buf); kfree(buf); }
    } else if (!strncmp(path, "/lx", 3)) {
        /* a Linux program (lx.c): ?run=/path+arg&packs=a.pak,b.pak, ?stop, ?status, ?con */
        int lx_dev(const char *q, char *out, int cap);
        char *buf = kmalloc(32768);
        if (buf) { lx_dev(path + 3, buf, 32768); http_text(c, 200, buf); kfree(buf); }
    } else if (!strncmp(path, "/app?", 5)) {
        /* /app?<id>&<cmd>: the package's own dev hook; %20 and + are spaces */
        char id[32] = "", cmd[256] = "";
        const char *a = path + 5, *amp = strchr(a, '&');
        snprintf(id, sizeof id, "%.*s", amp ? (int)(amp - a) : (int)strlen(a), a);
        int k = 0;
        for (const char *q = amp ? amp + 1 : ""; *q && k < (int)sizeof cmd - 1; q++) {
            if (*q == '+') cmd[k++] = ' ';
            else if (*q == '%' && q[1] && q[2]) { char hx[3] = { q[1], q[2], 0 }; cmd[k++] = (char)strtol(hx, 0, 16); q += 2; }
            else cmd[k++] = *q;
        }
        cmd[k] = 0;
        char *buf = kmalloc(65536);
        if (buf) { pkg_dev(id, cmd, buf, 65536); http_text(c, 200, buf); kfree(buf); }
    } else if (!strncmp(path, "/fs?", 4)) {
        /* the card's files: ?path=/Code lists a folder or sends a file, ?rm=... removes,
         * POST ?path=... writes (above), ?df the room */
        char p[256];
        if (!strncmp(path + 4, "rm=", 3)) { url_decode(path + 7, p, sizeof p); http_text(c, 200, fs_remove(p) ? fs_why() : "removed\n"); }
        else if (!strcmp(path + 4, "df")) {
            uint64_t fr, tot; char b[160];
            if (fs_space(&fr, &tot)) snprintf(b, sizeof b, "%s\n", fs_why());
            else snprintf(b, sizeof b, "%lu of %lu KiB free\n", (unsigned long)(fr >> 10), (unsigned long)(tot >> 10));
            http_text(c, 200, b);
        } else {
            url_decode(!strncmp(path + 4, "path=", 5) ? path + 9 : "/", p, sizeof p);
            fs_ent_t st;
            if (fs_stat(p, &st)) http_text(c, 404, fs_why());
            else if (st.dir) {
                fs_ent_t *e = kmalloc(sizeof(fs_ent_t) * 256);
                char *b = kmalloc(32768);
                if (e && b) {
                    int n = fs_list(p, e, 256), o = 0;
                    for (int k2 = 0; k2 < n && o < 32000; k2++) {
                        char d[24]; fs_date_str(e[k2].date, e[k2].time, d, sizeof d);
                        o += snprintf(b + o, (size_t)(32768 - o), "%s %9u %s%s\n", d, e[k2].size, e[k2].name, e[k2].dir ? "/" : "");
                    }
                    if (n < 0) snprintf(b, 32768, "%s\n", fs_why()); else if (!n) snprintf(b, 32768, "(empty)\n");
                    http_text(c, 200, b);
                }
                kfree(e); kfree(b);
            } else {
                char *d; uint32_t n;
                if (fs_read(p, &d, &n)) http_text(c, 500, fs_why());
                else { http_text(c, 200, d); kfree(d); }
            }
        }
    } else if (!strncmp(path, "/run?", 5)) {
        char cmd[256], *buf = kmalloc(32768);
        url_decode(path + 5, cmd, sizeof cmd);
        /* url_decode stops at '&': keep "file=..." whole */
        if (buf) { run_dev(cmd, buf, 32768); http_text(c, 200, buf); kfree(buf); }
    } else if (!strncmp(path, "/update", 7)) {
        /* /update: where updates stand; ?check, ?install, ?auto=0|1 */
        char buf[1024];
        update_dev(path[7] == '?' ? path + 8 : "", buf, sizeof buf);
        http_text(c, 200, buf);
    } else if (strcmp(path, "/perf") == 0) {
        char buf[2048];
        perf_report(buf, sizeof buf);
        http_text(c, 200, buf);
    } else if (strcmp(path, "/log?prev") == 0) {
        char *buf = kmalloc(256 * 1024);
        if (buf) { klog_previous(buf, 256 * 1024); http_text(c, 200, buf); kfree(buf); }
    } else if (strcmp(path, "/log") == 0) {
        char *buf = kmalloc(256 * 1024);
        if (buf) { klog_snapshot(buf, 256 * 1024); http_text(c, 200, buf); kfree(buf); }
    } else if (strcmp(path, "/debug") == 0) {
        int usbh_debug(char *out, int cap);
        char *buf = kmalloc(8192);
        int wcn_debug(char *out, int cap);
        if (buf) {
            int n = eth_debug(buf, 2048);
            n += wcn_debug(buf + n, 1024);
            n += wifi_debug(buf + n, 1024);
            usbh_debug(buf + n, 8192 - n);
            http_text(c, 200, buf);
            kfree(buf);
        }
    } else if (strncmp(path, "/hdmi", 5) == 0 && (path[5] == 0 || path[5] == '?')) {
        int hdmi_debug(const char *path, char *out, int cap);
        char *buf = kmalloc(4096);
        if (buf) { hdmi_debug(path, buf, 4096); http_text(c, 200, buf); kfree(buf); }
    } else if (strncmp(path, "/lcd", 4) == 0 && (path[4] == 0 || path[4] == '?')) {
        /* the SPI screen (lcd.c): state, touch readings, ?mode= ?flip= ?inv= ?rgb= ?madctl= ?mhz= ?fill= ?cal */
        int lcd_debug(const char *path, char *out, int cap);
        char *buf = kmalloc(4096);
        if (buf) { lcd_debug(path, buf, 4096); http_text(c, 200, buf); kfree(buf); }
    } else if (strcmp(path, "/screen.bmp") == 0) {
        http_shot(c);
        http[i].c = 0;                         /* the pump owns it now         */
        return;
    } else http_text(c, 404, "not here\n");
    tcp_close(c);
    http[i].c = 0;
}

static void http_poll(void) {
    /* A handler that waits (an /app command, a fetch) keeps the network
     * turning, which comes back here: the same request would be handled
     * again inside itself. While one is running, the others wait. */
    static int busy;
    if (busy) { shot_pump(); return; }
    busy = 1;
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
    up_pump();
    busy = 0;
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
