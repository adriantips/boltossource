#include <stdint.h>
#include "wpa.h"
#include "crypto.h"
#include "string.h"
#include "wifi.h"

/* WPA2-personal arithmetic: SHA-1 and its HMAC/PBKDF2, the AES inverse cipher
 * and the two things built on it (RFC 3394 key unwrap, CMAC), AES-CCM, and
 * then the 802.11 key hierarchy and CCMP framing on top.
 *
 * Nothing here touches hardware or global state, which is deliberate: every
 * function is a pure transform, so test/host/wpa.c compiles this exact file on
 * the host and checks it against the published vectors. A radio driver is a
 * thing you can only test by having the radio; this is a thing you can only
 * *fail* to test by choosing not to. */

/* ============================== SHA-1 ================================== */
static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(sha1_ctx *c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3], f = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t k, t;
        if      (i < 20) { t = (b & d) | (~b & e);            k = 0x5A827999; }
        else if (i < 40) { t = b ^ d ^ e;                     k = 0x6ED9EBA1; }
        else if (i < 60) { t = (b & d) | (b & e) | (d & e);   k = 0x8F1BBCDC; }
        else             { t = b ^ d ^ e;                     k = 0xCA62C1D6; }
        uint32_t tmp = rol(a, 5) + t + f + k + w[i];
        f = e; e = d; d = rol(b, 30); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;
}

void sha1_init(sha1_ctx *c) {
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
    c->total = 0; c->n = 0;
}

void sha1_update(sha1_ctx *c, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->total += len;
    while (len) {
        uint32_t take = 64 - c->n;
        if (take > len) take = len;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; len -= take;
        if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
    }
}

void sha1_final(sha1_ctx *c, uint8_t out[20]) {
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    static const uint8_t z[64] = { 0 };
    while (c->n != 56) sha1_update(c, z, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - i*8));
    sha1_update(c, l, 8);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)c->h[i];
    }
}

void sha1(const void *data, uint32_t len, uint8_t out[20]) {
    sha1_ctx c; sha1_init(&c); sha1_update(&c, data, len); sha1_final(&c, out);
}

void hmac_sha1(const uint8_t *key, uint32_t klen,
               const uint8_t *msg, uint32_t mlen, uint8_t out[20]) {
    uint8_t k[64], ip[64], op[64], inner[20];
    memset(k, 0, sizeof k);
    if (klen > 64) sha1(key, klen, k);
    else           memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5C; }

    sha1_ctx c;
    sha1_init(&c); sha1_update(&c, ip, 64); sha1_update(&c, msg, mlen);
    sha1_final(&c, inner);
    sha1_init(&c); sha1_update(&c, op, 64); sha1_update(&c, inner, 20);
    sha1_final(&c, out);
}

void pbkdf2_sha1(const void *pw, uint32_t pwlen,
                 const uint8_t *salt, uint32_t saltlen,
                 uint32_t rounds, uint8_t *out, uint32_t outlen) {
    uint8_t u[20], t[20];
    uint8_t block[128 + 4];
    uint32_t done = 0, idx = 1;
    if (saltlen > 128) saltlen = 128;         /* an SSID is 32; this is slack */

    while (done < outlen) {
        memcpy(block, salt, saltlen);
        block[saltlen]     = (uint8_t)(idx >> 24);
        block[saltlen + 1] = (uint8_t)(idx >> 16);
        block[saltlen + 2] = (uint8_t)(idx >> 8);
        block[saltlen + 3] = (uint8_t)idx;
        hmac_sha1((const uint8_t *)pw, pwlen, block, saltlen + 4, u);
        memcpy(t, u, 20);
        for (uint32_t r = 1; r < rounds; r++) {
            hmac_sha1((const uint8_t *)pw, pwlen, u, 20, u);
            for (int i = 0; i < 20; i++) t[i] ^= u[i];
        }
        uint32_t take = outlen - done;
        if (take > 20) take = 20;
        memcpy(out + done, t, take);
        done += take;
        idx++;
    }
}

/* =========================== AES inverse cipher ========================== */
/* Inverting net/crypto.c's forward S-box at first use costs 256 bytes and
 * saves carrying a second table that has to agree with the first one forever. */
static uint8_t inv_sbox[256];
static int     inv_ready;

static void build_inv_sbox(void) {
    for (int i = 0; i < 256; i++) inv_sbox[aes_sbox[i]] = (uint8_t)i;
    inv_ready = 1;
}

static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = (uint8_t)((a << 1) ^ ((a >> 7) * 0x1B));
        b >>= 1;
    }
    return r;
}

void aes128_decrypt(const aes128_ctx *c, const uint8_t in[16], uint8_t out[16]) {
    if (!inv_ready) build_inv_sbox();
    /* InvShiftRows: column c of row r comes from column (c + r) mod 4, which in
     * this column-major byte order is the forward table read backwards. */
    static const uint8_t isr[16] = {0,13,10,7,4,1,14,11,8,5,2,15,12,9,6,3};
    uint8_t s[16], t[16];
    const uint8_t *rk = c->rk;

    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[160 + i];
    for (int round = 9; round >= 0; round--) {
        for (int i = 0; i < 16; i++) t[i] = inv_sbox[s[isr[i]]];
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[round*16 + i];
        if (round != 0) {
            for (int col = 0; col < 4; col++) {
                uint8_t *p = s + col*4;
                uint8_t a0=p[0],a1=p[1],a2=p[2],a3=p[3];
                p[0] = (uint8_t)(gmul(a0,14) ^ gmul(a1,11) ^ gmul(a2,13) ^ gmul(a3,9));
                p[1] = (uint8_t)(gmul(a0,9)  ^ gmul(a1,14) ^ gmul(a2,11) ^ gmul(a3,13));
                p[2] = (uint8_t)(gmul(a0,13) ^ gmul(a1,9)  ^ gmul(a2,14) ^ gmul(a3,11));
                p[3] = (uint8_t)(gmul(a0,11) ^ gmul(a1,13) ^ gmul(a2,9)  ^ gmul(a3,14));
            }
        }
    }
    memcpy(out, s, 16);
}

/* ======================= AES key wrap (RFC 3394) ======================== */
static const uint8_t kw_iv[8] = { 0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6 };

void aes_key_wrap(const uint8_t kek[16], const uint8_t *in, uint32_t n, uint8_t *out) {
    aes128_ctx ks;
    aes128_init(&ks, kek);
    uint32_t blocks = n / 8;
    uint8_t a[8], b[16];
    memcpy(a, kw_iv, 8);
    memcpy(out + 8, in, n);
    for (uint32_t j = 0; j < 6; j++) {
        for (uint32_t i = 1; i <= blocks; i++) {
            memcpy(b, a, 8);
            memcpy(b + 8, out + i*8, 8);
            aes128_encrypt(&ks, b, b);
            uint64_t t = (uint64_t)blocks * j + i;
            memcpy(a, b, 8);
            for (int k = 0; k < 8; k++) a[7 - k] ^= (uint8_t)(t >> (k * 8));
            memcpy(out + i*8, b + 8, 8);
        }
    }
    memcpy(out, a, 8);
}

int aes_key_unwrap(const uint8_t kek[16], const uint8_t *in, uint32_t n, uint8_t *out) {
    if (n < 24 || (n % 8) != 0) return -1;
    aes128_ctx ks;
    aes128_init(&ks, kek);
    uint32_t blocks = n / 8 - 1;
    uint8_t a[8], b[16];
    memcpy(a, in, 8);
    memcpy(out, in + 8, blocks * 8);
    for (int j = 5; j >= 0; j--) {
        for (uint32_t i = blocks; i >= 1; i--) {
            uint64_t t = (uint64_t)blocks * (uint32_t)j + i;
            memcpy(b, a, 8);
            for (int k = 0; k < 8; k++) b[7 - k] ^= (uint8_t)(t >> (k * 8));
            memcpy(b + 8, out + (i-1)*8, 8);
            aes128_decrypt(&ks, b, b);
            memcpy(a, b, 8);
            memcpy(out + (i-1)*8, b + 8, 8);
        }
    }
    /* The whole point of the integrity value: a wrong KEK lands here. */
    int bad = 0;
    for (int i = 0; i < 8; i++) bad |= a[i] ^ kw_iv[i];
    return bad ? -1 : 0;
}

/* ============================= AES-CMAC ================================= */
static void cmac_shift(const uint8_t in[16], uint8_t out[16]) {
    uint8_t carry = 0;
    for (int i = 15; i >= 0; i--) {
        uint8_t c = (uint8_t)(in[i] >> 7);
        out[i] = (uint8_t)((in[i] << 1) | carry);
        carry = c;
    }
    if (in[0] & 0x80) out[15] ^= 0x87;
}

void aes_cmac(const uint8_t key[16], const uint8_t *msg, uint32_t len, uint8_t out[16]) {
    aes128_ctx ks;
    aes128_init(&ks, key);
    uint8_t l[16] = { 0 }, k1[16], k2[16];
    aes128_encrypt(&ks, l, l);
    cmac_shift(l, k1);
    cmac_shift(k1, k2);

    uint8_t x[16] = { 0 }, blk[16];
    uint32_t full = len ? (len - 1) / 16 : 0;        /* complete blocks before the last */
    for (uint32_t i = 0; i < full; i++) {
        for (int j = 0; j < 16; j++) blk[j] = x[j] ^ msg[i*16 + j];
        aes128_encrypt(&ks, blk, x);
    }
    uint32_t rest = len - full * 16;
    if (len && rest == 16) {
        for (int j = 0; j < 16; j++) blk[j] = msg[full*16 + j] ^ k1[j];
    } else {
        memset(blk, 0, 16);
        memcpy(blk, msg + full*16, rest);
        blk[rest] = 0x80;
        for (int j = 0; j < 16; j++) blk[j] ^= k2[j];
    }
    for (int j = 0; j < 16; j++) blk[j] ^= x[j];
    aes128_encrypt(&ks, blk, out);
}

/* ============================== AES-CCM ================================= */
/* L=2 and M=8 throughout, because that is what CCMP uses and generalising it
 * would mean writing code nothing on this machine calls. */
static void ccm_mac(const aes128_ctx *ks, const uint8_t nonce[13],
                    const uint8_t *aad, uint32_t aadlen,
                    const uint8_t *pt, uint32_t ptlen, uint8_t t[16]) {
    uint8_t b[16], x[16];
    b[0] = (uint8_t)((aadlen ? 0x40 : 0) | 0x18 | 0x01);   /* Adata | M' | L' */
    memcpy(b + 1, nonce, 13);
    b[14] = (uint8_t)(ptlen >> 8);
    b[15] = (uint8_t)ptlen;
    aes128_encrypt(ks, b, x);

    if (aadlen) {
        /* An 802.11 AAD is never near 2^16-2^8, so the two-byte length encoding
         * is the only one that can come up here. */
        memset(b, 0, 16);
        b[0] = (uint8_t)(aadlen >> 8);
        b[1] = (uint8_t)aadlen;
        uint32_t take = aadlen < 14 ? aadlen : 14;
        memcpy(b + 2, aad, take);
        uint32_t pos = take;
        for (int i = 0; i < 16; i++) b[i] ^= x[i];
        aes128_encrypt(ks, b, x);
        while (pos < aadlen) {
            memset(b, 0, 16);
            take = aadlen - pos < 16 ? aadlen - pos : 16;
            memcpy(b, aad + pos, take);
            pos += take;
            for (int i = 0; i < 16; i++) b[i] ^= x[i];
            aes128_encrypt(ks, b, x);
        }
    }
    for (uint32_t pos = 0; pos < ptlen; pos += 16) {
        memset(b, 0, 16);
        uint32_t take = ptlen - pos < 16 ? ptlen - pos : 16;
        memcpy(b, pt + pos, take);
        for (int i = 0; i < 16; i++) b[i] ^= x[i];
        aes128_encrypt(ks, b, x);
    }
    memcpy(t, x, 16);
}

static void ccm_ctr(const aes128_ctx *ks, const uint8_t nonce[13], uint16_t counter,
                    uint8_t s[16]) {
    uint8_t a[16];
    a[0] = 0x01;                                   /* flags: L' = 1 */
    memcpy(a + 1, nonce, 13);
    a[14] = (uint8_t)(counter >> 8);
    a[15] = (uint8_t)counter;
    aes128_encrypt(ks, a, s);
}

void aes_ccm_encrypt(const uint8_t key[16], const uint8_t nonce[13],
                     const uint8_t *aad, uint32_t aadlen,
                     const uint8_t *pt, uint32_t ptlen,
                     uint8_t *ct, uint8_t mic[CCM_MIC_LEN]) {
    aes128_ctx ks;
    aes128_init(&ks, key);
    uint8_t t[16], s[16];
    ccm_mac(&ks, nonce, aad, aadlen, pt, ptlen, t);
    ccm_ctr(&ks, nonce, 0, s);
    for (int i = 0; i < CCM_MIC_LEN; i++) mic[i] = (uint8_t)(t[i] ^ s[i]);
    for (uint32_t pos = 0; pos < ptlen; pos += 16) {
        ccm_ctr(&ks, nonce, (uint16_t)(pos / 16 + 1), s);
        uint32_t take = ptlen - pos < 16 ? ptlen - pos : 16;
        for (uint32_t i = 0; i < take; i++) ct[pos + i] = (uint8_t)(pt[pos + i] ^ s[i]);
    }
}

int aes_ccm_decrypt(const uint8_t key[16], const uint8_t nonce[13],
                    const uint8_t *aad, uint32_t aadlen,
                    const uint8_t *ct, uint32_t ctlen,
                    const uint8_t mic[CCM_MIC_LEN], uint8_t *pt) {
    aes128_ctx ks;
    aes128_init(&ks, key);
    uint8_t t[16], s[16];
    for (uint32_t pos = 0; pos < ctlen; pos += 16) {
        ccm_ctr(&ks, nonce, (uint16_t)(pos / 16 + 1), s);
        uint32_t take = ctlen - pos < 16 ? ctlen - pos : 16;
        for (uint32_t i = 0; i < take; i++) pt[pos + i] = (uint8_t)(ct[pos + i] ^ s[i]);
    }
    ccm_mac(&ks, nonce, aad, aadlen, pt, ctlen, t);
    ccm_ctr(&ks, nonce, 0, s);
    int bad = 0;
    for (int i = 0; i < CCM_MIC_LEN; i++) bad |= (t[i] ^ s[i]) ^ mic[i];
    return bad ? -1 : 0;
}

/* ========================== the key hierarchy =========================== */
void wpa_pmk(const char *passphrase, const char *ssid, uint8_t pmk[WPA_PMK_LEN]) {
    uint32_t plen = 0, slen = 0;
    while (passphrase[plen]) plen++;
    while (ssid[slen]) slen++;
    pbkdf2_sha1(passphrase, plen, (const uint8_t *)ssid, slen, 4096, pmk, WPA_PMK_LEN);
}

void wpa_prf(const uint8_t *key, uint32_t klen, const char *label,
             const uint8_t *data, uint32_t dlen, uint8_t *out, uint32_t outlen) {
    uint8_t buf[160], digest[20];
    uint32_t llen = 0;
    while (label[llen]) llen++;
    if (llen + 1 + dlen + 1 > sizeof buf) {
        /* Returning quietly would leave a key's worth of stack garbage in out,
         * and the caller would encrypt with it. Callers here pass about 76
         * bytes, so this is a "cannot happen" -- which is exactly the kind
         * that does. */
        memset(out, 0, outlen);
        return;
    }

    memcpy(buf, label, llen);
    buf[llen] = 0;
    memcpy(buf + llen + 1, data, dlen);
    uint32_t n = llen + 1 + dlen;

    uint32_t done = 0;
    for (uint8_t i = 0; done < outlen; i++) {
        buf[n] = i;
        hmac_sha1(key, klen, buf, n + 1, digest);
        uint32_t take = outlen - done;
        if (take > 20) take = 20;
        memcpy(out + done, digest, take);
        done += take;
    }
}

void wpa_ptk(const uint8_t pmk[WPA_PMK_LEN], const uint8_t aa[6], const uint8_t spa[6],
             const uint8_t anonce[WPA_NONCE_LEN], const uint8_t snonce[WPA_NONCE_LEN],
             uint8_t ptk[WPA_PTK_LEN]) {
    uint8_t d[2*6 + 2*WPA_NONCE_LEN];
    const uint8_t *lo = aa, *hi = spa;
    if (memcmp(aa, spa, 6) > 0) { lo = spa; hi = aa; }
    memcpy(d, lo, 6);
    memcpy(d + 6, hi, 6);

    const uint8_t *nlo = anonce, *nhi = snonce;
    if (memcmp(anonce, snonce, WPA_NONCE_LEN) > 0) { nlo = snonce; nhi = anonce; }
    memcpy(d + 12, nlo, WPA_NONCE_LEN);
    memcpy(d + 12 + WPA_NONCE_LEN, nhi, WPA_NONCE_LEN);

    wpa_prf(pmk, WPA_PMK_LEN, "Pairwise key expansion", d, sizeof d, ptk, WPA_PTK_LEN);
}

/* =============================== EAPOL ================================== */
/* The MIC covers the frame with its own MIC field zeroed, so this copies -- a
 * caller handing us a frame it is about to send would otherwise have to zero,
 * compute, and put back, and the receive side must not modify what it verifies
 * at all. 200 bytes on the stack is cheaper than getting that wrong. */
#define EAPOL_MIC_OFF (4 + 1 + 2 + 2 + 8 + WPA_NONCE_LEN + 16 + 8 + 8)

void wpa_eapol_mic(const uint8_t kck[WPA_KCK_LEN], int ver,
                   const uint8_t *frame, uint32_t len, uint8_t mic[16]) {
    uint8_t copy[512];
    if (len > sizeof copy || len < EAPOL_MIC_OFF + 16) { memset(mic, 0, 16); return; }
    memcpy(copy, frame, len);
    memset(copy + EAPOL_MIC_OFF, 0, 16);

    if (ver == KI_VER_AES_CMAC) {
        aes_cmac(kck, copy, len, mic);
    } else {
        uint8_t full[20];
        hmac_sha1(kck, WPA_KCK_LEN, copy, len, full);
        memcpy(mic, full, 16);                 /* HMAC-SHA1-128: truncated */
    }
}

int wpa_eapol_mic_ok(const uint8_t kck[WPA_KCK_LEN], int ver,
                     const uint8_t *frame, uint32_t len) {
    uint8_t want[16];
    if (len < EAPOL_MIC_OFF + 16) return 0;
    wpa_eapol_mic(kck, ver, frame, len, want);
    int bad = 0;
    for (int i = 0; i < 16; i++) bad |= want[i] ^ frame[EAPOL_MIC_OFF + i];
    return bad == 0;
}

/* =============================== CCMP =================================== */
/* The AAD is the MAC header with the fields that may legitimately change in
 * flight masked off, and the nonce is priority || A2 || PN. Both are spelled
 * out in 802.11-2012 11.4.3.3; getting either subtly wrong gives a MIC failure
 * on every frame and no clue as to which end is at fault, so they live here
 * next to each other rather than at the two call sites. */
static uint32_t ccmp_aad(const uint8_t *hdr, uint32_t hdrlen, uint8_t *aad) {
    uint16_t fc = (uint16_t)(hdr[0] | (hdr[1] << 8));
    fc &= ~(uint16_t)0x0070;                            /* subtype bits 4-6      */
    fc &= ~(uint16_t)(FC_RETRY | 0x1000 | 0x2000);      /* retry, pwr mgmt, more */
    fc |= FC_PROTECTED;

    uint32_t n = 0;
    aad[n++] = (uint8_t)fc;
    aad[n++] = (uint8_t)(fc >> 8);
    memcpy(aad + n, hdr + 4, 18); n += 18;              /* addr1, addr2, addr3 */
    aad[n++] = (uint8_t)(hdr[22] & 0x0F);               /* seq ctrl: fragment number only */
    aad[n++] = 0;
    if (hdrlen >= 30) { memcpy(aad + n, hdr + 24, 6); n += 6; }   /* addr4 */
    return n;
}

static void ccmp_nonce(const uint8_t *hdr, uint64_t pn, uint8_t nonce[13]) {
    nonce[0] = 0;                                       /* priority; QoS would set it */
    memcpy(nonce + 1, hdr + 10, 6);                     /* A2 */
    for (int i = 0; i < 6; i++) nonce[7 + i] = (uint8_t)(pn >> (40 - i*8));
}

int ccmp_encrypt(const uint8_t tk[WPA_TK_LEN], uint64_t pn,
                 const uint8_t *hdr, uint32_t hdrlen,
                 const uint8_t *pt, uint32_t ptlen,
                 uint8_t *out, uint32_t out_cap) {
    if (hdrlen < 24 || out_cap < hdrlen + CCMP_HDR_LEN + ptlen + CCM_MIC_LEN) return -1;

    memcpy(out, hdr, hdrlen);
    out[1] |= (uint8_t)(FC_PROTECTED >> 8);

    uint8_t *ch = out + hdrlen;
    ch[0] = (uint8_t)pn;
    ch[1] = (uint8_t)(pn >> 8);
    ch[2] = 0;
    ch[3] = 0x20;                                       /* ExtIV, key id 0 */
    ch[4] = (uint8_t)(pn >> 16);
    ch[5] = (uint8_t)(pn >> 24);
    ch[6] = (uint8_t)(pn >> 32);
    ch[7] = (uint8_t)(pn >> 40);

    uint8_t aad[32], nonce[13];
    uint32_t aadlen = ccmp_aad(out, hdrlen, aad);
    ccmp_nonce(out, pn, nonce);
    aes_ccm_encrypt(tk, nonce, aad, aadlen, pt, ptlen,
                    out + hdrlen + CCMP_HDR_LEN,
                    out + hdrlen + CCMP_HDR_LEN + ptlen);
    return (int)(hdrlen + CCMP_HDR_LEN + ptlen + CCM_MIC_LEN);
}

int ccmp_decrypt(const uint8_t tk[WPA_TK_LEN],
                 const uint8_t *frame, uint32_t len,
                 uint8_t *out, uint32_t out_cap, uint64_t *pn_out) {
    uint32_t hdrlen = 24;
    uint16_t fc = (uint16_t)(frame[0] | (frame[1] << 8));
    if ((fc & (FC_TODS | FC_FROMDS)) == (FC_TODS | FC_FROMDS)) hdrlen = 30;
    if (len < hdrlen + CCMP_HDR_LEN + CCM_MIC_LEN) return -1;
    if (!(fc & FC_PROTECTED)) return -1;

    const uint8_t *ch = frame + hdrlen;
    if (!(ch[3] & 0x20)) return -1;                     /* no ExtIV: not CCMP */
    uint64_t pn = (uint64_t)ch[0] | ((uint64_t)ch[1] << 8) |
                  ((uint64_t)ch[4] << 16) | ((uint64_t)ch[5] << 24) |
                  ((uint64_t)ch[6] << 32) | ((uint64_t)ch[7] << 40);

    uint32_t ctlen = len - hdrlen - CCMP_HDR_LEN - CCM_MIC_LEN;
    if (ctlen > out_cap) return -1;

    uint8_t aad[32], nonce[13];
    uint32_t aadlen = ccmp_aad(frame, hdrlen, aad);
    ccmp_nonce(frame, pn, nonce);
    if (aes_ccm_decrypt(tk, nonce, aad, aadlen,
                        frame + hdrlen + CCMP_HDR_LEN, ctlen,
                        frame + hdrlen + CCMP_HDR_LEN + ctlen, out) != 0)
        return -2;
    if (pn_out) *pn_out = pn;
    return (int)ctlen;
}
