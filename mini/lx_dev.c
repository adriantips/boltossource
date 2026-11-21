/* ===========================================================================
 *  BoltOS Mini  -  mini/lx_dev.c
 *
 *  The devices a Linux program finds under /dev (lx.h):
 *    fb0            a framebuffer: 32-bit XRGB, two screens tall so the
 *                   program can draw one half while the app shows the other.
 *                   FBIOPAN_DISPLAY's yoffset says which half is finished;
 *                   the app copies it out (lx_fb_frame) and says so
 *                   (lx_fb_done), and the next pan waits for that, briefly,
 *                   so a half is never drawn into while it is being shown.
 *    input/event0   the keyboard and mouse together, Linux input_event
 *                   records from input.c's raw ring
 *    tty, console   KDSETMODE and friends say yes; writes go to the output
 *    null, zero, urandom, random
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "lxint.h"
#include "string.h"
#include "stdio.h"

static struct {
    uint32_t *mem;
    int      w, h;
    uint32_t yres_virtual, yoffset;
    volatile uint32_t seq __attribute__((aligned(64)));     /* the program's: frames finished */
    volatile uint32_t half;
    volatile uint32_t done __attribute__((aligned(64)));    /* the app's: the last one it took */
} FB;

int lxdev_init(int w, int h) {
    memset((void *)&FB, 0, sizeof FB);
    FB.w = w; FB.h = h;
    FB.yres_virtual = (uint32_t)h;
    FB.mem = lx_kpages((uint64_t)w * (uint64_t)h * 4 * 2);
    return FB.mem ? 0 : -1;
}

uint64_t lxdev_fb_addr(uint64_t *len) {
    *len = (uint64_t)FB.w * (uint64_t)FB.h * 4 * 2;
    return (uint64_t)(uintptr_t)FB.mem;
}

const uint32_t *lx_fb_frame(int *w, int *h, uint32_t *seq) {
    if (!FB.mem || lx_state() != LX_RUNNING) return 0;
    *w = FB.w; *h = FB.h;
    *seq = __atomic_load_n(&FB.seq, __ATOMIC_ACQUIRE);
    return FB.mem + (uint64_t)FB.half * (uint64_t)FB.w * (uint64_t)FB.h;
}
void lx_fb_done(uint32_t seq) {
    __atomic_store_n(&FB.done, seq, __ATOMIC_RELEASE);
    __asm__ volatile("dsb ish\nsev" ::: "memory");
}

static void fb_pan(uint32_t yoffset) {
    /* The app takes frame n before n+1 goes up -- at most 40 ms, so a game
     * with nobody looking (the app not in front) only slows down. */
    uint32_t s = FB.seq;
    uint64_t end = lx_now_ns() + 40000000ull;
    while (s && __atomic_load_n(&FB.done, __ATOMIC_ACQUIRE) != s && lx_now_ns() < end && !lx_stopping())
        __asm__ volatile("wfe");
    FB.half = (yoffset >= (uint32_t)FB.h && FB.yres_virtual >= 2u * (uint32_t)FB.h) ? 1 : 0;
    __atomic_store_n(&FB.seq, s + 1, __ATOMIC_RELEASE);
}

/* ---- opening ---------------------------------------------------------------------------- */
int lxdev_open(const char *p, lxfd_t *fd) {
    int k = FD_NONE;
    if (!strcmp(p, "/dev/fb0")) k = FB.mem ? FD_FB : FD_NONE;
    else if (!strcmp(p, "/dev/input/event0")) k = FD_EVDEV;
    else if (!strcmp(p, "/dev/tty") || !strcmp(p, "/dev/tty0")) k = FD_TTY;
    else if (!strcmp(p, "/dev/console")) k = FD_CON;
    else if (!strcmp(p, "/dev/null")) k = FD_NULL;
    else if (!strcmp(p, "/dev/zero")) k = FD_ZERO;
    else if (!strcmp(p, "/dev/urandom") || !strcmp(p, "/dev/random")) k = FD_RANDOM;
    else return 0;
    fd->kind = (uint8_t)k;
    fd->ino = 0x900004000ull + (uint64_t)k;
    return 1;
}

int lxdev_list(const char *dir, void (*fn)(void *ctx, const char *name, int isdir), void *ctx) {
    if (!strcmp(dir, "/dev")) {
        static const char *const names[] = { "tty", "tty0", "console", "null", "zero", "urandom", "random", 0 };
        if (FB.mem) fn(ctx, "fb0", 0);
        fn(ctx, "input", 1);
        for (int i = 0; names[i]; i++) fn(ctx, names[i], 0);
    } else if (!strcmp(dir, "/dev/input")) fn(ctx, "event0", 0);
    return 0;
}

/* ---- reading and writing ---------------------------------------------------------------------- */
static uint64_t rnd_state;
static uint64_t rnd64(void) {
    if (!rnd_state) rnd_state = lx_now_ns() | 1;
    rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 7; rnd_state ^= rnd_state << 17;
    return rnd_state * 0x2545F4914F6CDD1Dull;
}

int64_t lxdev_read(lxfd_t *f, void *buf, uint64_t n) {
    uint8_t *b = buf;
    switch (f->kind) {
    case FD_NULL: case FD_CON: case FD_TTY: return 0;
    case FD_ZERO: memset(buf, 0, n); return (int64_t)n;
    case FD_RANDOM: for (uint64_t i = 0; i < n; i++) b[i] = (uint8_t)(rnd64() >> 32); return (int64_t)n;
    case FD_EVDEV: {
        uint64_t got = 0;
        for (;;) {
            input_raw_t e;
            while (got + 24 <= n && input_raw_pop(&e)) {
                uint64_t ns = lx_now_ns();
                int64_t sec = (int64_t)(ns / 1000000000ull), usec = (int64_t)(ns % 1000000000ull / 1000);
                memcpy(b + got, &sec, 8); memcpy(b + got + 8, &usec, 8);
                memcpy(b + got + 16, &e.type, 2); memcpy(b + got + 18, &e.code, 2); memcpy(b + got + 20, &e.value, 4);
                got += 24;
            }
            if (got || (f->flags & L_O_NONBLOCK) || n < 24) break;
            if (lx_stopping()) return -L_EINTR;
            __asm__ volatile("wfe");
        }
        return got ? (int64_t)got : (n < 24 ? -L_EINVAL : -L_EAGAIN);
    }
    case FD_FB: {
        uint64_t len = (uint64_t)FB.w * (uint64_t)FB.h * 8;
        if (f->pos >= len) return 0;
        if (n > len - f->pos) n = len - f->pos;
        memcpy(buf, (uint8_t *)FB.mem + f->pos, n);
        f->pos += n;
        return (int64_t)n;
    }
    }
    return -L_EINVAL;
}

int64_t lxdev_write(lxfd_t *f, const void *buf, uint64_t n) {
    switch (f->kind) {
    case FD_CON: case FD_TTY: lx_out(buf, n); return (int64_t)n;
    case FD_NULL: case FD_ZERO: case FD_RANDOM: case FD_EVDEV: return (int64_t)n;
    case FD_FB: {
        uint64_t len = (uint64_t)FB.w * (uint64_t)FB.h * 8;
        if (f->pos >= len) return -L_ENOSPC;
        if (n > len - f->pos) n = len - f->pos;
        memcpy((uint8_t *)FB.mem + f->pos, buf, n);
        f->pos += n;
        return (int64_t)n;
    }
    }
    return -L_EINVAL;
}

int lxdev_ready(lxfd_t *f) {
    if (f->kind == FD_EVDEV) return input_raw_pending();
    return f->kind == FD_NULL || f->kind == FD_ZERO || f->kind == FD_RANDOM;
}

/* ---- ioctl ----------------------------------------------------------------------------------- */
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static int fb_ioctl(uint64_t cmd, uint8_t *a) {
    uint32_t w = (uint32_t)FB.w, h = (uint32_t)FB.h;
    switch (cmd) {
    case 0x4600:                                   /* FBIOGET_VSCREENINFO */
        if (!lx_uok((uint64_t)(uintptr_t)a, 160)) return -L_EFAULT;
        memset(a, 0, 160);
        put32(a + 0, w); put32(a + 4, h); put32(a + 8, w); put32(a + 12, FB.yres_virtual);
        put32(a + 20, FB.yoffset); put32(a + 24, 32);
        put32(a + 32, 16); put32(a + 36, 8);       /* red   */
        put32(a + 44, 8);  put32(a + 48, 8);       /* green */
        put32(a + 56, 0);  put32(a + 60, 8);       /* blue  */
        put32(a + 68, 24); put32(a + 72, 0);       /* transp: none */
        put32(a + 88, (uint32_t)-1); put32(a + 92, (uint32_t)-1);
        put32(a + 100, 13468);                     /* pixclock: ~74 MHz, nobody minds */
        return 0;
    case 0x4601: {                                 /* FBIOPUT_VSCREENINFO */
        if (!lx_uok((uint64_t)(uintptr_t)a, 160)) return -L_EFAULT;
        uint32_t xr, yr, yv, bpp, yo;
        memcpy(&xr, a, 4); memcpy(&yr, a + 4, 4); memcpy(&yv, a + 12, 4); memcpy(&yo, a + 20, 4); memcpy(&bpp, a + 24, 4);
        if (xr != w || yr != h || (bpp && bpp != 32)) return -L_EINVAL;
        FB.yres_virtual = yv >= 2 * h ? 2 * h : h;
        FB.yoffset = yo < FB.yres_virtual ? yo : 0;
        return fb_ioctl(0x4600, a);
    }
    case 0x4602:                                   /* FBIOGET_FSCREENINFO */
        if (!lx_uok((uint64_t)(uintptr_t)a, 80)) return -L_EFAULT;
        memset(a, 0, 80);
        memcpy(a, "BoltOS Mini", 12);
        { uint64_t start = (uint64_t)(uintptr_t)FB.mem; memcpy(a + 16, &start, 8); }
        put32(a + 24, w * h * 8);
        put32(a + 28, 0);                          /* FB_TYPE_PACKED_PIXELS */
        put32(a + 36, 2);                          /* FB_VISUAL_TRUECOLOR   */
        a[42] = 1;                                 /* ypanstep              */
        put32(a + 48, w * 4);
        return 0;
    case 0x4606: {                                 /* FBIOPAN_DISPLAY */
        uint32_t yo = 0;
        if (a && lx_uok((uint64_t)(uintptr_t)a, 24)) memcpy(&yo, a + 20, 4);
        FB.yoffset = yo;
        fb_pan(yo);
        return 0;
    }
    case 0x40044620:                               /* FBIO_WAITFORVSYNC */
        return 0;
    case 0x4605: case 0x4604: return 0;            /* FBIOPUTCMAP / GETCMAP */
    }
    return -L_ENOTTY;
}

static int set_bits(uint8_t *a, uint64_t len, uint64_t size, const int *bits, int n) {
    if (!lx_uok((uint64_t)(uintptr_t)a, len)) return -L_EFAULT;
    uint64_t k = len < size ? len : size;
    memset(a, 0, k);
    for (int i = 0; i < n; i++) if ((uint64_t)bits[i] / 8 < k) a[bits[i] / 8] |= (uint8_t)(1 << (bits[i] % 8));
    return (int)k;
}

static int evdev_ioctl(uint64_t cmd, uint8_t *a) {
    uint32_t dir = (uint32_t)(cmd >> 30), size = (uint32_t)(cmd >> 16 & 0x3FFF), type = (uint32_t)(cmd >> 8 & 0xFF), nr = (uint32_t)(cmd & 0xFF);
    if (type != 'E') return -L_ENOTTY;
    if (nr == 0x90 || nr == 0x91) return 0;       /* EVIOCGRAB, EVIOCREVOKE */
    if (dir != 2) return dir == 1 ? 0 : -L_EINVAL;
    if (nr == 0x01) { if (!lx_uok((uint64_t)(uintptr_t)a, 4)) return -L_EFAULT; put32(a, 0x010001); return 0; }
    if (nr == 0x02) {                              /* EVIOCGID: bus, vendor, product, version */
        if (!lx_uok((uint64_t)(uintptr_t)a, 8)) return -L_EFAULT;
        uint16_t id[4] = { 0x03, 0x1d6b, 0x0104, 1 };
        memcpy(a, id, 8);
        return 0;
    }
    if (nr == 0x06 || nr == 0x07 || nr == 0x08) { /* EVIOCGNAME / PHYS / UNIQ */
        const char *s = nr == 0x06 ? "BoltOS Mini keyboard and mouse" : nr == 0x07 ? "boltos/input0" : "";
        uint32_t k = (uint32_t)strlen(s) + 1;
        if (k > size) k = size;
        if (!lx_uok((uint64_t)(uintptr_t)a, k)) return -L_EFAULT;
        memcpy(a, s, k);
        if (k) a[k - 1] = 0;
        return (int)k;
    }
    if (nr >= 0x18 && nr <= 0x1b) {                /* EVIOCGKEY / LED / SND / SW: nothing held */
        if (!lx_uok((uint64_t)(uintptr_t)a, size)) return -L_EFAULT;
        memset(a, 0, size);
        return (int)size;
    }
    if (nr >= 0x20 && nr < 0x40) {                 /* EVIOCGBIT(ev, len) */
        int ev = (int)(nr - 0x20);
        if (ev == 0) { static const int b[] = { 0, 1, 2 }; return set_bits(a, size, 4, b, 3); }
        if (ev == 1) {
            int b[260], n = 0;
            for (int k = 1; k < 256 && n < 250; k++) b[n++] = k;
            for (int k = 0x110; k <= 0x114; k++) b[n++] = k;
            return set_bits(a, size, 96, b, n);    /* KEY_CNT / 8 */
        }
        if (ev == 2) { static const int b[] = { 0, 1, 8 }; return set_bits(a, size, 2, b, 3); }
        return set_bits(a, size, 0, 0, 0);
    }
    return -L_EINVAL;
}

int lxdev_ioctl(lxfd_t *f, uint64_t cmd, uint64_t arg) {
    uint8_t *a = (uint8_t *)(uintptr_t)arg;
    switch (f->kind) {
    case FD_FB: return fb_ioctl(cmd, a);
    case FD_EVDEV: return evdev_ioctl(cmd, a);
    case FD_TTY: case FD_CON:
        if (cmd == 0x5413) {                       /* TIOCGWINSZ: a terminal, so stdio flushes by line */
            if (!lx_uok(arg, 8)) return -L_EFAULT;
            uint16_t ws[4] = { 40, 120, 0, 0 };
            memcpy(a, ws, 8);
            return 0;
        }
        if (cmd == 0x5401) {                       /* TCGETS */
            if (!lx_uok(arg, 60)) return -L_EFAULT;
            memset(a, 0, 60);
            return 0;
        }
        if (cmd == 0x4B3A || cmd == 0x4B3B || cmd == 0x4B44 || cmd == 0x4B45 || cmd == 0x5402 ||
            cmd == 0x5403 || cmd == 0x5404 || cmd == 0x540E || cmd == 0x5603 || cmd == 0x5606)
            return 0;                              /* KDSETMODE, KDGETMODE, KD(G|S)KBMODE, TCSETS*, TIOCSCTTY, VT_* */
        return -L_ENOTTY;
    }
    return -L_ENOTTY;
}
