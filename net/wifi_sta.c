#include <stdint.h>
#include "wifi.h"
#include "wpa.h"
#include "net.h"
#include "netif.h"
#include "netcfg.h"
#include "crypto.h"
#include "string.h"
#include "kprintf.h"
#include "pit.h"

/* The 802.11 station: scan, authenticate, associate, run the WPA2 four-way
 * handshake, then carry Ethernet frames over the air.
 *
 * All of it is management frames and arithmetic -- there is not one register
 * access in this file. The radio underneath is whatever registered itself with
 * wifi_radio_register(), and it only has to tune, transmit and hand frames up.
 * That split is the reason a new card is a driver rather than a rewrite, and
 * it is also the reason this code can be run at all today: drivers/wifi_sim.c
 * is a radio, so every line below executes on every boot that joins a network.
 *
 * Not implemented, deliberately and visibly: WEP (refused), SAE (WPA3 networks
 * are listed and refused rather than joined on a downgrade), QoS/802.11n rates,
 * fragmentation, and power save. `wifi` says so rather than failing quietly. */

#define ETHERTYPE_EAPOL 0x888E
#define STA_HDR_LEN     24

static struct wifi_radio *radio;
static struct netif       wlan_if;
static int                wlan_registered;

static struct {
    enum wifi_state state;
    struct wifi_bss bss;                     /* the BSS we are on or joining */
    char            ssid[33];
    uint8_t         pmk[WPA_PMK_LEN];
    uint8_t         ptk[WPA_PTK_LEN];
    uint8_t         gtk[32];
    int             gtk_len;
    int             secure;                  /* the TK is installed          */
    uint8_t         anonce[WPA_NONCE_LEN], snonce[WPA_NONCE_LEN];
    uint8_t         replay[8];
    uint64_t        tx_pn, rx_pn;
    uint16_t        seq;
    int8_t          rssi;

    /* set by the receive path, waited on by whoever is driving the join */
    int             auth_seen, assoc_seen, assoc_status;
    int             hs_msg;                  /* last handshake message handled */
    int             hs_fail;

    int             scanning, scan_n;
    struct wifi_bss scan[WIFI_MAX_BSS];
} S;

/* ------------------------------------------------------------- plumbing --- */

static void radio_tx(const uint8_t *frame, uint16_t len) {
    if (!radio || !radio->ops->tx) return;
    if (radio->ops->tx(radio, frame, len) == 0) radio->tx_frames++;
}

static void poll_once(void) {
    if (radio && radio->ops->poll) radio->ops->poll(radio);
}

/* Poll until *flag is set either way, or the deadline passes. Returns 1 if an
 * answer arrived in time -- including a refusal, which the caller then reads
 * off the flag's sign. Waiting only for a positive answer meant an AP that
 * said no was reported as a timeout, which sends somebody looking at their
 * aerial when the answer is on the screen. Everything that waits for a
 * management frame goes through here, so there is one place where a radio that
 * says nothing at all turns into a timeout. */
static int wait_for(volatile int *flag, int ms) {
    uint64_t deadline = pit_ticks() + (uint64_t)ms;
    while (pit_ticks() < deadline) {
        poll_once();
        if (*flag) return 1;
        __asm__ volatile("hlt");
    }
    poll_once();
    return *flag != 0;
}

static void link_down(void) {
    S.state    = WIFI_IDLE;
    S.secure   = 0;
    S.tx_pn    = 0;
    S.rx_pn    = 0;
    wlan_if.link_up = 0;
    memset(S.ptk, 0, sizeof S.ptk);
}

/* ------------------------------------------------------------ transmit ---- */

/* One buffer for the frame being built. The station is single-threaded by
 * construction -- everything runs from a shell command or the netif poll --
 * so this is not the reentrancy hazard it looks like. */
static uint8_t txbuf[WIFI_FRAME_MAX];
static uint8_t encbuf[WIFI_FRAME_MAX];

static int send_eth(const uint8_t *eth, uint16_t len, int encrypt) {
    int n = ieee80211_eth_to_data(eth, len, S.bss.bssid, S.seq++, 1,
                                  txbuf, sizeof txbuf);
    if (n < 0) return -1;
    if (encrypt && S.secure) {
        int m = ccmp_encrypt(S.ptk + 32, ++S.tx_pn, txbuf, STA_HDR_LEN,
                             txbuf + STA_HDR_LEN, (uint32_t)(n - STA_HDR_LEN),
                             encbuf, sizeof encbuf);
        if (m < 0) return -1;
        radio_tx(encbuf, (uint16_t)m);
    } else {
        radio_tx(txbuf, (uint16_t)n);
    }
    return 0;
}

static int wlan_send(struct netif *nif, const void *frame, uint16_t len) {
    (void)nif;
    if (S.state != WIFI_RUN) return -1;
    return send_eth((const uint8_t *)frame, len, 1);
}

/* EAPOL goes out as an ordinary data frame with a SNAP ethertype of 0x888E and
 * is never CCMP-protected: the key that would protect it is what the exchange
 * is for. */
static uint8_t eapbuf[4 + EAPOL_KEY_FIXED + 128 + ETH_HDR_LEN];

static void send_eapol(const uint8_t *body, uint16_t bodylen) {
    struct eth_hdr *e = (struct eth_hdr *)eapbuf;
    memcpy(e->dst, S.bss.bssid, 6);
    memcpy(e->src, radio->mac, 6);
    e->type = htons(ETHERTYPE_EAPOL);
    memcpy(eapbuf + ETH_HDR_LEN, body, bodylen);
    send_eth(eapbuf, (uint16_t)(ETH_HDR_LEN + bodylen), 0);
}

/* ------------------------------------------------------- the handshake ---- */

static void eapol_reply(uint16_t key_info, const uint8_t *kd, uint16_t kdlen) {
    uint8_t buf[4 + EAPOL_KEY_FIXED + 64];
    uint16_t total = (uint16_t)(EAPOL_KEY_FIXED + kdlen);
    if (4 + total > (int)sizeof buf) return;
    memset(buf, 0, 4 + total);

    struct eapol_hdr *eh = (struct eapol_hdr *)buf;
    eh->version = EAPOL_VERSION;
    eh->type    = EAPOL_TYPE_KEY;
    eh->length  = htons(total);

    struct eapol_key *k = (struct eapol_key *)(buf + 4);
    k->desc_type  = EAPOL_DESC_RSN;
    k->key_info   = htons(key_info);
    k->key_length = 0;                       /* a supplicant leaves this zero */
    memcpy(k->replay, S.replay, 8);
    memcpy(k->nonce, S.snonce, WPA_NONCE_LEN);
    k->data_len   = htons(kdlen);
    if (kdlen) memcpy(k->data, kd, kdlen);

    wpa_eapol_mic(S.ptk, key_info & KI_DESC_VER, buf, (uint32_t)(4 + total), k->mic);
    send_eapol(buf, (uint16_t)(4 + total));
}

/* The GTK arrives inside message 3 as a KDE: 0xDD, length, 00-0F-AC, type 1,
 * then a key id byte, a reserved byte, and the key itself. */
static void take_gtk(const uint8_t *kd, uint16_t len) {
    uint16_t off = 0;
    while (off + 2 <= len) {
        uint8_t id = kd[off], ilen = kd[off + 1];
        if (off + 2u + ilen > len) break;
        if (id == 0xDD && ilen >= 6 &&
            kd[off+2] == 0x00 && kd[off+3] == 0x0F && kd[off+4] == 0xAC && kd[off+5] == 1) {
            int klen = ilen - 6;
            if (klen > (int)sizeof S.gtk) klen = (int)sizeof S.gtk;
            memcpy(S.gtk, kd + off + 8, (uint32_t)klen);
            S.gtk_len = klen;
        }
        off = (uint16_t)(off + 2 + ilen);
    }
}

static void eapol_input(const uint8_t *body, uint16_t len) {
    if (len < 4 + EAPOL_KEY_FIXED) return;
    const struct eapol_hdr *eh = (const struct eapol_hdr *)body;
    if (eh->type != EAPOL_TYPE_KEY) return;
    const struct eapol_key *k = (const struct eapol_key *)(body + 4);

    uint16_t ki  = ntohs(k->key_info);
    uint16_t kdl = ntohs(k->data_len);
    int ver = ki & KI_DESC_VER;
    if (4 + EAPOL_KEY_FIXED + kdl > len) return;
    if (!(ki & KI_PAIRWISE)) return;                    /* group rekey: later */

    if (!(ki & KI_MIC)) {
        /* Message 1: the ANonce, and nothing to verify it with yet. That is
         * how the protocol is, and it is why a forged message 1 costs a
         * supplicant one wasted PTK derivation and nothing else. */
        memcpy(S.anonce, k->nonce, WPA_NONCE_LEN);
        memcpy(S.replay, k->replay, 8);
        crypto_rand_bytes(S.snonce, WPA_NONCE_LEN);
        wpa_ptk(S.pmk, S.bss.bssid, radio->mac, S.anonce, S.snonce, S.ptk);

        /* Message 2 carries the station's RSN element so the AP can check that
         * what it saw in the association request is what we still mean. */
        static const uint8_t rsn[] = {
            IE_RSN, 0x14, 0x01, 0x00, 0x00,0x0F,0xAC,0x04,
            0x01,0x00, 0x00,0x0F,0xAC,0x04, 0x01,0x00, 0x00,0x0F,0xAC,0x02, 0x00,0x00
        };
        eapol_reply((uint16_t)(KI_PAIRWISE | KI_MIC | ver), rsn, sizeof rsn);
        S.hs_msg = 1;
        return;
    }

    /* Message 3. Everything from here is authenticated, so check before use. */
    if (!wpa_eapol_mic_ok(S.ptk, ver, body, len)) {
        /* The passphrase is wrong, or someone else is talking. Either way the
         * frame is not from the AP we think we are joining. */
        S.hs_fail = 1;
        return;
    }
    if (memcmp(k->replay, S.replay, 8) <= 0) return;    /* replayed or stale */
    memcpy(S.replay, k->replay, 8);

    if (kdl) {
        uint8_t plain[128];
        if (ki & KI_ENCRYPTED) {
            if (kdl > sizeof plain + 8 || aes_key_unwrap(S.ptk + 16, k->data, kdl, plain) != 0) {
                S.hs_fail = 1;
                return;
            }
            take_gtk(plain, (uint16_t)(kdl - 8));
        } else {
            take_gtk(k->data, kdl);
        }
    }

    /* Message 4: same replay counter, no key data, MIC over the lot. The TK is
     * installed after it is sent and not before -- install first and the reply
     * goes out encrypted under a key the AP has not confirmed. */
    memset(S.snonce, 0, WPA_NONCE_LEN);                 /* message 4 carries none */
    eapol_reply((uint16_t)(KI_PAIRWISE | KI_MIC | KI_SECURE | ver), 0, 0);
    S.secure = 1;
    S.tx_pn  = 0;
    S.rx_pn  = 0;
    S.hs_msg = 4;
}

/* ------------------------------------------------------------- receive ---- */

static void scan_add(const struct wifi_bss *b, int8_t rssi) {
    for (int i = 0; i < S.scan_n; i++)
        if (memcmp(S.scan[i].bssid, b->bssid, 6) == 0) {
            if (rssi > S.scan[i].rssi) S.scan[i].rssi = rssi;
            return;
        }
    if (S.scan_n >= WIFI_MAX_BSS) return;
    S.scan[S.scan_n] = *b;
    S.scan[S.scan_n].rssi = rssi;
    S.scan_n++;
}

static void mgmt_input(const uint8_t *frame, uint16_t len, int8_t rssi) {
    const struct ieee80211_hdr *h = (const struct ieee80211_hdr *)frame;
    uint16_t st = h->frame_control & FC_SUBTYPE_MASK;
    uint32_t off = sizeof(struct ieee80211_hdr);

    if (st == MGMT_BEACON || st == MGMT_PROBE_RESP) {
        struct wifi_bss b;
        if (ieee80211_parse_beacon(frame, len, &b) != 0) return;
        if (S.scanning) scan_add(&b, rssi);
        if (S.state >= WIFI_AUTH && memcmp(b.bssid, S.bss.bssid, 6) == 0) S.rssi = rssi;
        return;
    }
    /* Everything below is addressed to this station specifically. */
    if (memcmp(h->addr1, radio->mac, 6) != 0) return;

    if (st == MGMT_AUTH && len >= off + 6) {
        uint16_t status = (uint16_t)(frame[off+4] | (frame[off+5] << 8));
        S.auth_seen = status == 0 ? 1 : -1;
    } else if (st == MGMT_ASSOC_RESP && len >= off + 6) {
        S.assoc_status = (uint16_t)(frame[off+2] | (frame[off+3] << 8));
        S.assoc_seen   = 1;
    } else if (st == MGMT_DEAUTH || st == MGMT_DISASSOC) {
        if (S.state != WIFI_IDLE) {
            kprintf("[wifi] deauthenticated by %s\n", S.ssid);
            link_down();
        }
    }
}

static uint8_t rxplain[WIFI_FRAME_MAX];

static void data_input(const uint8_t *frame, uint16_t len) {
    uint16_t fc = ((const struct ieee80211_hdr *)frame)->frame_control;
    uint32_t hdrlen = STA_HDR_LEN;
    const uint8_t *use = frame;
    uint16_t uselen = len;

    if (fc & FC_PROTECTED) {
        if (!S.secure) { radio->rx_dropped++; return; }
        /* Traffic the AP sends to everyone is under the group key, not the
         * pairwise one. Which key to try is decided by the destination address
         * and nothing else, so a frame cannot claim its way into the wrong
         * one. */
        const uint8_t *key = (frame[4] & 1) ? S.gtk : S.ptk + 32;
        if ((frame[4] & 1) && S.gtk_len < WPA_TK_LEN) { radio->rx_dropped++; return; }

        uint64_t pn = 0;
        int n = ccmp_decrypt(key, frame, len,
                             rxplain + hdrlen, (uint32_t)(sizeof rxplain - hdrlen), &pn);
        if (n < 0) { radio->rx_dropped++; return; }
        /* A packet number that has been seen is a replay, and CCMP's whole
         * defence against one is that the receiver refuses to go backwards. */
        if (!(frame[4] & 1)) {
            if (pn <= S.rx_pn) { radio->rx_dropped++; return; }
            S.rx_pn = pn;
        }
        memcpy(rxplain, frame, hdrlen);
        rxplain[1] &= (uint8_t)~(FC_PROTECTED >> 8);
        use    = rxplain;
        uselen = (uint16_t)(hdrlen + n);
    }

    if (ieee80211_snap_ethertype(use, uselen, hdrlen) == ETHERTYPE_EAPOL) {
        eapol_input(use + hdrlen + sizeof(struct llc_snap),
                    (uint16_t)(uselen - hdrlen - sizeof(struct llc_snap)));
        return;
    }
    if (S.state != WIFI_RUN) return;
    /* Once there is a key, anything that arrives without having used it is
     * somebody else talking. EAPOL above is the exception the protocol needs. */
    if (S.secure && !(fc & FC_PROTECTED)) { radio->rx_dropped++; return; }

    uint8_t eth[ETH_FRAME_MAX];
    int n = ieee80211_data_to_eth(use, uselen, eth, sizeof eth);
    if (n > 0) netif_rx(&wlan_if, eth, (uint16_t)n);
}

void wifi_rx(struct wifi_radio *r, const uint8_t *frame, uint16_t len, int8_t rssi) {
    if (r != radio || len < sizeof(struct ieee80211_hdr)) return;
    radio->rx_frames++;
    uint16_t fc = (uint16_t)(frame[0] | (frame[1] << 8));
    if ((fc & FC_TYPE_MASK) == FC_TYPE_MGMT)      mgmt_input(frame, len, rssi);
    else if ((fc & FC_TYPE_MASK) == FC_TYPE_DATA) data_input(frame, len);
    /* control frames (ACK, RTS/CTS) are the radio's business, not ours */
}

/* ---------------------------------------------------------------- scan ---- */

int wifi_scan(int ms) {
    if (!radio) return WIFI_ENORADIO;
    enum wifi_state prev = S.state;
    S.scan_n   = 0;
    S.scanning = 1;
    if (prev != WIFI_RUN) S.state = WIFI_SCANNING;

    int nch = radio->max_channel > 0 ? radio->max_channel : 1;
    int dwell = ms / nch;
    if (dwell < 60) dwell = 60;

    uint8_t probe[128];
    for (int ch = 1; ch <= nch; ch++) {
        if (radio->ops->set_channel) radio->ops->set_channel(radio, ch);
        int n = ieee80211_build_probe_req(probe, radio->mac, 0);
        radio_tx(probe, (uint16_t)n);
        uint64_t until = pit_ticks() + (uint64_t)dwell;
        while (pit_ticks() < until) { poll_once(); __asm__ volatile("hlt"); }
    }

    S.scanning = 0;
    /* A scan retunes the radio; if we were associated, go back to the channel
     * the BSS is on or the association is over without anyone saying so. */
    if (prev == WIFI_RUN && radio->ops->set_channel)
        radio->ops->set_channel(radio, S.bss.channel);
    S.state = prev;
    return S.scan_n;
}

int wifi_scan_count(void) { return S.scan_n; }

const struct wifi_bss *wifi_scan_get(int i) {
    if (i < 0 || i >= S.scan_n) return 0;
    return &S.scan[i];
}

/* ------------------------------------------------------------- connect ---- */

static const struct wifi_bss *find_bss(const char *ssid) {
    const struct wifi_bss *best = 0;
    for (int i = 0; i < S.scan_n; i++) {
        if (strcmp(S.scan[i].ssid, ssid) != 0) continue;
        if (!best || S.scan[i].rssi > best->rssi) best = &S.scan[i];
    }
    return best;
}

const char *wifi_err_str(int rc) {
    switch (rc) {
    case 0:              return "connected";
    case WIFI_ENORADIO:  return "no radio bound";
    case WIFI_ENOTFOUND: return "network not found";
    case WIFI_EAUTH:     return "the access point refused the association";
    case WIFI_EKEY:      return "wrong passphrase";
    case WIFI_EUNSUPP:   return "unsupported security (WEP or WPA3-only)";
    case WIFI_ETIMEOUT:  return "timed out";
    default:             return "failed";
    }
}

int wifi_connect(const char *ssid, const char *psk, int timeout_ms) {
    if (!radio) return WIFI_ENORADIO;
    if (S.state != WIFI_IDLE) wifi_disconnect();

    const struct wifi_bss *b = find_bss(ssid);
    if (!b) {
        wifi_scan(1200);
        b = find_bss(ssid);
    }
    if (!b) return WIFI_ENOTFOUND;

    S.bss = *b;
    strlcpy(S.ssid, ssid, sizeof S.ssid);
    S.rssi = b->rssi;

    int need_key = 0;
    if (b->rsn) {
        if (b->akm == WIFI_AKM_SAE)  return WIFI_EUNSUPP;   /* no SAE here yet */
        if (!b->ccmp)                return WIFI_EUNSUPP;   /* TKIP only       */
        need_key = 1;
    } else if (b->privacy) {
        return WIFI_EUNSUPP;                                /* WEP is not security */
    }
    if (need_key && (!psk || !psk[0])) return WIFI_EKEY;

    if (radio->ops->set_channel) radio->ops->set_channel(radio, b->channel);

    /* 4096 iterations of HMAC-SHA1 before a single frame goes out. It is the
     * slowest thing in the join by a wide margin and there is nothing to be
     * done about that -- it is the point of PBKDF2. */
    if (need_key) wpa_pmk(psk, ssid, S.pmk);

    uint8_t mgmt[192];
    int n;

    /* Cleared here and not at the handshake below, which is where they used to
     * be: the AP sends message 1 the instant it accepts the association, and a
     * whole four-way exchange can finish inside the poll that was only waiting
     * for the association response. Resetting afterwards threw that away and
     * then waited six seconds for a message 3 that had already been answered,
     * so a correct passphrase reported itself as a wrong one. */
    S.hs_msg    = 0;
    S.hs_fail   = 0;
    S.auth_seen = 0;
    S.state     = WIFI_AUTH;
    n = ieee80211_build_auth(mgmt, radio->mac, S.bss.bssid, 1);
    radio_tx(mgmt, (uint16_t)n);
    if (!wait_for(&S.auth_seen, 800)) {
        n = ieee80211_build_auth(mgmt, radio->mac, S.bss.bssid, 1);
        radio_tx(mgmt, (uint16_t)n);                 /* one retry; frames get lost */
        if (!wait_for(&S.auth_seen, 800)) { link_down(); return WIFI_ETIMEOUT; }
    }
    if (S.auth_seen < 0) { link_down(); return WIFI_EAUTH; }

    S.assoc_seen = 0;
    S.state      = WIFI_ASSOC;
    n = ieee80211_build_assoc_req(mgmt, radio->mac, S.bss.bssid, ssid, need_key);
    radio_tx(mgmt, (uint16_t)n);
    if (!wait_for(&S.assoc_seen, 1000)) { link_down(); return WIFI_ETIMEOUT; }
    if (S.assoc_status != 0) {
        kprintf("[wifi] association refused, status %u\n", (unsigned)S.assoc_status);
        link_down();
        return WIFI_EAUTH;
    }

    if (need_key) {
        S.state = WIFI_4WAY;
        int left = timeout_ms > 0 ? timeout_ms : 6000;
        /* The AP sends message 1 unprompted; all this side does is wait for it
         * and then answer. A wrong passphrase shows up as message 3 never
         * arriving, because the AP cannot verify our MIC -- so the timeout is
         * the error message, which is why it is not a long one. */
        uint64_t deadline = pit_ticks() + (uint64_t)left;
        while (pit_ticks() < deadline && S.hs_msg < 4 && !S.hs_fail) {
            poll_once();
            __asm__ volatile("hlt");
        }
        if (S.hs_fail || S.hs_msg < 4) { link_down(); return WIFI_EKEY; }
    }

    S.state         = WIFI_RUN;
    wlan_if.link_up = 1;
    kprintf("[wifi] joined %s on channel %u (%s)\n", S.ssid, (unsigned)S.bss.channel,
            need_key ? "WPA2-PSK/CCMP" : "open");
    return 0;
}

void wifi_disconnect(void) {
    if (!radio || S.state == WIFI_IDLE) return;
    uint8_t mgmt[64];
    int n = ieee80211_build_deauth(mgmt, radio->mac, S.bss.bssid, 3);  /* leaving */
    radio_tx(mgmt, (uint16_t)n);
    link_down();
    S.ssid[0] = 0;
}

enum wifi_state wifi_get_state(void) { return S.state; }

const struct wifi_bss *wifi_current(void) {
    return S.state == WIFI_RUN ? &S.bss : 0;
}

struct netif *wifi_netif(void) { return radio ? &wlan_if : 0; }

int wifi_signal(void) { return S.state >= WIFI_AUTH ? S.rssi : 0; }

void wifi_sta_poll(void) { poll_once(); }

/* ------------------------------------------------------------ registry ---- */

static void wlan_poll(struct netif *nif) { (void)nif; poll_once(); }

void wifi_radio_register(struct wifi_radio *r) {
    if (radio) {
        kprintf("[wifi] %s ignored: %s is already bound\n", r->name, radio->name);
        return;
    }
    radio = r;
    memset(&S, 0, sizeof S);
    if (r->ops->start && r->ops->start(r) != 0) {
        kprintf("[wifi] %s failed to start\n", r->name);
        radio = 0;
        return;
    }

    memset(&wlan_if, 0, sizeof wlan_if);
    memcpy(wlan_if.mac, r->mac, ETH_ALEN);
    wlan_if.name[0] = 'w'; wlan_if.name[1] = 'l'; wlan_if.name[2] = 'a';
    wlan_if.name[3] = 'n'; wlan_if.name[4] = '0'; wlan_if.name[5] = 0;
    wlan_if.send    = wlan_send;
    wlan_if.poll    = wlan_poll;
    wlan_if.link_up = 0;                    /* nothing carries traffic until we join */
    if (!wlan_registered) { netif_register(&wlan_if); wlan_registered = 1; }
    kprintf("[wifi] %s: %s, %d channels\n", r->name, r->chip ? r->chip : "radio",
            r->max_channel);
}

struct wifi_radio *wifi_radio_get(void) { return radio; }

void wifi_autoconnect(void) {
    if (!radio || netcfg_wifi_count() == 0) return;

    kprintf("[wifi] scanning for a remembered network\n");
    wifi_scan(1200);
    for (int i = 0; i < netcfg_wifi_count(); i++) {
        const char *ssid = netcfg_wifi_ssid(i);
        if (!ssid || !find_bss(ssid)) continue;
        char psk[80];
        if (!netcfg_wifi_find(ssid, psk, sizeof psk)) continue;
        int rc = wifi_connect(ssid, psk, 6000);
        memset(psk, 0, sizeof psk);
        if (rc == 0) return;
        kprintf("[wifi] %s: %s\n", ssid, wifi_err_str(rc));
    }
}
