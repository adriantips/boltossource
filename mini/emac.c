/* ===========================================================================
 *  BoltOS Mini  -  mini/emac.c
 *
 *  The Zero3's Ethernet socket: the H618's EMAC0 (a sun8i "EMAC", the same
 *  block as the H6's and the A64's) wired over RGMII to a Motorcomm YT8531C
 *  gigabit PHY at MDIO address 1. This is how the Pi reaches the internet on
 *  its own, without the Mac.
 *
 *  The EMAC moves frames through two rings of descriptors in the uncached
 *  DMA pool, each with a 2 KiB buffer: receive polls the ring's own bits,
 *  transmit copies a frame into the next free slot and kicks the DMA. The
 *  PHY is polled twice a second; when its link comes up the EMAC is set to
 *  the speed it negotiated, and DHCP (netsvc.c) asks the router for an
 *  address.
 *
 *  The clock delays are the device tree's: "rgmii-rxid", so the PHY delays
 *  receive (1.95 ns) and the EMAC delays transmit (0.7 ns, the syscon's
 *  ETXDC = 7). The PHY drives the 125 MHz clock out.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

#define EMAC        0x05020000u
#define SYSCON_EMAC 0x03000030u
#define CCU_EMAC    0x0300197Cu             /* EMAC_BGR: bit 0 gate, bit 16 reset */
#define PIO_I       (0x0300B000u + 8 * 0x24u)

#define E_CTL0      0x00                    /* duplex bit 0, speed bits 3:2     */
#define E_CTL1      0x04                    /* soft reset bit 0, burst 31:24     */
#define E_INT_STA   0x08
#define E_TX_CTL0   0x10
#define E_TX_CTL1   0x14
#define E_TX_DESC   0x20
#define E_RX_CTL0   0x24
#define E_RX_CTL1   0x28
#define E_RX_DESC   0x34
#define E_RX_FLT    0x38
#define E_MII_CMD   0x48
#define E_MII_DATA  0x4C
#define E_ADDR_HI   0x50
#define E_ADDR_LO   0x54
#define E_TX_STA    0xB0
#define E_RX_STA    0xC0

#define D_OWN       (1u << 31)
#define D_LAST      (1u << 30)
#define D_FIRST     (1u << 29)
#define D_CHAIN     (1u << 24)
#define RX_ERR      0x400068DBu

#define NRX   64
#define NTX   64
#define BUFSZ 2048

typedef struct { volatile uint32_t status, ctl, buf, next; } desc_t;

static netif_t eth0;
static desc_t *rx, *tx;
static uint8_t *rxb, *txb;
static int rx_i, tx_i, phy = -1, present, link, speed, duplex;
static uint64_t next_phy_poll;
static uint32_t phy_id, rx_errs;

static inline uint32_t er(int r) { return r32(EMAC + (uint32_t)r); }
static inline void ew(int r, uint32_t v) { w32(EMAC + (uint32_t)r, v); }

/* ---- MDIO: the EMAC runs at 200-300 MHz, so divide by 128 to stay under 2.5 MHz */
static int mdio_wait(void) {
    for (int i = 0; i < 10000; i++) { if (!(er(E_MII_CMD) & 1)) return 0; delay_us(1); }
    return -1;
}
static int mdio_rd(int addr, int reg) {
    ew(E_MII_CMD, ((uint32_t)reg << 4) | ((uint32_t)addr << 12) | (3u << 20) | 1u);
    if (mdio_wait()) return -1;
    return (int)(er(E_MII_DATA) & 0xFFFF);
}
static int mdio_wr(int addr, int reg, uint16_t v) {
    ew(E_MII_DATA, v);
    ew(E_MII_CMD, ((uint32_t)reg << 4) | ((uint32_t)addr << 12) | (3u << 20) | 2u | 1u);
    return mdio_wait();
}
/* The YT8531's extended registers sit behind a page select (0x1E) and data (0x1F). */
static int ext_rd(int reg) { mdio_wr(phy, 0x1E, (uint16_t)reg); return mdio_rd(phy, 0x1F); }
static void ext_mod(int reg, uint16_t mask, uint16_t set) {
    int v = ext_rd(reg);
    if (v < 0) return;
    mdio_wr(phy, 0x1F, (uint16_t)((v & ~mask) | set));
}

static void phy_setup(void) {
    mdio_wr(phy, 0, 0x8000);                                     /* BMCR: reset    */
    for (int i = 0; i < 100 && (mdio_rd(phy, 0) & 0x8000); i++) delay_ms(1);
    ext_mod(0xA012, 0x005E, 0x0050);         /* clock out: on, 125 MHz, from the PLL */
    ext_mod(0xA001, 0x0100, 0);              /* no fixed 1.9 ns RXC delay ...        */
    ext_mod(0xA003, 0x3C0F, 13u << 10);      /* ... RX 1.95 ns from RGMII_CONFIG1, TX 0 */
    ext_mod(0xA010, 0xE030, (3u << 13) | (3u << 4));            /* RX drive strength */
    ext_mod(0x000C, 0x1000, 0);              /* keep RXC running with no cable, so the
                                              * EMAC's own reset always completes   */
    mdio_wr(phy, 4, 0x01E1);                 /* advertise 10/100, half and full      */
    mdio_wr(phy, 9, 0x0300);                 /* and 1000 full/half                   */
    mdio_wr(phy, 0, 0x1200);                 /* autonegotiate, restart               */
}

static void rings_init(void) {
    for (int i = 0; i < NRX; i++) {
        rx[i].buf = dma_addr(rxb + i * BUFSZ);
        rx[i].next = dma_addr(&rx[(i + 1) % NRX]);
        rx[i].ctl = BUFSZ - 4;
        rx[i].status = D_OWN;
    }
    for (int i = 0; i < NTX; i++) {
        tx[i].buf = dma_addr(txb + i * BUFSZ);
        tx[i].next = dma_addr(&tx[(i + 1) % NTX]);
        tx[i].ctl = 0;
        tx[i].status = 0;
    }
    rx_i = tx_i = 0;
    dsb();
    ew(E_RX_DESC, dma_addr(&rx[0]));
    ew(E_TX_DESC, dma_addr(&tx[0]));
}

static void mac_start(void) {
    ew(E_CTL1, 1);                                              /* soft reset     */
    for (int i = 0; i < 1000 && (er(E_CTL1) & 1); i++) delay_us(10);
    if (er(E_CTL1) & 1) kprintf("[eth] the EMAC did not come out of reset (no clock from the PHY?)\n");
    const uint8_t *m = eth0.mac;
    ew(E_ADDR_HI, (uint32_t)m[4] | (uint32_t)m[5] << 8);
    ew(E_ADDR_LO, (uint32_t)m[0] | (uint32_t)m[1] << 8 | (uint32_t)m[2] << 16 | (uint32_t)m[3] << 24);
    ew(E_CTL1, 8u << 24);                                        /* DMA burst 8    */
    ew(E_TX_CTL1, 1u << 1);                  /* send once a whole frame is in the FIFO */
    ew(E_RX_CTL1, 1u << 1);                  /* and hand one up once it has all arrived */
    ew(E_RX_FLT, 0);                         /* ours and broadcast                   */
    rings_init();
    set32(EMAC + E_RX_CTL1, 1u << 30);                           /* RX DMA on      */
    set32(EMAC + E_TX_CTL1, 1u << 30);                           /* TX DMA on      */
    set32(EMAC + E_RX_CTL0, 1u << 31);                           /* receiver on    */
    set32(EMAC + E_TX_CTL0, 1u << 31);                           /* transmitter on */
}

static void set_speed(void) {
    uint32_t v = er(E_CTL0) & ~0xDu;
    if (duplex) v |= 1;
    v |= speed == 1000 ? 0 : speed == 100 ? (3u << 2) : (2u << 2);
    ew(E_CTL0, v);
}

static int eth_tx(netif_t *nif, const void *frame, int len) {
    (void)nif;
    if (!link || len > BUFSZ) return -1;
    desc_t *d = &tx[tx_i];
    if (d->status & D_OWN) return -1;                            /* ring full      */
    memcpy(txb + tx_i * BUFSZ, frame, (size_t)len);
    d->ctl = (uint32_t)len | D_FIRST | D_LAST | D_CHAIN;
    dsb();
    d->status = D_OWN;
    dsb();
    tx_i = (tx_i + 1) % NTX;
    set32(EMAC + E_TX_CTL1, 1u << 31);                           /* start          */
    return 0;
}

int eth_init(void) {
    if (g_plat != PLAT_H618) return -1;
    /* Pins: PI0-PI5 and PI7-PI16 are the RGMII and MDIO lines (function 2), at the
     * strongest drive. PI6 belongs to I2C0. */
    for (int p = 0; p <= 16; p++) {
        if (p == 6) continue;
        uintptr_t cfg = PIO_I + (uint32_t)(p / 8) * 4;
        int sh = (p % 8) * 4;
        w32(cfg, (r32(cfg) & ~(0xFu << sh)) | (2u << sh));
        uintptr_t drv = PIO_I + 0x14 + (uint32_t)(p / 16) * 4;
        int dsh = (p % 16) * 2;
        w32(drv, (r32(drv) & ~(3u << dsh)) | (3u << dsh));
    }
    set32(CCU_EMAC, 1u << 0);                                    /* bus clock      */
    set32(CCU_EMAC, 1u << 16);                                   /* out of reset   */
    delay_us(100);
    /* RGMII, the 125 MHz TX clock made inside, TX delayed 0.7 ns; no internal PHY. */
    w32(SYSCON_EMAC, (1u << 16) | (7u << 10) | (1u << 2) | 2u);

    for (int a = 1; a < 32 + 1 && phy < 0; a++) {                /* address 1 first */
        int addr = a & 31;
        int id1 = mdio_rd(addr, 2), id2 = mdio_rd(addr, 3);
        if (id1 > 0 && id1 != 0xFFFF && id2 >= 0) { phy = addr; phy_id = (uint32_t)id1 << 16 | (uint32_t)id2; }
    }
    if (phy < 0) { kprintf("[eth] no PHY answers on MDIO\n"); return -1; }
    present = 1;

    /* The MAC: from the chip ID, like the USB-C link's, with a different second byte. */
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < 4; i++) { h ^= r32(0x03006200u + 4u * (uint32_t)i); h *= 16777619u; }
    uint8_t m[6] = { 0x02, 0xE0, (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h };
    memcpy(eth0.mac, m, 6);
    eth0.name = "eth0";
    eth0.tx = eth_tx;

    rx = dma_alloc(NRX * sizeof(desc_t), 64);
    tx = dma_alloc(NTX * sizeof(desc_t), 64);
    rxb = dma_alloc(NRX * BUFSZ, 64);
    txb = dma_alloc(NTX * BUFSZ, 64);

    phy_setup();
    mac_start();
    net_register(&eth0);
    kprintf("[eth] EMAC0, PHY %08x at MDIO %d (%s)\n", phy_id, phy,
            (phy_id & 0xFFFFFFF0u) == 0x4F51E910u ? "Motorcomm YT8531" : "unknown PHY");
    next_phy_poll = now_ms();
    return 0;
}

static void phy_poll(void) {
    if (now_ms() < next_phy_poll) return;
    next_phy_poll = now_ms() + 500;
    mdio_rd(phy, 1);                                             /* latched-low link bit */
    int bmsr = mdio_rd(phy, 1);
    int up = bmsr >= 0 && (bmsr & 4);
    if (up && !link) {
        int st = mdio_rd(phy, 0x11);                             /* specific status */
        if (st < 0 || !(st & (1 << 11))) return;                 /* not resolved yet */
        int sm = (st >> 14) & 3;
        speed = sm == 2 ? 1000 : sm == 1 ? 100 : 10;
        duplex = (st >> 13) & 1;
        set_speed();
        link = 1;
        eth0.up = 1;
        kprintf("[eth] link up: %d Mbit/s, %s duplex\n", speed, duplex ? "full" : "half");
        shell_toast("Ethernet connected", speed == 1000 ? "1 Gbit/s" : speed == 100 ? "100 Mbit/s" : "10 Mbit/s");
    } else if (!up && link) {
        link = 0;
        eth0.up = 0;
        eth0.ip = 0; eth0.gw = 0; eth0.mask = 0;
        kprintf("[eth] link down\n");
        shell_toast("Ethernet disconnected", "the cable is out");
    }
}

void eth_poll(void) {
    if (!present) return;
    phy_poll();
    int n = 0;
    while (n < NRX) {
        desc_t *d = &rx[rx_i];
        uint32_t st = d->status;
        if (st & D_OWN) break;
        int len = (int)((st >> 16) & 0x3FFF);
        if (st & RX_ERR) rx_errs++;
        else if (len >= 18 && len <= BUFSZ - 4) net_rx(&eth0, rxb + rx_i * BUFSZ, len - 4);   /* minus the FCS */
        d->status = D_OWN;
        rx_i = (rx_i + 1) % NRX;
        n++;
    }
    if (n) { dsb(); set32(EMAC + E_RX_CTL1, 1u << 31); }        /* resume a starved DMA */
}

int eth_link(void) { return link ? speed : 0; }
netif_t *eth_netif(void) { return present ? &eth0 : 0; }

int eth_debug(char *out, int cap) {
    if (!present) return snprintf(out, (size_t)cap, "eth: no PHY\n");
    int n = snprintf(out, (size_t)cap, "eth: phy %08x @%d bmcr %04x bmsr %04x anar %04x anlpar %04x gctl %04x gsts %04x spec %04x\n",
                     phy_id, phy, mdio_rd(phy, 0), mdio_rd(phy, 1), mdio_rd(phy, 4), mdio_rd(phy, 5),
                     mdio_rd(phy, 9), mdio_rd(phy, 10), mdio_rd(phy, 0x11));
    n += snprintf(out + n, (size_t)(cap - n), "     ext a001 %04x a003 %04x a012 %04x  syscon %08x ctl0 %08x ctl1 %08x int %08x\n",
                  ext_rd(0xA001), ext_rd(0xA003), ext_rd(0xA012), r32(SYSCON_EMAC), er(E_CTL0), er(E_CTL1), er(E_INT_STA));
    n += snprintf(out + n, (size_t)(cap - n), "     tx_sta %08x rx_sta %08x rx_i %d tx_i %d rx errs %u  rx %lu tx %lu drops %lu\n",
                  er(E_TX_STA), er(E_RX_STA), rx_i, tx_i, rx_errs, (unsigned long)eth0.rx_pkts,
                  (unsigned long)eth0.tx_pkts, (unsigned long)eth0.tx_drops);
    return n;
}
