#include <stdint.h>
#include "net.h"
#include "net6.h"
#include "netif.h"
#include "pit.h"
#include "string.h"

/* ===========================================================================
 *  ICMPv6. Echo, and the door Neighbour Discovery arrives through -- on IPv6
 *  the two are the same protocol number, which is why address resolution has
 *  no separate ethertype the way ARP does.
 *
 *  Unlike IPv4's, the ICMPv6 checksum is mandatory and covers a pseudo-header
 *  of the source and destination addresses. A receiver that checks it will
 *  drop anything computed the IPv4 way, so this never takes the shortcut.
 * ===========================================================================*/

#define ICMP6_PAYLOAD 32

static volatile uint16_t reply_id, reply_seq;
static volatile int      reply_got;

int icmp6_echo_send(struct netif *nif, const ip6_addr *dst, uint16_t id, uint16_t seq) {
    uint8_t buf[sizeof(struct icmp6_hdr) + 4 + ICMP6_PAYLOAD];
    struct icmp6_hdr *h = (struct icmp6_hdr *)buf;
    h->type = ICMP6_ECHO_REQUEST; h->code = 0; h->checksum = 0;
    buf[4] = (uint8_t)(id >> 8);  buf[5] = (uint8_t)id;
    buf[6] = (uint8_t)(seq >> 8); buf[7] = (uint8_t)seq;
    for (int i = 0; i < ICMP6_PAYLOAD; i++) buf[8 + i] = (uint8_t)('a' + (i % 26));

    const ip6_addr *src = (net6_have_addr && !ip6_is_link_local(dst))
                          ? &net6_addr : &net6_link;
    h->checksum = htons(ip6_checksum(src, dst, IPPROTO_ICMPV6, buf, sizeof buf));
    return ip6_output_from(nif, src, dst, IPPROTO_ICMPV6, buf, sizeof buf, 64);
}

int icmp6_wait_reply(uint16_t id, uint16_t seq, uint32_t timeout_ms) {
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < timeout_ms) {
        netif_poll_all();
        if (reply_got && reply_id == id && reply_seq == seq) { reply_got = 0; return 1; }
        __asm__ volatile("hlt");
    }
    return 0;
}

void icmp6_input(struct netif *nif, const ip6_addr *src, const ip6_addr *dst,
                 const uint8_t *pkt, uint16_t len) {
    if (len < (uint16_t)sizeof(struct icmp6_hdr)) return;
    const struct icmp6_hdr *h = (const struct icmp6_hdr *)pkt;

    /* Verify before acting on it. A corrupt Neighbour Advertisement that is
     * believed puts a wrong MAC in the neighbour cache, which is a harder
     * problem than a dropped packet. */
    if (ip6_checksum(src, dst, IPPROTO_ICMPV6, pkt, len) != 0) return;

    switch (h->type) {
    case ICMP6_ECHO_REPLY:
        if (len >= 8) {
            reply_id  = (uint16_t)((pkt[4] << 8) | pkt[5]);
            reply_seq = (uint16_t)((pkt[6] << 8) | pkt[7]);
            reply_got = 1;
        }
        break;

    case ICMP6_ECHO_REQUEST: {
        if (len < 8 || len > 1400) return;
        static uint8_t reply[1408];
        memcpy(reply, pkt, len);
        struct icmp6_hdr *r = (struct icmp6_hdr *)reply;
        r->type = ICMP6_ECHO_REPLY;
        r->checksum = 0;
        /* Answer from the address it was sent to, unless that was a multicast
         * group -- nothing may use one as a source. */
        const ip6_addr *from = ip6_is_multicast(dst) ? &net6_link : dst;
        r->checksum = htons(ip6_checksum(from, src, IPPROTO_ICMPV6, reply, len));
        ip6_output_from(nif, from, src, IPPROTO_ICMPV6, reply, len, 64);
        break;
    }

    case ICMP6_ROUTER_ADVERT:
    case ICMP6_NEIGH_SOLICIT:
    case ICMP6_NEIGH_ADVERT:
        nd6_input(nif, src, dst, pkt, len);
        break;

    default:
        break;
    }
}
