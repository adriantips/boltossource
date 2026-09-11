#include "ulibc.h"
#include "string.h"

/* /bin/netget -- fetch a URL over a socket, from ring 3.
 *
 * The point is not the HTTP: it is that every byte of this runs in ring 3 and
 * reaches the network through the ordinary socket calls, not through a kernel
 * syscall that already knows what HTTP is. Until sockets existed, SYS_HTTPGET
 * was the only way out of a user program, which meant a ring-3 program could
 * speak exactly one protocol and nothing else could be written for BoltOS.
 *
 * It also demonstrates that a socket is a plain file descriptor: the request
 * goes out with write() and the response comes back with read(), the same
 * calls that work on a file.
 *
 *     netget HOST [PATH] [PORT]
 */

#define BUFCAP 8192

static void puts_(const char *s) { write(1, s, strlen(s)); }

static void put_u(unsigned long v) {
    char t[24]; int i = 0;
    if (!v) { puts_("0"); return; }
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) { char c = t[--i]; write(1, &c, 1); }
}

static void put_ip(uint32_t ip) {
    for (int i = 3; i >= 0; i--) {
        put_u((ip >> (i * 8)) & 0xFF);
        if (i) puts_(".");
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { puts_("usage: netget HOST [PATH] [PORT]\n"); return 1; }
    const char *host = argv[1];
    const char *path = argc > 2 ? argv[2] : "/";
    int port = argc > 3 ? atoi(argv[3]) : 80;

    uint32_t ip = 0;
    if (resolve_host(host, &ip) != 0) {
        puts_("netget: cannot resolve "); puts_(host); puts_("\n");
        return 1;
    }
    puts_("resolved "); puts_(host); puts_(" -> "); put_ip(ip); puts_("\n");

    int fd = socket(AF_INET, SOCK_STREAM);
    if (fd < 0) { puts_("netget: no socket\n"); return 1; }
    if (connect_ip(fd, ip, (uint16_t)port, 5000) != 0) {
        puts_("netget: connect failed\n"); close(fd); return 1;
    }
    puts_("connected\n");

    /* A socket is just an fd, so the request is an ordinary write(). */
    char req[512];
    int n = 0;
    n += snprintf(req + n, sizeof req - n, "GET %s HTTP/1.1\r\n", path);
    n += snprintf(req + n, sizeof req - n, "Host: %s\r\n", host);
    n += snprintf(req + n, sizeof req - n, "Connection: close\r\n\r\n");
    if (write(fd, req, (unsigned long)n) < 0) {
        puts_("netget: send failed\n"); close(fd); return 1;
    }

    /* ...and the response an ordinary read(), until the peer closes. */
    char buf[BUFCAP];
    unsigned long total = 0;
    for (;;) {
        long got = read(fd, buf, sizeof buf);
        if (got <= 0) break;
        write(1, buf, (unsigned long)got);
        total += (unsigned long)got;
    }
    close(fd);

    puts_("\nnetget: "); put_u(total); puts_(" bytes over a ring-3 socket\n");
    return 0;
}
