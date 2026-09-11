/* The IPv6 address text forms, and the checksum, against the real net/ip6.c.
 *
 * These are pure functions over sixteen bytes, so they do not need a machine
 * to run -- and they are exactly the kind of code where a case that never
 * comes up in QEMU is wrong for years. "::" printing as ":::" and "fe80:::1"
 * parsing as fe80::1 were both found here rather than on the wire. */
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
/* Not <string.h>: the kernel's own declares the same functions without the
   dllimport the host header puts on them, and including both is an error.
   net/ip6.c brings in include/string.h, and the host libc supplies the code. */

#include "net.h"
#include "net6.h"

/* The parts of the stack an address does not need. */
struct netif;
int  eth_send(struct netif *nif, const uint8_t dst[6], uint16_t ethertype,
              const void *payload, uint16_t len) {
    (void)nif; (void)dst; (void)ethertype; (void)payload; (void)len; return 0;
}
int  nd6_lookup(const ip6_addr *a, uint8_t mac_out[6]) { (void)a; (void)mac_out; return 0; }
void nd6_solicit(struct netif *nif, const ip6_addr *t) { (void)nif; (void)t; }
void icmp6_input(struct netif *nif, const ip6_addr *s, const ip6_addr *d,
                 const uint8_t *p, uint16_t l) {
    (void)nif; (void)s; (void)d; (void)p; (void)l;
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

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    roundtrip();
    shortening();
    rejects();
    classify();
    eui64();
    checksums();
    puts("PASS ip6-host: address text, classification, EUI-64 and pseudo-header checksums");
    return 0;
}
