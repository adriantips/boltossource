#include <stdint.h>
#include <stddef.h>
#include "hw.h"
#include "dma.h"
#include "mmio.h"
#include "string.h"
#include "kprintf.h"
#include "timeout.h"
#include "xhci.h"
#include "usb.h"
#include "keyboard.h"
#include "mouse.h"
#include "interrupts.h"

/* ===========================================================================
 *  xHCI USB host controller driver.
 *
 *  Brings up the controller (DCBAA, command ring, single-segment event ring,
 *  scratchpad buffers), then for every connected root-hub port: reset it,
 *  Enable Slot, Address Device (BSR=0), and walk EP0 control transfers to read
 *  the device + configuration descriptors. Devices are reported and remembered.
 *
 *  Bring-up (enumeration) runs with interrupts off and finds completions by
 *  polling the event-ring cycle bit -- a bounded, one-time setup sequence. Once
 *  enumeration is done the driver switches HID input to hardware interrupts:
 *  interrupter 0 + USBCMD.INTE are enabled and an MSI/MSI-X vector is routed to
 *  the IDT, so boot keyboard/mouse reports arrive as IRQs (xhci_irq drains the
 *  event ring) instead of being polled from the GUI/shell idle loop. If the
 *  controller has no MSI capability it stays on the polled xhci_poll() path.
 *
 *  This is a faithful subset of the spec: enough to enumerate and classify
 *  HID/mass-storage/hub devices over EP0.
 * ===========================================================================*/

/* --- capability registers (BAR0 + 0) -------------------------------------- */
#define CAP_CAPLENGTH   0x00   /* byte: operational regs offset    */
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* --- operational registers (BAR0 + CAPLENGTH) ----------------------------- */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_DNCTRL       0x14
#define OP_CRCR         0x18   /* 64-bit */
#define OP_DCBAAP       0x30   /* 64-bit */
#define OP_CONFIG       0x38
#define OP_PORTSC(p)    (0x400 + (p) * 0x10)   /* p is 0-based here */

#define USBCMD_RS       (1u << 0)
#define USBCMD_HCRST    (1u << 1)
#define USBCMD_INTE     (1u << 2)
#define USBSTS_HCH      (1u << 0)
#define USBSTS_EINT     (1u << 3)    /* event interrupt (write 1 to clear) */
#define USBSTS_CNR      (1u << 11)

#define XHCI_MSI_VECTOR (MSI_VECTOR_BASE + 2)   /* 0x72 */

#define PORTSC_CCS      (1u << 0)
#define PORTSC_PED      (1u << 1)
#define PORTSC_PR       (1u << 4)
#define PORTSC_PP       (1u << 9)
#define PORTSC_PRC      (1u << 21)
/* write-1-to-clear status change bits we must preserve-mask when writing */
#define PORTSC_RW1CS    (0x00FE0000u)   /* CSC..CEC change bits */

/* --- runtime / interrupter 0 (BAR0 + RTSOFF + 0x20) ----------------------- */
#define IR0_IMAN        0x00
#define IR0_IMOD        0x04
#define IR0_ERSTSZ      0x08
#define IR0_ERSTBA      0x10   /* 64-bit */
#define IR0_ERDP        0x18   /* 64-bit */

/* --- TRB types ------------------------------------------------------------ */
#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDRESS_DEV 11
#define TRB_CONFIG_EP   12
#define TRB_RESET_EP    14
#define TRB_SET_TR_DEQ  16
#define TRB_TR_EVENT    32
#define TRB_CMD_EVENT   33
#define TRB_PORTSC_EVENT 34

#define CC_SUCCESS      1
#define CC_STALL        6
#define CC_SHORT_PACKET 13

#define RING_SZ   16     /* TRBs per ring (incl. link)            */

/* Waits, in microseconds, and every one of them is a deadline rather than a
 * spin count. The difference is not cosmetic: an MMIO read costs a few
 * nanoseconds under an emulator and about a microsecond on a real PCH, so the
 * 5,000,000-iteration loop this driver used to spin was 20 ms in QEMU and half
 * a minute on the metal -- once per root port, on a controller with 24 of them.
 * The numbers below come from the xHCI 1.1 spec's own limits with slack:
 * 5.4.1 for the halt, 4.22.1 for the ownership handshake, and USB 2.0 10.2.8.1
 * for the 20 ms port reset. */
#define W_HALT_US       32000ull    /* USBSTS.HCH after clearing R/S        */
#define W_RESET_US    1000000ull    /* HCRST self-clear, then CNR clear     */
#define W_RUN_US        32000ull    /* HCH drop after setting R/S           */
#define W_OWN_US      1000000ull    /* BIOS letting go of the controller    */
#define W_PORT_US      200000ull    /* port reset completing (PRC)          */
#define W_EVENT_US    1000000ull    /* an event landing on the event ring   */
#define W_BULK_US     5000000ull    /* a bulk transfer: a real disk, so longer */

struct trb { uint32_t d[4]; };

struct ring {
    volatile uint32_t *trb;   /* RING_SZ * 4 dwords */
    uint64_t phys;
    uint32_t enq;
    uint32_t cycle;
    struct dma_buf buf;
};

struct usbdev {
    int      used;
    uint8_t  slot, port, speed;
    uint16_t vendor, product;
    uint8_t  dev_class, if_class, if_sub, if_proto;
};

#define MAX_DEV 8

static struct {
    int present;
    int irq_ok;                /* 1 once MSI + interrupter 0 drive HID input */
    volatile void *cap;        /* BAR0 base (capability regs)   */
    volatile void *op;         /* operational regs              */
    volatile void *rt;         /* runtime regs                  */
    volatile uint32_t *db;     /* doorbell array                */
    uint32_t maxslots, maxports;
    int      csz;              /* context size: 0=32B, 1=64B    */

    struct dma_buf dcbaa_buf;
    volatile uint64_t *dcbaa;

    struct ring cmd;
    struct ring evt;
    uint32_t evt_cycle;
    uint32_t evt_deq;

    struct usbdev dev[MAX_DEV];
    int ndev;
} xh;

static inline uint32_t cap_rd(uint32_t o)  { return mmio_read32(xh.cap, o); }
static inline uint32_t op_rd(uint32_t o)   { return mmio_read32(xh.op, o); }
static inline void     op_wr(uint32_t o, uint32_t v) { mmio_write32(xh.op, o, v); }
static inline void     rt_wr(uint32_t o, uint32_t v) { mmio_write32(xh.rt, 0x20 + o, v); }
static void op_wr64(uint32_t o, uint64_t v) { op_wr(o, (uint32_t)v); op_wr(o + 4, (uint32_t)(v >> 32)); }
static void rt_wr64(uint32_t o, uint64_t v) { rt_wr(o, (uint32_t)v); rt_wr(o + 4, (uint32_t)(v >> 32)); }

/* context entry size in bytes */
static uint32_t ctxsz(void) { return xh.csz ? 64 : 32; }

/* --- ring helpers --------------------------------------------------------- */
static int ring_init(struct ring *r) {
    if (dma_alloc(RING_SZ * 16, &r->buf) != 0) return -1;
    r->trb   = (volatile uint32_t *)r->buf.virt;
    r->phys  = r->buf.phys;
    r->enq   = 0;
    r->cycle = 1;
    /* last TRB is a Link back to the start, with Toggle Cycle set */
    volatile uint32_t *l = r->trb + (RING_SZ - 1) * 4;
    l[0] = (uint32_t)r->phys;
    l[1] = (uint32_t)(r->phys >> 32);
    l[2] = 0;
    l[3] = (TRB_LINK << 10) | (1u << 1);   /* TC=1 */
    return 0;
}

static uint64_t ring_push(struct ring *r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    volatile uint32_t *t = r->trb + r->enq * 4;
    uint64_t addr = r->phys + r->enq * 16;
    t[0] = d0; t[1] = d1; t[2] = d2;
    __asm__ volatile("" ::: "memory");
    t[3] = (d3 & ~1u) | r->cycle;          /* set cycle last */
    r->enq++;
    if (r->enq == RING_SZ - 1) {
        volatile uint32_t *l = r->trb + (RING_SZ - 1) * 4;
        l[3] = (l[3] & ~1u) | r->cycle;    /* arm the link TRB */
        r->enq = 0;
        r->cycle ^= 1;
    }
    return addr;
}

/* Poll the event ring for the next event. Fills cc/slot/ptr/type. Returns 0 on
 * an event, -1 on timeout. Advances ERDP. */
static int evt_poll(uint32_t *cc, uint32_t *slot, uint64_t *ptr, uint32_t *type) {
    uint64_t deadline = now_us() + W_EVENT_US;
    for (;;) {
        int expired = now_us() >= deadline;            /* clock before the TRB */
        volatile uint32_t *e = xh.evt.trb + xh.evt_deq * 4;
        uint32_t d3 = e[3];
        if ((d3 & 1u) == xh.evt_cycle) {
            if (type) *type = (d3 >> 10) & 0x3F;
            if (cc)   *cc   = (e[2] >> 24) & 0xFF;
            if (slot) *slot = (d3 >> 24) & 0xFF;
            if (ptr)  *ptr  = (uint64_t)e[0] | ((uint64_t)e[1] << 32);
            xh.evt_deq++;
            if (xh.evt_deq == RING_SZ) { xh.evt_deq = 0; xh.evt_cycle ^= 1; }
            uint64_t erdp = xh.evt.phys + xh.evt_deq * 16;
            rt_wr64(IR0_ERDP, erdp | (1u << 3));   /* EHB: clear event handler busy */
            return 0;
        }
        if (expired) return -1;
        __asm__ volatile("pause");
    }
}

/* Issue a command-ring TRB and wait for its Command Completion Event. Returns
 * completion code, and the slot id via *out_slot. -1 on timeout. */
static int cmd_exec(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, uint32_t *out_slot) {
    ring_push(&xh.cmd, d0, d1, d2, d3);
    __asm__ volatile("" ::: "memory");
    xh.db[0] = 0;                                  /* ring command doorbell */
    uint32_t cc = 0, slot = 0, type = 0; uint64_t p = 0;
    for (int guard = 0; guard < 8; guard++) {
        if (evt_poll(&cc, &slot, &p, &type) != 0) return -1;
        if (type == TRB_CMD_EVENT) { if (out_slot) *out_slot = slot; return (int)cc; }
        /* swallow port-status-change events that arrive between commands */
    }
    return -1;
}

/* --- EP0 control transfer ------------------------------------------------- */
/* tr = the device's EP0 transfer ring; setup the 8-byte request; dir_in=1 for
 * data stage device->host. buf/len describe the data stage (len 0 -> none). */
static int control_xfer(struct ring *tr, uint8_t slot,
                        uint8_t bmReqType, uint8_t bReq, uint16_t wValue,
                        uint16_t wIndex, void *buf, uint16_t len) {
    uint32_t trt = len ? (bmReqType & 0x80 ? 3u : 2u) : 0u;   /* IN=3 OUT=2 none=0 */
    /* Setup stage (IDT: data inline in the TRB) */
    ring_push(tr,
        (uint32_t)bmReqType | ((uint32_t)bReq << 8) | ((uint32_t)wValue << 16),
        (uint32_t)wIndex | ((uint32_t)len << 16),
        8,
        (1u << 6) | (TRB_SETUP << 10) | (trt << 16));
    /* Data stage */
    if (len) {
        struct dma_buf db; if (dma_alloc(len, &db) != 0) return -1;
        if (!(bmReqType & 0x80)) memcpy(db.virt, buf, len);
        ring_push(tr,
            (uint32_t)db.phys, (uint32_t)(db.phys >> 32),
            len,
            (TRB_DATA << 10) | ((bmReqType & 0x80 ? 1u : 0u) << 16));
        /* Status stage: opposite direction, raise IOC */
        ring_push(tr, 0, 0, 0,
            (TRB_STATUS << 10) | (1u << 5) | ((bmReqType & 0x80 ? 0u : 1u) << 16));
        __asm__ volatile("" ::: "memory");
        xh.db[slot] = 1;                           /* DCI 1 = EP0 */
        uint32_t cc = 0, type = 0;
        if (evt_poll(&cc, NULL, NULL, &type) != 0) { dma_free(&db); return -1; }
        if (type != TRB_TR_EVENT || (cc != CC_SUCCESS && cc != CC_SHORT_PACKET)) {
            dma_free(&db); return -1;
        }
        if (bmReqType & 0x80) memcpy(buf, db.virt, len);
        dma_free(&db);
        return 0;
    }
    /* no data: status IN */
    ring_push(tr, 0, 0, 0, (TRB_STATUS << 10) | (1u << 5) | (1u << 16));
    __asm__ volatile("" ::: "memory");
    xh.db[slot] = 1;
    uint32_t cc = 0, type = 0;
    if (evt_poll(&cc, NULL, NULL, &type) != 0) return -1;
    return (type == TRB_TR_EVENT && (cc == CC_SUCCESS || cc == CC_SHORT_PACKET)) ? 0 : -1;
}

static uint32_t mps0_for_speed(uint8_t speed) {
    switch (speed) {
        case 2:  return 8;    /* Low  */
        case 1:  return 8;    /* Full (QEMU HID is FS, bMaxPacketSize0=8) */
        case 3:  return 64;   /* High */
        case 4:  return 512;  /* Super */
        default: return 8;
    }
}

/* ---------------------------------------------------------------------------
 *  Taking the controller away from the firmware (xHCI 1.1, section 4.22.1).
 *
 *  This is the step whose absence is invisible under emulation and fatal on a
 *  real machine. QEMU's xHCI publishes no extended capabilities at all, so a
 *  driver that ignores them enumerates perfectly there. A PC's firmware, on the
 *  other hand, owns the controller when it hands control to the kernel, and
 *  keeps a System Management Interrupt armed on the registers below: every
 *  write the driver makes traps into SMM, which is free to undo it. The symptom
 *  is not an error -- it is a controller that accepts commands and answers
 *  none, so every wait in the driver runs to its limit and the machine looks
 *  hung partway down the boot log.
 *
 *  The handshake: find the USB Legacy Support capability, set the OS-owned
 *  semaphore, wait for the firmware to drop the BIOS-owned one, then disable
 *  every SMI it had armed and clear the events it had pending.
 * ---------------------------------------------------------------------------*/
#define XECP_LEGACY     1u
#define LEGSUP_BIOS_OWN (1u << 16)
#define LEGSUP_OS_OWN   (1u << 24)
/* USBLEGCTLSTS: the SMI *enable* bits (3 + 8 + 3 of them) that have to go, and
 * the three write-1-to-clear SMI event bits at the top. */
#define LEGCTL_SMI_ENABLES ((0x7u << 1) | (0xFFu << 5) | (0x7u << 17))
#define LEGCTL_SMI_EVENTS  (0x7u << 29)

static void take_ownership(uint32_t hcc1) {
    uint32_t off = ((hcc1 >> 16) & 0xFFFF) * 4;    /* xECP, counted in dwords */
    if (!off) return;                              /* no extended caps: QEMU  */

    volatile uint8_t *cap = (volatile uint8_t *)xh.cap + off;
    /* The list is firmware-provided, so it is walked defensively: a bad `next`
     * of 0 ends it, and a bounded guard stops a loop that points at itself. */
    for (int guard = 0; guard < 64; guard++) {
        uint32_t w = *(volatile uint32_t *)cap;
        uint32_t id = w & 0xFF, next = (w >> 8) & 0xFF;

        if (id == XECP_LEGACY) {
            volatile uint32_t *legsup = (volatile uint32_t *)cap;
            volatile uint32_t *legctl = (volatile uint32_t *)(cap + 4);

            if (!(*legsup & LEGSUP_BIOS_OWN)) {
                /* Already ours -- but the SMIs below may still be armed. */
                *legctl = (*legctl & ~LEGCTL_SMI_ENABLES) | LEGCTL_SMI_EVENTS;
                return;
            }

            *legsup = *legsup | LEGSUP_OS_OWN;
            if (wait_until(!(*legsup & LEGSUP_BIOS_OWN), W_OWN_US)) {
                kprintf("[xhci] firmware handed the controller over\n");
            } else {
                /* Some firmware never answers. Linux forces the bit for the
                 * same reason: a controller half-owned by a BIOS that has
                 * stopped listening is worse than one taken by force. */
                *legsup = (*legsup | LEGSUP_OS_OWN) & ~LEGSUP_BIOS_OWN;
                kprintf("[xhci] firmware would not release the controller in "
                        "%u ms; taking it\n", (unsigned)(W_OWN_US / 1000));
            }
            *legctl = (*legctl & ~LEGCTL_SMI_ENABLES) | LEGCTL_SMI_EVENTS;
            return;
        }

        if (!next) return;
        cap += next * 4;
    }
}

/* Reset one root-hub port (0-based). Returns 1 if a device is connected and
 * the port enabled, 0 otherwise. */
static int port_reset(uint32_t p) {
    uint32_t sc = op_rd(OP_PORTSC(p));
    if (!(sc & PORTSC_CCS)) return 0;              /* nothing attached */
    /* power on if needed */
    if (!(sc & PORTSC_PP)) {
        op_wr(OP_PORTSC(p), (sc & ~PORTSC_RW1CS) | PORTSC_PP);
    }
    /* assert reset (write 1 to PR), preserving non-change bits */
    sc = op_rd(OP_PORTSC(p)) & ~PORTSC_RW1CS;
    op_wr(OP_PORTSC(p), sc | PORTSC_PR);
    uint64_t deadline = now_us() + W_PORT_US;
    for (;;) {
        int expired = now_us() >= deadline;
        uint32_t s = op_rd(OP_PORTSC(p));
        if (s & PORTSC_PRC) {                      /* reset complete */
            op_wr(OP_PORTSC(p), (s & ~PORTSC_RW1CS) | PORTSC_PRC);  /* clear PRC */
            return (s & PORTSC_PED) ? 1 : 0;
        }
        if (expired) {
            kprintf("[xhci] port %u: reset did not complete in %u ms\n",
                    p + 1, (unsigned)(W_PORT_US / 1000));
            return 0;
        }
        __asm__ volatile("pause");
    }
}

static void hid_setup(struct usbdev *d, struct ring *ep0, const uint8_t *cfg, uint16_t total);
static void bulk_setup(struct usbdev *d, struct ring *ep0, const uint8_t *cfg, uint16_t total);

/* Build the input context for Address Device and enumerate one device. */
static void enumerate_port(uint32_t p) {
    if (!port_reset(p)) return;
    uint32_t sc = op_rd(OP_PORTSC(p));
    uint8_t speed = (sc >> 10) & 0xF;

    /* Enable Slot */
    uint32_t slot = 0;
    int cc = cmd_exec(0, 0, 0, (TRB_ENABLE_SLOT << 10), &slot);
    if (cc != CC_SUCCESS || slot == 0 || slot > xh.maxslots) {
        kprintf("[xhci] port %u: enable slot failed (cc=%u)\n", p + 1, cc);
        return;
    }

    /* Device context (output) -> DCBAA[slot] */
    struct dma_buf devctx;
    if (dma_alloc(ctxsz() * 32, &devctx) != 0) return;
    xh.dcbaa[slot] = devctx.phys;

    /* EP0 transfer ring */
    struct ring *tr = (struct ring *)0;
    static struct ring ep0_rings[MAX_DEV];
    if (xh.ndev >= MAX_DEV) return;
    tr = &ep0_rings[xh.ndev];
    if (ring_init(tr) != 0) return;

    /* Input context: ICC + slot + EP0, each ctxsz() bytes */
    struct dma_buf inctx;
    if (dma_alloc(ctxsz() * 3, &inctx) != 0) return;
    memset(inctx.virt, 0, ctxsz() * 3);
    volatile uint32_t *icc  = (volatile uint32_t *)inctx.virt;
    volatile uint32_t *sctx = (volatile uint32_t *)((uint8_t *)inctx.virt + ctxsz());
    volatile uint32_t *ep0  = (volatile uint32_t *)((uint8_t *)inctx.virt + ctxsz() * 2);
    icc[1] = 0x3;                                  /* Add A0 (slot) | A1 (EP0) */
    sctx[0] = ((uint32_t)speed << 20) | (1u << 27);/* speed, context entries=1 */
    sctx[1] = ((uint32_t)(p + 1)) << 16;           /* root hub port number     */
    uint32_t mps = mps0_for_speed(speed);
    ep0[1] = (4u << 3) | (3u << 1) | (mps << 16);  /* EP type=control, CErr=3  */
    ep0[2] = (uint32_t)(tr->phys) | 1u;            /* TR dequeue | DCS=1        */
    ep0[3] = (uint32_t)(tr->phys >> 32);

    /* Address Device (BSR=0) */
    cc = cmd_exec((uint32_t)inctx.phys, (uint32_t)(inctx.phys >> 32), 0,
                  (TRB_ADDRESS_DEV << 10) | (slot << 24), NULL);
    if (cc != CC_SUCCESS) {
        kprintf("[xhci] port %u slot %u: address device failed (cc=%u)\n", p + 1, slot, cc);
        return;
    }

    /* GET_DESCRIPTOR(device, 18) */
    uint8_t desc[18]; memset(desc, 0, sizeof desc);
    if (control_xfer(tr, slot, 0x80, 6, 0x0100, 0, desc, 18) != 0) {
        kprintf("[xhci] port %u slot %u: get device descriptor failed\n", p + 1, slot);
        return;
    }

    struct usbdev *d = &xh.dev[xh.ndev];
    d->used = 1; d->slot = slot; d->port = p + 1; d->speed = speed;
    d->dev_class = desc[4];
    d->vendor  = (uint16_t)desc[8]  | ((uint16_t)desc[9]  << 8);
    d->product = (uint16_t)desc[10] | ((uint16_t)desc[11] << 8);
    d->if_class = 0;

    /* GET_DESCRIPTOR(config, header then full) to read the interface class */
    uint8_t cfg[64]; memset(cfg, 0, sizeof cfg);
    if (control_xfer(tr, slot, 0x80, 6, 0x0200, 0, cfg, 9) == 0) {
        uint16_t total = (uint16_t)cfg[2] | ((uint16_t)cfg[3] << 8);
        if (total > sizeof cfg) total = sizeof cfg;
        if (total > 9 && control_xfer(tr, slot, 0x80, 6, 0x0200, 0, cfg, total) == 0) {
            /* walk descriptors for the first interface (bDescriptorType==4) */
            uint32_t i = 0;
            while (i + 1 < total) {
                uint8_t blen = cfg[i], btype = cfg[i + 1];
                if (blen == 0) break;
                if (btype == 4 && i + 7 < total) {
                    d->if_class = cfg[i + 5];
                    d->if_sub   = cfg[i + 6];
                    d->if_proto = cfg[i + 7];
                    break;
                }
                i += blen;
            }
        }
    }

    const char *kind = "device";
    uint8_t kc = d->dev_class ? d->dev_class : d->if_class;
    if (kc == 3) kind = "HID";
    else if (kc == 8) kind = "mass-storage";
    else if (kc == 9) kind = "hub";
    else if (kc == 2) kind = "comm";
    kprintf("[xhci] port %u slot %u: %s %x:%x class %x speed %u\n",
            p + 1, slot, kind, d->vendor, d->product, kc, speed);
    xh.ndev++;

    /* boot-protocol keyboard (proto 1) or mouse (proto 2): drive it */
    if (kc == 3 && (d->if_proto == 1 || d->if_proto == 2))
        hid_setup(d, tr, cfg, (uint16_t)sizeof cfg);
    /* SCSI transparent command set (6) over bulk-only transport (0x50) is what
     * every USB stick made in the last twenty years is. Anything else calling
     * itself class 8 is left alone rather than guessed at. */
    else if (kc == 8 && d->if_sub == 6 && d->if_proto == 0x50)
        bulk_setup(d, tr, cfg, (uint16_t)sizeof cfg);
    else if (kc == 8)
        kprintf("[xhci] slot %u: mass storage subclass %x proto %x is not "
                "bulk-only SCSI; left alone\n", d->slot, d->if_sub, d->if_proto);
}

/* ===========================================================================
 *  USB HID boot keyboard. After a HID device is addressed we SET_CONFIGURATION,
 *  add its interrupt-IN endpoint via Configure Endpoint, switch it to the boot
 *  protocol, and then poll the endpoint for 8-byte boot reports. Decoded keys
 *  are fed into the PS/2 keyboard ring (kbd_inject) so the rest of the system
 *  is transport-agnostic.
 * ===========================================================================*/
struct hidkbd {
    int            used;
    uint8_t        slot, dci, proto;   /* proto: 1=keyboard, 2=mouse    */
    struct ring    tr;            /* interrupt-IN transfer ring        */
    struct dma_buf rpt;           /* boot report buffer (DMA)          */
    uint8_t        last[8];       /* previous report, for edge detect  */
};
static struct hidkbd hidk[MAX_DEV];
static int nhid;

/* HID usage id -> char, honouring shift/ctrl. 0 == produce nothing. Mirrors the
 * PS/2 driver's control-byte conventions (KEY_*, KEY_COPY/PASTE/...). */
static const char hid_unshift[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
static const char hid_shift[]   = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";
static char hid_translate(uint8_t u, int shift, int ctrl) {
    if (u >= 0x04 && u <= 0x1D) {                 /* letters */
        char lc = (char)('a' + (u - 0x04));
        if (ctrl) {
            switch (lc) { case 'a': return KEY_SELALL; case 'c': return KEY_COPY;
                          case 'v': return KEY_PASTE;  case 'x': return KEY_CUT;
                          case 's': return KEY_SAVE;   default: return 0; }
        }
        return shift ? (char)('A' + (u - 0x04)) : lc;
    }
    if (u >= 0x1E && u <= 0x38) {                  /* digits + symbols (table) */
        int idx = 26 + (u - 0x1E);
        return shift ? hid_shift[idx] : hid_unshift[idx];
    }
    switch (u) {                                   /* navigation / editing */
        case 0x4F: return shift ? KEY_SRIGHT : KEY_RIGHT;
        case 0x50: return shift ? KEY_SLEFT  : KEY_LEFT;
        case 0x51: return shift ? KEY_SDOWN  : KEY_DOWN;
        case 0x52: return shift ? KEY_SUP    : KEY_UP;
        case 0x4A: return shift ? KEY_SHOME  : KEY_HOME;
        case 0x4D: return shift ? KEY_SEND   : KEY_END;
        case 0x4B: return KEY_PGUP; case 0x4E: return KEY_PGDN;
        case 0x4C: return KEY_DEL;
        default:   return 0;
    }
}

/* Configure the device's interrupt-IN endpoint and arm the first transfer. */
static void hid_setup(struct usbdev *d, struct ring *ep0, const uint8_t *cfg, uint16_t total) {
    if (nhid >= MAX_DEV) return;
    /* walk the config descriptor: capture interface number + the interrupt-IN
     * endpoint's address / max-packet size */
    uint8_t iface = 0, epaddr = 0; uint16_t mps = 8; int have_ep = 0;
    for (uint32_t i = 0; i + 1 < total; ) {
        uint8_t blen = cfg[i], btype = cfg[i + 1];
        if (blen == 0) break;
        if (btype == 4) iface = cfg[i + 2];                       /* interface  */
        else if (btype == 5) {                                    /* endpoint   */
            uint8_t addr = cfg[i + 2], attr = cfg[i + 3];
            if ((addr & 0x80) && (attr & 3) == 3) {               /* interrupt IN */
                epaddr = addr; mps = (uint16_t)cfg[i + 4] | ((uint16_t)cfg[i + 5] << 8);
                have_ep = 1; break;
            }
        }
        i += blen;
    }
    if (!have_ep) return;
    uint8_t epnum = epaddr & 0x0F;
    uint8_t dci   = (uint8_t)(epnum * 2 + 1);                     /* IN endpoint DCI */

    /* SET_CONFIGURATION(bConfigurationValue) */
    if (control_xfer(ep0, d->slot, 0x00, 9, cfg[5], 0, NULL, 0) != 0) return;

    /* transfer ring + report buffer for the interrupt endpoint */
    struct hidkbd *h = &hidk[nhid];
    if (ring_init(&h->tr) != 0) return;
    if (dma_alloc(8, &h->rpt) != 0) return;
    memset(h->last, 0, 8);

    /* Configure Endpoint: input context = ICC + slot + (dci) endpoint contexts */
    struct dma_buf inctx;
    if (dma_alloc(ctxsz() * (dci + 2), &inctx) != 0) return;
    memset(inctx.virt, 0, ctxsz() * (dci + 2));
    volatile uint32_t *icc  = (volatile uint32_t *)inctx.virt;
    volatile uint32_t *sctx = (volatile uint32_t *)((uint8_t *)inctx.virt + ctxsz());
    volatile uint32_t *ep   = (volatile uint32_t *)((uint8_t *)inctx.virt + ctxsz() * (dci + 1));
    icc[1] = 1u | (1u << dci);                          /* add slot + this endpoint */
    sctx[0] = ((uint32_t)d->speed << 20) | ((uint32_t)dci << 27);   /* ctx entries=dci */
    sctx[1] = ((uint32_t)d->port) << 16;
    ep[1] = (7u << 3) | (3u << 1) | ((uint32_t)mps << 16);  /* EP type=Interrupt-IN, CErr=3 */
    ep[2] = (uint32_t)(h->tr.phys) | 1u;                   /* TR dequeue | DCS=1 */
    ep[3] = (uint32_t)(h->tr.phys >> 32);
    ep[4] = mps;                                           /* avg TRB len / max ESIT */
    if (cmd_exec((uint32_t)inctx.phys, (uint32_t)(inctx.phys >> 32), 0,
                 (TRB_CONFIG_EP << 10) | ((uint32_t)d->slot << 24), NULL) != CC_SUCCESS) {
        dma_free(&inctx); return;
    }
    dma_free(&inctx);

    /* SET_PROTOCOL(boot=0) + SET_IDLE(0) on the interface */
    control_xfer(ep0, d->slot, 0x21, 0x0B, 0, iface, NULL, 0);
    control_xfer(ep0, d->slot, 0x21, 0x0A, 0, iface, NULL, 0);

    /* arm the first interrupt transfer */
    h->used = 1; h->slot = d->slot; h->dci = dci; h->proto = d->if_proto;
    ring_push(&h->tr, (uint32_t)h->rpt.phys, (uint32_t)(h->rpt.phys >> 32), 8,
              (TRB_NORMAL << 10) | (1u << 5));             /* IOC */
    __asm__ volatile("" ::: "memory");
    xh.db[d->slot] = dci;                                  /* ring the EP doorbell */
    nhid++;
    kprintf("[xhci] HID %s armed (slot %u ep DCI %u)\n",
            d->if_proto == 2 ? "mouse" : "keyboard", d->slot, dci);
}

/* Non-blocking peek of the event ring; returns 1 and fills the out-params if
 * an event is pending, else 0. `resid` is the TRB Transfer Length field, which
 * on a completion means the bytes that did NOT move -- the thing you need to
 * know how much of a short read actually arrived. */
static int evt_try(uint32_t *type, uint32_t *cc, uint32_t *resid, uint64_t *ptr) {
    volatile uint32_t *e = xh.evt.trb + xh.evt_deq * 4;
    uint32_t d3 = e[3];
    if ((d3 & 1u) != xh.evt_cycle) return 0;
    if (type)  *type  = (d3 >> 10) & 0x3F;
    if (cc)    *cc    = (e[2] >> 24) & 0xFF;
    if (resid) *resid = e[2] & 0x00FFFFFFu;
    if (ptr)   *ptr   = (uint64_t)e[0] | ((uint64_t)e[1] << 32);
    xh.evt_deq++;
    if (xh.evt_deq == RING_SZ) { xh.evt_deq = 0; xh.evt_cycle ^= 1; }
    rt_wr64(IR0_ERDP, (xh.evt.phys + xh.evt_deq * 16) | (1u << 3));
    return 1;
}

/* Does this event point at a TRB inside this ring? Every consumer of the event
 * ring is identified this way now. It used to be that HID was the only one, so
 * any transfer event re-armed every HID endpoint there was -- which worked
 * exactly as long as nothing else used a transfer ring, and stopped working
 * the moment a USB disk did. */
static int ring_owns(const struct ring *r, uint64_t ptr) {
    return ptr >= r->phys && ptr < r->phys + (uint64_t)RING_SZ * 16;
}

/* --- bulk pipes -----------------------------------------------------------
 *  What a mass-storage class driver gets: two endpoints and a way to push
 *  bytes through them. See include/usb.h for why this is a seam at all.
 * ------------------------------------------------------------------------*/
#define BULK_BOUNCE  (64u * 1024)     /* one transfer's worth of DMA-able memory */

struct bulkpipe {
    int      used;
    uint8_t  slot, dci_in, dci_out;
    struct ring in, out;
    struct ring *ep0;                 /* for the class control requests       */
    struct dma_buf bounce;
    /* filled in by the drain when one of this pipe's TRBs completes */
    volatile int      done;
    volatile uint32_t cc, resid;
};
static struct bulkpipe bulk[MAX_DEV];
static int nbulk;

/* 1 if the event belonged to a bulk pipe (and has been recorded). */
static int bulk_complete(uint64_t ptr, uint32_t cc, uint32_t resid) {
    for (int i = 0; i < nbulk; i++) {
        struct bulkpipe *b = &bulk[i];
        if (!b->used) continue;
        if (!ring_owns(&b->in, ptr) && !ring_owns(&b->out, ptr)) continue;
        b->cc = cc; b->resid = resid; b->done = 1;
        return 1;
    }
    return 0;
}

/* Drain every pending event-ring entry: decode HID boot reports, inject keys /
 * mouse deltas, and re-arm each interrupt transfer. Shared by the IRQ handler
 * (xhci_irq) and the polled fallback (xhci_poll). */
static void xhci_drain(void) {
    uint32_t type, cc, resid; uint64_t ptr;
    while (evt_try(&type, &cc, &resid, &ptr)) {
        if (type != TRB_TR_EVENT) continue;
        if (bulk_complete(ptr, cc, resid)) continue;
        for (int k = 0; k < nhid; k++) {
            struct hidkbd *h = &hidk[k];
            if (!h->used || !ring_owns(&h->tr, ptr)) continue;
            uint8_t *r = (uint8_t *)h->rpt.virt;
            if (h->proto == 2) {                   /* boot mouse: btn, dx, dy, wheel */
                int dx = (int)(int8_t)r[1], dy = (int)(int8_t)r[2];
                int wh = (int)(int8_t)r[3];
                mouse_inject(dx, dy, r[0], wh);
            } else {                               /* boot keyboard */
                int shift = (r[0] & 0x22) != 0;    /* L/R shift */
                int ctrl  = (r[0] & 0x11) != 0;    /* L/R ctrl  */
                for (int i = 2; i < 8; i++) {
                    uint8_t u = r[i];
                    if (u == 0) continue;
                    int was = 0;
                    for (int j = 2; j < 8; j++) if (h->last[j] == u) { was = 1; break; }
                    if (was) continue;             /* held since last report */
                    char c = hid_translate(u, shift, ctrl);
                    if (c) kbd_inject(c);
                }
                for (int i = 0; i < 8; i++) h->last[i] = r[i];
            }
            /* re-arm the transfer for the next report */
            ring_push(&h->tr, (uint32_t)h->rpt.phys, (uint32_t)(h->rpt.phys >> 32), 8,
                      (TRB_NORMAL << 10) | (1u << 5));
            __asm__ volatile("" ::: "memory");
            xh.db[h->slot] = h->dci;
        }
    }
}

/* MSI/MSI-X interrupt handler: a boot keyboard/mouse report (or any other event)
 * arrived. Acknowledge the interrupt (USBSTS.EINT + IMAN.IP are write-1-clear),
 * keeping IE set, then drain the event ring. Acking before draining means an
 * event that lands mid-drain simply re-asserts the MSI -- nothing is lost. */
static void xhci_irq(struct registers *r) {
    (void)r;
    op_wr(OP_USBSTS, USBSTS_EINT);
    rt_wr(IR0_IMAN, 0x3);            /* bit0 IP (W1C) + bit1 IE (kept set) */
    if (xh.present) xhci_drain();
}

/* Polled fallback for the idle loop. A no-op once hardware interrupts drive the
 * controller (xh.irq_ok), so the IRQ handler is the sole event-ring consumer. */
void xhci_poll(void) {
    if (!xh.present || nhid == 0 || xh.irq_ok) return;
    xhci_drain();
}

/* ===========================================================================
 *  Bulk transfers, for the mass-storage class driver.
 *
 *  These are synchronous, and they have to share the event ring with an
 *  interrupt handler that is also draining it. The arrangement is: mask
 *  interrupter 0 for the duration, drain the ring ourselves in the wait loop,
 *  then put the mask back. Nothing is lost -- HID events that arrive while we
 *  hold the ring are processed by our own drain, keystrokes included, so the
 *  keyboard keeps working through a disk read.
 *
 *  The one gap, stated because it is real: an MSI already in flight when we
 *  mask can still deliver, and then the handler and this loop are both in
 *  xhci_drain(). The window is a few instructions wide and the consequence is
 *  one event consumed by the wrong one of two functions that both know how to
 *  handle it. Closing it properly wants a lock around the event ring, which is
 *  worth doing the day anything else here runs concurrently.
 * ===========================================================================*/

static void bulk_irq_mask(int off) {
    if (!xh.irq_ok) return;
    rt_wr(IR0_IMAN, off ? 0x0 : 0x2);
}

/* Wait for a TRB on this pipe to complete. Returns the completion code, or -1
 * if nothing came back at all -- which is a wedged controller, not a failed
 * command, and the caller should say so differently. */
static int bulk_wait(struct bulkpipe *b) {
    uint64_t deadline = now_us() + W_BULK_US;
    for (;;) {
        int expired = now_us() >= deadline;
        xhci_drain();
        if (b->done) return (int)b->cc;
        if (expired) return -1;
        __asm__ volatile("pause");
    }
}

/* Recover a halted endpoint: Reset Endpoint, then point its transfer ring at a
 * known-good TRB, then tell the device to clear its own halt. All three, in
 * that order -- the controller and the device each hold half of the stall, and
 * clearing one without the other leaves the pipe dead in a way that looks like
 * a broken cable. */
static int bulk_unstall(struct bulkpipe *b, int dir_in) {
    struct ring *r = dir_in ? &b->in : &b->out;
    uint8_t dci = dir_in ? b->dci_in : b->dci_out;

    cmd_exec(0, 0, 0, (TRB_RESET_EP << 10) | ((uint32_t)dci << 16) |
                      ((uint32_t)b->slot << 24), NULL);
    /* Restart the ring from the top with a fresh cycle bit; anything queued on
     * it died with the stall. */
    r->enq = 0; r->cycle = 1;
    volatile uint32_t *l = r->trb + (RING_SZ - 1) * 4;
    l[3] = (TRB_LINK << 10) | (1u << 1);
    cmd_exec((uint32_t)(r->phys | 1u), (uint32_t)(r->phys >> 32), 0,
             (TRB_SET_TR_DEQ << 10) | ((uint32_t)dci << 16) |
             ((uint32_t)b->slot << 24), NULL);

    /* CLEAR_FEATURE(ENDPOINT_HALT) on the endpoint address, not the DCI. */
    uint8_t epaddr = (uint8_t)((dci / 2) | (dir_in ? 0x80 : 0x00));
    return control_xfer(b->ep0, b->slot, 0x02, 1, 0, epaddr, NULL, 0);
}

int usb_bulk_clear_halt(usb_bulk_t *p, int dir_in) {
    struct bulkpipe *b = (struct bulkpipe *)p->hc;
    if (!b || !b->used) return -1;
    bulk_irq_mask(1);
    int rc = bulk_unstall(b, dir_in);
    bulk_irq_mask(0);
    return rc;
}

int usb_bulk_xfer(usb_bulk_t *p, int dir_in, void *buf, uint32_t len) {
    struct bulkpipe *b = (struct bulkpipe *)p->hc;
    if (!b || !b->used || len > BULK_BOUNCE) return -1;

    struct ring *r = dir_in ? &b->in : &b->out;
    if (!dir_in && len) memcpy(b->bounce.virt, buf, len);

    bulk_irq_mask(1);
    b->done = 0; b->cc = 0; b->resid = 0;
    ring_push(r, (uint32_t)b->bounce.phys, (uint32_t)(b->bounce.phys >> 32),
              len, (TRB_NORMAL << 10) | (1u << 5));      /* IOC */
    __asm__ volatile("" ::: "memory");
    xh.db[b->slot] = dir_in ? b->dci_in : b->dci_out;

    int cc = bulk_wait(b);
    uint32_t moved = (b->resid <= len) ? len - b->resid : 0;
    bulk_irq_mask(0);

    if (cc == CC_STALL) { usb_bulk_clear_halt(p, dir_in); return -1; }
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) return -1;
    if (dir_in && moved) memcpy(buf, b->bounce.virt, moved);
    return (int)moved;
}

int usb_control(usb_bulk_t *p, uint8_t bmReqType, uint8_t bReq,
                uint16_t wValue, uint16_t wIndex, void *buf, uint16_t len) {
    struct bulkpipe *b = (struct bulkpipe *)p->hc;
    if (!b || !b->used) return -1;
    bulk_irq_mask(1);
    int rc = control_xfer(b->ep0, b->slot, bmReqType, bReq, wValue, wIndex, buf, len);
    bulk_irq_mask(0);
    return rc;
}

/* Configure a bulk-only mass-storage interface's two endpoints and hand the
 * pipe to the class driver. Called during enumeration, interrupts still off. */
static void bulk_setup(struct usbdev *d, struct ring *ep0,
                       const uint8_t *cfg, uint16_t total) {
    if (nbulk >= MAX_DEV) return;

    uint8_t iface = 0, in_addr = 0, out_addr = 0;
    uint16_t in_mps = 512, out_mps = 512;
    for (uint32_t i = 0; i + 1 < total; ) {
        uint8_t blen = cfg[i], btype = cfg[i + 1];
        if (blen == 0) break;
        if (btype == 4) iface = cfg[i + 2];
        else if (btype == 5 && i + 5 < total) {
            uint8_t addr = cfg[i + 2], attr = cfg[i + 3];
            uint16_t mps = (uint16_t)cfg[i + 4] | ((uint16_t)cfg[i + 5] << 8);
            if ((attr & 3) == 2) {                       /* bulk */
                if (addr & 0x80) { in_addr = addr;  in_mps = mps; }
                else             { out_addr = addr; out_mps = mps; }
            }
        }
        i += blen;
    }
    if (!in_addr || !out_addr) {
        kprintf("[xhci] slot %u: mass storage with no bulk pair\n", d->slot);
        return;
    }

    struct bulkpipe *b = &bulk[nbulk];
    uint8_t dci_in  = (uint8_t)((in_addr  & 0x0F) * 2 + 1);
    uint8_t dci_out = (uint8_t)((out_addr & 0x0F) * 2);
    uint8_t maxdci  = dci_in > dci_out ? dci_in : dci_out;

    if (control_xfer(ep0, d->slot, 0x00, 9, cfg[5], 0, NULL, 0) != 0) return;
    if (ring_init(&b->in) != 0) return;
    if (ring_init(&b->out) != 0) return;
    if (dma_alloc(BULK_BOUNCE, &b->bounce) != 0) return;

    struct dma_buf inctx;
    if (dma_alloc(ctxsz() * (maxdci + 2), &inctx) != 0) return;
    memset(inctx.virt, 0, ctxsz() * (maxdci + 2));
    volatile uint32_t *icc  = (volatile uint32_t *)inctx.virt;
    volatile uint32_t *sctx = (volatile uint32_t *)((uint8_t *)inctx.virt + ctxsz());
    volatile uint32_t *epi  = (volatile uint32_t *)((uint8_t *)inctx.virt + ctxsz() * (dci_in + 1));
    volatile uint32_t *epo  = (volatile uint32_t *)((uint8_t *)inctx.virt + ctxsz() * (dci_out + 1));

    icc[1] = 1u | (1u << dci_in) | (1u << dci_out);
    sctx[0] = ((uint32_t)d->speed << 20) | ((uint32_t)maxdci << 27);
    sctx[1] = ((uint32_t)d->port) << 16;

    /* EP Type in the endpoint context: the spec numbers Bulk OUT 2 and Bulk IN
     * 6, which are not adjacent and are easy to transpose. CErr=3 both ways. */
    epi[1] = (6u << 3) | (3u << 1) | ((uint32_t)in_mps << 16);
    epi[2] = (uint32_t)(b->in.phys) | 1u;
    epi[3] = (uint32_t)(b->in.phys >> 32);
    epi[4] = in_mps;
    epo[1] = (2u << 3) | (3u << 1) | ((uint32_t)out_mps << 16);
    epo[2] = (uint32_t)(b->out.phys) | 1u;
    epo[3] = (uint32_t)(b->out.phys >> 32);
    epo[4] = out_mps;

    int cc = cmd_exec((uint32_t)inctx.phys, (uint32_t)(inctx.phys >> 32), 0,
                      (TRB_CONFIG_EP << 10) | ((uint32_t)d->slot << 24), NULL);
    dma_free(&inctx);
    if (cc != CC_SUCCESS) {
        kprintf("[xhci] slot %u: configure endpoint failed (cc=%d)\n", d->slot, cc);
        return;
    }

    b->used = 1; b->slot = d->slot; b->dci_in = dci_in; b->dci_out = dci_out;
    b->ep0 = ep0;
    nbulk++;

    usb_bulk_t p;
    memset(&p, 0, sizeof p);
    p.hc = b; p.vendor = d->vendor; p.product = d->product; p.iface = iface;
    p.port[0] = 'u'; p.port[1] = 's'; p.port[2] = 'b';
    p.port[3] = (char)('0' + (d->port % 10)); p.port[4] = 0;
    usbstor_attach(&p);
}

void xhci_init(void) {
    struct pci_dev pd;
    if (!pci_find_by_class(0x0C, 0x03, &pd)) return;
    if (pd.prog_if != 0x30) return;                /* 30h = xHCI */

    struct pci_bar bar;
    if (pci_bar(&pd, 0, &bar) != 0 || !bar.is_mmio || !pci_map_bar(&bar)) {
        kprintf("[xhci] BAR0 map failed\n");
        return;
    }
    xh.cap = bar.virt;
    pci_enable_bus_master(&pd);
    pci_write16(&pd, 0x04, pci_read16(&pd, 0x04) | (1u << 10));  /* Interrupt-Disable */

    uint32_t caplen  = cap_rd(CAP_CAPLENGTH) & 0xFF;
    uint32_t hcs1    = cap_rd(CAP_HCSPARAMS1);
    uint32_t hcc1    = cap_rd(CAP_HCCPARAMS1);
    uint32_t dboff   = cap_rd(CAP_DBOFF) & ~0x3u;
    uint32_t rtsoff  = cap_rd(CAP_RTSOFF) & ~0x1Fu;
    xh.op = (volatile uint8_t *)xh.cap + caplen;
    xh.rt = (volatile uint8_t *)xh.cap + rtsoff;
    xh.db = (volatile uint32_t *)((volatile uint8_t *)xh.cap + dboff);
    xh.maxslots = hcs1 & 0xFF;
    xh.maxports = (hcs1 >> 24) & 0xFF;
    xh.csz = (hcc1 >> 2) & 1;

    /* Before touching a single operational register: the firmware still owns
     * this controller on a real machine, and writes made behind its back are
     * trapped into SMM. */
    take_ownership(hcc1);

    /* reset: stop, then HCRST, wait for HCRST clear and CNR clear */
    op_wr(OP_USBCMD, op_rd(OP_USBCMD) & ~USBCMD_RS);
    wait_until(op_rd(OP_USBSTS) & USBSTS_HCH, W_HALT_US);
    op_wr(OP_USBCMD, USBCMD_HCRST);
    if (!wait_until(!(op_rd(OP_USBCMD) & USBCMD_HCRST), W_RESET_US) ||
        !wait_until(!(op_rd(OP_USBSTS) & USBSTS_CNR),  W_RESET_US)) {
        /* A controller that will not come out of reset will not answer
         * anything that follows either, and every one of those answers is
         * another timeout. Say so once and leave it alone. */
        kprintf("[xhci] controller did not come out of reset; skipping USB\n");
        return;
    }

    /* DCBAA */
    if (dma_alloc((xh.maxslots + 1) * 8, &xh.dcbaa_buf) != 0) { kprintf("[xhci] DCBAA OOM\n"); return; }
    xh.dcbaa = (volatile uint64_t *)xh.dcbaa_buf.virt;

    /* scratchpad buffers, if the controller needs them */
    uint32_t hcs2 = cap_rd(CAP_HCSPARAMS2);
    uint32_t spb  = ((hcs2 >> 21) & 0x1F) | (((hcs2 >> 27) & 0x1F) << 5);
    if (spb) {
        struct dma_buf arr; if (dma_alloc(spb * 8, &arr) != 0) { kprintf("[xhci] scratch OOM\n"); return; }
        volatile uint64_t *a = (volatile uint64_t *)arr.virt;
        for (uint32_t i = 0; i < spb; i++) {
            struct dma_buf pg; if (dma_alloc(4096, &pg) != 0) { kprintf("[xhci] scratch pg OOM\n"); return; }
            a[i] = pg.phys;
        }
        xh.dcbaa[0] = arr.phys;
    }
    op_wr64(OP_DCBAAP, xh.dcbaa_buf.phys);
    op_wr(OP_CONFIG, xh.maxslots);

    /* command ring */
    if (ring_init(&xh.cmd) != 0) { kprintf("[xhci] cmd ring OOM\n"); return; }
    op_wr64(OP_CRCR, xh.cmd.phys | 1u);            /* RCS=1 */

    /* event ring: single segment + ERST */
    if (ring_init(&xh.evt) != 0) { kprintf("[xhci] evt ring OOM\n"); return; }
    /* event ring has no link TRB; clear the one ring_init armed */
    xh.evt.trb[(RING_SZ - 1) * 4 + 3] = 0;
    xh.evt_cycle = 1; xh.evt_deq = 0;
    struct dma_buf erst; if (dma_alloc(16, &erst) != 0) { kprintf("[xhci] ERST OOM\n"); return; }
    volatile uint32_t *e = (volatile uint32_t *)erst.virt;
    e[0] = (uint32_t)xh.evt.phys;
    e[1] = (uint32_t)(xh.evt.phys >> 32);
    e[2] = RING_SZ;                                /* ring segment size */
    e[3] = 0;
    rt_wr(IR0_ERSTSZ, 1);
    rt_wr64(IR0_ERDP, xh.evt.phys);
    rt_wr64(IR0_ERSTBA, erst.phys);
    rt_wr(IR0_IMAN, 0);                            /* keep interrupts disabled */

    /* run */
    op_wr(OP_USBCMD, op_rd(OP_USBCMD) | USBCMD_RS);
    if (!wait_until(!(op_rd(OP_USBSTS) & USBSTS_HCH), W_RUN_US)) {
        kprintf("[xhci] controller stayed halted after R/S; skipping USB\n");
        return;
    }

    xh.present = 1;
    kprintf("[xhci] %x:%x up: %u slots, %u ports, %u-byte ctx\n",
            pd.vendor, pd.device, xh.maxslots, xh.maxports, ctxsz());

    uint64_t t_enum = now_us();
    for (uint32_t p = 0; p < xh.maxports; p++) enumerate_port(p);
    t_enum = now_us() - t_enum;

    if (xh.ndev == 0) kprintf("[xhci] no devices attached\n");
    /* Printed only when it was slow, and printed at all because "the boot
     * stopped just after the xhci banner" was the whole of the evidence the
     * first time this went wrong on real hardware. A number here says whether
     * a port is fighting us or the controller simply has a lot of them. */
    if (t_enum > 1000000ull)
        kprintf("[xhci] %u ports took %lu ms to enumerate\n",
                xh.maxports, t_enum / 1000ull);

    /* Enumeration done -- switch HID input from idle-loop polling to hardware
     * interrupts. Route an MSI/MSI-X vector to the IDT, enable interrupter 0
     * (IMAN.IE) and the controller's master interrupt (USBCMD.INTE). From here
     * key/mouse reports raise IRQs that xhci_irq services. */
    if (pci_msi_enable(&pd, XHCI_MSI_VECTOR) == 0) {
        msi_install(XHCI_MSI_VECTOR, xhci_irq);
        op_wr(OP_USBSTS, USBSTS_EINT);                       /* clear stale EINT */
        rt_wr(IR0_IMAN, 0x2);                                /* IE = 1           */
        op_wr(OP_USBCMD, op_rd(OP_USBCMD) | USBCMD_INTE);    /* master INT enable */
        xh.irq_ok = 1;
        kprintf("[xhci] HID input on MSI vector 0x%x (interrupt-driven)\n",
                XHCI_MSI_VECTOR);
    } else {
        kprintf("[xhci] no MSI capability; HID input stays polled\n");
    }
}
