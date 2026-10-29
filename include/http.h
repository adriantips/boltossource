#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Dead-simple HTTP/1.0 GET. Plain TCP for http://, TLS 1.2 for https:// (see
 *  net/tls.c -- certificate NOT verified). No chunked transfer: we ask for
 *  Connection: close and read to EOF. Enough to pull a basic HTML page or text
 *  file into a buffer.
 * ===========================================================================*/

/* Fetch URL ("http[s]://host[:port]/path" or "host/path") into out (NUL-term,
 * truncated to cap-1). Returns body length on success, -1 on failure.
 *  *status   <- HTTP status code (e.g. 200, 404), if non-NULL. On failure it is
 *               a negative reason: -3 DNS, -4 TCP connect, -5 TLS handshake.
 *  location  <- value of a redirect's Location header (3xx), if non-NULL. */
int http_get(const char *url, char *out, uint32_t cap,
             int *status, char *location, uint32_t loc_cap);

/* What the http_request running now has read, headers included: a big
 * download's progress, for another thread to show. */
extern volatile uint32_t http_rx_bytes;

/* The same with a method, extra header lines ("Name: value\r\n" each, or
 * NULL) and a body (NULL for none; it gets a Content-Length). A User-Agent
 * among the headers replaces BoltOS's own. */
int http_request(const char *method, const char *url, const char *headers,
                 const void *body, uint32_t body_len, char *out, uint32_t cap,
                 int *status, char *location, uint32_t loc_cap);

/* ---- keep-alive session: reuse one connection for many same-host fetches --
 * http_open() connects (status: 0 ok, -3 DNS, -4 TCP, -5 TLS). http_fetch()
 * sends one keep-alive GET and reads exactly one length-delimited response
 * (returns body length, or -1). http_conn_can_reuse() reports whether a URL
 * targets the same scheme/host/port and the connection is still live. */
struct http_conn;
struct http_conn *http_open(const char *url, int *status);
int  http_conn_can_reuse(struct http_conn *c, const char *url);
int  http_fetch(struct http_conn *c, const char *url, char *out, uint32_t cap,
                int *status, char *location, uint32_t loc_cap);
void http_close_conn(struct http_conn *c);

/* Cookie jar access for document.cookie. get -> "k=v; k2=v2" for host. */
int  http_cookie_get(const char *host, char *out, uint32_t cap);
void http_cookie_set(const char *host, const char *kv);
