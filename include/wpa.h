#pragma once
#include <stdint.h>
#include "crypto.h"      /* aes128_ctx and the forward cipher, reused here */

/* ===========================================================================
 *  WPA2 personal: the key schedule, the EAPOL-Key frame, and CCMP.
 *
 *  All of this is arithmetic over bytes -- no radio, no registers, nothing
 *  that needs a machine to run -- which is why it is a separate file from the
 *  station state machine in net/wifi_sta.c and why test/host/wpa.c can hold it
 *  up against the published vectors (RFC 2202, 3394, 3610, 4493, 6070 and the
 *  802.11i PSK examples) on the host, in a second, without QEMU.
 *
 *  The primitives here exist because net/crypto.c is TLS's and TLS has needed
 *  none of them: 802.11's key hierarchy is SHA-1 all the way down, and the
 *  GTK arrives wrapped with the AES inverse cipher, which nothing else on the
 *  machine has ever had to run.
 * ===========================================================================*/

/* ---- SHA-1 ------------------------------------------------------------- */
typedef struct {
    uint32_t h[5];
    uint64_t total;
    uint8_t  buf[64];
    uint32_t n;
} sha1_ctx;

void sha1_init(sha1_ctx *c);
void sha1_update(sha1_ctx *c, const void *data, uint32_t len);
void sha1_final(sha1_ctx *c, uint8_t out[20]);
void sha1(const void *data, uint32_t len, uint8_t out[20]);

void hmac_sha1(const uint8_t *key, uint32_t klen,
               const uint8_t *msg, uint32_t mlen, uint8_t out[20]);

void pbkdf2_sha1(const void *pw, uint32_t pwlen,
                 const uint8_t *salt, uint32_t saltlen,
                 uint32_t rounds, uint8_t *out, uint32_t outlen);

/* ---- AES-128 pieces WPA needs and TLS never did ------------------------ */

/* Inverse cipher. Takes the same forward key schedule aes128_init() builds. */
void aes128_decrypt(const aes128_ctx *c, const uint8_t in[16], uint8_t out[16]);

/* NIST AES key wrap (RFC 3394) with a 128-bit KEK. `n` is the plaintext
 * length in bytes and must be a multiple of 8 and at least 16; the wrapped
 * form is 8 bytes longer. aes_key_unwrap() returns 0 when the integrity check
 * value came back as expected, -1 when it did not -- which is how a wrong KEK
 * is detected, so the caller must actually look at it. */
void aes_key_wrap(const uint8_t kek[16], const uint8_t *in, uint32_t n, uint8_t *out);
int  aes_key_unwrap(const uint8_t kek[16], const uint8_t *in, uint32_t n, uint8_t *out);

/* AES-CMAC (RFC 4493), the MIC for key descriptor version 3. */
void aes_cmac(const uint8_t key[16], const uint8_t *msg, uint32_t len, uint8_t out[16]);

/* AES-CCM exactly as CCMP uses it: 13-byte nonce, L=2, 8-byte MIC.
 * aes_ccm_decrypt() returns 0 when the MIC verifies and -1 when it does not;
 * on failure the plaintext buffer holds garbage and must not be used. */
#define CCM_MIC_LEN 8
void aes_ccm_encrypt(const uint8_t key[16], const uint8_t nonce[13],
                     const uint8_t *aad, uint32_t aadlen,
                     const uint8_t *pt, uint32_t ptlen,
                     uint8_t *ct, uint8_t mic[CCM_MIC_LEN]);
int  aes_ccm_decrypt(const uint8_t key[16], const uint8_t nonce[13],
                     const uint8_t *aad, uint32_t aadlen,
                     const uint8_t *ct, uint32_t ctlen,
                     const uint8_t mic[CCM_MIC_LEN], uint8_t *pt);

/* ---- the key hierarchy ------------------------------------------------- */
#define WPA_PMK_LEN 32
#define WPA_PTK_LEN 48        /* KCK(16) || KEK(16) || TK(16) for CCMP */
#define WPA_KCK_LEN 16
#define WPA_KEK_LEN 16
#define WPA_TK_LEN  16
#define WPA_NONCE_LEN 32

/* PMK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 256 bits). 4096 iterations of
 * HMAC-SHA1 over a 32-byte block is not free: about 40 ms on the test machine,
 * so derive it once and keep it, not once per handshake. */
void wpa_pmk(const char *passphrase, const char *ssid, uint8_t pmk[WPA_PMK_LEN]);

/* The 802.11 PRF: HMAC-SHA1(key, label || 0x00 || data || i) for i = 0, 1, ... */
void wpa_prf(const uint8_t *key, uint32_t klen, const char *label,
             const uint8_t *data, uint32_t dlen, uint8_t *out, uint32_t outlen);

/* PTK = PRF-384(PMK, "Pairwise key expansion", min||max of the two addresses
 * then of the two nonces). Which side is the authenticator does not enter into
 * it -- that is the whole point of the sort. */
void wpa_ptk(const uint8_t pmk[WPA_PMK_LEN], const uint8_t aa[6], const uint8_t spa[6],
             const uint8_t anonce[WPA_NONCE_LEN], const uint8_t snonce[WPA_NONCE_LEN],
             uint8_t ptk[WPA_PTK_LEN]);

/* ---- EAPOL-Key --------------------------------------------------------- */
#define EAPOL_VERSION      2
#define EAPOL_TYPE_KEY     3
#define EAPOL_DESC_RSN     2

/* key_info bits, host order (the field itself is big-endian on the wire). */
#define KI_DESC_VER    0x0007
#define KI_PAIRWISE    0x0008
#define KI_INSTALL     0x0040
#define KI_ACK         0x0080
#define KI_MIC         0x0100
#define KI_SECURE      0x0200
#define KI_ERROR       0x0400
#define KI_REQUEST     0x0800
#define KI_ENCRYPTED   0x1000

#define KI_VER_HMAC_SHA1 2      /* HMAC-SHA1-128 MIC, AES key wrap  */
#define KI_VER_AES_CMAC  3      /* AES-128-CMAC MIC, AES key wrap   */

/* The EAPOL header and the EAPOL-Key body, on the wire, big-endian throughout.
 * 4 + 95 bytes before any key data. */
struct eapol_hdr {
    uint8_t  version;
    uint8_t  type;
    uint16_t length;                  /* body length, not counting this header */
} __attribute__((packed));

struct eapol_key {
    uint8_t  desc_type;
    uint16_t key_info;
    uint16_t key_length;
    uint8_t  replay[8];
    uint8_t  nonce[WPA_NONCE_LEN];
    uint8_t  iv[16];
    uint8_t  rsc[8];
    uint8_t  id[8];
    uint8_t  mic[16];
    uint16_t data_len;
    uint8_t  data[];
} __attribute__((packed));

#define EAPOL_KEY_FIXED   95          /* sizeof(struct eapol_key), spelled out */

/* Compute the MIC over a whole EAPOL frame (from the version byte to the end
 * of the key data) with the MIC field taken as zero, per descriptor version
 * `ver`. `frame` is not modified. */
void wpa_eapol_mic(const uint8_t kck[WPA_KCK_LEN], int ver,
                   const uint8_t *frame, uint32_t len, uint8_t mic[16]);
/* 1 if the MIC in the frame is the one the KCK produces. */
int  wpa_eapol_mic_ok(const uint8_t kck[WPA_KCK_LEN], int ver,
                      const uint8_t *frame, uint32_t len);

/* ---- CCMP -------------------------------------------------------------- */
#define CCMP_HDR_LEN  8               /* PN0 PN1 rsvd keyid PN2..PN5 */

/* Encrypt an 802.11 data frame in place-ish: `hdr`/`hdrlen` is the plaintext
 * MAC header, `pt`/`ptlen` the payload, and out gets hdr || CCMP header ||
 * ciphertext || MIC. Returns the total length, or -1 if out_cap is too small.
 * pn is the 48-bit packet number, which must never repeat under one key. */
int ccmp_encrypt(const uint8_t tk[WPA_TK_LEN], uint64_t pn,
                 const uint8_t *hdr, uint32_t hdrlen,
                 const uint8_t *pt, uint32_t ptlen,
                 uint8_t *out, uint32_t out_cap);

/* The reverse. `frame` is a received protected frame; out gets the plaintext
 * payload. Returns the payload length, -1 on a malformed frame and -2 when the
 * MIC does not verify. *pn_out, when given, receives the frame's packet
 * number so the caller can enforce replay ordering. */
int ccmp_decrypt(const uint8_t tk[WPA_TK_LEN],
                 const uint8_t *frame, uint32_t len,
                 uint8_t *out, uint32_t out_cap, uint64_t *pn_out);
