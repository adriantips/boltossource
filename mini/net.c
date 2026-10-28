/* ===========================================================================
 *  BoltOS Mini  -  mini/net.c
 *
 *  A small IPv4 stack for a small machine on a short wire. The wire is
 *  usually a USB cable to a Mac -- a link with a sub-millisecond round trip
 *  that never loses a packet unless something is broken -- and what runs
 *  over it is a VNC server pushing a screen at tens of megabits. So:
 *
 *    ARP      answers for our address, and learns every sender it hears.
 *    IPv4     no fragments, no options, no routing beyond "on link or gw".
 *    ICMP     echo, so `ping 10.77.0.1` works from the Mac.
 *    UDP      a port table; DHCP and mDNS live on it (netsvc.c).
 *    TCP      passive open only. Each connection has a 256 KiB send ring
 *             and sends as much of it as the peer's window allows, without
 *             waiting for each ACK; loss is repaired go-back-N from the
 *             oldest unacknowledged byte, on a timeout or three duplicate
 *             ACKs. The receive side takes in-order data and ACKs it at once.
 *
 *  Everything is driven from net_poll() and net_rx(): there are no threads
 *  and no blocking calls, so a service checks its connections each loop.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

#define ETH_IP   0x0800
#define ETH_ARP  0x0806
#define PROTO_ICMP 1
#define PROTO_TCP  6
#define PROTO_UDP  17

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---- interfaces ------------------------------------------------------------- */
#define MAX_IF 4
static netif_t *ifs[MAX_IF];
static int nifs;

void net_register(netif_t *nif) {
    if (nifs < MAX_IF) ifs[nifs++] = nif;
    kprintf("[net] %s: %02x:%02x:%02x:%02x:%02x:%02x\n", nif->name, nif->mac[0], nif->mac[1],
            nif->mac[2], nif->mac[3], nif->mac[4], nif->mac[5]);
}
netif_t *net_if(int i) { return (i >= 0 && i < nifs) ? ifs[i] : 0; }

void net_fmt_ip(uint32_t ip, char *out) {
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

uint32_t net_parse_ip(const char *s, int *ok) {
    uint32_t ip = 0; int parts = 0;
    *ok = 0;
    while (parts < 4) {
        if (*s < '0' || *s > '9') return 0;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s++ - '0'); if (v > 255) return 0; }
        ip = (ip << 8) | v; parts++;
        if (parts < 4) { if (*s != '.') return 0; s++; }
    }
    if (*s) return 0;
    *ok = 1;
    return ip;
}

static uint32_t csum_add(uint32_t sum, const uint8_t *p, int len) {
    while (len > 1) { sum += (uint32_t)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (uint32_t)(p[0] << 8);
    return sum;
}
static uint16_t csum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ---- ARP -------------------------------------------------------------------- */
#define ARP_N 16
static struct { uint32_t ip; uint8_t mac[6]; uint64_t t; } arp[ARP_N];

static void arp_learn(uint32_t ip, const uint8_t *mac) {
    if (!ip || mac[0] & 1) return;
    int slot = 0;
    for (int i = 0; i < ARP_N; i++) {
        if (arp[i].ip == ip) { slot = i; break; }
        if (arp[i].t < arp[slot].t) slot = i;
    }
    arp[slot].ip = ip;
    memcpy(arp[slot].mac, mac, 6);
    arp[slot].t = now_ms() + 1;
}

static int arp_find(uint32_t ip, uint8_t *mac) {
    for (int i = 0; i < ARP_N; i++)
        if (arp[i].ip == ip && arp[i].t) { memcpy(mac, arp[i].mac, 6); return 1; }
    return 0;
}

static int eth_send(netif_t *nif, const uint8_t *dst, uint16_t type, uint8_t *frame, int paylen) {
    memcpy(frame, dst, 6);
    memcpy(frame + 6, nif->mac, 6);
    put16(frame + 12, type);
    int len = 14 + paylen;
    if (len < 60) { memset(frame + len, 0, (size_t)(60 - len)); len = 60; }
    if (nif->tx(nif, frame, len) != 0) { nif->tx_drops++; return -1; }
    nif->tx_pkts++; nif->tx_bytes += (uint64_t)len;
    return 0;
}

static void arp_request(netif_t *nif, uint32_t ip) {
    uint8_t f[64];
    uint8_t *a = f + 14;
    put16(a, 1); put16(a + 2, ETH_IP); a[4] = 6; a[5] = 4; put16(a + 6, 1);
    memcpy(a + 8, nif->mac, 6); put32(a + 14, nif->ip);
    memset(a + 18, 0, 6); put32(a + 24, ip);
    eth_send(nif, bcast_mac, ETH_ARP, f, 28);
}

static void arp_input(netif_t *nif, const uint8_t *a, int len) {
    if (len < 28 || be16(a) != 1 || be16(a + 2) != ETH_IP) return;
    uint16_t op = be16(a + 6);
    uint32_t sip = be32(a + 14), tip = be32(a + 24);
    arp_learn(sip, a + 8);
    if (op == 1 && nif->ip && tip == nif->ip) {
        uint8_t f[64];
        uint8_t *r = f + 14;
        put16(r, 1); put16(r + 2, ETH_IP); r[4] = 6; r[5] = 4; put16(r + 6, 2);
        memcpy(r + 8, nif->mac, 6); put32(r + 14, nif->ip);
        memcpy(r + 18, a + 8, 6); put32(r + 24, sip);
        eth_send(nif, a + 8, ETH_ARP, f, 28);
    }
}

/* ---- IPv4 ------------------------------------------------------------------- */
static uint16_t ip_id;

/* `frame` has room for the Ethernet and IP headers in front of the payload,
 * which the caller has already written at frame + 34. */
static int ip_send(netif_t *nif, const uint8_t *dmac, uint32_t src, uint32_t dst, uint8_t proto,
                   uint8_t *frame, int paylen) {
    uint8_t mac[6];
    if (!dmac) {
        uint32_t hop = ((dst & nif->mask) == (nif->ip & nif->mask) || !nif->gw) ? dst : nif->gw;
        if (dst == 0xFFFFFFFFu || (dst & 0xF0000000u) == 0xE0000000u) {
            if (dst == 0xFFFFFFFFu) memcpy(mac, bcast_mac, 6);
            else { mac[0] = 0x01; mac[1] = 0x00; mac[2] = 0x5E; mac[3] = (dst >> 16) & 0x7F;
                   mac[4] = (dst >> 8) & 0xFF; mac[5] = dst & 0xFF; }
        } else if (!arp_find(hop, mac)) {
            arp_request(nif, hop);
            return -1;
        }
        dmac = mac;
    }
    uint8_t *ip = frame + 14;
    ip[0] = 0x45; ip[1] = 0;
    put16(ip + 2, (uint16_t)(20 + paylen));
    put16(ip + 4, ip_id++);
    put16(ip + 6, 0x4000);                         /* don't fragment       */
    ip[8] = (dst & 0xF0000000u) == 0xE0000000u ? 255 : 64;
    ip[9] = proto;
    put16(ip + 10, 0);
    put32(ip + 12, src ? src : nif->ip);
    put32(ip + 16, dst);
    put16(ip + 10, csum_fold(csum_add(0, ip, 20)));
    return eth_send(nif, dmac, ETH_IP, frame, 20 + paylen);
}

/* ---- UDP -------------------------------------------------------------------- */
#define UDP_N 8
static struct { uint16_t port; udp_fn fn; } udp_tab[UDP_N];

int udp_bind(uint16_t port, udp_fn fn) {
    for (int i = 0; i < UDP_N; i++)
        if (!udp_tab[i].port) { udp_tab[i].port = port; udp_tab[i].fn = fn; return 0; }
    return -1;
}

int udp_send(netif_t *nif, uint32_t src_ip, uint32_t dst_ip, const uint8_t *dst_mac,
             uint16_t sport, uint16_t dport, const void *data, int len) {
    if (!nif || len > 1472) return -1;
    uint8_t f[1514];
    uint8_t *u = f + 34;
    put16(u, sport); put16(u + 2, dport); put16(u + 4, (uint16_t)(8 + len)); put16(u + 6, 0);
    memcpy(u + 8, data, (size_t)len);
    uint32_t s = src_ip ? src_ip : nif->ip;
    uint8_t ph[12];
    put32(ph, s); put32(ph + 4, dst_ip); ph[8] = 0; ph[9] = PROTO_UDP; put16(ph + 10, (uint16_t)(8 + len));
    uint16_t c = csum_fold(csum_add(csum_add(0, ph, 12), u, 8 + len));
    put16(u + 6, c ? c : 0xFFFF);
    if (dst_ip == 0xFFFFFFFFu && !dst_mac) dst_mac = bcast_mac;
    return ip_send(nif, dst_mac, s, dst_ip, PROTO_UDP, f, 8 + len);
}

static void udp_input(netif_t *nif, const uint8_t *eth, uint32_t src, uint32_t dst, const uint8_t *u, int len) {
    if (len < 8) return;
    int ulen = be16(u + 4);
    if (ulen < 8 || ulen > len) return;
    udp_meta_t m = { .nif = nif, .src_ip = src, .dst_ip = dst,
                     .sport = be16(u), .dport = be16(u + 2) };
    memcpy(m.src_mac, eth + 6, 6);
    for (int i = 0; i < UDP_N; i++)
        if (udp_tab[i].port == m.dport && udp_tab[i].fn) { udp_tab[i].fn(&m, u + 8, ulen - 8); return; }
}

/* ---- ICMP ------------------------------------------------------------------- */
static void icmp_input(netif_t *nif, const uint8_t *eth, uint32_t src, const uint8_t *p, int len) {
    if (len < 8 || p[0] != 8) return;                  /* echo request only    */
    if (len > 1472) return;
    uint8_t f[1514];
    uint8_t *r = f + 34;
    memcpy(r, p, (size_t)len);
    r[0] = 0; put16(r + 2, 0);
    put16(r + 2, csum_fold(csum_add(0, r, len)));
    ip_send(nif, eth + 6, nif->ip, src, PROTO_ICMP, f, len);
}

/* ---- TCP -------------------------------------------------------------------- */
enum { T_FREE = 0, T_SYN_RCVD, T_ESTAB, T_CLOSE_WAIT, T_FIN_WAIT1, T_FIN_WAIT2,
       T_CLOSING, T_LAST_ACK, T_TIME_WAIT,
       T_DEAD };   /* gone, but a service still holds the handle: freed on its tcp_close */
#define TF_FIN 0x01
#define TF_SYN 0x02
#define TF_RST 0x04
#define TF_PSH 0x08
#define TF_ACK 0x10

#define SBUF  (256 * 1024)
#define RBUF  (32 * 1024)
#define TCB_N 8
#define RTO_MIN 200
#define RTO_MAX 3000

struct tcb {
    int      state;
    netif_t *nif;
    uint32_t lip, rip;
    uint16_t lport, rport;
    uint8_t  rmac[6];
    uint32_t iss, snd_una, snd_nxt, snd_max, snd_wnd, rcv_nxt;
    uint16_t mss;
    uint8_t *sbuf; uint32_t s_start, s_len;     /* ring: s_len bytes from snd_una */
    uint8_t *rbuf; uint32_t r_head, r_tail;
    int      accepted, user_closed, fin_sent, peer_fin;
    uint32_t fin_seq;
    uint64_t rto_at;                            /* when the oldest unacked byte times out */
    uint32_t rto;
    int      dupacks;
    uint64_t last_rx, dead_at;
};

static struct tcb tcbs[TCB_N];
static uint16_t listeners[8];

int tcp_listen(uint16_t port) {
    for (int i = 0; i < 8; i++) if (listeners[i] == port) return 0;
    for (int i = 0; i < 8; i++) if (!listeners[i]) { listeners[i] = port; return 0; }
    return -1;
}

static int listening(uint16_t port) {
    for (int i = 0; i < 8; i++) if (listeners[i] == port) return 1;
    return 0;
}

/* A connection a service has accepted and not yet closed keeps its slot as
 * T_DEAD when the network ends it, so the handle the service holds can
 * never come to mean somebody else's new connection. */
static void tcb_free(struct tcb *c) {
    if (c->sbuf) kfree(c->sbuf);
    if (c->rbuf) kfree(c->rbuf);
    c->sbuf = c->rbuf = 0;
    if (c->accepted && !c->user_closed && c->state != T_DEAD) { c->state = T_DEAD; return; }
    memset(c, 0, sizeof *c);
}

static uint32_t rx_space(struct tcb *c) { return RBUF - (c->r_head - c->r_tail); }

/* Send one segment: `flags`, sequence `seq`, `len` bytes of the send ring
 * from offset `off` (bytes past snd_una). */
static int seg_send(struct tcb *c, uint8_t flags, uint32_t seq, uint32_t off, int len) {
    uint8_t f[1514];
    uint8_t *t = f + 34;
    int hl = (flags & TF_SYN) ? 24 : 20;
    put16(t, c->lport); put16(t + 2, c->rport);
    put32(t + 4, seq);
    put32(t + 8, c->rcv_nxt);
    t[12] = (uint8_t)((hl / 4) << 4);
    t[13] = flags;
    uint32_t win = rx_space(c); if (win > 0xFFFF) win = 0xFFFF;
    put16(t + 14, (uint16_t)win);
    put16(t + 16, 0); put16(t + 18, 0);
    if (flags & TF_SYN) { t[20] = 2; t[21] = 4; put16(t + 22, 1460); }   /* MSS */
    for (int i = 0; i < len; i++) t[hl + i] = c->sbuf[(c->s_start + off + (uint32_t)i) % SBUF];
    uint8_t ph[12];
    put32(ph, c->lip); put32(ph + 4, c->rip); ph[8] = 0; ph[9] = PROTO_TCP; put16(ph + 10, (uint16_t)(hl + len));
    put16(t + 16, csum_fold(csum_add(csum_add(0, ph, 12), t, hl + len)));
    return ip_send(c->nif, c->rmac, c->lip, c->rip, PROTO_TCP, f, hl + len);
}

static void send_ack(struct tcb *c) { seg_send(c, TF_ACK, c->snd_nxt, 0, 0); }

static void send_rst_reply(netif_t *nif, const uint8_t *eth, uint32_t src, uint32_t dst,
                           const uint8_t *t, int dlen) {
    struct tcb tmp; memset(&tmp, 0, sizeof tmp);
    tmp.nif = nif; tmp.lip = dst; tmp.rip = src;
    tmp.lport = be16(t + 2); tmp.rport = be16(t);
    memcpy(tmp.rmac, eth + 6, 6);
    uint8_t fl = t[13];
    if (fl & TF_ACK) { tmp.rcv_nxt = 0; seg_send(&tmp, TF_RST, be32(t + 8), 0, 0); }
    else { tmp.rcv_nxt = be32(t + 4) + (uint32_t)dlen + ((fl & TF_SYN) ? 1 : 0) + ((fl & TF_FIN) ? 1 : 0);
           seg_send(&tmp, TF_RST | TF_ACK, 0, 0, 0); }
}

/* Push out whatever the window allows. */
static void tcp_output(struct tcb *c) {
    if (c->state != T_ESTAB && c->state != T_CLOSE_WAIT &&
        c->state != T_FIN_WAIT1 && c->state != T_CLOSING && c->state != T_LAST_ACK) return;
    uint32_t wnd = c->snd_wnd ? c->snd_wnd : 0;
    for (int burst = 0; burst < 64; burst++) {
        uint32_t off = c->snd_nxt - c->snd_una;
        if (off >= c->s_len) break;
        uint32_t inflight = off;
        if (inflight >= wnd) break;
        uint32_t n = c->s_len - off;
        if (n > c->mss) n = c->mss;
        if (n > wnd - inflight) n = wnd - inflight;
        uint8_t fl = TF_ACK | ((off + n == c->s_len) ? TF_PSH : 0);
        if (seg_send(c, fl, c->snd_nxt, off, (int)n) != 0) break;   /* link busy  */
        if (c->snd_nxt == c->snd_una) c->rto_at = now_ms() + c->rto;
        c->snd_nxt += n;
        if ((int32_t)(c->snd_nxt - c->snd_max) > 0) c->snd_max = c->snd_nxt;
    }
    /* A shut window with data waiting: probe it when the timer runs out. */
    if (!wnd && c->s_len && c->snd_nxt == c->snd_una && !c->rto_at) c->rto_at = now_ms() + c->rto;
    /* Everything handed over and the user is done: our FIN. */
    if (c->user_closed && !c->fin_sent && c->snd_nxt == c->snd_una + c->s_len) {
        c->fin_seq = c->snd_nxt;
        if (seg_send(c, TF_FIN | TF_ACK, c->snd_nxt, 0, 0) == 0) {
            c->fin_sent = 1;
            c->snd_nxt++;
            if ((int32_t)(c->snd_nxt - c->snd_max) > 0) c->snd_max = c->snd_nxt;
            c->rto_at = now_ms() + c->rto;
            c->state = (c->state == T_CLOSE_WAIT) ? T_LAST_ACK : T_FIN_WAIT1;
        }
    }
}

static void tcp_input(netif_t *nif, const uint8_t *eth, uint32_t src, uint32_t dst,
                      const uint8_t *t, int len) {
    if (len < 20) return;
    int hl = (t[12] >> 4) * 4;
    if (hl < 20 || hl > len) return;
    uint8_t ph[12];
    put32(ph, src); put32(ph + 4, dst); ph[8] = 0; ph[9] = PROTO_TCP; put16(ph + 10, (uint16_t)len);
    if (csum_fold(csum_add(csum_add(0, ph, 12), t, len)) != 0) return;

    uint16_t sport = be16(t), dport = be16(t + 2);
    uint32_t seq = be32(t + 4), ack = be32(t + 8);
    uint8_t fl = t[13];
    uint32_t wnd = be16(t + 14);
    const uint8_t *data = t + hl;
    int dlen = len - hl;

    struct tcb *c = 0;
    for (int i = 0; i < TCB_N; i++)
        if (tcbs[i].state && tcbs[i].state != T_DEAD && tcbs[i].rip == src && tcbs[i].rport == sport && tcbs[i].lport == dport) { c = &tcbs[i]; break; }

    if (!c) {
        if ((fl & (TF_SYN | TF_ACK | TF_RST)) != TF_SYN || !listening(dport)) {
            if (!(fl & TF_RST)) send_rst_reply(nif, eth, src, dst, t, dlen);
            return;
        }
        for (int i = 0; i < TCB_N; i++) if (!tcbs[i].state) { c = &tcbs[i]; break; }
        if (!c) return;                                /* full: let them retry */
        memset(c, 0, sizeof *c);
        c->sbuf = kmalloc(SBUF);
        c->rbuf = kmalloc(RBUF);
        if (!c->sbuf || !c->rbuf) { tcb_free(c); return; }
        c->nif = nif; c->lip = dst; c->rip = src; c->lport = dport; c->rport = sport;
        memcpy(c->rmac, eth + 6, 6);
        c->mss = 536;
        for (int o = 20; o + 1 < hl; ) {               /* options: MSS          */
            uint8_t kind = t[o];
            if (kind == 0) break;
            if (kind == 1) { o++; continue; }
            uint8_t ol = t[o + 1];
            if (ol < 2) break;
            if (kind == 2 && ol == 4 && o + 4 <= hl) c->mss = be16(t + o + 2);
            o += ol;
        }
        if (c->mss > 1460) c->mss = 1460;
        c->iss = mini_rand() ^ (uint32_t)now_us();
        c->snd_una = c->iss; c->snd_nxt = c->snd_max = c->iss + 1;
        c->rcv_nxt = seq + 1;
        c->snd_wnd = wnd;
        c->rto = RTO_MIN;
        c->state = T_SYN_RCVD;
        c->last_rx = now_ms();
        seg_send(c, TF_SYN | TF_ACK, c->iss, 0, 0);
        c->rto_at = now_ms() + c->rto;
        return;
    }

    c->last_rx = now_ms();
    if (fl & TF_RST) { c->state = T_FREE; tcb_free(c); return; }

    if (c->state == T_DEAD) return;
    if (c->state == T_SYN_RCVD) {
        if (fl & TF_SYN) { seg_send(c, TF_SYN | TF_ACK, c->iss, 0, 0); return; }   /* their retry */
        if (!(fl & TF_ACK) || ack != c->iss + 1) return;
        c->snd_una = ack;
        c->state = T_ESTAB;
        c->rto_at = 0;
    }

    /* ACK processing */
    if (fl & TF_ACK) {
        uint32_t acked = ack - c->snd_una;
        uint32_t outstanding = c->snd_max - c->snd_una;
        if (acked > 0 && acked <= outstanding) {
            uint32_t data_acked = acked;
            if (c->fin_sent && ack == c->fin_seq + 1) data_acked--;     /* our FIN    */
            if (data_acked > c->s_len) data_acked = c->s_len;
            c->s_start = (c->s_start + data_acked) % SBUF;
            c->s_len -= data_acked;
            c->snd_una = ack;
            if ((int32_t)(ack - c->snd_nxt) > 0) c->snd_nxt = ack;   /* after a rewind */
            c->dupacks = 0;
            c->rto = RTO_MIN;
            c->rto_at = (c->snd_max != c->snd_una) ? now_ms() + c->rto : 0;
            if (c->fin_sent && ack == c->fin_seq + 1) {
                if (c->state == T_FIN_WAIT1) c->state = T_FIN_WAIT2;
                else if (c->state == T_CLOSING) { c->state = T_TIME_WAIT; c->dead_at = now_ms() + 500; }
                else if (c->state == T_LAST_ACK) { tcb_free(c); return; }
            }
        } else if (acked == 0 && dlen == 0 && outstanding && wnd == c->snd_wnd && !(fl & (TF_SYN | TF_FIN))) {
            if (++c->dupacks == 3) { c->snd_nxt = c->snd_una; c->dupacks = 0; }  /* fast retransmit */
        }
        c->snd_wnd = wnd;
    }

    /* Data, in order only. */
    if (dlen > 0) {
        if (seq == c->rcv_nxt && (c->state == T_ESTAB || c->state == T_FIN_WAIT1 || c->state == T_FIN_WAIT2)) {
            uint32_t take = (uint32_t)dlen;
            if (take > rx_space(c)) take = rx_space(c);
            for (uint32_t i = 0; i < take; i++) c->rbuf[(c->r_head++) % RBUF] = data[i];
            c->rcv_nxt += take;
        }
        send_ack(c);
    }

    if ((fl & TF_FIN) && seq + (uint32_t)dlen == c->rcv_nxt && !c->peer_fin) {
        c->peer_fin = 1;
        c->rcv_nxt++;
        send_ack(c);
        if (c->state == T_ESTAB) c->state = T_CLOSE_WAIT;
        else if (c->state == T_FIN_WAIT1) c->state = T_CLOSING;
        else if (c->state == T_FIN_WAIT2) { c->state = T_TIME_WAIT; c->dead_at = now_ms() + 500; }
    }
    tcp_output(c);
}

tcb_t *tcp_accept(uint16_t port) {
    for (int i = 0; i < TCB_N; i++) {
        struct tcb *c = &tcbs[i];
        if (c->state && !c->accepted && c->lport == port &&
            (c->state == T_ESTAB || c->state == T_CLOSE_WAIT)) { c->accepted = 1; return c; }
    }
    return 0;
}

int tcp_space(tcb_t *c) { return (c && c->state && !c->user_closed) ? (int)(SBUF - c->s_len) : 0; }
int tcp_unsent(tcb_t *c) { return c && c->state ? (int)c->s_len : 0; }

int tcp_write(tcb_t *c, const void *data, int len) {
    if (!c || !(c->state == T_ESTAB || c->state == T_CLOSE_WAIT) || c->user_closed) return -1;
    uint32_t room = SBUF - c->s_len;
    if ((uint32_t)len > room) len = (int)room;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t at = (c->s_start + c->s_len) % SBUF;
    for (int i = 0; i < len; ) {
        uint32_t chunk = SBUF - at;
        if (chunk > (uint32_t)(len - i)) chunk = (uint32_t)(len - i);
        memcpy(c->sbuf + at, p + i, chunk);
        at = (at + chunk) % SBUF; i += (int)chunk;
    }
    c->s_len += (uint32_t)len;
    tcp_output(c);
    return len;
}

int tcp_avail(tcb_t *c) { return c && c->rbuf ? (int)(c->r_head - c->r_tail) : 0; }

int tcp_read(tcb_t *c, void *buf, int cap) {
    if (!c || !c->rbuf) return -1;
    int was_full = rx_space(c) < 1460;
    int n = 0;
    uint8_t *o = (uint8_t *)buf;
    while (n < cap && c->r_tail != c->r_head) o[n++] = c->rbuf[(c->r_tail++) % RBUF];
    if (was_full && n) send_ack(c);                    /* window update      */
    return n;
}

int tcp_open(tcb_t *c) { return c && (c->state == T_ESTAB || c->state == T_CLOSE_WAIT); }
int tcp_peer_closed(tcb_t *c) { return !c || !c->state || c->state == T_DEAD || (c->peer_fin && c->r_head == c->r_tail); }
uint32_t tcp_peer(tcb_t *c, uint16_t *port) { if (port) *port = c ? c->rport : 0; return c ? c->rip : 0; }

void tcp_close(tcb_t *c) {
    if (!c || !c->state) return;
    if (c->state == T_DEAD) { memset(c, 0, sizeof *c); return; }
    c->user_closed = 1;
    c->accepted = 1;
    c->dead_at = now_ms() + 10000;                     /* give up eventually */
    tcp_output(c);
}

void tcp_abort(tcb_t *c) {
    if (!c || !c->state) return;
    if (c->state != T_DEAD) seg_send(c, TF_RST | TF_ACK, c->snd_nxt, 0, 0);
    c->user_closed = 1;
    tcb_free(c);
}

int tcp_conn_count(void) {
    int n = 0;
    for (int i = 0; i < TCB_N; i++) if (tcbs[i].state == T_ESTAB) n++;
    return n;
}

static void tcp_timers(void) {
    uint64_t now = now_ms();
    for (int i = 0; i < TCB_N; i++) {
        struct tcb *c = &tcbs[i];
        if (!c->state || c->state == T_DEAD) continue;
        if (c->dead_at && now >= c->dead_at &&
            (c->state == T_TIME_WAIT || c->user_closed)) { tcb_free(c); continue; }
        if (c->state == T_SYN_RCVD && now - c->last_rx > 5000) { tcb_free(c); continue; }
        /* A peer that vanished (the cable came out) is never going to ACK. */
        if (now - c->last_rx > 60000 && c->s_len) { tcb_free(c); continue; }
        if (c->rto_at && now >= c->rto_at) {
            if (c->state == T_SYN_RCVD) seg_send(c, TF_SYN | TF_ACK, c->iss, 0, 0);
            else {
                c->snd_nxt = c->snd_una;               /* go back N         */
                if (c->fin_sent && c->s_len == 0) {
                    seg_send(c, TF_FIN | TF_ACK, c->fin_seq, 0, 0);
                    c->snd_nxt = c->fin_seq + 1;
                } else if (c->fin_sent) {
                    c->fin_sent = 0;                   /* resent after the data */
                    c->state = (c->state == T_LAST_ACK) ? T_CLOSE_WAIT : T_ESTAB;
                } else if (c->snd_wnd == 0 && c->s_len) {
                    seg_send(c, TF_ACK, c->snd_una, 0, 1);        /* window probe */
                }
            }
            c->rto = c->rto * 2 > RTO_MAX ? RTO_MAX : c->rto * 2;
            c->rto_at = now + c->rto;
        }
        tcp_output(c);
    }
}

/* ---- the way in ---------------------------------------------------------------- */
void net_rx(netif_t *nif, const uint8_t *f, int len) {
    nif->rx_pkts++; nif->rx_bytes += (uint64_t)len;
    if (len < 14) return;
    uint16_t type = be16(f + 12);
    if (type == ETH_ARP) { arp_input(nif, f + 14, len - 14); return; }
    if (type != ETH_IP || len < 34) return;
    const uint8_t *ip = f + 14;
    if ((ip[0] >> 4) != 4) return;
    int ihl = (ip[0] & 15) * 4;
    int tot = be16(ip + 2);
    if (ihl < 20 || tot < ihl || tot > len - 14) return;
    if (csum_fold(csum_add(0, ip, ihl)) != 0) return;
    if (be16(ip + 6) & 0x3FFF) return;                 /* fragments: no     */
    uint32_t src = be32(ip + 12), dst = be32(ip + 16);
    int for_us = (nif->ip && dst == nif->ip) || dst == 0xFFFFFFFFu ||
                 (nif->ip && dst == (nif->ip | ~nif->mask)) || (dst & 0xF0000000u) == 0xE0000000u;
    if (!for_us) return;
    if (src && (src & 0xF0000000u) != 0xE0000000u) arp_learn(src, f + 6);
    const uint8_t *p = ip + ihl;
    int plen = tot - ihl;
    switch (ip[9]) {
    case PROTO_ICMP: if (dst == nif->ip) icmp_input(nif, f, src, p, plen); break;
    case PROTO_UDP:  udp_input(nif, f, src, dst, p, plen); break;
    case PROTO_TCP:  if (dst == nif->ip) tcp_input(nif, f, src, dst, p, plen); break;
    }
}

void net_init(void) { kprintf("[net] stack up: ARP, IPv4, ICMP, UDP, TCP\n"); }
void net_poll(void) { tcp_timers(); }
