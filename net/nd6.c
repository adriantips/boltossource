#include <stdint.h>
#include "net.h"
#include "net6.h"
#include "netif.h"
#include "pit.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  Neighbour Discovery: what ARP does on the IPv4 side, and rather more.
 *
 *  Address resolution is the same idea reached differently -- a Neighbour
 *  Solicitation goes to the target's solicited-node multicast group rather
 *  than to the whole broadcast domain, so only the machine being asked, and
 *  the handful that happen to share the low 24 bits of its address, are
 *  interrupted by it.
 *
 *  On top of that: Router Discovery and SLAAC, which is how an IPv6 host gets
 *  an address without a DHCP server. It solicits, a router advertises a
 *  prefix, and the host forms its own address from that prefix and its own
 *  interface identifier. Then it has to check nobody else got there first,
 *  which is Duplicate Address Detection.
 * ===========================================================================*/

#define ND_CACHE 16
struct nd_entry { ip6_addr addr; uint8_t mac[6]; int valid; };
static struct nd_entry cache[ND_CACHE];

/* Set while DAD is waiting: an advertisement for this address means somebody
 * else has it. */
static ip6_addr dad_target;
static volatile int dad_seen, dad_active;

/* An address a router advertisement suggested. It is only a candidate here:
 * claiming it means Duplicate Address Detection, and DAD polls for frames,
 * which cannot happen inside the handler that is processing one. */
static ip6_addr slaac_candidate;
static volatile int slaac_pending;

int nd6_lookup(const ip6_addr *a, uint8_t mac_out[6]) {
    for (int i = 0; i < ND_CACHE; i++)
        if (cache[i].valid && ip6_equal(&cache[i].addr, a)) {
            memcpy(mac_out, cache[i].mac, 6);
            return 1;
        }
    return 0;
}

void nd6_cache_put(const ip6_addr *a, const uint8_t mac[6]) {
    for (int i = 0; i < ND_CACHE; i++)
        if (cache[i].valid && ip6_equal(&cache[i].addr, a)) {
            memcpy(cache[i].mac, mac, 6);
            return;
        }
    for (int i = 0; i < ND_CACHE; i++)
        if (!cache[i].valid) {
            cache[i].valid = 1;
            ip6_copy(&cache[i].addr, a);
            memcpy(cache[i].mac, mac, 6);
            return;
        }
    ip6_copy(&cache[0].addr, a);                       /* evict slot 0 */
    memcpy(cache[0].mac, mac, 6);
}

int nd6_cache_dump(int i, ip6_addr *addr, uint8_t mac_out[6]) {
    if (i < 0 || i >= ND_CACHE || !cache[i].valid) return 0;
    ip6_copy(addr, &cache[i].addr);
    memcpy(mac_out, cache[i].mac, 6);
    return 1;
}

/* ---- sending ------------------------------------------------------------- */
/* A Neighbour Solicitation for `target`, from `src`. DAD sends one from the
 * unspecified address, which is the difference between "who has this?" and
 * "does anyone already have this?" -- and it is why the source link-layer
 * option must be left out of the second: an unspecified source has no address
 * the answer could be sent back to. */
static void solicit_from(struct netif *nif, const ip6_addr *src, const ip6_addr *target) {
    uint8_t buf[24 + 8];
    memset(buf, 0, sizeof buf);
    buf[0] = ICMP6_NEIGH_SOLICIT;
    memcpy(buf + 8, target->b, 16);
    uint16_t len = 24;
    if (!ip6_is_zero(src)) {
        buf[24] = ND_OPT_SOURCE_LLADDR;
        buf[25] = 1;                                   /* one 8-byte unit */
        memcpy(buf + 26, nif->mac, 6);
        len = 32;
    }

    ip6_addr group;
    ip6_solicited_node(target, &group);
    uint16_t ck = ip6_checksum(src, &group, IPPROTO_ICMPV6, buf, len);
    buf[2] = (uint8_t)(ck >> 8); buf[3] = (uint8_t)ck;
    ip6_output_from(nif, src, &group, IPPROTO_ICMPV6, buf, len, 255);
}

void nd6_solicit(struct netif *nif, const ip6_addr *target) {
    solicit_from(nif, &net6_link, target);
}

static void advertise(struct netif *nif, const ip6_addr *target, const ip6_addr *to) {
    uint8_t buf[24 + 8];
    memset(buf, 0, sizeof buf);
    buf[0] = ICMP6_NEIGH_ADVERT;
    buf[4] = 0x60;                    /* Solicited | Override, not a router */
    memcpy(buf + 8, target->b, 16);
    buf[24] = ND_OPT_TARGET_LLADDR;
    buf[25] = 1;
    memcpy(buf + 26, nif->mac, 6);

    uint16_t ck = ip6_checksum(target, to, IPPROTO_ICMPV6, buf, sizeof buf);
    buf[2] = (uint8_t)(ck >> 8); buf[3] = (uint8_t)ck;
    ip6_output_from(nif, target, to, IPPROTO_ICMPV6, buf, sizeof buf, 255);
}

int nd6_resolve(struct netif *nif, const ip6_addr *a, uint8_t mac_out[6],
                uint32_t timeout_ms) {
    if (ip6_is_multicast(a)) { ip6_multicast_mac(a, mac_out); return 1; }
    if (nd6_lookup(a, mac_out)) return 1;
    uint64_t start = pit_ticks(), last = 0;
    while (pit_ticks() - start < timeout_ms) {
        if (!last || pit_ticks() - last >= 400) { nd6_solicit(nif, a); last = pit_ticks(); }
        netif_poll_all();
        if (nd6_lookup(a, mac_out)) return 1;
        __asm__ volatile("hlt");
    }
    return 0;
}

/* ---- receiving ----------------------------------------------------------- */
/* Walk the options that follow a Neighbour Discovery message. Each is a type,
 * a length in 8-byte units, and that many bytes including the two. A length of
 * zero is malformed and would loop forever, so it ends the walk. */
static const uint8_t *nd_option(const uint8_t *opts, uint16_t len, uint8_t want,
                                uint16_t *opt_len) {
    uint16_t i = 0;
    while (i + 2 <= len) {
        uint16_t olen = (uint16_t)(opts[i + 1] * 8);
        if (olen == 0 || i + olen > len) return 0;
        if (opts[i] == want) { if (opt_len) *opt_len = olen; return opts + i; }
        i = (uint16_t)(i + olen);
    }
    return 0;
}

/* An advertisement's prefix names an address we could have. Whether we may
 * have it is a question for the network, asked once we are off this stack. */
static void slaac_from_prefix(const uint8_t *opt) {
    uint8_t prefix_len = opt[2];
    uint8_t flags = opt[3];
    if (prefix_len != 64 || !(flags & 0x40)) return;   /* the A bit, /64 only */

    ip6_addr candidate;
    memset(candidate.b, 0, 16);
    memcpy(candidate.b, opt + 16, 8);                  /* the prefix */
    memcpy(candidate.b + 8, net6_link.b + 8, 8);       /* our interface id */

    if (net6_have_addr && ip6_equal(&candidate, &net6_addr)) return;   /* already */
    ip6_copy(&slaac_candidate, &candidate);
    slaac_pending = 1;
}

void nd6_input(struct netif *nif, const ip6_addr *src, const ip6_addr *dst,
               const uint8_t *pkt, uint16_t len) {
    (void)dst;
    switch (pkt[0]) {

    case ICMP6_NEIGH_SOLICIT: {
        if (len < 24) return;
        ip6_addr target;
        memcpy(target.b, pkt + 8, 16);

        /* Somebody running DAD for an address we hold: answering is what tells
         * them to pick another one. */
        if (!ip6_is_ours(&target)) return;
        uint16_t olen = 0;
        const uint8_t *sl = nd_option(pkt + 24, (uint16_t)(len - 24),
                                      ND_OPT_SOURCE_LLADDR, &olen);
        if (sl && olen >= 8 && !ip6_is_zero(src)) nd6_cache_put(src, sl + 2);

        /* A solicitation from :: is DAD, and has nowhere to reply to but the
         * all-nodes group. */
        static const uint8_t all_nodes[16] = { 0xFF,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,1 };
        ip6_addr to;
        if (ip6_is_zero(src)) memcpy(to.b, all_nodes, 16); else ip6_copy(&to, src);
        advertise(nif, &target, &to);
        break;
    }

    case ICMP6_NEIGH_ADVERT: {
        if (len < 24) return;
        ip6_addr target;
        memcpy(target.b, pkt + 8, 16);
        if (dad_active && ip6_equal(&target, &dad_target)) { dad_seen = 1; return; }
        uint16_t olen = 0;
        const uint8_t *tl = nd_option(pkt + 24, (uint16_t)(len - 24),
                                      ND_OPT_TARGET_LLADDR, &olen);
        if (tl && olen >= 8) nd6_cache_put(&target, tl + 2);
        break;
    }

    case ICMP6_ROUTER_ADVERT: {
        if (len < 16) return;
        uint16_t lifetime = (uint16_t)((pkt[6] << 8) | pkt[7]);
        uint16_t olen = 0;
        const uint8_t *sl = nd_option(pkt + 16, (uint16_t)(len - 16),
                                      ND_OPT_SOURCE_LLADDR, &olen);
        if (sl && olen >= 8) nd6_cache_put(src, sl + 2);

        if (lifetime) { ip6_copy(&net6_gw, src); net6_have_gw = 1; }

        /* Every prefix option, not just the first: a router may advertise more
         * than one, and only the ones marked autonomous are ours to use. */
        uint16_t i = 16;
        while (i + 2 <= len) {
            uint16_t o = (uint16_t)(pkt[i + 1] * 8);
            if (o == 0 || i + o > len) break;
            if (pkt[i] == ND_OPT_PREFIX_INFO && o >= 32)
                slaac_from_prefix(pkt + i);
            i = (uint16_t)(i + o);
        }
        break;
    }

    default:
        break;
    }
}

/* ---- duplicate address detection ----------------------------------------- */
int nd6_dad(struct netif *nif, const ip6_addr *a, uint32_t timeout_ms) {
    ip6_addr unspecified;
    ip6_zero(&unspecified);

    ip6_copy(&dad_target, a);
    dad_seen = 0;
    dad_active = 1;
    solicit_from(nif, &unspecified, a);

    uint64_t start = pit_ticks();
    while (pit_ticks() - start < timeout_ms) {
        netif_poll_all();
        if (dad_seen) { dad_active = 0; return 0; }
        __asm__ volatile("hlt");
    }
    dad_active = 0;
    return 1;                      /* silence means nobody else claims it */
}

/* ---- stateless address autoconfiguration --------------------------------- */
int nd6_autoconfigure(struct netif *nif, uint32_t timeout_ms) {
    if (!nif) return 0;
    ip6_init(nif);

    /* The link-local address is ours too, and has to be checked before it is
     * used -- including by the solicitations below. */
    if (!nd6_dad(nif, &net6_link, 600)) {
        kprintf("[ipv6] the link-local address is already in use\n");
        ip6_zero(&net6_link);
        return 0;
    }
    char txt[46];
    ip6_format(&net6_link, txt);
    kprintf("[ok] IPv6 link-local %s\n", txt);

    /* Solicit, and keep soliciting: the first Router Solicitation can easily
     * be sent before the link is really carrying frames. */
    static const uint8_t all_routers[16] = { 0xFF,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,2 };
    ip6_addr rtr;
    memcpy(rtr.b, all_routers, 16);

    uint8_t rs[8 + 8];
    memset(rs, 0, sizeof rs);
    rs[0] = ICMP6_ROUTER_SOLICIT;
    rs[8] = ND_OPT_SOURCE_LLADDR;
    rs[9] = 1;
    memcpy(rs + 10, nif->mac, 6);

    uint64_t start = pit_ticks(), last = 0;
    while (pit_ticks() - start < timeout_ms) {
        if (!last || pit_ticks() - last >= 1000) {
            uint16_t ck = ip6_checksum(&net6_link, &rtr, IPPROTO_ICMPV6, rs, sizeof rs);
            rs[2] = (uint8_t)(ck >> 8); rs[3] = (uint8_t)ck;
            ip6_output_from(nif, &net6_link, &rtr, IPPROTO_ICMPV6, rs, sizeof rs, 255);
            last = pit_ticks();
        }
        netif_poll_all();
        if (slaac_pending) break;
        __asm__ volatile("hlt");
    }

    /* Now, on this stack rather than inside a handler, the candidate can be
     * put to the network. */
    if (slaac_pending) {
        slaac_pending = 0;
        if (nd6_dad(nif, &slaac_candidate, 1000)) {
            ip6_copy(&net6_addr, &slaac_candidate);
            net6_prefix_len = 64;
            net6_have_addr = 1;
        } else {
            ip6_format(&slaac_candidate, txt);
            kprintf("[ipv6] %s is already in use; not configuring it\n", txt);
        }
    }

    if (net6_have_addr) {
        ip6_format(&net6_addr, txt);
        kprintf("[ok] IPv6 SLAAC address %s/%d", txt, net6_prefix_len);
        if (net6_have_gw) {
            char g[46]; ip6_format(&net6_gw, g);
            kprintf(" via %s", g);
        }
        kprintf("\n");
        return 1;
    }
    kprintf("[ipv6] no router advertisement; link-local only\n");
    return 0;
}
