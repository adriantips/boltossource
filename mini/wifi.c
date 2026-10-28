/* ===========================================================================
 *  BoltOS Mini  -  mini/wifi.c
 *
 *  Wi-Fi, on the UWE5622 (wcn.c). Its firmware is a "full MAC": it scans,
 *  joins and encrypts by itself, and talks to us in commands, events and
 *  Ethernet frames -- the protocol of Unisoc's Linux driver (sprdwl). What
 *  it leaves to the host is the WPA2 handshake: the four EAPOL-Key messages
 *  arrive as ordinary frames, net/wpa.c (the desktop's) does the keys, and
 *  the keys go back to the firmware with a command.
 *
 *    out, channel 8    commands: a 12-byte header (type, id, length, time)
 *    in,  channel 22   responses and events, behind the same header
 *    out, channel 10   frames: 3 bytes, an 11-byte descriptor, the frame
 *    in,  channel 24   frames: a 28-byte descriptor, the frame at its offset
 *
 *  Sending frames takes credits, which the firmware hands out in every
 *  received frame's descriptor and in "flow control" events.
 *
 *  Starting it: API versions, the RF calibration (built from Orange Pi's
 *  .ini by mini/tools/mkwifiini.py), the firmware's details (our MAC address
 *  among them), then a station interface. The link is "wlan0" to mini/net.c,
 *  and inet.c runs DHCP on it like on any other.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "wcn.h"
#include "wlan.h"
#include "wpa.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

#define CH_CMD_TX   8
#define CH_DATA_TX  10
#define CH_CMD_RX   22
#define CH_LOG_RX   23
#define CH_DATA_RX  24

enum { T_CMD, T_EVENT, T_DATA, T_DATA_SPECIAL };
enum {
    CMD_GET_INFO = 1, CMD_SET_REGDOM = 2, CMD_OPEN = 3, CMD_CLOSE = 4, CMD_POWER_SAVE = 5, CMD_SYNC_VERSION = 9,
    CMD_CONNECT = 10, CMD_SCAN = 11, CMD_DISCONNECT = 13, CMD_KEY = 14, CMD_SET_IE = 25,
    CMD_NOTIFY_IP = 26, CMD_DOWNLOAD_INI = 76,
};
enum {
    EV_CONNECT = 0x80, EV_DISCONNECT = 0x81, EV_SCAN_DONE = 0x82, EV_MGMT_FRAME = 0x83,
    EV_FLOWCON = 0xB3, EV_STA_LUT = 0xF5,
};
#define MODE_STATION    1
#define VALID           0x80
#define CIPHER_TKIP     3
#define CIPHER_CCMP     4
#define AKM_PSK         2
#define HDR             12               /* struct sprdwl_cmd_hdr */

extern const uint8_t _binary_wifi_ini_bin_start[], _binary_wifi_ini_bin_end[];

static netif_t wlan;
static struct {
    int state;
    char why[120];
    uint8_t mac[6], ctx;
    int no_credit;
    int flow[4];
    /* the command waiting for its response */
    volatile int wait_id, got;
    int8_t status;
    uint8_t rsp[400];
    int rsp_len;
    /* scanning */
    wifi_net_t nets[WIFI_MAXNETS];
    int nnets, scanning;
    uint64_t scan_end;
    /* the network */
    char ssid[33];
    wifi_net_t bss;
    uint8_t pmk[32];
    int lut, lut_ok;
    int8_t rssi;
    uint64_t deadline;
    int want_up;                         /* connected, keys in: raise the link in wifi_poll */
    /* the handshake */
    uint8_t anonce[32], snonce[32], ptk[48], replay[8], rsn_ie[24];
    int rsn_len, hs, hs_fail, keys_due;
    uint8_t gtk[32], gtk_rsc[8];
    int gtk_len, gtk_id;
    uint32_t rx_frames, tx_frames, tx_nocredit, eapol;
} F;

int wifi_state(void) { return F.state; }
const char *wifi_why(void) { return F.why; }
const char *wifi_ssid(void) { return F.state >= WIFI_CONNECTING ? F.ssid : ""; }
int wifi_rssi(void) { return F.rssi; }
netif_t *wifi_netif(void) { return wlan.name ? &wlan : 0; }
int wifi_scanning(void) { return F.scanning; }
int wifi_nets(const wifi_net_t **out) { *out = F.nets; return F.nnets; }

static void say(int state, const char *why) {
    F.state = state;
    snprintf(F.why, sizeof F.why, "%s", why);
    kprintf("[wifi] %s\n", why);
    shell_request_frame();
}

static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ---- commands -------------------------------------------------------------------------- */
static uint8_t cmdbuf[1600];

/* Send a command and wait for its response (the machine keeps running).
 * The response's payload lands in F.rsp. Its status, or -100 for no answer. */
static int command(int id, const void *payload, int len, int timeout_ms) {
    if (HDR + len > (int)sizeof cmdbuf) return -101;
    uint8_t *h = cmdbuf;
    memset(h, 0, HDR);
    h[0] = (uint8_t)(T_CMD | 1 << 4 | (F.ctx & 7) << 5);    /* a command, wanting a response */
    h[1] = (uint8_t)id;
    put16(h + 2, (uint16_t)(HDR + len));
    uint32_t ms = (uint32_t)now_ms();
    memcpy(h + 4, &ms, 4);
    if (len) memcpy(h + HDR, payload, (size_t)len);
    F.wait_id = id; F.got = 0; F.rsp_len = 0;
    if (wcn_send(CH_CMD_TX, h, HDR + len)) { F.wait_id = 0; return -102; }
    uint64_t end = now_ms() + (uint64_t)timeout_ms;
    while (!F.got && now_ms() < end) { mini_pump(); delay_us(100); }
    F.wait_id = 0;
    if (!F.got) { kprintf("[wifi] command %d: no answer\n", id); return -100; }
    if (F.status) kprintf("[wifi] command %d: status %d\n", id, F.status);
    return F.status;
}

/* CRC-16 of the calibration sections (cmdevt.c's CRC16). */
static uint16_t crc16(const uint8_t *p, int n) {
    static const uint16_t t[16] = { 0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
                                    0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400 };
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c = t[(p[i] ^ c) & 15] ^ (c >> 4);
        c = t[((p[i] >> 4) ^ c) & 15] ^ (c >> 4);
    }
    return c;
}

static int download_ini(int sec, const uint8_t *data, int len) {
    static uint8_t b[1500 + 8];
    if (len + 6 > (int)sizeof b) return -1;
    memset(b, 0, 4);
    b[0] = (uint8_t)sec;
    memcpy(b + 4, data, (size_t)len);
    uint16_t c = crc16(data, len);
    memcpy(b + 4 + len, &c, 2);
    /* Linux only stops on a CRC, index or length error; other statuses are notes. */
    int st = command(CMD_DOWNLOAD_INI, b, len + 6, 3000);
    return st == -9 || st == -10 || st == -11 || st == -100 ? -1 : 0;
}

/* ---- scanning --------------------------------------------------------------------------- */
/* A beacon or probe response: its name, channel and what it takes to join. */
static void scan_result(int channel, int8_t rssi, const uint8_t *f, int len) {
    if (len < 36) return;
    const uint8_t *bssid = f + 16;
    uint16_t capab = get16(f + 34);
    wifi_net_t n;
    memset(&n, 0, sizeof n);
    memcpy(n.bssid, bssid, 6);
    n.rssi = rssi;
    n.channel = (uint8_t)channel;
    n.security = (capab & 0x10) ? WIFI_SEC_OTHER : WIFI_SEC_OPEN;
    int rsn_psk = 0, rsn_other = 0, wpa1 = 0;
    for (int o = 36; o + 2 <= len; ) {
        int id = f[o], l = f[o + 1];
        const uint8_t *v = f + o + 2;
        if (o + 2 + l > len) break;
        if (id == 0 && l <= 32) { memcpy(n.ssid, v, (size_t)l); n.ssid[l] = 0; }
        else if (id == 3 && l >= 1) n.channel = v[0];
        else if (id == 48 && l >= 8) {
            /* version, group suite, pairwise suites, AKM suites */
            n.group = v[5];
            int np = get16(v + 6), p = 8 + 4 * np;
            if (p + 2 <= l) {
                int na = get16(v + p);
                for (int i = 0; i < na && p + 2 + 4 * i + 4 <= l; i++) {
                    const uint8_t *s = v + p + 2 + 4 * i;
                    if (s[0] == 0 && s[1] == 0x0F && s[2] == 0xAC && s[3] == AKM_PSK) rsn_psk = 1;
                    else rsn_other = 1;
                }
            }
        } else if (id == 221 && l >= 4 && v[0] == 0 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 1) wpa1 = 1;
        o += 2 + l;
    }
    if (rsn_psk) n.security = WIFI_SEC_WPA2;
    else if (rsn_other || wpa1) n.security = WIFI_SEC_OTHER;
    if (!n.ssid[0]) return;                                 /* hidden: nothing to show */
    for (int i = 0; i < F.nnets; i++)
        if (!strcmp(F.nets[i].ssid, n.ssid)) {
            if (n.rssi > F.nets[i].rssi) F.nets[i] = n;    /* the nearest one of its name */
            return;
        }
    if (F.nnets < WIFI_MAXNETS) F.nets[F.nnets++] = n;
}

int wifi_scan(void) {
    if (F.state < WIFI_READY || F.scanning) return -1;
    /* 2.4 GHz channels 1-13 as a bitmap, then the 5 GHz ones by number. */
    static const uint16_t five[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124,
                                     128, 132, 136, 140, 149, 153, 157, 161, 165 };
    uint8_t b[12 + 2 + sizeof five];
    memset(b, 0, sizeof b);
    uint32_t chans = 0x1FFF;
    memcpy(b, &chans, 4);
    /* reserved (4), no SSIDs (2), then the 5 GHz list */
    put16(b + 10, (uint16_t)(sizeof five / 2));
    memcpy(b + 12, five, sizeof five);
    if (F.state != WIFI_CONNECTED) F.nnets = 0;
    F.scanning = 1;
    F.scan_end = now_ms() + 10000;
    int st = command(CMD_SCAN, b, 12 + (int)sizeof five, 3000);
    if (st) { F.scanning = 0; return -1; }
    return 0;
}

/* ---- the frames -------------------------------------------------------------------------- */
static uint8_t txf[3 + 11 + 1600];

static int send_frame(const uint8_t *eth, int len) {
    if (len < 14 || len > 1600 || !F.lut_ok) return -1;
    int color = 0;
    if (!F.no_credit) {
        while (color < 4 && F.flow[color] <= 0) color++;
        if (color == 4) { F.tx_nocredit++; return -1; }
        F.flow[color]--;
    }
    uint8_t *d = txf + 3;                                   /* 3 bytes: the firmware wants it so */
    memset(txf, 0, 3 + 11);
    d[0] = (uint8_t)(T_DATA | (F.ctx & 7) << 5);
    d[1] = 11;                                              /* the frame starts after this descriptor */
    uint16_t type = (uint16_t)(eth[12] << 8 | eth[13]);
    if (type == 0x0806) d[2] = 1 << 2;                      /* ARP: a basic rate, no aggregation */
    put16(d + 3, (uint16_t)len);
    d[6] = (uint8_t)F.lut;
    d[7] = (uint8_t)color;
    memcpy(d + 11, eth, (size_t)len);
    if (wcn_send(CH_DATA_TX, txf, 3 + 11 + len)) return -1;
    F.tx_frames++;
    return 0;
}

static int wlan_tx(netif_t *nif, const void *frame, int len) {
    (void)nif;
    if (F.state != WIFI_CONNECTED) return -1;
    return send_frame(frame, len) ? -1 : 0;
}

/* ---- WPA2: the four-way handshake ---------------------------------------------------------- */
static void eapol_send(uint16_t key_info, const uint8_t *kd, int kdlen) {
    uint8_t f[14 + 4 + EAPOL_KEY_FIXED + 32];
    int total = EAPOL_KEY_FIXED + kdlen;
    memset(f, 0, sizeof f);
    memcpy(f, F.bss.bssid, 6);
    memcpy(f + 6, F.mac, 6);
    f[12] = 0x88; f[13] = 0x8E;
    uint8_t *b = f + 14;
    b[0] = EAPOL_VERSION; b[1] = EAPOL_TYPE_KEY; b[2] = (uint8_t)(total >> 8); b[3] = (uint8_t)total;
    struct eapol_key *k = (struct eapol_key *)(b + 4);
    k->desc_type = EAPOL_DESC_RSN;
    k->key_info = (uint16_t)(key_info >> 8 | key_info << 8);
    memcpy(k->replay, F.replay, 8);
    memcpy(k->nonce, F.snonce, 32);
    k->data_len = (uint16_t)(kdlen >> 8 | kdlen << 8);
    if (kdlen) memcpy(k->data, kd, (size_t)kdlen);
    wpa_eapol_mic(F.ptk, key_info & KI_DESC_VER, b, (uint32_t)(4 + total), k->mic);
    send_frame(f, 14 + 4 + total);
}

static void take_gtk(const uint8_t *kd, int len) {
    for (int o = 0; o + 2 <= len; ) {
        int id = kd[o], l = kd[o + 1];
        if (o + 2 + l > len) break;
        if (id == 0xDD && l >= 6 && kd[o + 2] == 0 && kd[o + 3] == 0x0F && kd[o + 4] == 0xAC && kd[o + 5] == 1) {
            F.gtk_id = kd[o + 6] & 3;
            F.gtk_len = l - 6 > 32 ? 32 : l - 6;
            memcpy(F.gtk, kd + o + 8, (size_t)F.gtk_len);
        }
        o += 2 + l;
    }
}

void crypto_rand_bytes(void *out, uint32_t n);

static void eapol_input(const uint8_t *b, int len) {
    F.eapol++;
    if (len < 4 + EAPOL_KEY_FIXED || b[1] != EAPOL_TYPE_KEY) return;
    const struct eapol_key *k = (const struct eapol_key *)(b + 4);
    uint16_t ki = (uint16_t)(k->key_info >> 8 | k->key_info << 8);
    uint16_t kdl = (uint16_t)(k->data_len >> 8 | k->data_len << 8);
    int ver = ki & KI_DESC_VER;
    if (4 + EAPOL_KEY_FIXED + kdl > len) return;
    if (!(ki & KI_PAIRWISE)) return;                        /* group rekeying: not yet */
    if (!(ki & KI_MIC)) {                                   /* message 1 */
        memcpy(F.anonce, k->nonce, 32);
        memcpy(F.replay, k->replay, 8);
        crypto_rand_bytes(F.snonce, 32);
        wpa_ptk(F.pmk, F.bss.bssid, F.mac, F.anonce, F.snonce, F.ptk);
        eapol_send((uint16_t)(KI_PAIRWISE | KI_MIC | ver), F.rsn_ie, F.rsn_len);
        F.hs = 2;
        kprintf("[wifi] handshake: message 1 in, 2 out\n");
        return;
    }
    if (!wpa_eapol_mic_ok(F.ptk, ver, b, (uint32_t)len)) { F.hs_fail = 1; kprintf("[wifi] handshake: message 3's MIC is wrong\n"); return; }
    if (memcmp(k->replay, F.replay, 8) <= 0) return;
    memcpy(F.replay, k->replay, 8);
    memcpy(F.gtk_rsc, k->rsc, 8);
    if (kdl) {
        uint8_t plain[160];
        if (ki & KI_ENCRYPTED) {
            if (kdl > sizeof plain + 8 || aes_key_unwrap(F.ptk + 16, k->data, kdl, plain)) { F.hs_fail = 1; return; }
            take_gtk(plain, kdl - 8);
        } else take_gtk(k->data, kdl);
    }
    memset(F.snonce, 0, 32);
    eapol_send((uint16_t)(KI_PAIRWISE | KI_MIC | KI_SECURE | ver), 0, 0);
    F.hs = 4;
    F.keys_due = 1;                                         /* installed from wifi_poll, not in here */
    kprintf("[wifi] handshake: message 3 in, 4 out (group key %d, %d bytes)\n", F.gtk_id, F.gtk_len);
}

static int add_key(int index, int pairwise, const uint8_t *mac, const uint8_t *seq, int cipher, const uint8_t *key, int klen) {
    uint8_t b[1 + 26 + 32];
    memset(b, 0, sizeof b);
    b[0] = 3;                                               /* SPRDWL_SUBCMD_ADD */
    b[1] = (uint8_t)index;
    b[2] = (uint8_t)pairwise;
    if (mac) memcpy(b + 3, mac, 6);
    if (seq) memcpy(b + 9, seq, 8);
    b[25] = (uint8_t)cipher;
    b[26] = (uint8_t)klen;
    memcpy(b + 27, key, (size_t)klen);
    return command(CMD_KEY, b, 27 + klen, 3000);
}

/* ---- receiving -------------------------------------------------------------------------------- */
static void frame_in(const uint8_t *eth, int len) {
    if (len < 14) return;
    F.rx_frames++;
    if (eth[12] == 0x88 && eth[13] == 0x8E) { eapol_input(eth + 14, len - 14); return; }
    if (F.state == WIFI_CONNECTED) net_rx(&wlan, eth, len);
}

static void take_credit(const uint8_t *flow) {
    for (int i = 0; i < 4; i++) F.flow[i] += flow[i];
}

static void data_in(const uint8_t *d, int len) {
    if (len < 28) return;
    take_credit(d + 24);
    int n = d[8];                                           /* short packets in one buffer */
    if (n <= 1) {
        int off = d[1], fl = get16(d + 2);
        if (off + fl <= len) frame_in(d + off, fl);
        return;
    }
    const uint8_t *p = d, *end = d + len;
    while (n-- && p + 28 <= end) {
        int off = p[1], fl = get16(p + 2), tot = off + fl;
        if (p + tot > end) break;
        frame_in(p + off, fl);
        p += (tot + 28 + 7) & ~7;
    }
}

static void event_in(int id, const uint8_t *p, int len) {
    switch (id) {
    case EV_MGMT_FRAME:
        if (len >= 12 && p[0] == 4) {                       /* a scan result */
            int fl = get16(p + 10);
            if (12 + fl <= len) scan_result(p[1], (int8_t)p[2], p + 12, fl);
        } else if (len >= 12 && (p[0] == 2 || p[0] == 3) && F.state >= WIFI_CONNECTING) {
            kprintf("[wifi] the network let us go (%s)\n", p[0] == 2 ? "deauthenticated" : "disassociated");
        }
        break;
    case EV_SCAN_DONE:
        F.scanning = 0;
        for (int i = 1; i < F.nnets; i++)                   /* nearest first */
            for (int j = i; j > 0 && F.nets[j].rssi > F.nets[j - 1].rssi; j--) {
                wifi_net_t t = F.nets[j]; F.nets[j] = F.nets[j - 1]; F.nets[j - 1] = t;
            }
        kprintf("[wifi] scan done: %d networks\n", F.nnets);
        shell_request_frame();
        break;
    case EV_CONNECT:
        if (len >= 1 && p[0] == 0) {
            if (len >= 9) { F.bss.channel = p[7]; F.rssi = (int8_t)p[8]; }
            kprintf("[wifi] associated with %s (channel %d, %d dBm)\n", F.ssid, F.bss.channel, F.rssi);
            if (F.bss.security == WIFI_SEC_OPEN) F.want_up = 1;
            else { F.hs = 1; F.deadline = now_ms() + 8000; }
        } else {
            char w[120];
            snprintf(w, sizeof w, "Couldn't join %s (status %d)", F.ssid, len >= 3 ? p[2] : -1);
            say(WIFI_READY, w);
        }
        break;
    case EV_DISCONNECT: {
        int reason = len >= 2 ? get16(p) : 0;
        if (F.state >= WIFI_CONNECTING) {
            char w[120];
            if (F.state == WIFI_CONNECTING && (reason == 15 || reason == 2 || reason == 23))
                snprintf(w, sizeof w, "Wrong password for %s", F.ssid);
            else snprintf(w, sizeof w, "Disconnected from %s (reason %d)", F.ssid, reason);
            wlan.up = 0; wlan.ip = 0;
            F.lut_ok = 0;
            say(WIFI_READY, w);
        }
        break;
    }
    case EV_STA_LUT:
        if (len >= 9) {
            if (p[1] == 1 || p[1] == 2) { F.lut = p[2]; F.lut_ok = 1; }
            else if (p[1] == 0 && p[2] == F.lut) F.lut_ok = 0;
        }
        break;
    case EV_FLOWCON:
        if (len >= 4) take_credit(p);
        break;
    }
}

static void rx(int chn, const uint8_t *p, int len) {
    (void)chn;
    if (len < 4) return;
    int type = p[0] & 7;
    if (type == T_DATA_SPECIAL) { data_in(p, len); return; }
    if (len < HDR) return;
    int id = p[1], plen = get16(p + 2);
    if (plen > len || plen < HDR) plen = len;
    if (type == T_CMD) {
        if (id == F.wait_id) {
            F.status = (int8_t)p[8];
            F.rsp_len = plen - HDR > (int)sizeof F.rsp ? (int)sizeof F.rsp : plen - HDR;
            memcpy(F.rsp, p + HDR, (size_t)F.rsp_len);
            if (id == CMD_OPEN) F.ctx = (uint8_t)((p[0] >> 5) & 7);
            F.got = 1;
        }
    } else if (type == T_EVENT) event_in(id, p + HDR, plen - HDR);
}

/* ---- starting it ----------------------------------------------------------------------------- */
static int fw_ready;                     /* versions, calibration and details done */

int wifi_start(void) {
    if (F.state >= WIFI_READY || F.state == WIFI_STARTING) return 0;
    F.state = WIFI_STARTING;
    F.why[0] = 0;
    shell_request_frame();
    if (wcn_start()) { say(WIFI_FAILED, wcn_why()); return -1; }
    wcn_set_rx(CH_CMD_RX, rx);
    wcn_set_rx(CH_LOG_RX, rx);
    wcn_set_rx(CH_DATA_RX, rx);
    if (fw_ready) goto open_it;

    /* Our API version of every command and event: 1, as Unisoc's driver says. */
    static uint8_t api[4 + 256];
    memset(api, 0, sizeof api);
    api[0] = 1;
    for (int i = 0; i < 256; i++)
        if (i <= 41 || (i >= 56 && i <= 80) || i == 83 || i == 84 || (i >= 128 && i <= 134) || i == 136 || i == 137 ||
            i == 160 || i == 161 || (i >= 176 && i <= 179) || i == 224 || (i >= 242 && i <= 248)) api[4 + i] = 1;
    if (command(CMD_SYNC_VERSION, api, sizeof api, 3000) < 0) { say(WIFI_FAILED, "The Wi-Fi firmware didn't answer"); return -1; }

    const uint8_t *ini = _binary_wifi_ini_bin_start;
    int inilen = (int)(_binary_wifi_ini_bin_end - _binary_wifi_ini_bin_start);
    if (inilen >= 1796) {
        int rf = (int)(ini[1792] | ini[1793] << 8);
        int bad = download_ini(1, ini, 328);
        bad |= download_ini(2, ini + 328, 1464);
        if (rf > 0 && rf <= inilen - 1796) bad |= download_ini(3, ini + 1796, rf);
        if (bad) kprintf("[wifi] the calibration wasn't taken; going on\n");
    }

    /* Its details: TLVs saying we're a debug build of the host side and that our
     * frames come 4-byte aligned (the 3 bytes in front of the descriptor). */
    uint8_t tlv[10] = { 2, 0, 1, 0, 1,   1, 0, 1, 0, 1 };
    if (command(CMD_GET_INFO, tlv, sizeof tlv, 3000) || F.rsp_len < 77) { say(WIFI_FAILED, "The Wi-Fi firmware didn't describe itself"); return -1; }
    uint32_t model, ver, fwver;
    memcpy(&model, F.rsp, 4); memcpy(&ver, F.rsp + 4, 4); memcpy(&fwver, F.rsp + 8, 4);
    memcpy(F.mac, F.rsp + 76, 6);
    F.no_credit = F.rsp_len >= 83 && F.rsp[82] == 1;
    kprintf("[wifi] firmware %x (chip %x/%x), MAC %02x:%02x:%02x:%02x:%02x:%02x, %s\n", (unsigned)fwver, (unsigned)model,
            (unsigned)ver, F.mac[0], F.mac[1], F.mac[2], F.mac[3], F.mac[4], F.mac[5], F.no_credit ? "no credits" : "credits");

    fw_ready = 1;
open_it:;
    uint8_t open[8] = { MODE_STATION, 0 };
    memcpy(open + 2, F.mac, 6);
    F.ctx = 0;
    if (command(CMD_OPEN, open, sizeof open, 3000)) { say(WIFI_FAILED, "The Wi-Fi interface wouldn't open"); return -1; }

    if (!wlan.name) {
        wlan.name = "wlan0";
        memcpy(wlan.mac, F.mac, 6);
        wlan.tx = wlan_tx;
        wlan.up = 0;
        net_register(&wlan);
    }
    say(WIFI_READY, "Choose a network to join.");
    return 0;
}

/* ---- joining ------------------------------------------------------------------------------------ */
/* The network we joined last, kept across a restart: its name and key (the PMK,
 * not the password), so Mini can join it again by itself. */
struct kept { char ssid[33]; uint8_t pmk[32]; uint8_t open; };
static int left_on_purpose;              /* the user chose to leave: don't rejoin */

static int join(const wifi_net_t *n, const uint8_t *pmk) {
    if (F.state >= WIFI_CONNECTING) wifi_disconnect();
    F.bss = *n;
    snprintf(F.ssid, sizeof F.ssid, "%s", n->ssid);
    F.hs = 0; F.hs_fail = 0; F.keys_due = 0; F.want_up = 0; F.lut_ok = 0; F.gtk_len = 0;
    memset(F.replay, 0, 8);
    left_on_purpose = 0;
    char w[120];
    snprintf(w, sizeof w, "Joining %s\x85", n->ssid);
    say(WIFI_CONNECTING, w);

    uint8_t con[82];
    memset(con, 0, sizeof con);
    if (n->security == WIFI_SEC_WPA2) {
        memcpy(F.pmk, pmk, 32);
        /* Our RSN element: the AP's group cipher, CCMP for us, PSK. The same bytes
         * go in the association request and in handshake message 2. */
        static const uint8_t base[22] = { 48, 20, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 2, 0, 0 };
        memcpy(F.rsn_ie, base, sizeof base);
        int group = n->group == 2 ? CIPHER_TKIP : CIPHER_CCMP;  /* the suite's type: 2 TKIP, 4 CCMP */
        F.rsn_ie[7] = n->group == 2 ? 2 : 4;
        F.rsn_len = sizeof base;
        uint8_t ie[3 + 22];
        ie[0] = 3;                                          /* the association request's */
        put16(ie + 1, (uint16_t)F.rsn_len);
        memcpy(ie + 3, F.rsn_ie, (size_t)F.rsn_len);
        command(CMD_SET_IE, ie, 3 + F.rsn_len, 3000);
        uint32_t wpa2 = 2;
        memcpy(con, &wpa2, 4);
        con[12] = CIPHER_CCMP | VALID;
        con[13] = (uint8_t)(group | VALID);
        con[14] = AKM_PSK | VALID;
    }
    memcpy(con + 4, n->bssid, 6);
    con[10] = n->channel;
    con[11] = 0;                                            /* open system authentication */
    con[17] = (uint8_t)strlen(F.ssid);
    memcpy(con + 50, F.ssid, strlen(F.ssid));
    F.deadline = now_ms() + 15000;
    if (command(CMD_CONNECT, con, sizeof con, 3000)) { say(WIFI_READY, "The Wi-Fi firmware wouldn't start joining"); return -1; }
    return 0;
}

int wifi_connect(const char *ssid, const char *pass) {
    if (F.state < WIFI_READY) return -1;
    const wifi_net_t *n = 0;
    for (int i = 0; i < F.nnets; i++) if (!strcmp(F.nets[i].ssid, ssid)) n = &F.nets[i];
    if (!n) { say(WIFI_READY, "That network isn't in range"); return -1; }
    if (n->security == WIFI_SEC_OTHER) { say(WIFI_READY, "That network's security isn't one Mini can join yet"); return -1; }
    uint8_t pmk[32] = { 0 };
    if (n->security == WIFI_SEC_WPA2) {
        if (!pass || strlen(pass) < 8) { say(WIFI_READY, "A WPA2 password has at least 8 characters"); return -1; }
        uint64_t t = now_ms();
        wpa_pmk(pass, ssid, pmk);
        kprintf("[wifi] key from the password in %lu ms\n", (unsigned long)(now_ms() - t));
    }
    return join(n, pmk);
}

/* Leave on purpose: and don't come back to it by itself. */
void wifi_leave(void) {
    wifi_disconnect();
    left_on_purpose = 1;
    keep_put("wifi", 0, -1);
}

/* After a scan, or a while after the network dropped us: the kept one, if it's here. */
static void rejoin(void) {
    struct kept k;
    if (left_on_purpose || F.state != WIFI_READY || keep_get("wifi", &k, sizeof k) != (int)sizeof k) return;
    k.ssid[32] = 0;
    for (int i = 0; i < F.nnets; i++)
        if (!strcmp(F.nets[i].ssid, k.ssid) && (F.nets[i].security == WIFI_SEC_OPEN) == (k.open != 0)) {
            kprintf("[wifi] joining %s again\n", k.ssid);
            join(&F.nets[i], k.pmk);
            return;
        }
}

void wifi_disconnect(void) {
    if (F.state < WIFI_CONNECTING) return;
    uint8_t r[2] = { 3, 0 };                                /* deauthenticated: leaving */
    wlan.up = 0; wlan.ip = 0;
    F.lut_ok = 0;
    F.state = WIFI_READY;
    command(CMD_DISCONNECT, r, 2, 3000);
    say(WIFI_READY, "Disconnected");
}

/* Off: leave the network and close the interface. The chip stays up, for Bluetooth. */
void wifi_stop(void) {
    if (F.state < WIFI_READY) return;
    if (F.state >= WIFI_CONNECTING) wifi_disconnect();
    uint8_t m = MODE_STATION;
    command(CMD_CLOSE, &m, 1, 3000);
    F.nnets = 0; F.scanning = 0;
    say(WIFI_OFF, "Off");
}

/* From the main loop: what mustn't happen inside a receive. */
static int ip_due;
static void tell_ip(void);
void wifi_poll(void) {
    static int autostarted;
    if (!autostarted && F.state == WIFI_OFF && now_ms() > 2000 && g_plat == PLAT_H618) {
        autostarted = 1;                                    /* on by default, like a phone's */
        if (wifi_start() == 0) wifi_scan();
        return;
    }
    if (F.scanning && now_ms() > F.scan_end) F.scanning = 0;
    if (ip_due && F.state == WIFI_CONNECTED) tell_ip();
    static int was_scanning;
    static uint64_t retry_at;
    if (F.state == WIFI_READY && !left_on_purpose) {
        if (was_scanning && !F.scanning) rejoin();          /* a scan just finished */
        else if (!F.scanning && now_ms() > retry_at && keep_get("wifi", 0, 0) >= 0) {
            retry_at = now_ms() + 30000;                    /* dropped: look again now and then */
            wifi_scan();
        }
    }
    was_scanning = F.scanning;
    if (F.state != WIFI_CONNECTING) return;
    if (F.hs_fail) { F.hs_fail = 0; wifi_disconnect(); keep_put("wifi", 0, -1); say(WIFI_READY, "Wrong password"); return; }
    if (F.keys_due) {
        F.keys_due = 0;
        int a = add_key(0, 1, F.bss.bssid, 0, CIPHER_CCMP, F.ptk + 32, 16);
        int b = F.gtk_len ? add_key(F.gtk_id, 0, 0, F.gtk_rsc, F.gtk_len == 32 ? CIPHER_TKIP : CIPHER_CCMP, F.gtk, F.gtk_len) : 0;
        if (a || b) { wifi_disconnect(); say(WIFI_READY, "The Wi-Fi firmware refused the keys"); return; }
        F.want_up = 1;
    }
    if (F.want_up) {
        F.want_up = 0;
        /* Power saving off: with it on, the AP holds our frames until the next
         * beacon we wake for, and a ping takes anything up to most of a second. */
        uint8_t ps[2] = { 4, 0 };                           /* SPRDWL_SET_PS_STATE, off */
        command(CMD_POWER_SAVE, ps, 2, 3000);
        wlan.up = 1;
        struct kept k;
        memset(&k, 0, sizeof k);
        snprintf(k.ssid, sizeof k.ssid, "%s", F.ssid);
        memcpy(k.pmk, F.pmk, 32);
        k.open = F.bss.security == WIFI_SEC_OPEN;
        keep_put("wifi", &k, sizeof k);
        char w[120];
        snprintf(w, sizeof w, "Connected to %s", F.ssid);
        say(WIFI_CONNECTED, w);
        return;
    }
    if (now_ms() > F.deadline) {
        int was_hs = F.hs;
        wifi_disconnect();
        say(WIFI_READY, was_hs ? "The network didn't finish the handshake: check the password" : "The network didn't answer");
    }
}

/* DHCP gave us an address (inet.c, from inside a receive): the firmware wants to
 * know, and wifi_poll tells it. */
void wifi_got_ip(netif_t *nif) { if (nif == &wlan && nif->ip) ip_due = 1; }

static void tell_ip(void) {
    ip_due = 0;
    uint32_t a = wlan.ip;
    uint8_t ip[4] = { (uint8_t)(a >> 24), (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a };
    command(CMD_NOTIFY_IP, ip, 4, 3000);
}

int wifi_debug(char *out, int cap) {
    return snprintf(out, (size_t)cap, "wifi: state %d (%s), ctx %d, lut %d%s, credits %d %d %d %d%s, rx %u tx %u (no credit %u), eapol %u, hs %d\n",
                    F.state, F.why, F.ctx, F.lut, F.lut_ok ? "" : " (none)", F.flow[0], F.flow[1], F.flow[2], F.flow[3],
                    F.no_credit ? " (not needed)" : "", (unsigned)F.rx_frames, (unsigned)F.tx_frames, (unsigned)F.tx_nocredit,
                    (unsigned)F.eapol, F.hs);
}
