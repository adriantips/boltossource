#pragma once
#include <stdint.h>
#include "netaddr.h"

/* ===========================================================================
 *  Sockets, as ordinary file descriptors.
 *
 *  A socket here is a `file` (include/vfs.h) whose ops are socket ops and whose
 *  priv is the connection. That is the whole design: because it is a file,
 *  read(), write() and close() already work on it, the process fd table already
 *  owns it, and proc_release() already closes it when the process dies. There
 *  is no second descriptor space to keep in step with the first.
 *
 *  Only blocking stream sockets exist. The kernel TCP is blocking and
 *  poll-driven (every wait spins netif_poll_all), so there is nothing honest to
 *  return for O_NONBLOCK yet; a socket that lied about it would be worse than
 *  one that does not offer it.
 *
 *  Both families. A listening socket takes whichever arrives -- a port is a
 *  port, and the connection that comes off it carries its own peer address --
 *  so AF_INET6 only ever has to be asked for on the client side.
 * ===========================================================================*/

#define AF_INET      2
#define AF_INET6    10
#define SOCK_STREAM  1

struct file;

/* An unbound, unconnected stream socket. 0 on failure. */
struct file *sock_create(int domain, int type);

/* Client: connect to `ip` (host order) on `port`. 0 on success, -1 otherwise. */
int  sock_connect(struct file *f, uint32_t ip, uint16_t port, uint32_t timeout_ms);
int  sock_connect_addr(struct file *f, const netaddr *dst, uint16_t port,
                       uint32_t timeout_ms);

/* Server: claim a local port, then take connections on it. Ports below 1024 are
 * privileged, as everywhere else -- a normal account binding port 22 could
 * otherwise impersonate the ssh server to anyone who connected. */
int  sock_listen(struct file *f, uint16_t port);
/* peer_ip is 0 when the connection came in over IPv6, which has no uint32_t
 * form -- sock_accept_addr is the one that can say. */
struct file *sock_accept(struct file *f, uint32_t *peer_ip, uint16_t *peer_port,
                         uint32_t timeout_ms);
struct file *sock_accept_addr(struct file *f, netaddr *peer, uint16_t *peer_port,
                              uint32_t timeout_ms);

int  sock_is_socket(const struct file *f);
