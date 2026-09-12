#include <stdint.h>
#include "wifi.h"
#include "wpa.h"
#include "net.h"
#include "netif.h"
#include "crypto.h"
#include "string.h"
#include "kprintf.h"
#include "pit.h"

/* A radio, and the access point on the other side of it.
 *
 * Why this exists, plainly: QEMU emulates no 802.11 device of any kind, and
 * there is no Wi-Fi card in the machine this is developed on. A driver for a
 * real chipset could be written from the datasheets and would never once be
 * executed -- and several thousand lines of register programming that has
 * never run, marked done, is worse than no Wi-Fi and an honest note saying so.
 *
 * So the radio is the part that gets replaced, and everything above it gets
 * run. This file implements struct wifi_ops the same way an ath9k driver would
 * -- tune, transmit, hand frames up -- and behind it an access point that
 * beacons, authenticates, associates, runs the authenticator half of the WPA2
 * four-way handshake and encrypts with CCMP. The station in net/wifi_sta.c
 * cannot tell the difference, because there is nothing for it to tell: the
 * frames are real 802.11 frames built by the same net/wifi.c the station uses,
 * and the keys are real keys from the same net/wpa.c the vectors check. This
 * is what mac80211_hwsim is to Linux and what wpa_supplicant's whole test
 * suite runs against.
 *
 * Frames the station sends to the AP for somewhere else are relayed out the
 * wired interface, and frames arriving there for the station come back through
 * the air -- so a DHCP lease, a DNS lookup and an HTTPS page fetch all travel
 * the 802.11 path end to end, encrypted, and the browser cannot tell either.
 *
 * The two things this is NOT: it is not a radio (no PHY, no channel loss, no
 * hidden nodes, no rate control), and it is not a second Wi-Fi stack -- the AP
 * side here is ~250 lines because the frame formats and the crypto are shared
 * with the station rather than reimplemented to agree with it. */

#define SIM_CHANNEL   6
#define SIM_QUEUE     24
#define BEACON_MS     100

static const char *const default_ssid = "BoltNet";
static const char *const default_psk  = "boltos-test-key";

static struct {
    struct wifi_radio radio;
    int               up;
    int               tuned;                  /* the channel the STA is on */

    struct netif     *uplink;                 /* the wire the AP bridges to */
    char              ssid[33];
    char              psk[64];
    uint8_t           bssid[6];
    uint8_t           pmk[WPA_PMK_LEN];
    uint8_t           gtk[WPA_TK_LEN];

    /* The one associated station. A second one would need a table; there is
     * exactly one 802.11 station on this machine, so there is no table. */
    uint8_t  sta[6];
    int      authed, assoced, keyed;
    uint8_t  anonce[WPA_NONCE_LEN], snonce[WPA_NONCE_LEN];
    uint8_t  ptk[WPA_PTK_LEN];
    uint8_t  replay[8];
    uint64_t tx_pn, gtk_pn;
    uint16_t seq;
    uint64_t next_beacon;

    struct { uint16_t len; uint8_t buf[WIFI_FRAME_MAX]; } q[SIM_QUEUE];
    int qhead, qtail, qdrop;
} AP;

/* ------------------------------------------------------------- the air ---- */

/* Put a frame on the air towards the station. It is only heard if the station
 * is tuned to this channel, which is what makes a scan a scan rather than a
 * formality. */
static void air_tx(const uint8_t *frame, uint16_t len) {
    if (AP.tuned != SIM_CHANNEL) return;
    int next = (AP.qtail + 1) % SIM_QUEUE;
    if (next == AP.qhead) { AP.qdrop++; return; }
    if (len > WIFI_FRAME_MAX) return;
    memcpy(AP.q[AP.qtail].buf, frame, len);
    AP.q[AP.qtail].len = len;
    AP.qtail = next;
}

static uint8_t apbuf[WIFI_FRAME_MAX];
static uint8_t apenc[WIFI_FRAME_MAX];

/* Wrap an Ethernet frame for the station: FromDS, and under the pairwise key
 * if it is addressed to it or the group key if it is addressed to everyone. */
static void air_tx_eth(const uint8_t *eth, uint16_t len) {
    if (len < ETH_HDR_LEN) return;
    int n = ieee80211_eth_to_data(eth, len, AP.bssid, AP.seq++, 0, apbuf, sizeof apbuf);
    if (n < 0) return;
    if (!AP.keyed) { air_tx(apbuf, (uint16_t)n); return; }

    int group = (eth[0] & 1) != 0;
    const uint8_t *key = group ? AP.gtk : AP.ptk + 32;
    uint64_t pn = group ? ++AP.gtk_pn : ++AP.tx_pn;
    int m = ccmp_encrypt(key, pn, apbuf, 24, apbuf + 24, (uint32_t)(n - 24),
                         apenc, sizeof apenc);
    if (m > 0) air_tx(apenc, (uint16_t)m);
}

static uint8_t eapframe[ETH_HDR_LEN + 4 + EAPOL_KEY_FIXED + 64];

static void ap_send_eapol(const uint8_t *body, uint16_t len) {
    struct eth_hdr *e = (struct eth_hdr *)eapframe;
    memcpy(e->dst, AP.sta, 6);
    memcpy(e->src, AP.bssid, 6);
    e->type = htons(0x888E);
    memcpy(eapframe + ETH_HDR_LEN, body, len);

    /* EAPOL is never protected, even after the keys exist: the station has not
     * installed them until message 4 is sent. */
    int keyed = AP.keyed;
    AP.keyed = 0;
    air_tx_eth(eapframe, (uint16_t)(ETH_HDR_LEN + len));
    AP.keyed = keyed;
}

/* ------------------------------------------------ the authenticator half --- */

static void bump_replay(void) {
    for (int i = 7; i >= 0; i--) if (++AP.replay[i]) break;
}

static void send_msg1(void) {
    uint8_t buf[4 + EAPOL_KEY_FIXED];
    memset(buf, 0, sizeof buf);
    struct eapol_hdr *eh = (struct eapol_hdr *)buf;
    eh->version = EAPOL_VERSION;
    eh->type    = EAPOL_TYPE_KEY;
    eh->length  = htons(EAPOL_KEY_FIXED);

    crypto_rand_bytes(AP.anonce, WPA_NONCE_LEN);
    bump_replay();

    struct eapol_key *k = (struct eapol_key *)(buf + 4);
    k->desc_type  = EAPOL_DESC_RSN;
    k->key_info   = htons(KI_PAIRWISE | KI_ACK | KI_VER_HMAC_SHA1);
    k->key_length = htons(WPA_TK_LEN);
    memcpy(k->replay, AP.replay, 8);
    memcpy(k->nonce, AP.anonce, WPA_NONCE_LEN);
    ap_send_eapol(buf, sizeof buf);
}

/* Message 3 carries the group key, wrapped under the KEK. */
static void send_msg3(void) {
    uint8_t kde[8 + WPA_TK_LEN];              /* KDE header + the GTK       */
    kde[0] = 0xDD;
    kde[1] = (uint8_t)(6 + WPA_TK_LEN);
    kde[2] = 0x00; kde[3] = 0x0F; kde[4] = 0xAC; kde[5] = 1;   /* GTK KDE   */
    kde[6] = 1;                                                 /* key id 1 */
    kde[7] = 0;
    memcpy(kde + 8, AP.gtk, WPA_TK_LEN);

    /* Key wrap grows by 8 and needs a multiple of 8 going in; the KDE above is
     * 24 bytes, which is why there is no padding here. */
    uint8_t wrapped[sizeof kde + 8];
    aes_key_wrap(AP.ptk + 16, kde, sizeof kde, wrapped);

    uint8_t buf[4 + EAPOL_KEY_FIXED + sizeof wrapped];
    memset(buf, 0, sizeof buf);
    uint16_t total = (uint16_t)(EAPOL_KEY_FIXED + sizeof wrapped);
    struct eapol_hdr *eh = (struct eapol_hdr *)buf;
    eh->version = EAPOL_VERSION;
    eh->type    = EAPOL_TYPE_KEY;
    eh->length  = htons(total);

    bump_replay();
    struct eapol_key *k = (struct eapol_key *)(buf + 4);
    k->desc_type  = EAPOL_DESC_RSN;
    k->key_info   = htons(KI_PAIRWISE | KI_MIC | KI_ACK | KI_INSTALL |
                          KI_SECURE | KI_ENCRYPTED | KI_VER_HMAC_SHA1);
    k->key_length = htons(WPA_TK_LEN);
    memcpy(k->replay, AP.replay, 8);
    memcpy(k->nonce, AP.anonce, WPA_NONCE_LEN);
    k->data_len   = htons((uint16_t)sizeof wrapped);
    memcpy(k->data, wrapped, sizeof wrapped);

    wpa_eapol_mic(AP.ptk, KI_VER_HMAC_SHA1, buf, sizeof buf, k->mic);
    ap_send_eapol(buf, (uint16_t)sizeof buf);
}

static void ap_eapol_input(const uint8_t *body, uint16_t len) {
    if (len < 4 + EAPOL_KEY_FIXED) return;
    const struct eapol_key *k = (const struct eapol_key *)(body + 4);
    uint16_t ki = ntohs(k->key_info);
    if (!(ki & KI_MIC)) return;                   /* only a station sends those */

    if (!(ki & KI_SECURE)) {                      /* message 2 */
        memcpy(AP.snonce, k->nonce, WPA_NONCE_LEN);
        wpa_ptk(AP.pmk, AP.bssid, AP.sta, AP.anonce, AP.snonce, AP.ptk);
        if (!wpa_eapol_mic_ok(AP.ptk, ki & KI_DESC_VER, body, len)) {
            /* The station proved it does not have the passphrase. Saying
             * nothing is the correct answer -- there is no "wrong password"
             * frame in 802.11, which is why a wrong key looks like a timeout
             * from the other end. */
            kprintf("[wifi-ap] message 2 MIC failed: wrong passphrase\n");
            return;
        }
        send_msg3();
        return;
    }

    /* Message 4. Once it verifies, both ends have the same PTK and everything
     * after this point is encrypted. */
    if (!wpa_eapol_mic_ok(AP.ptk, ki & KI_DESC_VER, body, len)) return;
    AP.keyed  = 1;
    AP.tx_pn  = 0;
    AP.gtk_pn = 0;
    kprintf("[wifi-ap] %s: four-way handshake complete, CCMP up\n", AP.ssid);
}

/* --------------------------------------------------- frames from the STA --- */

static uint8_t rxplain[WIFI_FRAME_MAX];

static void ap_data(const uint8_t *frame, uint16_t len) {
    const uint8_t *use = frame;
    uint16_t uselen = len;
    uint16_t fc = (uint16_t)(frame[0] | (frame[1] << 8));

    if (fc & FC_PROTECTED) {
        if (!AP.keyed) return;
        int n = ccmp_decrypt(AP.ptk + 32, frame, len, rxplain + 24,
                             (uint32_t)(sizeof rxplain - 24), 0);
        if (n < 0) return;
        memcpy(rxplain, frame, 24);
        rxplain[1] &= (uint8_t)~(FC_PROTECTED >> 8);
        use    = rxplain;
        uselen = (uint16_t)(24 + n);
    }

    if (ieee80211_snap_ethertype(use, uselen, 24) == 0x888E) {
        ap_eapol_input(use + 24 + sizeof(struct llc_snap),
                       (uint16_t)(uselen - 24 - sizeof(struct llc_snap)));
        return;
    }
    if (!AP.assoced || !AP.uplink) return;

    /* Out the wire, unchanged. The station's MAC is the wire's MAC, so what
     * leaves here is byte for byte what would have left without any of this --
     * which is the point: slirp, the DHCP server and every host on the other
     * side see an ordinary Ethernet machine. */
    uint8_t eth[ETH_FRAME_MAX];
    int n = ieee80211_data_to_eth(use, uselen, eth, sizeof eth);
    if (n > 0) netif_send(AP.uplink, eth, (uint16_t)n);
}

static void ap_mgmt(const uint8_t *frame, uint16_t len) {
    const struct ieee80211_hdr *h = (const struct ieee80211_hdr *)frame;
    uint16_t st = h->frame_control & FC_SUBTYPE_MASK;
    uint8_t out[256];
    int n;

    if (st == MGMT_PROBE_REQ) {
        n = ieee80211_build_beacon(out, AP.bssid, AP.ssid, SIM_CHANNEL, 1, 1, h->addr2);
        air_tx(out, (uint16_t)n);
        return;
    }
    if (st == MGMT_AUTH) {
        memcpy(AP.sta, h->addr2, 6);
        AP.authed  = 1;
        AP.assoced = 0;
        AP.keyed   = 0;
        n = ieee80211_build_mgmt_resp(out, MGMT_AUTH, AP.bssid, AP.sta, 0, 0);
        air_tx(out, (uint16_t)n);
        return;
    }
    if (st == MGMT_ASSOC_REQ) {
        if (!AP.authed || memcmp(h->addr2, AP.sta, 6) != 0) {
            /* 802.11 status 1: unspecified failure. An association from a
             * station that never authenticated is refused rather than ignored,
             * so the other end gets an error instead of a timeout. */
            n = ieee80211_build_mgmt_resp(out, MGMT_ASSOC_RESP, AP.bssid, h->addr2, 1, 0);
            air_tx(out, (uint16_t)n);
            return;
        }
        AP.assoced = 1;
        n = ieee80211_build_mgmt_resp(out, MGMT_ASSOC_RESP, AP.bssid, AP.sta, 0, 1);
        air_tx(out, (uint16_t)n);
        kprintf("[wifi-ap] %s associated\n", AP.ssid);
        send_msg1();
        return;
    }
    if (st == MGMT_DEAUTH || st == MGMT_DISASSOC) {
        AP.authed = AP.assoced = AP.keyed = 0;
        (void)len;
    }
}

/* ------------------------------------------------------------ the bridge --- */

/* Installed on the wired interface while a station is associated: a frame for
 * the station goes over the air instead of up the wired stack. */
static int uplink_hook(struct netif *nif, const uint8_t *frame, uint16_t len) {
    (void)nif;
    if (!AP.assoced || !AP.keyed || len < ETH_HDR_LEN) return 0;
    int group = (frame[0] & 1) != 0;
    if (!group && memcmp(frame, AP.sta, 6) != 0) return 0;
    air_tx_eth(frame, len);
    return 1;
}

/* ---------------------------------------------------------- struct wifi_ops */

static int sim_start(struct wifi_radio *r) {
    (void)r;
    AP.up    = 1;
    AP.tuned = SIM_CHANNEL;
    return 0;
}

static int sim_set_channel(struct wifi_radio *r, int channel) {
    (void)r;
    AP.tuned = channel;
    return 0;
}

static int sim_tx(struct wifi_radio *r, const void *frame, uint16_t len) {
    (void)r;
    if (!AP.up || len < sizeof(struct ieee80211_hdr)) return -1;
    if (AP.tuned != SIM_CHANNEL) return 0;         /* transmitted into the void */
    const uint8_t *f = (const uint8_t *)frame;
    uint16_t fc = (uint16_t)(f[0] | (f[1] << 8));
    if ((fc & FC_TYPE_MASK) == FC_TYPE_MGMT)      ap_mgmt(f, len);
    else if ((fc & FC_TYPE_MASK) == FC_TYPE_DATA) ap_data(f, len);
    return 0;
}

static void sim_poll(struct wifi_radio *r) {
    if (!AP.up) return;

    uint64_t now = pit_ticks();
    if (now >= AP.next_beacon) {
        AP.next_beacon = now + BEACON_MS;
        uint8_t b[256];
        int n = ieee80211_build_beacon(b, AP.bssid, AP.ssid, SIM_CHANNEL, 1, 0, 0);
        air_tx(b, (uint16_t)n);
    }

    /* Drain what is on the air. The queue is emptied by index rather than
     * while(head!=tail) on purpose: wifi_rx can transmit, which can queue, and
     * a self-feeding loop here would never return to the scheduler. */
    int budget = SIM_QUEUE;
    while (AP.qhead != AP.qtail && budget-- > 0) {
        int i = AP.qhead;
        AP.qhead = (AP.qhead + 1) % SIM_QUEUE;
        wifi_rx(r, AP.q[i].buf, AP.q[i].len, -42);
    }

    /* Follow the wire: the bridge is only in the way while there is a station
     * to bridge to. */
    if (AP.uplink) AP.uplink->rx_hook = (AP.assoced && AP.keyed) ? uplink_hook : 0;
}

static const struct wifi_ops sim_ops = {
    .start       = sim_start,
    .set_channel = sim_set_channel,
    .tx          = sim_tx,
    .poll        = sim_poll,
    .stop        = 0,
};

/* ------------------------------------------------------------------ setup --- */

const char *wifi_sim_ssid(void) { return AP.ssid; }

int wifi_sim_start(const char *ssid, const char *psk) {
    if (AP.up) return 0;
    struct wifi_radio *bound = wifi_radio_get();
    if (bound && bound != &AP.radio) return -1;      /* a real radio is bound */

    struct netif *wire = 0;
    for (struct netif *n = netif_list(); n; n = n->next)
        if (n->send && n->name[0] == 'e') wire = n;   /* eth0, whichever driver */
    if (!wire) return -2;
    AP.uplink = wire;

    strlcpy(AP.ssid, ssid && ssid[0] ? ssid : default_ssid, sizeof AP.ssid);
    strlcpy(AP.psk,  psk  && psk[0]  ? psk  : default_psk,  sizeof AP.psk);
    wpa_pmk(AP.psk, AP.ssid, AP.pmk);
    crypto_rand_bytes(AP.gtk, WPA_TK_LEN);

    /* A locally-administered BSSID, so it cannot collide with a real vendor's.
     * The station's own MAC is the wire's, which is what keeps the far side of
     * the bridge none the wiser. */
    AP.bssid[0] = 0x02; AP.bssid[1] = 0x00; AP.bssid[2] = 0x00;
    AP.bssid[3] = 0x62; AP.bssid[4] = 0x6F; AP.bssid[5] = 0x6C;   /* "bol" */

    if (bound == &AP.radio) {                        /* stopped earlier; re-arm */
        AP.up = 1;
        AP.tuned = SIM_CHANNEL;
        return 0;
    }

    memset(&AP.radio, 0, sizeof AP.radio);
    strlcpy(AP.radio.name, "wlan0", sizeof AP.radio.name);
    memcpy(AP.radio.mac, wire->mac, 6);
    AP.radio.chip        = "built-in 802.11 (no PHY; bridged to the wire)";
    AP.radio.max_channel = 11;
    AP.radio.ops         = &sim_ops;
    wifi_radio_register(&AP.radio);
    return wifi_radio_get() == &AP.radio ? 0 : -3;
}

void wifi_sim_stop(void) {
    if (!AP.up) return;
    if (AP.uplink) AP.uplink->rx_hook = 0;
    AP.up = AP.authed = AP.assoced = AP.keyed = 0;
}

int wifi_sim_running(void) { return AP.up; }
