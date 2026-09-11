#include <stdint.h>
#include "hw.h"
#include "io.h"
#include "dma.h"
#include "netif.h"
#include "driver.h"
#include "interrupts.h"
#include "string.h"
#include "kprintf.h"

/* ===========================================================================
 *  virtio-net (legacy / VIRTIO 0.9.5 PCI transport, port-I/O registers).
 *
 *  The e1000 is the NIC QEMU offers by default, but virtio-net is what almost
 *  everything else offers: it is the default on KVM/libvirt, Proxmox, VirtualBox
 *  (with the virtio adapter), and every mainstream cloud hypervisor. Without it
 *  BoltOS has no network at all on most machines it would actually be run on.
 *
 *  The legacy transport is deliberate. It is a handful of I/O ports rather than
 *  the modern capability-walking BAR layout, QEMU still presents it by default
 *  (device id 0x1000, no `disable-legacy=on`), and it is the same virtqueue
 *  model underneath -- so this is the smallest amount of code that reaches the
 *  most hypervisors.
 *
 *  Layout of a virtqueue, all in one physically contiguous allocation:
 *      desc[qsize]                  16 bytes each
 *      avail: flags, idx, ring[qsize], used_event
 *      (padding to the next 4 KiB boundary)
 *      used:  flags, idx, ring[qsize] (8 bytes each), avail_event
 *  Queue 0 is receive, queue 1 is transmit.
 * ===========================================================================*/
#define VIRTIO_VENDOR      0x1AF4
#define VIRTIO_DEV_NET_LEG 0x1000     /* legacy/transitional virtio-net */
#define VIRTIO_DEV_NET_MOD 0x1041     /* modern id; transitional devices answer both */

/* Legacy PCI configuration registers, offsets from BAR0 (port I/O). */
#define VR_DEVICE_FEATURES 0x00
#define VR_GUEST_FEATURES  0x04
#define VR_QUEUE_PFN       0x08
#define VR_QUEUE_SIZE      0x0C
#define VR_QUEUE_SELECT    0x0E
#define VR_QUEUE_NOTIFY    0x10
#define VR_STATUS          0x12
#define VR_ISR             0x13
#define VR_MAC             0x14       /* present when VIRTIO_NET_F_MAC */

#define ST_ACK             0x01
#define ST_DRIVER          0x02
#define ST_DRIVER_OK       0x04
#define ST_FEATURES_OK     0x08
#define ST_FAILED          0x80

#define VIRTIO_NET_F_MAC   (1u << 5)

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2          /* device writes into this buffer */

#define QSIZE      256                /* both queues; QEMU offers 256 or 1024 */
#define BUFSZ      1536               /* one Ethernet frame plus the net header */
#define VNET_HDRSZ 10                 /* legacy header without mergeable buffers */

struct vring_desc  { uint64_t addr; uint32_t len; uint16_t flags, next; } __attribute__((packed));
struct vring_avail { uint16_t flags, idx; uint16_t ring[]; } __attribute__((packed));
struct vring_used_elem { uint32_t id, len; } __attribute__((packed));
struct vring_used  { uint16_t flags, idx; struct vring_used_elem ring[]; } __attribute__((packed));

struct virtq {
    struct dma_buf      mem;          /* descriptors + avail + used             */
    struct dma_buf      bufs;         /* QSIZE packet buffers, BUFSZ each       */
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    uint16_t            size;
    uint16_t            last_used;    /* how far we have consumed the used ring */
    uint16_t            free_head;    /* next descriptor to hand out (TX)       */
};

static struct {
    uint16_t     io;                  /* BAR0 port base */
    struct virtq rx, tx;
    struct netif nif;
    int          up;
} vn;

static inline uint8_t  vr8 (uint16_t off)            { return inb(vn.io + off); }
static inline void     vw8 (uint16_t off, uint8_t v) { outb(vn.io + off, v); }
static inline uint16_t vr16(uint16_t off)            { return inw(vn.io + off); }
static inline void     vw16(uint16_t off, uint16_t v){ outw(vn.io + off, v); }
static inline uint32_t vr32(uint16_t off)            { return inl(vn.io + off); }
static inline void     vw32(uint16_t off, uint32_t v){ outl(vn.io + off, v); }

/* Byte size of a vring with `n` descriptors, laid out per the spec (the used
 * ring starts on the next 4 KiB boundary after the avail ring). */
static uint64_t vring_bytes(uint16_t n) {
    uint64_t a = (uint64_t)16 * n                       /* desc  */
               + (uint64_t)6 + (uint64_t)2 * n;         /* avail */
    a = (a + 4095) & ~4095ull;
    a += (uint64_t)6 + (uint64_t)8 * n;                 /* used  */
    return (a + 4095) & ~4095ull;
}

static int q_setup(struct virtq *q, uint16_t index, int device_writes) {
    vw16(VR_QUEUE_SELECT, index);
    uint16_t n = vr16(VR_QUEUE_SIZE);
    if (n == 0) return -1;
    if (n > QSIZE) n = QSIZE;
    q->size = n;

    if (dma_alloc(vring_bytes(n), &q->mem) != 0) return -1;
    if (dma_alloc((uint64_t)n * BUFSZ, &q->bufs) != 0) { dma_free(&q->mem); return -1; }
    memset(q->mem.virt, 0, q->mem.size);

    uint8_t *base = (uint8_t *)q->mem.virt;
    q->desc  = (struct vring_desc *)base;
    q->avail = (struct vring_avail *)(base + 16ull * n);
    uint64_t used_off = (16ull * n + 6 + 2ull * n + 4095) & ~4095ull;
    q->used  = (struct vring_used *)(base + used_off);
    q->last_used = 0;
    q->free_head = 0;

    /* One descriptor per buffer, so a frame never needs a chain. */
    for (uint16_t i = 0; i < n; i++) {
        q->desc[i].addr  = q->bufs.phys + (uint64_t)i * BUFSZ;
        q->desc[i].len   = BUFSZ;
        q->desc[i].flags = device_writes ? VRING_DESC_F_WRITE : 0;
        q->desc[i].next  = 0;
    }

    /* The queue address is given as a page frame number. */
    vw32(VR_QUEUE_PFN, (uint32_t)(q->mem.phys >> 12));
    return 0;
}

/* Hand every receive descriptor to the device so frames have somewhere to land. */
static void rx_fill(void) {
    for (uint16_t i = 0; i < vn.rx.size; i++)
        vn.rx.avail->ring[i] = i;
    __asm__ volatile("" ::: "memory");
    vn.rx.avail->idx = vn.rx.size;
    __asm__ volatile("" ::: "memory");
    vw16(VR_QUEUE_NOTIFY, 0);
}

/* Drain the receive used-ring into the stack. Called from the ISR and from the
 * poll hook, so a machine whose interrupt never arrives still receives. */
/* Reached from the poll every blocking wait performs and from the receive
 * interrupt, so the two are kept out of each other: both walk the same used
 * ring, and one arriving in the middle of the other would deliver a frame
 * twice. Whichever is already draining takes the new ones too. */
static void rx_drain(void) {
    static volatile int busy;
    if (busy) return;
    busy = 1;
    while (vn.rx.last_used != vn.rx.used->idx) {
        uint16_t slot = vn.rx.last_used % vn.rx.size;
        struct vring_used_elem *e = &vn.rx.used->ring[slot];
        uint32_t id = e->id % vn.rx.size;
        uint32_t len = e->len;

        if (len > VNET_HDRSZ && len <= BUFSZ) {
            const uint8_t *p = (const uint8_t *)vn.rx.bufs.virt + (uint64_t)id * BUFSZ;
            netif_rx(&vn.nif, p + VNET_HDRSZ, (uint16_t)(len - VNET_HDRSZ));
        }

        /* Recycle the descriptor immediately. */
        uint16_t ai = vn.rx.avail->idx % vn.rx.size;
        vn.rx.avail->ring[ai] = (uint16_t)id;
        __asm__ volatile("" ::: "memory");
        vn.rx.avail->idx++;
        vn.rx.last_used++;
    }
    __asm__ volatile("" ::: "memory");
    vw16(VR_QUEUE_NOTIFY, 0);
    busy = 0;
}

static void virtio_net_isr(struct registers *r) {
    (void)r;
    uint8_t isr = vr8(VR_ISR);      /* reading it acknowledges the interrupt */
    if (isr & 1) rx_drain();
}

static void virtio_net_poll(struct netif *nif) {
    (void)nif;
    if (vn.up) rx_drain();
}

static int virtio_net_send(struct netif *nif, const void *frame, uint16_t len) {
    (void)nif;
    if (!vn.up || len == 0 || len > BUFSZ - VNET_HDRSZ) return -1;

    uint16_t id = vn.tx.free_head;
    vn.tx.free_head = (uint16_t)((vn.tx.free_head + 1) % vn.tx.size);

    uint8_t *buf = (uint8_t *)vn.tx.bufs.virt + (uint64_t)id * BUFSZ;
    memset(buf, 0, VNET_HDRSZ);                 /* no checksum offload requested */
    memcpy(buf + VNET_HDRSZ, frame, len);
    vn.tx.desc[id].addr  = vn.tx.bufs.phys + (uint64_t)id * BUFSZ;
    vn.tx.desc[id].len   = VNET_HDRSZ + len;
    vn.tx.desc[id].flags = 0;

    uint16_t ai = vn.tx.avail->idx % vn.tx.size;
    vn.tx.avail->ring[ai] = id;
    __asm__ volatile("" ::: "memory");
    vn.tx.avail->idx++;
    __asm__ volatile("" ::: "memory");
    vw16(VR_QUEUE_NOTIFY, 1);

    /* Wait briefly for the device to take it. Bounded, so a wedged device
     * costs one stall rather than the machine. */
    for (int spin = 0; spin < 200000; spin++) {
        if (vn.tx.used->idx != vn.tx.last_used) { vn.tx.last_used = vn.tx.used->idx; break; }
        __asm__ volatile("pause");
    }
    return 0;
}

static int virtio_net_probe(struct device *d) {
    if (vn.up) return -1;                        /* one interface is enough */
    const struct pci_dev *pd = &d->pci;

    /* Legacy virtio puts its registers in an I/O BAR. A device that only offers
     * the modern MMIO layout is left for a future driver rather than
     * half-driven. */
    struct pci_bar bar;
    if (pci_bar(pd, 0, &bar) != 0 || bar.is_mmio) return -1;
    vn.io = bar.port;
    pci_enable_bus_master(pd);

    /* Handshake: acknowledge, claim to be a driver, negotiate, then go live. */
    vw8(VR_STATUS, 0);                           /* reset */
    vw8(VR_STATUS, ST_ACK);
    vw8(VR_STATUS, ST_ACK | ST_DRIVER);

    uint32_t features = vr32(VR_DEVICE_FEATURES);
    /* Take only VIRTIO_NET_F_MAC. Everything else -- mergeable buffers, offloads,
     * multiqueue -- would change the header layout or the ring discipline, and
     * this driver deliberately implements the simplest one that works. */
    vw32(VR_GUEST_FEATURES, features & VIRTIO_NET_F_MAC);

    if (q_setup(&vn.rx, 0, 1) != 0) { vw8(VR_STATUS, ST_FAILED); return -1; }
    if (q_setup(&vn.tx, 1, 0) != 0) { vw8(VR_STATUS, ST_FAILED); return -1; }

    if (features & VIRTIO_NET_F_MAC) {
        for (int i = 0; i < ETH_ALEN; i++) vn.nif.mac[i] = vr8(VR_MAC + i);
    } else {
        /* No MAC from the device: use a locally administered one so the address
         * is at least valid and stable for this boot. */
        static const uint8_t la[ETH_ALEN] = { 0x02, 0x42, 0xB0, 0x17, 0x05, 0x01 };
        memcpy(vn.nif.mac, la, ETH_ALEN);
    }

    vw8(VR_STATUS, ST_ACK | ST_DRIVER | ST_DRIVER_OK);

    rx_fill();

    vn.nif.link_up = 1;
    vn.nif.send    = virtio_net_send;
    vn.nif.poll    = virtio_net_poll;
    vn.nif.drv     = &vn;
    vn.nif.name[0] = 'e'; vn.nif.name[1] = 't'; vn.nif.name[2] = 'h';
    vn.nif.name[3] = '0'; vn.nif.name[4] = 0;
    vn.up = 1;

    uint8_t line = pci_interrupt_line(pd);
    irq_install_pci(line, virtio_net_isr);

    kprintf("[virtio-net] %x:%x @ PCI %x:%x.%x io=0x%x IRQ%u "
            "mac %x:%x:%x:%x:%x:%x q=%u/%u\n",
            pd->vendor, pd->device, pd->bus, pd->slot, pd->func, vn.io, line,
            vn.nif.mac[0], vn.nif.mac[1], vn.nif.mac[2],
            vn.nif.mac[3], vn.nif.mac[4], vn.nif.mac[5],
            vn.rx.size, vn.tx.size);
    netif_register(&vn.nif);
    return 0;
}

static const struct dev_id virtio_net_ids[] = {
    { .vendor = VIRTIO_VENDOR, .device = VIRTIO_DEV_NET_LEG },
    { .vendor = VIRTIO_VENDOR, .device = VIRTIO_DEV_NET_MOD },
};
static const struct driver virtio_net_driver = {
    .name  = "virtio-net",
    .bus   = BUS_PCI,
    .ids   = virtio_net_ids,
    .nids  = 2,
    .probe = virtio_net_probe,
};
DRIVER_REGISTER(virtio_net_driver);
