#pragma once
#include <stdint.h>
#include "netif.h"

/* ===========================================================================
 *  IPv6: addressing, Neighbour Discovery and ICMPv6.
 *
 *  The IPv4 side of this stack carries addresses as a uint32_t in host order.
 *  That does not generalise, so IPv6 addresses are kept the way they travel --
 *  sixteen bytes, most significant first -- and never byte-swapped. Everything
 *  here takes and returns `ip6_addr` by pointer.
 *
 *  What is here: link-local addressing from the MAC, Neighbour Discovery
 *  (solicitation, advertisement and a neighbour cache, which is ARP's job on
 *  this side), Router Discovery and SLAAC, duplicate address detection, and
 *  ICMPv6 echo.
 *
 *  TCP and UDP over IPv6 live in net/tcp.c and net/udp.c with their v4
 *  selves, because a connection is a connection: what differs is which
 *  pseudo-header the checksum covers and which output path the segment
 *  leaves by, and both of those are three lines. See include/netaddr.h for
 *  the address type that let those two files stop caring.
 * ===========================================================================*/

#define ETHERTYPE_IPV6  0x86DD
#define IPPROTO_ICMPV6  58
#define IP6_HDR_LEN     40

struct eth_hdr;   /* net.h; only ever taken by pointer here */

typedef struct { uint8_t b[16]; } ip6_addr;

/* ICMPv6 types this stack knows about. */
#define ICMP6_ECHO_REQUEST   128
#define ICMP6_ECHO_REPLY     129
#define ICMP6_ROUTER_SOLICIT 133
#define ICMP6_ROUTER_ADVERT  134
#define ICMP6_NEIGH_SOLICIT  135
#define ICMP6_NEIGH_ADVERT   136

/* Neighbour Discovery option types. */
#define ND_OPT_SOURCE_LLADDR 1
#define ND_OPT_TARGET_LLADDR 2
#define ND_OPT_PREFIX_INFO   3
#define ND_OPT_MTU           5

struct ip6_hdr {
    uint32_t ver_tc_flow;      /* 4-bit version, traffic class, flow label */
    uint16_t payload_len;
    uint8_t  next_hdr;
    uint8_t  hop_limit;
    uint8_t  src[16];
    uint8_t  dst[16];
} __attribute__((packed));

struct icmp6_hdr {
    uint8_t  type, code;
    uint16_t checksum;
} __attribute__((packed));

/* ---- interface state ----------------------------------------------------
 *  One interface's worth, matching how net_ip and friends work on the IPv4
 *  side. A second configured interface is a change to both halves at once. */
extern ip6_addr net6_link;        /* fe80::/64, formed from the MAC        */
extern ip6_addr net6_addr;        /* the SLAAC global address, :: if none  */
extern ip6_addr net6_gw;          /* the advertising router's link-local   */
extern int      net6_prefix_len;  /* of net6_addr                          */
extern int      net6_have_addr;   /* a global address is configured        */
extern int      net6_have_gw;

/* ---- addresses ---------------------------------------------------------- */
int  ip6_is_zero(const ip6_addr *a);
int  ip6_equal(const ip6_addr *a, const ip6_addr *b);
int  ip6_is_multicast(const ip6_addr *a);
int  ip6_is_link_local(const ip6_addr *a);
void ip6_copy(ip6_addr *dst, const ip6_addr *src);
void ip6_zero(ip6_addr *a);

/* "fe80::1", "::", "2001:db8::1" -> 16 bytes. 1 on success. */
int  ip6_parse(const char *s, ip6_addr *out);
/* The shortest legal form, with one "::" run. `out` needs 46 bytes. */
void ip6_format(const ip6_addr *a, char *out);

/* The Ethernet address a multicast IPv6 address maps onto: 33:33 then the
 * last four bytes of the address. */
void ip6_multicast_mac(const ip6_addr *a, uint8_t mac_out[6]);
/* ff02::1:ffXX:XXXX -- the group a node listens on for solicitations of a. */
void ip6_solicited_node(const ip6_addr *a, ip6_addr *out);

/* ---- the layer ---------------------------------------------------------- */
void ip6_init(struct netif *nif);          /* form the link-local address */
int  ip6_output(struct netif *nif, const ip6_addr *dst, uint8_t next_hdr,
                const void *payload, uint16_t len);
/* The address a datagram to `dst` should claim to be from: the global one for
 * a global destination, the link-local otherwise. An upper layer needs this
 * before ip6_output does, because the checksum covers the source address. */
const ip6_addr *ip6_source_for(const ip6_addr *dst);
/* Where a datagram to `dst` is actually handed to the link: `dst` itself when
 * it is on-link or multicast, otherwise the router. 0 when there is no route
 * -- off-link with no router advertised is nowhere. */
int  ip6_nexthop(const ip6_addr *dst, ip6_addr *out);
/* Same, but with the source address chosen explicitly -- Neighbour Discovery
 * has to send from :: before it owns an address, and from the link-local when
 * a global one exists. */
int  ip6_output_from(struct netif *nif, const ip6_addr *src, const ip6_addr *dst,
                     uint8_t next_hdr, const void *payload, uint16_t len,
                     uint8_t hop_limit);
void ip6_input(struct netif *nif, const uint8_t *pkt, uint16_t len,
               const struct eth_hdr *eth);
/* Is this one of ours: a unicast address we hold, or a group we listen to? */
int  ip6_is_ours(const ip6_addr *a);

/* The IPv6 pseudo-header checksum, which every upper layer over IPv6 needs
 * and which, unlike IPv4's, is not optional. */
uint16_t ip6_checksum(const ip6_addr *src, const ip6_addr *dst, uint8_t next_hdr,
                      const void *payload, uint32_t len);

/* ---- ICMPv6 ------------------------------------------------------------- */
void icmp6_input(struct netif *nif, const ip6_addr *src, const ip6_addr *dst,
                 const uint8_t *pkt, uint16_t len);
int  icmp6_echo_send(struct netif *nif, const ip6_addr *dst, uint16_t id, uint16_t seq);
int  icmp6_wait_reply(uint16_t id, uint16_t seq, uint32_t timeout_ms);

/* ---- Neighbour Discovery ------------------------------------------------ */
void nd6_input(struct netif *nif, const ip6_addr *src, const ip6_addr *dst,
               const uint8_t *pkt, uint16_t len);
int  nd6_lookup(const ip6_addr *a, uint8_t mac_out[6]);        /* 1 = known */
void nd6_cache_put(const ip6_addr *a, const uint8_t mac[6]);
void nd6_solicit(struct netif *nif, const ip6_addr *target);
int  nd6_resolve(struct netif *nif, const ip6_addr *a, uint8_t mac_out[6],
                 uint32_t timeout_ms);
/* Walk the neighbour cache for `ifconfig -6`; 0 when slot i is empty. */
int  nd6_cache_dump(int i, ip6_addr *addr, uint8_t mac_out[6]);

/* Router Solicitation, then whatever Router Advertisement comes back: the
 * prefix becomes a global address and the router becomes the default route.
 * Returns 1 if an address was configured. Blocking, like dhcp_configure. */
int  nd6_autoconfigure(struct netif *nif, uint32_t timeout_ms);
/* Duplicate Address Detection for one address: 1 if nobody else claims it. */
int  nd6_dad(struct netif *nif, const ip6_addr *a, uint32_t timeout_ms);
