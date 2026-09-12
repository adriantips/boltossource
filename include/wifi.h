#pragma once
#include <stdint.h>
#include "netif.h"

/* ===========================================================================
 *  IEEE 802.11: the MAC frame formats, the radio seam a driver binds to, and
 *  the station that joins a network through it.
 *
 *  Three layers, and the split matters:
 *
 *    net/wifi.c      frame format only -- no hardware, no state. Beacons in,
 *                    scan rows out; Ethernet in, 802.11 out.
 *    net/wifi_sta.c  the station: scan, authenticate, associate, run the WPA2
 *                    handshake in net/wpa.c, then carry Ethernet frames.
 *    a radio driver  registers a struct wifi_radio and does nothing else. It
 *                    transmits the bytes it is given and calls wifi_rx() with
 *                    the bytes it receives.
 *
 *  Everything above the radio is chip-independent, so a new card is a driver
 *  and not a Wi-Fi stack. drivers/wifi_sim.c is one such driver.
 * ===========================================================================*/
#define IEEE80211_ADDR_LEN 6

/* Frame Control (little-endian on the wire): bits 0-1 version, 2-3 type,
 * 4-7 subtype, then per-bit flags in the high byte. */
#define FC_TYPE_MASK    0x000C
#define FC_SUBTYPE_MASK 0x00F0
#define FC_TYPE_MGMT    0x0000
#define FC_TYPE_CTRL    0x0004
#define FC_TYPE_DATA    0x0008
#define FC_TODS         0x0100
#define FC_FROMDS       0x0200
#define FC_RETRY        0x0800
#define FC_PROTECTED    0x4000

/* Management subtypes, pre-shifted into the subtype field (bits 4-7). */
#define MGMT_ASSOC_REQ  0x0000
#define MGMT_ASSOC_RESP 0x0010
#define MGMT_PROBE_REQ  0x0040
#define MGMT_PROBE_RESP 0x0050
#define MGMT_BEACON     0x0080
#define MGMT_DISASSOC   0x00A0
#define MGMT_AUTH       0x00B0
#define MGMT_DEAUTH     0x00C0

/* Generic 3-address MAC header (the common data/management layout). */
struct ieee80211_hdr {
    uint16_t frame_control;
    uint16_t duration_id;
    uint8_t  addr1[IEEE80211_ADDR_LEN];   /* RA / DA   */
    uint8_t  addr2[IEEE80211_ADDR_LEN];   /* TA / SA   */
    uint8_t  addr3[IEEE80211_ADDR_LEN];   /* BSSID     */
    uint16_t seq_ctrl;
} __attribute__((packed));

/* Fixed body of a beacon / probe-response, followed by tagged info elements. */
struct ieee80211_beacon {
    uint64_t timestamp;
    uint16_t beacon_interval;
    uint16_t capability;
} __attribute__((packed));
#define CAP_PRIVACY 0x0010                /* capability bit: encrypted BSS */

/* Tagged information element (TLV). */
struct ieee80211_ie { uint8_t id, len; uint8_t data[]; } __attribute__((packed));
#define IE_SSID    0
#define IE_RATES   1
#define IE_DSPARAM 3                       /* one byte: channel */
#define IE_RSN     48                      /* WPA2 */

/* LLC/SNAP header prefixing an 802.11 data payload; its ethertype maps directly
 * onto the Ethernet II type field. */
struct llc_snap {
    uint8_t  dsap, ssap, control;
    uint8_t  oui[3];
    uint16_t ethertype;
} __attribute__((packed));
#define LLC_SNAP_AA 0xAA                   /* dsap==ssap==0xAA, control==0x03 */

/* What a beacon's RSN element says the network wants. AKM_NONE with privacy
 * set means WEP, which nothing here will join -- it is not security and
 * pretending otherwise on a status line would be worse than refusing. */
enum wifi_akm { WIFI_AKM_NONE = 0, WIFI_AKM_PSK, WIFI_AKM_PSK_SHA256, WIFI_AKM_SAE };

/* One discovered BSS (a scan-result row). */
struct wifi_bss {
    uint8_t  bssid[IEEE80211_ADDR_LEN];
    char     ssid[33];
    uint8_t  channel;
    int8_t   rssi;
    uint16_t capability;
    int      privacy;
    int      rsn;                 /* an RSN element was present  */
    int      akm;                 /* enum wifi_akm               */
    int      ccmp;                /* CCMP offered as a pairwise cipher */
};

/* Association state. WIFI_4WAY is where a wrong passphrase ends up: the
 * association succeeds and then the handshake times out, which is exactly what
 * a real AP does and why "wrong password" takes a few seconds to find out. */
enum wifi_state { WIFI_IDLE, WIFI_SCANNING, WIFI_AUTH, WIFI_ASSOC, WIFI_4WAY, WIFI_RUN };

/* ---- MAC-layer helpers (net/wifi.c) ---- */

/* Convert a received 802.11 data frame (hdr + LLC/SNAP + payload) into an
 * Ethernet II frame in out. Returns the eth frame length, or -1 if the frame is
 * not a forwardable 3-address data frame. */
int ieee80211_data_to_eth(const uint8_t *frame, uint16_t len,
                          uint8_t *out, uint16_t out_cap);

/* Parse a beacon / probe-response into *bss. Returns 0 on success, -1 if the
 * frame is not a beacon/probe-response or is too short. */
int ieee80211_parse_beacon(const uint8_t *frame, uint16_t len, struct wifi_bss *bss);

/* Build a probe-request (broadcast DA/BSSID) into out; ssid NULL/"" = wildcard.
 * Returns the frame length. out must hold >= 38 + len(ssid) bytes. */
int ieee80211_build_probe_req(uint8_t *out, const uint8_t src[6], const char *ssid);

const char *wifi_state_name(enum wifi_state s);

/* Parse the RSN element out of a beacon body into bss->rsn/akm/ccmp. Returns 1
 * if an RSN element was found. */
int ieee80211_parse_rsn(const uint8_t *ie, uint8_t len, struct wifi_bss *bss);

/* Management-frame builders. Each returns the frame length. `out` needs 128
 * bytes for auth and 128 + len(ssid) for an association request. */
int ieee80211_build_auth(uint8_t *out, const uint8_t src[6], const uint8_t bssid[6],
                         uint16_t seq);
int ieee80211_build_assoc_req(uint8_t *out, const uint8_t src[6], const uint8_t bssid[6],
                              const char *ssid, int rsn);
int ieee80211_build_deauth(uint8_t *out, const uint8_t src[6], const uint8_t bssid[6],
                           uint16_t reason);
/* Beacon/probe-response and the two responses an AP sends, for the AP side of
 * a radio that implements one (drivers/wifi_sim.c). */
int ieee80211_build_beacon(uint8_t *out, const uint8_t bssid[6], const char *ssid,
                           uint8_t channel, int rsn, int probe_resp,
                           const uint8_t *da);
int ieee80211_build_mgmt_resp(uint8_t *out, uint16_t subtype, const uint8_t bssid[6],
                              const uint8_t da[6], uint16_t status, uint16_t aid);

/* Wrap an Ethernet II frame as an 802.11 data frame. `tods` picks the
 * direction: a station sets it, an AP relaying to a station clears it and sets
 * FromDS instead. Returns the length, or -1 if out_cap is too small. */
int ieee80211_eth_to_data(const uint8_t *eth, uint16_t ethlen,
                          const uint8_t bssid[6], uint16_t seq, int tods,
                          uint8_t *out, uint16_t out_cap);

/* The ethertype an 802.11 data frame carries, without converting it -- so the
 * EAPOL frames of the handshake can be picked out before the key exists that
 * would let the rest be decrypted. Returns 0 if there is no LLC/SNAP header. */
uint16_t ieee80211_snap_ethertype(const uint8_t *frame, uint16_t len, uint32_t hdrlen);

/* =======================================================================
 *  The radio seam.
 *
 *  A driver fills one of these in and calls wifi_radio_register(). Everything
 *  it has to do is here: tune, transmit, and hand received frames up. There is
 *  no scan op, no association op and no key op, because those are 802.11 and
 *  not silicon -- net/wifi_sta.c does them out of management frames, so a new
 *  chip costs a driver rather than a stack.
 * =======================================================================*/
#define WIFI_FRAME_MAX 2048

struct wifi_radio;
struct wifi_ops {
    int  (*start)(struct wifi_radio *r);
    int  (*set_channel)(struct wifi_radio *r, int channel);
    int  (*tx)(struct wifi_radio *r, const void *frame, uint16_t len);
    void (*poll)(struct wifi_radio *r);      /* optional: drain the RX ring */
    void (*stop)(struct wifi_radio *r);      /* optional                    */
};

struct wifi_radio {
    char                   name[8];
    uint8_t                mac[IEEE80211_ADDR_LEN];
    const char            *chip;             /* shown by `wifi`             */
    int                    max_channel;
    const struct wifi_ops *ops;
    void                  *drv;
    uint64_t               rx_frames, tx_frames, rx_dropped;
};

void wifi_radio_register(struct wifi_radio *r);
struct wifi_radio *wifi_radio_get(void);     /* 0 when no radio is bound */

/* Driver -> stack. Called from the driver's poll or its interrupt handler with
 * one complete 802.11 frame, FCS already stripped. */
void wifi_rx(struct wifi_radio *r, const uint8_t *frame, uint16_t len, int8_t rssi);

/* =======================================================================
 *  The station (net/wifi_sta.c).
 * =======================================================================*/
#define WIFI_MAX_BSS 16

/* Scan for `ms` milliseconds across every channel the radio will tune to, and
 * return how many distinct networks answered. */
int  wifi_scan(int ms);
int  wifi_scan_count(void);
const struct wifi_bss *wifi_scan_get(int i);

/* Join `ssid`. `psk` is the passphrase, or 0/"" for an open network. Blocks
 * until associated and keyed, or until `timeout_ms` runs out. Returns 0, or
 * one of the negative codes below -- the caller wants to tell the difference
 * between a wrong password and a network that is not there. */
#define WIFI_ENORADIO   -1
#define WIFI_ENOTFOUND  -2
#define WIFI_EAUTH      -3     /* the AP refused the association            */
#define WIFI_EKEY       -4     /* the handshake failed: wrong passphrase     */
#define WIFI_EUNSUPP    -5     /* WEP, or an AKM this does not implement     */
#define WIFI_ETIMEOUT   -6
int  wifi_connect(const char *ssid, const char *psk, int timeout_ms);
void wifi_disconnect(void);
const char *wifi_err_str(int rc);

enum wifi_state        wifi_get_state(void);
const struct wifi_bss *wifi_current(void);      /* 0 unless associated */
struct netif          *wifi_netif(void);
int                    wifi_signal(void);       /* RSSI in dBm, 0 if unknown */

/* Called from the netif poll: drains the radio and ages the association. */
void wifi_sta_poll(void);

/* Bring up whatever the credential store remembers, if a radio is bound.
 * Called once during boot, after the link drivers are probed. */
void wifi_autoconnect(void);

/* =======================================================================
 *  The built-in radio (drivers/wifi_sim.c).
 *
 *  There is no 802.11 device in QEMU and no Wi-Fi card in the development
 *  machine, so this is the radio the stack above is actually run against: an
 *  access point in the kernel, bridged to the wired interface, speaking real
 *  802.11 frames and real WPA2. Off unless asked for -- `wifi radio on`, or
 *  `simradio 1` in /etc/network.conf.
 * =======================================================================*/
int         wifi_sim_start(const char *ssid, const char *psk);
void        wifi_sim_stop(void);
int         wifi_sim_running(void);
const char *wifi_sim_ssid(void);
