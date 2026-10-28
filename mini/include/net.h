#pragma once
#include <stdint.h>
#include "netaddr.h"

/* BoltOS Mini: the desktop's net.h as far as net/http.c and net/tls.c use
 * it -- blocking TCP and DNS -- answered by mini/netcompat.c on Mini's own
 * stack. The names are mapped so they never meet Mini's tcp_* (minet.h). */
static inline uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) { return __builtin_bswap32(x); }
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

#define tcp_connect_addr bt_tcp_connect_addr
#define tcp_connect      bt_tcp_connect
#define tcp_send         bt_tcp_send
#define tcp_recv         bt_tcp_recv
#define tcp_close        bt_tcp_close
#define dns_resolve      bt_dns_resolve
#define dns_resolve_addr bt_dns_resolve_addr

struct tcp_conn;
struct tcp_conn *tcp_connect_addr(const netaddr *dst, uint16_t dport, uint32_t timeout_ms);
struct tcp_conn *tcp_connect(uint32_t dst_ip, uint16_t dport, uint32_t timeout_ms);
int  tcp_send(struct tcp_conn *c, const void *data, uint32_t len);
int  tcp_recv(struct tcp_conn *c, void *buf, uint32_t cap, uint32_t timeout_ms);
void tcp_close(struct tcp_conn *c);
int  dns_resolve(const char *host, uint32_t *ip_out, uint32_t timeout_ms);
int  dns_resolve_addr(const char *host, netaddr *out, uint32_t timeout_ms);
