/* Persistent network configuration -- see include/netcfg.h for the model.
 *
 * Plain text on the filesystem, deliberately. A machine whose network is
 * broken is exactly the machine where you want to fix the settings with cat
 * and a text editor, not a binary blob and a tool that needs the network to
 * go fetch. */
#include <stdint.h>
#include "netcfg.h"
#include "net.h"
#include "fs.h"
#include "string.h"
#include "kprintf.h"

netcfg_t g_netcfg = { .use_dhcp = 1, .ntp_enabled = 1 };

/* ---------------------------------------------------------------- helpers --*/
int netcfg_parse_ip(const char *s, uint32_t *out) {
    uint32_t v = 0;
    int part = 0, digits = 0, acc = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            acc = acc * 10 + (*s - '0');
            if (acc > 255) return -1;
            digits++;
        } else if (*s == '.' || *s == 0) {
            if (!digits || part > 3) return -1;
            v = (v << 8) | (uint32_t)acc;
            part++; acc = 0; digits = 0;
            if (*s == 0) break;
        } else {
            return -1;
        }
    }
    if (part != 4) return -1;
    *out = v;
    return 0;
}

static char *put_u(char *o, uint32_t v) {
    char t[12]; int i = 0;
    if (!v) { *o++ = '0'; return o; }
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) *o++ = t[--i];
    return o;
}

void netcfg_format_ip(uint32_t ip, char *out) {
    char *o = out;
    for (int i = 3; i >= 0; i--) {
        o = put_u(o, (ip >> (i * 8)) & 0xFF);
        if (i) *o++ = '.';
    }
    *o = 0;
}

/* Read a whole file into a NUL-terminated static buffer. Configuration files
 * are small by construction, so this stays off the heap. */
static char *slurp(const char *path, char *buf, uint32_t cap) {
    fs_node *f = fs_lookup(path);
    if (!f || f->is_dir || !f->data) return 0;
    uint32_t n = f->size < cap - 1 ? f->size : cap - 1;
    memcpy(buf, f->data, n);
    buf[n] = 0;
    return buf;
}

/* Split "key value" out of one line, in place. Blank lines and #comments give
 * key = NULL. */
static void kv(char *line, char **key, char **val) {
    *key = *val = 0;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#') return;
    *key = line;
    while (*line && *line != ' ' && *line != '\t') line++;
    if (*line) { *line++ = 0; }
    while (*line == ' ' || *line == '\t') line++;
    *val = line;
    /* trim trailing whitespace/CR */
    char *e = line + strlen(line);
    while (e > line && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
}

/* Call `fn` for every "key value" line in `text`. */
static void for_each_line(char *text, void (*fn)(char *k, char *v, void *ctx), void *ctx) {
    char *p = text;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        char *k, *v;
        kv(line, &k, &v);
        if (k) fn(k, v, ctx);
    }
}

/* ------------------------------------------------------- network.conf ------*/
static void cfg_line(char *k, char *v, void *ctx) {
    (void)ctx;
    if      (!strcmp(k, "dhcp"))     g_netcfg.use_dhcp = (v[0] == '1' || v[0] == 'y' || v[0] == 't');
    else if (!strcmp(k, "ip"))       netcfg_parse_ip(v, &g_netcfg.ip);
    else if (!strcmp(k, "netmask"))  netcfg_parse_ip(v, &g_netcfg.mask);
    else if (!strcmp(k, "gateway"))  netcfg_parse_ip(v, &g_netcfg.gw);
    else if (!strcmp(k, "dns"))      netcfg_parse_ip(v, &g_netcfg.dns);
    else if (!strcmp(k, "hostname")) { strncpy(g_netcfg.hostname, v, NETCFG_HOST_MAX - 1);
                                       g_netcfg.hostname[NETCFG_HOST_MAX - 1] = 0; }
    else if (!strcmp(k, "ntp"))      g_netcfg.ntp_enabled = (v[0] == '1' || v[0] == 'y' || v[0] == 't');
    else if (!strcmp(k, "ntpserver"))netcfg_parse_ip(v, &g_netcfg.ntp_server);
}

int netcfg_load(void) {
    static char buf[1024];
    if (!slurp(NETCFG_PATH, buf, sizeof buf)) return 0;
    for_each_line(buf, cfg_line, 0);
    return 1;
}

int netcfg_save(void) {
    char out[512]; char *o = out;
    const char *hdr =
        "# BoltOS network configuration\n"
        "# dhcp 1        use DHCP (ignores the static values below)\n"
        "# dhcp 0        use the static ip/netmask/gateway/dns\n";
    while (*hdr) *o++ = *hdr++;

    #define EMIT_KEY(name) do { const char *s = name; while (*s) *o++ = *s++; *o++ = ' '; } while (0)
    EMIT_KEY("dhcp");     *o++ = g_netcfg.use_dhcp ? '1' : '0'; *o++ = '\n';
    if (!g_netcfg.use_dhcp) {
        char ipbuf[16];
        EMIT_KEY("ip");      netcfg_format_ip(g_netcfg.ip, ipbuf);   { const char *s = ipbuf; while (*s) *o++ = *s++; } *o++ = '\n';
        EMIT_KEY("netmask"); netcfg_format_ip(g_netcfg.mask, ipbuf); { const char *s = ipbuf; while (*s) *o++ = *s++; } *o++ = '\n';
        EMIT_KEY("gateway"); netcfg_format_ip(g_netcfg.gw, ipbuf);   { const char *s = ipbuf; while (*s) *o++ = *s++; } *o++ = '\n';
    }
    if (g_netcfg.dns) {
        char ipbuf[16];
        EMIT_KEY("dns"); netcfg_format_ip(g_netcfg.dns, ipbuf); { const char *s = ipbuf; while (*s) *o++ = *s++; } *o++ = '\n';
    }
    if (g_netcfg.hostname[0]) {
        EMIT_KEY("hostname"); { const char *s = g_netcfg.hostname; while (*s) *o++ = *s++; } *o++ = '\n';
    }
    EMIT_KEY("ntp"); *o++ = g_netcfg.ntp_enabled ? '1' : '0'; *o++ = '\n';
    if (g_netcfg.ntp_server) {
        char ipbuf[16];
        EMIT_KEY("ntpserver"); netcfg_format_ip(g_netcfg.ntp_server, ipbuf); { const char *s = ipbuf; while (*s) *o++ = *s++; } *o++ = '\n';
    }
    #undef EMIT_KEY
    *o = 0;

    if (!fs_lookup("/etc")) fs_create("/etc", 1);
    fs_node *f = fs_lookup(NETCFG_PATH);
    if (!f) f = fs_create(NETCFG_PATH, 0);
    if (!f) return -1;
    return fs_write(f, out, (uint32_t)(o - out));
}

void netcfg_apply(void) {
    /* A resolver the user picked deliberately wins even under DHCP. Choosing a
     * DNS server is a decision about privacy and filtering as much as about
     * reachability, and having the next lease renewal silently undo it is not
     * what anyone means by "saved". */
    if (g_netcfg.dns)  net_dns  = g_netcfg.dns;
    if (g_netcfg.use_dhcp) return;               /* DHCP fills in the rest */
    if (g_netcfg.ip)   net_ip   = g_netcfg.ip;
    if (g_netcfg.mask) net_mask = g_netcfg.mask;
    if (g_netcfg.gw)   net_gw   = g_netcfg.gw;
    if (g_netcfg.dns)  net_dns  = g_netcfg.dns;
}

/* ---------------------------------------------------------- wifi.conf ------*/
/* One "ssid psk" line per remembered network. Written 0600 because a Wi-Fi
 * passphrase is a credential: every account on the machine being able to read
 * it would defeat the point of having accounts. */
static char wifi_buf[1024];

int netcfg_wifi_save(const char *ssid, const char *psk) {
    if (!ssid || !ssid[0]) return -1;
    netcfg_wifi_forget(ssid);                    /* replace any existing entry */

    fs_node *f = fs_lookup(WIFICFG_PATH);
    if (!f) {
        if (!fs_lookup("/etc")) fs_create("/etc", 1);
        f = fs_create(WIFICFG_PATH, 0);
        if (!f) return -1;
    }
    fs_chown_node(f, 0, 0, 0600);

    char line[NETCFG_SSID_MAX + NETCFG_PSK_MAX + 4]; int n = 0;
    for (const char *s = ssid; *s && n < NETCFG_SSID_MAX; s++) line[n++] = *s;
    line[n++] = ' ';
    for (const char *s = psk ? psk : ""; *s && n < (int)sizeof line - 2; s++) line[n++] = *s;
    line[n++] = '\n';
    return fs_append(f, line, (uint32_t)n);
}

struct wfind { const char *want; char *out; int cap; int found; };
static void wifi_line(char *k, char *v, void *ctx) {
    struct wfind *w = (struct wfind *)ctx;
    if (w->found || !w->want || strcmp(k, w->want) != 0) return;
    if (w->out && w->cap > 0) { strncpy(w->out, v, w->cap - 1); w->out[w->cap - 1] = 0; }
    w->found = 1;
}

int netcfg_wifi_find(const char *ssid, char *psk_out, int cap) {
    if (!slurp(WIFICFG_PATH, wifi_buf, sizeof wifi_buf)) return 0;
    struct wfind w = { ssid, psk_out, cap, 0 };
    for_each_line(wifi_buf, wifi_line, &w);
    return w.found;
}

struct wcount { int n; int want; const char *name; };
static void wifi_count_line(char *k, char *v, void *ctx) {
    (void)v;
    struct wcount *c = (struct wcount *)ctx;
    if (c->want == c->n) c->name = k;
    c->n++;
}

int netcfg_wifi_count(void) {
    if (!slurp(WIFICFG_PATH, wifi_buf, sizeof wifi_buf)) return 0;
    struct wcount c = { 0, -1, 0 };
    for_each_line(wifi_buf, wifi_count_line, &c);
    return c.n;
}

const char *netcfg_wifi_ssid(int i) {
    static char name[NETCFG_SSID_MAX];
    if (!slurp(WIFICFG_PATH, wifi_buf, sizeof wifi_buf)) return "";
    struct wcount c = { 0, i, 0 };
    for_each_line(wifi_buf, wifi_count_line, &c);
    if (!c.name) return "";
    strncpy(name, c.name, sizeof name - 1);
    name[sizeof name - 1] = 0;
    return name;
}

int netcfg_wifi_forget(const char *ssid) {
    fs_node *f = fs_lookup(WIFICFG_PATH);
    if (!f || !f->data) return 0;

    static char keep[1024];
    int n = 0, removed = 0;
    const char *p = (const char *)f->data;
    uint32_t left = f->size;
    while (left) {
        const char *line = p;
        uint32_t len = 0;
        while (len < left && p[len] != '\n') len++;
        uint32_t adv = len + (len < left ? 1 : 0);

        /* keep the line unless its first token is the ssid we are forgetting */
        uint32_t t = 0;
        while (t < len && line[t] != ' ') t++;
        int match = (t == strlen(ssid)) && memcmp(line, ssid, t) == 0;
        if (match) removed = 1;
        else if (len && n + (int)len + 1 < (int)sizeof keep) {
            memcpy(keep + n, line, len); n += (int)len; keep[n++] = '\n';
        }
        p += adv; left -= adv;
    }
    if (removed) fs_write(f, keep, (uint32_t)n);
    return removed;
}

/* ------------------------------------------------------------- hosts -------*/
struct hlook { const char *want; uint32_t ip; };
static void hosts_line(char *k, char *v, void *ctx) {
    /* /etc/hosts is "address name", the conventional order. */
    struct hlook *h = (struct hlook *)ctx;
    if (h->ip) return;
    if (!v || !v[0]) return;
    if (strcmp(v, h->want) != 0) return;
    uint32_t ip;
    if (netcfg_parse_ip(k, &ip) == 0) h->ip = ip;
}

uint32_t netcfg_hosts_lookup(const char *name) {
    static char buf[1024];
    if (!name || !name[0]) return 0;
    if (!slurp(NETCFG_HOSTS, buf, sizeof buf)) return 0;
    struct hlook h = { name, 0 };
    for_each_line(buf, hosts_line, &h);
    return h.ip;
}

int netcfg_hosts_add(const char *name, uint32_t ip) {
    if (!fs_lookup("/etc")) fs_create("/etc", 1);
    fs_node *f = fs_lookup(NETCFG_HOSTS);
    if (!f) f = fs_create(NETCFG_HOSTS, 0);
    if (!f) return -1;
    char line[80]; int n = 0;
    char ipbuf[16]; netcfg_format_ip(ip, ipbuf);
    for (const char *s = ipbuf; *s; s++) line[n++] = *s;
    line[n++] = ' ';
    for (const char *s = name; *s && n < (int)sizeof line - 2; s++) line[n++] = *s;
    line[n++] = '\n';
    return fs_append(f, line, (uint32_t)n);
}
