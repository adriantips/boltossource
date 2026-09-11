#include <stdint.h>
#include "net.h"
#include "net6.h"
#include "netif.h"
#include "string.h"

/* Minimal UDP: send + a port -> callback demux for receive (DHCP, DNS, NTP
 * all register one). No real socket object -- a "listener" is just a port
 * number and a function pointer in a small table.
 *
 * The checksum is where the two families differ and the only place they do.
 * Over IPv4 it is optional, so this sends 0 and computes nothing. Over IPv6
 * there is no header checksum underneath to fall back on, so it is mandatory
 * -- and a zero there means "not computed", which a conforming receiver
 * drops, so a sum that comes out zero goes on the wire as all ones. */
static uint8_t udppkt[1500];

int udp_send_addr(struct netif *nif, const netaddr *dst, uint16_t sport, uint16_t dport,
                  const void *data, uint16_t len) {
    if (!dst) return -1;
    if (sizeof(struct udp_hdr) + len > sizeof(udppkt)) return -1;
    struct udp_hdr *h = (struct udp_hdr *)udppkt;
    h->src_port = htons(sport);
    h->dst_port = htons(dport);
    h->len      = htons((uint16_t)(sizeof(*h) + len));
    h->checksum = 0;
    memcpy(udppkt + sizeof(*h), data, len);
    uint16_t total = (uint16_t)(sizeof(*h) + len);

    if (dst->af == NET_AF_INET6) {
        const ip6_addr *src = ip6_source_for(&dst->v6);
        uint16_t ck = ip6_checksum(src, &dst->v6, IPPROTO_UDP, udppkt, total);
        h->checksum = htons(ck ? ck : 0xFFFFu);
        return ip6_output_from(nif, src, &dst->v6, IPPROTO_UDP, udppkt, total, 64);
    }
    return ip_output(nif, dst->v4, IPPROTO_UDP, udppkt, total);
}

int udp_send(struct netif *nif, uint32_t dst_ip, uint16_t sport, uint16_t dport,
             const void *data, uint16_t len) {
    netaddr dst = netaddr_v4(dst_ip);
    return udp_send_addr(nif, &dst, sport, dport, data, len);
}

/* Port -> listener demux. Small table; the DNS resolver binds one ephemeral
 * port at a time. */
#define UDP_LISTENERS 8
static struct { uint16_t port; udp_listener_fn fn; } listeners[UDP_LISTENERS];

void udp_listen(uint16_t port, udp_listener_fn fn) {
    for (int i = 0; i < UDP_LISTENERS; i++)
        if (listeners[i].port == 0) { listeners[i].port = port; listeners[i].fn = fn; return; }
}
void udp_unlisten(uint16_t port) {
    for (int i = 0; i < UDP_LISTENERS; i++)
        if (listeners[i].port == port) { listeners[i].port = 0; listeners[i].fn = 0; return; }
}

static void udp_deliver(const netaddr *src, const uint8_t *pkt, uint16_t len) {
    if (len < (int)sizeof(struct udp_hdr)) return;
    const struct udp_hdr *h = (const struct udp_hdr *)pkt;
    uint16_t dport = ntohs(h->dst_port);
    uint16_t ulen  = ntohs(h->len);
    if (ulen < sizeof(*h) || ulen > len) ulen = len;
    const uint8_t *data = pkt + sizeof(*h);
    uint16_t dlen = (uint16_t)(ulen - sizeof(*h));
    for (int i = 0; i < UDP_LISTENERS; i++)
        if (listeners[i].port == dport && listeners[i].fn) {
            listeners[i].fn(src, ntohs(h->src_port), data, dlen);
            return;
        }
}

void udp_input(struct netif *nif, uint32_t src_ip, const uint8_t *pkt, uint16_t len) {
    (void)nif;
    netaddr src = netaddr_v4(src_ip);
    udp_deliver(&src, pkt, len);
}

void udp_input6(struct netif *nif, const ip6_addr *src6, const ip6_addr *dst6,
                const uint8_t *pkt, uint16_t len) {
    (void)nif;
    if (len < (int)sizeof(struct udp_hdr)) return;
    const struct udp_hdr *h = (const struct udp_hdr *)pkt;
    /* A v6 datagram with no checksum is malformed, not lenient. */
    if (h->checksum == 0) return;
    if (ip6_checksum(src6, dst6, IPPROTO_UDP, pkt, len) != 0) return;
    netaddr src = netaddr_v6(src6);
    udp_deliver(&src, pkt, len);
}
