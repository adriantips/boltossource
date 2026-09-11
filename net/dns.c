#include <stdint.h>
#include "net.h"
#include "netif.h"
#include "netcfg.h"
#include "pit.h"
#include "string.h"

/* ===========================================================================
 *  Tiny DNS resolver: one A-record query over UDP/53 to net_dns, blocking until
 *  the reply arrives or we time out. Handles name compression in the answer
 *  section by skipping compressed/literal names; returns the first A record.
 *
 *  A lookup is answered from the cheapest source that can answer it:
 *      1. a literal dotted quad      -- no lookup at all
 *      2. /etc/hosts                 -- so a machine can name its own peers,
 *                                       and a local name never leaves the wire
 *      3. the cache, if not expired  -- honours the record's own TTL
 *      4. the wire
 *  Before the cache existed every image on a page re-resolved the same host,
 *  which put a full DNS round trip in front of each one; loading a page over a
 *  slow link spent more time resolving than fetching.
 * ===========================================================================*/

static volatile uint32_t dns_result;
static volatile int      dns_done;
static volatile uint16_t dns_want_id;
static volatile uint32_t dns_ttl;          /* TTL of the answer we accepted */

/* ------------------------------------------------------------------ cache --*/
#define DNS_CACHE_N    16
#define DNS_NAME_MAX   64
#define DNS_TTL_MIN    30u      /* clamp: a 0-TTL record would never cache    */
#define DNS_TTL_MAX    3600u    /* and a 30-day one would outlive its truth   */

struct dns_entry {
    char     name[DNS_NAME_MAX];
    uint32_t ip;
    uint64_t expires_tick;      /* pit_ticks() value after which it is stale  */
};
static struct dns_entry cache[DNS_CACHE_N];
static int              cache_next;         /* round-robin eviction cursor    */

/* TODO: A records only, no AAAA. Matches the rest of the stack -- sockets,
 * the resolver's own callers, everything above this takes a uint32_t address
 * -- but it means a AAAA-only name just fails here even after SLAAC gave the
 * machine a real IPv6 address (see net/nd6.c). Widening this is the same
 * "not something to do halfway" as widening the socket layer, so it waits on
 * that. */
static int name_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

static int cache_get(const char *host, uint32_t *ip_out) {
    for (int i = 0; i < DNS_CACHE_N; i++) {
        if (!cache[i].ip || !name_eq(cache[i].name, host)) continue;
        if (pit_ticks() >= cache[i].expires_tick) { cache[i].ip = 0; return 0; }
        *ip_out = cache[i].ip;
        return 1;
    }
    return 0;
}

static void cache_put(const char *host, uint32_t ip, uint32_t ttl_secs) {
    if (!ip) return;
    if (ttl_secs < DNS_TTL_MIN) ttl_secs = DNS_TTL_MIN;
    if (ttl_secs > DNS_TTL_MAX) ttl_secs = DNS_TTL_MAX;

    int slot = -1;
    for (int i = 0; i < DNS_CACHE_N && slot < 0; i++)
        if (!cache[i].ip || name_eq(cache[i].name, host)) slot = i;
    if (slot < 0) { slot = cache_next; cache_next = (cache_next + 1) % DNS_CACHE_N; }

    strncpy(cache[slot].name, host, DNS_NAME_MAX - 1);
    cache[slot].name[DNS_NAME_MAX - 1] = 0;
    cache[slot].ip           = ip;
    /* The PIT runs at 1000 Hz, so a second is 1000 ticks. */
    cache[slot].expires_tick = pit_ticks() + (uint64_t)ttl_secs * 1000ull;
}

void dns_cache_flush(void) {
    for (int i = 0; i < DNS_CACHE_N; i++) cache[i].ip = 0;
}

int dns_cache_dump(int i, const char **name, uint32_t *ip, uint32_t *secs_left) {
    if (i < 0 || i >= DNS_CACHE_N || !cache[i].ip) return 0;
    uint64_t now = pit_ticks();
    *name = cache[i].name;
    *ip   = cache[i].ip;
    *secs_left = now >= cache[i].expires_tick ? 0
               : (uint32_t)((cache[i].expires_tick - now) / 1000ull);
    return 1;
}

/* encode "www.example.com" as length-prefixed labels into out; return length */
static int encode_qname(const char *host, uint8_t *out) {
    int o = 0;
    const char *s = host;
    while (*s) {
        const char *dot = s;
        while (*dot && *dot != '.') dot++;
        int seglen = (int)(dot - s);
        if (seglen <= 0 || seglen > 63) return -1;
        out[o++] = (uint8_t)seglen;
        for (int i = 0; i < seglen; i++) out[o++] = (uint8_t)s[i];
        s = (*dot == '.') ? dot + 1 : dot;
    }
    out[o++] = 0;
    return o;
}

/* advance past a (possibly compressed) name at off; return offset after it */
static uint32_t skip_name(const uint8_t *msg, uint32_t len, uint32_t off) {
    while (off < len) {
        uint8_t b = msg[off];
        if ((b & 0xC0) == 0xC0) return off + 2;       /* compression pointer */
        if (b == 0) return off + 1;                   /* end of name         */
        off += 1u + b;                                /* literal label       */
    }
    return len;
}

static void dns_recv(uint32_t src_ip, uint16_t sport, const uint8_t *msg, uint16_t len) {
    (void)src_ip; (void)sport;
    if (len < 12) return;
    uint16_t id = (uint16_t)((msg[0] << 8) | msg[1]);
    if (id != dns_want_id) return;
    uint16_t qd = (uint16_t)((msg[4] << 8) | msg[5]);
    uint16_t an = (uint16_t)((msg[6] << 8) | msg[7]);
    if ((msg[3] & 0x0F) != 0) { dns_done = 1; return; } /* RCODE != 0 */

    uint32_t off = 12;
    for (uint16_t i = 0; i < qd; i++) off = skip_name(msg, len, off) + 4; /* +QTYPE+QCLASS */

    for (uint16_t i = 0; i < an && off + 10 <= len; i++) {
        off = skip_name(msg, len, off);
        if (off + 10 > len) break;
        uint16_t type   = (uint16_t)((msg[off] << 8) | msg[off + 1]);
        uint16_t rdlen  = (uint16_t)((msg[off + 8] << 8) | msg[off + 9]);
        uint32_t rdata  = off + 10;
        if (type == 1 && rdlen == 4 && rdata + 4 <= len) {     /* A record */
            dns_result = ((uint32_t)msg[rdata] << 24) | ((uint32_t)msg[rdata + 1] << 16) |
                         ((uint32_t)msg[rdata + 2] << 8) | (uint32_t)msg[rdata + 3];
            /* TTL is the 32-bit field at off+4, after TYPE and CLASS. */
            dns_ttl    = ((uint32_t)msg[off + 4] << 24) | ((uint32_t)msg[off + 5] << 16) |
                         ((uint32_t)msg[off + 6] << 8)  |  (uint32_t)msg[off + 7];
            dns_done = 1;
            return;
        }
        off = rdata + rdlen;
    }
    dns_done = 1;                                       /* answered, but no A */
}

int dns_resolve(const char *host, uint32_t *ip_out, uint32_t timeout_ms) {
    if (!host || !host[0]) return 0;

    /* a literal dotted quad needs no lookup */
    int ok = 0;
    uint32_t lit = net_parse_ipv4(host, &ok);
    if (ok) { *ip_out = lit; return 1; }

    /* /etc/hosts, then the cache -- both answer without touching the network,
     * which is also what makes them work when the network is down. */
    uint32_t hip = netcfg_hosts_lookup(host);
    if (hip) { *ip_out = hip; return 1; }
    if (cache_get(host, ip_out)) return 1;

    struct netif *nif = netif_default();
    if (!nif) return 0;

    uint8_t q[300];
    static uint16_t seq = 0x1000;
    uint16_t id = ++seq;
    q[0] = id >> 8; q[1] = id & 0xFF;
    q[2] = 0x01; q[3] = 0x00;       /* recursion desired */
    q[4] = 0; q[5] = 1;             /* QDCOUNT = 1 */
    q[6] = q[7] = q[8] = q[9] = q[10] = q[11] = 0;
    int qn = encode_qname(host, q + 12);
    if (qn < 0) return 0;
    int o = 12 + qn;
    q[o++] = 0; q[o++] = 1;         /* QTYPE  = A  */
    q[o++] = 0; q[o++] = 1;         /* QCLASS = IN */

    uint16_t sport = 50000 + (id & 0x0FFF);
    dns_done = 0; dns_result = 0; dns_want_id = id; dns_ttl = 0;
    udp_listen(sport, dns_recv);

    int got = 0;
    for (int attempt = 0; attempt < 3 && !got; attempt++) {
        udp_send(nif, net_dns, sport, 53, q, (uint16_t)o);
        uint64_t start = pit_ticks();
        while (pit_ticks() - start < timeout_ms / 3 + 1) {
            netif_poll_all();
            if (dns_done) { got = 1; break; }
            __asm__ volatile("hlt");
        }
    }

    udp_unlisten(sport);
    if (got && dns_result) {
        *ip_out = dns_result;
        cache_put(host, dns_result, dns_ttl);
        return 1;
    }
    return 0;
}
