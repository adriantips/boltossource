#include <stdint.h>
#include "socket.h"
#include "vfs.h"
#include "net.h"
#include "users.h"
#include "kheap.h"
#include "string.h"

/* ===========================================================================
 *  BoltOS  -  kernel/socket.c
 *  Stream sockets, exposed through the VFS. See include/socket.h for why.
 *
 *  Until now the network stack was reachable only from ring 0 -- the browser
 *  got at it through SYS_HTTPGET, a syscall that fetches one URL and knows what
 *  HTTP is. That is a hole in the platform rather than in the browser: a ring-3
 *  program could not speak any other protocol, so nothing that talks to a
 *  server could be written for BoltOS at all.
 * ===========================================================================*/

#define PRIVILEGED_PORT 1024

typedef struct {
    struct tcp_conn *conn;         /* connected socket, or 0                  */
    uint16_t         port;         /* listening port, or 0                    */
    int              listening;
} sock_t;

static int64_t sock_read(file *f, void *buf, uint64_t len) {
    sock_t *s = (sock_t *)f->priv;
    if (!s || !s->conn || len == 0) return -1;
    /* Blocking, with a bound. A socket that waited forever would wedge the
     * thread against a peer that simply stopped talking, and this kernel has no
     * way to interrupt that wait. */
    int n = tcp_recv(s->conn, buf, (uint32_t)len, 30000);
    return n < 0 ? -1 : (int64_t)n;
}

static int64_t sock_write(file *f, const void *buf, uint64_t len) {
    sock_t *s = (sock_t *)f->priv;
    if (!s || !s->conn) return -1;
    int n = tcp_send(s->conn, buf, (uint32_t)len);
    return n < 0 ? -1 : (int64_t)n;
}

static int64_t sock_lseek(file *f, int64_t off, int whence) {
    (void)f; (void)off; (void)whence;
    return -1;                     /* a stream has no position to seek to */
}

static void sock_close_op(file *f) {
    sock_t *s = (sock_t *)f->priv;
    if (!s) return;
    if (s->conn)      tcp_close(s->conn);
    if (s->listening) tcp_unlisten(s->port);
    kfree(s);
    f->priv = 0;
}

static const file_ops sock_ops = {
    .read = sock_read, .write = sock_write, .lseek = sock_lseek, .close = sock_close_op,
};

int sock_is_socket(const file *f) { return f && f->ops == &sock_ops; }

static file *sock_alloc(void) {
    file *f = (file *)kmalloc(sizeof(*f));
    if (!f) return 0;
    sock_t *s = (sock_t *)kmalloc(sizeof(*s));
    if (!s) { kfree(f); return 0; }
    memset(f, 0, sizeof(*f));
    memset(s, 0, sizeof(*s));
    f->ops  = &sock_ops;
    f->priv = s;
    return f;
}

file *sock_create(int domain, int type) {
    if (domain != AF_INET || type != SOCK_STREAM) return 0;
    return sock_alloc();
}

int sock_connect(file *f, uint32_t ip, uint16_t port, uint32_t timeout_ms) {
    if (!sock_is_socket(f)) return -1;
    sock_t *s = (sock_t *)f->priv;
    if (!s || s->conn || s->listening) return -1;      /* already in use */
    s->conn = tcp_connect(ip, port, timeout_ms ? timeout_ms : 5000);
    return s->conn ? 0 : -1;
}

int sock_listen(file *f, uint16_t port) {
    if (!sock_is_socket(f) || !port) return -1;
    sock_t *s = (sock_t *)f->priv;
    if (!s || s->conn || s->listening) return -1;

    /* The well-known ports are privileged for the usual reason: if any account
     * could bind 22, it could stand in front of the ssh server and collect the
     * passwords of everyone who tried to connect. */
    if (port < PRIVILEGED_PORT && !users_is_root()) return -1;

    if (tcp_listen(port) != 0) return -1;
    s->port = port;
    s->listening = 1;
    return 0;
}

file *sock_accept(file *f, uint32_t *peer_ip, uint16_t *peer_port, uint32_t timeout_ms) {
    if (!sock_is_socket(f)) return 0;
    sock_t *s = (sock_t *)f->priv;
    if (!s || !s->listening) return 0;

    struct tcp_conn *c = tcp_accept(s->port, timeout_ms ? timeout_ms : 30000);
    if (!c) return 0;

    file *nf = sock_alloc();
    if (!nf) { tcp_close(c); return 0; }
    ((sock_t *)nf->priv)->conn = c;
    if (peer_ip)   *peer_ip   = tcp_peer_ip(c);
    if (peer_port) *peer_port = tcp_peer_port(c);
    return nf;
}
