#include <stdint.h>
#include "net.h"
#include "net6.h"
#include "netif.h"
#include "string.h"

/* ===========================================================================
 *  IPv6 addressing and the datagram layer. See include/net6.h.
 * ===========================================================================*/

ip6_addr net6_link, net6_addr, net6_gw;
int      net6_prefix_len, net6_have_addr, net6_have_gw;

/* ---- address helpers ----------------------------------------------------- */
void ip6_zero(ip6_addr *a) { memset(a->b, 0, 16); }
void ip6_copy(ip6_addr *d, const ip6_addr *s) { memcpy(d->b, s->b, 16); }
int  ip6_equal(const ip6_addr *a, const ip6_addr *b) { return memcmp(a->b, b->b, 16) == 0; }
int  ip6_is_zero(const ip6_addr *a) {
    for (int i = 0; i < 16; i++) if (a->b[i]) return 0;
    return 1;
}
int  ip6_is_multicast(const ip6_addr *a) { return a->b[0] == 0xFF; }
int  ip6_is_link_local(const ip6_addr *a) {
    return a->b[0] == 0xFE && (a->b[1] & 0xC0) == 0x80;
}

void ip6_multicast_mac(const ip6_addr *a, uint8_t mac_out[6]) {
    mac_out[0] = 0x33; mac_out[1] = 0x33;
    mac_out[2] = a->b[12]; mac_out[3] = a->b[13];
    mac_out[4] = a->b[14]; mac_out[5] = a->b[15];
}

void ip6_solicited_node(const ip6_addr *a, ip6_addr *out) {
    memset(out->b, 0, 16);
    out->b[0] = 0xFF; out->b[1] = 0x02;
    out->b[11] = 0x01; out->b[12] = 0xFF;
    out->b[13] = a->b[13]; out->b[14] = a->b[14]; out->b[15] = a->b[15];
}

/* ---- text ---------------------------------------------------------------- */
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Groups before the "::" go in head, groups after it in tail, and the zeros
 * the gap stands for are whatever is left between them. Written strictly: a
 * colon has to be part of a separator or part of the one permitted gap, so
 * "fe80:::1" is rejected rather than quietly read as fe80::1. */
int ip6_parse(const char *s, ip6_addr *out) {
    uint16_t head[8], tail[8];
    int nh = 0, nt = 0, after_gap = 0, seen_gap = 0;

    memset(out->b, 0, 16);
    if (!s || !*s) return 0;

    if (s[0] == ':') {                                 /* only legal as "::" */
        if (s[1] != ':') return 0;
        seen_gap = after_gap = 1;
        s += 2;
        if (!*s) return 1;                             /* "::" on its own */
        if (*s == ':') return 0;
    }

    for (;;) {
        int v = 0, digits = 0;
        while (hexval(*s) >= 0) {
            v = v * 16 + hexval(*s);
            if (++digits > 4) return 0;
            s++;
        }
        if (!digits) return 0;
        if (after_gap) { if (nt >= 8) return 0; tail[nt++] = (uint16_t)v; }
        else           { if (nh >= 8) return 0; head[nh++] = (uint16_t)v; }

        if (!*s) break;
        if (*s != ':') return 0;                       /* no dotted-quad tail */
        s++;
        if (*s == ':') {
            if (seen_gap) return 0;                    /* only one gap */
            seen_gap = after_gap = 1;
            s++;
            if (!*s) break;                            /* "fe80::" */
            if (*s == ':') return 0;                   /* ":::" */
        } else if (!*s) {
            return 0;                                  /* a trailing ':' */
        }
    }
    if (!seen_gap && nh != 8) return 0;
    if (nh + nt > 8) return 0;
    if (seen_gap && nh + nt == 8) return 0;            /* "::" standing for none */

    for (int i = 0; i < nh; i++) { out->b[i * 2] = (uint8_t)(head[i] >> 8); out->b[i * 2 + 1] = (uint8_t)head[i]; }
    for (int i = 0; i < nt; i++) {
        int w = 8 - nt + i;
        out->b[w * 2] = (uint8_t)(tail[i] >> 8); out->b[w * 2 + 1] = (uint8_t)tail[i];
    }
    return 1;
}

void ip6_format(const ip6_addr *a, char *out) {
    uint16_t w[8];
    for (int i = 0; i < 8; i++) w[i] = (uint16_t)((a->b[i * 2] << 8) | a->b[i * 2 + 1]);

    /* The longest run of zero words becomes "::", ties going to the first --
     * which is what RFC 5952 asks for, and what makes two printings of the
     * same address compare equal. */
    int best = -1, bestlen = 0, run = -1, runlen = 0;
    for (int i = 0; i < 8; i++) {
        if (w[i] == 0) {
            if (run < 0) { run = i; runlen = 0; }
            if (++runlen > bestlen) { best = run; bestlen = runlen; }
        } else { run = -1; runlen = 0; }
    }
    if (bestlen < 2) best = -1;                        /* a single zero stays 0 */

    /* The gap writes both of its colons and then steps over the run. A word
     * writes a separator only when the last character is not already one,
     * which is what keeps "::1" from coming out as ":::1" -- and "::" itself
     * from coming out as ":::", which is what it did. */
    static const char hex[] = "0123456789abcdef";
    int o = 0, i = 0;
    while (i < 8) {
        if (i == best) {
            out[o++] = ':';
            out[o++] = ':';
            i += bestlen;
            continue;
        }
        if (o && out[o - 1] != ':') out[o++] = ':';
        int started = 0;
        for (int sh = 12; sh >= 0; sh -= 4) {
            int d = (w[i] >> sh) & 0xF;
            if (d || started || sh == 0) { out[o++] = hex[d]; started = 1; }
        }
        i++;
    }
    out[o] = 0;
}

/* ---- checksums ----------------------------------------------------------- */
/* The pseudo-header is source, destination, a 32-bit length and the next
 * header in a 32-bit field, then the payload. */
static uint8_t ck_buf[40 + 1500];

uint16_t ip6_checksum(const ip6_addr *src, const ip6_addr *dst, uint8_t next_hdr,
                      const void *payload, uint32_t len) {
    if (len > sizeof(ck_buf) - 40) return 0;
    memcpy(ck_buf, src->b, 16);
    memcpy(ck_buf + 16, dst->b, 16);
    ck_buf[32] = (uint8_t)(len >> 24); ck_buf[33] = (uint8_t)(len >> 16);
    ck_buf[34] = (uint8_t)(len >> 8);  ck_buf[35] = (uint8_t)len;
    ck_buf[36] = ck_buf[37] = ck_buf[38] = 0;
    ck_buf[39] = next_hdr;
    memcpy(ck_buf + 40, payload, len);
    return net_checksum(ck_buf, 40 + len);
}

/* ---- the interface's own addresses --------------------------------------- */
void ip6_init(struct netif *nif) {
    if (!nif) return;
    /* Link-local: fe80::/64 with a Modified EUI-64 interface identifier. The
     * flipped universal/local bit in the first octet is the "modified" part,
     * and the ff:fe in the middle is where the 48-bit MAC is stretched. */
    memset(net6_link.b, 0, 16);
    net6_link.b[0] = 0xFE; net6_link.b[1] = 0x80;
    net6_link.b[8]  = (uint8_t)(nif->mac[0] ^ 0x02);
    net6_link.b[9]  = nif->mac[1];
    net6_link.b[10] = nif->mac[2];
    net6_link.b[11] = 0xFF;
    net6_link.b[12] = 0xFE;
    net6_link.b[13] = nif->mac[3];
    net6_link.b[14] = nif->mac[4];
    net6_link.b[15] = nif->mac[5];
    ip6_zero(&net6_addr);
    ip6_zero(&net6_gw);
    net6_prefix_len = 0;
    net6_have_addr = net6_have_gw = 0;
}

int ip6_is_ours(const ip6_addr *a) {
    if (ip6_equal(a, &net6_link)) return 1;
    if (net6_have_addr && ip6_equal(a, &net6_addr)) return 1;
    if (ip6_is_multicast(a)) {
        /* ff02::1, all nodes, and the solicited-node group of each address we
         * hold. Nothing else is listened for, so a multicast the machine never
         * joined does not get answered. */
        static const uint8_t all_nodes[16] = { 0xFF,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,1 };
        if (memcmp(a->b, all_nodes, 16) == 0) return 1;
        ip6_addr sn;
        ip6_solicited_node(&net6_link, &sn);
        if (ip6_equal(a, &sn)) return 1;
        if (net6_have_addr) {
            ip6_solicited_node(&net6_addr, &sn);
            if (ip6_equal(a, &sn)) return 1;
        }
    }
    return 0;
}

/* ---- output -------------------------------------------------------------- */
static uint8_t pkt6[IP6_HDR_LEN + 1500];

int ip6_output_from(struct netif *nif, const ip6_addr *src, const ip6_addr *dst,
                    uint8_t next_hdr, const void *payload, uint16_t len,
                    uint8_t hop_limit) {
    if (!nif || IP6_HDR_LEN + len > sizeof(pkt6)) return -1;

    uint8_t mac[6];
    if (ip6_is_multicast(dst)) {
        ip6_multicast_mac(dst, mac);
    } else {
        /* On-link if it is link-local, or inside the prefix we were given;
         * otherwise it goes to the router. Without a router and without a
         * matching prefix there is nowhere to send it. */
        const ip6_addr *nexthop = dst;
        if (!ip6_is_link_local(dst) && net6_have_addr) {
            int bytes = net6_prefix_len / 8;
            if (memcmp(dst->b, net6_addr.b, (size_t)bytes) != 0) {
                if (!net6_have_gw) return -1;
                nexthop = &net6_gw;
            }
        } else if (!ip6_is_link_local(dst)) {
            if (!net6_have_gw) return -1;
            nexthop = &net6_gw;
        }
        if (!nd6_lookup(nexthop, mac)) { nd6_solicit(nif, nexthop); return -1; }
    }

    struct ip6_hdr *h = (struct ip6_hdr *)pkt6;
    h->ver_tc_flow = htonl(6u << 28);
    h->payload_len = htons(len);
    h->next_hdr    = next_hdr;
    h->hop_limit   = hop_limit;
    memcpy(h->src, src->b, 16);
    memcpy(h->dst, dst->b, 16);
    memcpy(pkt6 + IP6_HDR_LEN, payload, len);

    return eth_send(nif, mac, ETHERTYPE_IPV6, pkt6, (uint16_t)(IP6_HDR_LEN + len));
}

int ip6_output(struct netif *nif, const ip6_addr *dst, uint8_t next_hdr,
               const void *payload, uint16_t len) {
    /* A global destination gets the global source if there is one; anything
     * link-local or multicast is answered from the link-local address, which
     * always exists. */
    const ip6_addr *src = &net6_link;
    if (net6_have_addr && !ip6_is_link_local(dst) && !ip6_is_multicast(dst))
        src = &net6_addr;
    return ip6_output_from(nif, src, dst, next_hdr, payload, len, 64);
}

/* ---- input --------------------------------------------------------------- */
void ip6_input(struct netif *nif, const uint8_t *pkt, uint16_t len,
               const struct eth_hdr *eth) {
    (void)eth;
    if (len < IP6_HDR_LEN) return;
    const struct ip6_hdr *h = (const struct ip6_hdr *)pkt;
    if ((ntohl(h->ver_tc_flow) >> 28) != 6) return;

    uint16_t plen = ntohs(h->payload_len);
    if ((uint32_t)IP6_HDR_LEN + plen > len) return;

    ip6_addr src, dst;
    memcpy(src.b, h->src, 16);
    memcpy(dst.b, h->dst, 16);
    if (!ip6_is_ours(&dst)) return;

    switch (h->next_hdr) {
    case IPPROTO_ICMPV6:
        icmp6_input(nif, &src, &dst, pkt + IP6_HDR_LEN, plen);
        break;
    default:
        /* TCP and UDP over IPv6 are not wired up; a datagram for them is
         * dropped rather than handed to a v4 handler that would read the
         * wrong addresses out of it. */
        break;
    }
}
