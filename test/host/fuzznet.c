/* Everything the machine parses because a server sent it.
 *
 * net/tls.c, net/x509.c, net/http.c and net/inflate.c read bytes chosen by
 * whoever is on the other end of the socket, before anything has been
 * authenticated -- the TLS record layer and the certificate parser run
 * *before* the certificate has been checked, which is the definition of
 * pre-authentication attack surface. All of it runs in ring 0 on this machine.
 *
 * The transport underneath them is replaced here rather than the parsers
 * themselves: tcp_connect/tcp_send/tcp_recv below hand out the fuzz input as
 * though a server had written it, in pieces, the way a real socket does. So
 * http_get() and tls_connect() run exactly as they ship, and what varies is
 * what the network said.
 *
 * net/tcp.c is not linked in here for that reason -- it would collide with the
 * fake transport, and it is a protocol rather than a parser, so it has its own
 * harness in test/host/fuzztcp.c.
 *
 * Build and run through test/t_fuzz.py; --list names the targets. */
#include <stdint.h>

#include "fuzz.h"        /* first: it defines the heap everything else uses */

#include "net.h"
#include "net6.h"
#include "netaddr.h"
#include "tls.h"
#include "http.h"
#include "x509.h"
#include "inflate.h"
#include "crypto.h"
#include "hw.h"

/* ------------------------------------------------------- the substitutes --*/

static uint64_t kmsgs;
void kputc(char c) { (void)c; }
void kprintf(const char *fmt, ...) { (void)fmt; kmsgs++; }
void panic(const char *msg) {
    fprintf(stderr, "panic(\"%s\") from a network parser -- that is the bug\n", msg);
    abort();
}

/* A clock that always moves. Every timeout in tls.c and http.c is
 * `while (pit_ticks() < deadline)`, so a clock that stood still would be a
 * hang rather than a finding. */
static uint64_t fake_ticks;
uint64_t pit_ticks(void) { return ++fake_ticks; }

/* A fixed clock, inside the built-in certificate's validity window and
 * nowhere near the host's idea of today -- x509.c compares notBefore and
 * notAfter against this, so a test that used the real date would start failing
 * on its own in 2036. It also failed immediately the first time, because the
 * certificate was generated the day this was written and the clock was set a
 * few months earlier. */
void rtc_now(struct rtc_time *t) {
    t->year = 2030; t->mon = 6; t->day = 15; t->hour = 12; t->min = 0; t->sec = 0;
}

netaddr netaddr_v4(uint32_t ip) { netaddr a; memset(&a, 0, sizeof a); a.af = NET_AF_INET; a.v4 = ip; return a; }

/* Any name resolves, to an address nothing will be sent to. Non-zero for
 * success, which is net/dns.c's convention and not the usual 0-is-good one --
 * getting that backwards made every target here answer "DNS failed" in a
 * microsecond and report a clean run, which is the worst way for a fuzzer to
 * be broken. selftest() below exists because of it. */
int dns_resolve_addr(const char *host, netaddr *out, uint32_t timeout_ms) {
    (void)host; (void)timeout_ms;
    if (!out) return 0;
    *out = netaddr_v4(0x0A000202);          /* 10.0.2.2, QEMU's gateway */
    return 1;
}

/* ---- the fake socket ---------------------------------------------------- */
/* One connection, and the fuzz input is what the peer says. Handed over in
 * pieces whose sizes come out of the data itself: a parser that only ever sees
 * a whole response in one read is not the parser that runs on a real network,
 * and record reassembly is exactly where these things break. Deterministic,
 * because a replay has to reproduce the split as well as the bytes. */
struct tcp_conn { int open; };
static struct tcp_conn the_conn;
static const uint8_t *srv;
static uint32_t srv_len, srv_pos;
static uint64_t bytes_sent;

static void wire_set(const uint8_t *d, uint32_t n) {
    srv = d; srv_len = n; srv_pos = 0; the_conn.open = 1; bytes_sent = 0;
}

struct tcp_conn *tcp_connect_addr(const netaddr *dst, uint16_t dport, uint32_t timeout_ms) {
    (void)dst; (void)dport; (void)timeout_ms;
    the_conn.open = 1;
    return &the_conn;
}
struct tcp_conn *tcp_connect(uint32_t ip, uint16_t dport, uint32_t timeout_ms) {
    netaddr a = netaddr_v4(ip);
    return tcp_connect_addr(&a, dport, timeout_ms);
}
int tcp_send(struct tcp_conn *c, const void *data, uint32_t len) {
    (void)data;
    if (!c || !c->open) return -1;
    bytes_sent += len;
    return (int)len;
}
int tcp_recv(struct tcp_conn *c, void *buf, uint32_t cap, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (!c || !c->open || !cap) return -1;
    if (srv_pos >= srv_len) return 0;                  /* clean close */
    uint32_t left = srv_len - srv_pos;
    uint32_t n = left < cap ? left : cap;
    if (n > 1) n = 1 + (uint32_t)srv[srv_pos] % n;     /* the split, from the data */
    memcpy(buf, srv + srv_pos, n);
    srv_pos += n;
    return (int)n;
}
void tcp_close(struct tcp_conn *c) { if (c) c->open = 0; }

/* ---------------------------------------------------------- the corpus ----*/
#define S(x) { (const uint8_t *)(x), (uint32_t)(sizeof(x) - 1) }

static const fz_seed http_corpus[] = {
    S("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
      "Content-Length: 26\r\nServer: fuzz\r\n\r\n<html><body>hi</body></html>"),
    S("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nplain body, read to EOF"),
    S("HTTP/1.1 301 Moved Permanently\r\nLocation: http://elsewhere.example/there\r\n"
      "Content-Length: 0\r\n\r\n"),
    S("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"),
    S("HTTP/1.1 200 OK\r\nSet-Cookie: a=1; Path=/; HttpOnly\r\n"
      "Set-Cookie: session=deadbeef; Domain=.example.com\r\nContent-Length: 2\r\n\r\nok"),
    S("HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot here!"),
    S("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 30\r\n\r\n"
      "\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03\xcb\x48\xcd\xc9\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00"),
    S("HTTP/1.1 204 No Content\r\n\r\n"),
    S("HTTP/1.1 200 OK\r\n\r\n"),
    S("HTTP/1.1 200\r\n"), S("HTTP/1.1"), S("\r\n\r\n"), S(""),
};

static const fz_seed url_corpus[] = {
    S("http://example.com/index.html"),
    S("https://example.com:8443/a/b/c?q=1&r=2#frag"),
    S("example.com/bare"),
    S("http://[fe80::1]:8080/v6"),
    S("http://user@host.example/path"),
    S("http://"), S("https://:"), S("http://["), S(":///"), S("/"), S(""),
};

static const fz_seed cookie_corpus[] = {
    S("a=1; Path=/; HttpOnly"),
    S("session=deadbeef; Domain=.example.com; Max-Age=3600"),
    S("name=value"),
    S("="), S(";;;"), S("a"), S(""),
};

static const fz_seed gzip_corpus[] = {
    /* "hello" through gzip, and the same DEFLATE stream without the wrapper. */
    S("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03\xcb\x48\xcd\xc9\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00"),
    S("\xcb\x48\xcd\xc9\xc9\x07\x00"),
    S("\x01\x05\x00\xfa\xffhello"),                    /* a stored block */
    S("\x1f\x8b\x08"), S("\x78\x9c"), S(""),
};

/* Filled in by main(): a DER certificate, and the TLS Certificate message
 * wrapped around it. */
static fz_seed cert_corpus[2];
static fz_seed certmsg_corpus[2];

/* One TLS 1.3 server flight, near enough that the record layer and the first
 * handshake message parse before anything goes wrong. Building a real one
 * would mean doing the key schedule from the client's side, which is a second
 * TLS implementation -- the mutator gets further than that would. */
static const fz_seed tls_corpus[] = {
    /* ServerHello: record header, handshake header, version, random,
     * session id echo, cipher suite, compression, extensions. */
    S("\x16\x03\x03\x00\x5a\x02\x00\x00\x56\x03\x03"
      "0123456789abcdef0123456789abcdef"
      "\x20" "0123456789abcdef0123456789abcdef"
      "\x13\x01\x00\x00\x2e"
      "\x00\x2b\x00\x02\x03\x04"
      "\x00\x33\x00\x24\x00\x1d\x00\x20"
      "0123456789abcdef0123456789abcdef"),
    S("\x16\x03\x01\x00\x04\x02\x00\x00\x00"),        /* a truncated one */
    S("\x15\x03\x03\x00\x02\x02\x28"),                /* an alert         */
    S("\x17\x03\x03\x00\x05hello"),                   /* application data early */
    S("\x16\x03\x03\xff\xff"),                        /* a length with no body  */
    S("\x16"), S(""),
};

/* -------------------------------------------------------------- targets ---*/

static char resp[256 * 1024];
static char location[512];

/* The whole HTTP client: status line, headers, cookies, chunked decoding,
 * gzip and deflate, and the redirect's Location. */
static void t_http(const uint8_t *d, uint32_t n) {
    wire_set(d, n);
    int status = 0;
    http_get("http://fuzz.example/page", resp, sizeof resp, &status, location, sizeof location);
}

/* The same response over a keep-alive session, which is the length-delimited
 * path rather than the read-to-EOF one, and the reuse test on top. */
static void t_keepalive(const uint8_t *d, uint32_t n) {
    wire_set(d, n);
    int status = 0;
    struct http_conn *c = http_open("http://fuzz.example/", &status);
    if (!c) return;
    http_fetch(c, "http://fuzz.example/a", resp, sizeof resp, &status, location, sizeof location);
    http_conn_can_reuse(c, "http://fuzz.example/b");
    http_fetch(c, "http://fuzz.example/b", resp, sizeof resp, &status, NULL, 0);
    http_close_conn(c);
}

/* The URL parser, with a fixed response behind it. A URL is untrusted too --
 * it comes off a page, out of a redirect's Location, or from a script. */
static char urlbuf[8192];
static void t_url(const uint8_t *d, uint32_t n) {
    static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";
    wire_set((const uint8_t *)ok, (uint32_t)(sizeof ok - 1));
    if (n > sizeof urlbuf - 1) n = sizeof urlbuf - 1;
    memcpy(urlbuf, d, n); urlbuf[n] = 0;
    int status = 0;
    http_get(urlbuf, resp, sizeof resp, &status, location, sizeof location);
}

/* The cookie jar: a Set-Cookie value straight off the wire, then the header
 * built back out of it. */
static char ckbuf[4096];
static void t_cookie(const uint8_t *d, uint32_t n) {
    if (n > sizeof ckbuf - 1) n = sizeof ckbuf - 1;
    memcpy(ckbuf, d, n); ckbuf[n] = 0;
    http_cookie_set("fuzz.example", ckbuf);
    char out[1024];
    http_cookie_get("fuzz.example", out, sizeof out);
    http_cookie_get("other.example", out, sizeof out);
}

/* TLS, from the first record to wherever it gives up. Covers the record
 * layer, both handshake paths, and -- on the 1.3 path -- the certificate
 * parser, which runs before anything has been verified. */
static void t_tls(const uint8_t *d, uint32_t n) {
    wire_set(d, n);
    struct tls_conn *c = tls_connect(0x0A000202, 443, "fuzz.example", 200);
    if (!c) return;
    char buf[1024];
    tls_recv(c, buf, sizeof buf, 10);
    tls_send(c, "GET / HTTP/1.0\r\n\r\n", 18);
    tls_close(c);
}

/* Same, through http_get, so the HTTP parser reads a TLS-framed body. */
static void t_https(const uint8_t *d, uint32_t n) {
    wire_set(d, n);
    int status = 0;
    http_get("https://fuzz.example/page", resp, sizeof resp, &status, location, sizeof location);
}

/* The certificate chain on its own: the input is one DER certificate, wrapped
 * in the TLS 1.3 Certificate framing the verifier expects. This is the DER
 * parser, the SAN matching and the date arithmetic. */
static uint8_t certmsg[128 * 1024];
static uint32_t wrap_certmsg(const uint8_t *der, uint32_t n) {
    if (n > sizeof certmsg - 16) n = sizeof certmsg - 16;
    uint32_t p = 0;
    certmsg[p++] = 0;                                    /* request context: empty */
    uint32_t list = n + 5;                               /* 3 len + cert + 2 ext len */
    certmsg[p++] = (uint8_t)(list >> 16); certmsg[p++] = (uint8_t)(list >> 8); certmsg[p++] = (uint8_t)list;
    certmsg[p++] = (uint8_t)(n >> 16); certmsg[p++] = (uint8_t)(n >> 8); certmsg[p++] = (uint8_t)n;
    memcpy(certmsg + p, der, n); p += n;
    certmsg[p++] = 0; certmsg[p++] = 0;                  /* no extensions */
    return p;
}

static void t_cert(const uint8_t *d, uint32_t n) {
    uint32_t len = wrap_certmsg(d, n);
    uint8_t spki[1024]; uint32_t spkilen = 0;
    tls_verify_chain(certmsg, len, "example.com", spki, &spkilen, sizeof spki);
}

/* And the framing itself, so a certificate_list length that disagrees with the
 * message is fuzzed rather than always built correctly. */
static void t_certmsg(const uint8_t *d, uint32_t n) {
    uint8_t spki[1024]; uint32_t spkilen = 0;
    tls_verify_chain(d, n, "example.com", spki, &spkilen, sizeof spki);
}

/* CertificateVerify: a public key, a signed blob and a signature, all three
 * cut out of the input so the mutator moves the boundaries too. */
static void t_certverify(const uint8_t *d, uint32_t n) {
    if (n < 8) return;
    uint32_t a = 1 + ((uint32_t)d[0] << 4 | d[1]) % (n - 2);
    uint32_t b = a + 1 + ((uint32_t)d[2] << 4 | d[3]) % (n - a - 1);
    static const uint16_t algs[] = { 0x0401, 0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0000 };
    x509_verify_certverify(d, a, algs[d[4] % (sizeof algs / sizeof *algs)],
                           d + a, b - a, d + b, n - b);
}

/* gzip and raw DEFLATE. The body of any page that arrives compressed goes
 * through these, and a decompressor is a length field being believed. */
static uint8_t inflated[512 * 1024];
static void t_gzip(const uint8_t *d, uint32_t n) {
    uint32_t out = 0;
    gunzip(d, n, inflated, sizeof inflated, &out);
}
static void t_inflate(const uint8_t *d, uint32_t n) {
    uint32_t out = 0;
    inflate_raw(d, n, inflated, sizeof inflated, &out);
}

/* ---------------------------------------------------------------- main ----*/

/* A throwaway certificate, made once with
 *   openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
 *       -keyout /dev/null -outform DER -out cert.der -days 3650 -nodes \
 *       -subj /CN=example.com -addext subjectAltName=DNS:example.com,DNS:*.example.com
 * and pasted in as hex. The private key was thrown away and is not anywhere:
 * nothing here verifies a signature this certificate made, only that the
 * parser survives reading it and every mutation of it. Its dates are why
 * rtc_now() above answers 2026. */
static const char CERT_HEX[] =
#include "fuzzcert.h"
;

static uint8_t cert_der[4096];
static uint32_t cert_len;
static uint8_t certmsg_seed[8192];

static int unhex(const char *h, uint8_t *out, uint32_t cap, uint32_t *len) {
    uint32_t n = 0;
    for (const char *p = h; p[0] && p[1]; p += 2) {
        int hi = p[0] <= '9' ? p[0] - '0' : (p[0] | 32) - 'a' + 10;
        int lo = p[1] <= '9' ? p[1] - '0' : (p[1] | 32) - 'a' + 10;
        if (hi < 0 || hi > 15 || lo < 0 || lo > 15 || n >= cap) return -1;
        out[n++] = (uint8_t)(hi << 4 | lo);
    }
    *len = n;
    return 0;
}

/* Proof that the fake transport reaches the parsers. This is the check that
 * would have caught dns_resolve_addr's return convention being inverted, which
 * made every HTTP target here exit before parsing a byte and report a clean
 * run at two million inputs a second. */
/* RFC 7748 section 6.1 (the Diffie-Hellman example) and section 5.2 (a single
 * scalar multiplication), byte for byte. The numbers are the RFC's, which is
 * the point: they were not produced by this code. */
static int x25519_vectors(void) {
    struct { const char *scalar, *point, *want; } v[] = {
        /* 5.2: one multiplication with no special structure in either input */
        { "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
          "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
          "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552" },
        /* 6.1: Alice and Bob both arrive at the same secret, from either side */
        { "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",
          "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
          "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742" },
        { "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb",
          "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a",
          "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742" },
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof v / sizeof *v; i++) {
        uint8_t sc[32], pt[32], want[32], got[32]; uint32_t n;
        unhex(v[i].scalar, sc, sizeof sc, &n);
        unhex(v[i].point, pt, sizeof pt, &n);
        unhex(v[i].want, want, sizeof want, &n);
        x25519(got, sc, pt);
        if (memcmp(got, want, 32)) { fprintf(stderr, "  x25519 vector %u\n", i); bad++; }
    }
    /* And the public keys, which is the ladder against the base point. */
    uint8_t sc[32], want[32], got[32]; uint32_t n;
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", sc, sizeof sc, &n);
    unhex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", want, sizeof want, &n);
    x25519(got, sc, x25519_basepoint);
    if (memcmp(got, want, 32)) { fprintf(stderr, "  x25519 base point\n"); bad++; }
    return bad;
}

int fz_selftest(void) {
    fz_check_failures = 0;
    fz_check_failures += x25519_vectors();

    static const char ok200[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 28\r\n"
        "Set-Cookie: sid=abc; Path=/\r\n\r\n<html><body>hi</body></html>";
    wire_set((const uint8_t *)ok200, (uint32_t)(sizeof ok200 - 1));
    int status = 0;
    int n = http_get("http://fuzz.example/page", resp, sizeof resp, &status, location, sizeof location);
    FZ_CHECK(status == 200);
    FZ_CHECK(n == 28);
    FZ_CHECK(n > 0 && !memcmp(resp, "<html>", 6));
    FZ_CHECK(bytes_sent > 0);                       /* the request went out */

    char ck[128];
    FZ_CHECK(http_cookie_get("fuzz.example", ck, sizeof ck) > 0);

    static const char chunked[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
    wire_set((const uint8_t *)chunked, (uint32_t)(sizeof chunked - 1));
    n = http_get("http://fuzz.example/c", resp, sizeof resp, &status, NULL, 0);
    FZ_CHECK(n == 5 && !memcmp(resp, "hello", 5));

    static const char moved[] =
        "HTTP/1.1 301 Moved\r\nLocation: http://there.example/x\r\nContent-Length: 0\r\n\r\n";
    wire_set((const uint8_t *)moved, (uint32_t)(sizeof moved - 1));
    location[0] = 0;
    http_get("http://fuzz.example/r", resp, sizeof resp, &status, location, sizeof location);
    FZ_CHECK(status == 301);
    FZ_CHECK(!strcmp(location, "http://there.example/x"));

    /* gzip: the corpus entry is "hello" compressed. */
    uint32_t out = 0;
    FZ_CHECK(gunzip(gzip_corpus[0].d, gzip_corpus[0].n, inflated, sizeof inflated, &out) == 0);
    FZ_CHECK(out == 5 && !memcmp(inflated, "hello", 5));

    /* The certificate parser: the built-in certificate names example.com and
     * nothing else, so one hostname has to be accepted where the other is
     * refused -- which only happens if the DER was actually walked. */
    uint32_t ml = wrap_certmsg(cert_der, cert_len);
    uint8_t spki[1024]; uint32_t spkilen = 0;
    int good = tls_verify_chain(certmsg, ml, "example.com", spki, &spkilen, sizeof spki);
    int bad  = tls_verify_chain(certmsg, ml, "attacker.example", spki, &spkilen, sizeof spki);
    FZ_CHECK(good == 0);
    FZ_CHECK(bad != 0);
    FZ_CHECK(spkilen > 0);

    /* TLS: the handshake will not complete against a canned ServerHello, but
     * it has to get far enough to allocate a connection and read records. */
    uint64_t before = fz_alloc_calls;
    wire_set(tls_corpus[0].d, tls_corpus[0].n);
    struct tls_conn *tc = tls_connect(0x0A000202, 443, "fuzz.example", 200);
    if (tc) tls_close(tc);
    FZ_CHECK(fz_alloc_calls > before);
    FZ_CHECK(srv_pos > 0);                          /* it read the flight */

    return fz_check_failures ? -1 : 0;
}

int main(int argc, char **argv) {
    if (unhex(CERT_HEX, cert_der, sizeof cert_der, &cert_len) < 0) {
        fprintf(stderr, "the built-in certificate is not hex\n");
        return 2;
    }
    cert_corpus[0].d = cert_der; cert_corpus[0].n = cert_len;
    cert_corpus[1].d = cert_der; cert_corpus[1].n = cert_len / 2;   /* a truncated one */

    uint32_t ml = wrap_certmsg(cert_der, cert_len);
    memcpy(certmsg_seed, certmsg, ml);
    certmsg_corpus[0].d = certmsg_seed; certmsg_corpus[0].n = ml;
    certmsg_corpus[1].d = certmsg_seed; certmsg_corpus[1].n = 4;

    static const fz_target targets[] = {
        { "http",       t_http,       http_corpus,    (int)(sizeof http_corpus   / sizeof *http_corpus),   0 },
        { "keepalive",  t_keepalive,  http_corpus,    (int)(sizeof http_corpus   / sizeof *http_corpus),   0 },
        { "url",        t_url,        url_corpus,     (int)(sizeof url_corpus    / sizeof *url_corpus),    0 },
        { "cookie",     t_cookie,     cookie_corpus,  (int)(sizeof cookie_corpus / sizeof *cookie_corpus), 0 },
        { "tls",        t_tls,        tls_corpus,     (int)(sizeof tls_corpus    / sizeof *tls_corpus),    1 },
        { "https",      t_https,      tls_corpus,     (int)(sizeof tls_corpus    / sizeof *tls_corpus),    1 },
        { "cert",       t_cert,       cert_corpus,    2, 0 },
        { "certmsg",    t_certmsg,    certmsg_corpus, 2, 0 },
        { "certverify", t_certverify, cert_corpus,    2, 0 },
        { "gzip",       t_gzip,       gzip_corpus,    (int)(sizeof gzip_corpus   / sizeof *gzip_corpus),   0 },
        { "inflate",    t_inflate,    gzip_corpus,    (int)(sizeof gzip_corpus   / sizeof *gzip_corpus),   0 },
    };
    int rc = fz_main(argc, argv, targets, (int)(sizeof targets / sizeof *targets));
    if (kmsgs > (1ull << 40)) fprintf(stderr, "%llu\n", (unsigned long long)kmsgs);
    return rc;
}
