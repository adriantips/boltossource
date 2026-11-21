/* ===========================================================================
 *  BoltOS Mini  -  mini/bt.c
 *
 *  Bluetooth on the UWE5622 (wcn.c). The chip's firmware is the controller;
 *  this is the host: HCI in H4 framing, out on the chip's channel 3 and in
 *  on its 17, in the packets wcn.c carries.
 *
 *  Starting it: three vendor commands Unisoc's hciattach sends -- the
 *  "pskey" (0xFCA0, with an address made from the H618's ID), the RF tables
 *  (0xFCA2) and "enable" (0xFCA1) -- built from Orange Pi's .ini files by
 *  mini/tools/mkbtcfg.py. Then the usual: reset, address, buffers, events,
 *  Secure Simple Pairing on, a name, page scan on so paired devices can
 *  come back by themselves.
 *
 *  Looking: classic inquiry (with names from the extended inquiry response,
 *  or asked for after) and LE scanning at the same time.
 *
 *  Pairing and using a keyboard or mouse (classic Bluetooth HID):
 *    connect, authenticate -- Secure Simple Pairing, which the controller
 *    computes; we answer its questions: our I/O is "display, yes/no", so a
 *    keyboard gets a six-digit passkey to type and a mouse just pairs --
 *    encrypt, then L2CAP channels to the HID control (PSM 17) and interrupt
 *    (PSM 19) services, the device asked for boot protocol, and its reports
 *    turned into keys and pointer moves. The link key is kept (across a
 *    restart, as Wi-Fi's network is), and a paired device that comes back
 *    by itself is let in.
 *
 *  Bluetooth LE devices are found and listed; pairing them is still to come.
 * ===========================================================================*/
#include <stdint.h>
#include <stdarg.h>
#include "mini.h"
#include "wcn.h"
#include "bt.h"
#include "hid.h"
#include "crypto.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

#define CH_TX   3
#define CH_RX   17
enum { H4_CMD = 1, H4_ACL = 2, H4_EVT = 4 };

#define OP(ogf, ocf) ((uint16_t)((ogf) << 10 | (ocf)))
#define OP_INQUIRY          OP(1, 0x01)
#define OP_INQUIRY_CANCEL   OP(1, 0x02)
#define OP_CREATE_CONN      OP(1, 0x05)
#define OP_DISCONNECT       OP(1, 0x06)
#define OP_ACCEPT_CONN      OP(1, 0x09)
#define OP_REJECT_CONN      OP(1, 0x0A)
#define OP_LINK_KEY_REPLY   OP(1, 0x0B)
#define OP_LINK_KEY_NEG     OP(1, 0x0C)
#define OP_PIN_REPLY        OP(1, 0x0D)
#define OP_AUTH_REQ         OP(1, 0x11)
#define OP_SET_ENCRYPT      OP(1, 0x13)
#define OP_REMOTE_NAME      OP(1, 0x19)
#define OP_IO_CAP_REPLY     OP(1, 0x2B)
#define OP_USER_CONF_REPLY  OP(1, 0x2C)
#define OP_PASSKEY_NEG      OP(1, 0x2F)
#define OP_SET_EVENT_MASK   OP(3, 0x01)
#define OP_RESET            OP(3, 0x03)
#define OP_WRITE_NAME       OP(3, 0x13)
#define OP_WRITE_SCAN       OP(3, 0x1A)
#define OP_WRITE_COD        OP(3, 0x24)
#define OP_WRITE_INQ_MODE   OP(3, 0x45)
#define OP_WRITE_SSP        OP(3, 0x56)
#define OP_WRITE_LE_HOST    OP(3, 0x6D)
#define OP_READ_VERSION     OP(4, 0x01)
#define OP_READ_BUFFER      OP(4, 0x05)
#define OP_READ_BDADDR      OP(4, 0x09)
#define OP_LE_EVENT_MASK    OP(8, 0x01)
#define OP_LE_SCAN_PARAMS   OP(8, 0x0B)
#define OP_LE_SCAN_ENABLE   OP(8, 0x0C)
#define OP_VSC_PSKEY        0xFCA0
#define OP_VSC_ENABLE       0xFCA1
#define OP_VSC_RF           0xFCA2

#define PSM_HID_CTRL 0x11
#define PSM_HID_INTR 0x13

void hid_pointer_move(int dx, int dy, int btn, int wheel);

/* What we know about a device beyond what the Settings page shows. */
typedef struct {
    uint8_t  psrm;                       /* page scan repetition mode       */
    uint16_t clkoff;
    int      handle;                     /* -1: no link                     */
    int      have_key, initiator, encrypted, named;
    uint8_t  key[16];
    /* L2CAP: our channel ids and theirs, and how far each has got */
    uint16_t ctl_l, ctl_r, int_l, int_r;
    int      ctl_up, int_up, cfg_mask;   /* bit 0/1: ours accepted, bit 2/3: theirs answered */
    uint8_t  sig_id;
    /* HID */
    uint8_t  last[8];
    int      rep_key, rep_ch, rep_mods;
    uint64_t rep_at;
    uint8_t  acl[700]; int acl_n, acl_want;
    hid_map_t *map;                      /* from SDP or the Report Map; none: boot format */
    hid_state_t hs;
    /* SDP, for a classic device's HID descriptor */
    uint16_t sdp_l, sdp_r, sdp_tid;
    uint8_t  sdp_cont[17];
    int      sdp_up, sdp_n, sdp_cont_n;
    uint8_t  *sdp_buf;
    /* LE: keys, pairing, and the GATT walk to the HID service */
    uint8_t  peer_type, ltk[16], rnd[8], irk[16];
    uint16_t ediv;
    int      have_ltk, have_irk, smp_mitm;
    uint8_t  preq[7], pres[7], tk[16], mrand[16], sconf[16];
    int      g_step, g_busy, g_rep;
    uint16_t g_mtu, g_start, g_end, g_next, map_handle, map_got, proto_handle;
    struct { uint16_t val, end, cccd, ref; uint8_t props, rid, type; } rep[12];
    int      nrep;
} link_t;

static void give_up(int i, const char *why);

static struct {
    int state;
    char why[120];
    uint8_t addr[6];
    int acl_mtu, acl_credits;
    /* commands: a queue, sent as the controller has room */
    struct { uint16_t op; uint8_t len; uint8_t p[255]; } q[16];
    int qh, qt, cmd_credits;
    volatile uint16_t wait_op;
    volatile int got;
    uint8_t status, rsp[64];
    /* incoming H4, reassembled */
    uint8_t rx[1100];
    int rxn;
    /* devices */
    bt_device_t dev[BT_MAXDEV];
    link_t lk[BT_MAXDEV];
    int ndev, scanning, inquiring, le_scanning, naming, changes, pairing;
    uint64_t scan_end, pair_deadline;
    uint32_t evts, acls, cmds;
} B;

int bt_state(void) { return B.state; }
const char *bt_why(void) { return B.why; }
int bt_scanning(void) { return B.scanning; }
/* What the Settings page lists: paired ones, then the rest that have a name (or are
 * classic), nearest first. An LE beacon with no name isn't something to pair with. */
static int vis[BT_MAXDEV], nvis, vis_at = -1;

static int rank(int i) {
    const bt_device_t *d = &B.dev[i];
    return (d->state >= BTD_PAIRED || d->state == BTD_PAIRING ? 0 : !d->le ? 1 : 2) * 1000 - d->rssi;
}

static void visible(void) {
    if (vis_at == B.changes) return;
    vis_at = B.changes;
    nvis = 0;
    for (int i = 0; i < B.ndev; i++)
        if (B.dev[i].state >= BTD_PAIRING || B.dev[i].name[0] || !B.dev[i].le) vis[nvis++] = i;
    for (int a = 1; a < nvis; a++)
        for (int b = a; b > 0 && rank(vis[b]) < rank(vis[b - 1]); b--) { int t = vis[b]; vis[b] = vis[b - 1]; vis[b - 1] = t; }
}

int bt_device_count(void) { visible(); return nvis; }
const bt_device_t *bt_device(int i) { visible(); return i >= 0 && i < nvis ? &B.dev[vis[i]] : 0; }
static int real(int i) { visible(); return i >= 0 && i < nvis ? vis[i] : -1; }
int bt_changes(void) { return B.changes; }

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(B.why, sizeof B.why, fmt, ap);
    va_end(ap);
    kprintf("[bt] %s\n", B.why);
    B.changes++;
    shell_request_frame();
}

static inline uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* ---- sending ---------------------------------------------------------------------------- */
static void cmd_pump(void) {
    while (B.cmd_credits > 0 && B.qh != B.qt) {
        uint8_t b[4 + 255];
        b[0] = H4_CMD;
        p16(b + 1, B.q[B.qh].op);
        b[3] = B.q[B.qh].len;
        memcpy(b + 4, B.q[B.qh].p, B.q[B.qh].len);
        if (wcn_send(CH_TX, b, 4 + B.q[B.qh].len)) return;
        B.cmd_credits--;
        B.cmds++;
        B.qh = (B.qh + 1) % 16;
    }
}

static int cmd(uint16_t op, const void *p, int len) {
    int next = (B.qt + 1) % 16;
    if (next == B.qh || len > 255) return -1;
    B.q[B.qt].op = op;
    B.q[B.qt].len = (uint8_t)len;
    if (len) memcpy(B.q[B.qt].p, p, (size_t)len);
    B.qt = next;
    cmd_pump();
    return 0;
}

/* Send, and wait for its Command Complete (or Status). The status, or -1. */
static int cmd_wait(uint16_t op, const void *p, int len, uint32_t ms) {
    B.wait_op = op; B.got = 0;
    if (cmd(op, p, len)) { B.wait_op = 0; return -1; }
    uint64_t end = now_ms() + ms;
    while (!B.got && now_ms() < end) { mini_pump(); cmd_pump(); delay_us(100); }
    B.wait_op = 0;
    if (!B.got) { kprintf("[bt] command %04x: no answer\n", op); return -1; }
    return B.status;
}

static int acl_send(link_t *l, uint16_t cid, const uint8_t *data, int len) {
    if (l->handle < 0 || len + 4 > B.acl_mtu) return -1;
    uint8_t b[1 + 4 + 4 + 700];
    b[0] = H4_ACL;
    p16(b + 1, (uint16_t)(l->handle | 0x2000));            /* first, automatically flushable */
    p16(b + 3, (uint16_t)(len + 4));
    p16(b + 5, (uint16_t)len);
    p16(b + 7, cid);
    memcpy(b + 9, data, (size_t)len);
    if (B.acl_credits <= 0) return -1;
    B.acl_credits--;
    return wcn_send(CH_TX, b, 9 + len);
}

/* ---- devices ------------------------------------------------------------------------------- */
static void fmt_addr(const uint8_t *a, char *out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

static int find(const uint8_t *a) {
    for (int i = 0; i < B.ndev; i++) if (!memcmp(B.dev[i].addr, a, 6)) return i;
    return -1;
}

static int by_handle(int h) {
    for (int i = 0; i < B.ndev; i++) if (B.lk[i].handle == (h & 0xFFF)) return i;
    return -1;
}

static const char *cod_kind(uint32_t cod) {
    int major = (int)(cod >> 8) & 0x1F, minor = (int)(cod >> 2) & 0x3F;
    switch (major) {
    case 1: return "Computer";
    case 2: return "Phone";
    case 4: return minor == 1 || minor == 2 ? "Headset" : minor == 6 ? "Headphones" : minor == 5 ? "Speaker" : "Audio";
    case 5: {
        int kb = (minor >> 4) & 3;
        if (kb == 1) return "Keyboard";
        if (kb == 2) return "Mouse";
        if (kb == 3) return "Keyboard and mouse";
        return (minor & 15) == 2 ? "Gamepad" : (minor & 15) == 3 ? "Remote" : "Input device";
    }
    case 6: return "Printer";
    case 7: return "Watch";
    }
    return "Device";
}

static const char *le_kind(uint16_t ap) {
    if (ap == 0x03C1) return "Keyboard (LE)";
    if (ap == 0x03C2) return "Mouse (LE)";
    if (ap >= 0x03C0 && ap < 0x0400) return "Input device (LE)";
    if ((ap >> 6) == 1) return "Phone (LE)";
    if ((ap >> 6) == 2) return "Computer (LE)";
    if ((ap >> 6) == 3) return "Watch (LE)";
    if ((ap >> 6) == 0x25 || (ap >> 6) == 0x21) return "Audio (LE)";
    return "LE device";
}

static int add_device(const uint8_t *a, int le) {
    int i = find(a);
    if (i >= 0) return i;
    if (B.ndev >= BT_MAXDEV) {                              /* full: make room from a nameless LE beacon */
        for (int j = 0; j < B.ndev && i < 0; j++)
            if (B.dev[j].le && !B.dev[j].name[0] && !B.lk[j].have_key && B.lk[j].handle < 0 && B.dev[j].state == BTD_FOUND) i = j;
        if (i < 0) return -1;
    } else i = B.ndev++;
    memset(&B.dev[i], 0, sizeof B.dev[i]);
    memset(&B.lk[i], 0, sizeof B.lk[i]);
    memcpy(B.dev[i].addr, a, 6);
    fmt_addr(a, B.dev[i].addr_str);
    B.dev[i].le = (uint8_t)le;
    B.dev[i].kind = le ? "LE device" : "Device";
    B.lk[i].handle = -1;
    B.changes++;
    return i;
}

/* Names and the like from an extended inquiry response or an LE advertisement. */
static void parse_eir(int i, const uint8_t *d, int len) {
    for (int o = 0; o + 1 < len; ) {
        int l = d[o];
        if (!l || o + 1 + l > len) break;
        int t = d[o + 1];
        const uint8_t *v = d + o + 2;
        if ((t == 0x09 || (t == 0x08 && !B.dev[i].name[0])) && l > 1) {
            int n = l - 1 < (int)sizeof B.dev[i].name - 1 ? l - 1 : (int)sizeof B.dev[i].name - 1;
            memcpy(B.dev[i].name, v, (size_t)n);
            B.dev[i].name[n] = 0;
            B.lk[i].named = 1;
            B.changes++;
        } else if (t == 0x19 && l >= 3) {
            B.dev[i].appearance = g16(v);
            B.dev[i].kind = le_kind(B.dev[i].appearance);
        }
        o += 1 + l;
    }
}

/* ---- kept pairings ----------------------------------------------------------------------- */
typedef struct {
    uint8_t  addr[6], key[16], le, peer_type, have_irk, pad;
    uint8_t  rnd[8], irk[16];
    uint16_t ediv, appearance;
    uint32_t cod;
    char     name[40];
} kept_t;
#define NKEPT 8

static void save_keys(void) {
    kept_t k[NKEPT];
    int n = 0;
    memset(k, 0, sizeof k);
    for (int i = 0; i < B.ndev && n < NKEPT; i++) {
        link_t *l = &B.lk[i];
        if (!l->have_key) continue;
        memcpy(k[n].addr, B.dev[i].addr, 6);
        k[n].le = B.dev[i].le;
        if (B.dev[i].le) {
            memcpy(k[n].key, l->ltk, 16); memcpy(k[n].rnd, l->rnd, 8); k[n].ediv = l->ediv;
            k[n].peer_type = l->peer_type; k[n].have_irk = (uint8_t)l->have_irk; memcpy(k[n].irk, l->irk, 16);
        } else memcpy(k[n].key, l->key, 16);
        k[n].cod = B.dev[i].cod;
        k[n].appearance = B.dev[i].appearance;
        snprintf(k[n].name, sizeof k[n].name, "%s", B.dev[i].name);
        n++;
    }
    if (n) keep_put("bt", k, n * (int)sizeof(kept_t));
    else keep_put("bt", 0, -1);
}

static void load_keys(void) {
    kept_t k[NKEPT];
    int n = keep_get("bt", k, sizeof k) / (int)sizeof(kept_t);
    for (int j = 0; j < n; j++) {
        int i = add_device(k[j].addr, k[j].le);
        if (i < 0) break;
        link_t *l = &B.lk[i];
        l->have_key = 1;
        if (k[j].le) {
            memcpy(l->ltk, k[j].key, 16); memcpy(l->rnd, k[j].rnd, 8); l->ediv = k[j].ediv; l->have_ltk = 1;
            l->peer_type = k[j].peer_type; l->have_irk = k[j].have_irk; memcpy(l->irk, k[j].irk, 16);
            B.dev[i].appearance = k[j].appearance;
            B.dev[i].kind = le_kind(k[j].appearance);
        } else {
            memcpy(l->key, k[j].key, 16);
            B.dev[i].kind = cod_kind(k[j].cod);
        }
        B.dev[i].cod = k[j].cod;
        k[j].name[39] = 0;
        snprintf(B.dev[i].name, sizeof B.dev[i].name, "%s", k[j].name);
        l->named = 1;
        B.dev[i].state = BTD_PAIRED;
    }
    if (n > 0) kprintf("[bt] %d paired device%s remembered\n", n, n == 1 ? "" : "s");
}

/* ---- HID -------------------------------------------------------------------------------------- */
static int hid_mods(uint8_t m) {
    int r = 0;
    if (m & 0x22) r |= MOD_SHIFT;
    if (m & 0x11) r |= MOD_CTRL;
    if (m & 0x44) r |= MOD_ALT;
    if (m & 0x88) r |= MOD_SUPER;
    return r;
}

static void kbd_report(link_t *l, const uint8_t *r, int n) {
    if (n < 8) return;
    int mods = hid_mods(r[0]), any = 0;
    if (r[2] == 1) return;                                  /* rollover */
    input_raw_keys(l->last[0], l->last + 2, 6, r[0], r + 2, 6);
    for (int i = 2; i < 8; i++) {
        uint8_t k = r[i];
        if (k < 4) continue;
        int was = 0;
        for (int j = 2; j < 8; j++) if (l->last[j] == k) was = 1;
        any = 1;
        if (was) continue;
        int ch, key = input_from_hid(k, mods & MOD_SHIFT, &ch);
        input_key(key, ch, mods, SRC_BT);
        l->rep_key = key; l->rep_ch = ch; l->rep_mods = mods;
        l->rep_at = now_ms() + 450;
    }
    if ((l->last[0] & 0x88) && !(r[0] & 0x88) && !any) input_key(KEY_SUPER, 0, 0, SRC_BT);
    if (!any) l->rep_key = 0;
    memcpy(l->last, r, 8);
}

/* Interrupt channel: 0xA1 (input data), then the report: in boot format its ID is 1
 * (keyboard) or 2 (mouse); otherwise the descriptor from SDP says what it is. */
static void hid_input(link_t *l, const uint8_t *d, int n) {
    if (n < 2 || d[0] != 0xA1) return;
    if (l->map) { hid_report(l->map, &l->hs, -1, d + 1, n - 1, SRC_BT); return; }
    if (n < 3) return;
    if (d[1] == 1) kbd_report(l, d + 2, n - 2);
    else if (d[1] == 2 && n >= 5) hid_pointer_move((int8_t)d[3], (int8_t)d[4], d[2], n >= 6 ? (int8_t)d[5] : 0);
}

/* ---- L2CAP --------------------------------------------------------------------------------------- */
static uint16_t next_cid = 0x0040;

static void sig_send(link_t *l, uint8_t code, uint8_t id, const uint8_t *data, int len) {
    uint8_t b[64];
    b[0] = code; b[1] = id;
    p16(b + 2, (uint16_t)len);
    memcpy(b + 4, data, (size_t)len);
    acl_send(l, 0x0001, b, 4 + len);
}

#define PSM_SDP 0x01

static void l2_connect(link_t *l, uint16_t psm) {
    uint8_t d[4];
    uint16_t cid = next_cid++;
    if (psm == PSM_HID_CTRL) l->ctl_l = cid; else if (psm == PSM_HID_INTR) l->int_l = cid; else l->sdp_l = cid;
    p16(d, psm); p16(d + 2, cid);
    sig_send(l, 0x02, ++l->sig_id, d, 4);
}

static void l2_config(link_t *l, uint16_t remote) {
    uint8_t d[8];
    p16(d, remote); p16(d + 2, 0);                          /* no flags */
    d[4] = 0x01; d[5] = 2; p16(d + 6, 672);                 /* our MTU */
    sig_send(l, 0x04, ++l->sig_id, d, 8);
}

/* SDP: one ServiceSearchAttributeRequest for the HID service's descriptor list
 * (attribute 0x0206), continued until the device has sent all of it. */
static void sdp_ask(link_t *l) {
    uint8_t q[5 + 13 + 17];
    int o = 5;
    static const uint8_t pat[] = { 0x35, 0x03, 0x19, 0x11, 0x24, 0x02, 0x80, 0x35, 0x03, 0x09, 0x02, 0x06 };
    memcpy(q + o, pat, sizeof pat); o += sizeof pat;
    q[o++] = (uint8_t)l->sdp_cont_n;
    memcpy(q + o, l->sdp_cont, (size_t)l->sdp_cont_n); o += l->sdp_cont_n;
    q[0] = 0x06; q[1] = (uint8_t)(++l->sdp_tid >> 8); q[2] = (uint8_t)l->sdp_tid;
    q[3] = (uint8_t)((o - 5) >> 8); q[4] = (uint8_t)(o - 5);
    acl_send(l, l->sdp_r, q, o);
}

/* The descriptor inside: ... 0x09 0x02 0x06 (the attribute), sequences, 0x08 0x22, a string. */
static int sdp_descriptor(const uint8_t *b, int n, const uint8_t **out) {
    for (int i = 0; i + 3 < n; i++) {
        if (b[i] != 0x09 || b[i + 1] != 0x02 || b[i + 2] != 0x06) continue;
        for (int j = i + 3; j + 3 < n && j < i + 16; j++)
            if (b[j] == 0x08 && b[j + 1] == 0x22) {
                int t = b[j + 2], len, o;
                if (t == 0x25) { len = b[j + 3]; o = j + 4; }
                else if (t == 0x26 && j + 4 < n) { len = b[j + 3] << 8 | b[j + 4]; o = j + 5; }
                else return -1;
                if (o + len > n) return -1;
                *out = b + o;
                return len;
            }
    }
    return -1;
}

static void sdp_in(int i, const uint8_t *p, int n) {
    link_t *l = &B.lk[i];
    if (n < 7 || p[0] != 0x07 || !l->sdp_buf) return;
    int cnt = p[5] << 8 | p[6];
    if (7 + cnt + 1 > n) return;
    if (l->sdp_n + cnt <= 2048) { memcpy(l->sdp_buf + l->sdp_n, p + 7, (size_t)cnt); l->sdp_n += cnt; }
    l->sdp_cont_n = p[7 + cnt] > 16 ? 0 : p[7 + cnt];
    memcpy(l->sdp_cont, p + 8 + cnt, (size_t)l->sdp_cont_n);
    if (l->sdp_cont_n) { sdp_ask(l); return; }
    const uint8_t *d;
    int dl = sdp_descriptor(l->sdp_buf, l->sdp_n, &d);
    if (dl > 0) {
        hid_map_t *m = kmalloc(sizeof *m);
        if (m && hid_parse(m, d, dl) == 0) {
            l->map = m;
            kprintf("[bt] %s's HID descriptor: %d bytes, %d fields%s%s%s\n", B.dev[i].addr_str, dl, m->nel,
                    m->keyboard ? ", keys" : "", m->mouse ? ", pointer" : "", m->gamepad ? ", gamepad" : "");
        } else if (m) kfree(m);
    } else kprintf("[bt] no HID descriptor from %s's SDP\n", B.dev[i].addr_str);
    kfree(l->sdp_buf); l->sdp_buf = 0;
}

static int is_boot_kind(int i) {
    int major = (int)(B.dev[i].cod >> 8) & 0x1F, kb = (int)(B.dev[i].cod >> 6) & 3;
    return major == 5 && kb != 0 && ((B.dev[i].cod >> 2) & 15) == 0;   /* a plain keyboard or mouse */
}

static void hid_ready(int i) {
    link_t *l = &B.lk[i];
    if (!(l->ctl_up && l->int_up) || B.dev[i].state == BTD_CONNECTED) return;
    if (is_boot_kind(i)) {
        uint8_t boot = 0x70;                                /* SET_PROTOCOL: boot */
        acl_send(l, l->ctl_r, &boot, 1);
    } else if (!l->map && !l->sdp_l) {                      /* a gamepad and the like: its descriptor */
        l->sdp_buf = kmalloc(2048);
        l->sdp_n = 0; l->sdp_cont_n = 0;
        if (l->sdp_buf) l2_connect(l, PSM_SDP);
    }
    B.dev[i].state = BTD_CONNECTED;
    B.pairing = 0; B.pair_deadline = 0;
    say("%s is connected", B.dev[i].name[0] ? B.dev[i].name : B.dev[i].addr_str);
}

static void channel_check(int i) {
    link_t *l = &B.lk[i];
    int was_c = l->ctl_up, was_i = l->int_up, was_s = l->sdp_up;
    l->ctl_up = l->ctl_r && (l->cfg_mask & 0x05) == 0x05;
    l->int_up = l->int_r && (l->cfg_mask & 0x0A) == 0x0A;
    l->sdp_up = l->sdp_r && (l->cfg_mask & 0x30) == 0x30;
    if (l->ctl_up && !was_c && l->initiator && !l->int_l) l2_connect(l, PSM_HID_INTR);
    if ((l->ctl_up && !was_c) || (l->int_up && !was_i)) hid_ready(i);
    if (l->sdp_up && !was_s) sdp_ask(l);
}

static void signaling(int i, const uint8_t *p, int len) {
    link_t *l = &B.lk[i];
    while (len >= 4) {
        uint8_t code = p[0], id = p[1];
        int n = g16(p + 2);
        const uint8_t *d = p + 4;
        if (4 + n > len) break;
        uint8_t r[16];
        switch (code) {
        case 0x02: {                                        /* Connection Request: the device opens one */
            uint16_t psm = g16(d), scid = g16(d + 2), cid = next_cid++;
            int ok = psm == PSM_HID_CTRL || psm == PSM_HID_INTR;
            if (psm == PSM_HID_CTRL) { l->ctl_l = cid; l->ctl_r = scid; }
            else if (psm == PSM_HID_INTR) { l->int_l = cid; l->int_r = scid; }
            p16(r, ok ? cid : 0); p16(r + 2, scid); p16(r + 4, ok ? 0 : 2); p16(r + 6, 0);
            sig_send(l, 0x03, id, r, 8);
            if (ok) l2_config(l, scid);
            break;
        }
        case 0x03: {                                        /* Connection Response to ours */
            uint16_t dcid = g16(d), scid = g16(d + 2), res = g16(d + 4);
            if (res == 1) break;                            /* pending */
            if (res) { give_up(i, "isn't a keyboard, mouse or controller."); break; }
            if (scid == l->ctl_l) l->ctl_r = dcid;
            else if (scid == l->int_l) l->int_r = dcid;
            else if (scid == l->sdp_l) l->sdp_r = dcid;
            l2_config(l, dcid);
            break;
        }
        case 0x04: {                                        /* Configuration Request: whatever it asks */
            uint16_t dcid = g16(d);
            uint16_t remote = dcid == l->ctl_l ? l->ctl_r : dcid == l->int_l ? l->int_r : dcid == l->sdp_l ? l->sdp_r : 0;
            p16(r, remote); p16(r + 2, 0); p16(r + 4, 0);
            sig_send(l, 0x05, id, r, 6);
            if (dcid == l->ctl_l) l->cfg_mask |= 4;
            if (dcid == l->int_l) l->cfg_mask |= 8;
            if (dcid == l->sdp_l) l->cfg_mask |= 0x20;
            channel_check(i);
            break;
        }
        case 0x05: {                                        /* Configuration Response to ours */
            uint16_t scid = g16(d), res = g16(d + 4);
            if (res == 0) {
                if (scid == l->ctl_l) l->cfg_mask |= 1;
                if (scid == l->int_l) l->cfg_mask |= 2;
                if (scid == l->sdp_l) l->cfg_mask |= 0x10;
                channel_check(i);
            }
            break;
        }
        case 0x06: {                                        /* Disconnection Request */
            p16(r, g16(d)); p16(r + 2, g16(d + 2));
            sig_send(l, 0x07, id, r, 4);
            if (g16(d) == l->ctl_l) { l->ctl_r = 0; l->cfg_mask &= ~5; l->ctl_up = 0; }
            if (g16(d) == l->int_l) { l->int_r = 0; l->cfg_mask &= ~10; l->int_up = 0; }
            break;
        }
        case 0x0A: {                                        /* Information Request */
            uint16_t type = g16(d);
            p16(r, type);
            if (type == 2) { p16(r + 2, 0); memset(r + 4, 0, 4); sig_send(l, 0x0B, id, r, 8); }
            else if (type == 3) { p16(r + 2, 0); memset(r + 4, 0, 8); r[4] = 0x02; sig_send(l, 0x0B, id, r, 12); }
            else { p16(r + 2, 1); sig_send(l, 0x0B, id, r, 4); }
            break;
        }
        case 0x01:
            kprintf("[bt] L2CAP command rejected (%d)\n", n >= 2 ? g16(d) : -1);
            break;
        }
        p += 4 + n; len -= 4 + n;
    }
}

static void l2cap_in(int i, const uint8_t *p, int len) {
    if (len < 4) return;
    int n = g16(p), cid = g16(p + 2);
    if (n + 4 > len) return;
    link_t *l = &B.lk[i];
    if (B.dev[i].le) { void le_l2cap(int i, int cid, const uint8_t *p, int n); le_l2cap(i, cid, p + 4, n); return; }
    if (cid == 0x0001) signaling(i, p + 4, n);
    else if (cid == l->int_l && l->int_l) hid_input(l, p + 4, n);
    else if (cid == l->sdp_l && l->sdp_l) sdp_in(i, p + 4, n);
    else if (cid == l->ctl_l && l->ctl_l && n >= 1 && p[4] == 0x15) {   /* HID_CONTROL: virtual cable unplug */
        say("%s unplugged itself", B.dev[i].name);
    }
}

static void acl_in(const uint8_t *p, int len) {
    if (len < 4) return;
    B.acls++;
    int h = g16(p), pb = (h >> 12) & 3, n = g16(p + 2);
    int i = by_handle(h);
    if (i < 0 || n + 4 > len) return;
    link_t *l = &B.lk[i];
    if (pb != 1) {                                          /* a start */
        l->acl_n = 0;
        l->acl_want = n >= 2 ? g16(p + 4) + 4 : 0;
    }
    if (l->acl_n + n > (int)sizeof l->acl) { l->acl_n = 0; return; }
    memcpy(l->acl + l->acl_n, p + 4, (size_t)n);
    l->acl_n += n;
    if (l->acl_want && l->acl_n >= l->acl_want) { l2cap_in(i, l->acl, l->acl_n); l->acl_n = 0; l->acl_want = 0; }
}

/* ---- events --------------------------------------------------------------------------------------- */
static void le_connected(const uint8_t *d, int n, int enhanced);
static void le_encrypted(int i);

static void name_next(void) {
    if (B.inquiring || B.naming) return;
    for (int i = 0; i < B.ndev; i++)
        if (!B.dev[i].le && !B.lk[i].named && B.lk[i].handle < 0) {
            uint8_t p[10];
            memcpy(p, B.dev[i].addr, 6);
            p[6] = B.lk[i].psrm; p[7] = 0; p16(p + 8, (uint16_t)(B.lk[i].clkoff | 0x8000));
            B.lk[i].named = 1;                              /* one try each */
            B.naming = 1;
            cmd(OP_REMOTE_NAME, p, 10);
            return;
        }
}

static void inquiry_result(const uint8_t *p, int with_rssi, int eir) {
    int i = add_device(p, 0);
    if (i < 0) return;
    B.lk[i].psrm = p[6];
    uint32_t cod = (uint32_t)(p[with_rssi ? 8 : 9] | p[with_rssi ? 9 : 10] << 8 | p[with_rssi ? 10 : 11] << 16);
    B.lk[i].clkoff = g16(p + (with_rssi ? 11 : 12));
    B.dev[i].cod = cod;
    B.dev[i].kind = cod_kind(cod);
    if (with_rssi) B.dev[i].rssi = (int8_t)p[13];
    if (eir) parse_eir(i, p + 14, 240);
    B.changes++;
}

static void start_auth(int i) {
    uint8_t p[2];
    p16(p, (uint16_t)B.lk[i].handle);
    cmd(OP_AUTH_REQ, p, 2);
}

static void event_in(const uint8_t *p, int len) {
    if (len < 2) return;
    B.evts++;
    int code = p[0], n = p[1];
    const uint8_t *d = p + 2;
    if (n + 2 > len) return;
    uint8_t r[32];
    int i;
    switch (code) {
    case 0x0E:                                              /* Command Complete */
        if (n < 3) break;
        B.cmd_credits = d[0];
        if (g16(d + 1) == B.wait_op) {
            B.status = n > 3 ? d[3] : 0;
            memcpy(B.rsp, d + 3, n - 3 < (int)sizeof B.rsp ? (size_t)(n - 3) : sizeof B.rsp);
            B.got = 1;
        }
        break;
    case 0x0F:                                              /* Command Status */
        if (n < 4) break;
        B.cmd_credits = d[1];
        if (g16(d + 2) == B.wait_op) { B.status = d[0]; B.got = 1; }
        if (d[0] && g16(d + 2) == OP_CREATE_CONN) { say("Couldn't reach the device (error %d)", d[0]); B.pairing = 0; }
        if (d[0] && g16(d + 2) == OP_REMOTE_NAME) { B.naming = 0; name_next(); }
        break;
    case 0x01:                                              /* Inquiry Complete */
        B.inquiring = 0;
        name_next();
        break;
    case 0x02:
        for (int k = 0; k < d[0] && 1 + 14 * (k + 1) <= n; k++) inquiry_result(d + 1 + 14 * k, 0, 0);
        break;
    case 0x22:
        for (int k = 0; k < d[0] && 1 + 14 * (k + 1) <= n; k++) inquiry_result(d + 1 + 14 * k, 1, 0);
        break;
    case 0x2F:
        if (n >= 255) inquiry_result(d + 1, 1, 1);
        break;
    case 0x07:                                              /* Remote Name Request Complete */
        B.naming = 0;
        if (n >= 7 && d[0] == 0 && (i = find(d + 1)) >= 0) {
            int m = 0;
            while (m < 47 && 7 + m < n && d[7 + m]) m++;
            memcpy(B.dev[i].name, d + 7, (size_t)m);
            B.dev[i].name[m] = 0;
            B.changes++;
        }
        name_next();
        break;
    case 0x3E:                                              /* LE */
        if (n >= 1 && d[0] == 0x01) { le_connected(d + 1, n - 1, 0); break; }
        if (n >= 1 && d[0] == 0x0A) {                       /* enhanced: the same, with two RPAs in between */
            uint8_t c[19];
            if (n < 31) break;
            memcpy(c, d + 1, 11);
            le_connected(c, 11, 1);
            break;
        }
        if (n >= 2 && d[0] == 0x02) {
            int cnt = d[1], o = 2;
            for (int k = 0; k < cnt && o + 9 <= n; k++) {
                int dl = d[o + 8];
                if (o + 9 + dl + 1 > n) break;
                int j = add_device(d + o + 2, 1);
                if (j >= 0) {
                    B.dev[j].addr_type = d[o + 1];
                    B.dev[j].rssi = (int8_t)d[o + 9 + dl];
                    parse_eir(j, d + o + 9, dl);
                }
                o += 10 + dl;
            }
        }
        break;
    case 0x03:                                              /* Connection Complete */
        if (n < 11 || (i = find(d + 3)) < 0) break;
        if (d[0]) {
            say("Couldn't connect to %s (error %d)", B.dev[i].name[0] ? B.dev[i].name : B.dev[i].addr_str, d[0]);
            B.dev[i].state = B.lk[i].have_key ? BTD_PAIRED : BTD_FOUND;
            B.pairing = 0;
            break;
        }
        B.lk[i].handle = g16(d + 1) & 0xFFF;
        B.lk[i].encrypted = 0; B.lk[i].cfg_mask = 0; B.lk[i].ctl_up = B.lk[i].int_up = 0;
        B.lk[i].ctl_l = B.lk[i].ctl_r = B.lk[i].int_l = B.lk[i].int_r = 0;
        kprintf("[bt] link to %s: handle %d\n", B.dev[i].addr_str, B.lk[i].handle);
        if (B.lk[i].initiator) start_auth(i);
        break;
    case 0x04:                                              /* Connection Request: a paired device coming back */
        if (n < 10) break;
        i = find(d);
        memcpy(r, d, 6);
        if (i >= 0 && B.lk[i].have_key && d[9] == 1) {
            r[6] = 0x00;                                    /* and we'll be the central */
            B.lk[i].initiator = 0;
            cmd(OP_ACCEPT_CONN, r, 7);
            kprintf("[bt] %s is coming back\n", B.dev[i].name);
        } else {
            r[6] = 0x0F;                                    /* unacceptable address */
            cmd(OP_REJECT_CONN, r, 7);
        }
        break;
    case 0x05:                                              /* Disconnection Complete */
        if (n >= 4 && (i = by_handle(g16(d + 1))) >= 0) {
            B.lk[i].handle = -1;
            B.lk[i].rep_key = 0;
            B.lk[i].hs.rep_key = 0;
            B.lk[i].sdp_l = B.lk[i].sdp_r = 0; B.lk[i].sdp_up = 0; B.lk[i].cfg_mask = 0;
            B.lk[i].g_step = 0;
            if (B.dev[i].state == BTD_CONNECTED || B.dev[i].state == BTD_PAIRING) {
                B.dev[i].state = B.lk[i].have_key ? BTD_PAIRED : BTD_FOUND;
                if (B.pairing) { say("%s went away (reason %d)", B.dev[i].name[0] ? B.dev[i].name : B.dev[i].addr_str, d[3]); B.pairing = 0; }
                B.changes++;
            }
        }
        break;
    case 0x17:                                              /* Link Key Request */
        if (n < 6) break;
        i = find(d);
        memcpy(r, d, 6);
        if (i >= 0 && B.lk[i].have_key) { memcpy(r + 6, B.lk[i].key, 16); cmd(OP_LINK_KEY_REPLY, r, 22); }
        else cmd(OP_LINK_KEY_NEG, r, 6);
        break;
    case 0x16:                                              /* PIN Code Request: an old device */
        if (n < 6) break;
        memset(r, 0, 23);
        memcpy(r, d, 6);
        r[6] = 4; memcpy(r + 7, "0000", 4);
        cmd(OP_PIN_REPLY, r, 23);
        break;
    case 0x31:                                              /* IO Capability Request */
        if (n < 6) break;
        memcpy(r, d, 6);
        r[6] = 0x01;                                        /* display, yes/no */
        r[7] = 0x00;                                        /* no out-of-band data */
        r[8] = 0x05;                                        /* bonding, with MITM if the device can */
        cmd(OP_IO_CAP_REPLY, r, 9);
        break;
    case 0x33:                                              /* User Confirmation: a mouse, "just works" */
        if (n < 6) break;
        memcpy(r, d, 6);
        cmd(OP_USER_CONF_REPLY, r, 6);
        break;
    case 0x34:                                              /* User Passkey Request: we have no keypad */
        if (n < 6) break;
        memcpy(r, d, 6);
        cmd(OP_PASSKEY_NEG, r, 6);
        break;
    case 0x3B:                                              /* User Passkey Notification: show it */
        if (n >= 10 && (i = find(d)) >= 0) {
            uint32_t pk = (uint32_t)(d[6] | d[7] << 8 | d[8] << 16 | (uint32_t)d[9] << 24);
            say("Type %06u on %s, then press Enter", (unsigned)pk, B.dev[i].name[0] ? B.dev[i].name : "the keyboard");
            B.pair_deadline = now_ms() + 60000;
        }
        break;
    case 0x18:                                              /* Link Key Notification: paired */
        if (n >= 22 && (i = find(d)) >= 0) {
            memcpy(B.lk[i].key, d + 6, 16);
            B.lk[i].have_key = 1;
            save_keys();
            kprintf("[bt] paired with %s (key type %d)\n", B.dev[i].addr_str, d[22]);
        }
        break;
    case 0x06:                                              /* Authentication Complete */
        if (n >= 3 && (i = by_handle(g16(d + 1))) >= 0) {
            if (d[0]) {
                if (d[0] == 0x06 && B.lk[i].have_key) {       /* the device forgot us: pair afresh */
                    B.lk[i].have_key = 0; save_keys();
                    start_auth(i);
                    break;
                }
                say("Pairing with %s failed (error %d)", B.dev[i].name[0] ? B.dev[i].name : B.dev[i].addr_str, d[0]);
                B.pairing = 0;
                uint8_t dc[3]; p16(dc, (uint16_t)B.lk[i].handle); dc[2] = 0x05;
                cmd(OP_DISCONNECT, dc, 3);
                break;
            }
            uint8_t e[3]; p16(e, (uint16_t)B.lk[i].handle); e[2] = 1;
            cmd(OP_SET_ENCRYPT, e, 3);
        }
        break;
    case 0x08:                                              /* Encryption Change */
        if (n >= 4 && (i = by_handle(g16(d + 1))) >= 0 && d[0] && B.dev[i].le) {
            say("Encrypting the link to %s failed (error %d)", B.dev[i].name[0] ? B.dev[i].name : B.dev[i].addr_str, d[0]);
            if (d[0] == 0x06) { B.lk[i].have_ltk = 0; B.lk[i].have_key = 0; save_keys(); }   /* it forgot us */
            break;
        }
        if (n >= 4 && (i = by_handle(g16(d + 1))) >= 0 && d[0] == 0 && d[3] && B.dev[i].le) { le_encrypted(i); break; }
        if (n >= 4 && (i = by_handle(g16(d + 1))) >= 0 && d[0] == 0 && d[3]) {
            B.lk[i].encrypted = 1;
            if (B.dev[i].state < BTD_PAIRED) B.dev[i].state = BTD_PAIRED;
            B.changes++;
            if (B.lk[i].initiator && !B.lk[i].ctl_l) l2_connect(&B.lk[i], PSM_HID_CTRL);
        }
        break;
    case 0x13:                                              /* Number Of Completed Packets */
        for (int k = 0; k < d[0] && 1 + 4 * d[0] <= n; k++) B.acl_credits += g16(d + 1 + 2 * d[0] + 2 * k);   /* handles, then counts */
        break;
    }
    cmd_pump();
}

/* ---- Bluetooth LE: SMP, GATT and HID over GATT -----------------------------------------------------
 *  SMP's arithmetic is AES-128 on little-endian values, so e() turns them round
 *  for the cipher and back. Legacy pairing: c1 makes the confirm values, s1 the
 *  short-term key that encrypts the link while the device hands over its long-
 *  term one. Our I/O is "display only": a keyboard gets a passkey to type, the
 *  rest "just works". */
#define OP_LE_CREATE_CONN   OP(8, 0x0D)
#define OP_LE_CANCEL_CONN   OP(8, 0x0E)
#define OP_LE_CLEAR_ACCEPT  OP(8, 0x10)
#define OP_LE_ADD_ACCEPT    OP(8, 0x11)
#define OP_LE_CONN_UPDATE   OP(8, 0x13)
#define OP_LE_START_ENC     OP(8, 0x19)
#define OP_LE_ADD_RESOLV    OP(8, 0x27)
#define OP_LE_CLEAR_RESOLV  OP(8, 0x29)
#define OP_LE_RESOLV_ENABLE OP(8, 0x2D)
#define CID_ATT 4
#define CID_LESIG 5
#define CID_SMP 6

void crypto_rand_bytes(void *out, uint32_t n);
static int le_waiting;                    /* a create-connection to the accept list is pending */

static void e_le(const uint8_t k[16], const uint8_t in[16], uint8_t out[16]) {
    uint8_t kk[16], ii[16], oo[16];
    for (int i = 0; i < 16; i++) { kk[i] = k[15 - i]; ii[i] = in[15 - i]; }
    aes128_ctx c;
    aes128_init(&c, kk);
    aes128_encrypt(&c, ii, oo);
    for (int i = 0; i < 16; i++) out[i] = oo[15 - i];
}

static void c1(const link_t *l, const uint8_t r[16], const uint8_t ra[6], uint8_t out[16]) {
    uint8_t p1[16], p2[16], t[16];
    p1[0] = 0;                                              /* our address is public */
    p1[1] = l->peer_type ? 1 : 0;
    memcpy(p1 + 2, l->preq, 7);
    memcpy(p1 + 9, l->pres, 7);
    memset(p2, 0, 16);
    memcpy(p2, ra, 6);
    memcpy(p2 + 6, B.addr, 6);
    for (int i = 0; i < 16; i++) t[i] = r[i] ^ p1[i];
    e_le(l->tk, t, t);
    for (int i = 0; i < 16; i++) t[i] ^= p2[i];
    e_le(l->tk, t, out);
}

static void smp_send(link_t *l, const uint8_t *d, int n) { acl_send(l, CID_SMP, d, n); }

static void att_send(link_t *l, const uint8_t *d, int n) { l->g_busy = 1; acl_send(l, CID_ATT, d, n); }

static void le_start_enc(int i, const uint8_t rnd[8], uint16_t ediv, const uint8_t ltk[16]) {
    uint8_t p[28];
    p16(p, (uint16_t)B.lk[i].handle);
    memcpy(p + 2, rnd, 8);
    p16(p + 10, ediv);
    memcpy(p + 12, ltk, 16);
    cmd(OP_LE_START_ENC, p, 28);
}

static void smp_in(int i, const uint8_t *p, int n) {
    link_t *l = &B.lk[i];
    const uint8_t *ra = B.dev[i].addr;
    uint8_t m[17];
    if (n < 1) return;
    switch (p[0]) {
    case 0x02:                                              /* Pairing Response */
        if (n < 7) return;
        memcpy(l->pres, p, 7);
        memset(l->tk, 0, 16);
        l->smp_mitm = p[1] == 0x02 || p[1] == 0x04;         /* it has a keyboard: it types a passkey we show */
        if (l->smp_mitm) {
            uint32_t pk;
            crypto_rand_bytes(&pk, 4);
            pk %= 1000000;
            l->tk[0] = (uint8_t)pk; l->tk[1] = (uint8_t)(pk >> 8); l->tk[2] = (uint8_t)(pk >> 16);
            say("Type %06u on %s, then press Enter", (unsigned)pk, B.dev[i].name[0] ? B.dev[i].name : "the keyboard");
            B.pair_deadline = now_ms() + 60000;
        }
        crypto_rand_bytes(l->mrand, 16);
        m[0] = 0x03;
        c1(l, l->mrand, ra, m + 1);
        smp_send(l, m, 17);
        break;
    case 0x03:                                              /* Pairing Confirm */
        if (n < 17) return;
        memcpy(l->sconf, p + 1, 16);
        m[0] = 0x04;
        memcpy(m + 1, l->mrand, 16);
        smp_send(l, m, 17);
        break;
    case 0x04: {                                            /* Pairing Random: check, then encrypt with the STK */
        if (n < 17) return;
        uint8_t want[16], r[16], stk[16];
        c1(l, p + 1, ra, want);
        if (memcmp(want, l->sconf, 16)) {
            uint8_t f[2] = { 0x05, 0x04 };                  /* failed: confirm value */
            smp_send(l, f, 2);
            give_up(i, "didn't take the passkey. Try again.");
            return;
        }
        memcpy(r, l->mrand, 8);                             /* s1: r1' = Srand's low half, r2' = Mrand's */
        memcpy(r + 8, p + 1, 8);
        e_le(l->tk, r, stk);
        static const uint8_t zero[8];
        le_start_enc(i, zero, 0, stk);
        break;
    }
    case 0x05: {                                            /* Pairing Failed */
        char w[48];
        snprintf(w, sizeof w, "refused to pair (reason %d).", n > 1 ? p[1] : 0);
        give_up(i, w);
        break;
    }
    case 0x06: if (n >= 17) { memcpy(l->ltk, p + 1, 16); } break;               /* Encryption Information */
    case 0x07: if (n >= 11) { l->ediv = g16(p + 1); memcpy(l->rnd, p + 3, 8); l->have_ltk = 1; l->have_key = 1; save_keys(); } break;
    case 0x08: if (n >= 17) { memcpy(l->irk, p + 1, 16); l->have_irk = 1; } break; /* Identity Information */
    case 0x09:                                              /* Identity Address: the one to come back as */
        if (n >= 8 && memcmp(B.dev[i].addr, p + 2, 6)) {
            memcpy(B.dev[i].addr, p + 2, 6);
            l->peer_type = p[1];
            fmt_addr(B.dev[i].addr, B.dev[i].addr_str);
        }
        if (l->have_ltk) save_keys();
        break;
    case 0x0B:                                              /* Security Request: the device wants pairing */
        if (l->have_ltk) le_start_enc(i, l->rnd, l->ediv, l->ltk);
        else { uint8_t q[7] = { 0x01, 0x00, 0x00, 0x05, 16, 0x00, 0x03 }; memcpy(l->preq, q, 7); smp_send(l, q, 7); }
        break;
    }
}

/* GATT, one request at a time: MTU, services, the HID service's characteristics,
 * each report's descriptors and reference, the report map, then notifications on. */
enum { G_MTU = 1, G_SERVICES, G_CHARS, G_DESCS, G_REFS, G_MAP, G_CCCD, G_DONE };

static void gatt_step(int i) {
    link_t *l = &B.lk[i];
    uint8_t q[8];
    switch (l->g_step) {
    case G_MTU:
        q[0] = 0x02; p16(q + 1, 185);
        att_send(l, q, 3);
        break;
    case G_SERVICES:
        q[0] = 0x10; p16(q + 1, l->g_next); p16(q + 3, 0xFFFF); p16(q + 5, 0x2800);
        att_send(l, q, 7);
        break;
    case G_CHARS:
        q[0] = 0x08; p16(q + 1, l->g_next); p16(q + 3, l->g_end); p16(q + 5, 0x2803);
        att_send(l, q, 7);
        break;
    case G_DESCS:
        while (l->g_rep < l->nrep && l->rep[l->g_rep].val + 1 > l->rep[l->g_rep].end) l->g_rep++;
        if (l->g_rep >= l->nrep) { l->g_step = G_REFS; l->g_rep = 0; gatt_step(i); return; }
        q[0] = 0x04; p16(q + 1, l->g_next ? l->g_next : (uint16_t)(l->rep[l->g_rep].val + 1)); p16(q + 3, l->rep[l->g_rep].end);
        att_send(l, q, 5);
        break;
    case G_REFS:
        while (l->g_rep < l->nrep && !l->rep[l->g_rep].ref) l->g_rep++;
        if (l->g_rep >= l->nrep) { l->g_step = G_MAP; l->map_got = 0; gatt_step(i); return; }
        q[0] = 0x0A; p16(q + 1, l->rep[l->g_rep].ref);
        att_send(l, q, 3);
        break;
    case G_MAP:
        if (!l->map_handle) { l->g_step = G_CCCD; l->g_rep = 0; gatt_step(i); return; }
        if (!l->sdp_buf) { l->sdp_buf = kmalloc(2048); l->sdp_n = 0; }
        if (!l->map_got) { q[0] = 0x0A; p16(q + 1, l->map_handle); att_send(l, q, 3); }
        else { q[0] = 0x0C; p16(q + 1, l->map_handle); p16(q + 3, l->map_got); att_send(l, q, 5); }
        break;
    case G_CCCD:
        while (l->g_rep < l->nrep && !(l->rep[l->g_rep].cccd && l->rep[l->g_rep].type == 1)) l->g_rep++;
        if (l->g_rep >= l->nrep) {
            l->g_step = G_DONE;
            B.dev[i].state = BTD_CONNECTED;
            B.pairing = 0; B.pair_deadline = 0;
            say("%s is connected", B.dev[i].name[0] ? B.dev[i].name : B.dev[i].addr_str);
            return;
        }
        q[0] = 0x12; p16(q + 1, l->rep[l->g_rep].cccd); p16(q + 3, 0x0001);
        att_send(l, q, 5);
        break;
    }
}

static void att_in(int i, const uint8_t *p, int n) {
    link_t *l = &B.lk[i];
    if (n < 1) return;
    int op = p[0];
    if (op == 0x1B || op == 0x1D) {                         /* a notification: input */
        if (n < 3) return;
        uint16_t h = g16(p + 1);
        if (op == 0x1D) { uint8_t c = 0x1E; acl_send(l, CID_ATT, &c, 1); }
        for (int k = 0; k < l->nrep; k++)
            if (l->rep[k].val == h && l->map) { hid_report(l->map, &l->hs, l->rep[k].rid, p + 3, n - 3, SRC_BT); break; }
        return;
    }
    if (op == 0x02) { uint8_t r[3] = { 0x03, 23, 0 }; acl_send(l, CID_ATT, r, 3); return; }   /* they ask our MTU */
    l->g_busy = 0;
    int err = op == 0x01 ? (n >= 5 ? p[4] : 1) : 0;
    switch (l->g_step) {
    case G_MTU:
        l->g_step = G_SERVICES; l->g_next = 1;
        break;
    case G_SERVICES:
        if (err) {
            if (!l->g_start) { l->g_step = 0; give_up(i, "isn't a keyboard, mouse or controller."); return; }
            l->g_step = G_CHARS; l->g_next = l->g_start;
            break;
        }
        if (op == 0x11 && n >= 2) {
            int len = p[1];
            for (int o = 2; len >= 6 && o + len <= n; o += len) {
                uint16_t st = g16(p + o), en = g16(p + o + 2);
                if (len == 6 && g16(p + o + 4) == 0x1812 && !l->g_start) { l->g_start = st; l->g_end = en; }
                l->g_next = (uint16_t)(en + 1);
                if (en == 0xFFFF) { l->g_next = 0; }
            }
            if (!l->g_next && !l->g_start) { l->g_step = 0; give_up(i, "isn't a keyboard, mouse or controller."); return; }
            if (!l->g_next) { l->g_step = G_CHARS; l->g_next = l->g_start; }
        }
        break;
    case G_CHARS:
        if (err || op != 0x09) {
            for (int k = 0; k < l->nrep; k++) if (!l->rep[k].end) l->rep[k].end = l->g_end;
            l->g_step = G_DESCS; l->g_rep = 0; l->g_next = 0;
            break;
        }
        {
            int len = p[1];
            for (int o = 2; len >= 7 && o + len <= n; o += len) {
                uint16_t decl = g16(p + o), val = g16(p + o + 3), uuid = len == 7 ? g16(p + o + 5) : 0;
                for (int k = 0; k < l->nrep; k++) if (!l->rep[k].end) l->rep[k].end = (uint16_t)(decl - 1);
                if (uuid == 0x2A4B) l->map_handle = val;
                else if (uuid == 0x2A4E) l->proto_handle = val;
                else if (uuid == 0x2A4D && l->nrep < 12) {
                    memset(&l->rep[l->nrep], 0, sizeof l->rep[0]);
                    l->rep[l->nrep].val = val; l->rep[l->nrep].props = p[o + 2]; l->rep[l->nrep].type = 1;
                    l->nrep++;
                }
                l->g_next = (uint16_t)(val + 1);
            }
            if (l->g_next > l->g_end) { for (int k = 0; k < l->nrep; k++) if (!l->rep[k].end) l->rep[k].end = l->g_end; l->g_step = G_DESCS; l->g_rep = 0; l->g_next = 0; }
        }
        break;
    case G_DESCS:
        if (!err && op == 0x05 && n >= 2 && p[1] == 1) {
            uint16_t last = 0;
            for (int o = 2; o + 4 <= n; o += 4) {
                uint16_t h = g16(p + o), u = g16(p + o + 2);
                if (u == 0x2902) l->rep[l->g_rep].cccd = h;
                if (u == 0x2908) l->rep[l->g_rep].ref = h;
                last = h;
            }
            if (last && last < l->rep[l->g_rep].end) { l->g_next = (uint16_t)(last + 1); break; }
        }
        l->g_rep++; l->g_next = 0;
        break;
    case G_REFS:
        if (!err && op == 0x0B && n >= 3) { l->rep[l->g_rep].rid = p[1]; l->rep[l->g_rep].type = p[2]; }
        l->g_rep++;
        break;
    case G_MAP:
        if (!err && (op == 0x0B || op == 0x0D) && l->sdp_buf) {
            int k = n - 1;
            if (l->sdp_n + k <= 2048) { memcpy(l->sdp_buf + l->sdp_n, p + 1, (size_t)k); l->sdp_n += k; }
            l->map_got = (uint16_t)(l->map_got + k);
            if (k >= 22) break;                             /* a full MTU's worth: there may be more */
        }
        if (l->sdp_buf && l->sdp_n) {
            hid_map_t *m = l->map ? l->map : kmalloc(sizeof *m);
            if (m && hid_parse(m, l->sdp_buf, l->sdp_n) == 0) {
                l->map = m;
                kprintf("[bt] %s's report map: %d bytes, %d fields%s%s%s, %d reports\n", B.dev[i].addr_str, l->sdp_n, m->nel,
                        m->keyboard ? ", keys" : "", m->mouse ? ", pointer" : "", m->gamepad ? ", gamepad" : "", l->nrep);
            } else if (m && m != l->map) kfree(m);
        }
        if (l->sdp_buf) { kfree(l->sdp_buf); l->sdp_buf = 0; }
        if (l->proto_handle) { uint8_t w[4] = { 0x52, 0, 0, 1 }; p16(w + 1, l->proto_handle); acl_send(l, CID_ATT, w, 4); }   /* report protocol */
        l->g_step = G_CCCD; l->g_rep = 0;
        break;
    case G_CCCD:
        l->g_rep++;
        break;
    }
    if (l->g_step && l->g_step != G_DONE && !l->g_busy) gatt_step(i);
}

void le_l2cap(int i, int cid, const uint8_t *p, int n) {
    link_t *l = &B.lk[i];
    if (cid == CID_SMP) smp_in(i, p, n);
    else if (cid == CID_ATT) att_in(i, p, n);
    else if (cid == CID_LESIG && n >= 12 && p[0] == 0x12) {  /* connection parameters: yes, and ask the controller */
        uint8_t r[6] = { 0x13, p[1], 2, 0, 0, 0 };
        acl_send(l, CID_LESIG, r, 6);
        uint8_t u[14];
        p16(u, (uint16_t)l->handle);
        memcpy(u + 2, p + 4, 8);
        p16(u + 10, 0); p16(u + 12, 0);
        cmd(OP_LE_CONN_UPDATE, u, 14);
    }
}

/* Paired LE devices: the controller connects to whichever of them advertises. */
static void le_autoconnect(void) {
    if (le_waiting || B.pairing || B.scanning) return;
    int any = 0;
    for (int i = 0; i < B.ndev; i++) if (B.dev[i].le && B.lk[i].have_ltk && B.lk[i].handle < 0) any = 1;
    if (!any) return;
    cmd(OP_LE_CLEAR_ACCEPT, 0, 0);
    cmd(OP_LE_CLEAR_RESOLV, 0, 0);
    for (int i = 0; i < B.ndev; i++) {
        link_t *l = &B.lk[i];
        if (!B.dev[i].le || !l->have_ltk || l->handle >= 0) continue;
        uint8_t a[7] = { l->peer_type };
        memcpy(a + 1, B.dev[i].addr, 6);
        cmd(OP_LE_ADD_ACCEPT, a, 7);
        if (l->have_irk) {
            uint8_t rl[39];
            rl[0] = l->peer_type; memcpy(rl + 1, B.dev[i].addr, 6);
            memcpy(rl + 7, l->irk, 16); memset(rl + 23, 0, 16);
            cmd(OP_LE_ADD_RESOLV, rl, 39);
        }
    }
    uint8_t on = 1;
    cmd(OP_LE_RESOLV_ENABLE, &on, 1);
    uint8_t c[25] = { 0x60, 0x00, 0x30, 0x00, 0x01 };       /* the accept list */
    c[12] = 0;                                              /* our address: public */
    p16(c + 13, 6); p16(c + 15, 12); p16(c + 17, 0); p16(c + 19, 200); p16(c + 21, 0); p16(c + 23, 0);
    cmd(OP_LE_CREATE_CONN, c, 25);
    le_waiting = 1;
}

static void le_cancel_wait(void) {
    if (!le_waiting) return;
    cmd(OP_LE_CANCEL_CONN, 0, 0);
    le_waiting = 0;
}

static void le_connected(const uint8_t *d, int n, int enhanced) {
    if (n < 11) return;
    le_waiting = 0;
    int i = find(d + 5);
    if (d[0]) {
        if (d[0] != 0x02) say("Couldn't connect (LE error %d)", d[0]);   /* 2: we cancelled it */
        if (i >= 0 && B.dev[i].state == BTD_PAIRING) { B.dev[i].state = BTD_FOUND; B.pairing = 0; }
        return;
    }
    if (i < 0) i = add_device(d + 5, 1);
    if (i < 0) return;
    (void)enhanced;
    link_t *l = &B.lk[i];
    l->handle = g16(d + 1) & 0xFFF;
    l->peer_type = d[4] & 1;
    l->g_step = 0; l->g_busy = 0; l->g_start = l->g_end = 0; l->nrep = 0; l->map_handle = l->proto_handle = 0;
    kprintf("[bt] LE link to %s: handle %d\n", B.dev[i].addr_str, l->handle);
    if (l->have_ltk) le_start_enc(i, l->rnd, l->ediv, l->ltk);
    else {
        uint8_t q[7] = { 0x01, 0x00, 0x00, 0x05, 16, 0x00, 0x03 };   /* display only, bond with MITM, its keys */
        memcpy(l->preq, q, 7);
        smp_send(l, q, 7);
    }
}

static void le_encrypted(int i) {
    link_t *l = &B.lk[i];
    if (B.dev[i].state < BTD_PAIRED) B.dev[i].state = BTD_PAIRED;
    B.changes++;
    if (!l->g_step) { l->g_step = G_MTU; gatt_step(i); }
}

/* ---- the H4 stream ------------------------------------------------------------------------------ */
static void rx(int chn, const uint8_t *p, int len) {
    (void)chn;
    for (int k = 0; k < len; k++) {
        if (B.rxn >= (int)sizeof B.rx) B.rxn = 0;
        B.rx[B.rxn++] = p[k];
        int need = 0;
        if (B.rx[0] == H4_EVT) need = B.rxn >= 3 ? 3 + B.rx[2] : 0;
        else if (B.rx[0] == H4_ACL) need = B.rxn >= 5 ? 5 + g16(B.rx + 3) : 0;
        else { B.rxn = 0; continue; }                       /* lost our place: wait for a type byte */
        if (need && B.rxn >= need) {
            if (B.rx[0] == H4_EVT) event_in(B.rx + 1, need - 1);
            else acl_in(B.rx + 1, need - 1);
            B.rxn = 0;
        }
    }
}

/* ---- giving up on a device ------------------------------------------------------------------------ */
#define OP_CREATE_CONN_CANCEL OP(1, 0x08)
static void le_cancel_wait(void);

static void drop_link(int i, int reason) {
    link_t *l = &B.lk[i];
    if (l->handle >= 0) { uint8_t dc[3]; p16(dc, (uint16_t)l->handle); dc[2] = (uint8_t)reason; cmd(OP_DISCONNECT, dc, 3); }
    else if (B.dev[i].state == BTD_PAIRING) {
        if (B.dev[i].le) cmd(OP(8, 0x0E), 0, 0);            /* LE Create Connection Cancel */
        else cmd(OP_CREATE_CONN_CANCEL, B.dev[i].addr, 6);
    }
}

/* Not something Mini can use (or it never answered): let go, and don't keep it. */
static void give_up(int i, const char *why) {
    const char *nm = B.dev[i].name[0] ? B.dev[i].name : B.dev[i].addr_str;
    drop_link(i, 0x13);
    if (B.dev[i].state == BTD_PAIRING || !B.lk[i].map) {    /* paired just now and of no use: forget it */
        if (B.lk[i].have_key && B.dev[i].state != BTD_CONNECTED) { B.lk[i].have_key = 0; B.lk[i].have_ltk = 0; save_keys(); }
    }
    B.dev[i].state = B.lk[i].have_key ? BTD_PAIRED : BTD_FOUND;
    B.pairing = 0;
    B.changes++;
    say("%s %s", nm, why);
}

/* ---- starting ------------------------------------------------------------------------------------- */
int bt_start(void) {
    if (B.state == BT_READY || B.state == BT_STARTING) return 0;
    B.state = BT_STARTING;
    B.changes++;
    keep_put("btoff", 0, -1);
    shell_request_frame();
    if (wcn_start()) { B.state = BT_FAILED; say("%s", wcn_why()); return -1; }
    wcn_set_rx(CH_RX, rx);
    B.cmd_credits = 1;
    B.qh = B.qt = 0;
    B.rxn = 0;

    /* The vendor's three: settings (with our own address), RF tables, enable. */
    uint32_t pl, rl;
    const uint8_t *ps = wcn_blob(WCN_FW_BT_PSKEY, &pl), *rf = wcn_blob(WCN_FW_BT_RF, &rl);
    int plen = (int)pl, rlen = (int)rl;
    if (plen < 26 || plen > 255 || rlen < 1 || rlen > 255) { B.state = BT_FAILED; say("This kernel has no Bluetooth settings, and the card has no copy of them"); return -1; }
    uint8_t pskey[255];
    memcpy(pskey, ps, (size_t)plen);
    uint32_t h = 0x811C9DC5u;                               /* from the H618's ID, as the USB link's MAC */
    for (int i = 0; i < 4; i++) { h ^= r32(0x03006200u + 4u * (uint32_t)i) + 0x42u; h *= 16777619u; }
    uint8_t a[6] = { (uint8_t)h, (uint8_t)(h >> 8), (uint8_t)(h >> 16), (uint8_t)(h >> 24), 0x2E, 0xB0 };
    if (a[2] == 0x9E && a[1] == 0x8B && a[0] < 0x40) a[0] |= 0x40;   /* not an inquiry access code */
    memcpy(pskey + 20, a, 6);
    if (cmd_wait(OP_VSC_PSKEY, pskey, plen, 2000) < 0 || cmd_wait(OP_VSC_RF, rf, rlen, 2000) < 0) {
        B.state = BT_FAILED; say("The Bluetooth controller didn't answer"); return -1;
    }
    uint8_t en[3] = { 0, 0, 1 };                            /* dual mode, enable */
    if (cmd_wait(OP_VSC_ENABLE, en, 3, 2000) < 0) { B.state = BT_FAILED; say("The Bluetooth controller wouldn't start"); return -1; }
    delay_ms(20);
    if (cmd_wait(OP_RESET, 0, 0, 3000)) { B.state = BT_FAILED; say("The Bluetooth controller didn't reset"); return -1; }

    if (cmd_wait(OP_READ_BDADDR, 0, 0, 1000) == 0) memcpy(B.addr, B.rsp + 1, 6);
    if (cmd_wait(OP_READ_BUFFER, 0, 0, 1000) == 0) { B.acl_mtu = g16(B.rsp + 1); B.acl_credits = g16(B.rsp + 4); }
    if (B.acl_mtu <= 0 || B.acl_mtu > 700) B.acl_mtu = B.acl_mtu > 700 ? 700 : 300;
    if (B.acl_credits <= 0) B.acl_credits = 4;
    int ver = -1;
    if (cmd_wait(OP_READ_VERSION, 0, 0, 1000) == 0) ver = B.rsp[1];
    static const uint8_t mask[8] = { 0xFF, 0xFF, 0xFB, 0xFF, 0x07, 0xF8, 0xBF, 0x3D };   /* BlueZ's: SSP and LE too */
    cmd_wait(OP_SET_EVENT_MASK, mask, 8, 1000);
    uint8_t one = 1, two = 2;
    cmd_wait(OP_WRITE_SSP, &one, 1, 1000);
    cmd_wait(OP_WRITE_INQ_MODE, &two, 1, 1000);
    uint8_t le_host[2] = { 1, 0 };
    cmd_wait(OP_WRITE_LE_HOST, le_host, 2, 1000);
    uint8_t lemask[8] = { 0x1F, 0x02, 0, 0, 0, 0, 0, 0 };   /* + enhanced connection complete */
    cmd_wait(OP_LE_EVENT_MASK, lemask, 8, 1000);
    uint8_t cod[3] = { 0x0C, 0x01, 0x10 };                  /* a laptop-ish computer */
    cmd_wait(OP_WRITE_COD, cod, 3, 1000);
    uint8_t name[248];
    memset(name, 0, sizeof name);
    memcpy(name, "BoltOS Mini", 11);
    cmd_wait(OP_WRITE_NAME, name, 248, 1000);
    uint8_t scan = 0x02;                                    /* page scan: paired devices can come back */
    cmd_wait(OP_WRITE_SCAN, &scan, 1, 1000);

    load_keys();
    B.state = BT_READY;
    char ad[18];
    fmt_addr(B.addr, ad);
    kprintf("[bt] controller up: %s, HCI version %d, ACL %d bytes x %d\n", ad, ver, B.acl_mtu, B.acl_credits);
    say("On. This Pi is %s.", ad);
    return 0;
}

/* ---- looking ---------------------------------------------------------------------------------- */
int bt_scan(void) {
    if (B.state != BT_READY || B.scanning) return -1;
    le_cancel_wait();
    /* Forget what was only seen, keep what's paired. */
    int k = 0;
    for (int i = 0; i < B.ndev; i++)
        if (B.lk[i].have_key || B.lk[i].handle >= 0) { if (k != i) { B.dev[k] = B.dev[i]; B.lk[k] = B.lk[i]; } k++; }
    B.ndev = k;
    uint8_t inq[5] = { 0x33, 0x8B, 0x9E, 0x08, 0x00 };     /* GIAC, 10.24 s, as many as answer */
    B.inquiring = cmd(OP_INQUIRY, inq, 5) == 0;
    uint8_t sp[7] = { 0x01, 0x30, 0x00, 0x30, 0x00, 0x00, 0x00 };   /* active, 30 ms of every 30 ms */
    cmd(OP_LE_SCAN_PARAMS, sp, 7);
    uint8_t se[2] = { 1, 1 };
    cmd(OP_LE_SCAN_ENABLE, se, 2);
    B.le_scanning = 1;
    B.scanning = 1;
    B.scan_end = now_ms() + 11000;
    say("Looking for devices\x85");
    return 0;
}

/* ---- pairing ---------------------------------------------------------------------------------- */
int bt_pair(int vi) {
    int i = real(vi);
    if (B.state != BT_READY || i < 0 || B.pairing) return -1;
    bt_device_t *d = &B.dev[i];
    link_t *l = &B.lk[i];
    if (l->handle >= 0) { say("%s is already connected", d->name[0] ? d->name : d->addr_str); return 0; }
    if (!d->le && ((d->cod >> 8) & 0x1F) == 4) { say("Mini has no sound output yet, so headphones and speakers can't be used."); return -1; }
    B.pair_deadline = now_ms() + 30000;
    if (B.inquiring) { cmd(OP_INQUIRY_CANCEL, 0, 0); B.inquiring = 0; }
    if (B.le_scanning) { uint8_t se[2] = { 0, 0 }; cmd(OP_LE_SCAN_ENABLE, se, 2); B.le_scanning = 0; }
    B.scanning = 0;
    le_cancel_wait();
    if (d->le) {
        uint8_t c[25] = { 0x60, 0x00, 0x30, 0x00, 0x00 };   /* this one device */
        c[5] = d->addr_type & 1;
        memcpy(c + 6, d->addr, 6);
        c[12] = 0;
        p16(c + 13, 6); p16(c + 15, 12); p16(c + 17, 0); p16(c + 19, 200); p16(c + 21, 0); p16(c + 23, 0);
        l->peer_type = d->addr_type & 1;
        d->state = BTD_PAIRING;
        B.pairing = 1;
        B.changes++;
        say("Connecting to %s\x85", d->name[0] ? d->name : d->addr_str);
        return cmd(OP_LE_CREATE_CONN, c, 25);
    }
    uint8_t p[13];
    memcpy(p, d->addr, 6);
    p16(p + 6, 0xCC18);                                     /* DM1, DH1, DM3, DH3, DM5, DH5 */
    p[8] = l->psrm; p[9] = 0;
    p16(p + 10, (uint16_t)(l->clkoff | (l->clkoff ? 0x8000 : 0)));
    p[12] = 1;                                              /* role switch allowed */
    l->initiator = 1;
    d->state = BTD_PAIRING;
    B.pairing = 1;
    B.changes++;
    say("Connecting to %s\x85", d->name[0] ? d->name : d->addr_str);
    return cmd(OP_CREATE_CONN, p, 13);
}

int bt_cancel(int vi) {
    int i = real(vi);
    if (i < 0 || B.dev[i].state != BTD_PAIRING) return -1;
    give_up(i, "wasn't paired.");
    return 0;
}

int bt_disconnect(int vi) {
    int i = real(vi);
    if (i < 0 || B.lk[i].handle < 0) return -1;
    drop_link(i, 0x13);
    return 0;
}

/* Off: let every device go, stop looking and listening, and turn the controller's
 * Bluetooth off (the chip stays up for Wi-Fi). Stays off across a restart. */
void bt_stop(void) {
    if (B.state != BT_READY) return;
    if (B.inquiring) { cmd(OP_INQUIRY_CANCEL, 0, 0); B.inquiring = 0; }
    if (B.le_scanning) { uint8_t se[2] = { 0, 0 }; cmd(OP_LE_SCAN_ENABLE, se, 2); B.le_scanning = 0; }
    le_cancel_wait();
    for (int i = 0; i < B.ndev; i++) if (B.lk[i].handle >= 0) drop_link(i, 0x15);   /* powering off */
    uint8_t scan = 0;
    cmd_wait(OP_WRITE_SCAN, &scan, 1, 1000);
    uint8_t dis[3] = { 0, 0, 0 };
    cmd_wait(OP_VSC_ENABLE, dis, 3, 1000);
    for (int i = 0; i < B.ndev; i++) { B.lk[i].handle = -1; if (B.dev[i].state > BTD_PAIRED) B.dev[i].state = BTD_PAIRED; }
    B.scanning = 0; B.pairing = 0;
    B.state = BT_OFF;
    uint8_t off = 1;
    keep_put("btoff", &off, 1);
    say("Off.");
}

int bt_forget(int vi) {
    int i = real(vi);
    if (i < 0) return -1;
    link_t *l = &B.lk[i];
    if (l->handle >= 0) { uint8_t dc[3]; p16(dc, (uint16_t)l->handle); dc[2] = 0x13; cmd(OP_DISCONNECT, dc, 3); }
    l->have_key = 0;
    B.dev[i].state = BTD_FOUND;
    save_keys();
    say("Forgot %s", B.dev[i].name[0] ? B.dev[i].name : B.dev[i].addr_str);
    return 0;
}

void bt_poll(void) {
    static int autostarted;
    if (!autostarted && B.state == BT_OFF && now_ms() > 4000 && g_plat == PLAT_H618) {
        autostarted = 1;                                    /* a paired keyboard should just work */
        if (keep_get("bt", 0, 0) >= 0 && keep_get("btoff", 0, 0) < 0) bt_start();
        return;
    }
    if (B.state != BT_READY) return;
    cmd_pump();
    if (B.scanning && now_ms() > B.scan_end) {
        if (B.le_scanning) { uint8_t se[2] = { 0, 0 }; cmd(OP_LE_SCAN_ENABLE, se, 2); B.le_scanning = 0; }
        B.scanning = 0;
        B.changes++;
        int k = bt_device_count();
        say(k ? "Found %d device%s. Return pairs one." : "Nothing found%.0d%s. Put the device in pairing mode, then look again.", k, k == 1 ? "" : "s");
    }
    for (int i = 0; i < B.ndev; i++) {                      /* held keys repeat */
        link_t *l = &B.lk[i];
        if (l->rep_key && now_ms() >= l->rep_at) { input_key(l->rep_key, l->rep_ch, l->rep_mods, SRC_BT); l->rep_at = now_ms() + 33; }
        if (l->handle >= 0) hid_repeat(&l->hs, SRC_BT);
    }
    if (B.pairing && B.pair_deadline && now_ms() > B.pair_deadline) {
        B.pair_deadline = 0;
        for (int i = 0; i < B.ndev; i++)
            if (B.dev[i].state == BTD_PAIRING) give_up(i, "didn't answer. Check it's in pairing mode, then try again.");
        B.pairing = 0;
    }
    static uint64_t le_at;
    if (!le_waiting && !B.pairing && !B.scanning && now_ms() > le_at) { le_at = now_ms() + 5000; le_autoconnect(); }
}

int bt_debug(char *out, int cap) {
    char a[18];
    fmt_addr(B.addr, a);
    return snprintf(out, (size_t)cap, "bt: state %d (%s), %s, %d devices, events %u, acl %u, commands %u, credits cmd %d acl %d\n",
                    B.state, B.why, a, B.ndev, (unsigned)B.evts, (unsigned)B.acls, (unsigned)B.cmds, B.cmd_credits, B.acl_credits);
}
