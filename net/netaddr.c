#include <stdint.h>
#include "net.h"
#include "net6.h"
#include "netaddr.h"
#include "string.h"

/* Address handling common to both families. Small enough that it could have
 * lived in ip.c, but it belongs to neither half. */

netaddr netaddr_v4(uint32_t ip) {
    netaddr a;
    memset(&a, 0, sizeof a);
    a.af = NET_AF_INET;
    a.v4 = ip;
    return a;
}

netaddr netaddr_v6(const ip6_addr *src) {
    netaddr a;
    memset(&a, 0, sizeof a);
    a.af = NET_AF_INET6;
    if (src) ip6_copy(&a.v6, src);
    return a;
}

int netaddr_equal(const netaddr *a, const netaddr *b) {
    if (!a || !b || a->af != b->af) return 0;
    return a->af == NET_AF_INET6 ? ip6_equal(&a->v6, &b->v6) : a->v4 == b->v4;
}

int netaddr_is_unspecified(const netaddr *a) {
    if (!a) return 1;
    return a->af == NET_AF_INET6 ? ip6_is_zero(&a->v6) : a->v4 == 0;
}

int netaddr_parse(const char *s, netaddr *out) {
    if (!s || !out) return 0;

    /* A colon settles it. No IPv4 literal has one, and every IPv6 address has
     * at least two -- including "::". */
    for (const char *p = s; *p; p++)
        if (*p == ':') {
            ip6_addr v6;
            if (!ip6_parse(s, &v6)) return 0;
            *out = netaddr_v6(&v6);
            return 1;
        }

    int ok = 0;
    uint32_t v4 = net_parse_ipv4(s, &ok);
    if (!ok) return 0;
    *out = netaddr_v4(v4);
    return 1;
}

void netaddr_format(const netaddr *a, char *out) {
    if (!out) return;
    if (!a) { out[0] = 0; return; }
    if (a->af == NET_AF_INET6) { ip6_format(&a->v6, out); return; }

    uint8_t b[4] = { (uint8_t)(a->v4 >> 24), (uint8_t)(a->v4 >> 16),
                     (uint8_t)(a->v4 >> 8),  (uint8_t)a->v4 };
    int o = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t v = b[i];
        if (v >= 100) out[o++] = (char)('0' + v / 100);
        if (v >= 10)  out[o++] = (char)('0' + (v / 10) % 10);
        out[o++] = (char)('0' + v % 10);
        if (i < 3) out[o++] = '.';
    }
    out[o] = 0;
}
