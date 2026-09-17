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
#define PORTSC_LWS      (1u << 16)
#define PORTSC_WPR      (1u << 31)
/* write-1-to-clear status change bits we must preserve-mask when writing */
#define PORTSC_RW1CS    (0x00FE0000u)   /* CSC..CEC change bits */

/* Every bit that *does something* when a 1 is written to it, and which must
 * therefore be zero in a read-modify-write of PORTSC.
 *
 * PED is the one that mattered. It reads 1 on an enabled port and writing 1
 * back does not mean "stay enabled" -- it means *disable this port*. A
 * SuperSpeed port trains its link and enables itself the moment something is
 * plugged in, with no reset from anybody, so PED read 1 on exactly the ports
 * with a device on them, and the reset below wrote it straight back. The port
 * was disabled by the act of trying to reset it. Every port on the machine
 * reported a connection and not one of them enumerated.
 *
 * PR and WPR start resets, LWS commits a link-state write, and 17..23 are the
 * write-one-to-clear change bits. Everything else -- port power, link state,
 * the wake enables -- has to survive the write, so this is a mask of what to
 * drop rather than a list of what to keep. */
#define PORTSC_WMASK    (PORTSC_PED | PORTSC_PR | PORTSC_LWS | \
                         PORTSC_RW1CS | PORTSC_WPR)
#define portsc_neutral(v)  ((v) & ~PORTSC_WMASK)

/* A device may not be ready to answer until TRSTRCY after its reset ends --
 * 10 ms in USB 2.0 7.1.7.5. Enable Slot and Address Device went out the
 * microsecond PRC appeared, which an emulated device answers and a real one
 * does not have to. */
#define W_RECOVERY_US    10000ull

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
#define TRB_EVAL_CTX    13
#define TRB_RESET_EP    14
#define TRB_SET_TR_DEQ  16
#define TRB_NOOP_CMD    23
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
#define W_MSI_US       100000ull    /* a No Op's completion arriving as an IRQ */
#define W_ATTACH_US    200000ull    /* connects settling after the controller starts */

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
    /* Highest Device Context Index configured on this slot. A composite device
     * is configured one interface at a time, and each Configure Endpoint has
     * to declare a Context Entries count that still covers the endpoints added
     * by the one before it, or the controller drops them. */
    uint8_t  max_dci;
};

/* A keyboard with a hub in it, a monitor, a dock: a machine has more USB
 * devices than it has things plugged into it, and every one needs a slot, an
 * EP0 ring and a device context. Eight was enough for an emulator with two. */
#define MAX_DEV 16

/* Where a device sits, which on a bus with hubs in it is not just a port
 * number. `route` is the xHCI Route String: four bits per tier naming the hub
 * port taken at that tier, which is how the controller addresses a packet
 * through up to five hubs. `tt_slot`/`tt_port` name the high-speed hub doing
 * transaction translation for a low- or full-speed device below it -- without
 * them the controller has no idea how to reach a 1.5 Mbit keyboard hanging off
 * a 480 Mbit hub, and the device addresses and then answers nothing. */
struct usbpath {
    uint8_t  root_port;        /* 1-based root hub port                     */
    uint32_t route;            /* xHCI route string, 4 bits per tier        */
    uint8_t  depth;            /* 0 = on a root port                        */
    uint8_t  speed;            /* xHCI speed id: 1=FS 2=LS 3=HS 4=SS        */
    uint8_t  tt_slot, tt_port; /* the TT responsible for us, 0 if none      */
};

static struct {
    int present;
    int irq_ok;                /* 1 once MSI + interrupter 0 drive HID input */
    volatile uint32_t irqs;    /* interrupts actually delivered on the vector */
    /* Everything xhci_report() needs to say what happened, kept because a
     * machine with no keyboard cannot be asked a follow-up question and its
     * boot log scrolls. */
    uint16_t pci_vendor, pci_device;
    uint64_t bar_phys;
    volatile const void *bar_virt;
    const char *why;           /* why there is no USB, or 0 if there is       */
    uint32_t ports_powered;    /* ports this driver had to switch on          */
    uint32_t ports_connected;  /* ports reporting a device after the debounce */
    /* Where enumeration gave up, per port. Counted rather than only printed,
     * because the printing scrolls off a screen nobody can scroll back. */
    uint32_t fail_reset, fail_slot, fail_addr, fail_desc;
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

    if (!(sc & PORTSC_PP)) {                       /* power on if needed */
        op_wr(OP_PORTSC(p), portsc_neutral(sc) | PORTSC_PP);
        delay_us(20000);                           /* PP to a usable port */
        sc = op_rd(OP_PORTSC(p));
    }

    /* A port that is already enabled needs no reset and must not be given one.
     * That is every SuperSpeed port with a device on it: the link trains and
     * the port enables itself, and a reset here is both unnecessary and -- see
     * PORTSC_WMASK -- the thing that used to disable it. */
    if (sc & PORTSC_PED) {
        op_wr(OP_PORTSC(p), portsc_neutral(sc) | (sc & PORTSC_RW1CS));
        delay_us(W_RECOVERY_US);
        return 1;
    }

    op_wr(OP_PORTSC(p), portsc_neutral(sc) | PORTSC_PR);
    uint64_t deadline = now_us() + W_PORT_US;
    for (;;) {
        int expired = now_us() >= deadline;
        uint32_t s = op_rd(OP_PORTSC(p));
        if (s & PORTSC_PRC) {                      /* reset complete */
            op_wr(OP_PORTSC(p), portsc_neutral(s) | PORTSC_PRC);   /* clear PRC */
            if (!(s & PORTSC_PED)) {
                kprintf("[xhci] port %u: reset finished but the port did not "
                        "enable (portsc 0x%x)\n", p + 1, s);
                xh.fail_reset++;
                return 0;
            }
            /* The device is allowed 10 ms before it has to answer anything. */
            delay_us(W_RECOVERY_US);
            return 1;
        }
        if (expired) {
            kprintf("[xhci] port %u: reset did not complete in %u ms "
                    "(portsc 0x%x)\n", p + 1, (unsigned)(W_PORT_US / 1000), s);
            xh.fail_reset++;
            return 0;
        }
        __asm__ volatile("pause");
    }
}

static void hid_attach(struct usbdev *d, struct ring *ep0, const uint8_t *cfg, uint16_t total);
static void bulk_setup(struct usbdev *d, struct ring *ep0, const uint8_t *cfg, uint16_t total);
static void bulk_irq_mask(int off);
static void hub_attach(struct usbdev *d, struct ring *ep0, const struct usbpath *path);
static void enumerate_device(const struct usbpath *path);

/* One device on a root port: reset it, read the speed the port negotiated,
 * and hand it to enumerate_device() as a path of depth zero. */
static void enumerate_port(uint32_t p) {
    if (!port_reset(p)) return;
    struct usbpath path;
    memset(&path, 0, sizeof path);
    path.root_port = (uint8_t)(p + 1);
    path.speed     = (uint8_t)((op_rd(OP_PORTSC(p)) >> 10) & 0xF);
    enumerate_device(&path);
}

/* Build the input context for Address Device and enumerate one device, wherever
 * on the bus it is. */
static void enumerate_device(const struct usbpath *path) {
    uint32_t p = (uint32_t)path->root_port - 1;
    uint8_t speed = path->speed;

    /* Enable Slot */
    uint32_t slot = 0;
    int cc = cmd_exec(0, 0, 0, (TRB_ENABLE_SLOT << 10), &slot);
    if (cc != CC_SUCCESS || slot == 0 || slot > xh.maxslots) {
        kprintf("[xhci] port %u: enable slot failed (cc=%u)\n", p + 1, cc);
        xh.fail_slot++;
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
    sctx[0] = path->route | ((uint32_t)speed << 20) | (1u << 27);
    sctx[1] = ((uint32_t)path->root_port) << 16;   /* root hub port number     */
    /* A low- or full-speed device under a high-speed hub is reached through
     * that hub's transaction translator, and the controller has to be told
     * which one. Left at zero, the device addresses and then answers nothing. */
    if (path->tt_slot)
        sctx[2] = (uint32_t)path->tt_slot | ((uint32_t)path->tt_port << 8);
    uint32_t mps = mps0_for_speed(speed);
    ep0[1] = (4u << 3) | (3u << 1) | (mps << 16);  /* EP type=control, CErr=3  */
    ep0[2] = (uint32_t)(tr->phys) | 1u;            /* TR dequeue | DCS=1        */
    ep0[3] = (uint32_t)(tr->phys >> 32);
    ep0[4] = 8;                                    /* avg TRB len: 8 for control */

    /* Address Device (BSR=0) */
    cc = cmd_exec((uint32_t)inctx.phys, (uint32_t)(inctx.phys >> 32), 0,
                  (TRB_ADDRESS_DEV << 10) | (slot << 24), NULL);
    if (cc != CC_SUCCESS) {
        kprintf("[xhci] port %u slot %u: address device failed (cc=%u)\n", p + 1, slot, cc);
        xh.fail_addr++;
        return;
    }

    /* GET_DESCRIPTOR(device) -- the first eight bytes only, to begin with.
     *
     * Byte 7 is bMaxPacketSize0, and the EP0 context above was built with a
     * guess at it taken from the port speed. That guess is exact for low speed
     * (always 8) and for high speed (always 64), and a coin toss for full
     * speed, where 8, 16, 32 and 64 are all legal. Eight bytes is the one
     * length every device can answer whatever its real maximum is, because the
     * first packet of a control transfer is never split. */
    uint8_t desc[18]; memset(desc, 0, sizeof desc);
    if (control_xfer(tr, slot, 0x80, 6, 0x0100, 0, desc, 8) != 0) {
        kprintf("[xhci] port %u slot %u: get device descriptor failed\n", p + 1, slot);
        xh.fail_desc++;
        return;
    }

    /* Correct EP0 before anything longer relies on it. A controller splitting
     * an 18-byte read into 8-byte packets for a device that speaks 64 gets a
     * transaction error on the second one, and the symptom is a device that
     * enumerates on one machine and not on another. Evaluate Context (xHCI
     * 4.6.7) is how EP0 is changed under a slot that is already addressed. */
    uint32_t real_mps0 = speed == 4 ? (1u << desc[7]) : desc[7];
    if (real_mps0 && real_mps0 != mps) {
        memset(inctx.virt, 0, ctxsz() * 3);
        icc[1] = 0x2;                                  /* A1: EP0 alone      */
        ep0[1] = (4u << 3) | (3u << 1) | (real_mps0 << 16);
        ep0[2] = (uint32_t)(tr->phys) | 1u;
        ep0[3] = (uint32_t)(tr->phys >> 32);
        ep0[4] = 8;
        if (cmd_exec((uint32_t)inctx.phys, (uint32_t)(inctx.phys >> 32), 0,
                     (TRB_EVAL_CTX << 10) | (slot << 24), NULL) != CC_SUCCESS)
            kprintf("[xhci] port %u slot %u: EP0 packet size %u -> %u refused\n",
                    p + 1, slot, mps, real_mps0);
    }

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
    d->max_dci  = 1;                               /* EP0, and nothing else yet */

    /* GET_DESCRIPTOR(config, header then full).
     *
     * This buffer used to be 64 bytes and the read was clamped to it. That is
     * enough for the single-interface keyboard QEMU emulates (34 bytes) and
     * not for most real ones: a keyboard with a second HID interface for its
     * media keys is 59 bytes, a third takes it past 84, and a wireless
     * receiver carrying a keyboard and a mouse is longer still. Clamping does
     * not fail -- it truncates, so the endpoint descriptor this driver is
     * looking for is simply not in the buffer, and the device enumerates,
     * prints a line, and never delivers a keystroke. */
    static uint8_t cfg[512];
    memset(cfg, 0, sizeof cfg);
    uint16_t total = 0;
    if (control_xfer(tr, slot, 0x80, 6, 0x0200, 0, cfg, 9) == 0) {
        total = (uint16_t)cfg[2] | ((uint16_t)cfg[3] << 8);
        if (total > sizeof cfg) {
            kprintf("[xhci] slot %u: %u-byte configuration truncated to %u\n",
                    slot, total, (unsigned)sizeof cfg);
            total = (uint16_t)sizeof cfg;
        }
        if (total > 9 && control_xfer(tr, slot, 0x80, 6, 0x0200, 0, cfg, total) != 0)
            total = 0;
    }
    /* The first interface, for the summary line below. What actually gets
     * driven is decided per interface further down, not from this one. */
    for (uint32_t i = 0; i + 7 < total; ) {
        uint8_t blen = cfg[i];
        if (blen == 0) break;
        if (cfg[i + 1] == 4) {
            d->if_class = cfg[i + 5];
            d->if_sub   = cfg[i + 6];
            d->if_proto = cfg[i + 7];
            break;
        }
        i += blen;
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

    if (total == 0) {
        kprintf("[xhci] slot %u: no configuration descriptor; left alone\n", slot);
        return;
    }

    if (kc == 3)
        hid_attach(d, tr, cfg, total);
    /* SCSI transparent command set (6) over bulk-only transport (0x50) is what
     * every USB stick made in the last twenty years is. Anything else calling
     * itself class 8 is left alone rather than guessed at. */
    else if (kc == 8 && d->if_sub == 6 && d->if_proto == 0x50)
        bulk_setup(d, tr, cfg, total);
    else if (kc == 8)
        kprintf("[xhci] slot %u: mass storage subclass %x proto %x is not "
                "bulk-only SCSI; left alone\n", d->slot, d->if_sub, d->if_proto);
    else if (kc == 9)
        hub_attach(d, tr, path);
}

/* ===========================================================================
 *  USB HID boot keyboard and mouse.
 *
 *  After a HID device is addressed we SET_CONFIGURATION once, then for every
 *  boot-protocol interface it declares -- and there is usually more than one,
 *  because a wireless receiver presents its keyboard and its mouse as two
 *  interfaces of a single device -- add that interface's interrupt-IN endpoint
 *  via Configure Endpoint, switch it to the boot protocol, and arm a transfer.
 *  Reports are decoded here and pushed into the PS/2 drivers' rings
 *  (kbd_inject, mouse_inject), so nothing above this file has to know which
 *  wire the input arrived on.
 *
 *  Endpoints, not just devices, are what this deals in now. The old code took
 *  the first interrupt-IN endpoint in the whole configuration descriptor and
 *  armed it against the first interface's protocol byte, which on a combined
 *  keyboard-and-mouse receiver means one of the two devices is decoded as the
 *  other and neither works.
 * ===========================================================================*/
#define MAX_HID 8

struct hidkbd {
    int            used;
    uint8_t        slot, dci, proto;   /* proto: 1=keyboard, 2=mouse    */
    uint8_t        ep_addr;       /* USB endpoint address, for CLEAR_FEATURE */
    uint16_t       rptlen;        /* bytes asked for per transfer      */
    struct ring    tr;            /* interrupt-IN transfer ring        */
    struct ring   *ep0;           /* EP0 ring, for clearing a halt     */
    struct dma_buf rpt;           /* boot report buffer (DMA)          */
    uint8_t        last[8];       /* previous report, for edge detect  */
    /* Set by the interrupt handler when a transfer comes back with anything
     * other than a completion, cleared by the idle loop that recovers it. */
    volatile int   halted;
    /* Reports this endpoint has delivered. `usb` prints it, which is the
     * difference between "the keyboard was never found" and "the keyboard is
     * found, armed, and saying nothing" -- two problems that look identical
     * from in front of the machine and have nothing in common behind it. */
    volatile uint32_t reports;
};
static struct hidkbd hidk[MAX_HID];
static int nhid;

/* HID usage id -> char, honouring shift/ctrl. 0 == produce nothing. Mirrors the
 * PS/2 driver's control-byte conventions (KEY_*, KEY_COPY/PASTE/...). */
/* HID usage id -> PS/2 set-1 scancode, so the raw make/break tap works on a USB
 * keyboard too. Only the keys a game asks about are here; 0 means "no set-1
 * equivalent I care about" and the entry is skipped. The high bit marks the
 * extended (0xE0) page -- cursor up is 0xC8 here and numeric 8 is 0x48, which
 * is the same distinction the 8042 makes with a prefix byte. */
static const uint8_t hid_sc[0x68] = {
    [0x04]=0x1E,[0x05]=0x30,[0x06]=0x2E,[0x07]=0x20,[0x08]=0x12,[0x09]=0x21,
    [0x0A]=0x22,[0x0B]=0x23,[0x0C]=0x17,[0x0D]=0x24,[0x0E]=0x25,[0x0F]=0x26,
    [0x10]=0x32,[0x11]=0x31,[0x12]=0x18,[0x13]=0x19,[0x14]=0x10,[0x15]=0x13,
    [0x16]=0x1F,[0x17]=0x14,[0x18]=0x16,[0x19]=0x2F,[0x1A]=0x11,[0x1B]=0x2D,
    [0x1C]=0x15,[0x1D]=0x2C,
    [0x1E]=0x02,[0x1F]=0x03,[0x20]=0x04,[0x21]=0x05,[0x22]=0x06,[0x23]=0x07,
    [0x24]=0x08,[0x25]=0x09,[0x26]=0x0A,[0x27]=0x0B,
    [0x28]=0x1C,[0x29]=0x01,[0x2A]=0x0E,[0x2B]=0x0F,[0x2C]=0x39,
    [0x2D]=0x0C,[0x2E]=0x0D,[0x2F]=0x1A,[0x30]=0x1B,[0x31]=0x2B,
    [0x33]=0x27,[0x34]=0x28,[0x35]=0x29,[0x36]=0x33,[0x37]=0x34,[0x38]=0x35,
    [0x39]=0x3A,
    [0x3A]=0x3B,[0x3B]=0x3C,[0x3C]=0x3D,[0x3D]=0x3E,[0x3E]=0x3F,[0x3F]=0x40,
    [0x40]=0x41,[0x41]=0x42,[0x42]=0x43,[0x43]=0x44,[0x44]=0x57,[0x45]=0x58,
    [0x4F]=0xCD,[0x50]=0xCB,[0x51]=0xD0,[0x52]=0xC8,
};
/* The eight modifier bits of report byte 0, in order. Same encoding. */
static const uint8_t hid_mod_sc[8] = { 0x1D, 0x2A, 0x38, 0xDB, 0x9D, 0x36, 0xB8, 0xDC };

static void hid_raw(uint8_t usage, int pressed) {
    if (usage >= 0x68) return;
    uint8_t sc = hid_sc[usage];
    if (!sc) return;
    kbd_raw_inject(sc & 0x7F, pressed, (sc & 0x80) != 0);
}

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

/* bInterval means two different things, and neither of them is the number the
 * endpoint context wants. That field is an exponent: the controller services
 * the endpoint every 2^Interval * 125 us. At high and super speed bInterval is
 * already that exponent, off by one. At low and full speed it is a count of
 * whole 1 ms frames, so it has to be turned back into an exponent, and the
 * legal range there is 3..10 (xHCI 6.2.3.6, table 6-12).
 *
 * The field was left at zero until now. QEMU accepts that; a real controller
 * does not, because zero is out of range for a full-speed interrupt endpoint,
 * and Configure Endpoint comes back with a parameter error -- so the keyboard
 * is addressed, printed, and never scheduled. */
static uint32_t ep_interval(uint8_t speed, uint8_t binterval) {
    if (speed == 3 || speed == 4) {                    /* High / Super */
        int v = (int)binterval - 1;
        if (v < 0)  v = 0;
        if (v > 15) v = 15;
        return (uint32_t)v;
    }
    int frames = binterval ? binterval : 1;            /* Low / Full: 1..255 ms */
    int v = 3;
    while (v < 10 && (1 << (v - 3)) < frames) v++;
    return (uint32_t)v;
}

/* Queue one report transfer and ring the endpoint's doorbell. */
static void hid_arm(struct hidkbd *h) {
    ring_push(&h->tr, (uint32_t)h->rpt.phys, (uint32_t)(h->rpt.phys >> 32), h->rptlen,
              (TRB_NORMAL << 10) | (1u << 5));             /* IOC */
    __asm__ volatile("" ::: "memory");
    xh.db[h->slot] = h->dci;
}

/* Configure one boot-protocol interface's interrupt-IN endpoint and arm it.
 * `at` is the offset of that interface's descriptor within cfg. Returns 1 if
 * the endpoint is live. */
static int hid_iface(struct usbdev *d, struct ring *ep0r, const uint8_t *cfg,
                     uint16_t total, uint32_t at) {
    uint8_t iface = cfg[at + 2], proto = cfg[at + 7];
    if (nhid >= MAX_HID) return 0;

    /* Walk forward to this interface's first interrupt-IN endpoint, stopping
     * at the next interface descriptor. An endpoint belongs to whichever
     * interface it follows, and taking the first one in the configuration
     * regardless is how a mouse ends up armed on the keyboard's pipe. */
    uint8_t epaddr = 0, binterval = 1; uint32_t mps = 8; int have_ep = 0;
    for (uint32_t i = at + cfg[at]; i + 6 < total; ) {
        uint8_t blen = cfg[i], btype = cfg[i + 1];
        if (blen == 0 || btype == 4) break;
        if (btype == 5) {
            uint8_t addr = cfg[i + 2], attr = cfg[i + 3];
            if ((addr & 0x80) && (attr & 3) == 3) {               /* interrupt IN */
                epaddr = addr;
                mps = (((uint32_t)cfg[i + 4] | ((uint32_t)cfg[i + 5] << 8)) & 0x7FF);
                binterval = cfg[i + 6];
                have_ep = 1;
                break;
            }
        }
        i += blen;
    }
    if (!have_ep) {
        kprintf("[xhci] slot %u iface %u: no interrupt-IN endpoint\n", d->slot, iface);
        return 0;
    }

    uint8_t dci = (uint8_t)((epaddr & 0x0F) * 2 + 1);             /* IN endpoint DCI */
    if (dci < 2 || dci > 31) return 0;

    /* A boot report is 8 bytes for a keyboard and 3 or 4 for a mouse, but the
     * transfer has to be sized from the endpoint, not from the protocol: ask
     * for fewer bytes than the device sends and the packet is a babble error,
     * which halts the endpoint on the first keystroke. */
    if (mps < 8)  mps = 8;
    if (mps > 64) mps = 64;

    struct hidkbd *h = &hidk[nhid];
    if (ring_init(&h->tr) != 0) return 0;
    if (dma_alloc(64, &h->rpt) != 0) return 0;
    memset(h->rpt.virt, 0, 64);
    memset(h->last, 0, 8);
    h->rptlen = (uint16_t)mps;

    /* Context Entries has to cover every endpoint on the slot, not just this
     * one, or a second Configure Endpoint retires the first interface's. */
    uint32_t entries = dci > d->max_dci ? dci : d->max_dci;

    /* Configure Endpoint: ICC + slot + up to 31 endpoint contexts. */
    struct dma_buf inctx;
    if (dma_alloc(ctxsz() * 33, &inctx) != 0) return 0;
    memset(inctx.virt, 0, ctxsz() * 33);
    volatile uint32_t *icc  = (volatile uint32_t *)inctx.virt;
    volatile uint32_t *sctx = (volatile uint32_t *)((uint8_t *)inctx.virt + ctxsz());
    volatile uint32_t *ep   = (volatile uint32_t *)((uint8_t *)inctx.virt + ctxsz() * (dci + 1));
    icc[1] = 1u | (1u << dci);                          /* add slot + this endpoint */
    sctx[0] = ((uint32_t)d->speed << 20) | (entries << 27);
    sctx[1] = ((uint32_t)d->port) << 16;
    ep[0] = ep_interval(d->speed, binterval) << 16;
    ep[1] = (7u << 3) | (3u << 1) | (mps << 16);        /* EP type=Interrupt-IN, CErr=3 */
    ep[2] = (uint32_t)(h->tr.phys) | 1u;                /* TR dequeue | DCS=1 */
    ep[3] = (uint32_t)(h->tr.phys >> 32);
    /* Max ESIT Payload in the high half, Average TRB Length in the low half.
     * The high half used to be zero, which is a request for no periodic
     * bandwidth at all: the endpoint is configured, the doorbell is accepted,
     * and the controller never schedules a single transaction for it. */
    ep[4] = (mps << 16) | mps;
    int cc = cmd_exec((uint32_t)inctx.phys, (uint32_t)(inctx.phys >> 32), 0,
                      (TRB_CONFIG_EP << 10) | ((uint32_t)d->slot << 24), NULL);
    dma_free(&inctx);
    if (cc != CC_SUCCESS) {
        kprintf("[xhci] slot %u iface %u: configure endpoint failed (cc=%d)\n",
                d->slot, iface, cc);
        return 0;
    }
    d->max_dci = (uint8_t)entries;

    /* SET_PROTOCOL(boot=0) + SET_IDLE(indefinite) on this interface. */
    control_xfer(ep0r, d->slot, 0x21, 0x0B, 0, iface, NULL, 0);
    control_xfer(ep0r, d->slot, 0x21, 0x0A, 0, iface, NULL, 0);

    h->used = 1; h->slot = d->slot; h->dci = dci; h->proto = proto;
    h->ep_addr = epaddr; h->ep0 = ep0r; h->halted = 0;
    hid_arm(h);
    nhid++;
    kprintf("[xhci] slot %u iface %u: boot %s armed on DCI %u (%u-byte reports)\n",
            d->slot, iface, proto == 2 ? "mouse" : "keyboard", dci, mps);
    return 1;
}

/* Bring up every boot keyboard and boot mouse a HID device declares. */
static void hid_attach(struct usbdev *d, struct ring *ep0r, const uint8_t *cfg, uint16_t total) {
    /* SET_CONFIGURATION once for the device, before any endpoint is added. */
    if (control_xfer(ep0r, d->slot, 0x00, 9, cfg[5], 0, NULL, 0) != 0) {
        kprintf("[xhci] slot %u: set configuration %u failed\n", d->slot, cfg[5]);
        return;
    }

    /* Boot-subclass interfaces first, because those are the ones whose report
     * layout this driver actually knows. A device left in report protocol
     * sends something else entirely, often with a leading report id, so every
     * byte is off by one and a mouse walks off the screen on its own. */
    int armed = 0, seen = 0;
    for (uint32_t i = 0; i + 7 < total; ) {
        uint8_t blen = cfg[i];
        if (blen == 0) break;
        if (cfg[i + 1] == 4 && cfg[i + 5] == 3) {                 /* HID interface */
            seen++;
            if (cfg[i + 6] == 1 && (cfg[i + 7] == 1 || cfg[i + 7] == 2))
                armed += hid_iface(d, ep0r, cfg, total, i);
        }
        i += blen;
    }
    if (armed || seen == 0) {
        if (!seen) kprintf("[xhci] slot %u: HID device with no HID interface\n", d->slot);
        return;
    }

    /* Nothing declared itself boot-capable. The protocol byte is only defined
     * when the subclass is 1, so reading it here is a guess -- but it is a
     * guess in favour of a machine that can be typed on, and a device that
     * says "keyboard" almost always does send the boot layout when asked to.
     * If this is ever wrong the symptom is loud and immediate, which is more
     * than can be said for the alternative. */
    for (uint32_t i = 0; i + 7 < total; ) {
        uint8_t blen = cfg[i];
        if (blen == 0) break;
        if (cfg[i + 1] == 4 && cfg[i + 5] == 3 &&
            (cfg[i + 7] == 1 || cfg[i + 7] == 2)) {
            kprintf("[xhci] slot %u iface %u: subclass %u is not the boot one; "
                    "trying it as a %s anyway\n", d->slot, cfg[i + 2], cfg[i + 6],
                    cfg[i + 7] == 2 ? "mouse" : "keyboard");
            armed += hid_iface(d, ep0r, cfg, total, i);
        }
        i += blen;
    }
    if (!armed)
        kprintf("[xhci] slot %u: %d HID interface%s, none of them a usable "
                "keyboard or mouse\n", d->slot, seen, seen == 1 ? "" : "s");
}

/* ===========================================================================
 *  USB hubs.
 *
 *  Not driving these was the last thing standing between a real machine and a
 *  working keyboard. The machine that prompted it reports six things attached
 *  to its root ports -- audio, a hub, the boot stick, a mouse, a board sensor
 *  -- and the keyboard is not among them, because the keyboard is a Dell one
 *  with a hub built into it and everything behind that hub was invisible. A
 *  monitor's USB ports, a dock and the passthrough socket on another keyboard
 *  are all the same shape of problem, and "plug it in somewhere else" is not an
 *  answer when the hub is inside the keyboard.
 *
 *  A hub is driven entirely over EP0 with the hub class requests: read the hub
 *  descriptor for the port count, switch each port on, wait the descriptor's
 *  own power-good delay, then for every port reporting a connection, reset it
 *  and enumerate whatever answers -- recursively, because hubs plug into hubs.
 *
 *  Two things make this more than a loop. The controller addresses a device
 *  through a *route string*, four bits per tier naming the hub port taken at
 *  each one, so every device below a hub needs its path and not just a port
 *  number. And a low- or full-speed device under a high-speed hub is reached
 *  through that hub's transaction translator, whose slot and port have to be in
 *  the slot context or the device addresses and then never answers again --
 *  which is most keyboards, since a boot keyboard is a 1.5 Mbit device.
 * ===========================================================================*/
#define HUB_DESC_TYPE      0x29
#define HUB_PORT_CONNECTION 0
#define HUB_PORT_ENABLE     1
#define HUB_PORT_RESET      4
#define HUB_PORT_POWER      8
#define HUB_C_PORT_CONNECTION 16
#define HUB_C_PORT_RESET      20
#define MAX_HUB_DEPTH       5      /* the route string is five tiers of 4 bits */

/* Hub class requests, all on the hub's own EP0. */
static int hub_set_port_feature(struct ring *ep0, uint8_t slot, uint16_t feat, uint16_t port) {
    return control_xfer(ep0, slot, 0x23, 3, feat, port, NULL, 0);
}
static int hub_clear_port_feature(struct ring *ep0, uint8_t slot, uint16_t feat, uint16_t port) {
    return control_xfer(ep0, slot, 0x23, 1, feat, port, NULL, 0);
}
static int hub_port_status(struct ring *ep0, uint8_t slot, uint16_t port,
                           uint16_t *status, uint16_t *change) {
    uint8_t st[4];
    if (control_xfer(ep0, slot, 0xA3, 0, 0, port, st, 4) != 0) return -1;
    if (status) *status = (uint16_t)st[0] | ((uint16_t)st[1] << 8);
    if (change) *change = (uint16_t)st[2] | ((uint16_t)st[3] << 8);
    return 0;
}

/* wPortStatus bits 9 and 10 carry the speed of whatever answered the reset.
 * Neither set means full speed, which is the one that has no bit of its own. */
static uint8_t hub_port_speed(uint16_t status) {
    if (status & (1u << 9))  return 2;    /* low   */
    if (status & (1u << 10)) return 3;    /* high  */
    return 1;                             /* full  */
}

static void hub_attach(struct usbdev *d, struct ring *ep0, const struct usbpath *path) {
    if (path->depth >= MAX_HUB_DEPTH) {
        kprintf("[xhci] slot %u: hub is %u tiers down; the route string has "
                "room for %u\n", d->slot, path->depth + 1, MAX_HUB_DEPTH);
        return;
    }

    uint8_t hd[9];
    memset(hd, 0, sizeof hd);
    if (control_xfer(ep0, d->slot, 0xA0, 6, HUB_DESC_TYPE << 8, 0, hd, sizeof hd) != 0) {
        kprintf("[xhci] slot %u: hub descriptor unreadable\n", d->slot);
        return;
    }
    uint8_t nports = hd[2];
    /* bPwrOn2PwrGood is in 2 ms units and is the hub telling us how long its
     * ports take to come up. Doubled, because hubs lie low and a port read
     * before its power is good reports nothing attached. */
    uint32_t pwr_good_us = (uint32_t)hd[5] * 2000u * 2u;
    if (pwr_good_us < 20000u)  pwr_good_us = 20000u;
    if (pwr_good_us > 1000000u) pwr_good_us = 1000000u;
    if (nports == 0 || nports > 15) {
        kprintf("[xhci] slot %u: hub claims %u ports\n", d->slot, nports);
        return;
    }

    /* Tell the controller this slot is a hub before anything is put behind it:
     * it needs the port count to route packets, and Address Device has already
     * been and gone, so this is a Configure Endpoint that adds only the slot
     * context back with the hub bits set. */
    struct dma_buf inctx;
    if (dma_alloc(ctxsz() * 2, &inctx) != 0) return;
    memset(inctx.virt, 0, ctxsz() * 2);
    volatile uint32_t *icc  = (volatile uint32_t *)inctx.virt;
    volatile uint32_t *sctx = (volatile uint32_t *)((uint8_t *)inctx.virt + ctxsz());
    icc[1] = 1u;                                    /* A0: the slot context   */
    sctx[0] = path->route | ((uint32_t)d->speed << 20) | (1u << 26) |
              ((uint32_t)d->max_dci << 27);         /* bit 26 = Hub           */
    sctx[1] = ((uint32_t)path->root_port << 16) | ((uint32_t)nports << 24);
    if (path->tt_slot)
        sctx[2] = (uint32_t)path->tt_slot | ((uint32_t)path->tt_port << 8);
    /* TT Think Time, from bits 5-6 of wHubCharacteristics, in FS bit times. */
    if (d->speed == 3)
        sctx[2] |= (uint32_t)((hd[3] >> 5) & 0x3) << 16;
    int cc = cmd_exec((uint32_t)inctx.phys, (uint32_t)(inctx.phys >> 32), 0,
                      (TRB_CONFIG_EP << 10) | ((uint32_t)d->slot << 24), NULL);
    dma_free(&inctx);
    if (cc != CC_SUCCESS) {
        kprintf("[xhci] slot %u: the controller would not accept it as a hub "
                "(cc=%d)\n", d->slot, cc);
        return;
    }

    kprintf("[xhci] slot %u: hub with %u ports, power good in %u ms\n",
            d->slot, nports, pwr_good_us / 1000);

    for (uint16_t port = 1; port <= nports; port++)
        hub_set_port_feature(ep0, d->slot, HUB_PORT_POWER, port);
    delay_us(pwr_good_us);

    for (uint16_t port = 1; port <= nports; port++) {
        uint16_t status = 0, change = 0;
        if (hub_port_status(ep0, d->slot, port, &status, &change) != 0) continue;
        if (change & (1u << 0))
            hub_clear_port_feature(ep0, d->slot, HUB_C_PORT_CONNECTION, port);
        if (!(status & (1u << HUB_PORT_CONNECTION))) continue;

        /* USB 2.0 9.1.2: reset, then the port is enabled and the device is at
         * address zero and has TRSTRCY to be ready for it. */
        if (hub_set_port_feature(ep0, d->slot, HUB_PORT_RESET, port) != 0) continue;
        int ok = 0;
        uint64_t deadline = now_us() + W_PORT_US;
        for (;;) {
            int expired = now_us() >= deadline;
            if (hub_port_status(ep0, d->slot, port, &status, &change) == 0 &&
                (change & (1u << HUB_PORT_RESET))) { ok = 1; break; }
            if (expired) break;
            delay_us(2000);
        }
        if (!ok) {
            kprintf("[xhci] slot %u port %u: reset did not finish\n", d->slot, port);
            xh.fail_reset++;
            continue;
        }
        hub_clear_port_feature(ep0, d->slot, HUB_C_PORT_RESET, port);
        delay_us(W_RECOVERY_US);
        if (hub_port_status(ep0, d->slot, port, &status, 0) != 0) continue;
        if (!(status & (1u << HUB_PORT_ENABLE))) continue;

        struct usbpath below;
        memset(&below, 0, sizeof below);
        below.root_port = path->root_port;
        below.depth     = (uint8_t)(path->depth + 1);
        below.route     = path->route | ((uint32_t)(port & 0xF) << (4 * path->depth));
        below.speed     = hub_port_speed(status);
        /* Whoever is translating for us: this hub if it is the high-speed one
         * directly above a slower device, otherwise whatever was translating
         * for the hub itself. */
        if (below.speed < 3 && d->speed == 3) {
            below.tt_slot = d->slot;
            below.tt_port = (uint8_t)port;
        } else {
            below.tt_slot = path->tt_slot;
            below.tt_port = path->tt_port;
        }
        enumerate_device(&below);
    }
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

            /* A completion that is not a completion. Real devices stall, and a
             * marginal cable reports a transaction error; either way the
             * endpoint is halted, and the doorbell below then does nothing at
             * all. Re-arming without looking is how a keyboard works for a
             * while and then stops for good, with nothing said. Flag it and
             * let the idle loop recover it -- that needs the command ring, and
             * the command ring waits for events on the ring being drained
             * right here, which is not something to do inside an interrupt. */
            if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
                h->halted = 1;
                break;
            }

            h->reports++;
            /* Residual is what did NOT move, so this is what actually arrived. */
            uint32_t got = h->rptlen > resid ? h->rptlen - resid : 0;
            uint8_t *r = (uint8_t *)h->rpt.virt;
            if (h->proto == 2) {                   /* boot mouse: btn, dx, dy, wheel */
                if (got >= 3) {
                    int dx = (int)(int8_t)r[1], dy = (int)(int8_t)r[2];
                    int wh = got >= 4 ? (int)(int8_t)r[3] : 0;
                    mouse_inject(dx, dy, r[0], wh);
                }
            } else if (got >= 3) {                 /* boot keyboard */
                int shift = (r[0] & 0x22) != 0;    /* L/R shift */
                int ctrl  = (r[0] & 0x11) != 0;    /* L/R ctrl  */
                for (uint32_t i = 2; i < got && i < 8; i++) {
                    uint8_t u = r[i];
                    if (u == 0) continue;
                    int was = 0;
                    for (int j = 2; j < 8; j++) if (h->last[j] == u) { was = 1; break; }
                    if (was) continue;             /* held since last report */
                    hid_raw(u, 1);
                    char c = hid_translate(u, shift, ctrl);
                    if (c) kbd_inject(c);
                }
                /* Keys that were in the last report and are not in this one are
                 * releases. Nothing above needs them -- kbd_inject deals in
                 * characters -- but a game holding W does. */
                for (int j = 2; j < 8; j++) {
                    uint8_t u = h->last[j];
                    if (u == 0) continue;
                    int still = 0;
                    for (uint32_t i = 2; i < got && i < 8; i++) if (r[i] == u) { still = 1; break; }
                    if (!still) hid_raw(u, 0);
                }
                uint8_t mchg = (uint8_t)(r[0] ^ h->last[0]);
                for (int b = 0; b < 8; b++)
                    if (mchg & (1 << b))
                        kbd_raw_inject(hid_mod_sc[b] & 0x7F, (r[0] >> b) & 1,
                                       (hid_mod_sc[b] & 0x80) != 0);
                for (uint32_t i = 0; i < 8; i++) h->last[i] = i < got ? r[i] : 0;
            }
            hid_arm(h);                            /* ready for the next report */
            break;                                 /* one ring owns this event */
        }
    }
}

/* MSI/MSI-X interrupt handler: a boot keyboard/mouse report (or any other event)
 * arrived. Acknowledge the interrupt (USBSTS.EINT + IMAN.IP are write-1-clear),
 * keeping IE set, then drain the event ring. Acking before draining means an
 * event that lands mid-drain simply re-asserts the MSI -- nothing is lost. */
static void xhci_irq(struct registers *r) {
    (void)r;
    xh.irqs++;
    op_wr(OP_USBSTS, USBSTS_EINT);
    rt_wr(IR0_IMAN, 0x3);            /* bit0 IP (W1C) + bit1 IE (kept set) */
    if (xh.present) xhci_drain();
}

/* Put a halted interrupt endpoint back to work: Reset Endpoint, point its ring
 * at a known-good TRB, ask the device to clear its own half of the halt, arm a
 * transfer. The controller and the device each hold half of a stall, which is
 * why all three commands are here and not just the first.
 *
 * Called from the idle loop, never from xhci_irq: every step waits for its own
 * completion event on the ring the interrupt handler drains. */
static void hid_recover(void) {
    for (int k = 0; k < nhid; k++) {
        struct hidkbd *h = &hidk[k];
        if (!h->used || !h->halted) continue;
        h->halted = 0;
        kprintf("[xhci] slot %u DCI %u halted; resetting the endpoint\n",
                h->slot, h->dci);
        bulk_irq_mask(1);
        cmd_exec(0, 0, 0, (TRB_RESET_EP << 10) | ((uint32_t)h->dci << 16) |
                          ((uint32_t)h->slot << 24), NULL);
        h->tr.enq = 0; h->tr.cycle = 1;
        volatile uint32_t *l = h->tr.trb + (RING_SZ - 1) * 4;
        l[3] = (TRB_LINK << 10) | (1u << 1);
        cmd_exec((uint32_t)(h->tr.phys | 1u), (uint32_t)(h->tr.phys >> 32), 0,
                 (TRB_SET_TR_DEQ << 10) | ((uint32_t)h->dci << 16) |
                 ((uint32_t)h->slot << 24), NULL);
        if (h->ep0)                                /* CLEAR_FEATURE(ENDPOINT_HALT) */
            control_xfer(h->ep0, h->slot, 0x02, 1, 0, h->ep_addr, NULL, 0);
        hid_arm(h);
        bulk_irq_mask(0);
    }
}

/* The idle-loop half of HID input: drains the event ring when interrupts are
 * not doing it, and recovers any endpoint the drain found halted. Draining is
 * skipped while xh.irq_ok, so the handler stays the only consumer of the ring;
 * recovery runs either way, because it cannot happen inside the handler. */
void xhci_poll(void) {
    if (!xh.present || nhid == 0) return;
    if (!xh.irq_ok) xhci_drain();
    hid_recover();
}

/* 1 when HID reports arrive as interrupts, 0 when the idle loop is polling
 * for them. Both work; which one is in use is the first thing worth knowing
 * when input is late rather than absent. */
int xhci_irq_driven(void) { return xh.irq_ok; }

/* How many USB boot keyboards and mice are actually armed. For the one line
 * the boot log prints about input, which is the only evidence available to
 * somebody looking at a desktop that will not answer a keypress. */
void xhci_hid_counts(int *keyboards, int *mice) {
    int kb = 0, ms = 0;
    for (int k = 0; k < nhid; k++) {
        if (!hidk[k].used) continue;
        if (hidk[k].proto == 2) ms++; else kb++;
    }
    if (keyboards) *keyboards = kb;
    if (mice)      *mice      = ms;
}

/* Everything this driver knows about why the machine can or cannot be typed
 * on, in a handful of lines, printed at the very end of the boot.
 *
 * It exists because the boot log scrolls and a machine with no keyboard cannot
 * be asked to scroll it back. Twice now the one line that would have named the
 * fault was three rows above the top of a photograph of the screen. Printed
 * last, it is still there. */
void xhci_report(void) {
    if (!xh.present) {
        kprintf("[usb] no working USB controller%s%s\n",
                xh.why ? ": " : " was found on the PCI bus",
                xh.why ? xh.why : "");
        if (xh.bar_phys)
            kprintf("[usb] the controller is %x:%x with BAR0 at 0x%lx\n",
                    xh.pci_vendor, xh.pci_device, xh.bar_phys);
        return;
    }
    kprintf("[usb] %x:%x, BAR0 0x%lx -> %p, %u ports (%u powered here, "
            "%u with something attached), %d device%s\n",
            xh.pci_vendor, xh.pci_device, xh.bar_phys, xh.bar_virt,
            xh.maxports, xh.ports_powered, xh.ports_connected,
            xh.ndev, xh.ndev == 1 ? "" : "s");
    for (int i = 0; i < xh.ndev; i++) {
        struct usbdev *d = &xh.dev[i];
        if (!d->used) continue;
        int armed = 0;
        for (int k = 0; k < nhid; k++)
            if (hidk[k].used && hidk[k].slot == d->slot) armed++;
        kprintf("[usb]   port %u slot %u: %x:%x class %x/%x proto %x, "
                "%d endpoint%s armed\n",
                d->port, d->slot, d->vendor, d->product,
                d->dev_class ? d->dev_class : d->if_class, d->if_sub,
                d->if_proto, armed, armed == 1 ? "" : "s");
    }
    if (xh.fail_reset || xh.fail_slot || xh.fail_addr || xh.fail_desc)
        kprintf("[usb] enumeration gave up on %u port%s: %u at reset, %u at "
                "enable-slot, %u at address, %u at descriptor\n",
                xh.fail_reset + xh.fail_slot + xh.fail_addr + xh.fail_desc,
                (xh.fail_reset + xh.fail_slot + xh.fail_addr + xh.fail_desc) == 1
                    ? "" : "s",
                xh.fail_reset, xh.fail_slot, xh.fail_addr, xh.fail_desc);
    if (nhid)
        kprintf("[usb] HID input is %s\n",
                xh.irq_ok ? "interrupt-driven" : "polled from the idle loop");
}

/* Walk the armed HID endpoints, one per call. 0 once `i` is past the end. */
int xhci_hid_get(int i, struct xhci_hid *out) {
    if (i < 0 || i >= nhid || !hidk[i].used) return 0;
    if (out) {
        out->slot    = hidk[i].slot;
        out->dci     = hidk[i].dci;
        out->proto   = hidk[i].proto;
        out->rptlen  = hidk[i].rptlen;
        out->reports = hidk[i].reports;
    }
    return 1;
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
    /* The first serial-bus/USB function on the bus is not necessarily the xHCI
     * one. A board that also has a companion EHCI or UHCI controller lists it
     * first, and asking for "the first USB controller" and then returning
     * because it is not xHCI left such a machine with no USB at all. */
    struct pci_dev pd;
    {
        /* 64 was tight: a desktop board fills a good part of it with root
         * ports and functions on bus 0 alone, and a scan that fills its buffer
         * stops before whatever it has not reached yet. */
        struct pci_dev all[128];
        int n = pci_scan(all, 128), found = 0, extra = 0;
        for (int i = 0; i < n; i++) {
            if (all[i].class != 0x0C || all[i].subclass != 0x03) continue;
            if (all[i].prog_if != 0x30) continue;           /* 30h = xHCI */
            if (!found) { pd = all[i]; found = 1; } else extra++;
        }
        if (!found) return;
        if (extra)
            kprintf("[xhci] %d xHCI controllers; driving the first, so a device "
                    "on any other is invisible\n", extra + 1);
    }

    xh.pci_vendor = pd.vendor; xh.pci_device = pd.device;

    struct pci_bar bar;
    if (pci_bar(&pd, 0, &bar) != 0 || !bar.is_mmio) {
        xh.why = "BAR0 is not memory-mapped";
        kprintf("[xhci] BAR0 is not memory-mapped; no USB\n");
        return;
    }
    xh.bar_phys = bar.phys;
    if (!pci_map_bar(&bar)) {
        /* Worth spelling out, because the consequence is a machine with no
         * keyboard and a log line that used to say four words about it. The
         * kernel's direct map covers the low 4 GiB, and firmware -- OVMF
         * especially -- is fond of putting a 64-bit BAR far above that. Then
         * there is no USB at all: not the disk, and not the keyboard. */
        xh.why = "BAR0 could not be mapped";
        kprintf("[xhci] BAR0 at 0x%lx is outside the low 4 GiB the kernel maps, "
                "so there is no USB on this machine -- including its keyboard "
                "and mouse\n", bar.phys);
        return;
    }
    xh.bar_virt = bar.virt;
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
        xh.why = "the controller never came out of reset";
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
        xh.why = "the controller stayed halted";
        kprintf("[xhci] controller stayed halted after R/S; skipping USB\n");
        return;
    }

    xh.present = 1;
    kprintf("[xhci] %x:%x up: %u slots, %u ports, %u-byte ctx\n",
            pd.vendor, pd.device, xh.maxslots, xh.maxports, ctxsz());

    /* Switch every port on before looking at any of them.
     *
     * PORTSC.PP is not always set when the controller comes out of reset: a
     * controller that advertises Port Power Control (HCCPARAMS1.PPC) may hand
     * its ports over unpowered, and an unpowered port reports no connection no
     * matter what is plugged into it. port_reset() read CCS first and returned
     * for every port, so the controller came up, said it had 24 of them, and
     * enumerated nothing at all. QEMU powers its ports at creation, so there
     * the loop below never has anything to do and its absence cost nothing. */
    for (uint32_t p = 0; p < xh.maxports; p++) {
        uint32_t sc = op_rd(OP_PORTSC(p));
        if (!(sc & PORTSC_PP)) {
            op_wr(OP_PORTSC(p), (sc & ~PORTSC_RW1CS) | PORTSC_PP);
            xh.ports_powered++;
        }
    }
    if (xh.ports_powered)
        kprintf("[xhci] %u of %u ports came up unpowered; switched on\n",
                xh.ports_powered, xh.maxports);

    /* Let the connects settle before looking for them.
     *
     * The reset a few lines up dropped every device the firmware had
     * enumerated, and the ports come back the way a port comes back from any
     * reset: empty, and then occupied a moment later. USB 2.0 7.1.7.3 gives a
     * connect 100 ms to debounce, and a SuperSpeed port spends comparable time
     * training its link. Reading PORTSC.CCS in the microsecond after R/S
     * therefore finds nothing on real silicon -- and everything under an
     * emulator, where a port's connect bit is set the instant the device model
     * exists and this wait looks like dead weight. It is the difference
     * between a keyboard and no keyboard. */
    delay_us(W_ATTACH_US);

    for (uint32_t p = 0; p < xh.maxports; p++)
        if (op_rd(OP_PORTSC(p)) & PORTSC_CCS) xh.ports_connected++;

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
    if (pci_msi_enable(&pd, XHCI_MSI_VECTOR) != 0) {
        kprintf("[xhci] no MSI capability; HID input stays polled\n");
        return;
    }
    msi_install(XHCI_MSI_VECTOR, xhci_irq);
    op_wr(OP_USBSTS, USBSTS_EINT);                       /* clear stale EINT */
    rt_wr(IR0_IMAN, 0x2);                                /* IE = 1           */
    op_wr(OP_USBCMD, op_rd(OP_USBCMD) | USBCMD_INTE);    /* master INT enable */

    /* Prove the vector before trusting it.
     *
     * "The device has an MSI capability" and "the message arrives at the CPU"
     * are different claims, and only the first one was ever checked. The cost
     * of getting it wrong is not a slow keyboard: xhci_poll() stands down the
     * moment irq_ok is set, so a vector that goes nowhere takes the working
     * fallback with it and the machine has no input at all.
     *
     * So: issue a No Op command, which exists for exactly this and touches no
     * device, and wait a moment for the completion to arrive as an interrupt
     * rather than as an event we went looking for. */
    xh.irqs = 0;
    ring_push(&xh.cmd, 0, 0, 0, (TRB_NOOP_CMD << 10));
    __asm__ volatile("" ::: "memory");
    xh.db[0] = 0;
    if (wait_until(xh.irqs != 0, W_MSI_US)) {
        xh.irq_ok = 1;
        kprintf("[xhci] HID input on MSI vector 0x%x (interrupt-driven)\n",
                XHCI_MSI_VECTOR);
    } else {
        op_wr(OP_USBCMD, op_rd(OP_USBCMD) & ~USBCMD_INTE);
        rt_wr(IR0_IMAN, 0);
        kprintf("[xhci] MSI vector 0x%x delivered nothing in %u ms; HID input "
                "stays polled\n", XHCI_MSI_VECTOR, (unsigned)(W_MSI_US / 1000));
    }
}
