#include <stdint.h>
#include "ssh.h"
#include "net.h"
#include "netif.h"
#include "crypto.h"
#include "users.h"
#include "fs.h"
#include "console.h"
#include "shell.h"
#include "keyboard.h"
#include "kheap.h"
#include "pit.h"
#include "string.h"
#include "kprintf.h"
#include <stdio.h>

/* SSH-2, server and client. See include/ssh.h for the algorithm choices.
 *
 * Layering follows the RFCs: binary packet protocol (4253), then user auth
 * (4252), then connection-protocol channels (4254). Everything here blocks
 * and polls, same as the rest of the stack -- a wait spins netif_poll_all()
 * exactly like tcp_recv does.
 *
 * The shell a session gets is the real one: shell_exec_line() with kputc
 * redirected into the channel via console_set_sink(), the same hook the GUI
 * terminal uses. No command needs to know it's being run over the network.
 *
 * No publickey auth -- password only, same as the account table itself only
 * knows passwords. Would want to fix that before this ever faced the internet
 * for real. */

#define SSH_VERSION_STR   "SSH-2.0-BoltOS_1.0"
#define MAXPKT            35000
#define BLOCK             16          /* AES block; also the padding multiple  */
#define MACLEN            32          /* hmac-sha2-256                          */
#define IOTIMEOUT         30000

/* Message numbers (RFC 4250 section 4.1). */
#define MSG_DISCONNECT        1
#define MSG_IGNORE            2
#define MSG_UNIMPLEMENTED     3
#define MSG_DEBUG             4
#define MSG_SERVICE_REQUEST   5
#define MSG_SERVICE_ACCEPT    6
#define MSG_KEXINIT          20
#define MSG_NEWKEYS          21
#define MSG_KEX_ECDH_INIT    30
#define MSG_KEX_ECDH_REPLY   31
#define MSG_USERAUTH_REQUEST 50
#define MSG_USERAUTH_FAILURE 51
#define MSG_USERAUTH_SUCCESS 52
#define MSG_GLOBAL_REQUEST   80
#define MSG_REQUEST_FAILURE  82
#define MSG_CHANNEL_OPEN     90
#define MSG_CHANNEL_OPEN_CONFIRMATION 91
#define MSG_CHANNEL_OPEN_FAILURE      92
#define MSG_CHANNEL_WINDOW_ADJUST     93
#define MSG_CHANNEL_DATA     94
#define MSG_CHANNEL_EOF      96
#define MSG_CHANNEL_CLOSE    97
#define MSG_CHANNEL_REQUEST  98
#define MSG_CHANNEL_SUCCESS  99
#define MSG_CHANNEL_FAILURE 100

/* ------------------------------------------------------------------ buffers */
/* A growable byte buffer. SSH is all length-prefixed fields, so nearly every
 * routine here is "append some fields, then take the length". */
typedef struct {
    uint8_t *b;
    uint32_t len, cap;
    int      oom;          /* sticky: one check at the end beats one per put */
} buf;

static void buf_init(buf *o, uint32_t cap) {
    o->b = (uint8_t *)kmalloc(cap);
    o->len = 0; o->cap = o->b ? cap : 0; o->oom = o->b ? 0 : 1;
}
static void buf_free(buf *o) { if (o->b) kfree(o->b); o->b = 0; o->len = o->cap = 0; }

static void buf_need(buf *o, uint32_t extra) {
    if (o->oom || o->len + extra <= o->cap) return;
    uint32_t ncap = o->cap ? o->cap * 2 : 256;
    while (ncap < o->len + extra) ncap *= 2;
    uint8_t *nb = (uint8_t *)kmalloc(ncap);
    if (!nb) { o->oom = 1; return; }
    memcpy(nb, o->b, o->len);
    kfree(o->b);
    o->b = nb; o->cap = ncap;
}
static void put_raw(buf *o, const void *p, uint32_t n) {
    buf_need(o, n); if (o->oom) return;
    memcpy(o->b + o->len, p, n); o->len += n;
}
static void put_u8(buf *o, uint8_t v)  { put_raw(o, &v, 1); }
static void put_u32(buf *o, uint32_t v) {
    uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    put_raw(o, t, 4);
}
static void put_str(buf *o, const void *p, uint32_t n) { put_u32(o, n); put_raw(o, p, n); }
static void put_cstr(buf *o, const char *s) { put_str(o, s, (uint32_t)strlen(s)); }

/* An SSH mpint: two's-complement big-endian, no redundant leading bytes, and a
 * leading zero when the top bit would otherwise read as a sign. Getting this
 * wrong makes our exchange hash disagree with the peer's, and the handshake
 * then fails with nothing to see, so it gets its own function. */
static void put_mpint(buf *o, const uint8_t *v, uint32_t n) {
    uint32_t i = 0;
    while (i < n && v[i] == 0) i++;              /* strip leading zeros */
    if (i == n) { put_u32(o, 0); return; }       /* the value is zero   */
    int pad = (v[i] & 0x80) ? 1 : 0;
    put_u32(o, (n - i) + (uint32_t)pad);
    if (pad) put_u8(o, 0);
    put_raw(o, v + i, n - i);
}

/* ---------------------------------------------------------------- readers --*/
typedef struct { const uint8_t *b; uint32_t len, pos; int bad; } rdr;

static void rd_init(rdr *r, const uint8_t *b, uint32_t len) {
    r->b = b; r->len = len; r->pos = 0; r->bad = 0;
}
static uint8_t rd_u8(rdr *r) {
    if (r->pos + 1 > r->len) { r->bad = 1; return 0; }
    return r->b[r->pos++];
}
static uint32_t rd_u32(rdr *r) {
    if (r->pos + 4 > r->len) { r->bad = 1; return 0; }
    uint32_t v = ((uint32_t)r->b[r->pos] << 24) | ((uint32_t)r->b[r->pos+1] << 16) |
                 ((uint32_t)r->b[r->pos+2] << 8) | r->b[r->pos+3];
    r->pos += 4;
    return v;
}
/* Points into the packet, so the result must not outlive it. */
static const uint8_t *rd_str(rdr *r, uint32_t *out_len) {
    uint32_t n = rd_u32(r);
    if (r->bad || n > r->len - r->pos) { r->bad = 1; *out_len = 0; return r->b; }
    const uint8_t *p = r->b + r->pos;
    r->pos += n;
    *out_len = n;
    return p;
}
static void rd_cstr(rdr *r, char *out, uint32_t cap) {
    uint32_t n; const uint8_t *p = rd_str(r, &n);
    if (n > cap - 1) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

/* =========================== AES-128-CTR ================================= */
/* The counter is the whole 16-byte block, incremented big-endian, and the
 * keystream runs continuously across packets in one direction. */
typedef struct { aes128_ctx k; uint8_t ctr[16]; } ctr_state;

static void ctr_init(ctr_state *s, const uint8_t key[16], const uint8_t iv[16]) {
    aes128_init(&s->k, key);
    memcpy(s->ctr, iv, 16);
}
static void ctr_bump(uint8_t c[16]) {
    for (int i = 15; i >= 0; i--) if (++c[i]) break;
}
static void ctr_xor(ctr_state *s, uint8_t *data, uint32_t len) {
    uint8_t ks[16];
    while (len >= 16) {
        aes128_encrypt(&s->k, s->ctr, ks);
        ctr_bump(s->ctr);
        for (int i = 0; i < 16; i++) data[i] ^= ks[i];
        data += 16; len -= 16;
    }
    if (len) {                                   /* only if a peer misframes */
        aes128_encrypt(&s->k, s->ctr, ks);
        ctr_bump(s->ctr);
        for (uint32_t i = 0; i < len; i++) data[i] ^= ks[i];
    }
}

/* ========================== connection state ============================= */
typedef struct {
    struct tcp_conn *tcp;
    uint32_t  seq_in, seq_out;
    int       encrypted;
    ctr_state enc, dec;
    uint8_t   mac_out[32], mac_in[32];
    uint8_t   session_id[32];
    char      peer_version[256];
    int       is_server;

    uint8_t   pkt[MAXPKT];     /* the current inbound packet's payload */
    uint32_t  pktlen;
} ssh_conn;

/* ------------------------------------------------------------ raw stream ---*/
/* Read exactly n bytes. tcp_recv returns whatever has arrived, so everything
 * above this can be written as though the stream were already framed. */
static int rx_exact(ssh_conn *s, uint8_t *out, uint32_t n, uint32_t timeout_ms) {
    uint32_t got = 0;
    while (got < n) {
        int r = tcp_recv(s->tcp, out + got, n - got, timeout_ms);
        if (r <= 0) return -1;
        got += (uint32_t)r;
    }
    return 0;
}
static int tx_all(ssh_conn *s, const uint8_t *p, uint32_t n) {
    return tcp_send(s->tcp, p, n) < 0 ? -1 : 0;
}

/* ------------------------------------------------------------- packet i/o --*/
/* Before NEWKEYS a packet is plaintext with no MAC; after it, the packet is
 * encrypted and an HMAC over (sequence number || plaintext) is appended. */
static int pkt_send(ssh_conn *s, const uint8_t *payload, uint32_t plen) {
    uint32_t blk = s->encrypted ? BLOCK : 8;
    uint32_t base = 4 + 1 + plen;
    uint32_t pad  = blk - (base % blk);
    if (pad < 4) pad += blk;

    uint32_t total = base + pad;
    uint8_t *p = (uint8_t *)kmalloc(total + MACLEN);
    if (!p) return -1;

    uint32_t plen_field = 1 + plen + pad;
    p[0] = (uint8_t)(plen_field >> 24); p[1] = (uint8_t)(plen_field >> 16);
    p[2] = (uint8_t)(plen_field >> 8);  p[3] = (uint8_t)plen_field;
    p[4] = (uint8_t)pad;
    memcpy(p + 5, payload, plen);
    crypto_rand_bytes(p + 5 + plen, pad);

    uint32_t out = total;
    if (s->encrypted) {
        uint8_t macbuf[32];
        uint8_t *m = (uint8_t *)kmalloc(4 + total);
        if (!m) { kfree(p); return -1; }
        m[0] = (uint8_t)(s->seq_out >> 24); m[1] = (uint8_t)(s->seq_out >> 16);
        m[2] = (uint8_t)(s->seq_out >> 8);  m[3] = (uint8_t)s->seq_out;
        memcpy(m + 4, p, total);
        hmac_sha256(s->mac_out, 32, m, 4 + total, macbuf);  /* MAC the plaintext */
        kfree(m);
        ctr_xor(&s->enc, p, total);
        memcpy(p + total, macbuf, MACLEN);
        out = total + MACLEN;
    }
    s->seq_out++;
    int rc = tx_all(s, p, out);
    kfree(p);
    return rc;
}

static int pkt_recv(ssh_conn *s, uint32_t timeout_ms) {
    uint32_t headlen = s->encrypted ? BLOCK : 4;
    uint8_t plain_head[BLOCK];
    if (rx_exact(s, plain_head, headlen, timeout_ms) != 0) return -1;
    if (s->encrypted) ctr_xor(&s->dec, plain_head, BLOCK);

    uint32_t plen_field = ((uint32_t)plain_head[0] << 24) | ((uint32_t)plain_head[1] << 16) |
                          ((uint32_t)plain_head[2] << 8) | plain_head[3];
    if (plen_field < 8 || plen_field > MAXPKT - 8) return -1;

    uint32_t total = 4 + plen_field;
    uint32_t have  = headlen;
    if (total < have) return -1;

    uint8_t *full = (uint8_t *)kmalloc(total);
    if (!full) return -1;
    memcpy(full, plain_head, have);
    if (total > have) {
        if (rx_exact(s, full + have, total - have, timeout_ms) != 0) { kfree(full); return -1; }
        if (s->encrypted) ctr_xor(&s->dec, full + have, total - have);
    }

    if (s->encrypted) {
        uint8_t mac[MACLEN];
        if (rx_exact(s, mac, MACLEN, timeout_ms) != 0) { kfree(full); return -1; }
        uint8_t *m = (uint8_t *)kmalloc(4 + total);
        if (!m) { kfree(full); return -1; }
        m[0] = (uint8_t)(s->seq_in >> 24); m[1] = (uint8_t)(s->seq_in >> 16);
        m[2] = (uint8_t)(s->seq_in >> 8);  m[3] = (uint8_t)s->seq_in;
        memcpy(m + 4, full, total);
        uint8_t want[32];
        hmac_sha256(s->mac_in, 32, m, 4 + total, want);
        kfree(m);
        /* Constant time: a check that returns early leaks where it failed. */
        uint8_t diff = 0;
        for (int i = 0; i < MACLEN; i++) diff |= (uint8_t)(want[i] ^ mac[i]);
        if (diff) { kfree(full); return -1; }
    }

    uint8_t pad = full[4];
    if ((uint32_t)pad + 1 > plen_field) { kfree(full); return -1; }
    s->pktlen = plen_field - pad - 1;
    if (s->pktlen > MAXPKT) { kfree(full); return -1; }
    memcpy(s->pkt, full + 5, s->pktlen);
    kfree(full);
    s->seq_in++;
    return 0;
}

static void send_disconnect(ssh_conn *s, uint32_t reason, const char *msg) {
    buf o; buf_init(&o, 128);
    put_u8(&o, MSG_DISCONNECT);
    put_u32(&o, reason);
    put_cstr(&o, msg);
    put_cstr(&o, "");
    if (!o.oom) pkt_send(s, o.b, o.len);
    buf_free(&o);
}

/* ============================== host key ================================= */
static uint8_t hk_priv[32];
static uint8_t hk_pub[65];
static int     hk_loaded;

static void hostkey_blob(buf *o) {
    put_cstr(o, "ecdsa-sha2-nistp256");
    put_cstr(o, "nistp256");
    put_str(o, hk_pub, 65);
}

int ssh_hostkey_load(void) {
    if (hk_loaded) return 0;

    fs_node *f = fs_lookup(SSH_HOSTKEY_PATH);
    if (f && !f->is_dir && f->data && f->size == 32) {
        memcpy(hk_priv, f->data, 32);
        p256_pub_from_priv(hk_priv, hk_pub);
        hk_loaded = 1;
        return 0;
    }

    /* First use: make one and keep it. A host key that changed every boot would
     * warn the user on every connection, which teaches them to click through
     * the one warning that means somebody is intercepting them. */
    crypto_rand_stir(0, 0);
    crypto_rand_bytes(hk_priv, 32);
    p256_clamp_priv(hk_priv);
    p256_pub_from_priv(hk_priv, hk_pub);

    if (!fs_lookup("/etc"))     fs_create("/etc", 1);
    if (!fs_lookup("/etc/ssh")) fs_create("/etc/ssh", 1);
    f = fs_lookup(SSH_HOSTKEY_PATH);
    if (!f) f = fs_create(SSH_HOSTKEY_PATH, 0);
    if (!f) { kprintf("ssh: cannot create %s\n", SSH_HOSTKEY_PATH); return -1; }
    if (fs_write(f, hk_priv, 32) != 0) { kprintf("ssh: cannot write the host key\n"); return -1; }
    f->mode = 0600;                    /* a private key is not world-readable */
    f->uid  = 0;
    hk_loaded = 1;
    kprintf("ssh: generated a host key at %s\n", SSH_HOSTKEY_PATH);
    return 0;
}

static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint32_t b64(const uint8_t *in, uint32_t n, char *out, uint32_t cap) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i+1] << 8;
        if (i + 2 < n) v |= in[i+2];
        char q[4] = { b64tab[(v >> 18) & 63], b64tab[(v >> 12) & 63],
                      b64tab[(v >> 6) & 63],  b64tab[v & 63] };
        if (i + 1 >= n) q[2] = '=';
        if (i + 2 >= n) q[3] = '=';
        for (int k = 0; k < 4 && o < cap - 1; k++) out[o++] = q[k];
    }
    out[o] = 0;
    return o;
}

void ssh_hostkey_fingerprint(char *out, uint32_t cap) {
    if (ssh_hostkey_load() != 0) { out[0] = 0; return; }
    buf o; buf_init(&o, 128);
    hostkey_blob(&o);
    uint8_t h[32];
    sha256(o.b, o.len, h);
    buf_free(&o);

    char b[64];
    uint32_t n = b64(h, 32, b, sizeof b);
    while (n && b[n-1] == '=') b[--n] = 0;      /* OpenSSH strips the padding */
    snprintf(out, cap, "SHA256:%s", b);
}

/* ========================= version exchange ============================== */
/* Send our identification line and read the peer's. The peer may send banner
 * lines first; RFC 4253 says to ignore anything before the SSH- line. */
static int version_exchange(ssh_conn *s) {
    const char *v = SSH_VERSION_STR "\r\n";
    if (tx_all(s, (const uint8_t *)v, (uint32_t)strlen(v)) != 0) return -1;

    for (int line = 0; line < 32; line++) {
        uint32_t n = 0;
        char c;
        for (;;) {
            if (rx_exact(s, (uint8_t *)&c, 1, IOTIMEOUT) != 0) return -1;
            if (c == '\n') break;
            if (n < sizeof s->peer_version - 1) s->peer_version[n++] = c;
        }
        while (n && s->peer_version[n-1] == '\r') n--;
        s->peer_version[n] = 0;
        if (n >= 4 && !strncmp(s->peer_version, "SSH-", 4)) return 0;
    }
    return -1;
}

/* ============================== KEXINIT ================================== */
/* One algorithm per slot. Every extra name is more negotiation surface facing
 * a peer who has not authenticated yet, and this set is still in a current
 * OpenSSH's default proposal, so real clients need nothing relaxed. */
static void build_kexinit(buf *o) {
    uint8_t cookie[16];
    crypto_rand_bytes(cookie, 16);
    put_u8(o, MSG_KEXINIT);
    put_raw(o, cookie, 16);
    put_cstr(o, "curve25519-sha256");            /* kex                     */
    put_cstr(o, "ecdsa-sha2-nistp256");          /* host key                */
    put_cstr(o, "aes128-ctr");                   /* cipher c2s              */
    put_cstr(o, "aes128-ctr");                   /* cipher s2c              */
    put_cstr(o, "hmac-sha2-256");                /* mac c2s                 */
    put_cstr(o, "hmac-sha2-256");                /* mac s2c                 */
    put_cstr(o, "none");                         /* compression c2s         */
    put_cstr(o, "none");                         /* compression s2c         */
    put_cstr(o, "");                             /* languages c2s           */
    put_cstr(o, "");                             /* languages s2c           */
    put_u8(o, 0);                                /* first_kex_packet_follows*/
    put_u32(o, 0);                               /* reserved                */
}

/* Is `name` one of the comma-separated names in the list? */
static int name_in_list(const uint8_t *list, uint32_t len, const char *name) {
    uint32_t nl = (uint32_t)strlen(name), i = 0;
    while (i < len) {
        uint32_t j = i;
        while (j < len && list[j] != ',') j++;
        if (j - i == nl && !memcmp(list + i, name, nl)) return 1;
        i = j + 1;
    }
    return 0;
}

/* Check the peer offered everything we insist on, and say which one failed --
 * "no matching cipher" is a diagnosable message, a dropped connection is not. */
static int check_kexinit(const uint8_t *p, uint32_t len, const char **why) {
    rdr r; rd_init(&r, p, len);
    if (rd_u8(&r) != MSG_KEXINIT) { *why = "not a KEXINIT"; return -1; }
    r.pos += 16;                                  /* cookie */
    static const struct { const char *want, *label; } need[] = {
        { "curve25519-sha256",   "key exchange" },
        { "ecdsa-sha2-nistp256", "host key algorithm" },
        { "aes128-ctr",          "cipher (client to server)" },
        { "aes128-ctr",          "cipher (server to client)" },
        { "hmac-sha2-256",       "MAC (client to server)" },
        { "hmac-sha2-256",       "MAC (server to client)" },
        { "none",                "compression (client to server)" },
        { "none",                "compression (server to client)" },
    };
    for (int i = 0; i < 8; i++) {
        uint32_t n; const uint8_t *l = rd_str(&r, &n);
        if (r.bad) { *why = "truncated KEXINIT"; return -1; }
        if (!name_in_list(l, n, need[i].want)) { *why = need[i].label; return -1; }
    }
    return 0;
}

/* =========================== key derivation ============================== */
/* K1 = HASH(K || H || X || session_id), and longer keys extend by rehashing.
 * SHA-256 covers every length we need in one block, but the loop is written out
 * anyway so a future algorithm with a longer key does not silently truncate. */
static void derive(const buf *kmp, const uint8_t H[32], char letter,
                   const uint8_t sid[32], uint8_t *out, uint32_t outlen) {
    uint8_t block[32];
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, kmp->b, kmp->len);
    sha256_update(&c, H, 32);
    sha256_update(&c, &letter, 1);
    sha256_update(&c, sid, 32);
    sha256_final(&c, block);

    uint32_t have = outlen < 32 ? outlen : 32;
    memcpy(out, block, have);
    while (have < outlen) {
        sha256_init(&c);
        sha256_update(&c, kmp->b, kmp->len);
        sha256_update(&c, H, 32);
        sha256_update(&c, out, have);
        sha256_final(&c, block);
        uint32_t take = (outlen - have) < 32 ? (outlen - have) : 32;
        memcpy(out + have, block, take);
        have += take;
    }
}

/* ============================ server: key exchange ======================= */
/* H = HASH(V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K), every field a
 * length-prefixed string except K, which is an mpint. */
static int server_kex(ssh_conn *s, const buf *my_kexinit, const uint8_t *peer_kexinit,
                      uint32_t peer_kexinit_len) {
    if (pkt_recv(s, IOTIMEOUT) != 0) return -1;
    if (s->pktlen < 1 || s->pkt[0] != MSG_KEX_ECDH_INIT) return -1;

    rdr r; rd_init(&r, s->pkt, s->pktlen);
    rd_u8(&r);
    uint32_t qcl; const uint8_t *qc = rd_str(&r, &qcl);
    if (r.bad || qcl != 32) return -1;
    uint8_t Q_C[32]; memcpy(Q_C, qc, 32);

    /* Our ephemeral half. */
    uint8_t eph_priv[32], Q_S[32], K[32];
    crypto_rand_bytes(eph_priv, 32);
    static const uint8_t basepoint[32] = { 9 };
    x25519(Q_S, eph_priv, basepoint);
    x25519(K, eph_priv, Q_C);

    buf ks; buf_init(&ks, 128); hostkey_blob(&ks);
    buf kmp; buf_init(&kmp, 64); put_mpint(&kmp, K, 32);
    if (ks.oom || kmp.oom) { buf_free(&ks); buf_free(&kmp); return -1; }

    buf h; buf_init(&h, 1024);
    put_cstr(&h, s->peer_version);                /* V_C */
    put_cstr(&h, SSH_VERSION_STR);                /* V_S */
    put_str(&h, peer_kexinit, peer_kexinit_len);  /* I_C */
    put_str(&h, my_kexinit->b, my_kexinit->len);  /* I_S */
    put_str(&h, ks.b, ks.len);                    /* K_S */
    put_str(&h, Q_C, 32);
    put_str(&h, Q_S, 32);
    put_raw(&h, kmp.b, kmp.len);                  /* K, already an mpint */
    if (h.oom) { buf_free(&ks); buf_free(&kmp); buf_free(&h); return -1; }

    uint8_t H[32];
    sha256(h.b, h.len, H);
    buf_free(&h);

    /* Sign with the host key. ECDSA signs the digest of its message, and the
     * message here is H -- so what gets signed is SHA-256(H), not H. Signing H
     * directly produces a signature that verifies fine against itself and is
     * rejected by every real client, which is a genuinely confusing failure.
     *
     * A fresh nonce every time, and a retry if the arithmetic rejects one,
     * because a repeated nonce would publish the private key. */
    uint8_t Hh[32];
    sha256(H, 32, Hh);
    uint8_t sr[32], ss[32];
    int signed_ok = 0;
    for (int attempt = 0; attempt < 8 && !signed_ok; attempt++) {
        uint8_t nonce[32];
        crypto_rand_bytes(nonce, 32);
        if (p256_ecdsa_sign(Hh, 32, hk_priv, nonce, sr, ss) == 0) signed_ok = 1;
    }
    if (!signed_ok) { buf_free(&ks); buf_free(&kmp); return -1; }

    buf sigblob; buf_init(&sigblob, 128);
    {
        buf rs; buf_init(&rs, 96);
        put_mpint(&rs, sr, 32);
        put_mpint(&rs, ss, 32);
        put_cstr(&sigblob, "ecdsa-sha2-nistp256");
        put_str(&sigblob, rs.b, rs.len);
        buf_free(&rs);
    }

    buf reply; buf_init(&reply, 512);
    put_u8(&reply, MSG_KEX_ECDH_REPLY);
    put_str(&reply, ks.b, ks.len);
    put_str(&reply, Q_S, 32);
    put_str(&reply, sigblob.b, sigblob.len);
    int rc = (reply.oom || sigblob.oom) ? -1 : pkt_send(s, reply.b, reply.len);
    buf_free(&reply); buf_free(&sigblob); buf_free(&ks);
    if (rc != 0) { buf_free(&kmp); return -1; }

    /* NEWKEYS in both directions, then the keys take effect. */
    uint8_t nk = MSG_NEWKEYS;
    if (pkt_send(s, &nk, 1) != 0) { buf_free(&kmp); return -1; }
    if (pkt_recv(s, IOTIMEOUT) != 0 || s->pktlen < 1 || s->pkt[0] != MSG_NEWKEYS) {
        buf_free(&kmp); return -1;
    }

    /* The session id is the first exchange hash and never changes afterwards. */
    memcpy(s->session_id, H, 32);

    uint8_t iv_c2s[16], iv_s2c[16], key_c2s[16], key_s2c[16];
    derive(&kmp, H, 'A', s->session_id, iv_c2s,  16);
    derive(&kmp, H, 'B', s->session_id, iv_s2c,  16);
    derive(&kmp, H, 'C', s->session_id, key_c2s, 16);
    derive(&kmp, H, 'D', s->session_id, key_s2c, 16);
    derive(&kmp, H, 'E', s->session_id, s->mac_in,  32);
    derive(&kmp, H, 'F', s->session_id, s->mac_out, 32);
    buf_free(&kmp);

    ctr_init(&s->dec, key_c2s, iv_c2s);           /* we read what the client wrote */
    ctr_init(&s->enc, key_s2c, iv_s2c);
    s->encrypted = 1;
    return 0;
}

/* ============================ server: auth =============================== */
static int send_auth_failure(ssh_conn *s) {
    buf o; buf_init(&o, 64);
    put_u8(&o, MSG_USERAUTH_FAILURE);
    put_cstr(&o, "password");
    put_u8(&o, 0);
    int rc = o.oom ? -1 : pkt_send(s, o.b, o.len);
    buf_free(&o);
    return rc;
}

/* Returns 0 once a user has authenticated, with the name in `user_out` and the
 * uid in `uid_out`. Adopting that identity is the caller's job, so that it can
 * also put the previous one back -- see serve_one(). */
static int server_auth(ssh_conn *s, char *user_out, uint32_t cap, int *uid_out) {
    /* SSH_MSG_SERVICE_REQUEST for ssh-userauth comes first. */
    if (pkt_recv(s, IOTIMEOUT) != 0) return -1;
    if (s->pktlen < 1 || s->pkt[0] != MSG_SERVICE_REQUEST) return -1;
    {
        rdr r; rd_init(&r, s->pkt, s->pktlen); rd_u8(&r);
        char svc[64]; rd_cstr(&r, svc, sizeof svc);
        if (strcmp(svc, "ssh-userauth") != 0) return -1;
        buf o; buf_init(&o, 64);
        put_u8(&o, MSG_SERVICE_ACCEPT);
        put_cstr(&o, "ssh-userauth");
        int rc = o.oom ? -1 : pkt_send(s, o.b, o.len);
        buf_free(&o);
        if (rc != 0) return -1;
    }

    /* A handful of attempts, then the connection goes. The account table does
     * its own exponential backoff, so this only bounds the packet loop. */
    for (int tries = 0; tries < 6; tries++) {
        if (pkt_recv(s, IOTIMEOUT) != 0) return -1;
        if (s->pktlen < 1) return -1;
        if (s->pkt[0] == MSG_IGNORE || s->pkt[0] == MSG_DEBUG) { tries--; continue; }
        if (s->pkt[0] != MSG_USERAUTH_REQUEST) return -1;

        rdr r; rd_init(&r, s->pkt, s->pktlen);
        rd_u8(&r);
        char user[64], service[64], method[64];
        rd_cstr(&r, user, sizeof user);
        rd_cstr(&r, service, sizeof service);
        rd_cstr(&r, method, sizeof method);
        if (r.bad) return -1;

        if (!strcmp(method, "password")) {
            rd_u8(&r);                                  /* FALSE: not a change */
            char pw[128];
            rd_cstr(&r, pw, sizeof pw);
            if (r.bad) return -1;

            int uid = users_auth(user, pw);
            memset(pw, 0, sizeof pw);                   /* do not leave it lying about */
            if (uid >= 0) {
                uint8_t ok = MSG_USERAUTH_SUCCESS;
                if (pkt_send(s, &ok, 1) != 0) return -1;
                strncpy(user_out, user, cap - 1);
                user_out[cap - 1] = 0;
                *uid_out = uid;
                return 0;
            }
            kprintf("sshd: failed password for %s\n", user);
        }
        if (send_auth_failure(s) != 0) return -1;       /* "none" lands here too */
    }
    send_disconnect(s, 2, "too many authentication failures");
    return -1;
}

/* ========================== server: the session ========================== */
/* Output from a command is captured with console_set_sink and buffered, then
 * sent as one CHANNEL_DATA -- a packet per character would work but would put
 * an encrypt and a MAC behind every byte of a directory listing. */
static buf     *sink_target;
static uint32_t sink_dropped;

static void ssh_sink(char c) {
    if (!sink_target) return;
    if (sink_target->len > 1u << 20) { sink_dropped++; return; }   /* runaway output */
    /* The console emits bare newlines; a terminal on the other end wants CRLF. */
    if (c == '\n') put_u8(sink_target, '\r');
    put_u8(sink_target, (uint8_t)c);
}

static int send_channel_data(ssh_conn *s, uint32_t peer_ch, const uint8_t *d, uint32_t n) {
    /* Stay well inside the peer's initial window and any sane packet limit. */
    while (n) {
        uint32_t chunk = n > 16384 ? 16384 : n;
        buf o; buf_init(&o, chunk + 32);
        put_u8(&o, MSG_CHANNEL_DATA);
        put_u32(&o, peer_ch);
        put_str(&o, d, chunk);
        int rc = o.oom ? -1 : pkt_send(s, o.b, o.len);
        buf_free(&o);
        if (rc != 0) return -1;
        d += chunk; n -= chunk;
    }
    return 0;
}

static int send_str_data(ssh_conn *s, uint32_t peer_ch, const char *t) {
    return send_channel_data(s, peer_ch, (const uint8_t *)t, (uint32_t)strlen(t));
}

/* Run one shell command and return everything it printed. */
static int run_command(ssh_conn *s, uint32_t peer_ch, char *line) {
    buf out; buf_init(&out, 4096);
    if (out.oom) return -1;

    void (*prev)(char) = console_get_sink();
    sink_target = &out;
    sink_dropped = 0;
    console_set_sink(ssh_sink);
    shell_exec_line(line);
    console_set_sink(prev);
    sink_target = 0;

    int rc = 0;
    if (out.len) rc = send_channel_data(s, peer_ch, out.b, out.len);
    if (sink_dropped) rc |= send_str_data(s, peer_ch, "[output truncated]\r\n");
    buf_free(&out);
    return rc;
}

/* Close a channel the way a client expects: an exit status first, then EOF,
 * then CLOSE. Without the status the client has no idea how the command ended
 * and reports 255 -- indistinguishable from the connection having failed. */
static void close_channel(ssh_conn *s, uint32_t peer_ch, uint32_t status) {
    buf o; buf_init(&o, 64);
    put_u8(&o, MSG_CHANNEL_REQUEST);
    put_u32(&o, peer_ch);
    put_cstr(&o, "exit-status");
    put_u8(&o, 0);                       /* want_reply: never, for this one */
    put_u32(&o, status);
    if (!o.oom) pkt_send(s, o.b, o.len);
    buf_free(&o);

    buf e; buf_init(&e, 32);
    put_u8(&e, MSG_CHANNEL_EOF);   put_u32(&e, peer_ch);
    if (!e.oom) pkt_send(s, e.b, e.len);
    buf_free(&e);

    buf c; buf_init(&c, 32);
    put_u8(&c, MSG_CHANNEL_CLOSE); put_u32(&c, peer_ch);
    if (!c.oom) pkt_send(s, c.b, c.len);
    buf_free(&c);
}

static void send_prompt(ssh_conn *s, uint32_t peer_ch) {
    char path[FS_PATH_MAX], p[FS_PATH_MAX + 16];
    fs_abspath(fs_cwd(), path, sizeof path);      /* the same prompt the console shows */
    snprintf(p, sizeof p, "bolt:%s> ", path);
    send_str_data(s, peer_ch, p);
}

/* The interactive session. Line-oriented and echoing: the client is in raw mode
 * once it has a pty, so nothing else puts the characters on their screen. */
static int server_session(ssh_conn *s, const char *user) {
    uint32_t peer_ch = 0, my_ch = 0;
    int have_channel = 0, shell_started = 0, running = 1;
    char line[512];
    uint32_t linelen = 0;

    while (running) {
        if (pkt_recv(s, IOTIMEOUT) != 0) return -1;
        if (s->pktlen < 1) return -1;
        uint8_t t = s->pkt[0];
        rdr r; rd_init(&r, s->pkt, s->pktlen); rd_u8(&r);

        switch (t) {
        case MSG_IGNORE:
        case MSG_DEBUG:
        case MSG_CHANNEL_WINDOW_ADJUST:
            break;

        case MSG_DISCONNECT:
            return 0;

        case MSG_GLOBAL_REQUEST: {
            uint32_t n; rd_str(&r, &n);
            uint8_t want_reply = rd_u8(&r);
            if (want_reply) { uint8_t f = MSG_REQUEST_FAILURE; pkt_send(s, &f, 1); }
            break;
        }

        case MSG_CHANNEL_OPEN: {
            char kind[32]; rd_cstr(&r, kind, sizeof kind);
            uint32_t sender = rd_u32(&r);
            uint32_t iwin   = rd_u32(&r);
            uint32_t ipkt   = rd_u32(&r);
            (void)iwin; (void)ipkt;
            if (r.bad) return -1;
            if (strcmp(kind, "session") != 0 || have_channel) {
                buf o; buf_init(&o, 96);
                put_u8(&o, MSG_CHANNEL_OPEN_FAILURE);
                put_u32(&o, sender);
                put_u32(&o, 3);                       /* unknown channel type */
                put_cstr(&o, "only one session channel is supported");
                put_cstr(&o, "");
                if (!o.oom) pkt_send(s, o.b, o.len);
                buf_free(&o);
                break;
            }
            peer_ch = sender; my_ch = 0; have_channel = 1;
            buf o; buf_init(&o, 64);
            put_u8(&o, MSG_CHANNEL_OPEN_CONFIRMATION);
            put_u32(&o, peer_ch);
            put_u32(&o, my_ch);
            put_u32(&o, 1u << 20);                    /* our receive window */
            put_u32(&o, 16384);                       /* max packet         */
            int rc = o.oom ? -1 : pkt_send(s, o.b, o.len);
            buf_free(&o);
            if (rc != 0) return -1;
            break;
        }

        case MSG_CHANNEL_REQUEST: {
            rd_u32(&r);                               /* recipient channel */
            char req[64]; rd_cstr(&r, req, sizeof req);
            uint8_t want_reply = rd_u8(&r);
            if (r.bad) return -1;

            int ok = 0;
            if (!strcmp(req, "pty-req") || !strcmp(req, "env") ||
                !strcmp(req, "window-change")) {
                ok = 1;                               /* accepted and ignored */
            } else if (!strcmp(req, "shell")) {
                ok = 1; shell_started = 1;
            } else if (!strcmp(req, "exec")) {
                /* One command, then the channel closes -- what scp-style and
                 * scripted use looks like, and what the test drives. */
                char cmd[512]; rd_cstr(&r, cmd, sizeof cmd);
                ok = 1;
                if (want_reply) { uint8_t y = MSG_CHANNEL_SUCCESS;
                                  buf o; buf_init(&o, 16); put_u8(&o, y);
                                  put_u32(&o, peer_ch); pkt_send(s, o.b, o.len); buf_free(&o); }
                run_command(s, peer_ch, cmd);
                close_channel(s, peer_ch, 0);
                return 0;
            }

            if (want_reply) {
                buf o; buf_init(&o, 16);
                put_u8(&o, ok ? MSG_CHANNEL_SUCCESS : MSG_CHANNEL_FAILURE);
                put_u32(&o, peer_ch);
                if (!o.oom) pkt_send(s, o.b, o.len);
                buf_free(&o);
            }
            if (shell_started) {
                char hello[160];
                snprintf(hello, sizeof hello,
                         "BoltOS -- signed in as %s over SSH.\r\n"
                         "Type 'help' for commands, 'exit' to disconnect.\r\n", user);
                send_str_data(s, peer_ch, hello);
                send_prompt(s, peer_ch);
            }
            break;
        }

        case MSG_CHANNEL_DATA: {
            rd_u32(&r);
            uint32_t n; const uint8_t *d = rd_str(&r, &n);
            if (r.bad) return -1;
            for (uint32_t i = 0; i < n; i++) {
                char c = (char)d[i];
                if (c == '\r' || c == '\n') {
                    send_str_data(s, peer_ch, "\r\n");
                    line[linelen] = 0;
                    if (!strcmp(line, "exit") || !strcmp(line, "logout")) {
                        send_str_data(s, peer_ch, "bye\r\n");
                        running = 0;
                        break;
                    }
                    if (linelen) run_command(s, peer_ch, line);
                    linelen = 0;
                    send_prompt(s, peer_ch);
                } else if (c == 0x7F || c == '\b') {
                    if (linelen) { linelen--; send_str_data(s, peer_ch, "\b \b"); }
                } else if (c == 0x03) {               /* Ctrl+C: abandon the line */
                    linelen = 0;
                    send_str_data(s, peer_ch, "^C\r\n");
                    send_prompt(s, peer_ch);
                } else if (c == 0x04) {               /* Ctrl+D on an empty line */
                    if (!linelen) { send_str_data(s, peer_ch, "\r\n"); running = 0; break; }
                } else if ((unsigned char)c >= 32 && linelen < sizeof line - 1) {
                    line[linelen++] = c;
                    send_channel_data(s, peer_ch, (const uint8_t *)&c, 1);   /* echo */
                }
            }
            break;
        }

        case MSG_CHANNEL_EOF:
            break;
        case MSG_CHANNEL_CLOSE:
            running = 0;
            break;

        default: {
            /* Anything we do not implement gets an honest UNIMPLEMENTED rather
             * than silence, so the peer can decide what to do about it. */
            buf o; buf_init(&o, 16);
            put_u8(&o, MSG_UNIMPLEMENTED);
            put_u32(&o, s->seq_in - 1);
            if (!o.oom) pkt_send(s, o.b, o.len);
            buf_free(&o);
            break;
        }
        }
    }

    if (have_channel) close_channel(s, peer_ch, 0);
    return 0;
}

/* ============================ server: one connection ==================== */
static int serve_one(struct tcp_conn *tc, uint32_t peer_ip) {
    ssh_conn *s = (ssh_conn *)kmalloc(sizeof(ssh_conn));
    if (!s) return -1;
    memset(s, 0, sizeof(*s));
    s->tcp = tc;
    s->is_server = 1;

    kprintf("sshd: connection from %u.%u.%u.%u\n",
            (peer_ip >> 24) & 0xFF, (peer_ip >> 16) & 0xFF,
            (peer_ip >> 8) & 0xFF, peer_ip & 0xFF);

    int rc = -1;
    do {
        if (version_exchange(s) != 0) { kprintf("sshd: version exchange failed\n"); break; }

        buf my_kex; buf_init(&my_kex, 512);
        build_kexinit(&my_kex);
        if (my_kex.oom || pkt_send(s, my_kex.b, my_kex.len) != 0) { buf_free(&my_kex); break; }

        if (pkt_recv(s, IOTIMEOUT) != 0) { buf_free(&my_kex); break; }
        uint8_t *peer_kex = (uint8_t *)kmalloc(s->pktlen);
        if (!peer_kex) { buf_free(&my_kex); break; }
        uint32_t peer_kex_len = s->pktlen;
        memcpy(peer_kex, s->pkt, peer_kex_len);

        const char *why = "";
        if (check_kexinit(peer_kex, peer_kex_len, &why) != 0) {
            kprintf("sshd: no common %s\n", why);
            send_disconnect(s, 3, "no matching algorithm");
            kfree(peer_kex); buf_free(&my_kex); break;
        }
        int kexrc = server_kex(s, &my_kex, peer_kex, peer_kex_len);
        kfree(peer_kex); buf_free(&my_kex);
        if (kexrc != 0) { kprintf("sshd: key exchange failed\n"); break; }

        char user[64] = {0};
        int  uid = -1;
        if (server_auth(s, user, sizeof user, &uid) != 0) break;
        kprintf("sshd: %s authenticated\n", user);

        /* The identity is process-wide, and sshd runs on the console's own
         * thread -- so the session has to hand it back afterwards. Without
         * this, whoever signed in over SSH would still be the current user at
         * the console once the server stopped: an unprivileged user could
         * start sshd, have someone log in as root, and inherit root. */
        int caller_uid = users_current_uid();
        fs_node *caller_cwd = fs_cwd();
        users_login(uid);
        /* And into that account's home, the way signing in anywhere else
         * starts. The working directory is process-wide for the same reason
         * the identity is, so it goes back with it. */
        fs_node *home = fs_lookup(users_home());
        if (home && home->is_dir) fs_set_cwd(home);
        rc = server_session(s, user);
        users_login(caller_uid);
        if (caller_cwd) fs_set_cwd(caller_cwd);
        kprintf("sshd: session for %s ended\n", user);
    } while (0);

    kfree(s);
    return rc;
}

int ssh_server_run(uint16_t port) {
    if (ssh_hostkey_load() != 0) return -1;
    if (!netif_default()) { kprintf("sshd: no network interface is up\n"); return -1; }
    if (tcp_listen(port) != 0) { kprintf("sshd: cannot listen on port %u\n", port); return -1; }

    char fp[80];
    ssh_hostkey_fingerprint(fp, sizeof fp);
    kprintf("sshd: listening on %u.%u.%u.%u:%u  (any key stops)\n",
            (net_ip >> 24) & 0xFF, (net_ip >> 16) & 0xFF,
            (net_ip >> 8) & 0xFF, net_ip & 0xFF, port);
    kprintf("sshd: host key %s\n", fp);

    int served = 0;
    for (;;) {
        if (kbd_trygetc() >= 0) break;
        struct tcp_conn *c = tcp_accept(port, 250);
        if (!c) continue;
        uint32_t pip = tcp_peer_ip(c);
        serve_one(c, pip);
        tcp_close(c);
        served++;
    }
    tcp_unlisten(port);
    kprintf("sshd: stopped after %d connection%s\n", served, served == 1 ? "" : "s");
    return 0;
}


/* ============================== client =================================== *
 *  The other half. Everything above this -- the packet protocol, the key
 *  derivation, the buffers -- is written from neither end's point of view, so
 *  the client is the same machinery with the roles swapped: it sends the first
 *  KEXINIT, it verifies a signature instead of making one, and it reads what
 *  the server writes with the keys the server writes with.
 *
 *  The part a server does not have is deciding whether to trust the other end.
 *  A host key is checked against /etc/ssh/known_hosts, and a key that has
 *  changed for a host already in there stops the connection outright rather
 *  than asking: the question "the host key changed, continue?" has exactly one
 *  safe answer and asking it teaches people to press y.
 * ------------------------------------------------------------------------- */

#define MSG_USERAUTH_BANNER   53
#define MSG_CHANNEL_EXTENDED_DATA 95
#define SSH_KNOWN_HOSTS "/etc/ssh/known_hosts"

/* The SHA-256 fingerprint of any public key blob, in OpenSSH's format. */
static void blob_fingerprint(const uint8_t *blob, uint32_t len, char *out, uint32_t cap) {
    uint8_t h[32];
    sha256(blob, len, h);
    char b[64];
    uint32_t n = b64(h, 32, b, sizeof b);
    while (n && b[n-1] == '=') b[--n] = 0;
    snprintf(out, cap, "SHA256:%s", b);
}

/* An mpint as it arrives -- possibly with a leading zero the sender added to
 * keep it positive, possibly shorter than 32 bytes because leading zeros are
 * not sent -- into the fixed 32 bytes p256 wants. */
static int mpint_to_32(const uint8_t *p, uint32_t n, uint8_t out[32]) {
    while (n && p[0] == 0) { p++; n--; }
    if (n > 32) return -1;
    memset(out, 0, 32);
    memcpy(out + (32 - n), p, n);
    return 0;
}

/* ---- known hosts -------------------------------------------------------- *
 *  "host fingerprint" a line, which is not OpenSSH's format -- that stores the
 *  whole key -- but is what this can compare and what a person can read. */
static int known_hosts_find(const char *host, char *fp_out, uint32_t cap) {
    fs_node *f = fs_lookup(SSH_KNOWN_HOSTS);
    if (!f || !f->data) return 0;
    uint32_t i = 0, hl = (uint32_t)strlen(host);
    while (i < f->size) {
        uint32_t e = i;
        while (e < f->size && f->data[e] != '\n') e++;
        if (e - i > hl + 1 && !memcmp(f->data + i, host, hl) && f->data[i + hl] == ' ') {
            uint32_t o = 0, p = i + hl + 1;
            while (p < e && o < cap - 1) fp_out[o++] = (char)f->data[p++];
            fp_out[o] = 0;
            return 1;
        }
        i = e + 1;
    }
    return 0;
}

static void known_hosts_add(const char *host, const char *fp) {
    if (!fs_lookup("/etc/ssh")) {
        if (!fs_lookup("/etc")) fs_create("/etc", 1);
        fs_create("/etc/ssh", 1);
    }
    fs_node *f = fs_lookup(SSH_KNOWN_HOSTS);
    if (!f) f = fs_create(SSH_KNOWN_HOSTS, 0);
    if (!f) return;
    char line[160];
    snprintf(line, sizeof line, "%s %s\n", host, fp);
    fs_append(f, line, (uint32_t)strlen(line));
    f->mode = 0644;
}

/* Walk the file for `sshkey known`. 0 when there is no line i. */
int ssh_known_hosts_list(int idx, char *host, uint32_t hcap, char *fp, uint32_t fcap) {
    fs_node *f = fs_lookup(SSH_KNOWN_HOSTS);
    if (!f || !f->data) return 0;
    uint32_t i = 0;
    int n = 0;
    while (i < f->size) {
        uint32_t e = i;
        while (e < f->size && f->data[e] != '\n') e++;
        if (e > i) {
            if (n++ == idx) {
                uint32_t sp = i;
                while (sp < e && f->data[sp] != ' ') sp++;
                uint32_t o = 0;
                for (uint32_t p = i; p < sp && o < hcap - 1; p++) host[o++] = (char)f->data[p];
                host[o] = 0;
                o = 0;
                for (uint32_t p = sp + 1; p < e && o < fcap - 1; p++) fp[o++] = (char)f->data[p];
                fp[o] = 0;
                return 1;
            }
        }
        i = e + 1;
    }
    return 0;
}

/* Drop one host's line, so a machine that was legitimately rebuilt can be
 * connected to again without hand-editing the file. */
int ssh_known_hosts_forget(const char *host) {
    fs_node *f = fs_lookup(SSH_KNOWN_HOSTS);
    if (!f || !f->data) return -1;
    uint32_t hl = (uint32_t)strlen(host);
    char *out = (char *)kmalloc(f->size + 1);
    if (!out) return -1;
    uint32_t i = 0, n = 0;
    int dropped = 0;
    while (i < f->size) {
        uint32_t e = i;
        while (e < f->size && f->data[e] != '\n') e++;
        uint32_t len = e - i;
        int match = len > hl && !memcmp(f->data + i, host, hl) && f->data[i + hl] == ' ';
        if (match) dropped = 1;
        else if (len) { memcpy(out + n, f->data + i, len); n += len; out[n++] = '\n'; }
        i = e + 1;
    }
    int rc = dropped ? fs_write(f, out, n) : -1;
    kfree(out);
    return rc;
}

/* ---- key exchange ------------------------------------------------------- */
static int client_kex(ssh_conn *s, const buf *my_kexinit,
                      const uint8_t *peer_kexinit, uint32_t peer_kexinit_len,
                      const char *host) {
    uint8_t eph_priv[32], Q_C[32], Q_S[32], K[32];
    crypto_rand_bytes(eph_priv, 32);
    static const uint8_t basepoint[32] = { 9 };
    x25519(Q_C, eph_priv, basepoint);

    buf init; buf_init(&init, 64);
    put_u8(&init, MSG_KEX_ECDH_INIT);
    put_str(&init, Q_C, 32);
    int rc = init.oom ? -1 : pkt_send(s, init.b, init.len);
    buf_free(&init);
    if (rc != 0) return -1;

    if (pkt_recv(s, IOTIMEOUT) != 0) return -1;
    if (s->pktlen < 1 || s->pkt[0] != MSG_KEX_ECDH_REPLY) return -1;

    rdr r; rd_init(&r, s->pkt, s->pktlen);
    rd_u8(&r);
    uint32_t kslen;  const uint8_t *ks  = rd_str(&r, &kslen);
    uint32_t qslen;  const uint8_t *qs  = rd_str(&r, &qslen);
    uint32_t siglen; const uint8_t *sig = rd_str(&r, &siglen);
    if (r.bad || qslen != 32) return -1;

    /* Copy what is still needed: the shared secret goes into the same packet
     * buffer these point at the moment anything else is received. */
    uint8_t hostblob[256];
    if (kslen > sizeof hostblob) return -1;
    memcpy(hostblob, ks, kslen);
    memcpy(Q_S, qs, 32);
    uint8_t sigcopy[256];
    if (siglen > sizeof sigcopy) return -1;
    memcpy(sigcopy, sig, siglen);

    x25519(K, eph_priv, Q_S);

    buf kmp; buf_init(&kmp, 64); put_mpint(&kmp, K, 32);
    buf h; buf_init(&h, 1024);
    put_cstr(&h, SSH_VERSION_STR);                /* V_C -- ours, we are the client */
    put_cstr(&h, s->peer_version);                /* V_S */
    put_str(&h, my_kexinit->b, my_kexinit->len);  /* I_C */
    put_str(&h, peer_kexinit, peer_kexinit_len);  /* I_S */
    put_str(&h, hostblob, kslen);                 /* K_S */
    put_str(&h, Q_C, 32);
    put_str(&h, Q_S, 32);
    put_raw(&h, kmp.b, kmp.len);
    if (h.oom || kmp.oom) { buf_free(&h); buf_free(&kmp); return -1; }

    uint8_t H[32];
    sha256(h.b, h.len, H);
    buf_free(&h);

    /* The host key, out of its blob: name, curve name, then the point. */
    rdr hb; rd_init(&hb, hostblob, kslen);
    char alg[40]; rd_cstr(&hb, alg, sizeof alg);
    char curve[40]; rd_cstr(&hb, curve, sizeof curve);
    uint32_t publen; const uint8_t *pub = rd_str(&hb, &publen);
    if (hb.bad || strcmp(alg, "ecdsa-sha2-nistp256") || publen != 65 || pub[0] != 0x04) {
        kprintf("ssh: the server's host key is not an ecdsa-sha2-nistp256 point\n");
        buf_free(&kmp);
        return -1;
    }
    uint8_t hostpub[65];
    memcpy(hostpub, pub, 65);

    /* And the signature: name, then r and s as mpints inside a second string. */
    rdr sb; rd_init(&sb, sigcopy, siglen);
    char salg[40]; rd_cstr(&sb, salg, sizeof salg);
    uint32_t rslen; const uint8_t *rs = rd_str(&sb, &rslen);
    if (sb.bad || strcmp(salg, "ecdsa-sha2-nistp256")) { buf_free(&kmp); return -1; }
    rdr rsb; rd_init(&rsb, rs, rslen);
    uint32_t rl; const uint8_t *rv = rd_str(&rsb, &rl);
    uint32_t sl; const uint8_t *sv = rd_str(&rsb, &sl);
    uint8_t sr[32], ss2[32];
    if (rsb.bad || mpint_to_32(rv, rl, sr) != 0 || mpint_to_32(sv, sl, ss2) != 0) {
        buf_free(&kmp);
        return -1;
    }

    /* ECDSA signs the digest of its message, and the message is H. */
    uint8_t Hh[32];
    sha256(H, 32, Hh);
    if (p256_ecdsa_verify(Hh, 32, hostpub, sr, ss2) != 0) {
        kprintf("ssh: the server's signature does not verify -- not continuing\n");
        buf_free(&kmp);
        return -1;
    }

    /* Now that the key is proved to belong to whoever is on the other end,
     * whether it is the right whoever is a separate question. */
    char fp[80], known[80];
    blob_fingerprint(hostblob, kslen, fp, sizeof fp);
    if (known_hosts_find(host, known, sizeof known)) {
        if (strcmp(known, fp) != 0) {
            kprintf("ssh: the host key for %s has CHANGED.\n", host);
            kprintf("     known    %s\n", known);
            kprintf("     offered  %s\n", fp);
            kprintf("     Refusing to continue. Either the machine was rebuilt, in\n"
                    "     which case remove its line from %s, or somebody is\n"
                    "     between you and it.\n", SSH_KNOWN_HOSTS);
            buf_free(&kmp);
            return -1;
        }
    } else {
        kprintf("ssh: %s is new. Its key fingerprint is\n     %s\n", host, fp);
        kprintf("     Remembering it in %s.\n", SSH_KNOWN_HOSTS);
        known_hosts_add(host, fp);
    }

    uint8_t nk = MSG_NEWKEYS;
    if (pkt_send(s, &nk, 1) != 0) { buf_free(&kmp); return -1; }
    if (pkt_recv(s, IOTIMEOUT) != 0 || s->pktlen < 1 || s->pkt[0] != MSG_NEWKEYS) {
        buf_free(&kmp);
        return -1;
    }

    memcpy(s->session_id, H, 32);
    uint8_t iv_c2s[16], iv_s2c[16], key_c2s[16], key_s2c[16];
    derive(&kmp, H, 'A', s->session_id, iv_c2s,  16);
    derive(&kmp, H, 'B', s->session_id, iv_s2c,  16);
    derive(&kmp, H, 'C', s->session_id, key_c2s, 16);
    derive(&kmp, H, 'D', s->session_id, key_s2c, 16);
    derive(&kmp, H, 'E', s->session_id, s->mac_out, 32);   /* we are the client: */
    derive(&kmp, H, 'F', s->session_id, s->mac_in,  32);   /* c2s out, s2c in    */
    buf_free(&kmp);

    ctr_init(&s->enc, key_c2s, iv_c2s);
    ctr_init(&s->dec, key_s2c, iv_s2c);
    s->encrypted = 1;
    return 0;
}

/* ---- authentication ----------------------------------------------------- */
static int client_auth(ssh_conn *s, const char *user, const char *password) {
    buf o; buf_init(&o, 64);
    put_u8(&o, MSG_SERVICE_REQUEST);
    put_cstr(&o, "ssh-userauth");
    int rc = o.oom ? -1 : pkt_send(s, o.b, o.len);
    buf_free(&o);
    if (rc != 0) return -1;

    for (;;) {
        if (pkt_recv(s, IOTIMEOUT) != 0 || s->pktlen < 1) return -1;
        uint8_t t = s->pkt[0];
        if (t == MSG_SERVICE_ACCEPT) break;
        if (t == MSG_IGNORE || t == MSG_DEBUG) continue;
        if (t == MSG_DISCONNECT) return -1;
        return -1;
    }

    buf a; buf_init(&a, 128);
    put_u8(&a, MSG_USERAUTH_REQUEST);
    put_cstr(&a, user);
    put_cstr(&a, "ssh-connection");
    put_cstr(&a, "password");
    put_u8(&a, 0);                                /* not a change request */
    put_cstr(&a, password ? password : "");
    rc = a.oom ? -1 : pkt_send(s, a.b, a.len);
    buf_free(&a);
    if (rc != 0) return -1;

    for (;;) {
        if (pkt_recv(s, IOTIMEOUT) != 0 || s->pktlen < 1) return -1;
        uint8_t t = s->pkt[0];
        if (t == MSG_USERAUTH_SUCCESS) return 0;
        if (t == MSG_USERAUTH_FAILURE) {
            kprintf("ssh: authentication failed for '%s'\n", user);
            return -1;
        }
        if (t == MSG_USERAUTH_BANNER) {           /* the server has something to say */
            rdr r; rd_init(&r, s->pkt, s->pktlen);
            rd_u8(&r);
            uint32_t n; const uint8_t *m = rd_str(&r, &n);
            if (!r.bad) for (uint32_t i = 0; i < n; i++) kputc((char)m[i]);
            continue;
        }
        if (t == MSG_IGNORE || t == MSG_DEBUG) continue;
        return -1;
    }
}

/* ---- the session -------------------------------------------------------- */
/* One channel. `command` runs a single command and ends; without one the
 * session is a shell and the keyboard is forwarded to it until the far end
 * closes the channel or Ctrl+D is pressed. */
static int client_session(ssh_conn *s, const char *command) {
    const uint32_t local_ch = 0, window = 1u << 20, maxpkt = 32768;

    buf o; buf_init(&o, 64);
    put_u8(&o, MSG_CHANNEL_OPEN);
    put_cstr(&o, "session");
    put_u32(&o, local_ch);
    put_u32(&o, window);
    put_u32(&o, maxpkt);
    int rc = o.oom ? -1 : pkt_send(s, o.b, o.len);
    buf_free(&o);
    if (rc != 0) return -1;

    uint32_t peer_ch = 0;
    for (;;) {
        if (pkt_recv(s, IOTIMEOUT) != 0 || s->pktlen < 1) return -1;
        uint8_t t = s->pkt[0];
        if (t == MSG_CHANNEL_OPEN_CONFIRMATION) {
            rdr r; rd_init(&r, s->pkt, s->pktlen);
            rd_u8(&r); rd_u32(&r);
            peer_ch = rd_u32(&r);
            if (r.bad) return -1;
            break;
        }
        if (t == MSG_CHANNEL_OPEN_FAILURE) {
            kprintf("ssh: the server refused to open a session\n");
            return -1;
        }
        if (t == MSG_IGNORE || t == MSG_DEBUG || t == MSG_GLOBAL_REQUEST) continue;
        return -1;
    }

    buf req; buf_init(&req, 128);
    put_u8(&req, MSG_CHANNEL_REQUEST);
    put_u32(&req, peer_ch);
    if (command) {
        put_cstr(&req, "exec");
        put_u8(&req, 1);                          /* want a reply */
        put_cstr(&req, command);
    } else {
        put_cstr(&req, "shell");
        put_u8(&req, 1);
    }
    rc = req.oom ? -1 : pkt_send(s, req.b, req.len);
    buf_free(&req);
    if (rc != 0) return -1;

    int exit_status = 0, open = 1, sent_eof = 0;
    while (open) {
        /* Anything typed goes to the far end. Only in shell mode: a command
         * was given on our own command line, and its input is not the
         * keyboard. */
        if (!command) {
            int c;
            while ((c = kbd_trygetc()) >= 0) {
                if (c == 4) {                     /* Ctrl+D: close our side */
                    if (!sent_eof) {
                        buf e; buf_init(&e, 16);
                        put_u8(&e, MSG_CHANNEL_EOF);
                        put_u32(&e, peer_ch);
                        if (!e.oom) pkt_send(s, e.b, e.len);
                        buf_free(&e);
                        sent_eof = 1;
                    }
                    continue;
                }
                uint8_t ch = (uint8_t)c;
                kputc((char)c);                   /* local echo: the far end
                                                   * runs a line shell, not a
                                                   * pty, and echoes nothing */
                send_channel_data(s, peer_ch, &ch, 1);
            }
        }

        if (pkt_recv(s, 200) != 0) {
            if (!tcp_is_open(s->tcp)) break;
            continue;                             /* just nothing yet */
        }
        if (s->pktlen < 1) continue;
        rdr r; rd_init(&r, s->pkt, s->pktlen);
        uint8_t t = rd_u8(&r);

        switch (t) {
        case MSG_CHANNEL_DATA: {
            rd_u32(&r);
            uint32_t n; const uint8_t *d = rd_str(&r, &n);
            if (!r.bad) for (uint32_t i = 0; i < n; i++) kputc((char)d[i]);
            break;
        }
        case MSG_CHANNEL_EXTENDED_DATA: {
            rd_u32(&r); rd_u32(&r);               /* channel, data type code */
            uint32_t n; const uint8_t *d = rd_str(&r, &n);
            if (!r.bad) for (uint32_t i = 0; i < n; i++) kputc((char)d[i]);
            break;
        }
        case MSG_CHANNEL_REQUEST: {
            rd_u32(&r);
            char what[32]; rd_cstr(&r, what, sizeof what);
            rd_u8(&r);
            if (!r.bad && !strcmp(what, "exit-status")) exit_status = (int)rd_u32(&r);
            break;
        }
        case MSG_CHANNEL_EOF:
            break;
        case MSG_CHANNEL_CLOSE: {
            buf c; buf_init(&c, 16);
            put_u8(&c, MSG_CHANNEL_CLOSE);
            put_u32(&c, peer_ch);
            if (!c.oom) pkt_send(s, c.b, c.len);
            buf_free(&c);
            open = 0;
            break;
        }
        case MSG_DISCONNECT:
            open = 0;
            break;
        default:
            break;                                /* window adjust, success,
                                                   * failure: nothing to do */
        }
    }
    return exit_status;
}

int ssh_client_connect(const char *host, uint16_t port, const char *user,
                       const char *password, const char *command, int *status_out) {
    uint32_t ip;
    if (!dns_resolve(host, &ip, 4000)) {
        kprintf("ssh: cannot resolve '%s'\n", host);
        return -1;
    }

    struct tcp_conn *tc = tcp_connect(ip, port, 5000);
    if (!tc) {
        kprintf("ssh: cannot connect to %s port %u\n", host, (unsigned)port);
        return -1;
    }

    ssh_conn *s = (ssh_conn *)kmalloc(sizeof *s);
    if (!s) { tcp_close(tc); return -1; }
    memset(s, 0, sizeof *s);
    s->tcp = tc;
    s->is_server = 0;

    int rc = -1;
    buf mine; buf_init(&mine, 256);
    do {
        if (version_exchange(s) != 0) { kprintf("ssh: no SSH version from the server\n"); break; }

        build_kexinit(&mine);
        if (mine.oom || pkt_send(s, mine.b, mine.len) != 0) break;
        if (pkt_recv(s, IOTIMEOUT) != 0) break;

        const char *why = 0;
        if (check_kexinit(s->pkt, s->pktlen, &why) != 0) {
            kprintf("ssh: no shared %s with the server\n", why ? why : "algorithm");
            break;
        }
        static uint8_t peer_kexinit[4096];
        if (s->pktlen > sizeof peer_kexinit) break;
        uint32_t peer_len = s->pktlen;
        memcpy(peer_kexinit, s->pkt, peer_len);

        if (client_kex(s, &mine, peer_kexinit, peer_len, host) != 0) break;
        if (client_auth(s, user, password) != 0) break;
        int st = client_session(s, command);
        if (status_out) *status_out = st;
        rc = 0;
    } while (0);

    buf_free(&mine);
    if (rc != 0) send_disconnect(s, 2, "bye");
    tcp_close(tc);
    kfree(s);
    return rc;
}
