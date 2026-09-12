#include <stdint.h>
#include "netif.h"
#include "net.h"
#include "driver.h"
#include "kprintf.h"

static struct netif   *netifs;       /* singly-linked list of registered ifaces */
static netif_input_fn  input_fn;     /* protocol-stack hook (P3)                 */

void net_init(void) {
    netifs = 0;
    input_fn = 0;
    kprintf("[net] core up; probing link drivers\n");
    netif_set_input(eth_input);          /* IPv4 stack handles inbound frames */
    driver_probe_all();                  /* bind drivers from the .drivers table */
    if (!netifs) kprintf("[net] no network interface found\n");
}

/* kprintf has no %02x, so format the MAC into "xx:xx:xx:xx:xx:xx" ourselves. */
static char *mac_str(const uint8_t *m, char *buf) {
    static const char hx[] = "0123456789abcdef";
    int p = 0;
    for (int i = 0; i < ETH_ALEN; i++) {
        buf[p++] = hx[m[i] >> 4];
        buf[p++] = hx[m[i] & 0xF];
        if (i < ETH_ALEN - 1) buf[p++] = ':';
    }
    buf[p] = 0;
    return buf;
}

void netif_register(struct netif *nif) {
    char buf[18];
    nif->next = netifs;
    netifs = nif;
    kprintf("[net] %s: %s link %s\n",
            nif->name, mac_str(nif->mac, buf), nif->link_up ? "up" : "down");
}

struct netif *netif_default(void) {
    for (struct netif *n = netifs; n; n = n->next)
        if (n->link_up) return n;
    return netifs;
}
struct netif *netif_list(void)    { return netifs; }

int netif_send(struct netif *nif, const void *frame, uint16_t len) {
    if (!nif || !nif->send) return -1;
    int r = nif->send(nif, frame, len);
    if (r == 0) { nif->tx_packets++; nif->tx_bytes += len; }
    else        { nif->tx_errors++; }
    return r;
}

void netif_poll_all(void) {
    /* A handler that polls is a handler that can be re-entered by the frame
     * its own poll delivers, and there is no bound on how deep that goes:
     * every level is another packet's worth of kernel stack. Neighbour
     * Discovery found this the moment the NIC started accepting multicast --
     * a router advertisement ran duplicate detection, which polled, which
     * delivered the next advertisement, which ran duplicate detection. It cost
     * a page fault two seconds into the boot, and it would be a remotely
     * triggered one on a network with a chatty router.
     *
     * The work belongs outside the handler, and this makes the mistake a
     * dropped poll rather than a crash. */
    static int busy;
    if (busy) return;
    busy = 1;
    for (struct netif *n = netifs; n; n = n->next)
        if (n->poll) n->poll(n);
    busy = 0;
}

void netif_set_input(netif_input_fn fn) { input_fn = fn; }

void netif_rx(struct netif *nif, const uint8_t *frame, uint16_t len) {
    nif->rx_packets++;
    nif->rx_bytes += len;
    if (nif->rx_hook && nif->rx_hook(nif, frame, len)) return;
    if (input_fn) input_fn(nif, frame, len);
    else          nif->rx_dropped++;   /* no stack bound yet */
}
