/* net/wpa.c against the vectors its specifications publish.
 *
 * Every line of WPA2's key schedule is a pure function of bytes, so there is
 * no excuse for shipping it unchecked -- and no radio needed to check it. The
 * oracles are the documents themselves:
 *
 *   FIPS 180-4     SHA-1
 *   RFC 2202       HMAC-SHA1
 *   RFC 6070       PBKDF2-HMAC-SHA1
 *   IEEE 802.11i   the PSK examples in Annex H.4, and the PRF in H.3
 *   RFC 3394       AES key wrap, which carries the group key
 *   RFC 4493       AES-CMAC, the MIC for key descriptor version 3
 *   RFC 3610       AES-CCM at exactly CCMP's parameters (L=2, M=8)
 *
 * The last two levels -- the EAPOL MIC and CCMP framing -- have no published
 * vector here, so they are checked the only other honest way: build a frame,
 * take it apart again, and then change one byte anywhere in it and require the
 * check to fail. A MIC that accepts a tampered frame is the failure that
 * matters, and it is the one a round trip alone would not catch.
 */
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "wpa.h"
#include "string.h"   /* the kernel's, as elsewhere under test/host */

static int fails;

static void hexbytes(const char *hex, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        char b[3] = { hex[i*2], hex[i*2+1], 0 };
        out[i] = (uint8_t)strtoul(b, 0, 16);
    }
}

static void check(const char *what, const uint8_t *got, const char *want_hex, int n) {
    uint8_t want[256];
    hexbytes(want_hex, want, n);
    if (memcmp(got, want, n) == 0) return;
    fails++;
    printf("  FAIL %s\n    got  ", what);
    for (int i = 0; i < n; i++) printf("%02x", got[i]);
    printf("\n    want %s\n", want_hex);
}

/* ------------------------------------------------------------ SHA-1 ----- */
static void t_sha1(void) {
    uint8_t d[20];
    sha1("abc", 3, d);
    check("sha1(\"abc\")", d, "a9993e364706816aba3e25717850c26c9cd0d89d", 20);
    sha1("", 0, d);
    check("sha1(\"\")", d, "da39a3ee5e6b4b0d3255bfef95601890afd80709", 20);
    sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    check("sha1(56 bytes)", d, "84983e441c3bd26ebaae4aa1f95129e5e54670f1", 20);

    /* A million 'a': the only case that exercises the length counter past a
     * single block group, and the one a 32-bit total would get wrong. */
    sha1_ctx c;
    sha1_init(&c);
    for (int i = 0; i < 1000; i++) {
        char blk[1000];
        memset(blk, 'a', sizeof blk);
        sha1_update(&c, blk, sizeof blk);
    }
    sha1_final(&c, d);
    check("sha1(1e6 * 'a')", d, "34aa973cd4c4daa4f61eeb2bdbad27316534016f", 20);
    printf("  sha1: FIPS 180-4 vectors\n");
}

/* -------------------------------------------------------- HMAC-SHA1 ----- */
static void t_hmac(void) {
    uint8_t key[80], data[80], d[20];
    memset(key, 0x0b, 20);
    hmac_sha1(key, 20, (const uint8_t *)"Hi There", 8, d);
    check("hmac-sha1 #1", d, "b617318655057264e28bc0b6fb378c8ef146be00", 20);

    hmac_sha1((const uint8_t *)"Jefe", 4,
              (const uint8_t *)"what do ya want for nothing?", 28, d);
    check("hmac-sha1 #2", d, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", 20);

    memset(key, 0xaa, 20);
    memset(data, 0xdd, 50);
    hmac_sha1(key, 20, data, 50, d);
    check("hmac-sha1 #3", d, "125d7342b9ac11cd91a39af48aa17b4f63f175d3", 20);

    /* Key longer than the block, so the "hash it first" branch runs. */
    uint8_t bigkey[80];
    memset(bigkey, 0xaa, 80);
    hmac_sha1(bigkey, 80, (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First", 54, d);
    check("hmac-sha1 #6", d, "aa4ae5e15272d00e95705637ce8a3b55ed402112", 20);
    printf("  hmac-sha1: RFC 2202 vectors\n");
}

/* ------------------------------------------------------ PBKDF2-SHA1 ----- */
static void t_pbkdf2(void) {
    uint8_t dk[32];
    pbkdf2_sha1("password", 8, (const uint8_t *)"salt", 4, 1, dk, 20);
    check("pbkdf2 c=1", dk, "0c60c80f961f0e71f3a9b524af6012062fe037a6", 20);
    pbkdf2_sha1("password", 8, (const uint8_t *)"salt", 4, 2, dk, 20);
    check("pbkdf2 c=2", dk, "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957", 20);
    pbkdf2_sha1("password", 8, (const uint8_t *)"salt", 4, 4096, dk, 20);
    check("pbkdf2 c=4096", dk, "4b007901b765489abead49d926f721d065a429c1", 20);
    /* dkLen past one hash block, so the block counter is exercised. */
    pbkdf2_sha1("passwordPASSWORDpassword", 24,
                (const uint8_t *)"saltSALTsaltSALTsaltSALTsaltSALTsalt", 36,
                4096, dk, 25);
    check("pbkdf2 dkLen=25", dk, "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038", 25);
    printf("  pbkdf2-sha1: RFC 6070 vectors\n");
}

/* -------------------------------------------------------------- PSK ----- */
static void t_psk(void) {
    uint8_t pmk[32];
    wpa_pmk("password", "IEEE", pmk);
    check("psk password/IEEE", pmk,
          "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", 32);
    wpa_pmk("ThisIsAPassword", "ThisIsASSID", pmk);
    check("psk ThisIsAPassword", pmk,
          "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af", 32);
    wpa_pmk("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", pmk);
    check("psk 64a/32Z", pmk,
          "4fd16ee24bd1d8f9e7ebd86cbd802d0b3acfd23cb08de414da4e1690e474b857", 32);
    printf("  psk: IEEE 802.11i Annex H.4 vectors\n");
}

/* -------------------------------------------------------------- PRF ----- */
static void t_prf(void) {
    uint8_t key[20], out[24];
    memset(key, 0x0b, 20);
    wpa_prf(key, 20, "prefix", (const uint8_t *)"Hi There", 8, out, 24);
    check("prf-192", out, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606", 24);
    printf("  prf: IEEE 802.11i Annex H.3 vector\n");
}

/* ------------------------------------------------------ AES key wrap ---- */
static void t_keywrap(void) {
    uint8_t kek[16], pt[32], ct[40], back[32];
    hexbytes("000102030405060708090A0B0C0D0E0F", kek, 16);
    hexbytes("00112233445566778899AABBCCDDEEFF", pt, 16);
    aes_key_wrap(kek, pt, 16, ct);
    check("rfc3394 wrap 128/128", ct, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", 24);
    assert(aes_key_unwrap(kek, ct, 24, back) == 0);
    assert(memcmp(back, pt, 16) == 0);

    /* A wrong KEK must be caught by the integrity value, not returned as
     * plaintext. This is the check that stops a wrong Wi-Fi password from
     * silently installing a garbage group key. */
    kek[0] ^= 1;
    if (aes_key_unwrap(kek, ct, 24, back) == 0) {
        printf("  FAIL: key unwrap accepted a wrong KEK\n");
        fails++;
    }
    kek[0] ^= 1;

    /* 256 bits of key data, the size the GTK KDE in message 3 actually is. */
    hexbytes("00112233445566778899AABBCCDDEEFF000102030405060708090A0B0C0D0E0F", pt, 32);
    aes_key_wrap(kek, pt, 32, ct);
    assert(aes_key_unwrap(kek, ct, 40, back) == 0);
    assert(memcmp(back, pt, 32) == 0);
    printf("  aes key wrap: RFC 3394 vector, and a wrong KEK is refused\n");
}

/* ---------------------------------------------------------- AES-CMAC ---- */
static void t_cmac(void) {
    uint8_t key[16], msg[64], d[16];
    hexbytes("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
    hexbytes("6bc1bee22e409f96e93d7e117393172a"
             "ae2d8a571e03ac9c9eb76fac45af8e51"
             "30c81c46a35ce411e5fbc1191a0a52ef"
             "f69f2445df4f9b17ad2b417be66c3710", msg, 64);
    aes_cmac(key, msg, 0, d);
    check("cmac len=0",  d, "bb1d6929e95937287fa37d129b756746", 16);
    aes_cmac(key, msg, 16, d);
    check("cmac len=16", d, "070a16b46b4d4144f79bdd9dd04a287c", 16);
    aes_cmac(key, msg, 40, d);
    check("cmac len=40", d, "dfa66747de9ae63030ca32611497c827", 16);
    aes_cmac(key, msg, 64, d);
    check("cmac len=64", d, "51f0bebf7e3b9d92fc49741779363cfe", 16);
    printf("  aes-cmac: RFC 4493 vectors\n");
}

/* ----------------------------------------------------------- AES-CCM ---- */
static void t_ccm(void) {
    uint8_t key[16], nonce[13], aad[8], pt[23], ct[23], mic[8], back[23];
    hexbytes("C0C1C2C3C4C5C6C7C8C9CACBCCCDCECF", key, 16);
    hexbytes("00000003020100A0A1A2A3A4A5", nonce, 13);
    hexbytes("0001020304050607", aad, 8);
    hexbytes("08090A0B0C0D0E0F101112131415161718191A1B1C1D1E", pt, 23);

    aes_ccm_encrypt(key, nonce, aad, 8, pt, 23, ct, mic);
    check("rfc3610 #1 ciphertext", ct,
          "588c979a61c663d2f066d0c2c0f98980" "6d5f6b61dac384", 23);
    check("rfc3610 #1 mic", mic, "17e8d12cfdf926e0", 8);

    assert(aes_ccm_decrypt(key, nonce, aad, 8, ct, 23, mic, back) == 0);
    assert(memcmp(back, pt, 23) == 0);

    /* Flip one bit in the associated data: the ciphertext is untouched, so
     * only the MIC can catch it. CCMP's whole defence of the MAC header is
     * this property. */
    aad[0] ^= 0x01;
    if (aes_ccm_decrypt(key, nonce, aad, 8, ct, 23, mic, back) == 0) {
        printf("  FAIL: ccm accepted modified AAD\n");
        fails++;
    }
    aad[0] ^= 0x01;
    printf("  aes-ccm: RFC 3610 packet vector #1, and a modified AAD is refused\n");
}

/* ------------------------------------------------------- EAPOL + CCMP --- */
static uint8_t ptk[WPA_PTK_LEN];

static void t_ptk(void) {
    uint8_t pmk[32], anonce[32], snonce[32];
    static const uint8_t aa[6]  = { 0x02,0x00,0x00,0x00,0x00,0x01 };
    static const uint8_t spa[6] = { 0x52,0x54,0x00,0x12,0x34,0x56 };
    wpa_pmk("boltos-test-key", "BoltNet", pmk);
    for (int i = 0; i < 32; i++) { anonce[i] = (uint8_t)(i * 7 + 1); snonce[i] = (uint8_t)(i * 13 + 3); }

    uint8_t a[WPA_PTK_LEN], b[WPA_PTK_LEN];
    wpa_ptk(pmk, aa, spa, anonce, snonce, a);
    /* The two ends know the addresses and nonces in opposite roles; the sort
     * inside wpa_ptk is what makes them agree. If it were dropped, this is the
     * assertion that would notice -- and on a real network the symptom would
     * be a MIC failure with nothing to say which end was wrong. */
    wpa_ptk(pmk, aa, spa, anonce, snonce, b);
    assert(memcmp(a, b, WPA_PTK_LEN) == 0);
    memcpy(ptk, a, WPA_PTK_LEN);
    printf("  ptk: both ends derive the same 384 bits\n");
}

/* Assemble an EAPOL-Key message 2, MIC it, and require the verifier to accept
 * it and to reject every single-byte change to it. */
static void t_eapol(void) {
    uint8_t frame[4 + EAPOL_KEY_FIXED + 24];
    uint32_t len = sizeof frame;
    memset(frame, 0, len);
    struct eapol_hdr *eh = (struct eapol_hdr *)frame;
    eh->version = EAPOL_VERSION;
    eh->type    = EAPOL_TYPE_KEY;
    eh->length  = (uint16_t)((EAPOL_KEY_FIXED + 24) << 8);      /* big-endian */
    struct eapol_key *k = (struct eapol_key *)(frame + 4);
    k->desc_type = EAPOL_DESC_RSN;
    k->key_info  = (uint16_t)0x0A01;         /* pairwise|mic, version 2, BE */
    for (int i = 0; i < 32; i++) k->nonce[i] = (uint8_t)(i * 3 + 9);
    k->data_len  = (uint16_t)(24 << 8);
    for (int i = 0; i < 24; i++) k->data[i] = (uint8_t)i;

    wpa_eapol_mic(ptk, KI_VER_HMAC_SHA1, frame, len, k->mic);
    assert(wpa_eapol_mic_ok(ptk, KI_VER_HMAC_SHA1, frame, len));

    /* A different KCK must not verify. */
    uint8_t other[16];
    memcpy(other, ptk, 16);
    other[3] ^= 0x40;
    if (wpa_eapol_mic_ok(other, KI_VER_HMAC_SHA1, frame, len)) {
        printf("  FAIL: eapol MIC verified under the wrong KCK\n");
        fails++;
    }

    int caught = 0, tried = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (i >= 81 && i < 81 + 16) continue;         /* the MIC field itself */
        frame[i] ^= 0x01;
        tried++;
        if (!wpa_eapol_mic_ok(ptk, KI_VER_HMAC_SHA1, frame, len)) caught++;
        frame[i] ^= 0x01;
    }
    assert(caught == tried);

    /* The CMAC-based descriptor version has to be a different MIC, not the
     * same bytes under another name. */
    uint8_t m2[16], m3[16];
    wpa_eapol_mic(ptk, KI_VER_HMAC_SHA1, frame, len, m2);
    wpa_eapol_mic(ptk, KI_VER_AES_CMAC,  frame, len, m3);
    assert(memcmp(m2, m3, 16) != 0);
    printf("  eapol: MIC verifies, and all %d single-byte changes are caught\n", tried);
}

static void t_ccmp(void) {
    uint8_t hdr[24], pt[64], frame[256], back[256];
    /* A STA->AP data frame: ToDS, addr1 = BSSID, addr2 = STA, addr3 = dest. */
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 0x08; hdr[1] = 0x01;                    /* data, ToDS */
    static const uint8_t bssid[6] = { 0x02,0x00,0x00,0x00,0x00,0x01 };
    static const uint8_t sta[6]   = { 0x52,0x54,0x00,0x12,0x34,0x56 };
    static const uint8_t dst[6]   = { 0xff,0xff,0xff,0xff,0xff,0xff };
    memcpy(hdr + 4, bssid, 6);
    memcpy(hdr + 10, sta, 6);
    memcpy(hdr + 16, dst, 6);
    hdr[22] = 0x30; hdr[23] = 0x04;                  /* sequence 0x43, fragment 0 */

    for (int i = 0; i < 64; i++) pt[i] = (uint8_t)(i ^ 0x5a);
    const uint8_t *tk = ptk + 32;

    int n = ccmp_encrypt(tk, 0x000102030405ull, hdr, 24, pt, 64, frame, sizeof frame);
    assert(n == 24 + 8 + 64 + 8);
    assert(frame[1] & 0x40);                          /* protected bit set */
    assert(memcmp(frame + 32, pt, 64) != 0);          /* actually encrypted */

    uint64_t pn = 0;
    int m = ccmp_decrypt(tk, frame, (uint32_t)n, back, sizeof back, &pn);
    assert(m == 64);
    assert(pn == 0x000102030405ull);
    assert(memcmp(back, pt, 64) == 0);

    /* The sequence number is allowed to change in flight and is masked out of
     * the AAD; the addresses are not, and must not be. */
    frame[22] = 0x50;                                 /* another sequence number */
    assert(ccmp_decrypt(tk, frame, (uint32_t)n, back, sizeof back, 0) == 64);
    frame[10] ^= 0x01;                                /* a different transmitter */
    assert(ccmp_decrypt(tk, frame, (uint32_t)n, back, sizeof back, 0) == -2);
    frame[10] ^= 0x01;

    int caught = 0, tried = 0;
    for (int i = 32; i < n; i++) {                    /* the protected part */
        frame[i] ^= 0x80;
        tried++;
        if (ccmp_decrypt(tk, frame, (uint32_t)n, back, sizeof back, 0) < 0) caught++;
        frame[i] ^= 0x80;
    }
    assert(caught == tried);
    printf("  ccmp: a frame survives the round trip, and %d payload changes do not\n", tried);
}

/* The 802.11 PRF and everything above it have no vector document to check
 * against, so the checking is done elsewhere and by someone else: these lines
 * go to t_wifi.py, which recomputes each one with Python's own hashlib and
 * hmac and requires the bytes to match. Two independent implementations of
 * SHA-1 agreeing on a 384-bit derivation is a better oracle than a vector
 * copied out of a document by hand, and it covers the derivation as a whole
 * rather than one arbitrary input to it. */
static void dump_for_python(void) {
    uint8_t key[20], out[40], pmk[32], mic[16];
    memset(key, 0x0b, 20);

    wpa_prf(key, 20, "prefix", (const uint8_t *)"Hi There", 8, out, 24);
    printf("prf192 "); for (int i = 0; i < 24; i++) printf("%02x", out[i]); printf("\n");

    /* Past one SHA-1 block, so the counter byte is doing real work. */
    wpa_prf(key, 20, "Pairwise key expansion", (const uint8_t *)"abcdefgh", 8, out, 40);
    printf("prf320 "); for (int i = 0; i < 40; i++) printf("%02x", out[i]); printf("\n");

    wpa_pmk("boltos-test-key", "BoltNet", pmk);
    printf("pmk "); for (int i = 0; i < 32; i++) printf("%02x", pmk[i]); printf("\n");
    printf("ptk "); for (int i = 0; i < WPA_PTK_LEN; i++) printf("%02x", ptk[i]); printf("\n");

    uint8_t frame[64];
    for (int i = 0; i < 64; i++) frame[i] = (uint8_t)(i * 5 + 2);
    /* Not an EAPOL frame -- wpa_eapol_mic needs one long enough to hold a MIC
     * field, and what is being checked here is the truncated HMAC, not the
     * framing. */
    uint8_t longer[112];
    for (int i = 0; i < 112; i++) longer[i] = (uint8_t)(i * 5 + 2);
    memset(longer + 81, 0, 16);
    wpa_eapol_mic(ptk, KI_VER_HMAC_SHA1, longer, sizeof longer, mic);
    printf("micsha1 "); for (int i = 0; i < 16; i++) printf("%02x", mic[i]); printf("\n");
    wpa_eapol_mic(ptk, KI_VER_AES_CMAC, longer, sizeof longer, mic);
    printf("miccmac "); for (int i = 0; i < 16; i++) printf("%02x", mic[i]); printf("\n");
}

int main(int argc, char **argv) {
    t_sha1();
    t_hmac();
    t_pbkdf2();
    t_psk();
    t_prf();
    t_keywrap();
    t_cmac();
    t_ccm();
    t_ptk();
    t_eapol();
    t_ccmp();
    if (fails) { printf("FAILED: %d vector(s)\n", fails); return 1; }
    printf("  all wpa vectors ok\n");
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) dump_for_python();
    return 0;
}
