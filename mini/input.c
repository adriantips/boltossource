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

#define QCAP 256
static input_ev_t q[QCAP];
static volatile uint32_t qhead, qtail;
static uint32_t counts[8];
static int last_x = -1, last_y = -1, last_btn;

static void push(const input_ev_t *e) {
    if (qhead - qtail >= QCAP) qtail++;            /* full: drop the oldest */
    q[qhead % QCAP] = *e;
    qhead++;
    if (e->source < 8) counts[e->source]++;
    shell_request_frame();
}

void input_key(int key, int ch, int mods, int source) {
    if (key == KEY_NONE) return;
    input_ev_t e = { .type = EV_KEY, .key = (uint8_t)key, .mods = (uint8_t)mods,
                     .ch = ch, .source = (uint8_t)source };
    push(&e);
}

void input_pointer(int x, int y, int buttons, int wheel, int source) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= scr_w) x = scr_w - 1;
    if (y >= scr_h) y = scr_h - 1;
    if (x == last_x && y == last_y && buttons == last_btn && !wheel) return;
    last_x = x; last_y = y; last_btn = buttons;
    input_ev_t e = { .type = EV_POINTER, .buttons = (uint8_t)buttons, .x = (int16_t)x,
                     .y = (int16_t)y, .wheel = (int8_t)wheel, .source = (uint8_t)source };
    push(&e);
}

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

/* ---- odds and ends that want a home ---------------------------------------------- */
static uint64_t rng = 0x9E3779B97F4A7C15ull;
uint32_t mini_rand(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    rng += now_us();
    return (uint32_t)(rng >> 16);
}
