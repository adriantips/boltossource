/* BoltOS Mini - mini/wlan.h: Wi-Fi (mini/wifi.c). Not include/wifi.h, the desktop's
 * 802.11 frame layer, which net/wpa.c still uses. */
#pragma once
#include <stdint.h>
#include "minet.h"

enum { WIFI_OFF, WIFI_STARTING, WIFI_FAILED, WIFI_READY, WIFI_CONNECTING, WIFI_CONNECTED };
enum { WIFI_SEC_OPEN, WIFI_SEC_WPA2, WIFI_SEC_OTHER };
#define WIFI_MAXNETS 32

typedef struct {
    char    ssid[33];
    uint8_t bssid[6];
    int8_t  rssi;                        /* dBm */
    uint8_t channel;
    uint8_t security;                    /* WIFI_SEC_* */
    uint8_t group;                       /* the RSN group cipher suite's type */
} wifi_net_t;

int  wifi_start(void);                   /* the chip, its firmware, a station; blocks ~1 s */
void wifi_stop(void);
int  wifi_state(void);
const char *wifi_why(void);              /* what happened last, in words              */
int  wifi_scan(void);                    /* results arrive in wifi_nets()             */
int  wifi_scanning(void);
int  wifi_nets(const wifi_net_t **out);
int  wifi_connect(const char *ssid, const char *pass);
void wifi_disconnect(void);
void wifi_leave(void);                   /* disconnect, and forget it                 */
void wifi_poll(void);                    /* from the main loop                        */
const char *wifi_ssid(void);
int  wifi_rssi(void);
netif_t *wifi_netif(void);
void wifi_got_ip(netif_t *nif);
int  wifi_debug(char *out, int cap);
