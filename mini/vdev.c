/* ===========================================================================
 *  BoltOS Mini  -  mini/vdev.c
 *
 *  The Firefox machine's devices (mini/vm.c is the machine). All virtio
 *  over MMIO, version 2, split rings, so the guest's stock drivers find them
 *  in its device tree:
 *
 *    0  disk     the root file system, read-only (store "fx/root.sqfs")
 *    1  disk     home: Firefox's profile, downloads, the fonts it fetched
 *    2  network  NAT through Mini's own IP stack (net.c): the guest is
 *                10.0.3.2 behind 10.0.3.1, and leaves as the Pi
 *    3  screen   virtio-gpu, 2D: the Browser copies what changed of the
 *                buffer the guest shows straight from the guest's memory,
 *                at most 60 times a second
 *    4  keyboard virtio-input
 *    5  tablet   virtio-input: absolute pointer, three buttons, wheels
 *    6  random   virtio-rng
 *
 *  Two cores share this. A vCPU core answers the guest's register reads
 *  and writes the moment it makes them (vdev_mmio); a write to QueueNotify
 *  just marks the queue. Everything that moves data -- the rings, the card,
 *  the network, the screen -- happens on core 0, in vdev_poll, from the
 *  main loop, which is the only core that may touch the card, the heap's
 *  allocations and the network.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "vm.h"
#include "store.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

#define NQ        2
#define QMAX      256
#define VERSION_1 (1ull << 32)

typedef struct {
    volatile uint32_t num, ready;
    volatile uint64_t desc, avail, used;
    uint16_t last;                         /* core 0: the next avail entry to take */
    volatile uint32_t kicked;
} vq_t;

struct vdev;
typedef void (*vd_fn)(struct vdev *d, int q);

typedef struct vdev {
    int       type, slot;
    uint64_t  features, drv;
    uint32_t  fsel, dsel, qsel;
    vq_t      q[NQ];
    volatile uint32_t status, isr, gen;
    uint8_t   cfg[256];
    int       cfg_len;
    vd_fn     run;                         /* core 0: a queue has work              */
    void    (*cfg_write)(struct vdev *d, uint32_t off, uint32_t v);
    volatile int kicked;
    const store_entry_t *disk; int ro;
} vdev_t;

enum { D_ROOT, D_HOME, D_NET, D_GPU, D_KBD, D_TAB, D_RNG, ND };
static vdev_t dev[ND];
static int ndev;
static int on;

int vdev_count(void) { return on ? ndev : 0; }

/* ---- the rings ---------------------------------------------------------------- */
typedef struct { uint8_t *p; uint32_t len; int wr; } seg_t;
typedef struct { int head, n; seg_t s[64]; uint32_t rlen, wlen; } chain_t;

struct vdesc { uint64_t addr; uint32_t len; uint16_t flags, next; };

static int vq_pop(vdev_t *d, int qi, chain_t *c) {
    vq_t *q = &d->q[qi];
    if (!q->ready || !q->num) return 0;
    uint16_t *av = vm_ptr(q->avail, 4 + 2 * q->num);
    struct vdesc *ds = vm_ptr(q->desc, 16ull * q->num);
    if (!av || !ds) return 0;
    uint16_t idx = __atomic_load_n(&av[1], __ATOMIC_ACQUIRE);
    if (idx == q->last) return 0;
    __asm__ volatile("dmb ish" ::: "memory");
    int head = av[2 + q->last % q->num];
    q->last++;
    c->head = head; c->n = 0; c->rlen = c->wlen = 0;
    for (int i = head, guard = 0; guard < (int)q->num && c->n < 64; guard++) {
        struct vdesc *e = &ds[i % q->num];
        uint8_t *p = vm_ptr(e->addr, e->len ? e->len : 1);
        if (!p) return -1;
        c->s[c->n].p = p; c->s[c->n].len = e->len; c->s[c->n].wr = e->flags & 2;
        if (e->flags & 2) c->wlen += e->len; else c->rlen += e->len;
        c->n++;
        if (!(e->flags & 1)) break;
        i = e->next;
    }
    return 1;
}

static void vq_push(vdev_t *d, int qi, int head, uint32_t len) {
    vq_t *q = &d->q[qi];
    uint8_t *u = vm_ptr(q->used, 4 + 8ull * q->num);
    if (!u) return;
    uint16_t idx = *(volatile uint16_t *)(u + 2);
    uint32_t *e = (uint32_t *)(u + 4 + 8 * (idx % q->num));
    e[0] = (uint32_t)head; e[1] = len;
    __asm__ volatile("dmb ish" ::: "memory");
    __atomic_store_n((uint16_t *)(u + 2), (uint16_t)(idx + 1), __ATOMIC_RELEASE);
}

static void vd_irq(vdev_t *d) {
    __atomic_fetch_or(&d->isr, 1u, __ATOMIC_RELEASE);
    vm_irq(VM_SPI_VIRTIO + d->slot);
}

/* Bytes from a chain's readable part, from its start. */
static uint32_t chain_read(const chain_t *c, void *out, uint32_t cap) {
    uint32_t o = 0;
    for (int i = 0; i < c->n && o < cap; i++) {
        if (c->s[i].wr) continue;
        uint32_t k = c->s[i].len < cap - o ? c->s[i].len : cap - o;
        memcpy((uint8_t *)out + o, c->s[i].p, k);
        o += k;
    }
    return o;
}
/* Bytes into a chain's writable part, from its start. */
static uint32_t chain_write(const chain_t *c, const void *in, uint32_t len) {
    uint32_t o = 0;
    for (int i = 0; i < c->n && o < len; i++) {
        if (!c->s[i].wr) continue;
        uint32_t k = c->s[i].len < len - o ? c->s[i].len : len - o;
        memcpy(c->s[i].p, (const uint8_t *)in + o, k);
        o += k;
    }
    return o;
}

/* ---- the registers (a vCPU core) ------------------------------------------------- */
int vdev_mmio(int i, uint32_t off, int wr, int size, uint64_t *val) {
    if (i < 0 || i >= ndev) return -1;
    vdev_t *d = &dev[i];
    vq_t *q = &d->q[d->qsel < NQ ? d->qsel : 0];
    uint32_t v = (uint32_t)*val;
    if (off >= 0x100) {
        uint32_t o = off - 0x100;
        if (wr) { if (d->cfg_write) d->cfg_write(d, o, v); return 0; }
        uint64_t r = 0;
        for (int k = 0; k < size; k++) if (o + (uint32_t)k < sizeof d->cfg) r |= (uint64_t)d->cfg[o + (uint32_t)k] << (8 * k);
        *val = r;
        return 0;
    }
    if (!wr) {
        uint64_t r = 0;
        switch (off) {
        case 0x000: r = 0x74726976; break;
        case 0x004: r = 2; break;
        case 0x008: r = (uint64_t)d->type; break;
        case 0x00C: r = 0x544C4F42; break;                       /* "BOLT" */
        case 0x010: r = d->fsel == 0 ? (uint32_t)d->features : d->fsel == 1 ? (uint32_t)(d->features >> 32) : 0; break;
        case 0x034: r = d->qsel < NQ ? QMAX : 0; break;
        case 0x044: r = q->ready; break;
        case 0x060: r = d->isr; break;
        case 0x070: r = d->status; break;
        case 0x0FC: r = d->gen; break;
        case 0x0B0: case 0x0B4: case 0x0B8: case 0x0BC: r = 0xFFFFFFFF; break;   /* no shared memory regions */
        }
        *val = r;
        return 0;
    }
    switch (off) {
    case 0x014: d->fsel = v; break;
    case 0x020: if (d->dsel == 0) d->drv = (d->drv & ~0xFFFFFFFFull) | v; else if (d->dsel == 1) d->drv = (d->drv & 0xFFFFFFFFull) | (uint64_t)v << 32; break;
    case 0x024: d->dsel = v; break;
    case 0x030: d->qsel = v; break;
    case 0x038: if (d->qsel < NQ) q->num = v > QMAX ? QMAX : v; break;
    case 0x044:
        if (d->qsel < NQ) { if (v) q->last = 0; __atomic_store_n(&q->ready, v & 1, __ATOMIC_RELEASE); }
        break;
    case 0x050:
        if (v < NQ) { d->q[v].kicked = 1; __atomic_store_n(&d->kicked, 1, __ATOMIC_RELEASE); }
        break;
    case 0x064: __atomic_fetch_and(&d->isr, ~v, __ATOMIC_RELEASE); break;
    case 0x070:
        d->status = v;
        if (!v) { for (int k = 0; k < NQ; k++) { d->q[k].ready = 0; d->q[k].last = 0; d->q[k].num = 0; } d->isr = 0; d->drv = 0; }
        break;
    case 0x080: if (d->qsel < NQ) q->desc  = (q->desc  & ~0xFFFFFFFFull) | v; break;
    case 0x084: if (d->qsel < NQ) q->desc  = (q->desc  & 0xFFFFFFFFull) | (uint64_t)v << 32; break;
    case 0x090: if (d->qsel < NQ) q->avail = (q->avail & ~0xFFFFFFFFull) | v; break;
    case 0x094: if (d->qsel < NQ) q->avail = (q->avail & 0xFFFFFFFFull) | (uint64_t)v << 32; break;
    case 0x0A0: if (d->qsel < NQ) q->used  = (q->used  & ~0xFFFFFFFFull) | v; break;
    case 0x0A4: if (d->qsel < NQ) q->used  = (q->used  & 0xFFFFFFFFull) | (uint64_t)v << 32; break;
    }
    return 0;
}

static void put_le(uint8_t *p, uint64_t v, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

/* ---- disks -------------------------------------------------------------------------- */
static uint8_t *bounce;                    /* 64 KiB, for pieces the card can't take straight */
static uint64_t blk_reads, blk_writes;

static int blk_io(vdev_t *d, int wr, uint64_t off, uint8_t *p, uint32_t len) {
    while (len) {
        int direct = !((uintptr_t)p & 3) && len >= 512;
        uint32_t k = direct ? (len & ~511u) : (len < 512 ? len : 512);
        if (k > (1u << 20)) k = 1u << 20;
        if (off + k > d->disk->sectors * 512ull) return -1;
        if (direct) {
            if (wr ? store_write(d->disk, off, p, k) : store_read(d->disk, off, p, k)) return -1;
        } else {
            if (store_read(d->disk, off & ~511ull, bounce, 512)) return -1;
            uint32_t at = (uint32_t)(off & 511);
            if (k > 512 - at) k = 512 - at;
            if (wr) { memcpy(bounce + at, p, k); if (store_write(d->disk, off & ~511ull, bounce, 512)) return -1; }
            else memcpy(p, bounce + at, k);
        }
        if (wr) blk_writes += k; else blk_reads += k;
        off += k; p += k; len -= k;
    }
    return 0;
}

static void blk_run(vdev_t *d, int qi) {
    chain_t c;
    uint64_t end = now_ms() + 30;                  /* the rest next turn: keep the screen alive */
    while (now_ms() < end && vq_pop(d, qi, &c) > 0) {
        uint8_t hdr[16] = { 0 };
        uint8_t st = 0;
        if (c.n < 2 || chain_read(&c, hdr, 16) < 16 || !c.s[c.n - 1].wr) { vq_push(d, qi, c.head, 0); continue; }
        uint32_t type = le32(hdr);
        uint64_t off = le64(hdr + 8) * 512;
        uint32_t done = 0;
        if (type == 0 || type == 1) {                       /* IN, OUT */
            if (type == 1 && d->ro) st = 1;
            for (int i = 1; i < c.n && !st; i++) {
                seg_t *s = &c.s[i];
                uint32_t len = s->len;
                if (i == c.n - 1) { if (len <= 1) break; len -= 1; }  /* the status byte */
                if (blk_io(d, type == 1, off, s->p, len)) st = 1;
                off += len; if (type == 0) done += len;
            }
        } else if (type == 4) st = 0;                       /* FLUSH: the card already has it */
        else if (type == 8) {                               /* GET_ID */
            char id[20] = { 0 };
            snprintf(id, sizeof id, "boltos-%s", d->ro ? "root" : "home");
            if (c.n >= 3 && c.s[1].wr) { memcpy(c.s[1].p, id, c.s[1].len < 20 ? c.s[1].len : 20); done = 20; }
        } else st = 2;                                      /* UNSUPP */
        seg_t *ss = &c.s[c.n - 1];
        ss->p[ss->len - 1] = st;
        vq_push(d, qi, c.head, done + 1);
        vd_irq(d);
    }
    if (vq_pop(d, qi, &c) == 0) return;
    d->q[qi].last--;                                         /* left for next time */
    d->q[qi].kicked = 1; d->kicked = 1;
}

static void blk_init(vdev_t *d, const store_entry_t *e, int ro) {
    d->type = 2; d->disk = e; d->ro = ro;
    d->features = VERSION_1 | (ro ? (1ull << 5) : (1ull << 9));    /* RO, or FLUSH */
    put_le(d->cfg, e->sectors, 8);                                  /* capacity      */
    d->cfg_len = 24;
    d->run = blk_run;
}

/* ---- random ----------------------------------------------------------------------------- */
static uint64_t rs[2];
static uint64_t rnd(void) {
    uint64_t s1 = rs[0], s0 = rs[1];
    rs[0] = s0; s1 ^= s1 << 23; rs[1] = s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26);
    return rs[1] + s0;
}
static void rng_run(vdev_t *d, int qi) {
    chain_t c;
    while (vq_pop(d, qi, &c) > 0) {
        uint32_t n = 0;
        for (int i = 0; i < c.n; i++) if (c.s[i].wr)
            for (uint32_t k = 0; k < c.s[i].len; k++) {
                if (!(k & 7)) { rs[0] ^= now_us() * 0x9E3779B97F4A7C15ull ^ mini_rand(); }
                c.s[i].p[k] = (uint8_t)(rnd() >> (8 * (k & 7))); n++;
            }
        vq_push(d, qi, c.head, n);
    }
    vd_irq(d);
}

/* ---- input --------------------------------------------------------------------------------- */
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
static struct { uint16_t t, c; int32_t v; } evq[2][256];
static int evn[2];
static int pw = 1024, ph = 576;                 /* the screen, for the tablet's scale */
static int last_btn;

static void ev(int dv, int t, int c, int v) {
    if (evn[dv] >= 256) return;
    evq[dv][evn[dv]].t = (uint16_t)t; evq[dv][evn[dv]].c = (uint16_t)c; evq[dv][evn[dv]].v = v;
    evn[dv]++;
}

/* Linux's evdev keeps 64 events for each reader, and when a burst overflows
 * that it drops the oldest: typed-in text (a paste, /keys) lost its start.
 * So the keyboard's events go in at a fast typist's pace, four (a key down
 * and up) every 6 ms, which the guest takes in even while it is busy. */
static uint64_t kbd_at;

static void input_flush(int dv) {
    vdev_t *d = &dev[D_KBD + dv];
    chain_t c;
    int sent = 0, k = 0, max = evn[dv];
    if (dv == 0) {
        uint64_t t = now_us();
        if (t - kbd_at < 6000) return;
        kbd_at = t;
        if (max > 4) max = 4;
    }
    while (k < max && vq_pop(d, 0, &c) > 0) {
        uint8_t e[8];
        put_le(e, evq[dv][k].t, 2); put_le(e + 2, evq[dv][k].c, 2); put_le(e + 4, (uint32_t)evq[dv][k].v, 4);
        chain_write(&c, e, 8);
        vq_push(d, 0, c.head, 8);
        k++; sent = 1;
    }
    if (k) { memmove(evq[dv], evq[dv] + k, (size_t)(evn[dv] - k) * sizeof evq[dv][0]); evn[dv] -= k; }
    if (sent) vd_irq(d);
}

static void input_run(vdev_t *d, int qi) {
    if (qi == 1) {                                  /* LEDs and the like: noted, ignored */
        chain_t c;
        int any = 0;
        while (vq_pop(d, 1, &c) > 0) { vq_push(d, 1, c.head, 0); any = 1; }
        if (any) vd_irq(d);
        return;
    }
    input_flush(d == &dev[D_KBD] ? 0 : 1);
}

/* The config space answers whatever select/subsel the driver last wrote. */
static void set_bit(uint8_t *b, int n) { b[n / 8] |= (uint8_t)(1u << (n % 8)); }
static void input_cfg(vdev_t *d, uint32_t off, uint32_t v) {
    if (off > 1) return;
    d->cfg[off] = (uint8_t)v;
    uint8_t sel = d->cfg[0], sub = d->cfg[1], *u = d->cfg + 8, size = 0;
    memset(u, 0, 128);
    int kbd = d == &dev[D_KBD];
    switch (sel) {
    case 0x01: { const char *nm = kbd ? "BoltOS Keyboard" : "BoltOS Tablet"; size = (uint8_t)strlen(nm); memcpy(u, nm, size); break; }
    case 0x03: put_le(u, 0x06, 2); put_le(u + 2, 0x0627, 2); put_le(u + 4, kbd ? 1 : 2, 2); put_le(u + 6, 1, 2); size = 8; break;
    case 0x11:
        if (kbd && sub == EV_KEY) { for (int k = 1; k < 249; k++) set_bit(u, k); size = 32; }
        else if (kbd && sub == 0x14) size = 0;                         /* EV_REP */
        else if (!kbd && sub == EV_KEY) { set_bit(u, 0x110); set_bit(u, 0x111); set_bit(u, 0x112); size = 35; }
        else if (!kbd && sub == EV_REL) { set_bit(u, 6); set_bit(u, 8); size = 2; }
        else if (!kbd && sub == EV_ABS) { set_bit(u, 0); set_bit(u, 1); size = 1; }
        break;
    case 0x12:
        if (!kbd && sub <= 1) { put_le(u, 0, 4); put_le(u + 4, 32767, 4); size = 20; }
        break;
    }
    d->cfg[2] = size;
}

/* Mini's keys to Linux's, US layout; shifted characters say so. */
static int keycode(int key, int ch, int *shift) {
    static const char row[] = "1234567890-=";
    static const char sh_row[] = "!@#$%^&*()_+";
    static const uint8_t letters[26] = { 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44 };
    static const struct { char c, s; uint8_t k; } punct[] = {
        { '[', '{', 26 }, { ']', '}', 27 }, { ';', ':', 39 }, { '\'', '"', 40 }, { '`', '~', 41 },
        { '\\', '|', 43 }, { ',', '<', 51 }, { '.', '>', 52 }, { '/', '?', 53 } };
    *shift = 0;
    switch (key) {
    case KEY_ENTER: return 28;  case KEY_ESC: return 1;      case KEY_BACKSPACE: return 14;
    case KEY_TAB: return 15;    case KEY_DELETE: return 111; case KEY_INSERT: return 110;
    case KEY_UP: return 103;    case KEY_DOWN: return 108;   case KEY_LEFT: return 105;
    case KEY_RIGHT: return 106; case KEY_HOME: return 102;   case KEY_END: return 107;
    case KEY_PGUP: return 104;  case KEY_PGDN: return 109;   case KEY_SUPER: return 125;
    case KEY_F11: return 87;    case KEY_F12: return 88;
    }
    if (key >= KEY_F1 && key <= KEY_F10) return 59 + (key - KEY_F1);
    if (key != KEY_CHAR) return 0;
    if (ch >= 'a' && ch <= 'z') return letters[ch - 'a'];
    if (ch >= 'A' && ch <= 'Z') { *shift = 1; return letters[ch - 'A']; }
    if (ch == ' ') return 57;
    for (int i = 0; i < 12; i++) {
        if (ch == row[i]) return 2 + i;
        if (ch == sh_row[i]) { *shift = 1; return 2 + i; }
    }
    for (unsigned i = 0; i < sizeof punct / sizeof punct[0]; i++) {
        if (ch == punct[i].c) return punct[i].k;
        if (ch == punct[i].s) { *shift = 1; return punct[i].k; }
    }
    return 0;
}

void vinput_key(int key, int ch, int mods) {
    if (!on) return;
    int shift, code = keycode(key, ch, &shift);
    if (!code) return;
    static const struct { int bit, code; } m[] = { { MOD_CTRL, 29 }, { MOD_ALT, 56 }, { MOD_SUPER, 125 }, { MOD_SHIFT, 42 } };
    int sh = shift || (mods & MOD_SHIFT);
    for (int i = 0; i < 4; i++) if ((m[i].bit == MOD_SHIFT ? sh : mods & m[i].bit) && m[i].code != code) { ev(0, EV_KEY, m[i].code, 1); ev(0, EV_SYN, 0, 0); }
    ev(0, EV_KEY, code, 1); ev(0, EV_SYN, 0, 0);
    ev(0, EV_KEY, code, 0); ev(0, EV_SYN, 0, 0);
    for (int i = 3; i >= 0; i--) if ((m[i].bit == MOD_SHIFT ? sh : mods & m[i].bit) && m[i].code != code) { ev(0, EV_KEY, m[i].code, 0); ev(0, EV_SYN, 0, 0); }
    input_flush(0);
}

void vinput_pointer(int x, int y, int buttons, int wheel) {
    if (!on) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= pw) x = pw - 1;
    if (y >= ph) y = ph - 1;
    ev(1, EV_ABS, 0, (int)((long)x * 32767 / (pw - 1)));
    ev(1, EV_ABS, 1, (int)((long)y * 32767 / (ph - 1)));
    static const int btn[3] = { 0x110, 0x111, 0x112 };
    for (int i = 0; i < 3; i++) if ((buttons ^ last_btn) >> i & 1) ev(1, EV_KEY, btn[i], buttons >> i & 1);
    last_btn = buttons;
    if (wheel) ev(1, EV_REL, 8, wheel);
    ev(1, EV_SYN, 0, 0);
    input_flush(1);
}

static void input_init(vdev_t *d) {
    d->type = 18; d->features = VERSION_1; d->run = input_run; d->cfg_write = input_cfg;
    d->cfg_len = 136;
}

/* ---- the screen ------------------------------------------------------------------------------ */
#define GPU_RES 16
typedef struct {
    uint32_t id, fmt, w, h;
    uint32_t *px;                                   /* the host's copy: cursors only */
    uint32_t nent; uint64_t *addr; uint32_t *len; uint64_t *cum;
} gres_t;
static gres_t res[GPU_RES];
static uint32_t scanout;                            /* resource shown, 0 none     */
static int dmg_x0 = 1 << 30, dmg_y0 = 1 << 30, dmg_x1, dmg_y1;
static uint32_t cur_px[64 * 64]; static int cur_w, cur_h, cur_hx, cur_hy; static uint32_t cur_gen;
static uint8_t *greq;                               /* a command, gathered        */
static uint64_t gpu_flushes, gpu_xfer;

static gres_t *res_find(uint32_t id) { for (int i = 0; i < GPU_RES; i++) if (res[i].id && res[i].id == id) return &res[i]; return 0; }
static void res_detach(gres_t *r) {
    if (r->addr) kfree(r->addr);
    r->addr = 0; r->len = 0; r->cum = 0; r->nent = 0;
}
static void res_free(gres_t *r) { res_detach(r); if (r->px) kfree(r->px); memset(r, 0, sizeof *r); }

/* A run of bytes of a resource's backing, from wherever in the guest they are. */
static void backing_read(gres_t *r, uint64_t off, uint8_t *dst, uint32_t n) {
    uint32_t lo = 0, hi = r->nent;
    while (n && r->nent) {
        while (lo + 1 < hi) { uint32_t m = (lo + hi) / 2; if (r->cum[m] <= off) lo = m; else hi = m; }
        uint32_t i = lo;
        if (off < r->cum[i] || off >= r->cum[i] + r->len[i]) return;
        uint32_t in = (uint32_t)(r->cum[i] + r->len[i] - off);
        if (in > n) in = n;
        uint8_t *s = vm_ptr(r->addr[i] + (off - r->cum[i]), in);
        if (!s) return;
        memcpy(dst, s, in);
        dst += in; off += in; n -= in;
        lo = i; hi = r->nent;
    }
}

static void gpu_cmd(vdev_t *d, int qi, const chain_t *c) {
    uint32_t n = chain_read(c, greq, 65536);
    uint8_t resp[408];
    memset(resp, 0, sizeof resp);
    uint32_t rlen = 24, type = n >= 24 ? le32(greq) : 0, rtype = 0x1100;     /* OK_NODATA */
    const uint8_t *b = greq + 24;
    switch (type) {
    case 0x0100: {                                                           /* GET_DISPLAY_INFO */
        rtype = 0x1101; rlen = 408;
        put_le(resp + 24 + 8, (uint32_t)pw, 4); put_le(resp + 24 + 12, (uint32_t)ph, 4); put_le(resp + 24 + 16, 1, 4);
        break;
    }
    case 0x0101: {                                                           /* RESOURCE_CREATE_2D */
        uint32_t id = le32(b), fmt = le32(b + 4), w = le32(b + 8), h = le32(b + 12);
        gres_t *r = 0;
        for (int i = 0; i < GPU_RES && !r; i++) if (!res[i].id) r = &res[i];
        if (!r || !id || w > 4096 || h > 4096 || res_find(id)) { rtype = 0x1200; break; }
        /* Only a cursor needs a copy here; a picture is read where the guest
         * keeps it, when it is shown (vgpu_copy). */
        if (w * h <= 64 * 64) {
            r->px = kmalloc((uint64_t)w * h * 4 + 4);
            if (!r->px) { rtype = 0x1201; break; }                            /* OUT_OF_MEMORY */
            memset(r->px, 0, (size_t)w * h * 4);
        }
        r->id = id; r->fmt = fmt; r->w = w; r->h = h;
        break;
    }
    case 0x0102: { gres_t *r = res_find(le32(b)); if (r) { if (scanout == r->id) scanout = 0; res_free(r); } break; }
    case 0x0103: {                                                           /* SET_SCANOUT */
        uint32_t id = le32(b + 20);
        scanout = res_find(id) ? id : 0;
        dmg_x0 = 0; dmg_y0 = 0; dmg_x1 = pw; dmg_y1 = ph;
        break;
    }
    case 0x0104: {                                                           /* RESOURCE_FLUSH */
        uint32_t x = le32(b), y = le32(b + 4), w = le32(b + 8), h = le32(b + 12), id = le32(b + 16);
        gpu_flushes++;
        if (id == scanout) {
            if ((int)x < dmg_x0) dmg_x0 = (int)x;
            if ((int)y < dmg_y0) dmg_y0 = (int)y;
            if ((int)(x + w) > dmg_x1) dmg_x1 = (int)(x + w);
            if ((int)(y + h) > dmg_y1) dmg_y1 = (int)(y + h);
        }
        break;
    }
    case 0x0105: {                                                           /* TRANSFER_TO_HOST_2D */
        uint32_t x = le32(b), y = le32(b + 4), w = le32(b + 8), h = le32(b + 12);
        uint64_t off = le64(b + 16);
        gres_t *r = res_find(le32(b + 24));
        if (!r || !r->nent || x + w > r->w || y + h > r->h) { rtype = 0x1200; break; }
        if (!r->px) break;
        for (uint32_t j = 0; j < h; j++) {
            uint32_t *row = r->px + (y + j) * r->w + x;
            backing_read(r, off + (uint64_t)j * r->w * 4, (uint8_t *)row, w * 4);
            if (r->fmt == 2 || r->fmt == 4) for (uint32_t i = 0; i < w; i++) row[i] |= 0xFF000000u;   /* X means opaque */
        }
        break;
    }
    case 0x0106: {                                                           /* ATTACH_BACKING */
        gres_t *r = res_find(le32(b));
        uint32_t ne = le32(b + 4);
        if (!r || ne > 65536 || 32 + 16 * ne > n) { rtype = 0x1200; break; }
        res_detach(r);
        r->addr = kmalloc((uint64_t)ne * 20 + 8 * (uint64_t)ne);
        if (!r->addr) { rtype = 0x1201; break; }
        r->len = (uint32_t *)(r->addr + ne); r->cum = (uint64_t *)(r->len + ne + (ne & 1));
        uint64_t sum = 0;
        for (uint32_t i = 0; i < ne; i++) {
            r->addr[i] = le64(b + 8 + 16 * i); r->len[i] = le32(b + 8 + 16 * i + 8);
            r->cum[i] = sum; sum += r->len[i];
        }
        r->nent = ne;
        break;
    }
    case 0x0107: { gres_t *r = res_find(le32(b)); if (r) res_detach(r); break; }   /* DETACH_BACKING */
    case 0x0300: case 0x0301: {                                              /* UPDATE_ / MOVE_CURSOR */
        if (type == 0x0300) {
            gres_t *r = res_find(le32(b + 16));
            if (r && r->w <= 64 && r->h <= 64) {
                cur_w = (int)r->w; cur_h = (int)r->h; cur_hx = (int)le32(b + 20); cur_hy = (int)le32(b + 24);
                memcpy(cur_px, r->px, (size_t)r->w * r->h * 4);
            } else cur_w = cur_h = 0;
            cur_gen++;
        }
        break;
    }
    default: rtype = 0x1200; break;                                          /* ERR_UNSPEC */
    }
    put_le(resp, rtype, 4);
    if (n >= 24 && (le32(greq + 4) & 1)) { put_le(resp + 4, 1, 4); memcpy(resp + 8, greq + 8, 16); }   /* the fence */
    uint32_t w = chain_write(c, resp, rlen);
    vq_push(d, qi, c->head, w);
}

/* A real screen takes a picture per refresh, and the guest waits for each
 * flush before it draws the next: answered at once, Firefox drew 200 a
 * second. 60 a second, as a real one does: with only the damage read (the
 * guest's kernel keeps it across page flips, mini/firefox/build.sh), a frame
 * costs core 0 what changed in it. */
#define FRAME_US 16667
static uint64_t flush_at;

static void gpu_run(vdev_t *d, int qi) {
    chain_t c;
    int any = 0;
    while (vq_pop(d, qi, &c) > 0) {
        uint8_t h[4];
        if (qi == 0 && chain_read(&c, h, 4) == 4 && le32(h) == 0x0104) {          /* RESOURCE_FLUSH */
            uint64_t t = now_us();
            if (t - flush_at < FRAME_US) { d->q[qi].last--; d->q[qi].kicked = 1; d->kicked = 1; break; }
            flush_at = t;
        }
        gpu_cmd(d, qi, &c); any = 1;
    }
    if (any) vd_irq(d);
}

static void gpu_init(vdev_t *d) {
    d->type = 16; d->features = VERSION_1; d->run = gpu_run;
    put_le(d->cfg + 8, 1, 4);                                    /* num_scanouts */
    d->cfg_len = 16;
}

static gres_t *shown(void) {
    gres_t *r = on && scanout ? res_find(scanout) : 0;
    return r && r->nent ? r : 0;
}

int vgpu_size(int *w, int *h) {
    gres_t *r = shown();
    if (!r) return 0;
    if (w) *w = (int)r->w < pw ? (int)r->w : pw;
    if (h) *h = (int)r->h < ph ? (int)r->h : ph;
    return 1;
}

/* A row at a time out of the guest's memory, compared with what dst has:
 * only the pixels between a row's first and last difference are written,
 * and the rectangle shrinks to the rows and columns that changed. */
int vgpu_copy(uint32_t *dst, int stride, int *x, int *y, int *w, int *h) {
    static uint32_t row[4096];
    gres_t *r = shown();
    int sw, sh;
    if (!r || !vgpu_size(&sw, &sh)) return 0;
    int x0 = *x < 0 ? 0 : *x, y0 = *y < 0 ? 0 : *y, x1 = *x + *w, y1 = *y + *h;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    int n = x1 - x0, cx0 = n, cx1 = 0, cy0 = -1, cy1 = 0;
    if (n <= 0 || y1 <= y0) return 0;
    for (int j = y0; j < y1; j++) {
        backing_read(r, ((uint64_t)j * r->w + (uint64_t)x0) * 4, (uint8_t *)row, (uint32_t)n * 4);
        for (int i = 0; i < n; i++) row[i] |= 0xFF000000u;                  /* X: opaque */
        uint32_t *d = dst + (size_t)j * stride + x0;
        if (!memcmp(d, row, (size_t)n * 4)) continue;
        int a = 0, b = n;
        while (d[a] == row[a]) a++;
        while (d[b - 1] == row[b - 1]) b--;
        memcpy(d + a, row + a, (size_t)(b - a) * 4);
        if (cy0 < 0) cy0 = j;
        cy1 = j + 1;
        if (a < cx0) cx0 = a;
        if (b > cx1) cx1 = b;
    }
    gpu_xfer += (uint64_t)n * (uint64_t)(y1 - y0) * 4;
    if (cy0 < 0) return 0;
    *x = x0 + cx0; *w = cx1 - cx0; *y = cy0; *h = cy1 - cy0;
    return 1;
}

int vgpu_damage(int *x, int *y, int *w, int *h) {
    if (dmg_x1 <= dmg_x0 || dmg_y1 <= dmg_y0) return 0;
    *x = dmg_x0; *y = dmg_y0; *w = (dmg_x1 < pw ? dmg_x1 : pw) - dmg_x0; *h = (dmg_y1 < ph ? dmg_y1 : ph) - dmg_y0;
    dmg_x0 = dmg_y0 = 1 << 30; dmg_x1 = dmg_y1 = 0;
    return *w > 0 && *h > 0;
}

const uint32_t *vgpu_cursor(int *w, int *h, int *hx, int *hy, uint32_t *gen) {
    *w = cur_w; *h = cur_h; *hx = cur_hx; *hy = cur_hy; *gen = cur_gen;
    return cur_w ? cur_px : 0;
}

/* ---- the network --------------------------------------------------------------------------------- *
 *  The guest is 10.0.3.2 behind a router at 10.0.3.1 that is this file. Its
 *  TCP, UDP and pings leave as the Pi, from a port of 40000-47999; replies
 *  to those ports come back in through net_nat_in.                          */
#define GW_IP     0x0A000301u
#define GUEST_IP  0x0A000302u
#define NAT_LO    40000
#define NAT_N     1024
static const uint8_t gw_mac[6] = { 0x02, 0xB0, 0x17, 0x00, 0x03, 0x01 };
static const uint8_t guest_mac[6] = { 0x02, 0xB0, 0x17, 0x00, 0x03, 0x02 };
typedef struct { uint8_t proto, fin; uint16_t gport, rport; uint32_t rip; uint64_t last; } nat_t;
static nat_t nat[NAT_N];                          /* nat[i] owns port NAT_LO + i      */
static struct { uint8_t f[1600]; int len; } rxq[64];
static int rxn;
static uint64_t net_out, net_in, net_drop;

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void s16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint32_t g32(const uint8_t *p) { return (uint32_t)g16(p) << 16 | g16(p + 2); }
static void s32(uint8_t *p, uint32_t v) { s16(p, (uint16_t)(v >> 16)); s16(p + 2, (uint16_t)v); }
static uint32_t csum_add(uint32_t s, const uint8_t *p, int n) {
    for (; n > 1; n -= 2, p += 2) s += (uint32_t)(p[0] << 8 | p[1]);
    if (n) s += (uint32_t)p[0] << 8;
    return s;
}
static uint16_t csum_fold(uint32_t s) { while (s >> 16) s = (s & 0xFFFF) + (s >> 16); return (uint16_t)~s; }

/* IP header and TCP/UDP/ICMP checksums, after addresses or ports changed. */
static void ip_fix(uint8_t *ip) {
    int ihl = (ip[0] & 15) * 4, tot = g16(ip + 2), pl = tot - ihl;
    uint8_t *p = ip + ihl;
    s16(ip + 10, 0); s16(ip + 10, csum_fold(csum_add(0, ip, ihl)));
    int at = ip[9] == 6 ? 16 : ip[9] == 17 ? 6 : ip[9] == 1 ? 2 : -1;
    if (at < 0 || pl < at + 2) return;
    if (ip[9] == 17 && !g16(p + 6)) return;                      /* UDP with no checksum */
    s16(p + at, 0);
    uint32_t s = ip[9] == 1 ? 0 : csum_add(csum_add(0, ip + 12, 8), (const uint8_t[]){ 0, ip[9] }, 2) + (uint32_t)pl;
    uint16_t v = csum_fold(csum_add(s, p, pl));
    if (ip[9] == 17 && !v) v = 0xFFFF;
    s16(p + at, v);
}

static void to_guest(const uint8_t *ip, int len) {
    if (rxn >= 64 || len > 1500) { net_drop++; return; }
    uint8_t *f = rxq[rxn].f;
    memcpy(f, guest_mac, 6); memcpy(f + 6, gw_mac, 6); s16(f + 12, 0x0800);
    memcpy(f + 14, ip, (size_t)len);
    rxq[rxn].len = 14 + len;
    rxn++;
}

static void net_flush(void) {
    vdev_t *d = &dev[D_NET];
    chain_t c;
    int k = 0;
    while (k < rxn && vq_pop(d, 0, &c) > 0) {
        uint8_t h[12] = { 0 };
        h[10] = 1;                                                 /* num_buffers */
        uint8_t tmp[1612];
        memcpy(tmp, h, 12); memcpy(tmp + 12, rxq[k].f, (size_t)rxq[k].len);
        uint32_t w = chain_write(&c, tmp, (uint32_t)(12 + rxq[k].len));
        vq_push(d, 0, c.head, w);
        k++; net_in++;
    }
    if (k) { memmove(rxq, rxq + k, (size_t)(rxn - k) * sizeof rxq[0]); rxn -= k; vd_irq(d); }
}

/* A reply to one of the guest's ports. */
static int nat_in(netif_t *nif, const uint8_t *ip, int len) {
    (void)nif;
    if (!on) return 0;
    int ihl = (ip[0] & 15) * 4, proto = ip[9];
    const uint8_t *p = ip + ihl;
    int port;
    if (proto == 6 || proto == 17) { if (len < ihl + 8) return 0; port = g16(p + 2); }
    else if (proto == 1) { if (len < ihl + 8 || p[0] != 0) return 0; port = g16(p + 4); }      /* echo reply */
    else return 0;
    if (port < NAT_LO || port >= NAT_LO + NAT_N) return 0;
    nat_t *e = &nat[port - NAT_LO];
    if (!e->proto || e->proto != proto || e->rip != g32(ip + 12)) return 0;
    if (proto != 1 && e->rport != g16(p)) return 0;
    uint8_t buf[1600];
    if (len > (int)sizeof buf) return 1;
    memcpy(buf, ip, (size_t)len);
    s32(buf + 16, GUEST_IP);
    if (proto == 1) s16(buf + ihl + 4, e->gport); else s16(buf + ihl + 2, e->gport);
    if (proto == 6 && (p[13] & 0x05)) e->fin = 1;                     /* FIN or RST */
    e->last = now_ms();
    ip_fix(buf);
    to_guest(buf, len);
    net_flush();
    return 1;
}

static int nat_slot(int proto, uint16_t gport, uint32_t rip, uint16_t rport) {
    uint64_t now = now_ms();
    int free_i = -1, old_i = 0;
    for (int i = 0; i < NAT_N; i++) {
        nat_t *e = &nat[i];
        if (e->proto == proto && e->gport == gport && e->rip == rip && (proto == 1 || e->rport == rport)) return i;
        uint64_t idle = now - e->last;
        int dead = !e->proto || (e->fin && idle > 30000) || idle > (proto == 6 ? 900000u : 120000u);
        if (dead && free_i < 0) free_i = i;
        if (e->last < nat[old_i].last) old_i = i;
    }
    int i = free_i >= 0 ? free_i : old_i;
    nat[i].proto = (uint8_t)proto; nat[i].gport = gport; nat[i].rip = rip; nat[i].rport = rport; nat[i].fin = 0;
    return i;
}

/* A frame the guest sent. */
static void from_guest(uint8_t *f, int len) {
    if (len < 14) return;
    uint16_t type = g16(f + 12);
    if (type == 0x0806 && len >= 42) {                             /* ARP: who is 10.0.3.1 */
        uint8_t *a = f + 14;
        if (g16(a + 6) != 1 || g32(a + 24) != GW_IP) return;
        uint8_t r[42];
        memcpy(r, a + 8, 6); memcpy(r + 6, gw_mac, 6); s16(r + 12, 0x0806);
        uint8_t *o = r + 14;
        s16(o, 1); s16(o + 2, 0x0800); o[4] = 6; o[5] = 4; s16(o + 6, 2);
        memcpy(o + 8, gw_mac, 6); s32(o + 14, GW_IP); memcpy(o + 18, a + 8, 6); memcpy(o + 24, a + 14, 4);
        if (rxn < 64) { memcpy(rxq[rxn].f, r, 42); rxq[rxn].len = 42; rxn++; net_flush(); }
        return;
    }
    if (type != 0x0800 || len < 34) return;
    uint8_t *ip = f + 14;
    int ihl = (ip[0] & 15) * 4, tot = g16(ip + 2), proto = ip[9];
    if ((ip[0] >> 4) != 4 || tot > len - 14 || tot < ihl + 8) return;
    uint32_t dst = g32(ip + 16);
    if ((dst >> 8) == (GW_IP >> 8) || (dst & 0xF0000000u) == 0xE0000000u || dst == 0xFFFFFFFFu) return;
    uint32_t me = net_ip_of(dst);
    if (!me) { net_drop++; return; }
    uint8_t *p = ip + ihl;
    int i;
    if (proto == 6 || proto == 17) {
        i = nat_slot(proto, g16(p), dst, g16(p + 2));
        s16(p, (uint16_t)(NAT_LO + i));
        if (proto == 6 && (p[13] & 0x05)) nat[i].fin = 1;
    } else if (proto == 1 && p[0] == 8) {                          /* ping */
        i = nat_slot(1, g16(p + 4), dst, 0);
        s16(p + 4, (uint16_t)(NAT_LO + i));
    } else return;
    nat[i].last = now_ms();
    s32(ip + 12, me);
    ip_fix(ip);
    if (net_ip_out(f, tot) < 0) net_drop++; else net_out++;
}

static void net_run(vdev_t *d, int qi) {
    if (qi == 0) { net_flush(); return; }
    chain_t c;
    static uint8_t buf[1600 + 12];
    int any = 0;
    while (vq_pop(d, 1, &c) > 0) {
        uint32_t n = chain_read(&c, buf, sizeof buf);
        if (n > 12) from_guest(buf + 12, (int)(n - 12));   /* the frame; its header is room to send it */
        vq_push(d, 1, c.head, 0);
        any = 1;
    }
    if (any) vd_irq(d);
}

static void net_dev_init(vdev_t *d) {
    d->type = 1; d->features = VERSION_1 | (1ull << 5);           /* MAC */
    memcpy(d->cfg, guest_mac, 6);
    d->cfg_len = 8;
    d->run = net_run;
}

/* ---- setting up, and each turn ------------------------------------------------------------------ */
int vdev_setup(const vm_config *c) {
    if (store_open(1) != 0) return -1;
    const store_entry_t *root = store_find(c->root), *home = store_find(c->home);
    if (!root || !(root->flags & STORE_DONE)) { kprintf("[vm] %s isn't on the card\n", c->root); return -1; }
    if (!home) {
        home = store_create(c->home, c->home_size);
        if (!home) { kprintf("[vm] no room for %s: %s\n", c->home, store_why()); return -1; }
        store_progress(c->home, c->home_size, 1, 0);
        home = store_find(c->home);
        kprintf("[vm] made %s, %lu MiB, for Firefox's profile\n", c->home, (unsigned long)(c->home_size >> 20));
    }
    if (!bounce && !(bounce = kmalloc(65536))) return -1;
    if (!greq && !(greq = kmalloc(65536))) return -1;
    memset(dev, 0, sizeof dev);
    for (int i = 0; i < GPU_RES; i++) if (res[i].id) res_free(&res[i]);
    scanout = 0; cur_w = cur_h = 0; evn[0] = evn[1] = 0; rxn = 0; last_btn = 0;
    memset(nat, 0, sizeof nat);
    pw = c->screen_w > 0 ? c->screen_w : 1024; ph = c->screen_h > 0 ? c->screen_h : 576;
    for (int i = 0; i < ND; i++) dev[i].slot = i;
    blk_init(&dev[D_ROOT], root, 1);
    blk_init(&dev[D_HOME], home, 0);
    net_dev_init(&dev[D_NET]);
    gpu_init(&dev[D_GPU]);
    input_init(&dev[D_KBD]);
    input_init(&dev[D_TAB]);
    dev[D_RNG].type = 4; dev[D_RNG].features = VERSION_1; dev[D_RNG].run = rng_run;
    rs[0] = now_us() ^ 0x243F6A8885A308D3ull; rs[1] = (uint64_t)mini_rand() << 32 | mini_rand();
    ndev = ND;
    net_nat_in = nat_in;
    on = 1;
    return 0;
}

void vdev_stop(void) {
    on = 0;
    net_nat_in = 0;
    for (int i = 0; i < GPU_RES; i++) if (res[i].id) res_free(&res[i]);
    scanout = 0;
}

static uint64_t dev_us[ND], dbg_t0;                 /* time each device took, for /vm */

void vdev_poll(void) {
    if (!on) return;
    for (int i = 0; i < ndev; i++) {
        vdev_t *d = &dev[i];
        if (!__atomic_load_n(&d->kicked, __ATOMIC_ACQUIRE)) continue;
        d->kicked = 0;
        uint64_t t = now_us();
        for (int q = 0; q < NQ; q++)
            if (d->q[q].kicked) { d->q[q].kicked = 0; if (d->run) d->run(d, q); }
        dev_us[i] += now_us() - t;
    }
    if (rxn) net_flush();
    if (evn[0]) input_flush(0);
    if (evn[1]) input_flush(1);
}

int vdev_debug(char *out, int cap) {
    static const char *const nm[ND] = { "root", "home", "net", "screen", "keyboard", "tablet", "rng" };
    uint64_t t = now_us(), dt = dbg_t0 ? t - dbg_t0 : 1;
    int o = snprintf(out, (size_t)cap, "disks: read %lu KiB, wrote %lu KiB; network: %lu out, %lu in, %lu dropped; "
                     "screen: resource %u on it, %lu flushes, %lu MiB read\n", (unsigned long)(blk_reads >> 10), (unsigned long)(blk_writes >> 10),
                     (unsigned long)net_out, (unsigned long)net_in, (unsigned long)net_drop, scanout, (unsigned long)gpu_flushes,
                     (unsigned long)(gpu_xfer >> 20));
    o += snprintf(out + o, (size_t)(cap - o), "core 0 on devices over the last %lu ms:", (unsigned long)(dt / 1000));
    for (int i = 0; i < ND; i++) { o += snprintf(out + o, (size_t)(cap - o), " %s %lu ms", nm[i], (unsigned long)(dev_us[i] / 1000)); dev_us[i] = 0; }
    o += snprintf(out + o, (size_t)(cap - o), "\n");
    dbg_t0 = t;
    return o;
}
