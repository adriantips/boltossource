#include <stdint.h>
#include "commands.h"
#include "net.h"
#include "netif.h"
#include "netcfg.h"
#include "net6.h"
#include "ntp.h"
#include "dhcp.h"
#include "hw.h"
#include "fs.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_netcfg.c
 *  Commands for the parts of networking a person has to configure and keep:
 *  the interface (`ifconfig`), the local name table (`hosts`), the resolver
 *  cache (`dns`) and the clock (`ntp`).
 *
 *  All of them write through to /etc, so a change made once survives the
 *  reboot. That is the whole point: before this, every boot re-ran DHCP and
 *  forgot everything, and a machine with a fixed address had to be reconfigured
 *  by hand every time it started.
 * ===========================================================================*/

static void print_ip(uint32_t ip) {
    kprintf("%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
            (ip >> 8) & 0xFF, ip & 0xFF);
}

/* Turn a mask into its prefix length, so `ifconfig` can show /24 rather than
 * making the reader count bits in 255.255.255.0. */
static int mask_prefix(uint32_t m) {
    int n = 0;
    while (m & 0x80000000u) { n++; m <<= 1; }
    return n;
}

/* ------------------------------------------------------------- ifconfig ----*/
static void ifconfig_usage(void) {
    kprintf("usage: ifconfig                          show the interface and saved config\n"
            "       ifconfig dhcp                     use DHCP, and save that\n"
            "       ifconfig IP MASK GW [DNS]         static address, saved\n"
            "       ifconfig dns SERVER               resolver only (works under DHCP too)\n"
            "       ifconfig hostname NAME            set this machine's name\n"
            "       ifconfig renew                    re-run DHCP now\n");
    /* Nothing here sets an IPv6 address by hand -- it's autoconfig or nothing.
     * g_netcfg has no v6 fields to save one into even if this command grew
     * the option, so it would need its own bit of persisted state first. */
}

int cmd_ifconfig(int argc, char **argv) {
    struct netif *nif = netif_default();

    if (argc < 2) {
        if (!nif) kprintf("no interface is up\n");
        else {
            kprintf("%s  link %s  mtu %u\n", nif->name,
                    nif->link_up ? "up" : "down", 1500u);
            kprintf("  inet    "); print_ip(net_ip);
            kprintf("/%d  gw ", mask_prefix(net_mask)); print_ip(net_gw);
            kprintf("  dns "); print_ip(net_dns); kprintf("\n");
            char txt[46];
            if (!ip6_is_zero(&net6_link)) {
                ip6_format(&net6_link, txt);
                kprintf("  inet6   %s/64  (link-local)\n", txt);
            }
            if (net6_have_addr) {
                ip6_format(&net6_addr, txt);
                kprintf("  inet6   %s/%d", txt, net6_prefix_len);
                if (net6_have_gw) {
                    char g[46]; ip6_format(&net6_gw, g);
                    kprintf("  via %s", g);
                }
                kprintf("\n");
            } else if (!ip6_is_zero(&net6_link)) {
                kprintf("  inet6   no router advertisement; link-local only\n");
            }
            /* The neighbour cache, which is what `arp` would show on the other
             * half of the stack. */
            ip6_addr na; uint8_t nm[6];
            for (int i = 0; nd6_cache_dump(i, &na, nm); i++) {
                ip6_format(&na, txt);
                kprintf("  neigh   %s  %x:%x:%x:%x:%x:%x\n", txt,
                        nm[0], nm[1], nm[2], nm[3], nm[4], nm[5]);
            }
            kprintf("  rx      %lu pkts / %lu bytes\n", nif->rx_packets, nif->rx_bytes);
            kprintf("  tx      %lu pkts / %lu bytes\n", nif->tx_packets, nif->tx_bytes);
        }
        kprintf("saved (%s): %s", NETCFG_PATH,
                g_netcfg.use_dhcp ? "dhcp\n" : "static ");
        if (!g_netcfg.use_dhcp) {
            print_ip(g_netcfg.ip); kprintf("/%d gw ", mask_prefix(g_netcfg.mask));
            print_ip(g_netcfg.gw); kprintf("\n");
        }
        if (g_netcfg.hostname[0]) kprintf("hostname: %s\n", g_netcfg.hostname);
        return 0;
    }

    if (!strcmp(argv[1], "dhcp")) {
        g_netcfg.use_dhcp = 1;
        if (netcfg_save() != 0) { kprintf("ifconfig: could not write %s\n", NETCFG_PATH); return 1; }
        kprintf("using DHCP; leasing now\n");
        dhcp_configure(3000);
        return 0;
    }

    if (!strcmp(argv[1], "renew")) {
        if (!nif) { kprintf("ifconfig: no interface\n"); return 1; }
        return dhcp_configure(3000) == 0 ? 0 : 1;
    }

    if (!strcmp(argv[1], "hostname")) {
        if (argc < 3) { kprintf("usage: ifconfig hostname NAME\n"); return 1; }
        strncpy(g_netcfg.hostname, argv[2], NETCFG_HOST_MAX - 1);
        g_netcfg.hostname[NETCFG_HOST_MAX - 1] = 0;
        if (netcfg_save() != 0) { kprintf("ifconfig: could not write %s\n", NETCFG_PATH); return 1; }
        kprintf("hostname: %s\n", g_netcfg.hostname);
        return 0;
    }

    if (!strcmp(argv[1], "dns")) {
        uint32_t d;
        if (argc < 3 || netcfg_parse_ip(argv[2], &d) != 0) {
            kprintf("usage: ifconfig dns A.B.C.D\n"); return 1;
        }
        g_netcfg.dns = d;
        net_dns = d;
        dns_cache_flush();                    /* answers from the old resolver */
        if (netcfg_save() != 0) { kprintf("ifconfig: could not write %s\n", NETCFG_PATH); return 1; }
        kprintf("resolver: "); print_ip(d); kprintf(" (saved)\n");
        return 0;
    }

    /* ifconfig IP MASK GW [DNS] */
    if (argc < 4) { ifconfig_usage(); return 1; }
    uint32_t ip, mask, gw, dns = 0;
    if (netcfg_parse_ip(argv[1], &ip)   != 0 ||
        netcfg_parse_ip(argv[2], &mask) != 0 ||
        netcfg_parse_ip(argv[3], &gw)   != 0) {
        kprintf("ifconfig: bad address\n"); return 1;
    }
    if (argc > 4 && netcfg_parse_ip(argv[4], &dns) != 0) {
        kprintf("ifconfig: bad DNS address\n"); return 1;
    }

    g_netcfg.use_dhcp = 0;
    g_netcfg.ip = ip; g_netcfg.mask = mask; g_netcfg.gw = gw;
    if (dns) g_netcfg.dns = dns;
    netcfg_apply();
    dns_cache_flush();
    if (netcfg_save() != 0) { kprintf("ifconfig: could not write %s\n", NETCFG_PATH); return 1; }

    kprintf("static: "); print_ip(net_ip);
    kprintf("/%d gw ", mask_prefix(net_mask)); print_ip(net_gw);
    kprintf(" dns "); print_ip(net_dns);
    kprintf("  (saved to %s)\n", NETCFG_PATH);
    return 0;
}

/* ---------------------------------------------------------------- hosts ----*/
int cmd_hosts(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "add")) {
        uint32_t ip;
        if (netcfg_parse_ip(argv[3], &ip) != 0) { kprintf("hosts: bad address\n"); return 1; }
        if (netcfg_hosts_add(argv[2], ip) != 0) { kprintf("hosts: could not write %s\n", NETCFG_HOSTS); return 1; }
        dns_cache_flush();          /* a cached wire answer would still win */
        kprintf("%s -> ", argv[2]); print_ip(ip); kprintf("\n");
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "add")) {
        kprintf("usage: hosts add NAME A.B.C.D\n");
        return 1;
    }

    fs_node *f = fs_lookup(NETCFG_HOSTS);
    if (!f || !f->data || !f->size) {
        kprintf("%s is empty. Add a name with: hosts add NAME A.B.C.D\n", NETCFG_HOSTS);
        return 0;
    }
    kprintf("%s:\n", NETCFG_HOSTS);
    for (uint32_t i = 0; i < f->size; i++) kprintf("%c", ((const char *)f->data)[i]);
    if (f->size && ((const char *)f->data)[f->size - 1] != '\n') kprintf("\n");
    return 0;
}

/* ------------------------------------------------------------------ dns ----*/
int cmd_dns(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "flush")) {
        dns_cache_flush();
        kprintf("resolver cache cleared\n");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "-6")) {         /* dns -6 NAME -> AAAA */
        ip6_addr a;
        if (!dns_resolve6(argv[2], &a, 3000)) { kprintf("dns: %s has no AAAA record\n", argv[2]); return 1; }
        char txt[46]; ip6_format(&a, txt);
        kprintf("%s -> %s\n", argv[2], txt);
        return 0;
    }
    if (argc >= 2) {                                   /* dns NAME -> resolve */
        /* Whichever family this machine can reach, because that is what every
         * other caller of the resolver now gets -- `dns` reporting an A record
         * while the browser had connected over v6 would be a lie. */
        netaddr a;
        if (!dns_resolve_addr(argv[1], &a, 3000)) { kprintf("dns: %s not found\n", argv[1]); return 1; }
        char txt[NETADDR_STRLEN]; netaddr_format(&a, txt);
        kprintf("%s -> %s\n", argv[1], txt);
        return 0;
    }

    kprintf("resolver: "); print_ip(net_dns); kprintf("\n");
    const char *name; netaddr addr; uint32_t ttl; int n = 0;
    for (int i = 0; i < 64; i++) {
        if (!dns_cache_dump(i, &name, &addr, &ttl)) continue;
        if (!n++) { sh_pad("NAME", 34); sh_pad("ADDRESS", 40); kprintf("TTL\n"); }
        sh_pad(name, 34);
        char b[NETADDR_STRLEN]; netaddr_format(&addr, b); sh_pad(b, 40);
        kprintf("%us\n", ttl);
    }
    if (!n) kprintf("cache is empty\n");
    return 0;
}

/* ------------------------------------------------------------------ ntp ----*/
int cmd_ntp(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "off")) {
        g_netcfg.ntp_enabled = 0; netcfg_save();
        kprintf("ntp: disabled at boot\n"); return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "on")) {
        g_netcfg.ntp_enabled = 1; netcfg_save();
        kprintf("ntp: enabled at boot\n"); return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "server")) {
        uint32_t s;
        if (netcfg_parse_ip(argv[2], &s) != 0) { kprintf("ntp: bad address\n"); return 1; }
        g_netcfg.ntp_server = s; netcfg_save();
        kprintf("ntp server: "); print_ip(s); kprintf(" (saved)\n");
        return 0;
    }

    if (argc >= 2 && !strcmp(argv[1], "sync")) {
        uint32_t srv = g_netcfg.ntp_server;
        if (argc >= 3 && netcfg_parse_ip(argv[2], &srv) != 0) {
            kprintf("ntp: bad address\n"); return 1;
        }
        int rc = ntp_sync(srv, 3000);
        if (rc != 0) {
            kprintf("ntp: %s\n", rc == -1 ? "no interface, or the request could not be sent"
                                : rc == -2 ? "no reply (is UDP/123 reachable?)"
                                           : "the server sent a nonsensical timestamp");
            return 1;
        }
        struct rtc_time t; ntp_now(&t);
        kprintf("clock set: %u-%02u-%02u %02u:%02u:%02u UTC (offset %ld s)\n",
                t.year, t.mon, t.day, t.hour, t.min, t.sec, (long)ntp_offset());
        return 0;
    }

    /* status */
    struct rtc_time raw, now;
    rtc_now(&raw); ntp_now(&now);
    kprintf("ntp        : %s at boot\n", g_netcfg.ntp_enabled ? "enabled" : "disabled");
    kprintf("server     : ");
    print_ip(g_netcfg.ntp_server ? g_netcfg.ntp_server : (uint32_t)NTP_DEFAULT_SERVER);
    kprintf("%s\n", g_netcfg.ntp_server ? "" : " (default)");
    kprintf("rtc        : %u-%02u-%02u %02u:%02u:%02u\n",
            raw.year, raw.mon, raw.day, raw.hour, raw.min, raw.sec);
    if (ntp_synced()) {
        kprintf("system     : %u-%02u-%02u %02u:%02u:%02u UTC\n",
                now.year, now.mon, now.day, now.hour, now.min, now.sec);
        kprintf("correction : %ld s\n", (long)ntp_offset());
    } else {
        kprintf("system     : not synced this boot (run `ntp sync`)\n");
    }
    return 0;
}
