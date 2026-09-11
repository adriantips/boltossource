/* The IPv6 address text forms, the checksum, and UDP over IPv6, against the
 * real net/ip6.c, net/netaddr.c and net/udp.c.
 *
 * These are pure functions over sixteen bytes and one datagram buffer, so they
 * do not need a machine to run -- and they are exactly the kind of code where
 * a case that never comes up in QEMU is wrong for years. "::" printing as
 * ":::" and "fe80:::1" parsing as fe80::1 were both found here rather than on
 * the wire.
 *
 * UDP is here for a blunter reason: nothing on the machine sends a v6 datagram
 * yet. TCP over v6 has test/t_tcp6.py to prove it on a wire between two real
 * machines, but the resolver -- the only thing that speaks UDP -- still asks a
 * v4 server, because nothing parses the RDNSS option out of a Router
 * Advertisement. So the checksum this builds and the one it verifies are
 * checked here, byte for byte, rather than shipped unrun. */
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
/* Not <string.h>: the kernel's own declares the same functions without the
   dllimport the host header puts on them, and including both is an error.
   net/ip6.c brings in include/string.h, and the host libc supplies the code. */

#include "net.h"
#include "net6.h"
#include "string.h"   /* the kernel one, as above */

/* The parts of the stack an address does not need. The link layer is a
 * capture buffer instead: what ip6_output_from hands to eth_send is exactly
 * what would have gone on the wire, so it can be taken apart here. */
struct netif;
static uint8_t  tx_pkt[2048];
static uint16_t tx_len, tx_type;
static uint8_t  tx_mac[6];
static int      tx_count;

int  eth_send(struct netif *nif, const uint8_t dst[6], uint16_t ethertype,
              const void *payload, uint16_t len) {
    (void)nif;
    if (len > sizeof tx_pkt) return -1;
    memcpy(tx_mac, dst, 6);
    memcpy(tx_pkt, payload, len);
    tx_len  = len;
    tx_type = ethertype;
    tx_count++;
    return 0;
}
/* Every neighbour is known, so nothing under test ever loses a datagram to a
 * cache miss. The real lookup has the machine to prove it. */
int  nd6_lookup(const ip6_addr *a, uint8_t mac_out[6]) {
    (void)a;
    static const uint8_t peer[6] = { 0x52,0x54,0x00,0xaa,0xbb,0xcc };
    memcpy(mac_out, peer, 6);
    return 1;
}
void nd6_solicit(struct netif *nif, const ip6_addr *t) { (void)nif; (void)t; }
void icmp6_input(struct netif *nif, const ip6_addr *s, const ip6_addr *d,
                 const uint8_t *p, uint16_t l) {
    (void)nif; (void)s; (void)d; (void)p; (void)l;
}
static int tcp6_seen;
void tcp_input6(struct netif *nif, const ip6_addr *s, const ip6_addr *d,
                const uint8_t *p, uint16_t l) {
    (void)nif; (void)s; (void)d; (void)p; (void)l; tcp6_seen++;
}
/* net/tcp.c cannot run here: every wait in it is a `hlt`, which a host process
 * is not allowed to execute. t_tcp6.py drives it on a machine instead. */
int  ip_output(struct netif *nif, uint32_t dst_ip, uint8_t proto,
               const void *payload, uint16_t len) {
    (void)nif; (void)dst_ip; (void)proto; (void)payload; (void)len; return -1;
}
int  fw_check6(int dir, uint8_t proto, uint16_t port) {
    (void)dir; (void)proto; (void)port; return 1;
}
/* net_parse_ipv4 lives in net/eth.c next to eth_send, and pulling that in
 * would drag the whole link layer along. What is under test here is which
 * branch netaddr_parse takes, not the dotted-quad scanner -- every ifconfig
 * on the machine exercises that one. */
uint32_t net_parse_ipv4(const char *s, int *ok) {
    uint32_t b[4] = {0,0,0,0};
    int part = 0, digits = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') { b[part] = b[part] * 10 + (uint32_t)(*s - '0'); digits++; }
        else if (*s == '.' || *s == 0) {
            if (!digits || b[part] > 255 || part > 3) { *ok = 0; return 0; }
            digits = 0;
            if (*s == 0) break;
            part++;
        } else { *ok = 0; return 0; }
    }
    if (part != 3) { *ok = 0; return 0; }
    *ok = 1;
    return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}
uint16_t net_checksum(const void *data, uint32_t len) {
    const uint8_t *p = data;
    uint32_t sum = 0;
    while (len > 1) { sum += (uint32_t)(p[0] << 8 | p[1]); p += 2; len -= 2; }
    if (len) sum += (uint32_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

#include "../../net/ip6.c"
#include "../../net/netaddr.c"
#include "../../net/udp.c"

static void roundtrip(void) {
    /* Every one of these has to print back exactly as written: the shortest
     * form with one "::" over the longest run of zeros, ties to the first. */
    static const char *canonical[] = {
        "::",
        "::1",
        "fe80::1",
        "fe80::5054:ff:fe12:3456",
        "fec0::5054:ff:fe12:3456",
        "2001:db8::1",
        "2001:db8:0:1::1",
        "1:2:3:4:5:6:7:8",
        "ff02::1",
        "ff02::1:ff12:3456",
        "1::8",
        "a:b:c:d:e:f:1:2",
    };
    for (unsigned i = 0; i < sizeof canonical / sizeof canonical[0]; i++) {
        ip6_addr a;
        char back[46];
        assert(ip6_parse(canonical[i], &a));
        ip6_format(&a, back);
        if (strcmp(back, canonical[i]) != 0) {
            printf("  %s printed back as %s\n", canonical[i], back);
            assert(0);
        }
        assert(strstr(back, ":::") == NULL);
    }
    printf("  ok: %d canonical addresses parse and print back unchanged\n",
           (int)(sizeof canonical / sizeof canonical[0]));
}

static void shortening(void) {
    /* Forms that are legal but not canonical, and what they shorten to. */
    static const char *in[]  = { "0:0:0:0:0:0:0:0", "0:0:0:0:0:0:0:1",
                                 "FE80:0:0:0:0:0:0:1", "2001:0db8:0000:0000:0000:0000:0000:0001",
                                 "1:0:0:2:0:0:0:3", "1:0:0:0:2:0:0:3",
                                 /* One zero group is never worth a "::": RFC
                                    5952 says so, and it saves nothing. */
                                 "1:2:3:4:5:6:7::", "::2:3:4:5:6:7:8" };
    static const char *out[] = { "::", "::1",
                                 "fe80::1", "2001:db8::1",
                                 "1:0:0:2::3", "1::2:0:0:3",
                                 "1:2:3:4:5:6:7:0", "0:2:3:4:5:6:7:8" };
    for (unsigned i = 0; i < sizeof in / sizeof in[0]; i++) {
        ip6_addr a; char back[46];
        assert(ip6_parse(in[i], &a));
        ip6_format(&a, back);
        if (strcmp(back, out[i]) != 0) { printf("  %s -> %s, want %s\n", in[i], back, out[i]); assert(0); }
    }
    /* A lone zero group is not worth a "::", and the longest run wins. */
    ip6_addr a; char back[46];
    assert(ip6_parse("1:0:2:3:4:5:6:7", &a));
    ip6_format(&a, back);
    assert(!strcmp(back, "1:0:2:3:4:5:6:7"));
    printf("  ok: the longest zero run is shortened, a single zero group is not\n");
}

static void rejects(void) {
    static const char *bad[] = {
        "", ":", ":::", "::::",
        "fe80:::1",                       /* a third colon inside the gap */
        ":1::",                           /* a leading single colon       */
        "1:2:3:4:5:6:7",                  /* too few, and no gap          */
        "1:2:3:4:5:6:7:8:9",              /* too many                     */
        "1::2::3",                        /* two gaps                     */
        "0:0:0:0:0:0:0:0:0",
        "12345::",                        /* five hex digits              */
        "fe80::1:",                       /* trailing colon               */
        "fe80::g",                        /* not hex                      */
        "192.168.1.1",                    /* the other family             */
        "::ffff:192.168.1.1",             /* dotted-quad tail: unsupported */
        "1:2:3:4:5:6:7:8::",              /* a gap standing for nothing   */
        "::1:2:3:4:5:6:7:8",
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        ip6_addr a;
        if (ip6_parse(bad[i], &a)) { printf("  accepted %s\n", bad[i]); assert(0); }
    }
    printf("  ok: %d malformed addresses are all refused\n",
           (int)(sizeof bad / sizeof bad[0]));
}

static void classify(void) {
    ip6_addr a;
    assert(ip6_parse("::", &a) && ip6_is_zero(&a));
    assert(ip6_parse("fe80::1", &a) && ip6_is_link_local(&a) && !ip6_is_multicast(&a));
    assert(ip6_parse("febf::1", &a) && ip6_is_link_local(&a));
    assert(ip6_parse("fec0::1", &a) && !ip6_is_link_local(&a));   /* site-local */
    assert(ip6_parse("ff02::1", &a) && ip6_is_multicast(&a));
    assert(ip6_parse("2001:db8::1", &a) && !ip6_is_link_local(&a) && !ip6_is_multicast(&a));

    /* The multicast MAC is 33:33 and the last four bytes of the address. */
    uint8_t mac[6];
    assert(ip6_parse("ff02::1:ff12:3456", &a));
    ip6_multicast_mac(&a, mac);
    assert(mac[0] == 0x33 && mac[1] == 0x33 && mac[2] == 0xff &&
           mac[3] == 0x12 && mac[4] == 0x34 && mac[5] == 0x56);

    /* And the solicited-node group takes the low three bytes of the target. */
    ip6_addr t, sn; char txt[46];
    assert(ip6_parse("fe80::5054:ff:fe12:3456", &t));
    ip6_solicited_node(&t, &sn);
    ip6_format(&sn, txt);
    assert(!strcmp(txt, "ff02::1:ff12:3456"));
    printf("  ok: link-local, multicast, the 33:33 mapping and solicited-node groups\n");
}

static void eui64(void) {
    /* The link-local address of the MAC QEMU hands out, worked out by hand:
     * 52 with the universal/local bit flipped is 50, and ff:fe goes between
     * the halves. This is the one piece of IPv6 everybody gets wrong once. */
    struct netif_stub { char name[8]; uint8_t mac[6]; } nif = { "eth0", { 0x52,0x54,0x00,0x12,0x34,0x56 } };
    ip6_init((struct netif *)&nif);
    char txt[46];
    ip6_format(&net6_link, txt);
    assert(!strcmp(txt, "fe80::5054:ff:fe12:3456"));
    assert(!net6_have_addr && !net6_have_gw);
    printf("  ok: the link-local address is the MAC as a modified EUI-64\n");
}

static void checksums(void) {
    /* A checksum computed and then verified over the same bytes comes back
     * zero, which is how every receiver checks one. */
    ip6_addr s, d;
    assert(ip6_parse("fe80::1", &s) && ip6_parse("ff02::1", &d));
    uint8_t msg[24];
    for (int i = 0; i < 24; i++) msg[i] = (uint8_t)(i * 7 + 1);
    msg[2] = msg[3] = 0;
    uint16_t ck = ip6_checksum(&s, &d, IPPROTO_ICMPV6, msg, sizeof msg);
    msg[2] = (uint8_t)(ck >> 8); msg[3] = (uint8_t)ck;
    assert(ip6_checksum(&s, &d, IPPROTO_ICMPV6, msg, sizeof msg) == 0);

    /* It covers the addresses, so the same bytes from somewhere else fail. */
    ip6_addr other;
    assert(ip6_parse("fe80::2", &other));
    assert(ip6_checksum(&other, &d, IPPROTO_ICMPV6, msg, sizeof msg) != 0);

    /* And the payload: one flipped bit is caught. */
    msg[10] ^= 0x01;
    assert(ip6_checksum(&s, &d, IPPROTO_ICMPV6, msg, sizeof msg) != 0);
    printf("  ok: the pseudo-header checksum covers the addresses and the payload\n");
}

/* ---- one address, either family ---------------------------------------- */
static void addresses(void) {
    netaddr a, b;
    assert(netaddr_parse("10.0.2.2", &a) && netaddr_is_v4(&a) && !netaddr_is_v6(&a));
    assert(netaddr_parse("fe80::1", &b) && netaddr_is_v6(&b) && !netaddr_is_v4(&b));

    /* Two families never compare equal, whatever the bytes underneath say. */
    assert(!netaddr_equal(&a, &b));
    netaddr c = netaddr_v4(0x0A000202u);
    assert(netaddr_equal(&a, &c));

    char txt[NETADDR_STRLEN];
    netaddr_format(&a, txt); assert(!strcmp(txt, "10.0.2.2"));
    netaddr_format(&b, txt); assert(!strcmp(txt, "fe80::1"));
    netaddr_format(&(netaddr){ .af = NET_AF_INET, .v4 = 0 }, txt);
    assert(!strcmp(txt, "0.0.0.0"));

    assert(netaddr_is_unspecified(&(netaddr){ .af = NET_AF_INET, .v4 = 0 }));
    ip6_addr z; ip6_zero(&z);
    netaddr n6 = netaddr_v6(&z);
    assert(netaddr_is_unspecified(&n6));
    assert(!netaddr_is_unspecified(&b));

    /* Not an address of either kind. */
    assert(!netaddr_parse("", &a));
    assert(!netaddr_parse("fe80::g", &a));
    assert(!netaddr_parse("999.1.1.1", &a));
    printf("  ok: one address type holds either family, and never confuses them\n");
}

/* ---- UDP over IPv6 ------------------------------------------------------ */
static void udp6_send(void) {
    ip6_addr dst;
    assert(ip6_parse("fe80::5054:ff:feaa:bbcc", &dst));
    netaddr to = netaddr_v6(&dst);

    static const char payload[] = "boltos over v6";
    struct netif_stub { char name[8]; uint8_t mac[6]; } nif = { "eth0", { 0x52,0x54,0x00,0x12,0x34,0x56 } };
    tx_count = 0;
    assert(udp_send_addr((struct netif *)&nif, &to, 5353, 53,
                         payload, (uint16_t)strlen(payload)) == 0);
    assert(tx_count == 1);
    assert(tx_type == ETHERTYPE_IPV6);
    /* Unicast, so the destination MAC is the neighbour's, not the 33:33 form. */
    assert(tx_mac[0] == 0x52 && tx_mac[5] == 0xcc);

    const struct ip6_hdr *h = (const struct ip6_hdr *)tx_pkt;
    assert((ntohl(h->ver_tc_flow) >> 28) == 6);
    assert(h->next_hdr == IPPROTO_UDP);
    assert(h->hop_limit == 64);
    uint16_t plen = ntohs(h->payload_len);
    assert(plen == 8 + strlen(payload));
    assert(tx_len == IP6_HDR_LEN + plen);
    assert(memcmp(h->dst, dst.b, 16) == 0);
    /* A link-local destination is answered from the link-local address. */
    assert(memcmp(h->src, net6_link.b, 16) == 0);

    const struct udp_hdr *u = (const struct udp_hdr *)(tx_pkt + IP6_HDR_LEN);
    assert(ntohs(u->src_port) == 5353 && ntohs(u->dst_port) == 53);
    assert(ntohs(u->len) == plen);
    /* Mandatory over v6 -- zero would mean "not computed", and a receiver is
     * required to drop that. */
    assert(u->checksum != 0);

    /* And it is right: the receiver's own check is that the sum over the whole
     * datagram, checksum field included, comes to zero. */
    ip6_addr src; memcpy(src.b, h->src, 16);
    assert(ip6_checksum(&src, &dst, IPPROTO_UDP, tx_pkt + IP6_HDR_LEN, plen) == 0);
    printf("  ok: a v6 datagram carries the right header and a checksum that verifies\n");
}

static netaddr rx_from;
static int     rx_hits;
static char    rx_data[64];
static void udp6_cb(const netaddr *src, uint16_t sport, const uint8_t *data, uint16_t len) {
    (void)sport;
    rx_from = *src;
    if (len > sizeof rx_data - 1) len = sizeof rx_data - 1;
    memcpy(rx_data, data, len);
    rx_data[len] = 0;
    rx_hits++;
}

/* Build a whole IPv6 packet addressed to us, the way a peer would. */
static uint16_t build_udp6(uint8_t *out, const ip6_addr *from, const ip6_addr *to,
                           uint16_t sport, uint16_t dport, const char *body) {
    uint16_t blen = (uint16_t)strlen(body);
    uint16_t ulen = (uint16_t)(8 + blen);
    struct ip6_hdr *h = (struct ip6_hdr *)out;
    memset(h, 0, sizeof *h);
    h->ver_tc_flow = htonl(6u << 28);
    h->payload_len = htons(ulen);
    h->next_hdr    = IPPROTO_UDP;
    h->hop_limit   = 64;
    memcpy(h->src, from->b, 16);
    memcpy(h->dst, to->b, 16);
    struct udp_hdr *u = (struct udp_hdr *)(out + IP6_HDR_LEN);
    u->src_port = htons(sport); u->dst_port = htons(dport);
    u->len = htons(ulen); u->checksum = 0;
    memcpy(out + IP6_HDR_LEN + 8, body, blen);
    u->checksum = htons(ip6_checksum(from, to, IPPROTO_UDP, out + IP6_HDR_LEN, ulen));
    return (uint16_t)(IP6_HDR_LEN + ulen);
}

static void udp6_receive(void) {
    struct netif_stub { char name[8]; uint8_t mac[6]; } nif = { "eth0", { 0x52,0x54,0x00,0x12,0x34,0x56 } };
    ip6_addr from;
    assert(ip6_parse("fe80::5054:ff:feaa:bbcc", &from));

    uint8_t pkt[256];
    uint16_t n = build_udp6(pkt, &from, &net6_link, 4000, 7777, "hello");

    udp_listen(7777, udp6_cb);
    rx_hits = 0;
    ip6_input((struct netif *)&nif, pkt, n, 0);
    assert(rx_hits == 1);
    assert(!strcmp(rx_data, "hello"));
    assert(netaddr_is_v6(&rx_from) && ip6_equal(&rx_from.v6, &from));

    /* One flipped bit in the payload, and it does not arrive. Over v4 this
     * datagram would be delivered -- there is a header checksum underneath
     * and the UDP one is optional. Over v6 there is neither. */
    pkt[IP6_HDR_LEN + 9] ^= 0x01;
    ip6_input((struct netif *)&nif, pkt, n, 0);
    assert(rx_hits == 1);
    pkt[IP6_HDR_LEN + 9] ^= 0x01;

    /* A datagram that declines to compute one is malformed, not lenient. */
    struct udp_hdr *u = (struct udp_hdr *)(pkt + IP6_HDR_LEN);
    uint16_t keep = u->checksum;
    u->checksum = 0;
    ip6_input((struct netif *)&nif, pkt, n, 0);
    assert(rx_hits == 1);
    u->checksum = keep;

    /* Addressed to somebody else: dropped before the checksum is even looked
     * at, because ip6_is_ours says no. */
    ip6_addr elsewhere;
    assert(ip6_parse("fe80::dead", &elsewhere));
    n = build_udp6(pkt, &from, &elsewhere, 4000, 7777, "not yours");
    ip6_input((struct netif *)&nif, pkt, n, 0);
    assert(rx_hits == 1);

    udp_unlisten(7777);
    printf("  ok: a v6 datagram is delivered, and a corrupted or unchecked one is not\n");
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    roundtrip();
    shortening();
    rejects();
    classify();
    eui64();
    checksums();
    addresses();
    udp6_send();
    udp6_receive();
    puts("PASS ip6-host: address text, EUI-64, pseudo-header checksums and UDP over v6");
    return 0;
}
