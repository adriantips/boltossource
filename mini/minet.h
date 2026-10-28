#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS Mini  -  mini/minet.h
 *
 *  Mini's own small IPv4 stack (mini/net.c). The desktop's stack is built
 *  for a client fetching web pages and sends one TCP segment at a time,
 *  waiting for each ACK; a VNC server streaming a screen to a Mac needs a
 *  window of segments in flight, so Mini has its own: ARP, IPv4, ICMP echo,
 *  UDP, and a TCP that is a server first, with a send ring and a window.
 *
 *  Everything is polled from the main loop. Nothing blocks.
 * ===========================================================================*/
typedef struct netif {
    const char *name;                  /* "usb0", "eth0"                      */
    uint8_t  mac[6];
    uint32_t ip, mask, gw;             /* host order                          */
    int      up;
    /* Hand a frame to the wire. 0 = taken, -1 = no room right now (the
     * caller drops it; TCP will send it again). */
    int    (*tx)(struct netif *nif, const void *frame, int len);
    uint64_t rx_pkts, tx_pkts, rx_bytes, tx_bytes, tx_drops;
} netif_t;

void     net_init(void);
void     net_poll(void);
void     net_register(netif_t *nif);
netif_t *net_if(int i);                /* 0..n-1, NULL past the end           */
void     net_rx(netif_t *nif, const uint8_t *frame, int len);
uint32_t net_parse_ip(const char *s, int *ok);
void     net_fmt_ip(uint32_t ip, char *out);   /* 16 bytes                    */

/* ---- UDP --------------------------------------------------------------------- */
typedef struct {
    netif_t *nif;
    uint32_t src_ip, dst_ip;
    uint16_t sport, dport;
    uint8_t  src_mac[6];
} udp_meta_t;
typedef void (*udp_fn)(const udp_meta_t *m, const uint8_t *data, int len);
int  udp_bind(uint16_t port, udp_fn fn);
/* dst_mac NULL: resolve through ARP (dropped if not cached yet; ARP is
 * asked). dst_ip 0xFFFFFFFF goes to the broadcast MAC. */
int  udp_send(netif_t *nif, uint32_t src_ip, uint32_t dst_ip, const uint8_t *dst_mac,
              uint16_t sport, uint16_t dport, const void *data, int len);

/* ---- TCP --------------------------------------------------------------------- */
typedef struct tcb tcb_t;
int    tcp_listen(uint16_t port);
tcb_t *tcp_accept(uint16_t port);      /* a new established connection, or NULL */
int    tcp_write(tcb_t *c, const void *data, int len);  /* bytes queued (may be < len) */
int    tcp_space(tcb_t *c);            /* bytes tcp_write would take now       */
int    tcp_unsent(tcb_t *c);           /* queued but not yet acknowledged      */
int    tcp_read(tcb_t *c, void *buf, int cap);
int    tcp_avail(tcb_t *c);
int    tcp_open(tcb_t *c);             /* still connected                      */
int    tcp_peer_closed(tcb_t *c);      /* they sent FIN (and we read it all)   */
void   tcp_close(tcb_t *c);            /* FIN after the queue drains; frees later */
void   tcp_abort(tcb_t *c);
uint32_t tcp_peer(tcb_t *c, uint16_t *port);
int    tcp_conn_count(void);
