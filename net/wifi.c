#include <stdint.h>
#include "wifi.h"
#include "net.h"        /* struct eth_hdr, ETH_HDR_LEN */
#include "string.h"

/* 802.11 MAC-layer helpers -- pure frame format, no radio/register/DMA
 * knowledge, that's the per-chip driver's job. A driver feeds raw 802.11
 * frames in here and gets Ethernet frames and parsed BSS rows out; the station
 * in net/wifi_sta.c builds its management frames with the other half.
 *
 * Nothing in this file holds state or touches hardware, which is why the AP
 * side of drivers/wifi_sim.c can use the same builders in the other direction
 * without a second copy of the frame formats to disagree with this one. */

int ieee80211_data_to_eth(const uint8_t *frame, uint16_t len,
                          uint8_t *out, uint16_t out_cap) {
    if (len < sizeof(struct ieee80211_hdr) + sizeof(struct llc_snap)) return -1;
    const struct ieee80211_hdr *h = (const struct ieee80211_hdr *)frame;
    uint16_t fc = h->frame_control;
    if ((fc & FC_TYPE_MASK) != FC_TYPE_DATA) return -1;

    /* 3-address forms map ToDS/FromDS to which header address is DA vs SA. */
    const uint8_t *da, *sa;
    switch (fc & (FC_TODS | FC_FROMDS)) {
    case 0:            da = h->addr1; sa = h->addr2; break;   /* IBSS            */
    case FC_FROMDS:    da = h->addr1; sa = h->addr3; break;   /* AP -> STA       */
    case FC_TODS:      da = h->addr3; sa = h->addr2; break;   /* STA -> AP       */
    default:           return -1;                            /* 4-addr WDS: skip */
    }

    const struct llc_snap *snap =
        (const struct llc_snap *)(frame + sizeof(struct ieee80211_hdr));
    if (snap->dsap != LLC_SNAP_AA || snap->ssap != LLC_SNAP_AA) return -1;

    const uint8_t *payload = (const uint8_t *)snap + sizeof(struct llc_snap);
    uint16_t paylen = (uint16_t)(len - sizeof(struct ieee80211_hdr)
                                     - sizeof(struct llc_snap));
    if ((uint32_t)ETH_HDR_LEN + paylen > out_cap) return -1;

    struct eth_hdr *e = (struct eth_hdr *)out;
    memcpy(e->dst, da, IEEE80211_ADDR_LEN);
    memcpy(e->src, sa, IEEE80211_ADDR_LEN);
    e->type = snap->ethertype;            /* SNAP ethertype is already net order */
    memcpy(out + ETH_HDR_LEN, payload, paylen);
    return ETH_HDR_LEN + paylen;
}

int ieee80211_parse_beacon(const uint8_t *frame, uint16_t len, struct wifi_bss *bss) {
    uint32_t hdrlen = sizeof(struct ieee80211_hdr);
    if (len < hdrlen + sizeof(struct ieee80211_beacon)) return -1;
    const struct ieee80211_hdr *h = (const struct ieee80211_hdr *)frame;
    uint16_t fc = h->frame_control;
    if ((fc & FC_TYPE_MASK) != FC_TYPE_MGMT) return -1;
    uint16_t st = fc & FC_SUBTYPE_MASK;
    if (st != MGMT_BEACON && st != MGMT_PROBE_RESP) return -1;

    memset(bss, 0, sizeof(*bss));
    memcpy(bss->bssid, h->addr3, IEEE80211_ADDR_LEN);
    const struct ieee80211_beacon *b =
        (const struct ieee80211_beacon *)(frame + hdrlen);
    bss->capability = b->capability;
    bss->privacy    = (b->capability & CAP_PRIVACY) ? 1 : 0;

    /* Walk tagged IEs for SSID, channel and RSN; ignore the rest. */
    uint32_t off = hdrlen + sizeof(struct ieee80211_beacon);
    while (off + 2 <= len) {
        uint8_t id = frame[off], ilen = frame[off + 1];
        if (off + 2u + ilen > len) break;
        const uint8_t *d = frame + off + 2;
        if (id == IE_SSID && ilen <= 32) { memcpy(bss->ssid, d, ilen); bss->ssid[ilen] = 0; }
        else if (id == IE_DSPARAM && ilen >= 1) bss->channel = d[0];
        else if (id == IE_RSN) ieee80211_parse_rsn(d, ilen, bss);
        off += 2u + ilen;
    }
    return 0;
}

/* RSN element: version, group cipher, then counted lists of pairwise ciphers
 * and AKM suites, each a 4-byte OUI+type. Anything shorter than it claims is
 * dropped rather than read past -- this is the one part of a beacon that
 * decides whether traffic gets encrypted, and beacons come from strangers. */
int ieee80211_parse_rsn(const uint8_t *ie, uint8_t len, struct wifi_bss *bss) {
    if (len < 2) return 0;
    uint16_t ver = (uint16_t)(ie[0] | (ie[1] << 8));
    if (ver != 1) return 0;
    bss->rsn = 1;

    uint32_t off = 2;
    if (off + 4 > len) return 1;                    /* group cipher only */
    off += 4;

    if (off + 2 > len) return 1;
    uint16_t npair = (uint16_t)(ie[off] | (ie[off+1] << 8));
    off += 2;
    for (uint16_t i = 0; i < npair && off + 4 <= len; i++, off += 4)
        if (ie[off] == 0x00 && ie[off+1] == 0x0F && ie[off+2] == 0xAC && ie[off+3] == 4)
            bss->ccmp = 1;

    if (off + 2 > len) return 1;
    uint16_t nakm = (uint16_t)(ie[off] | (ie[off+1] << 8));
    off += 2;
    for (uint16_t i = 0; i < nakm && off + 4 <= len; i++, off += 4) {
        if (ie[off] != 0x00 || ie[off+1] != 0x0F || ie[off+2] != 0xAC) continue;
        /* SAE wins if both are offered: a WPA2/WPA3 transition network is one
         * this cannot join with SAE, so what matters is noticing it is there
         * rather than reporting the weaker half and joining on it. */
        if      (ie[off+3] == 8) bss->akm = WIFI_AKM_SAE;
        else if (ie[off+3] == 6 && bss->akm != WIFI_AKM_SAE) bss->akm = WIFI_AKM_PSK_SHA256;
        else if (ie[off+3] == 2 && bss->akm == WIFI_AKM_NONE) bss->akm = WIFI_AKM_PSK;
    }
    return 1;
}

int ieee80211_build_probe_req(uint8_t *out, const uint8_t src[6], const char *ssid) {
    static const uint8_t bcast[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    struct ieee80211_hdr *h = (struct ieee80211_hdr *)out;
    memset(h, 0, sizeof(*h));
    h->frame_control = FC_TYPE_MGMT | MGMT_PROBE_REQ;
    memcpy(h->addr1, bcast, 6);    /* DA    */
    memcpy(h->addr2, src,   6);    /* SA    */
    memcpy(h->addr3, bcast, 6);    /* BSSID */
    uint32_t off = sizeof(*h);

    /* SSID IE (empty len = wildcard probe). */
    uint8_t slen = 0;
    if (ssid) while (ssid[slen] && slen < 32) slen++;
    out[off++] = IE_SSID; out[off++] = slen;
    for (uint8_t i = 0; i < slen; i++) out[off++] = (uint8_t)ssid[i];

    /* Supported-rates IE: 1/2/5.5/11 Mbps, high bit = basic rate. */
    static const uint8_t rates[] = { 0x82, 0x84, 0x8B, 0x96 };
    out[off++] = IE_RATES; out[off++] = (uint8_t)sizeof(rates);
    for (uint32_t i = 0; i < sizeof(rates); i++) out[off++] = rates[i];

    return (int)off;
}

const char *wifi_state_name(enum wifi_state s) {
    switch (s) {
    case WIFI_IDLE:     return "idle";
    case WIFI_SCANNING: return "scanning";
    case WIFI_AUTH:     return "authenticating";
    case WIFI_ASSOC:    return "associating";
    case WIFI_4WAY:     return "handshaking";
    case WIFI_RUN:      return "connected";
    default:            return "?";
    }
}

/* The RSN element a station puts in its association request: version 1, CCMP
 * as both the group and pairwise cipher, PSK as the only AKM. A fixed 20
 * bytes, because there is exactly one thing this station can negotiate and
 * offering a menu it cannot cook from would be a lie the AP acts on. */
static const uint8_t rsn_ie_psk_ccmp[] = {
    IE_RSN, 0x14,
    0x01, 0x00,                             /* version 1                    */
    0x00,0x0F,0xAC, 0x04,                   /* group cipher:    CCMP        */
    0x01,0x00, 0x00,0x0F,0xAC, 0x04,        /* 1 pairwise:      CCMP        */
    0x01,0x00, 0x00,0x0F,0xAC, 0x02,        /* 1 AKM:           PSK         */
    0x00,0x00                               /* RSN capabilities             */
};

static void mgmt_hdr(struct ieee80211_hdr *h, uint16_t subtype,
                     const uint8_t da[6], const uint8_t sa[6], const uint8_t bssid[6]) {
    memset(h, 0, sizeof(*h));
    h->frame_control = (uint16_t)(FC_TYPE_MGMT | subtype);
    memcpy(h->addr1, da, 6);
    memcpy(h->addr2, sa, 6);
    memcpy(h->addr3, bssid, 6);
}

int ieee80211_build_auth(uint8_t *out, const uint8_t src[6], const uint8_t bssid[6],
                         uint16_t seq) {
    mgmt_hdr((struct ieee80211_hdr *)out, MGMT_AUTH, bssid, src, bssid);
    uint32_t off = sizeof(struct ieee80211_hdr);
    out[off++] = 0; out[off++] = 0;         /* algorithm 0: open system     */
    out[off++] = (uint8_t)seq; out[off++] = (uint8_t)(seq >> 8);
    out[off++] = 0; out[off++] = 0;         /* status: success              */
    return (int)off;
}

int ieee80211_build_assoc_req(uint8_t *out, const uint8_t src[6], const uint8_t bssid[6],
                              const char *ssid, int rsn) {
    mgmt_hdr((struct ieee80211_hdr *)out, MGMT_ASSOC_REQ, bssid, src, bssid);
    uint32_t off = sizeof(struct ieee80211_hdr);
    uint16_t cap = 0x0001 | (rsn ? CAP_PRIVACY : 0);         /* ESS */
    out[off++] = (uint8_t)cap; out[off++] = (uint8_t)(cap >> 8);
    out[off++] = 0x0A; out[off++] = 0x00;                    /* listen interval */

    uint8_t slen = 0;
    if (ssid) while (ssid[slen] && slen < 32) slen++;
    out[off++] = IE_SSID; out[off++] = slen;
    for (uint8_t i = 0; i < slen; i++) out[off++] = (uint8_t)ssid[i];

    static const uint8_t rates[] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x18, 0x30, 0x48 };
    out[off++] = IE_RATES; out[off++] = (uint8_t)sizeof(rates);
    for (uint32_t i = 0; i < sizeof(rates); i++) out[off++] = rates[i];

    if (rsn) {
        memcpy(out + off, rsn_ie_psk_ccmp, sizeof(rsn_ie_psk_ccmp));
        off += sizeof(rsn_ie_psk_ccmp);
    }
    return (int)off;
}

int ieee80211_build_deauth(uint8_t *out, const uint8_t src[6], const uint8_t bssid[6],
                           uint16_t reason) {
    mgmt_hdr((struct ieee80211_hdr *)out, MGMT_DEAUTH, bssid, src, bssid);
    uint32_t off = sizeof(struct ieee80211_hdr);
    out[off++] = (uint8_t)reason; out[off++] = (uint8_t)(reason >> 8);
    return (int)off;
}

int ieee80211_build_beacon(uint8_t *out, const uint8_t bssid[6], const char *ssid,
                           uint8_t channel, int rsn, int probe_resp,
                           const uint8_t *da) {
    static const uint8_t bcast[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    mgmt_hdr((struct ieee80211_hdr *)out, probe_resp ? MGMT_PROBE_RESP : MGMT_BEACON,
             da ? da : bcast, bssid, bssid);
    uint32_t off = sizeof(struct ieee80211_hdr);

    struct ieee80211_beacon *b = (struct ieee80211_beacon *)(out + off);
    b->timestamp       = 0;                 /* the TSF nothing here reads */
    b->beacon_interval = 100;
    b->capability      = (uint16_t)(0x0001 | (rsn ? CAP_PRIVACY : 0));
    off += sizeof(*b);

    uint8_t slen = 0;
    if (ssid) while (ssid[slen] && slen < 32) slen++;
    out[off++] = IE_SSID; out[off++] = slen;
    for (uint8_t i = 0; i < slen; i++) out[off++] = (uint8_t)ssid[i];

    static const uint8_t rates[] = { 0x82, 0x84, 0x8B, 0x96 };
    out[off++] = IE_RATES; out[off++] = (uint8_t)sizeof(rates);
    for (uint32_t i = 0; i < sizeof(rates); i++) out[off++] = rates[i];

    out[off++] = IE_DSPARAM; out[off++] = 1; out[off++] = channel;

    if (rsn) {
        memcpy(out + off, rsn_ie_psk_ccmp, sizeof(rsn_ie_psk_ccmp));
        off += sizeof(rsn_ie_psk_ccmp);
    }
    return (int)off;
}

int ieee80211_build_mgmt_resp(uint8_t *out, uint16_t subtype, const uint8_t bssid[6],
                              const uint8_t da[6], uint16_t status, uint16_t aid) {
    mgmt_hdr((struct ieee80211_hdr *)out, subtype, da, bssid, bssid);
    uint32_t off = sizeof(struct ieee80211_hdr);
    if (subtype == MGMT_AUTH) {
        out[off++] = 0; out[off++] = 0;                      /* open system  */
        out[off++] = 2; out[off++] = 0;                      /* sequence 2   */
        out[off++] = (uint8_t)status; out[off++] = (uint8_t)(status >> 8);
        return (int)off;
    }
    /* association response */
    out[off++] = 0x01; out[off++] = 0x00;                    /* capability   */
    out[off++] = (uint8_t)status; out[off++] = (uint8_t)(status >> 8);
    out[off++] = (uint8_t)(aid | 0xC000); out[off++] = (uint8_t)((aid | 0xC000) >> 8);
    static const uint8_t rates[] = { 0x82, 0x84, 0x8B, 0x96 };
    out[off++] = IE_RATES; out[off++] = (uint8_t)sizeof(rates);
    for (uint32_t i = 0; i < sizeof(rates); i++) out[off++] = rates[i];
    return (int)off;
}

int ieee80211_eth_to_data(const uint8_t *eth, uint16_t ethlen,
                          const uint8_t bssid[6], uint16_t seq, int tods,
                          uint8_t *out, uint16_t out_cap) {
    if (ethlen < ETH_HDR_LEN) return -1;
    uint32_t need = sizeof(struct ieee80211_hdr) + sizeof(struct llc_snap)
                  + (uint32_t)(ethlen - ETH_HDR_LEN);
    if (need > out_cap) return -1;

    const struct eth_hdr *e = (const struct eth_hdr *)eth;
    struct ieee80211_hdr *h = (struct ieee80211_hdr *)out;
    memset(h, 0, sizeof(*h));
    h->frame_control = (uint16_t)(FC_TYPE_DATA | (tods ? FC_TODS : FC_FROMDS));
    if (tods) {                                    /* STA -> AP: A1 is the BSSID */
        memcpy(h->addr1, bssid,  6);
        memcpy(h->addr2, e->src, 6);
        memcpy(h->addr3, e->dst, 6);
    } else {                                       /* AP -> STA                  */
        memcpy(h->addr1, e->dst, 6);
        memcpy(h->addr2, bssid,  6);
        memcpy(h->addr3, e->src, 6);
    }
    h->seq_ctrl = (uint16_t)(seq << 4);

    struct llc_snap *s = (struct llc_snap *)(out + sizeof(*h));
    s->dsap = LLC_SNAP_AA; s->ssap = LLC_SNAP_AA; s->control = 0x03;
    s->oui[0] = s->oui[1] = s->oui[2] = 0;
    s->ethertype = e->type;                        /* already network order */

    memcpy(out + sizeof(*h) + sizeof(*s), eth + ETH_HDR_LEN, ethlen - ETH_HDR_LEN);
    return (int)need;
}

uint16_t ieee80211_snap_ethertype(const uint8_t *frame, uint16_t len, uint32_t hdrlen) {
    if (len < hdrlen + sizeof(struct llc_snap)) return 0;
    const struct llc_snap *s = (const struct llc_snap *)(frame + hdrlen);
    if (s->dsap != LLC_SNAP_AA || s->ssap != LLC_SNAP_AA) return 0;
    return (uint16_t)((s->ethertype >> 8) | (s->ethertype << 8));   /* to host order */
}
