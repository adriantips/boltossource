/* ===========================================================================
 *  BoltOS Mini  -  mini/lx_net.c
 *
 *  Sockets for a Linux program (lx.h), on Mini's own stack (net.c), IPv4:
 *    - UDP: a game's client and server, musl's DNS lookups;
 *    - TCP, connecting out: the game's HTTP client (the public server list,
 *      downloads from a server). No listening sockets; IPv6 says it isn't
 *      supported, and callers fall back.
 *
 *  Binding, connecting and sending are the boot core's (a request through
 *  the mailbox); what arrives is put in the socket's queue on the boot core
 *  -- by net.c's UDP handler, or by lx_net_poll draining a TCP connection
 *  every turn -- and taken out on the program's core: one ring per socket,
 *  each index written by one side.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "lxint.h"
#include "string.h"
#include "stdio.h"

#define NS   16
#define QN   64
#define PKT  1536
#define DATA (PKT - 8)

typedef struct { uint32_t ip; uint16_t port, len; uint8_t data[DATA]; } pkt_t;

#define SRX  (64 * 1024)                              /* a TCP socket's receive ring */
enum { T_NONE, T_CONNECTING, T_UP, T_FAILED };

typedef struct {
    int      used, bound, connected;
    int      stream, tstate, terr;                     /* TCP: T_*, the error SO_ERROR gives */
    tcb_t   *tcb;                                      /* the boot core's                    */
    uint8_t *rxb;                                      /* SRX                                */
    volatile int eof;                                  /* the peer closed and it's all here   */
    uint16_t lport, rport;
    uint32_t rip;                                     /* host order */
    int      rcvtimeo_ms, broadcast, nonblock;
    pkt_t   *q;                                       /* QN, in the windows (lx_kpages) */
    uint32_t drops;
    struct { volatile uint32_t head; } __attribute__((aligned(64))) w;   /* the boot core's  */
    struct { volatile uint32_t tail; } __attribute__((aligned(64))) r;   /* the program's    */
} sock_t;
static sock_t S[NS];

enum { NET_BIND = 1, NET_SEND, NET_CLOSE, NET_TCONNECT, NET_TSEND };
typedef struct { uint8_t op; uint8_t slot; uint16_t port; uint32_t ip; } netrq_t;

static uint16_t be16v(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32v(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put_sa(uint8_t *sa, uint32_t ip, uint16_t port) {
    memset(sa, 0, 16);
    sa[0] = 2;                                         /* AF_INET, little-endian u16 */
    sa[2] = (uint8_t)(port >> 8); sa[3] = (uint8_t)port;
    sa[4] = (uint8_t)(ip >> 24); sa[5] = (uint8_t)(ip >> 16); sa[6] = (uint8_t)(ip >> 8); sa[7] = (uint8_t)ip;
}

/* ---- the boot core ------------------------------------------------------------------------ */
static void rx(const udp_meta_t *m, const uint8_t *data, int len) {
    for (int i = 0; i < NS; i++) {
        sock_t *s = &S[i];
        if (!s->used || !s->bound || s->lport != m->dport || !s->q) continue;
        if (s->connected && (s->rip != m->src_ip || s->rport != m->sport)) return;
        uint32_t h = s->w.head;
        if (h - __atomic_load_n(&s->r.tail, __ATOMIC_ACQUIRE) >= QN) { s->drops++; return; }
        pkt_t *p = &s->q[h % QN];
        int n = len < DATA ? len : DATA;
        p->ip = m->src_ip; p->port = m->sport; p->len = (uint16_t)n;
        memcpy(p->data, data, (size_t)n);
        __atomic_store_n(&s->w.head, h + 1, __ATOMIC_RELEASE);
        __asm__ volatile("dsb ish\nsev" ::: "memory");
        return;
    }
}

void lx_net_serve(lx_rq_t *q) {
    netrq_t r;
    memcpy(&r, q->net, sizeof r);
    if (r.slot >= NS) { q->result = -L_EBADF; return; }
    sock_t *s = &S[r.slot];
    switch (r.op) {
    case NET_BIND: {
        uint16_t port = r.port;
        if (!port) {
            static uint16_t next = 49152;
            for (int tries = 0; tries < 4000 && !port; tries++) {
                uint16_t c = next++;
                if (next >= 61000) next = 49152;
                if (!udp_bound(c)) port = c;
            }
        }
        if (!port || udp_bound(port)) { q->result = -L_EADDRINUSE; return; }
        if (udp_bind(port, rx)) { q->result = -L_EADDRINUSE; return; }
        s->lport = port; s->bound = 1;
        q->result = port;
        return;
    }
    case NET_SEND: {
        netif_t *nif = r.ip == 0xFFFFFFFFu ? net_if(0) : net_route(r.ip);
        if (r.ip == 0xFFFFFFFFu) for (int i = 0; net_if(i); i++) if (net_if(i)->up && net_if(i)->ip && strcmp(net_if(i)->name, "usb0")) nif = net_if(i);
        if (!nif || !nif->up || !nif->ip) { q->result = -L_ENETUNREACH; return; }
        int rr = udp_send(nif, nif->ip, r.ip, 0, s->lport, r.port, q->buf, (int)q->len);
        q->result = rr < 0 ? (int)q->len : (int)q->len;   /* a datagram waiting for ARP is lost, as UDP may */
        return;
    }
    case NET_CLOSE:
        if (s->stream) { if (s->tcb) tcp_close(s->tcb); s->tcb = 0; }
        else if (s->bound) udp_unbind(s->lport);
        s->bound = 0;
        q->result = 0;
        return;
    case NET_TCONNECT:
        s->tcb = tcp_connect(r.ip, r.port);
        if (!s->tcb) { s->tstate = T_FAILED; s->terr = L_ENETUNREACH; q->result = -L_ENETUNREACH; return; }
        s->rip = r.ip; s->rport = r.port;
        __atomic_store_n(&s->tstate, T_CONNECTING, __ATOMIC_RELEASE);
        q->result = 0;
        return;
    case NET_TSEND: {
        if (!s->tcb || s->tstate != T_UP || !tcp_open(s->tcb)) { q->result = -32; return; }   /* EPIPE */
        int n = tcp_write(s->tcb, q->buf, (int)q->len);
        q->result = n > 0 ? n : -L_EAGAIN;
        return;
    }
    }
    q->result = -L_EINVAL;
}

/* Every turn: TCP connections come up or fail, and what they received goes
 * into their rings. */
void lx_net_poll(void) {
    for (int i = 0; i < NS; i++) {
        sock_t *s = &S[i];
        if (!s->used || !s->stream || !s->tcb) continue;
        int st = __atomic_load_n(&s->tstate, __ATOMIC_ACQUIRE);
        if (st == T_CONNECTING && !tcp_connecting(s->tcb)) {
            if (tcp_open(s->tcb)) st = T_UP;
            else { s->terr = L_ECONNREFUSED; st = T_FAILED; }
            __atomic_store_n(&s->tstate, st, __ATOMIC_RELEASE);
            __asm__ volatile("dsb ish\nsev" ::: "memory");
        }
        if (st != T_UP || !s->rxb) continue;
        uint32_t h = s->w.head, room = SRX - (h - __atomic_load_n(&s->r.tail, __ATOMIC_ACQUIRE));
        int moved = 0;
        while (room && tcp_avail(s->tcb) > 0) {
            uint8_t b[1460];
            uint32_t want = room < sizeof b ? room : sizeof b;
            uint32_t at = h % SRX, first = SRX - at < want ? SRX - at : want;
            int n = tcp_read(s->tcb, b, (int)want);
            if (n <= 0) break;
            uint32_t k1 = (uint32_t)n < first ? (uint32_t)n : first;
            memcpy(s->rxb + at, b, k1);
            if ((uint32_t)n > k1) memcpy(s->rxb, b + k1, (size_t)n - k1);
            h += (uint32_t)n; room -= (uint32_t)n; moved = 1;
        }
        if (moved) __atomic_store_n(&s->w.head, h, __ATOMIC_RELEASE);
        if (!s->eof && tcp_peer_closed(s->tcb) && !tcp_avail(s->tcb)) s->eof = 1;
        if (moved || s->eof) __asm__ volatile("dsb ish\nsev" ::: "memory");
    }
}

void lx_net_reset(void) {
    for (int i = 0; i < NS; i++) {
        if (!S[i].used) continue;
        if (S[i].stream && S[i].tcb) tcp_abort(S[i].tcb);
        else if (S[i].bound) udp_unbind(S[i].lport);
    }
    memset(S, 0, sizeof S);
}

/* ---- the program's core ---------------------------------------------------------------------- */
static int ask(uint8_t op, int slot, uint16_t port, uint32_t ip, const void *buf, uint32_t len) {
    lx_rq_t rq;
    memset(&rq, 0, sizeof rq);
    rq.kind = RQ_NET;
    netrq_t r = { op, (uint8_t)slot, port, ip };
    memcpy(rq.net, &r, sizeof r);
    rq.buf = (void *)buf; rq.len = len;
    return lx_request(&rq);
}

static sock_t *get(int s) { return s >= 0 && s < NS && S[s].used ? &S[s] : 0; }

int lxnet_socket(int domain, int type, int proto) {
    (void)proto;
    if (domain != 2) return -L_EAFNOSUPPORT;
    int t = type & 0xF;
    if (t != 1 && t != 2) return -L_EPROTONOSUPPORT;     /* SOCK_STREAM, SOCK_DGRAM */
    for (int i = 0; i < NS; i++) {
        if (S[i].used) continue;
        sock_t *s = &S[i];
        pkt_t *q = s->q;                                   /* a slot's rings stay for the run */
        uint8_t *rxb = s->rxb;
        memset(s, 0, sizeof *s);
        s->stream = t == 1;
        if (s->stream) { s->rxb = rxb ? rxb : lx_kpages(SRX); if (!s->rxb) return -L_ENOMEM; s->q = q; }
        else { s->q = q ? q : lx_kpages(sizeof(pkt_t) * QN); if (!s->q) return -L_ENOMEM; s->rxb = rxb; }
        s->used = 1;
        return i;
    }
    return -L_EMFILE;
}

int lxnet_close(int i) {
    sock_t *s = get(i);
    if (!s) return -L_EBADF;
    if (s->bound || (s->stream && s->tcb)) ask(NET_CLOSE, i, 0, 0, 0, 0);
    s->used = 0; s->bound = 0;
    return 0;
}

int lxnet_bind(int i, const uint8_t *sa, int len) {
    sock_t *s = get(i);
    if (!s) return -L_EBADF;
    if (len < 8 || sa[0] != 2) return -L_EAFNOSUPPORT;
    if (s->bound) return -L_EINVAL;
    int r = ask(NET_BIND, i, be16v(sa + 2), 0, 0, 0);
    return r < 0 ? r : 0;
}

static int tstate(sock_t *s) { return __atomic_load_n(&s->tstate, __ATOMIC_ACQUIRE); }

int lxnet_connect(int i, const uint8_t *sa, int len) {
    sock_t *s = get(i);
    if (!s) return -L_EBADF;
    if (len < 8) return -L_EINVAL;
    if (s->stream) {
        if (sa[0] != 2) return -L_EAFNOSUPPORT;
        int st = tstate(s);
        if (st == T_UP) return -106;                       /* EISCONN */
        if (st == T_CONNECTING) return -114;               /* EALREADY */
        int r = ask(NET_TCONNECT, i, be16v(sa + 2), be32v(sa + 4), 0, 0);
        if (r < 0) return r;
        if (s->nonblock) return -115;                      /* EINPROGRESS */
        while (tstate(s) == T_CONNECTING) { if (lx_stopping()) return -L_EINTR; __asm__ volatile("wfe"); }
        return tstate(s) == T_UP ? 0 : -s->terr;
    }
    if (sa[0] == 0 && sa[1] == 0) { s->connected = 0; return 0; }   /* AF_UNSPEC: forget the peer */
    if (sa[0] != 2) return -L_EAFNOSUPPORT;
    if (!s->bound) { int r = ask(NET_BIND, i, 0, 0, 0, 0); if (r < 0) return r; }
    s->rip = be32v(sa + 4); s->rport = be16v(sa + 2); s->connected = 1;
    return 0;
}

int64_t lxnet_sendto(int i, const void *buf, uint64_t n, const uint8_t *sa, int salen, int nonblock) {
    sock_t *s = get(i);
    if (!s) return -L_EBADF;
    if (s->stream) {
        if (tstate(s) == T_CONNECTING) return -L_EAGAIN;
        if (tstate(s) != T_UP) return -107;                /* ENOTCONN */
        uint64_t done = 0;
        while (done < n) {
            uint32_t k = n - done > 16384 ? 16384 : (uint32_t)(n - done);
            int r = ask(NET_TSEND, i, 0, 0, (const uint8_t *)buf + done, k);
            if (r > 0) { done += (uint64_t)r; continue; }
            if (r != -L_EAGAIN) return done ? (int64_t)done : r;
            if (nonblock) return done ? (int64_t)done : -L_EAGAIN;
            if (lx_stopping()) return done ? (int64_t)done : -L_EINTR;
            lx_wait_until(lx_now_ns() + 1000000);
        }
        return (int64_t)done;
    }
    uint32_t ip; uint16_t port;
    if (sa && salen >= 8) {
        if (sa[0] != 2) return -L_EAFNOSUPPORT;
        ip = be32v(sa + 4); port = be16v(sa + 2);
    } else if (s->connected) { ip = s->rip; port = s->rport; }
    else return -L_EDESTADDRREQ;
    if (n > 1472) return -L_EMSGSIZE;
    if (!s->bound) { int r = ask(NET_BIND, i, 0, 0, 0, 0); if (r < 0) return r; }
    if ((ip >> 24) == 127) return (int64_t)n;              /* nobody listens on Mini's loopback */
    return ask(NET_SEND, i, port, ip, buf, (uint32_t)n);
}

int64_t lxnet_recvfrom(int i, void *buf, uint64_t n, uint8_t *sa, uint32_t *salen, int nonblock, int peek) {
    sock_t *s = get(i);
    if (!s) return -L_EBADF;
    if (s->stream) {
        if (sa && salen) *salen = 0;
        for (;;) {
            uint32_t t = s->r.tail, h = __atomic_load_n(&s->w.head, __ATOMIC_ACQUIRE);
            if (h != t) {
                uint64_t k = h - t < n ? h - t : n;
                for (uint64_t j = 0; j < k; j++) ((uint8_t *)buf)[j] = s->rxb[(t + j) % SRX];
                if (!peek) __atomic_store_n(&s->r.tail, t + (uint32_t)k, __ATOMIC_RELEASE);
                return (int64_t)k;
            }
            if (s->eof || tstate(s) == T_FAILED) return 0;
            if (tstate(s) == T_NONE) return -107;          /* ENOTCONN */
            if (nonblock) return -L_EAGAIN;
            if (lx_stopping()) return -L_EINTR;
            __asm__ volatile("wfe");
        }
    }
    uint64_t end = s->rcvtimeo_ms ? lx_now_ns() + (uint64_t)s->rcvtimeo_ms * 1000000ull : 0;
    for (;;) {
        uint32_t t = s->r.tail;
        if (t != __atomic_load_n(&s->w.head, __ATOMIC_ACQUIRE)) {
            pkt_t *p = &s->q[t % QN];
            uint64_t k = p->len < n ? p->len : n;
            memcpy(buf, p->data, k);
            if (sa && salen) { uint8_t tmp[16]; put_sa(tmp, p->ip, p->port); uint32_t c = *salen < 16 ? *salen : 16; memcpy(sa, tmp, c); *salen = 16; }
            if (!peek) __atomic_store_n(&s->r.tail, t + 1, __ATOMIC_RELEASE);
            return (int64_t)k;
        }
        if (nonblock) return -L_EAGAIN;
        if (lx_stopping()) return -L_EINTR;
        if (end && lx_now_ns() >= end) return -L_EAGAIN;
        __asm__ volatile("wfe");
    }
}

int lxnet_getsockname(int i, uint8_t *sa, uint32_t *len) {
    sock_t *s = get(i);
    if (!s) return -L_EBADF;
    uint8_t tmp[16];
    put_sa(tmp, 0, s->lport);
    uint32_t c = *len < 16 ? *len : 16;
    memcpy(sa, tmp, c); *len = 16;
    return 0;
}

int lxnet_getpeername(int i, uint8_t *sa, uint32_t *len) {
    sock_t *s = get(i);
    if (!s) return -L_EBADF;
    if (!s->connected && !(s->stream && tstate(s) == T_UP)) return -107;   /* ENOTCONN */
    uint8_t tmp[16];
    put_sa(tmp, s->rip, s->rport);
    uint32_t c = *len < 16 ? *len : 16;
    memcpy(sa, tmp, c); *len = 16;
    return 0;
}

int lxnet_setsockopt(int i, int level, int opt, const void *v, int len) {
    sock_t *s = get(i);
    if (!s) return -L_EBADF;
    if (level == 1 && opt == 20 && len >= 16) {             /* SO_RCVTIMEO */
        int64_t tv[2]; memcpy(tv, v, 16);
        s->rcvtimeo_ms = (int)(tv[0] * 1000 + tv[1] / 1000);
    }
    if (level == 1 && opt == 6 && len >= 4) { int b; memcpy(&b, v, 4); s->broadcast = b; }
    return 0;                                               /* the rest: yes */
}

int lxnet_getsockopt(int i, int level, int opt, void *v, uint32_t *len) {
    sock_t *s = get(i);
    if (!s) return -L_EBADF;
    int val = 0;
    if (level == 1 && opt == 3) val = s->stream ? 1 : 2;    /* SO_TYPE */
    if (level == 1 && opt == 4) { val = tstate(s) == T_FAILED ? s->terr : 0; s->terr = tstate(s) == T_FAILED ? s->terr : 0; }   /* SO_ERROR */
    if (level == 1 && (opt == 7 || opt == 8)) val = QN * DATA;   /* SO_SNDBUF / SO_RCVBUF */
    uint32_t c = *len < 4 ? *len : 4;
    memcpy(v, &val, c); *len = 4;
    return 0;
}

int lxnet_ready(int i) {
    sock_t *s = get(i);
    if (!s) return 0;
    if (s->stream && (s->eof || tstate(s) == T_FAILED)) return 1;
    return s->r.tail != __atomic_load_n(&s->w.head, __ATOMIC_ACQUIRE);
}

/* select/poll's "writable": a connection that's up (or failed: SO_ERROR says how). */
int lxnet_writable(int i) {
    sock_t *s = get(i);
    if (!s) return 0;
    return !s->stream || tstate(s) != T_CONNECTING;
}

void lxnet_set_nonblock(int i, int on) { sock_t *s = get(i); if (s) s->nonblock = on; }
