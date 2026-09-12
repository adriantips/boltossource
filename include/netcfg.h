#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Network configuration: what the machine should do on the network, kept
 *  across reboots.
 *
 *  Until now every boot ran DHCP and forgot everything afterwards, so a static
 *  address, a chosen DNS server or a Wi-Fi passphrase had to be re-entered by
 *  hand every single time. The settings live in /etc/network.conf as plain
 *  `key value` lines -- readable, editable with the text editor, and carried to
 *  a new machine by the installer along with the rest of the tree.
 *
 *  Wi-Fi credentials live in /etc/wifi.conf with mode 0600, so they are
 *  readable only by root (the network stack runs in the kernel, which is
 *  already root) rather than by every account on the machine.
 * ===========================================================================*/
#define NETCFG_PATH   "/etc/network.conf"
#define WIFICFG_PATH  "/etc/wifi.conf"
#define NETCFG_HOSTS  "/etc/hosts"

#define NETCFG_SSID_MAX 33
#define NETCFG_PSK_MAX  64
#define NETCFG_HOST_MAX 32

typedef struct {
    int      use_dhcp;         /* 1 = DHCP (default), 0 = the static values below */
    uint32_t ip, mask, gw, dns;/* host order; used when use_dhcp is 0             */
    char     hostname[NETCFG_HOST_MAX];
    int      ntp_enabled;
    uint32_t ntp_server;       /* host order; 0 = use the default pool address    */
    int      sim_radio;        /* bring up the built-in 802.11 radio at boot      */
} netcfg_t;

extern netcfg_t g_netcfg;

/* Load /etc/network.conf and apply it. Called during net bring-up, before DHCP
 * so a static configuration wins without a pointless DHCP round trip. Returns 1
 * if a saved configuration was found. */
int  netcfg_load(void);
int  netcfg_save(void);
void netcfg_apply(void);         /* push g_netcfg into the live net_* globals  */

/* Remember/forget a Wi-Fi network. Credentials are stored 0600. */
int  netcfg_wifi_save(const char *ssid, const char *psk);
int  netcfg_wifi_find(const char *ssid, char *psk_out, int cap);   /* 1 = found */
int  netcfg_wifi_count(void);
const char *netcfg_wifi_ssid(int i);
int  netcfg_wifi_forget(const char *ssid);

/* /etc/hosts. Checked before DNS, so a machine can name its own peers and a
 * lookup for something local never leaves the wire. 0 if not found. */
uint32_t netcfg_hosts_lookup(const char *name);
int      netcfg_hosts_add(const char *name, uint32_t ip);

/* Parse/format dotted-quad. netcfg_parse_ip returns 0 on success. */
int  netcfg_parse_ip(const char *s, uint32_t *out);
void netcfg_format_ip(uint32_t ip, char *out);   /* out >= 16 */
