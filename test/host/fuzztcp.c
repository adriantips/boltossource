/* net/tcp.c, with the segments coming from somewhere that does not mean well.
 *
 * A TCP segment is the first thing on the machine that a stranger gets to
 * choose: it arrives before any port is open, before the firewall has a
 * connection to match against, and -- since item 40 added tcp_listen -- before
 * anyone has authenticated to anything. And unlike the v6 path, tcp_input()
 * does not verify a checksum, because the IP layer under it is expected to
 * have; so every byte of a v4 segment reaches the state machine exactly as
 * sent.
 *
 * This is the harness net/tls.c and net/http.c could not share: they need a
 * fake transport, and this needs the real one. What is substituted here is the
 * layer *below* tcp.c -- ip_output keeps what would have gone on the wire, and
 * netif_poll_all hands up the next segment out of the fuzz input, which is
 * exactly the shape of the real thing and is why the blocking client paths run
 * here at all.
 *
 * The other thing that makes them run is the `hlt` handler in main(): tcp.c
 * waits by halting the processor, which is a ring-0 instruction and a fault in
 * a host process. See fz_step_over_hlt.
 *
 * Build and run through test/t_fuzz.py; --list names the targets. */
#include <stdint.h>

#include "fuzz.h"        /* first: it defines the heap everything else uses */

#include "net.h"
#include "net6.h"
#include "netaddr.h"
#include "netif.h"

/* ------------------------------------------------------- the substitutes --*/

static uint64_t kmsgs;
void kputc(char c) { (void)c; }
void kprintf(const char *fmt, ...) { (void)fmt; kmsgs++; }
void panic(const char *msg) {
    fprintf(stderr, "panic(\"%s\") from the TCP stack -- that is the bug\n", msg);
    abort();
}

/* Every wait in tcp.c is `while (pit_ticks() - start < ms)`, so the clock has
 * to move or a timeout is a hang. It moves fast: a run should spend its time
 * in the state machine, not counting. */
static uint64_t fake_ticks;
uint64_t pit_ticks(void) { fake_ticks += 8; return fake_ticks; }

/* ---- the link layer ----------------------------------------------------- */
static struct netif the_nif;
struct netif *netif_default(void) { return &the_nif; }

static uint64_t tx_segments, tx_bytes;
static uint8_t  last_tx[2048];
static uint16_t last_tx_len;

static void keep_tx(const void *p, uint16_t len) {
    tx_segments++;
    tx_bytes += len;
    last_tx_len = len > sizeof last_tx ? (uint16_t)sizeof last_tx : len;
    memcpy(last_tx, p, last_tx_len);
}

int ip_output(struct netif *nif, uint32_t dst_ip, uint8_t proto, const void *payload, uint16_t len) {
    (void)nif; (void)dst_ip; (void)proto;
    keep_tx(payload, len);
    return 0;
}
int ip6_output_from(struct netif *nif, const ip6_addr *src, const ip6_addr *dst,
                    uint8_t next_hdr, const void *payload, uint16_t len, uint8_t hop_limit) {
    (void)nif; (void)src; (void)dst; (void)next_hdr; (void)hop_limit;
    keep_tx(payload, len);
    return 0;
}

/* ---- the segment source ------------------------------------------------- */
/* The fuzz input is a stream of segments rather than one, because that is what
 * a connection is: the framing comes out of the data, so a mutation can move a
 * boundary as well as change a byte, and a replay reproduces both. */
static const uint8_t *feed;
static uint32_t feed_len, feed_pos, feed_left;
static int feed_v6;

static const ip6_addr PEER6 = {{ 0xfe,0x80,0,0,0,0,0,0, 0x52,0x54,0,0xff,0xfe,0xaa,0xbb,0xcc }};
static const ip6_addr SELF6 = {{ 0xfe,0x80,0,0,0,0,0,0, 0x52,0x54,0,0xff,0xfe,0x11,0x22,0x33 }};
#define PEER4 0x0A000202u

/* The one-complement sum a v6 pseudo-header needs. Written here rather than
 * linked from net/ip6.c on purpose: this harness *fixes up* the checksum after
 * mutating a segment, because tcp_input6 verifies it and would otherwise drop
 * every input the mutator produced -- a v6 target that only ever tested the
 * checksum would be worse than none. net/ip6.c's own version is checked by
 * test/host/ip6.c. */
static uint16_t v6_csum(const ip6_addr *s, const ip6_addr *d, uint8_t proto,
                        const uint8_t *p, uint32_t len) {
    uint32_t sum = 0;
    for (int i = 0; i < 16; i += 2) sum += ((uint32_t)s->b[i] << 8) | s->b[i+1];
    for (int i = 0; i < 16; i += 2) sum += ((uint32_t)d->b[i] << 8) | d->b[i+1];
    sum += len >> 16; sum += len & 0xFFFF;
    sum += proto;
    for (uint32_t i = 0; i + 1 < len; i += 2) sum += ((uint32_t)p[i] << 8) | p[i+1];
    if (len & 1) sum += (uint32_t)p[len-1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t ip6_checksum(const ip6_addr *s, const ip6_addr *d, uint8_t next_hdr,
                      const void *payload, uint32_t len) {
    return v6_csum(s, d, next_hdr, (const uint8_t *)payload, len);
}
const ip6_addr *ip6_source_for(const ip6_addr *dst) { (void)dst; return &SELF6; }
int ip6_nexthop(const ip6_addr *dst, ip6_addr *out) {
    memcpy(out, dst, sizeof *out);
    return 1;
}

/* net/netaddr.c is linked in whole, so the handful of things it reaches for
 * come from here. None of them is under test: what a v6 address prints as has
 * test/host/ip6.c, and the routing table is not something a segment gets to
 * choose. */
int ip6_equal(const ip6_addr *a, const ip6_addr *b) { return !memcmp(a, b, 16); }
void ip6_copy(ip6_addr *d, const ip6_addr *s) { memcpy(d, s, 16); }
int ip6_is_zero(const ip6_addr *a) {
    for (int i = 0; i < 16; i++) if (a->b[i]) return 0;
    return 1;
}
int ip6_parse(const char *s, ip6_addr *out) { (void)s; (void)out; return 0; }
void ip6_format(const ip6_addr *a, char *out) { (void)a; out[0] = 0; }
uint32_t net_parse_ipv4(const char *s, int *ok) { (void)s; if (ok) *ok = 0; return 0; }
uint32_t net_ip = 0x0A000210u, net_mask = 0xFFFFFF00u, net_gw = 0x0A000202u, net_dns;
int arp_resolve(struct netif *nif, uint32_t ip, uint8_t mac[6], uint32_t to) {
    (void)nif; (void)ip; (void)to; memset(mac, 0x52, 6); return 1;
}
int nd6_resolve(struct netif *nif, const ip6_addr *a, uint8_t mac[6], uint32_t to) {
    (void)nif; (void)a; (void)to; memset(mac, 0x52, 6); return 1;
}

static uint8_t segbuf[2048];

/* Hand up the next segment, the way a NIC's poll would. Returns 0 when the
 * input is spent, which is how the blocking paths in tcp.c reach their
 * timeouts instead of spinning forever. */
static int deliver_one(void) {
    if (feed_pos >= feed_len || feed_left == 0) return 0;
    feed_left--;
    uint32_t rest = feed_len - feed_pos;
    /* At least a header's worth where there is one to be had, and a length the
     * data chooses, so short and long segments both happen. */
    uint32_t n = 20 + (uint32_t)feed[feed_pos] % 200;
    if (n > rest) n = rest;
    if (n > sizeof segbuf) n = sizeof segbuf;
    memcpy(segbuf, feed + feed_pos, n);
    feed_pos += n;

    /* Half the segments get both ports put back to the pair the harness
     * listens on. The point is not the first segment -- tcp_input_at() matches
     * a connection on *both* ports and the peer address, so without this a
     * follow-up segment almost never lands on the connection the one before it
     * opened, and the state machine is only ever entered at SYN. Which half is
     * decided by a bit of the data, so a replay reproduces it. */
    if (n >= 20 && (segbuf[5] & 1)) {
        segbuf[0] = 0x9C; segbuf[1] = 0x40;      /* from 40000 */
        segbuf[2] = 0;    segbuf[3] = 80;        /* to the listening port */
    }

    if (feed_v6) {
        if (n >= 18) {                       /* zero the field, then fill it in */
            segbuf[16] = segbuf[17] = 0;
            uint16_t ck = v6_csum(&PEER6, &SELF6, IPPROTO_TCP, segbuf, n);
            segbuf[16] = (uint8_t)(ck >> 8); segbuf[17] = (uint8_t)ck;
        }
        tcp_input6(&the_nif, &PEER6, &SELF6, segbuf, (uint16_t)n);
    } else {
        tcp_input(&the_nif, PEER4, segbuf, (uint16_t)n);
    }
    return 1;
}

void netif_poll_all(void) { deliver_one(); }

/* ------------------------------------------------------------ the corpus --*/
/* Built at startup rather than written out as escaped bytes: a TCP header is
 * five 32-bit words and spelling it in hex in a string literal is how an
 * off-by-one in a corpus goes unnoticed for a year. */
#define SEG_MAX 16
static uint8_t  seed_bytes[SEG_MAX][256];
static fz_seed  tcp_corpus[SEG_MAX];
static int      n_seeds;

static uint32_t put_seg(uint8_t *b, uint16_t sport, uint16_t dport,
                        uint32_t seq, uint32_t ack, uint8_t flags,
                        uint16_t win, const char *data, uint32_t dlen) {
    uint32_t p = 0;
    b[p++] = (uint8_t)(sport >> 8); b[p++] = (uint8_t)sport;
    b[p++] = (uint8_t)(dport >> 8); b[p++] = (uint8_t)dport;
    b[p++] = (uint8_t)(seq >> 24); b[p++] = (uint8_t)(seq >> 16);
    b[p++] = (uint8_t)(seq >> 8);  b[p++] = (uint8_t)seq;
    b[p++] = (uint8_t)(ack >> 24); b[p++] = (uint8_t)(ack >> 16);
    b[p++] = (uint8_t)(ack >> 8);  b[p++] = (uint8_t)ack;
    b[p++] = 5 << 4;                       /* data offset: 5 words, no options */
    b[p++] = flags;
    b[p++] = (uint8_t)(win >> 8); b[p++] = (uint8_t)win;
    b[p++] = 0; b[p++] = 0;                /* checksum: v4 does not check it   */
    b[p++] = 0; b[p++] = 0;                /* urgent pointer                    */
    for (uint32_t i = 0; i < dlen; i++) b[p++] = (uint8_t)data[i];
    return p;
}

static void add_seed(uint32_t n) { tcp_corpus[n_seeds].d = seed_bytes[n_seeds];
                                   tcp_corpus[n_seeds].n = n; n_seeds++; }

static void build_corpus(void) {
    uint8_t *b;
    b = seed_bytes[n_seeds]; add_seed(put_seg(b, 40000, 80, 1000, 0, TCP_SYN, 64240, 0, 0));
    b = seed_bytes[n_seeds]; add_seed(put_seg(b, 40000, 80, 1001, 5000, TCP_ACK, 64240, 0, 0));
    b = seed_bytes[n_seeds]; add_seed(put_seg(b, 40000, 80, 1001, 5000, TCP_PSH | TCP_ACK, 64240,
                                              "GET / HTTP/1.0\r\n\r\n", 18));
    b = seed_bytes[n_seeds]; add_seed(put_seg(b, 40000, 80, 1019, 5000, TCP_FIN | TCP_ACK, 64240, 0, 0));
    b = seed_bytes[n_seeds]; add_seed(put_seg(b, 40000, 80, 1019, 5000, TCP_RST, 0, 0, 0));
    b = seed_bytes[n_seeds]; add_seed(put_seg(b, 40000, 80, 1000, 5000, TCP_SYN | TCP_ACK, 64240, 0, 0));

    /* A handshake and a request as one stream, which is what the framing above
     * turns into three segments -- the only corpus entry that gets a
     * connection to ESTABLISHED without the mutator's help. */
    b = seed_bytes[n_seeds];
    uint32_t p = put_seg(b, 40001, 80, 7000, 0, TCP_SYN, 64240, 0, 0);
    p += put_seg(b + p, 40001, 80, 7001, 1, TCP_ACK, 64240, 0, 0);
    p += put_seg(b + p, 40001, 80, 7001, 1, TCP_PSH | TCP_ACK, 64240, "hello", 5);
    add_seed(p);

    /* And the shapes with nothing valid in them at all. */
    b = seed_bytes[n_seeds]; memset(b, 0, 20);   add_seed(20);
    b = seed_bytes[n_seeds]; memset(b, 0xFF, 20); add_seed(20);
    b = seed_bytes[n_seeds]; add_seed(4);                        /* shorter than a header */
    b = seed_bytes[n_seeds]; add_seed(0);
}

/* ------------------------------------------------------------- targets ----*/

/* Anything left over from the last input, gone. tcp_close() is the only thing
 * that frees a connection, and it will not pump when the connection is not
 * established -- which after an RST it is not. */
static void reap(void) {
    for (;;) {
        netaddr peer; uint16_t pport = 0, lport = 0; const char *st = 0;
        if (!tcp_conn_dump(0, &peer, &pport, &lport, &st)) break;
        struct tcp_conn *c = tcp_accept(80, 0);
        if (!c) break;
        tcp_close(c);
    }
}

static void t_listen(const uint8_t *d, uint32_t n) {
    feed = d; feed_len = n; feed_pos = 0; feed_left = 64; feed_v6 = 0;
    tcp_listen(80);
    while (deliver_one()) { }
    reap();
    tcp_unlisten(80);
}

static void t_listen6(const uint8_t *d, uint32_t n) {
    feed = d; feed_len = n; feed_pos = 0; feed_left = 64; feed_v6 = 1;
    tcp_listen(80);
    while (deliver_one()) { }
    reap();
    tcp_unlisten(80);
}

/* The client half: tcp_connect and the receive path, answered by whatever the
 * input says. Everything it waits on is netif_poll_all(), which is the
 * function above, so the handshake is driven by the fuzz data. */
static void t_connect(const uint8_t *d, uint32_t n) {
    feed = d; feed_len = n; feed_pos = 0; feed_left = 64; feed_v6 = 0;
    struct tcp_conn *c = tcp_connect(PEER4, 80, 200);
    if (!c) return;
    char buf[2048];
    tcp_send(c, "GET / HTTP/1.0\r\n\r\n", 18);
    for (int i = 0; i < 4; i++) if (tcp_recv(c, buf, sizeof buf, 50) <= 0) break;
    tcp_is_open(c);
    tcp_peer_port(c);
    tcp_close(c);
}

/* ---------------------------------------------------------------- main ----*/

int fz_selftest(void) {
    fz_check_failures = 0;

    /* A SYN on a listening port has to produce a SYN/ACK on the wire, or
     * nothing below tcp_input() is connected to anything. */
    tcp_listen(80);
    tx_segments = 0;
    feed = tcp_corpus[0].d; feed_len = tcp_corpus[0].n; feed_pos = 0; feed_left = 4; feed_v6 = 0;
    FZ_CHECK(deliver_one() == 1);
    FZ_CHECK(tx_segments == 1);
    FZ_CHECK(last_tx_len >= 20);
    FZ_CHECK((last_tx[13] & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK));
    FZ_CHECK(((uint16_t)last_tx[2] << 8 | last_tx[3]) == 40000);   /* back to its port */
    reap();
    tcp_unlisten(80);

    /* And on a port nobody is listening on, nothing at all. RFC 793 says send
     * a reset; tcp_input_at() drops instead, which is a deliberate choice and
     * asserted here so a change to it is a test failure rather than a
     * surprise. */
    tx_segments = 0;
    feed = tcp_corpus[0].d; feed_len = tcp_corpus[0].n; feed_pos = 0; feed_left = 4;
    deliver_one();
    FZ_CHECK(tx_segments == 0);

    /* The v6 path drops anything whose checksum is wrong, so the fixup in
     * deliver_one() has to actually work or that target tests one branch. */
    tcp_listen(80);
    tx_segments = 0;
    feed = tcp_corpus[0].d; feed_len = tcp_corpus[0].n; feed_pos = 0; feed_left = 4; feed_v6 = 1;
    deliver_one();
    FZ_CHECK(tx_segments == 1);
    reap();
    tcp_unlisten(80);
    feed_v6 = 0;

    /* Sixteen half-open connections used to be the end of the machine's
     * ability to accept anything, on any port, until it rebooted -- there is
     * one table and nothing aged out of it. Sixteen SYNs from sixteen ports,
     * then a seventeenth that has to be ignored, then the same seventeenth
     * after the clock has moved past the stale threshold, which has to be
     * answered. */
    tcp_listen(80);
    uint8_t syn[64];
    for (int i = 0; i < 16; i++) {
        uint32_t n = put_seg(syn, (uint16_t)(50000 + i), 80, 1000 + i, 0, TCP_SYN, 64240, 0, 0);
        tcp_input(&the_nif, PEER4, syn, (uint16_t)n);
    }
    tx_segments = 0;
    uint32_t sn = put_seg(syn, 50099, 80, 2000, 0, TCP_SYN, 64240, 0, 0);
    tcp_input(&the_nif, PEER4, syn, (uint16_t)sn);
    FZ_CHECK(tx_segments == 0);                 /* table full, SYN dropped */

    fake_ticks += 60000;                        /* past STALE_MS in net/tcp.c */
    tx_segments = 0;
    tcp_input(&the_nif, PEER4, syn, (uint16_t)sn);
    FZ_CHECK(tx_segments == 1);                 /* a slot was taken back */
    FZ_CHECK((last_tx[13] & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK));
    tcp_unlisten(80);

    return fz_check_failures ? -1 : 0;
}

/* tcp.c waits by halting the processor. That is correct in ring 0 and a
 * privileged-instruction fault in a host process, so the choice was to fuzz
 * only the input path or to step over the instruction. `hlt` is one byte, the
 * fault reports the address, and a vectored handler can move RIP past it and
 * carry on -- which turns every wait in tcp.c into a spin and makes the whole
 * file, client paths included, runnable here. */
#ifdef _WIN32
#define CTX_RIP 0xF8                       /* Rip's offset in the x86-64 CONTEXT */
static uint64_t hlts_stepped;
static long __stdcall fz_step_over_hlt(fz_exc_ptrs *ep) {
    if (ep->rec->code == 0xC0000096u) {     /* EXCEPTION_PRIV_INSTRUCTION */
        uint64_t rip;
        memcpy(&rip, (unsigned char *)ep->ctx + CTX_RIP, sizeof rip);
        if (rip == (uint64_t)ep->rec->address && *(const unsigned char *)rip == 0xF4) {
            rip++;
            memcpy((unsigned char *)ep->ctx + CTX_RIP, &rip, sizeof rip);
            hlts_stepped++;
            return -1;                      /* EXCEPTION_CONTINUE_EXECUTION */
        }
    }
    return 0;                               /* EXCEPTION_CONTINUE_SEARCH */
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    AddVectoredExceptionHandler(1, fz_step_over_hlt);
#endif
    memset(&the_nif, 0, sizeof the_nif);
    build_corpus();

    static fz_target targets[3];
    targets[0] = (fz_target){ "listen",  t_listen,  tcp_corpus, 0, 0 };
    targets[1] = (fz_target){ "listen6", t_listen6, tcp_corpus, 0, 0 };
    targets[2] = (fz_target){ "connect", t_connect, tcp_corpus, 0, 0 };
    for (int i = 0; i < 3; i++) targets[i].ncorpus = n_seeds;

    int rc = fz_main(argc, argv, targets, 3);
    if (kmsgs > (1ull << 40)) fprintf(stderr, "%llu\n", (unsigned long long)kmsgs);
    return rc;
}
