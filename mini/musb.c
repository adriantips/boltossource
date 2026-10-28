/* ===========================================================================
 *  BoltOS Mini  -  mini/musb.c
 *
 *  The Orange Pi Zero3's USB-C socket, as a USB *device*: plugged into a
 *  Mac, the Pi shows up as a CDC Ethernet adapter called "BoltOS Mini". macOS
 *  drives CDC-ECM with its own driver, so nothing is installed on the Mac;
 *  the network on top (DHCP, VNC, the web page) is netsvc.c's.
 *
 *  The controller is Allwinner's Mentor Graphics MUSB, at 0x05100000, with
 *  Allwinner's own register layout (the same block U-Boot uses for DFU two
 *  seconds earlier). Everything is polled from the main loop:
 *
 *    ep0      control: enumeration, and the two CDC requests macOS sends
 *    ep1 IN   bulk, frames to the Mac (512-byte packets, double-buffered)
 *    ep2 OUT  bulk, frames from the Mac
 *    ep3 IN   interrupt, the "cable connected, 480 Mbit/s" notifications
 *
 *  An Ethernet frame is a run of full packets ended by a short one (or a
 *  zero-length one when the frame is an exact multiple of 512).
 *
 *  The register sequence follows U-Boot's sunxi glue (drivers/usb/musb-new/
 *  sunxi.c) and its PHY driver for the H616 (phy-sun4i-usb.c), including the
 *  H616 quirk where PHY0 only works once PHY2 has been powered up.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kprintf.h"
#include "string.h"

#define MUSB        0x05100000u
#define M_FIFO(n)   (MUSB + 0x00 + (n) * 4)
#define M_POWER     (MUSB + 0x40)
#define M_DEVCTL    (MUSB + 0x41)
#define M_INDEX     (MUSB + 0x42)
#define M_VEND0     (MUSB + 0x43)
#define M_INTRTX    (MUSB + 0x44)
#define M_INTRRX    (MUSB + 0x46)
#define M_INTRTXE   (MUSB + 0x48)
#define M_INTRRXE   (MUSB + 0x4A)
#define M_INTRUSB   (MUSB + 0x4C)
#define M_INTRUSBE  (MUSB + 0x50)
#define M_TESTMODE  (MUSB + 0x7C)
#define M_TXMAXP    (MUSB + 0x80)
#define M_CSR0      (MUSB + 0x82)
#define M_TXCSR     (MUSB + 0x82)
#define M_RXMAXP    (MUSB + 0x84)
#define M_RXCSR     (MUSB + 0x86)
#define M_COUNT0    (MUSB + 0x88)
#define M_RXCOUNT   (MUSB + 0x88)
#define M_TXFIFOSZ  (MUSB + 0x90)
#define M_TXFIFOADD (MUSB + 0x92)
#define M_RXFIFOSZ  (MUSB + 0x94)
#define M_RXFIFOADD (MUSB + 0x96)
#define M_FADDR     (MUSB + 0x98)
#define M_ISCR      (MUSB + 0x400)

#define PHY_CTRL    0x05100400u           /* the PHY block's base            */
#define PHY_PHYCTL  (PHY_CTRL + 0x10)
#define PHY_OTGCTL  (PHY_CTRL + 0x20)
#define PMU2        0x05310800u
#define CCU         0x03001000u
#define SRAMC       0x03000000u

/* POWER */
#define P_HSENAB    0x20
#define P_SOFTCONN  0x40
#define P_HSMODE    0x10
/* INTRUSB */
#define I_SUSPEND   0x01
#define I_RESUME    0x02
#define I_RESET     0x04
#define I_DISCON    0x20
/* CSR0 (peripheral) */
#define C0_RXPKTRDY 0x0001
#define C0_TXPKTRDY 0x0002
#define C0_SENTSTALL 0x0004
#define C0_DATAEND  0x0008
#define C0_SETUPEND 0x0010
#define C0_SENDSTALL 0x0020
#define C0_SVDRXPKTRDY 0x0040
#define C0_SVDSETUPEND 0x0080
/* TXCSR / RXCSR */
#define TX_PKTRDY   0x0001
#define TX_FIFONE   0x0002
#define TX_FLUSH    0x0008
#define TX_CLRTOG   0x0040
#define TX_MODE     0x2000
#define RX_PKTRDY   0x0001
#define RX_FLUSH    0x0010
#define RX_CLRTOG   0x0080

#define EP_IN   1
#define EP_OUT  2
#define EP_INT  3

/* ---- descriptors ----------------------------------------------------------------- */
#define VID 0x1D6B                       /* Linux Foundation, as Linux gadgets use */
#define PID 0x0104

static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02,                   /* USB 2.0                          */
    0x02, 0x00, 0x00,                    /* class: Communications            */
    64,
    VID & 0xFF, VID >> 8, PID & 0xFF, PID >> 8,
    0x00, 0x01,                          /* bcdDevice 1.00                   */
    1, 2, 3,                             /* manufacturer, product, serial    */
    1
};

static const uint8_t qual_desc[10] = { 10, 6, 0x00, 0x02, 0x02, 0x00, 0x00, 64, 1, 0 };

/* Built per speed: only the bulk packet size differs. */
static uint8_t cfg_desc[80];
static int cfg_len;

static void build_config(int hs, int other) {
    uint8_t *p = cfg_desc;
    int bulk = hs ? 512 : 64;
    *p++ = 9; *p++ = other ? 7 : 2; *p++ = 0; *p++ = 0; *p++ = 2; *p++ = 1; *p++ = 0;
    *p++ = 0x80; *p++ = 250;                 /* bus-powered: the Pi runs off this port */
    /* interface 0: communications, ECM */
    *p++ = 9; *p++ = 4; *p++ = 0; *p++ = 0; *p++ = 1; *p++ = 0x02; *p++ = 0x06; *p++ = 0x00; *p++ = 4;
    *p++ = 5; *p++ = 0x24; *p++ = 0x00; *p++ = 0x10; *p++ = 0x01;            /* header 1.10    */
    *p++ = 5; *p++ = 0x24; *p++ = 0x06; *p++ = 0; *p++ = 1;                  /* union 0 -> 1   */
    *p++ = 13; *p++ = 0x24; *p++ = 0x0F; *p++ = 5;                           /* ethernet       */
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;                                  /* statistics     */
    *p++ = 0xEA; *p++ = 0x05;                                                /* 1514           */
    *p++ = 0; *p++ = 0; *p++ = 0;
    *p++ = 7; *p++ = 5; *p++ = 0x80 | EP_INT; *p++ = 3; *p++ = 16; *p++ = 0; *p++ = hs ? 8 : 32;
    /* interface 1 alt 0: data, idle */
    *p++ = 9; *p++ = 4; *p++ = 1; *p++ = 0; *p++ = 0; *p++ = 0x0A; *p++ = 0; *p++ = 0; *p++ = 0;
    /* interface 1 alt 1: data, two bulk endpoints */
    *p++ = 9; *p++ = 4; *p++ = 1; *p++ = 1; *p++ = 2; *p++ = 0x0A; *p++ = 0; *p++ = 0; *p++ = 6;
    *p++ = 7; *p++ = 5; *p++ = 0x80 | EP_IN; *p++ = 2; *p++ = (uint8_t)bulk; *p++ = (uint8_t)(bulk >> 8); *p++ = 0;
    *p++ = 7; *p++ = 5; *p++ = EP_OUT;       *p++ = 2; *p++ = (uint8_t)bulk; *p++ = (uint8_t)(bulk >> 8); *p++ = 0;
    cfg_len = (int)(p - cfg_desc);
    cfg_desc[2] = (uint8_t)cfg_len; cfg_desc[3] = (uint8_t)(cfg_len >> 8);
}

static char host_mac_str[13];
static int string_desc(int idx, uint8_t *out) {
    const char *s;
    switch (idx) {
    case 0: out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04; return 4;
    case 1: s = "BoltOS"; break;
    case 2: s = "BoltOS Mini"; break;
    case 3: s = "ORANGEPIZERO3"; break;
    case 4: s = "BoltOS Mini Control"; break;
    case 5: s = host_mac_str; break;
    case 6: s = "BoltOS Mini Data"; break;
    default: return -1;
    }
    int n = (int)strlen(s);
    out[0] = (uint8_t)(2 + 2 * n); out[1] = 3;
    for (int i = 0; i < n; i++) { out[2 + 2 * i] = (uint8_t)s[i]; out[3 + 2 * i] = 0; }
    return 2 + 2 * n;
}

/* ---- state ------------------------------------------------------------------------- */
enum { EP0_IDLE, EP0_IN, EP0_OUT };
static struct {
    int live, hs, configured, data_alt, addr_pending, addr;
    int ep0_state;
    uint8_t ep0_buf[256]; int ep0_len, ep0_off, ep0_zlp;
    int out_len;                                   /* OUT data stage remaining */
    uint64_t resets, configs;
} u;

static netif_t usb0;

/* Frames to the Mac: a ring the network stack fills and ep1 drains. */
#define TXQ 64
static struct { uint16_t len; uint8_t data[1536]; } txq[TXQ];
static uint32_t txq_head, txq_tail;
static int tx_off, tx_zlp;
/* The frame coming in from the Mac. */
static uint8_t rx_frame[2048];
static int rx_len, rx_overflow;
/* Notifications on ep3. */
static uint8_t note[32]; static int note_len, note_off, note_pending;

static void fifo_write(int ep, const uint8_t *p, int n) {
    uintptr_t f = M_FIFO(ep);
    int i = 0;
    for (; i + 4 <= n; i += 4) w32(f, (uint32_t)p[i] | (uint32_t)p[i + 1] << 8 |
                                      (uint32_t)p[i + 2] << 16 | (uint32_t)p[i + 3] << 24);
    for (; i < n; i++) w8(f, p[i]);
}

static void fifo_read(int ep, uint8_t *p, int n) {
    uintptr_t f = M_FIFO(ep);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t v = r32(f);
        p[i] = (uint8_t)v; p[i + 1] = (uint8_t)(v >> 8); p[i + 2] = (uint8_t)(v >> 16); p[i + 3] = (uint8_t)(v >> 24);
    }
    for (; i < n; i++) p[i] = r8(f);
}

static int bulk_size(void) { return u.hs ? 512 : 64; }

static void ep_setup(void) {
    int mp = bulk_size();
    w8(M_INDEX, EP_IN);
    w16(M_TXMAXP, (uint16_t)mp);
    w16(M_TXCSR, TX_MODE | TX_CLRTOG | TX_FLUSH);
    w16(M_TXCSR, TX_MODE | TX_CLRTOG | TX_FLUSH);
    w8(M_INDEX, EP_OUT);
    w16(M_RXMAXP, (uint16_t)mp);
    w16(M_RXCSR, RX_FLUSH | RX_CLRTOG);
    w16(M_RXCSR, RX_FLUSH | RX_CLRTOG);
    w8(M_INDEX, EP_INT);
    w16(M_TXMAXP, 16);
    w16(M_TXCSR, TX_MODE | TX_CLRTOG | TX_FLUSH);
    w8(M_INDEX, 0);
    txq_tail = txq_head; tx_off = 0; tx_zlp = 0;
    rx_len = 0; rx_overflow = 0;
}

static void link_state(int up) {
    if (usb0.up == up) return;
    usb0.up = up;
    kprintf("[usb-c] link %s%s\n", up ? "up: the Mac has us, " : "down",
            up ? (u.hs ? "480 Mbit/s" : "12 Mbit/s") : "");
    if (up) { void netsvc_link_up(void); netsvc_link_up(); }
    shell_request_frame();
}

/* ---- ep0 --------------------------------------------------------------------------- */
static void ep0_send_chunk(void) {
    int n = u.ep0_len - u.ep0_off;
    if (n > 64) n = 64;
    if (n < 0) n = 0;
    fifo_write(0, u.ep0_buf + u.ep0_off, n);
    u.ep0_off += n;
    /* The last packet ends the data stage -- unless it was a full one and
     * the host asked for more than we have, when a zero-length one must
     * follow to say "that was all". */
    int last = u.ep0_off >= u.ep0_len && !(n == 64 && u.ep0_zlp);
    if (u.ep0_off >= u.ep0_len && n == 64) u.ep0_zlp = 0;
    w16(M_CSR0, (uint16_t)(C0_TXPKTRDY | (last ? C0_DATAEND : 0)));
    u.ep0_state = last ? EP0_IDLE : EP0_IN;
}

static void ep0_reply(const void *data, int len, int wlength) {
    if (len > wlength) len = wlength;
    if (len > (int)sizeof u.ep0_buf) len = (int)sizeof u.ep0_buf;
    memcpy(u.ep0_buf, data, (size_t)len);
    u.ep0_len = len; u.ep0_off = 0;
    u.ep0_zlp = (len < wlength) && (len % 64 == 0);
    w16(M_CSR0, C0_SVDRXPKTRDY);
    for (int i = 0; i < 100000 && (r16(M_CSR0) & C0_RXPKTRDY); i++) { }
    ep0_send_chunk();
}

static void ep0_ack(void)   { w16(M_CSR0, C0_SVDRXPKTRDY | C0_DATAEND); u.ep0_state = EP0_IDLE; }
static void ep0_stall(void) { w16(M_CSR0, C0_SVDRXPKTRDY | C0_SENDSTALL); u.ep0_state = EP0_IDLE; }

static void queue_notifications(void) {
    uint8_t *n = note;
    /* NETWORK_CONNECTION: connected */
    n[0] = 0xA1; n[1] = 0x00; n[2] = 1; n[3] = 0; n[4] = 0; n[5] = 0; n[6] = 0; n[7] = 0;
    /* CONNECTION_SPEED_CHANGE: up and down, bits/s */
    uint32_t bps = u.hs ? 480000000u : 12000000u;
    n[8] = 0xA1; n[9] = 0x2A; n[10] = 0; n[11] = 0; n[12] = 0; n[13] = 0; n[14] = 8; n[15] = 0;
    for (int i = 0; i < 4; i++) { n[16 + i] = (uint8_t)(bps >> (8 * i)); n[20 + i] = (uint8_t)(bps >> (8 * i)); }
    note_len = 24; note_off = 0; note_pending = 1;
}

static void handle_setup(const uint8_t *s) {
    uint8_t type = s[0], req = s[1];
    uint16_t value = (uint16_t)(s[2] | s[3] << 8), index = (uint16_t)(s[4] | s[5] << 8);
    uint16_t length = (uint16_t)(s[6] | s[7] << 8);
    uint8_t buf[256];

    if ((type & 0x60) == 0x00) {                           /* standard        */
        switch (req) {
        case 6: {                                          /* GET_DESCRIPTOR  */
            int dt = value >> 8, di = value & 0xFF, n;
            u.hs = (r8(M_POWER) & P_HSMODE) != 0;          /* settled by now  */
            if (dt == 1) { ep0_reply(dev_desc, 18, length); return; }
            if (dt == 2) { build_config(u.hs, 0); ep0_reply(cfg_desc, cfg_len, length); return; }
            if (dt == 7) { build_config(!u.hs, 1); ep0_reply(cfg_desc, cfg_len, length); return; }
            if (dt == 6) { ep0_reply(qual_desc, 10, length); return; }
            if (dt == 3 && (n = string_desc(di, buf)) > 0) { ep0_reply(buf, n, length); return; }
            ep0_stall();
            return;
        }
        case 5:                                            /* SET_ADDRESS     */
            u.addr = value & 0x7F; u.addr_pending = 1;
            ep0_ack();
            return;
        case 9:                                            /* SET_CONFIGURATION */
            u.hs = (r8(M_POWER) & P_HSMODE) != 0;
            u.configured = value == 1;
            u.data_alt = 0;
            if (u.configured) { ep_setup(); u.configs++; kprintf("[usb-c] configured by the host\n"); }
            else link_state(0);
            ep0_ack();
            return;
        case 8: buf[0] = (uint8_t)u.configured; ep0_reply(buf, 1, length); return;
        case 0: buf[0] = buf[1] = 0; ep0_reply(buf, 2, length); return;
        case 11:                                           /* SET_INTERFACE   */
            if (index == 1) {
                u.data_alt = value;
                ep_setup();
                if (value == 1) { queue_notifications(); link_state(1); }
                else link_state(0);
            }
            ep0_ack();
            return;
        case 10: buf[0] = (uint8_t)(index == 1 ? u.data_alt : 0); ep0_reply(buf, 1, length); return;
        case 1: case 3:                                    /* CLEAR/SET_FEATURE */
            if ((type & 0x1F) == 2 && req == 1) {           /* endpoint halt off */
                int ep = index & 0x0F;
                w8(M_INDEX, (uint8_t)ep);
                if (index & 0x80) w16(M_TXCSR, TX_MODE | TX_CLRTOG);
                else w16(M_RXCSR, RX_CLRTOG);
                w8(M_INDEX, 0);
            }
            ep0_ack();
            return;
        }
        ep0_stall();
        return;
    }
    if ((type & 0x60) == 0x20) {                           /* class (CDC)     */
        if (!(type & 0x80) && length > 0) {                /* OUT data: take it, ignore it */
            u.out_len = length;
            u.ep0_state = EP0_OUT;
            w16(M_CSR0, C0_SVDRXPKTRDY);
            return;
        }
        if (type & 0x80) { memset(buf, 0, sizeof buf); ep0_reply(buf, length > 64 ? 64 : length, length); return; }
        ep0_ack();                                         /* SET_ETHERNET_PACKET_FILTER & co */
        return;
    }
    ep0_stall();
}

static void ep0_irq(void) {
    w8(M_INDEX, 0);
    uint16_t csr = r16(M_CSR0);
    if (csr & C0_SENTSTALL) { w16(M_CSR0, (uint16_t)(csr & ~C0_SENTSTALL)); u.ep0_state = EP0_IDLE; csr = r16(M_CSR0); }
    if (csr & C0_SETUPEND)  { w16(M_CSR0, C0_SVDSETUPEND); u.ep0_state = EP0_IDLE; csr = r16(M_CSR0); }
    if (u.addr_pending) { w8(M_FADDR, (uint8_t)u.addr); u.addr_pending = 0; }

    if (u.ep0_state == EP0_IN) {
        if (!(csr & C0_TXPKTRDY)) ep0_send_chunk();
        return;
    }
    if (u.ep0_state == EP0_OUT) {
        if (csr & C0_RXPKTRDY) {
            int n = r16(M_COUNT0) & 0x7F;
            uint8_t junk[64];
            fifo_read(0, junk, n);
            u.out_len -= n;
            if (u.out_len <= 0) ep0_ack();
            else w16(M_CSR0, C0_SVDRXPKTRDY);
        }
        return;
    }
    if (csr & C0_RXPKTRDY) {
        int n = r16(M_COUNT0) & 0x7F;
        uint8_t setup[64];
        fifo_read(0, setup, n);
        if (n == 8) handle_setup(setup);
        else ep0_stall();
    }
}

/* ---- bulk ------------------------------------------------------------------------------ */
static int usb0_tx(netif_t *nif, const void *frame, int len) {
    (void)nif;
    if (!u.configured || u.data_alt != 1) return -1;
    if (txq_head - txq_tail >= TXQ || len > 1536) return -1;
    txq[txq_head % TXQ].len = (uint16_t)len;
    memcpy(txq[txq_head % TXQ].data, frame, (size_t)len);
    txq_head++;
    return 0;
}

static void bulk_in_pump(void) {
    int mp = bulk_size();
    w8(M_INDEX, EP_IN);
    for (int guard = 0; guard < 16; guard++) {
        uint16_t csr = r16(M_TXCSR);
        if (csr & TX_PKTRDY) break;                        /* both buffers full */
        if (tx_zlp) {
            w16(M_TXCSR, (uint16_t)(csr | TX_PKTRDY));
            tx_zlp = 0;
            txq_tail++; tx_off = 0;
            continue;
        }
        if (txq_tail == txq_head) break;
        int len = txq[txq_tail % TXQ].len;
        int n = len - tx_off;
        if (n > mp) n = mp;
        fifo_write(EP_IN, txq[txq_tail % TXQ].data + tx_off, n);
        tx_off += n;
        w16(M_TXCSR, (uint16_t)(csr | TX_PKTRDY));
        if (tx_off >= len) {
            if (n == mp) tx_zlp = 1;                       /* exact multiple: ZLP next */
            else { txq_tail++; tx_off = 0; }
        }
    }
    w8(M_INDEX, 0);
}

static void bulk_out_pump(void) {
    int mp = bulk_size();
    w8(M_INDEX, EP_OUT);
    for (int guard = 0; guard < 16; guard++) {
        uint16_t csr = r16(M_RXCSR);
        if (!(csr & RX_PKTRDY)) break;
        int n = r16(M_RXCOUNT) & 0x1FFF;
        if (rx_len + n <= (int)sizeof rx_frame) fifo_read(EP_OUT, rx_frame + rx_len, n);
        else { uint8_t junk[512]; fifo_read(EP_OUT, junk, n > 512 ? 512 : n); rx_overflow = 1; }
        rx_len += n;
        w16(M_RXCSR, (uint16_t)(csr & ~RX_PKTRDY));
        if (n < mp) {                                      /* short: end of frame */
            if (!rx_overflow && rx_len > 0) {
                w8(M_INDEX, 0);
                net_rx(&usb0, rx_frame, rx_len);
                w8(M_INDEX, EP_OUT);
            }
            rx_len = 0; rx_overflow = 0;
        }
    }
    w8(M_INDEX, 0);
}

static void notify_pump(void) {
    if (!note_pending) return;
    w8(M_INDEX, EP_INT);
    uint16_t csr = r16(M_TXCSR);
    if (!(csr & TX_PKTRDY)) {
        int n = note_off == 0 ? 8 : 16;
        fifo_write(EP_INT, note + note_off, n);
        w16(M_TXCSR, (uint16_t)(csr | TX_PKTRDY));
        note_off += n;
        if (note_off >= note_len) note_pending = 0;
    }
    w8(M_INDEX, 0);
}

/* ---- bring-up --------------------------------------------------------------------------- */
static void fifo_cfg(int ep, int tx, int size_log2, int dbl, int addr) {
    w8(M_INDEX, (uint8_t)ep);
    uint8_t sz = (uint8_t)((size_log2 - 3) | (dbl ? 0x10 : 0));
    if (tx) { w8(M_TXFIFOSZ, sz); w16(M_TXFIFOADD, (uint16_t)(addr >> 3)); }
    else    { w8(M_RXFIFOSZ, sz); w16(M_RXFIFOADD, (uint16_t)(addr >> 3)); }
}

static void make_macs(void) {
    /* From the H618's unique chip ID, so two Pis on one Mac would differ. */
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < 4; i++) { h ^= r32(0x03006200u + 4u * (uint32_t)i); h *= 16777619u; }
    uint8_t m[6] = { 0x02, 0xB0, (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)(h & 0xFE) };
    memcpy(usb0.mac, m, 6);                                /* the Pi's end     */
    m[5] |= 1;                                             /* the Mac's end    */
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) { host_mac_str[2 * i] = hx[m[i] >> 4]; host_mac_str[2 * i + 1] = hx[m[i] & 15]; }
    host_mac_str[12] = 0;
}

int musb_init(void) {
    if (g_plat != PLAT_H618) return -1;

    /* Clocks and resets: OTG and PHY0, plus PHY2 and its bus clock, which
     * the H616/H618's PHY0 needs (U-Boot: needs_phy2_siddq). Reset the OTG
     * block so whatever U-Boot's DFU left behind is gone, and the Mac sees
     * a clean unplug-and-replug. */
    clr32(CCU + 0xA8C, 1u << 24);                          /* OTG reset on     */
    delay_us(100);
    set32(CCU + 0xA70, (1u << 29) | (1u << 30));           /* PHY0 gate, reset off */
    set32(CCU + 0xA78, (1u << 29) | (1u << 30));           /* PHY2 gate, reset off */
    set32(CCU + 0xA8C, (1u << 6));                         /* EHCI2 bus gate    */
    set32(CCU + 0xA8C, (1u << 8) | (1u << 24));            /* OTG gate, reset off */
    delay_us(100);
    clr32(PMU2 + 0x10, 1u << 3);                           /* PHY2: SIDDQ off   */
    uint32_t v = r32(PHY_PHYCTL);
    v |= 1u << 5;                                          /* VBUSVLDEXT        */
    v &= ~(1u << 3);                                       /* SIDDQ off         */
    w32(PHY_PHYCTL, v);
    set32(PHY_OTGCTL, 1);                                  /* PHY0 -> MUSB      */

    /* Allwinner glue: FIFO RAM, pull-ups, ID and VBUS forced to "device". */
    w32(SRAMC + 0x04, (r32(SRAMC + 0x04) & ~3u) | 1u);
    v = r32(M_ISCR);
    v &= ~((1u << 6) | (1u << 5) | (1u << 4));             /* don't clear status */
    v |= (1u << 16) | (1u << 17);                          /* DP/DM, ID pull-ups */
    v = (v & ~(3u << 14)) | (3u << 14);                    /* ID forced high    */
    v = (v & ~(3u << 12)) | (3u << 12);                    /* VBUS forced valid */
    w32(M_ISCR, v);
    w8(M_VEND0, 0);                                        /* PIO mode          */

    /* The core: FIFOs, interrupt sources, high speed, then the pull-up. */
    w8(M_INDEX, 0);
    w8(M_FADDR, 0);
    fifo_cfg(EP_IN, 1, 9, 1, 64);                          /* 2 x 512 at 64     */
    fifo_cfg(EP_OUT, 0, 9, 1, 64 + 1024);                  /* 2 x 512 at 1088   */
    fifo_cfg(EP_INT, 1, 6, 0, 64 + 2048);                  /* 64 at 2112        */
    w8(M_INDEX, 0);
    w16(M_INTRTXE, 0x000F);
    w16(M_INTRRXE, 0x000E);
    w8(M_INTRUSBE, 0xF7);
    w8(M_TESTMODE, 0);
    (void)r8(M_INTRUSB); w8(M_INTRUSB, 0xFF);
    w16(M_INTRTX, 0xFFFF); w16(M_INTRRX, 0xFFFF);
    w8(M_DEVCTL, (uint8_t)(r8(M_DEVCTL) & ~1u));
    make_macs();
    usb0.name = "usb0";
    usb0.tx = usb0_tx;
    usb0.up = 0;
    net_register(&usb0);
    w8(M_POWER, P_HSENAB | P_SOFTCONN);
    u.live = 1;
    kprintf("[usb-c] MUSB up as a CDC Ethernet device, waiting for the Mac\n");
    return 0;
}

int musb_configured(void) { return u.configured; }

/* Frames waiting to go to the Mac: the main loop does not sleep while there
 * are, or the 1 kHz tick would cap the link at 16 packets a millisecond. */
int musb_busy(void) { return u.live && txq_head != txq_tail; }

void musb_poll(void) {
    if (!u.live) return;
    uint8_t usb = r8(M_INTRUSB);
    if (usb) w8(M_INTRUSB, usb);
    uint16_t tx = r16(M_INTRTX);
    if (tx) w16(M_INTRTX, tx);
    uint16_t rx = r16(M_INTRRX);
    if (rx) w16(M_INTRRX, rx);
    (void)rx;

    if (usb & I_RESET) {
        u.hs = (r8(M_POWER) & P_HSMODE) != 0;
        w8(M_FADDR, 0);
        u.configured = 0; u.data_alt = 0; u.addr_pending = 0;
        u.ep0_state = EP0_IDLE;
        u.resets++;
        link_state(0);
        if (u.resets <= 3 || u.resets % 50 == 0)
            kprintf("[usb-c] bus reset from the host (%s speed)\n", u.hs ? "high" : "full");
    }
    if (usb & I_DISCON) { u.configured = 0; u.data_alt = 0; link_state(0); }
    if (tx & 1) ep0_irq();
    if (u.configured && u.data_alt == 1) {
        bulk_out_pump();
        bulk_in_pump();
        notify_pump();
    }
}
