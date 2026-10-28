/* ===========================================================================
 *  BoltOS Mini  -  mini/inet.c
 *
 *  The Pi as a client on somebody else's network -- the Ethernet socket and
 *  the router behind it, or QEMU's user network on the Mac:
 *
 *    DHCP client   an address, a router and a DNS server for every link
 *                  that isn't the Mac's (that one we serve, netsvc.c).
 *    DNS           A records, through the server DHCP named, cached by TTL.
 *    SNTP          the time, once there is a route: the Zero3 has no
 *                  battery-backed clock, and HTTPS certificates are only
 *                  valid between two dates.
 *
 *  inet_resolve() blocks, the way a page load wants it to, by turning the
 *  rest of the machine over (mini_pump) until the answer comes or time is up.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }

/* ---- DHCP client ------------------------------------------------------------- */
enum { DC_IDLE, DC_DISCOVER, DC_REQUEST, DC_BOUND };
typedef struct {
    netif_t *nif;
    int state, tries;
    uint32_t xid, offer, server, lease;
    uint64_t next, renew_at;
} dhcpc_t;
static dhcpc_t dc[3];
static int ndc;

static void dhcp_send(dhcpc_t *d, int type) {
    uint8_t r[300];
    memset(r, 0, sizeof r);
    r[0] = 1; r[1] = 1; r[2] = 6;
    put32(r + 4, d->xid);
    put16(r + 10, 0x8000);                                 /* answer by broadcast */
    if (d->state == DC_BOUND) put32(r + 12, d->nif->ip);   /* renewing: ciaddr     */
    memcpy(r + 28, d->nif->mac, 6);
    r[236] = 99; r[237] = 130; r[238] = 83; r[239] = 99;
    int o = 240;
    r[o++] = 53; r[o++] = 1; r[o++] = (uint8_t)type;
    r[o++] = 61; r[o++] = 7; r[o++] = 1; memcpy(r + o, d->nif->mac, 6); o += 6;
    if (type == 3 && d->state != DC_BOUND) {
        r[o++] = 50; r[o++] = 4; put32(r + o, d->offer); o += 4;
        r[o++] = 54; r[o++] = 4; put32(r + o, d->server); o += 4;
    }
    r[o++] = 12; r[o++] = 6; memcpy(r + o, "boltos", 6); o += 6;
    r[o++] = 55; r[o++] = 4; r[o++] = 1; r[o++] = 3; r[o++] = 6; r[o++] = 42;   /* mask, router, DNS, NTP */
    r[o++] = 255;
    uint32_t src = d->state == DC_BOUND ? d->nif->ip : 0;
    udp_send(d->nif, src, 0xFFFFFFFFu, 0, 68, 67, r, 300);
}

static uint32_t ntp_hint;

static void dhcp_rx(const udp_meta_t *m, const uint8_t *p, int len) {
    if (len < 240 || p[0] != 2 || be32(p + 236) != 0x63825363u) return;
    dhcpc_t *d = 0;
    for (int i = 0; i < ndc; i++) if (dc[i].nif == m->nif && be32(p + 4) == dc[i].xid) d = &dc[i];
    if (!d || memcmp(p + 28, d->nif->mac, 6) != 0) return;
    int type = 0;
    uint32_t mask = 0xFFFFFF00u, router = 0, dns = 0, server = 0, lease = 3600, ntp = 0;
    for (int o = 240; o + 1 < len && p[o] != 255; ) {
        if (p[o] == 0) { o++; continue; }
        int t = p[o], l = p[o + 1];
        const uint8_t *v = p + o + 2;
        if (o + 2 + l > len) break;
        if (t == 53 && l >= 1) type = v[0];
        else if (t == 1 && l >= 4) mask = be32(v);
        else if (t == 3 && l >= 4) router = be32(v);
        else if (t == 6 && l >= 4) dns = be32(v);
        else if (t == 54 && l >= 4) server = be32(v);
        else if (t == 51 && l >= 4) lease = be32(v);
        else if (t == 42 && l >= 4) ntp = be32(v);
        o += 2 + l;
    }
    uint32_t yi = be32(p + 16);
    if (type == 2 && d->state == DC_DISCOVER) {                   /* OFFER */
        d->offer = yi; d->server = server;
        d->state = DC_REQUEST; d->tries = 0;
        dhcp_send(d, 3);
        d->next = now_ms() + 2000;
    } else if (type == 5 && (d->state == DC_REQUEST || d->state == DC_BOUND)) {   /* ACK */
        netif_t *n = d->nif;
        int fresh = n->ip != yi;
        n->ip = yi; n->mask = mask; n->gw = router; n->dns = dns ? dns : router;
        if (ntp) ntp_hint = ntp;
        d->state = DC_BOUND;
        d->lease = lease < 60 ? 60 : lease;
        d->renew_at = now_ms() + (uint64_t)d->lease * 500;        /* half the lease */
        if (fresh) {
            char a[16], g[16], s[16];
            net_fmt_ip(yi, a); net_fmt_ip(router, g); net_fmt_ip(n->dns, s);
            kprintf("[dhcp] %s is %s, router %s, DNS %s (lease %us)\n", n->name, a, g, s, d->lease);
            shell_toast("Online", a);
        }
    } else if (type == 6) {                                       /* NAK: start over */
        d->state = DC_DISCOVER; d->tries = 0; d->next = now_ms();
        d->nif->ip = 0;
    }
}

static void dhcp_poll(void) {
    for (int i = 0; i < ndc; i++) {
        dhcpc_t *d = &dc[i];
        netif_t *n = d->nif;
        if (!n->up) {                                             /* cable out */
            if (d->state != DC_IDLE) { d->state = DC_IDLE; n->ip = n->gw = n->dns = 0; }
            continue;
        }
        if (d->state == DC_IDLE) { d->state = DC_DISCOVER; d->tries = 0; d->next = now_ms(); }
        if (d->state == DC_BOUND) {
            if (now_ms() >= d->renew_at) { d->xid = mini_rand(); dhcp_send(d, 3); d->renew_at = now_ms() + 30000; }
            continue;
        }
        if (now_ms() < d->next) continue;
        if (d->state == DC_REQUEST && ++d->tries > 3) { d->state = DC_DISCOVER; d->tries = 0; }
        if (d->state == DC_DISCOVER) { d->xid = mini_rand(); dhcp_send(d, 1); }
        else dhcp_send(d, 3);
        d->next = now_ms() + (d->tries < 4 ? 1000u << d->tries : 16000u);
        if (d->state == DC_DISCOVER) d->tries++;
    }
}

uint32_t inet_dns_server(void) {
    for (int i = 0; i < ndc; i++) if (dc[i].state == DC_BOUND && dc[i].nif->dns) return dc[i].nif->dns;
    return 0;
}

/* ---- DNS ----------------------------------------------------------------------- */
#define DNS_PORT 53053
#define DNS_N    32
static struct { char name[64]; uint32_t ip; uint64_t until; } dcache[DNS_N];
static uint16_t dns_id;
static uint32_t dns_answer;
static int dns_done;

static int dns_skip_name(const uint8_t *p, int len, int o) {
    while (o < len) {
        if ((p[o] & 0xC0) == 0xC0) return o + 2;
        if (!p[o]) return o + 1;
        o += 1 + p[o];
    }
    return -1;
}

static void dns_rx(const udp_meta_t *m, const uint8_t *p, int len) {
    (void)m;
    if (len < 12 || be16(p) != dns_id || !(p[2] & 0x80)) return;
    int rcode = p[3] & 15, qd = be16(p + 4), an = be16(p + 6);
    if (rcode) { dns_done = -1; return; }
    int o = 12;
    for (int i = 0; i < qd && o > 0; i++) { o = dns_skip_name(p, len, o); if (o > 0) o += 4; }
    for (int i = 0; i < an && o > 0 && o + 10 <= len; i++) {
        o = dns_skip_name(p, len, o);
        if (o < 0 || o + 10 > len) break;
        int type = be16(p + o), rdl = be16(p + o + 8);
        uint32_t ttl = be32(p + o + 4);
        o += 10;
        if (type == 1 && rdl == 4 && o + 4 <= len) {
            dns_answer = be32(p + o);
            dns_done = 1;
            (void)ttl;
            return;
        }
        o += rdl;                                                 /* a CNAME: read on */
    }
    dns_done = -1;
}

static int dns_query(uint32_t server, const char *host) {
    uint8_t q[300];
    memset(q, 0, 12);
    dns_id = (uint16_t)mini_rand();
    put16(q, dns_id); q[2] = 1;                                   /* recursion desired */
    put16(q + 4, 1);
    int o = 12;
    const char *s = host;
    while (*s) {
        const char *dot = strchr(s, '.');
        int n = dot ? (int)(dot - s) : (int)strlen(s);
        if (n <= 0 || n > 63 || o + n + 6 > (int)sizeof q) return -1;
        q[o++] = (uint8_t)n; memcpy(q + o, s, (size_t)n); o += n;
        s += n; if (*s == '.') s++;
    }
    q[o++] = 0;
    put16(q + o, 1); put16(q + o + 2, 1); o += 4;                  /* A, IN */
    netif_t *nif = net_route(server);
    if (!nif) return -1;
    return udp_send(nif, 0, server, 0, DNS_PORT, 53, q, o);
}

uint32_t inet_resolve(const char *host, uint32_t timeout_ms) {
    int ok;
    uint32_t lit = net_parse_ip(host, &ok);
    if (ok) return lit;
    if (strlen(host) >= sizeof dcache[0].name) return 0;
    for (int i = 0; i < DNS_N; i++)
        if (dcache[i].ip && strcmp(dcache[i].name, host) == 0 && now_ms() < dcache[i].until) return dcache[i].ip;
    uint64_t end = now_ms() + timeout_ms;
    while (!inet_dns_server() && now_ms() < end) mini_pump();      /* not online yet */
    uint32_t server = inet_dns_server();
    if (!server) return 0;
    for (int attempt = 0; now_ms() < end; attempt++) {
        dns_done = 0;
        if (dns_query(server, host) < 0) mini_pump();             /* ARP for the router first */
        uint64_t wait = now_ms() + (attempt ? 1500 : 400);
        while (!dns_done && now_ms() < wait && now_ms() < end) mini_pump();
        if (dns_done < 0) return 0;
        if (dns_done > 0) {
            int slot = 0;
            for (int i = 0; i < DNS_N; i++) if (dcache[i].until < dcache[slot].until) slot = i;
            strcpy(dcache[slot].name, host);
            dcache[slot].ip = dns_answer;
            dcache[slot].until = now_ms() + 300000;
            return dns_answer;
        }
    }
    return 0;
}

/* ---- SNTP ----------------------------------------------------------------------------- */
#define NTP_PORT 51123
static uint64_t ntp_next, ntp_sent;
static uint32_t ntp_server;
static int ntp_ok;

static void ntp_rx(const udp_meta_t *m, const uint8_t *p, int len) {
    (void)m;
    if (len < 48 || (p[0] & 7) != 4 || !ntp_sent) return;
    uint32_t secs = be32(p + 40);                                 /* transmit timestamp */
    if (secs < 2208988800u) return;
    uint64_t unix = (uint64_t)(secs - 2208988800u);
    unix += (now_ms() - ntp_sent) / 2000;                         /* half the round trip */
    int was = clock_known();
    clock_set_unix(unix);
    ntp_ok = 1;
    ntp_sent = 0;
    if (!was) kprintf("[ntp] the clock is set from the network\n");
}

static void ntp_poll(void) {
    if (now_ms() < ntp_next || !inet_dns_server()) return;
    ntp_next = now_ms() + (ntp_ok ? 3600000 : 5000);
    if (!ntp_server || !ntp_ok) {
        ntp_server = ntp_hint;
        if (!ntp_server) {                                         /* resolving blocks: do it once */
            static int asked;
            if (asked++ % 4 == 0) ntp_server = inet_resolve("pool.ntp.org", 1500);
        }
    }
    if (!ntp_server) return;
    uint8_t q[48];
    memset(q, 0, sizeof q);
    q[0] = 0x23;                                                  /* v4, client */
    netif_t *nif = net_route(ntp_server);
    if (nif && udp_send(nif, 0, ntp_server, 0, NTP_PORT, 123, q, 48) == 0) ntp_sent = now_ms();
}

/* ---- together --------------------------------------------------------------------------- */
void inet_init(void) {
    for (int i = 0; net_if(i) && ndc < 3; i++) {
        netif_t *n = net_if(i);
        if (strcmp(n->name, "usb0") == 0) continue;               /* we are that link's server */
        dc[ndc].nif = n;
        dc[ndc].state = DC_IDLE;
        ndc++;
    }
    udp_bind(68, dhcp_rx);
    udp_bind(DNS_PORT, dns_rx);
    udp_bind(NTP_PORT, ntp_rx);
}

void inet_poll(void) {
    dhcp_poll();
    ntp_poll();
}
