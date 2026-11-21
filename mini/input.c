/* ===========================================================================
 *  BoltOS Mini  -  mini/input.c
 *
 *  One queue for every keyboard and pointer, whichever wire it came in on:
 *  the USB keyboard on the Pi's hub, the Mac's keyboard and trackpad through
 *  the VNC viewer, QEMU's window. Each source translates its own codes --
 *  HID usages, X11 keysyms, Linux evdev codes -- into KEY_* plus a
 *  character, here, so the shell only ever sees one vocabulary.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "ui.h"                                /* S(), for the touch distances */
#include "string.h"

#define QCAP 256
static input_ev_t q[QCAP];
static volatile uint32_t qhead, qtail;
static uint32_t counts[8];
static int last_x = -1, last_y = -1, last_btn;

static uint64_t last_act;                           /* the last key, click or wheel */

/* A pointer that only moved asks for no frame: the shell moves the arrow
 * over the picture, and redraws only if what is under it reacts. */
static void push(const input_ev_t *e, int act) {
    if (qhead - qtail >= QCAP) qtail++;            /* full: drop the oldest */
    q[qhead % QCAP] = *e;
    qhead++;
    if (e->source < 8) counts[e->source]++;
    if (act) { last_act = now_ms(); shell_request_frame(); }
}

uint64_t input_idle_ms(void) { return now_ms() - last_act; }

void input_key(int key, int ch, int mods, int source) {
    if (key == KEY_NONE) return;
    input_ev_t e = { .type = EV_KEY, .key = (uint8_t)key, .mods = (uint8_t)mods,
                     .ch = ch, .source = (uint8_t)source };
    push(&e, 1);
}

void input_pointer(int x, int y, int buttons, int wheel, int source) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= scr_w) x = scr_w - 1;
    if (y >= scr_h) y = scr_h - 1;
    if (x == last_x && y == last_y && buttons == last_btn && !wheel) return;
    int act = buttons != last_btn || wheel;
    last_x = x; last_y = y; last_btn = buttons;
    input_ev_t e = { .type = EV_POINTER, .buttons = (uint8_t)buttons, .x = (int16_t)x,
                     .y = (int16_t)y, .wheel = (int8_t)wheel, .source = (uint8_t)source };
    push(&e, act);
}

/* ---- a touchscreen ------------------------------------------------------------- *
 *  A finger becomes what every app already understands, a pointer. Apps click
 *  on the press, so the press waits until the finger shows what it means:
 *    - lifted where it landed: a click there;
 *    - slid past a few pixels: scrolling, one wheel step per S(48) of travel,
 *      the page following the finger, and no click at all;
 *    - held still for 0.4 s: the button goes down there and stays down while
 *      the finger moves, a drag (select text, move a slider).               */
enum { T_UP, T_WAIT, T_SCROLL, T_DRAG };
static struct { int state, x0, y0, x, y, ref, source; uint64_t t0; } tch;

static void touch_hold_check(void) {
    if (tch.state == T_WAIT && now_ms() - tch.t0 >= 400) {
        tch.state = T_DRAG;
        input_pointer(tch.x0, tch.y0, 1, 0, tch.source);
    }
}

void input_touch(int x, int y, int down, int source) {
    if (down && tch.state == T_UP) {                       /* the finger lands */
        tch.state = T_WAIT; tch.x0 = tch.x = x; tch.y0 = tch.y = y; tch.ref = y;
        tch.source = source; tch.t0 = now_ms();
        input_pointer(x, y, 0, 0, source);                 /* the arrow goes there, nothing pressed */
        return;
    }
    if (tch.state == T_UP) return;
    if (down) {
        tch.x = x; tch.y = y;
        if (tch.state == T_WAIT) {
            int dx = x - tch.x0, dy = y - tch.y0, slop = S(10);
            if (dx * dx + dy * dy > slop * slop) { tch.state = T_SCROLL; tch.ref = tch.y0; }
            else touch_hold_check();
        }
        if (tch.state == T_SCROLL) {
            int step = S(48);
            if (step < 8) step = 8;
            while (y - tch.ref >= step) { input_pointer(tch.x0, tch.y0, 0, 1, source); tch.ref += step; }
            while (tch.ref - y >= step) { input_pointer(tch.x0, tch.y0, 0, -1, source); tch.ref -= step; }
        } else if (tch.state == T_DRAG) input_pointer(x, y, 1, 0, source);
        return;
    }
    if (tch.state == T_WAIT) {                             /* a tap */
        input_pointer(tch.x0, tch.y0, 1, 0, source);
        input_pointer(tch.x0, tch.y0, 0, 0, source);
    } else if (tch.state == T_DRAG) input_pointer(tch.x, tch.y, 0, 0, source);   /* where it was, not 0,0 */
    tch.state = T_UP;
}

/* A finger held still may send nothing: the hold is timed here too. */
void input_touch_tick(void) { touch_hold_check(); }

int input_pop(input_ev_t *ev) {
    if (qtail == qhead) return 0;
    *ev = q[qtail % QCAP];
    qtail++;
    return 1;
}

uint32_t input_count(int source) { return (source >= 0 && source < 8) ? counts[source] : 0; }

/* ---- USB HID boot protocol (US layout) ------------------------------------- */
static const char hid_plain[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
static const char hid_shift[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";

int input_from_hid(uint8_t u, int shift, int *ch) {
    *ch = 0;
    if (u >= 0x04 && u <= 0x38) {
        char c = (shift ? hid_shift : hid_plain)[u - 0x04];
        if (c == '\n') return KEY_ENTER;
        if (c == 0x1b) return KEY_ESC;
        if (c == '\b') return KEY_BACKSPACE;
        if (c == '\t') return KEY_TAB;
        *ch = (unsigned char)c;
        return KEY_CHAR;
    }
    switch (u) {
    case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45:
        return KEY_F1 + (u - 0x3A);
    case 0x49: return KEY_INSERT;
    case 0x4A: return KEY_HOME;
    case 0x4B: return KEY_PGUP;
    case 0x4C: return KEY_DELETE;
    case 0x4D: return KEY_END;
    case 0x4E: return KEY_PGDN;
    case 0x4F: return KEY_RIGHT;
    case 0x50: return KEY_LEFT;
    case 0x51: return KEY_DOWN;
    case 0x52: return KEY_UP;
    case 0x58: return KEY_ENTER;                   /* keypad Enter          */
    case 0x54: *ch = '/'; return KEY_CHAR;
    case 0x55: *ch = '*'; return KEY_CHAR;
    case 0x56: *ch = '-'; return KEY_CHAR;
    case 0x57: *ch = '+'; return KEY_CHAR;
    case 0x63: *ch = '.'; return KEY_CHAR;
    case 0x64: *ch = shift ? '|' : '\\'; return KEY_CHAR;   /* non-US \ |      */
    }
    if (u >= 0x59 && u <= 0x62) {                  /* keypad 1..9, 0        */
        *ch = (u == 0x62) ? '0' : '1' + (u - 0x59);
        return KEY_CHAR;
    }
    return KEY_NONE;
}

/* ---- X11 keysyms (VNC) --------------------------------------------------------- */
int input_from_keysym(uint32_t s, int *ch) {
    *ch = 0;
    if ((s >= 0x20 && s <= 0x7E) || (s >= 0xA0 && s <= 0xFF)) { *ch = (int)s; return KEY_CHAR; }
    switch (s) {
    case 0xFF0D: case 0xFF8D: return KEY_ENTER;
    case 0xFF1B: return KEY_ESC;
    case 0xFF08: return KEY_BACKSPACE;
    case 0xFF09: case 0xFE20: return KEY_TAB;
    case 0xFFFF: return KEY_DELETE;
    case 0xFF63: return KEY_INSERT;
    case 0xFF50: return KEY_HOME;
    case 0xFF57: return KEY_END;
    case 0xFF55: return KEY_PGUP;
    case 0xFF56: return KEY_PGDN;
    case 0xFF51: return KEY_LEFT;
    case 0xFF52: return KEY_UP;
    case 0xFF53: return KEY_RIGHT;
    case 0xFF54: return KEY_DOWN;
    case 0xFFEB: case 0xFFEC: return KEY_SUPER;
    }
    if (s >= 0xFFBE && s <= 0xFFC9) return KEY_F1 + (int)(s - 0xFFBE);
    if (s >= 0xFFB0 && s <= 0xFFB9) { *ch = '0' + (int)(s - 0xFFB0); return KEY_CHAR; }
    return KEY_NONE;
}

/* ---- raw events, as Linux evdev has them ----------------------------------------- *
 *  A Linux program (lx_dev.c's /dev/input/event0) wants what the keys and the
 *  mouse really did: every key down and up by its Linux code, and how far the
 *  mouse moved, not where the arrow got to. While it is in front, each
 *  source sends those here as well (input_raw_on()); the queue above carries
 *  on as before, so Super still goes Home.
 *
 *  One ring, the boot core filling it, the program's core emptying it: each
 *  index written by one side only, on a cache line of its own.              */
#define RAWCAP 1024
static input_raw_t raw[RAWCAP];
static struct { volatile uint32_t head; } __attribute__((aligned(64))) rawp;
static struct { volatile uint32_t tail; } __attribute__((aligned(64))) rawc;
static volatile int raw_enabled;
static uint8_t raw_btn;                             /* the mouse buttons held, as sent */

void input_raw_enable(int on) {
    if (on && !raw_enabled) { rawc.tail = rawp.head; raw_btn = 0; }
    raw_enabled = on;
    __asm__ volatile("dmb ish" ::: "memory");
}
int input_raw_on(void) { return raw_enabled; }

void input_raw(int type, int code, int value) {
    if (!raw_enabled) return;
    uint32_t h = rawp.head;
    if (h - __atomic_load_n(&rawc.tail, __ATOMIC_ACQUIRE) >= RAWCAP) return;    /* full: the newest go */
    raw[h % RAWCAP] = (input_raw_t){ (uint16_t)type, (uint16_t)code, value };
    __atomic_store_n(&rawp.head, h + 1, __ATOMIC_RELEASE);
}

int input_raw_pop(input_raw_t *e) {
    uint32_t t = rawc.tail;
    if (t == __atomic_load_n(&rawp.head, __ATOMIC_ACQUIRE)) return 0;
    *e = raw[t % RAWCAP];
    __atomic_store_n(&rawc.tail, t + 1, __ATOMIC_RELEASE);
    return 1;
}
int input_raw_pending(void) { return rawc.tail != __atomic_load_n(&rawp.head, __ATOMIC_ACQUIRE); }

/* HID keyboard usages -> Linux KEY_* codes (Linux's own hid_keyboard table). */
static const uint8_t hid_evdev[0x68] = {
    [0x04] = 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,
    [0x1E] = 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    [0x28] = 28, 1, 14, 15, 57, 12, 13, 26, 27, 43, 43, 39, 40, 41, 51, 52, 53, 58,
    [0x3A] = 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88,
    [0x46] = 99, 70, 119, 110, 102, 104, 111, 107, 109, 106, 105, 108, 103,
    [0x53] = 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71, 72, 73, 82, 83, 86, 127, 116, 117,
};
static const uint8_t mod_evdev[8] = { 29, 42, 56, 125, 97, 54, 100, 126 };

int input_hid_to_evdev(int u) {
    if (u >= 0 && u < (int)sizeof hid_evdev) return hid_evdev[u];
    if (u >= 0xE0 && u <= 0xE7) return mod_evdev[u - 0xE0];
    return 0;
}

/* A keyboard report: what was held before and now (modifier bits and usages). */
void input_raw_keys(uint8_t old_mods, const uint8_t *old_keys, int nold, uint8_t mods, const uint8_t *keys, int n) {
    if (!raw_enabled) return;
    for (int b = 0; b < 8; b++)
        if ((old_mods ^ mods) >> b & 1) input_raw(1, mod_evdev[b], mods >> b & 1);
    for (int i = 0; i < nold; i++) {
        if (old_keys[i] < 4) continue;
        int still = 0;
        for (int j = 0; j < n; j++) if (keys[j] == old_keys[i]) still = 1;
        if (!still && input_hid_to_evdev(old_keys[i])) input_raw(1, input_hid_to_evdev(old_keys[i]), 0);
    }
    for (int j = 0; j < n; j++) {
        if (keys[j] < 4) continue;
        int was = 0;
        for (int i = 0; i < nold; i++) if (old_keys[i] == keys[j]) was = 1;
        if (!was && input_hid_to_evdev(keys[j])) input_raw(1, input_hid_to_evdev(keys[j]), 1);
    }
}

/* A mouse report: how far, the wheel, the buttons held (bit 0 left, 1 right,
 * 2 middle, 3 back, 4 forward). */
void input_raw_mouse(int dx, int dy, int wheel, int buttons) {
    if (!raw_enabled) return;
    if (dx) input_raw(2, 0, dx);
    if (dy) input_raw(2, 1, dy);
    if (wheel) input_raw(2, 8, wheel);
    for (int b = 0; b < 5; b++)
        if ((raw_btn ^ buttons) >> b & 1) input_raw(1, 0x110 + b, buttons >> b & 1);
    raw_btn = (uint8_t)buttons;
}

/* X11 keysyms (VNC) -> Linux codes, by the US key that makes them. */
int input_keysym_to_evdev(uint32_t s) {
    static const char row_sym[] = "1!2@3#4$5%6^7&8*9(0)-_=+[{]}\\|;:'\"`~,<.>/?";
    static const uint8_t row_code[] = { 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
                                        26, 26, 27, 27, 43, 43, 39, 39, 40, 40, 41, 41, 51, 51, 52, 52, 53, 53 };
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
    if (s >= 'A' && s <= 'Z') s += 32;
    if (s >= 'a' && s <= 'z') return hid_evdev[0x04 + (int)(strchr(letters, (int)s) - letters)];
    if (s == ' ') return 57;
    for (int i = 0; row_sym[i]; i++) if ((uint32_t)(unsigned char)row_sym[i] == s) return row_code[i];
    switch (s) {
    case 0xFF08: return 14;  case 0xFF09: case 0xFE20: return 15;  case 0xFF0D: return 28;  case 0xFF8D: return 96;
    case 0xFF1B: return 1;   case 0xFF50: return 102; case 0xFF51: return 105; case 0xFF52: return 103;
    case 0xFF53: return 106; case 0xFF54: return 108; case 0xFF55: return 104; case 0xFF56: return 109;
    case 0xFF57: return 107; case 0xFF63: return 110; case 0xFFFF: return 111; case 0xFFE5: return 58;
    case 0xFFE1: return 42;  case 0xFFE2: return 54;  case 0xFFE3: return 29;  case 0xFFE4: return 97;
    case 0xFFE9: return 56;  case 0xFFEA: return 100; case 0xFFE7: case 0xFFEB: return 125;
    case 0xFFE8: case 0xFFEC: return 126;
    case 0xFF13: return 119; case 0xFF14: return 70;  case 0xFF7F: return 69;
    }
    if (s >= 0xFFBE && s <= 0xFFC7) return 59 + (int)(s - 0xFFBE);          /* F1..F10 */
    if (s == 0xFFC8) return 87;
    if (s == 0xFFC9) return 88;
    if (s >= 0xFFB0 && s <= 0xFFB9) { static const uint8_t kp[10] = { 82, 79, 80, 81, 75, 76, 77, 71, 72, 73 }; return kp[s - 0xFFB0]; }
    return 0;
}

/* ---- odds and ends that want a home ---------------------------------------------- */
static uint64_t rng = 0x9E3779B97F4A7C15ull;
uint32_t mini_rand(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    rng += now_us();
    return (uint32_t)(rng >> 16);
}
