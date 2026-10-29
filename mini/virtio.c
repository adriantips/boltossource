/* ===========================================================================
 *  BoltOS Mini  -  mini/virtio.c
 *
 *  QEMU's virt board, standing in for the Pi on the Mac: a virtio network
 *  card (where the Pi has its USB-C link), and virtio keyboard and tablet
 *  devices (where the Pi has its USB keyboard). All three sit on virtio-mmio
 *  transports at 0x0A000000 + n * 0x200; both the legacy (version 1) and the
 *  modern (version 2) register layouts are handled, since which one QEMU
 *  offers depends on its settings.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kprintf.h"
#include "string.h"

#define VIO_BASE   0x0A000000u
#define VIO_SLOTS  32
#define R_MAGIC    0x000
#define R_VERSION  0x004
#define R_DEVID    0x008
#define R_HFEAT    0x010
#define R_HFEATSEL 0x014
#define R_GFEAT    0x020
#define R_GFEATSEL 0x024
#define R_PAGESIZE 0x028
#define R_QSEL     0x030
#define R_QMAX     0x034
#define R_QNUM     0x038
#define R_QALIGN   0x03C
#define R_QPFN     0x040
#define R_QREADY   0x044
#define R_NOTIFY   0x050
#define R_ISR      0x060
#define R_IACK     0x064
#define R_STATUS   0x070
#define R_QDESC    0x080
#define R_QAVAIL   0x090
#define R_QUSED    0x0A0
#define R_CONFIG   0x100

#define QSZ 64
struct vdesc { uint64_t addr; uint32_t len; uint16_t flags, next; };
struct vq {
    uintptr_t base; int idx;
    struct vdesc *desc;
    volatile uint16_t *avail;        /* flags, idx, ring[QSZ]                  */
    volatile uint8_t  *used;         /* flags, idx, {id, len}[QSZ]             */
    uint16_t last_used, avail_idx;
    uint8_t *bufs; uint32_t bufsz;
};

static int vq_setup(struct vq *q, uintptr_t base, int idx, int version, uint32_t bufsz) {
    q->base = base; q->idx = idx; q->bufsz = bufsz;
    w32(base + R_QSEL, (uint32_t)idx);
    uint32_t max = r32(base + R_QMAX);
    if (max < QSZ) return -1;
    /* desc 16*QSZ, avail 6+2*QSZ, padded to 4 KiB, then used 6+8*QSZ */
    uint8_t *mem = dma_alloc(8192, 4096);
    q->desc  = (struct vdesc *)mem;
    q->avail = (volatile uint16_t *)(mem + 16 * QSZ);
    q->used  = mem + 4096;
    q->bufs  = dma_alloc(QSZ * bufsz, 64);
    w32(base + R_QNUM, QSZ);
    if (version == 1) {
        w32(base + R_QALIGN, 4096);
        w32(base + R_QPFN, (uint32_t)((uintptr_t)mem >> 12));
    } else {
        w32(base + R_QDESC, (uint32_t)(uintptr_t)q->desc);  w32(base + R_QDESC + 4, 0);
        w32(base + R_QAVAIL, (uint32_t)(uintptr_t)q->avail); w32(base + R_QAVAIL + 4, 0);
        w32(base + R_QUSED, (uint32_t)(uintptr_t)q->used);   w32(base + R_QUSED + 4, 0);
        w32(base + R_QREADY, 1);
    }
    for (int i = 0; i < QSZ; i++) {
        q->desc[i].addr = (uint64_t)(uintptr_t)(q->bufs + (size_t)i * bufsz);
        q->desc[i].len = bufsz;
    }
    return 0;
}

static void vq_give(struct vq *q, int d, uint32_t len, int device_writes) {
    q->desc[d].len = len;
    q->desc[d].flags = device_writes ? 2 : 0;
    q->avail[2 + q->avail_idx % QSZ] = (uint16_t)d;
    dmb();
    q->avail_idx++;
    q->avail[1] = q->avail_idx;
    dmb();
}

static void vq_kick(struct vq *q) { dsb(); w32(q->base + R_NOTIFY, (uint32_t)q->idx); }

static int vq_take(struct vq *q, uint32_t *len) {
    uint16_t used_idx = *(volatile uint16_t *)(q->used + 2);
    if (used_idx == q->last_used) return -1;
    dmb();
    volatile uint8_t *e = q->used + 4 + 8 * (q->last_used % QSZ);
    int id = (int)*(volatile uint32_t *)e;
    *len = *(volatile uint32_t *)(e + 4);
    q->last_used++;
    return id;
}

static int dev_start(uintptr_t base, uint32_t want_lo, uint32_t *version) {
    *version = r32(base + R_VERSION);
    w32(base + R_STATUS, 0);
    w32(base + R_STATUS, 1 | 2);                       /* ACK | DRIVER          */
    w32(base + R_HFEATSEL, 0);
    uint32_t have = r32(base + R_HFEAT);
    w32(base + R_GFEATSEL, 0);
    w32(base + R_GFEAT, have & want_lo);
    if (*version == 1) {
        w32(base + R_PAGESIZE, 4096);
    } else {
        w32(base + R_GFEATSEL, 1);
        w32(base + R_GFEAT, 1);                        /* VIRTIO_F_VERSION_1    */
        w32(base + R_STATUS, 1 | 2 | 8);               /* FEATURES_OK           */
        if (!(r32(base + R_STATUS) & 8)) return -1;
    }
    return 0;
}

/* ---- the network card ---------------------------------------------------------- */
static struct { uintptr_t base; struct vq rx, tx; int hdr; int tx_next; netif_t nif; int live; } vnet;

static int vnet_tx(netif_t *nif, const void *frame, int len) {
    (void)nif;
    /* Reclaim what the device has finished with, then use the next slot. */
    uint32_t l;
    while (vq_take(&vnet.tx, &l) >= 0) { }
    if ((uint16_t)(vnet.tx.avail_idx - vnet.tx.last_used) >= QSZ) return -1;
    int d = vnet.tx_next; vnet.tx_next = (vnet.tx_next + 1) % QSZ;
    uint8_t *b = vnet.tx.bufs + (size_t)d * vnet.tx.bufsz;
    memset(b, 0, (size_t)vnet.hdr);
    memcpy(b + vnet.hdr, frame, (size_t)len);
    vq_give(&vnet.tx, d, (uint32_t)(vnet.hdr + len), 0);
    vq_kick(&vnet.tx);
    return 0;
}

static void vnet_init(uintptr_t base) {
    uint32_t ver;
    if (dev_start(base, 1u << 5, &ver) != 0) return;   /* VIRTIO_NET_F_MAC      */
    vnet.base = base;
    vnet.hdr = (ver == 1) ? 10 : 12;
    if (vq_setup(&vnet.rx, base, 0, (int)ver, 2048) || vq_setup(&vnet.tx, base, 1, (int)ver, 2048)) return;
    for (int i = 0; i < QSZ; i++) vq_give(&vnet.rx, i, 2048, 1);
    w32(base + R_STATUS, r32(base + R_STATUS) | 4);    /* DRIVER_OK             */
    vq_kick(&vnet.rx);
    for (int i = 0; i < 6; i++) vnet.nif.mac[i] = r8(base + R_CONFIG + (uintptr_t)i);
    vnet.nif.name = "eth0";
    vnet.nif.tx = vnet_tx;
    vnet.nif.up = 1;
    vnet.live = 1;
    net_register(&vnet.nif);
}

/* ---- keyboard and tablet ---------------------------------------------------------
 *  Both deliver Linux input events. Key codes go through the HID table so
 *  every keyboard Mini has ends up in the same place.                       */
static const uint8_t evdev_to_hid[128] = {
    [1] = 0x29, [2] = 0x1E, [3] = 0x1F, [4] = 0x20, [5] = 0x21, [6] = 0x22, [7] = 0x23,
    [8] = 0x24, [9] = 0x25, [10] = 0x26, [11] = 0x27, [12] = 0x2D, [13] = 0x2E, [14] = 0x2A,
    [15] = 0x2B, [16] = 0x14, [17] = 0x1A, [18] = 0x08, [19] = 0x15, [20] = 0x17, [21] = 0x1C,
    [22] = 0x18, [23] = 0x0C, [24] = 0x12, [25] = 0x13, [26] = 0x2F, [27] = 0x30, [28] = 0x28,
    [30] = 0x04, [31] = 0x16, [32] = 0x07, [33] = 0x09, [34] = 0x0A, [35] = 0x0B, [36] = 0x0D,
    [37] = 0x0E, [38] = 0x0F, [39] = 0x33, [40] = 0x34, [41] = 0x35, [43] = 0x31, [44] = 0x1D,
    [45] = 0x1B, [46] = 0x06, [47] = 0x19, [48] = 0x05, [49] = 0x11, [50] = 0x10, [51] = 0x36,
    [52] = 0x37, [53] = 0x38, [55] = 0x55, [57] = 0x2C, [59] = 0x3A, [60] = 0x3B, [61] = 0x3C,
    [62] = 0x3D, [63] = 0x3E, [64] = 0x3F, [65] = 0x40, [66] = 0x41, [67] = 0x42, [68] = 0x43,
    [87] = 0x44, [88] = 0x45, [96] = 0x58, [102] = 0x4A, [103] = 0x52, [104] = 0x4B,
    [105] = 0x50, [106] = 0x4F, [107] = 0x4D, [108] = 0x51, [109] = 0x4E, [110] = 0x49, [111] = 0x4C,
};

#define VIN_MAX 2
static struct { uintptr_t base; struct vq evq; } vin[VIN_MAX];
static int nvin, vmods, vbtn, vx, vy;

static void vin_event(uint16_t type, uint16_t code, uint32_t value) {
    if (type == 1 && code < 0x100) {                   /* EV_KEY: a key          */
        int bit = 0;
        if (code == 42 || code == 54) bit = MOD_SHIFT;
        else if (code == 29 || code == 97) bit = MOD_CTRL;
        else if (code == 56 || code == 100) bit = MOD_ALT;
        else if (code == 125 || code == 126) bit = MOD_SUPER;
        if (bit) {
            if (value) vmods |= bit; else vmods &= ~bit;
            if (bit == MOD_SUPER && !value) input_key(KEY_SUPER, 0, vmods, SRC_QEMU);
            return;
        }
        if (!value || code >= 128 || !evdev_to_hid[code]) return;
        int ch, k = input_from_hid(evdev_to_hid[code], vmods & MOD_SHIFT, &ch);
        input_key(k, ch, vmods, SRC_QEMU);
    } else if (type == 1) {                            /* EV_KEY: a button       */
        int bit = code == 0x110 ? 1 : code == 0x111 ? 2 : code == 0x112 ? 4 : 0;
        if (value) vbtn |= bit; else vbtn &= ~bit;
        input_pointer(vx, vy, vbtn, 0, SRC_QEMU);
    } else if (type == 3) {                            /* EV_ABS                 */
        if (code == 0) vx = (int)((uint64_t)value * (uint64_t)scr_w / 32768);
        if (code == 1) vy = (int)((uint64_t)value * (uint64_t)scr_h / 32768);
    } else if (type == 2 && code == 8) {               /* EV_REL wheel           */
        input_pointer(vx, vy, vbtn, (int32_t)value > 0 ? 1 : -1, SRC_QEMU);
    } else if (type == 0) {                            /* EV_SYN                 */
        input_pointer(vx, vy, vbtn, 0, SRC_QEMU);
    }
}

static void vin_init(uintptr_t base) {
    if (nvin >= VIN_MAX) return;
    uint32_t ver;
    if (dev_start(base, 0, &ver) != 0) return;
    struct vq *q = &vin[nvin].evq;
    if (vq_setup(q, base, 0, (int)ver, 8)) return;
    for (int i = 0; i < QSZ; i++) vq_give(q, i, 8, 1);
    w32(base + R_STATUS, r32(base + R_STATUS) | 4);
    vq_kick(q);
    vin[nvin].base = base;
    nvin++;
}

/* ---- a disk ------------------------------------------------------------------------
 *  QEMU's stand-in for the Pi's microSD card (sdcard.c sends us its sectors):
 *  one request at a time, three descriptors -- the header, the data, the
 *  status byte -- and wait for it.                                          */
static struct { uintptr_t base; struct vq q; uint64_t sectors; int live; } vblk;

static void vblk_init(uintptr_t base) {
    uint32_t ver;
    if (dev_start(base, 0, &ver) != 0) return;
    if (vq_setup(&vblk.q, base, 0, (int)ver, 64)) return;
    w32(base + R_STATUS, r32(base + R_STATUS) | 4);
    vblk.base = base;
    vblk.sectors = (uint64_t)r32(base + R_CONFIG) | (uint64_t)r32(base + R_CONFIG + 4) << 32;
    vblk.live = 1;
}

uint64_t vblk_sectors(void) { return vblk.live ? vblk.sectors : 0; }

int vblk_rw(int write, uint64_t lba, void *buf, uint32_t n) {
    if (!vblk.live || lba + n > vblk.sectors) return -1;
    struct vq *q = &vblk.q;
    uint8_t *h = q->bufs;                                 /* slot 0: header and status */
    uint32_t t = write ? 1 : 0;
    memcpy(h, &t, 4); memset(h + 4, 0, 4); memcpy(h + 8, &lba, 8);
    h[16] = 0xFF;
    q->desc[0].addr = (uint64_t)(uintptr_t)h;        q->desc[0].len = 16; q->desc[0].flags = 1; q->desc[0].next = 1;
    q->desc[1].addr = (uint64_t)(uintptr_t)buf;      q->desc[1].len = n * 512;
    q->desc[1].flags = (uint16_t)(1 | (write ? 0 : 2)); q->desc[1].next = 2;
    q->desc[2].addr = (uint64_t)(uintptr_t)(h + 16); q->desc[2].len = 1; q->desc[2].flags = 2; q->desc[2].next = 0;
    q->avail[2 + q->avail_idx % QSZ] = 0;
    dsb();
    q->avail_idx++;
    q->avail[1] = q->avail_idx;
    vq_kick(q);
    uint64_t end = now_ms() + 5000;
    uint32_t len;
    while (vq_take(q, &len) < 0) if (now_ms() > end) return -1;
    w32(vblk.base + R_IACK, r32(vblk.base + R_ISR));
    return *(volatile uint8_t *)(h + 16) == 0 ? 0 : -1;
}

int virtio_init(void) {
    int found = 0;
    for (int i = 0; i < VIO_SLOTS; i++) {
        uintptr_t b = VIO_BASE + (uintptr_t)i * 0x200;
        if (r32(b + R_MAGIC) != 0x74726976u) continue;            /* "virt"     */
        uint32_t id = r32(b + R_DEVID);
        if (id == 1 && !vnet.live) { vnet_init(b); found++; }
        else if (id == 18) { vin_init(b); found++; }
        else if (id == 2 && !vblk.live) { vblk_init(b); found++; }
    }
    kprintf("[virtio] %s, %d input device%s, %s\n", vnet.live ? "network card" : "no network card",
            nvin, nvin == 1 ? "" : "s", vblk.live ? "a disk" : "no disk");
    return found;
}

void virtio_poll(void) {
    uint32_t len;
    int d, again = 0;
    if (vnet.live) {
        w32(vnet.base + R_IACK, r32(vnet.base + R_ISR));
        while ((d = vq_take(&vnet.rx, &len)) >= 0) {
            uint8_t *b = vnet.rx.bufs + (size_t)d * vnet.rx.bufsz;
            if ((int)len > vnet.hdr) net_rx(&vnet.nif, b + vnet.hdr, (int)len - vnet.hdr);
            vq_give(&vnet.rx, d, 2048, 1);
            again = 1;
        }
        if (again) vq_kick(&vnet.rx);
    }
    for (int i = 0; i < nvin; i++) {
        w32(vin[i].base + R_IACK, r32(vin[i].base + R_ISR));
        again = 0;
        while ((d = vq_take(&vin[i].evq, &len)) >= 0) {
            uint8_t *e = vin[i].evq.bufs + (size_t)d * 8;
            vin_event((uint16_t)(e[0] | e[1] << 8), (uint16_t)(e[2] | e[3] << 8),
                      (uint32_t)(e[4] | e[5] << 8 | e[6] << 16 | (uint32_t)e[7] << 24));
            vq_give(&vin[i].evq, d, 8, 1);
            again = 1;
        }
        if (again) vq_kick(&vin[i].evq);
    }
}
