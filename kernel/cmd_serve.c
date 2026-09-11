#include <stdint.h>
#include "commands.h"
#include "net.h"
#include "netif.h"
#include "fs.h"
#include "users.h"
#include "proc.h"
#include "pit.h"
#include "keyboard.h"
#include "kprintf.h"
#include "string.h"
#include <stdio.h>
#include <stdarg.h>

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_serve.c
 *  `serve` -- an HTTP file server, and `netget` -- a ring-3 socket client.
 *
 *  These are the two halves of the passive-open work made usable. Until
 *  tcp_listen/tcp_accept existed the machine could only ever make connections,
 *  never take them, which meant it could browse the web but nothing could reach
 *  it: no way to pull a file off it from a laptop or a phone, and no basis for
 *  any service at all.
 *
 *  The server is deliberately small -- GET only, one connection at a time, no
 *  keep-alive -- because the job is handing files to a browser on the same
 *  network, and every additional feature is another parser exposed to whatever
 *  is on that network.
 *
 *  Paths are resolved through the same fs_may() check everything else uses, so
 *  serving a directory cannot hand out a file the serving user could not read
 *  themselves.
 * ===========================================================================*/

#define SERVE_DEFAULT_PORT 8080
#define REQ_MAX            2048
#define SEND_CHUNK         1400

static const char *mime_for(const char *path) {
    const char *dot = 0;
    for (const char *p = path; *p; p++) if (*p == '.') dot = p;
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html") || !strcmp(dot, ".htm")) return "text/html";
    if (!strcmp(dot, ".css"))  return "text/css";
    if (!strcmp(dot, ".js"))   return "application/javascript";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".txt") || !strcmp(dot, ".md"))  return "text/plain";
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcmp(dot, ".gif"))  return "image/gif";
    return "application/octet-stream";
}

/* Percent-decode in place, and refuse anything with a ".." segment. A path
 * traversal here would hand out the whole filesystem to the network, so this is
 * checked before the path is ever looked up rather than after. */
static int sanitise(char *p) {
    char *o = p;
    for (char *i = p; *i; ) {
        if (*i == '%' && i[1] && i[2]) {
            int hi = (i[1] <= '9') ? i[1] - '0' : (i[1] | 32) - 'a' + 10;
            int lo = (i[2] <= '9') ? i[2] - '0' : (i[2] | 32) - 'a' + 10;
            if (hi >= 0 && hi < 16 && lo >= 0 && lo < 16) {
                *o++ = (char)((hi << 4) | lo); i += 3; continue;
            }
        }
        if (*i == '?') break;                   /* query string: not our business */
        *o++ = *i++;
    }
    *o = 0;
    if (p[0] != '/') return -1;
    for (char *i = p; *i; i++)
        if (i[0] == '.' && i[1] == '.') return -1;
    return 0;
}

static void send_str(struct tcp_conn *c, const char *s) {
    tcp_send(c, s, (uint32_t)strlen(s));
}

static void send_status(struct tcp_conn *c, const char *status, const char *body) {
    char hdr[256];
    snprintf(hdr, sizeof hdr,
             "HTTP/1.1 %s\r\nContent-Type: text/plain\r\n"
             "Content-Length: %u\r\nConnection: close\r\n\r\n",
             status, (unsigned)strlen(body));
    send_str(c, hdr);
    send_str(c, body);
}

/* A directory listing, so a browser pointed at / can find its way around.
 *
 * Every append goes through `add`, which clamps rather than trusting snprintf's
 * return value. snprintf reports the length it *would* have written, so adding
 * it straight to the offset walks past the end of the buffer as soon as
 * anything truncates -- and then `buf + n` is out of bounds while `sizeof buf -
 * n` underflows to an enormous size. The lengths here come off the wire (the
 * request path) and off the disk (entry names), so that is reachable, not
 * theoretical. */
static int add(char *buf, int cap, int n, const char *fmt, ...) {
    if (n >= cap - 1) return n;                 /* already full */
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + n, (size_t)(cap - n), fmt, ap);
    va_end(ap);
    if (w < 0) return n;
    n += w;
    return n > cap - 1 ? cap - 1 : n;           /* clamp to what really fits */
}

static void send_index(struct tcp_conn *c, fs_node *dir, const char *urlpath) {
    char buf[4096];
    int n = add(buf, sizeof buf, 0,
                "<!doctype html><meta charset=utf-8>"
                "<title>%s</title><h1>%s</h1><ul>", urlpath, urlpath);
    int sep = (urlpath[0] && urlpath[strlen(urlpath) - 1] == '/') ? 0 : 1;
    for (fs_node *ch = dir->child; ch && n < (int)sizeof buf - 256; ch = ch->next)
        n = add(buf, sizeof buf, n, "<li><a href=\"%s%s%s\">%s%s</a></li>",
                urlpath, sep ? "/" : "", ch->name, ch->name, ch->is_dir ? "/" : "");
    n = add(buf, sizeof buf, n, "</ul>");

    char hdr[160];
    snprintf(hdr, sizeof hdr,
             "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
             "Content-Length: %d\r\nConnection: close\r\n\r\n", n);
    send_str(c, hdr);
    tcp_send(c, buf, (uint32_t)n);
}

/* Serve one request, then close. Returns 0 normally, -1 if the peer vanished. */
static int handle(struct tcp_conn *c, const char *root) {
    char req[REQ_MAX];
    int got = tcp_recv(c, req, sizeof req - 1, 5000);
    if (got <= 0) return -1;
    req[got] = 0;

    if (strncmp(req, "GET ", 4) != 0) {
        send_status(c, "501 Not Implemented", "this server only answers GET\n");
        return 0;
    }

    char urlpath[512];
    int n = 0;
    for (const char *p = req + 4; *p && *p != ' ' && n < (int)sizeof urlpath - 1; p++)
        urlpath[n++] = *p;
    urlpath[n] = 0;
    if (sanitise(urlpath) != 0) {
        send_status(c, "400 Bad Request", "bad path\n");
        return 0;
    }

    /* Join the served root with the request path. */
    char full[600];
    if (!strcmp(root, "/")) snprintf(full, sizeof full, "%s", urlpath);
    else                    snprintf(full, sizeof full, "%s%s", root, urlpath);
    int fl = (int)strlen(full);
    while (fl > 1 && full[fl - 1] == '/') full[--fl] = 0;

    fs_node *node = fs_lookup(full[0] ? full : "/");
    if (!node) { send_status(c, "404 Not Found", "no such file\n"); return 0; }

    /* The same permission gate the shell and the VFS use: serving a tree must
     * not become a way around the ownership rules on it. */
    if (!fs_may(node, FS_PERM_READ)) {
        send_status(c, "403 Forbidden", "permission denied\n");
        return 0;
    }

    if (node->is_dir) { send_index(c, node, urlpath); return 0; }

    char hdr[200];
    snprintf(hdr, sizeof hdr,
             "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
             "Content-Length: %u\r\nConnection: close\r\n\r\n",
             mime_for(full), (unsigned)node->size);
    send_str(c, hdr);
    for (uint32_t off = 0; off < node->size; off += SEND_CHUNK) {
        uint32_t chunk = node->size - off;
        if (chunk > SEND_CHUNK) chunk = SEND_CHUNK;
        if (tcp_send(c, (const uint8_t *)node->data + off, chunk) < 0) return -1;
    }
    return 0;
}

int cmd_serve(int argc, char **argv) {
    uint16_t port = SERVE_DEFAULT_PORT;
    const char *root = "/";
    if (argc > 1) {
        int v = atoi(argv[1]);
        if (v <= 0 || v > 65535) { kprintf("serve: bad port '%s'\n", argv[1]); return 1; }
        port = (uint16_t)v;
    }
    if (argc > 2) root = argv[2];

    if (port < 1024 && !users_is_root()) {
        kprintf("serve: port %u is privileged -- run under sudo\n", port);
        return 1;
    }
    fs_node *r = fs_lookup(root);
    if (!r || !r->is_dir) { kprintf("serve: '%s' is not a directory\n", root); return 1; }
    if (!netif_default())  { kprintf("serve: no network interface is up\n"); return 1; }

    if (tcp_listen(port) != 0) {
        kprintf("serve: could not listen on port %u\n", port);
        return 1;
    }

    kprintf("serving %s on http://%u.%u.%u.%u:%u/  (any key stops)\n", root,
            (net_ip >> 24) & 0xFF, (net_ip >> 16) & 0xFF,
            (net_ip >> 8) & 0xFF, net_ip & 0xFF, port);
    /* The same listener takes v6 connections, so say where. The brackets are
     * not decoration: without them nobody can tell the address from the port. */
    if (!ip6_is_zero(&net6_link)) {
        char txt[46];
        ip6_format(net6_have_addr ? &net6_addr : &net6_link, txt);
        kprintf("      and on http://[%s]:%u/\n", txt, port);
    }

    int served = 0;
    for (;;) {
        if (kbd_trygetc() >= 0) break;             /* a keypress ends the server */
        struct tcp_conn *c = tcp_accept(port, 250);
        if (!c) continue;                          /* just a poll timeout */
        netaddr peer; tcp_peer_addr(c, &peer);
        int rc = handle(c, root);
        tcp_close(c);
        served++;
        char who[NETADDR_STRLEN]; netaddr_format(&peer, who);
        kprintf("  [%d] %s %s\n", served, who, rc == 0 ? "served" : "dropped");
    }

    tcp_unlisten(port);
    kprintf("serve: stopped after %d request%s\n", served, served == 1 ? "" : "s");
    return 0;
}

/* `netget HOST [PATH] [PORT]` -- the client half, in RING 3. Execs /bin/netget,
 * which resolves the name and speaks HTTP over a socket using nothing but the
 * socket syscalls and read/write. */
int cmd_netget(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: netget HOST [PATH] [PORT]\n"); return 1; }
    if (argc > PROC_MAX_ARGV) argc = PROC_MAX_ARGV;

    /* Hand the program its whole argument vector. argv[0] is the path it was
     * loaded from, as a program expects -- passing only one operand meant
     * `netget HOST PATH PORT` silently ran as `netget HOST`. */
    const char *av[PROC_MAX_ARGV];
    av[0] = "/bin/netget";
    for (int i = 1; i < argc; i++) av[i] = argv[i];
    if (proc_exec_vec("/bin/netget", argc, av) < 0) {
        kprintf("netget: could not start the ring-3 client\n");
        return 1;
    }
    return 0;
}
