/* ===========================================================================
 *  BoltOS Mini  -  mini/netcompat.c
 *
 *  The desktop's HTTP client and TLS (net/http.c, net/tls.c) are written
 *  against blocking calls: connect and wait, send and wait, receive with a
 *  timeout. Mini's stack never blocks. This is the seam: each call here
 *  starts the work on Mini's TCP and turns the machine over (mini_pump)
 *  until it is done, so the links, the keyboard and VNC keep running while
 *  a page loads. The names are the desktop's, mapped by mini/include/net.h.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "netaddr.h"
#include "hw.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"
#include "http.h"

struct tcp_conn { tcb_t *t; };

netaddr netaddr_v4(uint32_t ip) {
    netaddr a;
    memset(&a, 0, sizeof a);
    a.af = NET_AF_INET;
    a.v4 = ip;
    return a;
}

void rtc_now(struct rtc_time *t) {
    int y, mo, d, h, mi, s, wd;
    /* x509.c compares UTC dates: undo the local offset clock_local adds. */
    int tz = clock_tz_minutes();
    clock_set_tz_minutes(0);
    clock_local(&y, &mo, &d, &h, &mi, &s, &wd);
    clock_set_tz_minutes(tz);
    t->year = (uint16_t)y; t->mon = (uint8_t)mo; t->day = (uint8_t)d;
    t->hour = (uint8_t)h; t->min = (uint8_t)mi; t->sec = (uint8_t)s;
}

struct tcp_conn *bt_tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms) {
    tcb_t *t = tcp_connect(ip, port);
    if (!t) return 0;
    uint64_t end = now_ms() + timeout_ms;
    while (tcp_connecting(t) && now_ms() < end) mini_pump();
    if (!tcp_open(t)) { tcp_abort(t); return 0; }
    struct tcp_conn *c = kmalloc(sizeof *c);
    if (!c) { tcp_abort(t); return 0; }
    c->t = t;
    return c;
}

struct tcp_conn *bt_tcp_connect_addr(const netaddr *dst, uint16_t port, uint32_t timeout_ms) {
    if (!dst || dst->af != NET_AF_INET) return 0;
    return bt_tcp_connect(dst->v4, port, timeout_ms);
}

int bt_tcp_send(struct tcp_conn *c, const void *data, uint32_t len) {
    if (!c) return -1;
    const uint8_t *p = data;
    uint32_t done = 0;
    uint64_t end = now_ms() + 15000;
    while (done < len) {
        if (!tcp_open(c->t) || now_ms() > end) return -1;
        int n = tcp_write(c->t, p + done, (int)(len - done));
        if (n < 0) return -1;
        done += (uint32_t)n;
        if (done < len) mini_pump();
    }
    return (int)len;
}

int bt_tcp_recv(struct tcp_conn *c, void *buf, uint32_t cap, uint32_t timeout_ms) {
    if (!c) return -1;
    uint64_t end = now_ms() + timeout_ms;
    for (;;) {
        if (tcp_avail(c->t) > 0) return tcp_read(c->t, buf, (int)cap);
        if (tcp_peer_closed(c->t)) return 0;
        if (now_ms() >= end) return -1;
        mini_pump();
    }
}

void bt_tcp_close(struct tcp_conn *c) {
    if (!c) return;
    tcp_close(c->t);
    kfree(c);
}

int bt_dns_resolve(const char *host, uint32_t *ip, uint32_t timeout_ms) {
    uint32_t a = inet_resolve(host, timeout_ms);
    if (!a) return 0;
    if (ip) *ip = a;
    return 1;
}

int bt_dns_resolve_addr(const char *host, netaddr *out, uint32_t timeout_ms) {
    uint32_t a = inet_resolve(host, timeout_ms);
    if (!a) return 0;
    if (out) *out = netaddr_v4(a);
    return 1;
}

/* /fetch?<url> and the Terminal's `fetch`: one GET through the whole stack. */
int web_fetch(const char *url, char *out, int cap, int *status, char *loc, int loc_cap) {
    return http_get(url, out, (uint32_t)cap, status, loc, (uint32_t)loc_cap);
}

void web_fetch_test(const char *url) {
    char *buf = kmalloc(512 * 1024);
    if (!buf) return;
    char loc[256] = "";
    int code = 0;
    uint64_t t0 = now_ms();
    int n = http_get(url, buf, 512 * 1024, &code, loc, sizeof loc);
    kprintf("[web] GET %s -> %d, %d bytes in %lu ms%s%s\n", url, code, n, now_ms() - t0,
            loc[0] ? ", Location: " : "", loc);
    if (n > 0) {
        char head[161];
        int k = 0;
        for (int i = 0; i < n && k < 160; i++) head[k++] = (buf[i] >= 32 && buf[i] < 127) ? buf[i] : ' ';
        head[k] = 0;
        kprintf("[web]   %s\n", head);
    }
    kfree(buf);
}
