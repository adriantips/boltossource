#pragma once
#include <stdint.h>
#include "net6.h"

/* One address, either family.
 *
 * The v4 half of this stack carries addresses as a uint32_t in host order and
 * the v6 half carries sixteen bytes in wire order. Neither of those is going
 * to change -- a uint32_t is the natural thing for v4 arithmetic and byte
 * swapping sixteen bytes would be pure ceremony -- so this is the seam where
 * the two meet, not a third representation imposed on both. Anything that
 * takes a peer (a TCP connection, a UDP datagram, the resolver, http, tls,
 * ssh) takes one of these; everything below still takes its own family's form.
 */

#define NET_AF_INET   4
#define NET_AF_INET6  6

typedef struct {
    uint8_t  af;                   /* NET_AF_INET / NET_AF_INET6 */
    uint32_t v4;                   /* host order  */
    ip6_addr v6;                   /* wire order  */
} netaddr;

#define NETADDR_STRLEN 46          /* the longest v6 text form, plus the NUL */

static inline int netaddr_is_v6(const netaddr *a) { return a && a->af == NET_AF_INET6; }
static inline int netaddr_is_v4(const netaddr *a) { return a && a->af == NET_AF_INET;  }

netaddr netaddr_v4(uint32_t ip);
netaddr netaddr_v6(const ip6_addr *a);

int  netaddr_equal(const netaddr *a, const netaddr *b);
/* 0.0.0.0 or ::, i.e. nothing to send to. */
int  netaddr_is_unspecified(const netaddr *a);

/* "10.0.2.2", "::1", "fe80::1" -- whichever it looks like. 1 on success.
 * A bare v6 address is recognised by the colon, which no v4 literal has. */
int  netaddr_parse(const char *s, netaddr *out);
void netaddr_format(const netaddr *a, char *out);   /* out: NETADDR_STRLEN */
