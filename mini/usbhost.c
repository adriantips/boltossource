/* ===========================================================================
 *  BoltOS Mini  -  mini/usbhost.c
 *
 *  The Pi's USB-A port (and the two on its header), as a USB *host*, for
 *  keyboards and mice -- including ones behind a hub, which is how the mini
 *  keyboard is plugged in.
 *
 *  Every H618 host port has an EHCI (high speed) and an OHCI (full and low
 *  speed) controller on the same wires. A keyboard is a low- or full-speed
 *  device, so Mini drives OHCI alone and leaves EHCI unconfigured: with its
 *  CONFIGFLAG clear, every port belongs to the OHCI companion, and a USB 2.0
 *  hub simply runs at full speed, passing low-speed keyboards through with
 *  the preamble OHCI adds on its own. One controller type covers it all.
 *
 *  Nothing is interrupt-driven. Enumeration uses control transfers that
 *  wait (with timeouts); keyboards and mice are interrupt endpoints, each
 *  with one transfer in flight, checked every loop. Hubs are polled for
 *  port changes four times a second rather than through their status pipe.
 *
 *  Under QEMU the same code drives `-device pci-ohci` (run-qemu.sh --ohci),
 *  with QEMU's own hub, keyboard and mouse behind it.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

/* ---- OHCI registers ---------------------------------------------------------------- */
#define HC_CONTROL   0x04
#define HC_CMDSTAT   0x08
#define HC_INTSTAT   0x0C
#define HC_INTDIS    0x14
#define HC_HCCA      0x18
#define HC_CTRLHEAD  0x20
#define HC_CTRLCUR   0x24
#define HC_BULKHEAD  0x28
#define HC_FMINTERVAL 0x34
#define HC_PERSTART  0x40
#define HC_LSTHRESH  0x44
#define HC_RHDESCA   0x48
#define HC_RHSTATUS  0x50
#define HC_RHPORT(n) (0x54 + 4 * (n))

#define PS_CCS   (1u << 0)
#define PS_PES   (1u << 1)
#define PS_PRS   (1u << 4)
#define PS_PPS   (1u << 8)
#define PS_LSDA  (1u << 9)
#define PS_CSC   (1u << 16)
#define PS_PESC  (1u << 17)
#define PS_PRSC  (1u << 20)

typedef struct { volatile uint32_t ctrl, tailp, headp, next; } ed_t;
typedef struct { volatile uint32_t ctrl, cbp, next, be; uint32_t pad[4]; } td_t;   /* 32-byte */

#define TD_SETUP  (0u << 19)
#define TD_OUT    (1u << 19)
#define TD_IN     (2u << 19)
#define TD_DATA0  (2u << 24)
#define TD_DATA1  (3u << 24)
#define TD_CARRY  (0u << 24)
#define TD_NOINT  (7u << 21)
#define TD_ROUND  (1u << 18)
#define TD_CC(t)  (((t)->ctrl >> 28) & 0xF)

/* ---- the model ------------------------------------------------------------------------ */
#define MAX_HC   4
#define MAX_DEV  16
#define MAX_INT  8

typedef struct {
    uintptr_t base;
    const char *name;
    int nports;
    uint32_t *hcca;
    ed_t *ctrl_ed;              /* one control ED, re-aimed for each transfer  */
    ed_t *int_head;             /* the periodic list: a dummy, then devices    */
    td_t *ctrl_td;              /* a pool for one control transfer             */
    uint8_t *ctrl_buf;
    int port_dev[16];           /* root port -> device slot, -1 = none          */
    int port_tried[16];         /* enumeration attempted since the last change   */
    uint64_t port_seen[16];
} hc_t;

typedef struct dev {
    int used, hc, addr, low, mps0;
    int parent, port;           /* -1 for a root port                          */
    int is_hub, hub_ports;
    uint64_t hub_next_poll;
    int hub_child[8];
    uint16_t vid, pid;
    uint8_t cls;
    char name[48];
} dev_t;

typedef struct {
    int used, dev, ep, mps, kind;   /* kind 1 keyboard, 2 mouse                 */
    ed_t *ed;
    td_t *td[2]; int cur;           /* td[cur] in flight, td[cur^1] the dummy    */
    uint8_t *buf;
    uint8_t last[8];
    struct {                        /* mouse layout from the HID report descriptor */
        int ok, rid, abs;
        int bo, bn;                 /* buttons: bit offset, count                  */
        int xo, xs, yo, ys, wo, ws; /* X, Y, wheel: bit offset, size in bits       */
        int xmin, xmax, ymin, ymax;
    } m;
    int repeat_key, repeat_ch, repeat_mods; uint64_t repeat_at;
} intr_t;

static hc_t hcs[MAX_HC];
static int nhc;
static dev_t devs[MAX_DEV];
static intr_t ints[MAX_INT];
static int next_addr = 1;
static int n_kbd, n_mouse;
static int mouse_x = 200, mouse_y = 200;

int usbh_keyboards(void) { return n_kbd; }
int usbh_mice(void) { return n_mouse; }

static inline uint32_t hr(hc_t *h, int r) { return r32(h->base + (uintptr_t)r); }
static inline void hw(hc_t *h, int r, uint32_t v) { w32(h->base + (uintptr_t)r, v); }

/* ---- control transfers ----------------------------------------------------------------- */
static void aim_ed(ed_t *ed, int addr, int ep, int low, int mps) {
    ed->ctrl = (uint32_t)addr | ((uint32_t)ep << 7) | (low ? (1u << 13) : 0) | ((uint32_t)mps << 16);
}

/* One control transfer, start to finish. Returns bytes of data moved, or -1. */
static int control(hc_t *h, int addr, int low, int mps, const uint8_t setup[8], void *data, int len) {
    int in = setup[0] & 0x80;
    td_t *t = h->ctrl_td;
    uint8_t *sb = h->ctrl_buf, *db = h->ctrl_buf + 64;
    if (len > 1024) len = 1024;
    memcpy(sb, setup, 8);
    if (!in && len) memcpy(db, data, (size_t)len);
    int n = 0;
    t[n].ctrl = TD_SETUP | TD_DATA0 | TD_NOINT | (0xFu << 28);
    t[n].cbp = dma_addr(sb); t[n].be = dma_addr(sb) + 7; t[n].next = dma_addr(&t[n + 1]); n++;
    if (len) {
        t[n].ctrl = (in ? TD_IN : TD_OUT) | TD_DATA1 | TD_NOINT | TD_ROUND | (0xFu << 28);
        t[n].cbp = dma_addr(db); t[n].be = dma_addr(db) + (uint32_t)len - 1; t[n].next = dma_addr(&t[n + 1]); n++;
    }
    t[n].ctrl = (in && len ? TD_OUT : TD_IN) | TD_DATA1 | TD_NOINT | (0xFu << 28);
    t[n].cbp = 0; t[n].be = 0; t[n].next = dma_addr(&t[n + 1]); n++;
    t[n].ctrl = 0; t[n].cbp = t[n].be = t[n].next = 0;           /* the dummy tail */

    ed_t *ed = h->ctrl_ed;
    aim_ed(ed, addr, 0, low, mps);
    ed->tailp = dma_addr(&t[n]);
    ed->headp = dma_addr(&t[0]);
    dsb();
    hw(h, HC_CMDSTAT, 1u << 1);                                   /* CLF          */

    uint64_t until = now_ms() + 500;
    while (now_ms() < until) {
        if ((ed->headp & ~0xFu) == ed->tailp) break;               /* all done     */
        if (ed->headp & 1) break;                                  /* halted       */
        __asm__ volatile("yield");
    }
    int ok = (ed->headp & ~0xFu) == ed->tailp && !(ed->headp & 1);
    for (int i = 0; i < n; i++) if (TD_CC(&t[i]) != 0) ok = 0;
    /* With buffer rounding on, a short reply is not an error: the TD just
     * stops with cbp at the next byte it would have filled (0 if full). */
    int got = len;
    if (len && t[1].cbp) got = (int)(t[1].cbp - dma_addr(db));
    ed->headp = ed->tailp = 0;                                     /* idle again   */
    aim_ed(ed, 0, 0, 0, 8);
    ed->ctrl |= 1u << 14;                                          /* skip         */
    if (!ok) return -1;
    if (in && got > 0) memcpy(data, db, (size_t)got);
    return got;
}

static int req(dev_t *d, uint8_t type, uint8_t rq, uint16_t val, uint16_t idx, void *data, int len) {
    uint8_t s[8] = { type, rq, (uint8_t)val, (uint8_t)(val >> 8), (uint8_t)idx, (uint8_t)(idx >> 8),
                     (uint8_t)len, (uint8_t)(len >> 8) };
    return control(&hcs[d->hc], d->addr, d->low, d->mps0, s, data, len);
}

/* ---- interrupt endpoints ---------------------------------------------------------------- */
static void intr_arm(intr_t *it) {
    td_t *td = it->td[it->cur], *dummy = it->td[it->cur ^ 1];
    dummy->ctrl = 0; dummy->next = 0; dummy->cbp = dummy->be = 0;
    td->ctrl = TD_IN | TD_CARRY | TD_NOINT | TD_ROUND | (0xFu << 28);
    td->cbp = dma_addr(it->buf);
    td->be = dma_addr(it->buf) + (uint32_t)it->mps - 1;
    td->next = dma_addr(dummy);
    dsb();
    it->ed->tailp = dma_addr(dummy);
}

static int intr_add(dev_t *d, int ep, int mps, int kind) {
    for (int i = 0; i < MAX_INT; i++) {
        intr_t *it = &ints[i];
        if (it->used) continue;
        memset(it, 0, sizeof *it);
        it->used = 1; it->dev = (int)(d - devs); it->ep = ep; it->mps = mps > 64 ? 64 : mps; it->kind = kind;
        it->ed = dma_alloc(sizeof(ed_t), 16);
        it->td[0] = dma_alloc(sizeof(td_t), 32);
        it->td[1] = dma_alloc(sizeof(td_t), 32);
        it->buf = dma_alloc(64, 64);
        aim_ed(it->ed, d->addr, ep, d->low, it->mps);
        it->ed->headp = dma_addr(it->td[0]);
        it->cur = 0;
        intr_arm(it);
        /* Into the periodic list, right after the dummy head. */
        hc_t *h = &hcs[d->hc];
        it->ed->next = h->int_head->next;
        dsb();
        h->int_head->next = dma_addr(it->ed);
        return i;
    }
    return -1;
}

static void intr_remove_dev(int dslot) {
    for (int i = 0; i < MAX_INT; i++) {
        intr_t *it = &ints[i];
        if (!it->used || it->dev != dslot) continue;
        hc_t *h = &hcs[devs[dslot].hc];
        it->ed->ctrl |= 1u << 14;                                  /* skip         */
        for (ed_t *p = h->int_head; p; p = p->next ? (ed_t *)(uintptr_t)p->next : 0)
            if (p->next == dma_addr(it->ed)) { p->next = it->ed->next; break; }
        if (it->kind == 1) n_kbd--; else n_mouse--;
        it->used = 0;
    }
}

/* ---- keyboards and mice ------------------------------------------------------------------- */
static int hid_mods(uint8_t m) {
    int r = 0;
    if (m & 0x22) r |= MOD_SHIFT;
    if (m & 0x11) r |= MOD_CTRL;
    if (m & 0x44) r |= MOD_ALT;
    if (m & 0x88) r |= MOD_SUPER;
    return r;
}

static void kbd_report(intr_t *it, const uint8_t *r, int n) {
    if (n < 3) return;
    int mods = hid_mods(r[0]);
    if (r[2] == 1) return;                                         /* rollover error */
    int pressed_any = 0;
    for (int i = 2; i < 8 && i < n; i++) {
        uint8_t k = r[i];
        if (k < 4) continue;
        int was = 0;
        for (int j = 2; j < 8; j++) if (it->last[j] == k) was = 1;
        if (was) { pressed_any = 1; continue; }
        int ch, key = input_from_hid(k, mods & MOD_SHIFT, &ch);
        input_key(key, ch, mods, SRC_USB);
        it->repeat_key = key; it->repeat_ch = ch; it->repeat_mods = mods;
        it->repeat_at = now_ms() + 450;
        pressed_any = 1;
    }
    /* Super alone, pressed and released: the "home" key. */
    if ((it->last[0] & 0x88) && !(r[0] & 0x88) && !pressed_any) input_key(KEY_SUPER, 0, 0, SRC_USB);
    if (!pressed_any) it->repeat_key = 0;
    memcpy(it->last, r, n < 8 ? (size_t)n : 8);
}

/* ---- HID report descriptor, enough to find a mouse or touchpad's buttons, X, Y and wheel ---- */
static int hid_bits(const uint8_t *r, int n, int off, int size, int sign) {
    if (size <= 0 || size > 24 || off < 0 || (off + size + 7) / 8 > n) return 0;
    uint32_t v = 0;
    for (int i = 0; i < size; i++) if (r[(off + i) >> 3] >> ((off + i) & 7) & 1) v |= 1u << i;
    if (sign && (v >> (size - 1) & 1)) v |= ~0u << size;
    return (int)v;
}

static int hid_parse_mouse(const uint8_t *d, int len, intr_t *it) {
    int page = 0, lmin = 0, lmax = 0, size = 0, count = 0, rid = 0, bitpos = 0;
    int us[16], nus = 0, umin = 0, umax = 0, have = 0, found = 0;
    memset(&it->m, 0, sizeof it->m);
    for (int i = 0; i < len; ) {
        uint8_t b = d[i];
        if (b == 0xFE) { i += 3 + (i + 1 < len ? d[i + 1] : 0); continue; }
        int sz = b & 3; if (sz == 3) sz = 4;
        int type = (b >> 2) & 3, tag = b >> 4;
        int v = 0, sv = 0;
        for (int k = 0; k < sz && i + 1 + k < len; k++) v |= d[i + 1 + k] << (8 * k);
        sv = sz == 1 ? (int8_t)v : sz == 2 ? (int16_t)v : v;
        i += 1 + sz;
        if (type == 1) {                                           /* global */
            if (tag == 0) page = v;
            else if (tag == 1) lmin = sv;
            else if (tag == 2) lmax = (sz == 1 && lmin >= 0) ? v : sv;
            else if (tag == 7) size = v;
            else if (tag == 8) { if (found && v != rid) break; rid = v; bitpos = 0; }
            else if (tag == 9) count = v;
        } else if (type == 2) {                                    /* local */
            if (tag == 0 && nus < 16) us[nus++] = v;
            else if (tag == 1) { umin = v; have = 1; }
            else if (tag == 2) umax = v;
        } else if (type == 0) {                                    /* main */
            if (tag == 8) {                                        /* Input */
                int cst = v & 1, rel = v & 4;
                if (!cst) {
                    if (page == 9 && !it->m.bn) { it->m.bo = bitpos; it->m.bn = count > 8 ? 8 : count; }
                    else if (page == 1)
                        for (int f = 0; f < count; f++) {
                            int u = have ? umin + f : f < nus ? us[f] : nus ? us[nus - 1] : 0;
                            int off = bitpos + f * size;
                            if (u == 0x30 && !it->m.xs) { it->m.xo = off; it->m.xs = size; it->m.abs = !rel; it->m.xmin = lmin; it->m.xmax = lmax; found = 1; it->m.rid = rid; }
                            else if (u == 0x31 && !it->m.ys) { it->m.yo = off; it->m.ys = size; it->m.ymin = lmin; it->m.ymax = lmax; }
                            else if (u == 0x38 && !it->m.ws) { it->m.wo = off; it->m.ws = size; }
                        }
                }
                bitpos += count * size;
            }
            nus = 0; have = 0; umin = umax = 0;
        }
    }
    it->m.ok = it->m.xs && it->m.ys && it->m.xs <= 16 && it->m.ys <= 16;
    return it->m.ok;
}

static void mouse_report(intr_t *it, const uint8_t *r, int n) {
    int dx, dy, wheel = 0, btn;
    if (it && it->m.ok) {
        if (it->m.rid) { if (n < 1 || r[0] != it->m.rid) return; r++; n--; }
        btn = it->m.bn ? hid_bits(r, n, it->m.bo, it->m.bn, 0) : (n ? r[0] & 7 : 0);
        dx = hid_bits(r, n, it->m.xo, it->m.xs, it->m.xmin < 0);
        dy = hid_bits(r, n, it->m.yo, it->m.ys, it->m.ymin < 0);
        if (it->m.ws) wheel = hid_bits(r, n, it->m.wo, it->m.ws, 1);
        if (it->m.abs) {                                           /* a touchpad that reports where, not how far */
            int w = it->m.xmax - it->m.xmin, h = it->m.ymax - it->m.ymin;
            if (w <= 0 || h <= 0) return;
            mouse_x = (dx - it->m.xmin) * scr_w / w;
            mouse_y = (dy - it->m.ymin) * scr_h / h;
            dx = dy = 0;
        }
        btn &= 7;
    } else {
        if (n < 3) return;
        dx = (int8_t)r[1]; dy = (int8_t)r[2]; wheel = n > 3 ? (int8_t)r[3] : 0; btn = r[0] & 7;
    }
    mouse_x += dx; mouse_y += dy;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= scr_w) mouse_x = scr_w - 1;
    if (mouse_y >= scr_h) mouse_y = scr_h - 1;
    input_pointer(mouse_x, mouse_y, btn, wheel > 0 ? 1 : wheel < 0 ? -1 : 0, SRC_USB);
}

static void intr_poll(void) {
    for (int i = 0; i < MAX_INT; i++) {
        intr_t *it = &ints[i];
        if (!it->used) continue;
        td_t *td = it->td[it->cur];
        int cc = (int)TD_CC(td);
        if (cc != 0xF) {
            int n = it->mps;
            if (td->cbp) n = (int)(td->cbp - dma_addr(it->buf));
            if (cc == 0 || cc == 9) {
                if (it->kind == 1) kbd_report(it, it->buf, n);
                else mouse_report(it, it->buf, n);
            }
            if (it->ed->headp & 1) it->ed->headp &= ~1u;           /* clear a halt  */
            it->cur ^= 1;
            intr_arm(it);
        }
        if (it->kind == 1 && it->repeat_key && now_ms() >= it->repeat_at) {
            input_key(it->repeat_key, it->repeat_ch, it->repeat_mods, SRC_USB);
            it->repeat_at = now_ms() + 33;
        }
    }
}

/* ---- enumeration ----------------------------------------------------------------------------- */
static void dev_free(int slot);

/* A new device is at address 0 on controller `hc`, its port just reset. */
static int enumerate(int hc, int low, int parent, int port) {
    int slot = -1;
    for (int i = 0; i < MAX_DEV; i++) if (!devs[i].used) { slot = i; break; }
    if (slot < 0 || next_addr > 127) return -1;
    dev_t *d = &devs[slot];
    memset(d, 0, sizeof *d);
    d->used = 1; d->hc = hc; d->addr = 0; d->low = low; d->mps0 = 8;
    d->parent = parent; d->port = port;
    for (int i = 0; i < 8; i++) d->hub_child[i] = -1;

    uint8_t desc[256];
    if (req(d, 0x80, 6, 0x0100, 0, desc, 8) < 8) { delay_ms(20); if (req(d, 0x80, 6, 0x0100, 0, desc, 8) < 8) goto fail; }
    d->mps0 = desc[7] ? desc[7] : 8;
    int addr = next_addr++;
    if (req(d, 0x00, 5, (uint16_t)addr, 0, 0, 0) < 0) goto fail;
    d->addr = addr;
    delay_ms(5);
    if (req(d, 0x80, 6, 0x0100, 0, desc, 18) < 18) goto fail;
    d->vid = (uint16_t)(desc[8] | desc[9] << 8); d->pid = (uint16_t)(desc[10] | desc[11] << 8);
    d->cls = desc[4];
    int iprod = desc[15];
    if (req(d, 0x80, 6, 0x0200, 0, desc, 9) < 9) goto fail;
    int total = desc[2] | desc[3] << 8;
    if (total > (int)sizeof desc) total = (int)sizeof desc;
    if (req(d, 0x80, 6, 0x0200, 0, desc, total) < total) goto fail;
    int cfgval = desc[5];

    snprintf(d->name, sizeof d->name, "%04x:%04x", d->vid, d->pid);
    if (iprod) {
        uint8_t s[96];
        int n = req(d, 0x80, 6, (uint16_t)(0x0300 | iprod), 0x0409, s, sizeof s);
        if (n > 2) {
            int k = 0;
            for (int i = 2; i + 1 < n && i + 1 < s[0] && k < (int)sizeof d->name - 1; i += 2)
                d->name[k++] = (s[i] >= 32 && s[i] < 127 && !s[i + 1]) ? (char)s[i] : '?';
            d->name[k] = 0;
        }
    }
    if (req(d, 0x00, 9, (uint16_t)cfgval, 0, 0, 0) < 0) goto fail;

    /* Walk the configuration: hubs, and HID keyboards and mice. */
    int cur_if = -1, cur_cls = 0, cur_proto = 0, hid_kind = 0, drives = 0, hid_len = 0;
    for (int o = 0; o + 2 <= total && desc[o] >= 2; o += desc[o]) {
        uint8_t *x = desc + o;
        if (x[1] == 4) {
            cur_if = x[2]; cur_cls = x[5]; cur_proto = x[7];
            hid_kind = 0; hid_len = 0;
            if (cur_cls == 3 && (cur_proto == 1 || cur_proto == 2)) {
                hid_kind = cur_proto;
                req(d, 0x21, 0x0B, 0, (uint16_t)cur_if, 0, 0);        /* SET_PROTOCOL boot */
                if (cur_proto == 1) req(d, 0x21, 0x0A, 0, (uint16_t)cur_if, 0, 0);   /* SET_IDLE */
            }
            if (cur_cls == 9) d->is_hub = 1;
        } else if (x[1] == 0x21 && x[0] >= 9) {
            hid_len = x[7] | x[8] << 8;
        } else if (x[1] == 5 && hid_kind && (x[2] & 0x80) && (x[3] & 3) == 3) {
            int mps = x[4] | (x[5] & 7) << 8;
            int ii = intr_add(d, x[2] & 0x0F, mps, hid_kind);
            if (ii >= 0 && hid_kind == 2 && hid_len > 0) {
                /* Many touchpads ignore boot protocol and send 12/16-bit or report-ID reports:
                 * read the real layout. */
                static uint8_t rd[512];
                int got = req(d, 0x81, 6, 0x2200, (uint16_t)cur_if, rd, hid_len > 512 ? 512 : hid_len);
                if (got > 0 && hid_parse_mouse(rd, got, &ints[ii])) {
                    req(d, 0x21, 0x0B, 1, (uint16_t)cur_if, 0, 0);    /* SET_PROTOCOL report */
                    kprintf("[usb] mouse report: x %d@%d y %d@%d id %d%s\n", ints[ii].m.xs, ints[ii].m.xo,
                            ints[ii].m.ys, ints[ii].m.yo, ints[ii].m.rid, ints[ii].m.abs ? " absolute" : "");
                }
            }
            if (ii >= 0) {
                if (hid_kind == 1) n_kbd++; else n_mouse++;
                drives = 1;
                kprintf("[usb] %s: %s on %s%s port %d\n", d->name, hid_kind == 1 ? "keyboard" : "mouse",
                        hcs[hc].name, parent >= 0 ? " hub" : "", port + 1);
                shell_toast(hid_kind == 1 ? "Keyboard connected" : "Mouse connected", d->name);
            }
            hid_kind = 0;
        }
    }
    if (d->is_hub) {
        uint8_t hd[16];
        if (req(d, 0xA0, 6, 0x2900, 0, hd, 9) >= 3) {
            d->hub_ports = hd[2] > 8 ? 8 : hd[2];
            for (int p = 1; p <= d->hub_ports; p++) req(d, 0x23, 3, 8, (uint16_t)p, 0, 0);   /* PORT_POWER */
            delay_ms(hd[5] * 2 + 20);
            kprintf("[usb] %s: hub with %d ports on %s port %d\n", d->name, d->hub_ports, hcs[hc].name, port + 1);
        }
        d->hub_next_poll = now_ms();
    } else if (!drives) {
        kprintf("[usb] %s: class %02x, not something Mini drives\n", d->name, d->cls);
    }
    return slot;
fail:
    kprintf("[usb] a device on %s port %d did not enumerate\n", hcs[hc].name, port + 1);
    d->used = 0;
    return -1;
}

static void dev_free(int slot) {
    if (slot < 0 || !devs[slot].used) return;
    dev_t *d = &devs[slot];
    for (int i = 0; i < 8; i++) if (d->hub_child[i] >= 0) dev_free(d->hub_child[i]);
    intr_remove_dev(slot);
    kprintf("[usb] %s unplugged\n", d->name);
    d->used = 0;
}

/* ---- hubs -------------------------------------------------------------------------------------- */
static void hub_poll(int slot) {
    dev_t *h = &devs[slot];
    if (now_ms() < h->hub_next_poll) return;
    h->hub_next_poll = now_ms() + 250;
    for (int p = 1; p <= h->hub_ports; p++) {
        uint8_t st[4];
        if (req(h, 0xA3, 0, 0, (uint16_t)p, st, 4) < 4) continue;
        uint16_t status = (uint16_t)(st[0] | st[1] << 8), change = (uint16_t)(st[2] | st[3] << 8);
        if (!(change & 1)) continue;
        req(h, 0x23, 1, 16, (uint16_t)p, 0, 0);                      /* C_PORT_CONNECTION */
        if (h->hub_child[p - 1] >= 0) { dev_free(h->hub_child[p - 1]); h->hub_child[p - 1] = -1; }
        if (!(status & 1)) continue;
        delay_ms(100);                                             /* debounce          */
        req(h, 0x23, 3, 4, (uint16_t)p, 0, 0);                       /* PORT_RESET        */
        for (int t = 0; t < 50; t++) {
            delay_ms(10);
            if (req(h, 0xA3, 0, 0, (uint16_t)p, st, 4) == 4 && (st[2] & 0x10)) break;
        }
        req(h, 0x23, 1, 20, (uint16_t)p, 0, 0);                      /* C_PORT_RESET      */
        status = (uint16_t)(st[0] | st[1] << 8);
        delay_ms(20);
        h->hub_child[p - 1] = enumerate(h->hc, (status >> 9) & 1, slot, p - 1);
    }
}

/* ---- root ports ---------------------------------------------------------------------------------- */
static void root_poll(int hci) {
    hc_t *h = &hcs[hci];
    for (int p = 0; p < h->nports; p++) {
        uint32_t ps = hr(h, HC_RHPORT(p));
        /* A device already there when the controller was reset need not
         * raise a connect change: an occupied port with nothing on it in
         * our table is a connect too. */
        int fresh = (ps & PS_CCS) && h->port_dev[p] < 0 && !h->port_tried[p];
        if (!(ps & PS_CSC) && !fresh) continue;
        h->port_tried[p] = 1;
        hw(h, HC_RHPORT(p), PS_CSC);
        if (h->port_dev[p] >= 0) { dev_free(h->port_dev[p]); h->port_dev[p] = -1; }
        if (!(ps & PS_CCS)) { h->port_tried[p] = 0; continue; }
        delay_ms(100);
        hw(h, HC_RHPORT(p), PS_PRS);
        for (int t = 0; t < 50 && !(hr(h, HC_RHPORT(p)) & PS_PRSC); t++) delay_ms(2);
        hw(h, HC_RHPORT(p), PS_PRSC);
        delay_ms(20);
        ps = hr(h, HC_RHPORT(p));
        if (!(ps & PS_PES)) { kprintf("[usb] %s port %d would not enable\n", h->name, p + 1); continue; }
        h->port_dev[p] = enumerate(hci, (ps & PS_LSDA) != 0, -1, p);
    }
}

/* ---- controllers ---------------------------------------------------------------------------------- */
static int hc_start(uintptr_t base, const char *name) {
    if (nhc >= MAX_HC) return -1;
    hc_t *h = &hcs[nhc];
    memset(h, 0, sizeof *h);
    h->base = base; h->name = name;
    for (int i = 0; i < 16; i++) h->port_dev[i] = -1;
    if ((hr(h, 0x00) & 0xFF) != 0x10) { kprintf("[usb] %s: no OHCI here (rev %x)\n", name, hr(h, 0)); return -1; }

    h->hcca = dma_alloc(256, 256);
    h->ctrl_ed = dma_alloc(sizeof(ed_t), 16);
    h->int_head = dma_alloc(sizeof(ed_t), 16);
    h->ctrl_td = dma_alloc(8 * sizeof(td_t), 32);
    h->ctrl_buf = dma_alloc(64 + 1024, 64);
    h->ctrl_ed->ctrl = 1u << 14;                                   /* skip until used */
    h->int_head->ctrl = 1u << 14;
    for (int i = 0; i < 32; i++) h->hcca[i] = dma_addr(h->int_head);

    hw(h, HC_CMDSTAT, 1);                                          /* HCR          */
    for (int i = 0; i < 1000 && (hr(h, HC_CMDSTAT) & 1); i++) delay_us(10);
    hw(h, HC_FMINTERVAL, 0xA7782EDFu);                             /* FIT, FSMPS, FI */
    hw(h, HC_PERSTART, 0x2A2F);
    hw(h, HC_LSTHRESH, 0x628);
    hw(h, HC_HCCA, dma_addr(h->hcca));
    hw(h, HC_CTRLHEAD, dma_addr(h->ctrl_ed));
    hw(h, HC_BULKHEAD, 0);
    hw(h, HC_INTDIS, 0xFFFFFFFFu);
    hw(h, HC_CONTROL, (2u << 6) | (1u << 4) | (1u << 2) | 3u);     /* operational, CLE, PLE */
    hw(h, HC_RHSTATUS, 1u << 16);                                  /* power on     */
    h->nports = (int)(hr(h, HC_RHDESCA) & 0xFF);
    if (h->nports > 15) h->nports = 15;
    for (int p = 0; p < h->nports; p++) hw(h, HC_RHPORT(p), PS_PPS);
    delay_ms(((hr(h, HC_RHDESCA) >> 24) & 0xFF) * 2 + 20);
    kprintf("[usb] %s: OHCI, %d port%s\n", name, h->nports, h->nports == 1 ? "" : "s");
    nhc++;
    return 0;
}

/* The H618's host ports: clocks, resets, PHY, VBUS. n is 1..3. */
#define CCU 0x03001000u
static const uintptr_t ohci_base[4] = { 0x05101400u, 0x05200400u, 0x05310400u, 0x05311400u };
static const uintptr_t ehci_base[4] = { 0x05101000u, 0x05200000u, 0x05310000u, 0x05311000u };
static const uintptr_t pmu_base[4]  = { 0x05101800u, 0x05200800u, 0x05310800u, 0x05311800u };
void h6_gpio_out(int bank, int pin, int val);

static void h618_port_up(int n) {
    set32(CCU + 0xA70 + 4u * (uint32_t)n, (1u << 29) | (1u << 30) | (1u << 31));   /* PHY gate+reset, OHCI 12M */
    set32(CCU + 0xA8C, (1u << n) | (1u << (4 + n)));                              /* OHCI, EHCI bus gates */
    set32(CCU + 0xA8C, (1u << (16 + n)) | (1u << (20 + n)));                      /* ...and out of reset */
    if (n != 2) {                                                                 /* H616: PHY2 helps    */
        set32(CCU + 0xA78, (1u << 29) | (1u << 30));
        set32(CCU + 0xA8C, 1u << 6);
        clr32(pmu_base[2] + 0x10, 1u << 3);
    }
    delay_us(100);
    clr32(pmu_base[n] + 0x10, 1u << 3);                                           /* SIDDQ off           */
    set32(pmu_base[n], (1u << 10) | (1u << 9) | (1u << 8) | 1u);                  /* AHB passby          */
    /* EHCI: reset it, which leaves CONFIGFLAG clear and every port with OHCI. */
    uintptr_t op = ehci_base[n] + (r32(ehci_base[n]) & 0xFF);         /* CAPLENGTH */
    w32(op + 0x00, 1u << 1);                                                      /* HCRESET             */
    for (int i = 0; i < 1000 && (r32(op) & 2); i++) delay_us(10);
    w32(op + 0x40, 0);                                                            /* CONFIGFLAG = 0      */
}

uintptr_t qemu_pci_find(uint8_t cls, uint8_t sub, uint8_t progif, int *irq);

int usbh_init(void) {
    if (g_plat == PLAT_H618) {
        h6_gpio_out(2, 16, 1);                                     /* PC16: USB-A VBUS on */
        static const char *names[] = { "usb0", "USB-A port", "header USB2", "header USB3" };
        for (int n = 1; n <= 3; n++) { h618_port_up(n); hc_start(ohci_base[n], names[n]); }
    } else if (g_plat == PLAT_QEMU) {
        uintptr_t b = qemu_pci_find(0x0C, 0x03, 0x10, 0);
        if (b) hc_start(b, "QEMU OHCI");
    }
    return nhc;
}

void usbh_poll(void) {
    static uint64_t next_root;
    if (!nhc) return;
    for (int i = 0; i < nhc; i++) hw(&hcs[i], HC_INTSTAT, 1u << 1); /* WDH: keep the done queue moving */
    intr_poll();
    if (now_ms() >= next_root) {
        next_root = now_ms() + 100;
        for (int i = 0; i < nhc; i++) root_poll(i);
        for (int i = 0; i < MAX_DEV; i++) if (devs[i].used && devs[i].is_hub) hub_poll(i);
    }
}

/* /debug: the registers that say why a port is or is not working. */
int usbh_debug(char *out, int cap) {
    int n = 0;
    for (int i = 0; i < nhc; i++) {
        hc_t *h = &hcs[i];
        n += snprintf(out + n, (size_t)(cap - n), "%s @%lx: rev %x ctrl %x cmd %x int %x rhA %x rhB %x rhS %x fm %x\n",
                      h->name, (unsigned long)h->base, hr(h, 0), hr(h, HC_CONTROL), hr(h, HC_CMDSTAT), hr(h, HC_INTSTAT),
                      hr(h, HC_RHDESCA), hr(h, 0x4C), hr(h, HC_RHSTATUS), hr(h, 0x3C));
        for (int p = 0; p < h->nports; p++)
            n += snprintf(out + n, (size_t)(cap - n), "   port %d: %08x (dev %d)\n", p + 1, hr(h, HC_RHPORT(p)), h->port_dev[p]);
    }
    if (g_plat == PLAT_H618) {
        for (int k = 1; k <= 3; k++) {
            uintptr_t e = ehci_base[k], op = e + (r32(e) & 0xFF);
            n += snprintf(out + n, (size_t)(cap - n), "ehci%d: cap %08x cmd %08x sts %08x cfg %08x portsc %08x pmu %08x pmu+10 %08x usbclk %08x\n",
                          k, r32(e), r32(op), r32(op + 4), r32(op + 0x40), r32(op + 0x44),
                          r32(pmu_base[k]), r32(pmu_base[k] + 0x10), r32(CCU + 0xA70 + 4u * (uint32_t)k));
        }
        n += snprintf(out + n, (size_t)(cap - n), "ccu usb_bgr %08x  pio PC cfg2 %08x dat %08x\n",
                      r32(CCU + 0xA8C), r32(0x0300B000u + 2 * 0x24 + 8), r32(0x0300B000u + 2 * 0x24 + 0x10));
    }
    return n;
}

int usbh_device_list(char *out, int cap) {
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < MAX_DEV; i++) {
        dev_t *d = &devs[i];
        if (!d->used) continue;
        const char *what = d->is_hub ? "hub" : "device";
        for (int k = 0; k < MAX_INT; k++)
            if (ints[k].used && ints[k].dev == i) what = ints[k].kind == 1 ? "keyboard" : "mouse";
        n += snprintf(out + n, (size_t)(cap - n), "%s%-26s %04x:%04x  %s, %s\n",
                      d->parent >= 0 ? "  " : "", d->name, d->vid, d->pid, what,
                      d->low ? "low speed" : "full speed");
        if (n >= cap - 1) break;
    }
    return n;
}
