#include <stdint.h>
#include "interrupts.h"
#include "io.h"
#include "keyboard.h"
#include "keymap.h"
#include "kbdlayout.h"

/* extended (0xE0-prefixed) make code -> our control-byte key code, or 0 */
static char ext_map(uint8_t sc) {
    switch (sc) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x49: return KEY_PGUP;
        case 0x51: return KEY_PGDN;
        case 0x53: return KEY_DEL;
        default:   return 0;
    }
}

/* The scancode -> character tables used to be two 128-byte arrays right here,
 * US only. They are kernel/kbdlayout.c now, because which character a key
 * produces is a property of the keyboard in front of the person and not of the
 * driver -- and because a third level was needed for AltGr, which cannot be
 * expressed as "shifted or not" at all. `map()` and `shiftmap()` become one
 * lookup taking a level. */
static char layout_char(uint8_t sc, int level) {
    return (char)kbd_layout_char(sc, level);
}

/* Single-producer (IRQ) / single-consumer (shell) ring. 8-bit indices wrap at
 * 256 == RBSZ, so the modulo is free. */
#define RBSZ 256
static volatile char    rb[RBSZ];
static volatile uint8_t rhead, rtail;
static volatile int     shift;
static volatile int     ctrl;            /* either Ctrl key held */
static volatile int     alt;             /* LEFT Alt only -- see altgr */
/* The right Alt key on a non-US keyboard is not Alt. It is a third-level shift
 * and on German, French, Spanish and Italian layouts it is the only way to
 * type @, the braces and the currency signs. It used to be folded into `alt`,
 * so AltGr+Q fired whatever Alt+Q was bound to and produced nothing at all. */
static volatile int     altgr;
static volatile int     win;             /* either Super/Win key held */
static volatile int     win_used;         /* a key was pressed while Win held -> not a tap */
static volatile int     extended;        /* last byte was the 0xE0 prefix */
/* A dead key that has been pressed and is waiting for the letter it modifies.
 * 0 when nothing is pending. See include/kbdlayout.h. */
static volatile uint8_t dead_pending;

/* Map a navigation key to its selection-extending (Shift+nav) variant. */
static char shift_nav(char e) {
    switch ((unsigned char)e) {
        case KEY_LEFT:  return KEY_SLEFT;
        case KEY_RIGHT: return KEY_SRIGHT;
        case KEY_UP:    return KEY_SUP;
        case KEY_DOWN:  return KEY_SDOWN;
        case KEY_HOME:  return KEY_SHOME;
        case KEY_END:   return KEY_SEND;
        default:        return e;
    }
}
/* Ctrl+letter -> editing shortcut control byte, or 0 to swallow the combo.
 * Ctrl+Alt+L used to be special-cased here. It is an ordinary entry in the
 * shortcut table now, matched before this is reached -- which is also what
 * makes it rebindable without the lock screen becoming unreachable. */
static char ctrl_shortcut(char c) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    switch (c) {
        case 'a': return KEY_SELALL;
        case 'c': return KEY_COPY;
        case 'v': return KEY_PASTE;
        case 'x': return KEY_CUT;
        case 's': return KEY_SAVE;
        case 'k': return KEY_CUTLINE;
        default:  return 0;
    }
}

static void push(char c) {
    if (win) win_used = 1;                          /* a key fired during Win -> combo, not a tap */
    uint8_t n = (uint8_t)(rtail + 1);
    if (n != rhead) { rb[rtail] = c; rtail = n; }   /* drop on overflow */
}

/* Raw scancode tap: a parallel ring that delivers real make/break events for
 * clients (DOOM) that need key up as well as key down. Each entry encodes the
 * 7-bit scancode plus 0x100 = release and 0x200 = extended (0xE0-prefixed). The
 * normal ASCII ring above is unaffected; raw capture is opt-in via kbd_raw_enable. */
static volatile uint16_t kraw[RBSZ];
static volatile uint8_t  krhead, krtail;
static volatile int      raw_on;

static void raw_push(uint16_t v) {
    uint8_t n = (uint8_t)(krtail + 1);
    if (n != krhead) { kraw[krtail] = v; krtail = n; }
}
void kbd_raw_enable(int on) { raw_on = on ? 1 : 0; krhead = krtail = 0; }

/* Resolved global shortcuts, in their own ring. Small, because these are
 * chords a person pressed and nobody presses sixteen of them before the
 * compositor next looks. */
#define ARSZ 16
static volatile uint8_t aring[ARSZ];
static volatile uint8_t arhead, artail;

static void action_push(int a) {
    uint8_t n = (uint8_t)((artail + 1) % ARSZ);
    if (n != arhead) { aring[artail] = (uint8_t)a; artail = n; }
}
int kbd_action_get(void) {
    if (arhead == artail) return -1;
    int a = aring[arhead];
    arhead = (uint8_t)((arhead + 1) % ARSZ);
    return a;
}
int kbd_raw_get(void) {
    if (krhead == krtail) return -1;
    uint16_t v = kraw[krhead];
    krhead = (uint8_t)(krhead + 1);
    return (int)v;
}

/* What the shortcut table matches against. Shift is a modifier here rather
 * than a different character, so Win+Shift+Left is (WIN|SHIFT, Left) and not
 * some separate key -- otherwise every chord would need naming twice. */
static uint8_t cur_mods(void) {
    return (uint8_t)((ctrl ? KMOD_CTRL : 0) | (shift ? KMOD_SHIFT : 0) |
                     (alt ? KMOD_ALT : 0) | (win ? KMOD_WIN : 0));
}

static uint8_t logical_key(uint8_t sc, int ext) {
    if (ext) return (uint8_t)ext_map(sc);
    if (sc >= 0x3B && sc <= 0x44) return (uint8_t)(KEYK_F1 + (sc - 0x3B));  /* F1..F10 */
    if (sc == 0x57) return (uint8_t)(KEYK_F1 + 10);                          /* F11 */
    if (sc == 0x58) return (uint8_t)(KEYK_F1 + 11);                          /* F12 */
    char c = layout_char(sc, 0);             /* unshifted: Shift is a modifier */
    if (!c) return 0;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return (uint8_t)c;
}

static void on_key(struct registers *r) {
    (void)r;
    uint8_t sc = inb(0x60);
    if (sc == 0xE0) { extended = 1; return; }       /* prefix: next byte is extended */
    int ext = extended; extended = 0;
    if (raw_on)                                     /* feed the raw tap (real up/down) */
        raw_push((uint16_t)(sc & 0x7F) | ((sc & 0x80) ? 0x100 : 0) | (ext ? 0x200 : 0));

    /* ---- modifiers, and the Super key tapped on its own ------------------ */
    if (ext) {
        if (sc == 0x1D) { ctrl = 1; return; }       /* right Ctrl pressed  */
        if (sc == 0x9D) { ctrl = 0; return; }       /* right Ctrl released */
        /* Right Alt is AltGr, not Alt. The distinction is the whole reason a
         * German keyboard can type an @ sign. */
        if (sc == 0x38) { altgr = 1; return; }
        if (sc == 0xB8) { altgr = 0; return; }
        if (sc == 0x5B || sc == 0x5C) { win = 1; win_used = 0; return; }  /* L/R Super pressed  */
        if (sc == 0xDB || sc == 0xDC) {                                   /* L/R Super released */
            int wu = win_used; win = 0;
            if (!wu) {                              /* a tap: whatever is bound to Win alone */
                int a = keymap_tap(KMOD_WIN);
                if (a != ACT_NONE) action_push(a);
            }
            return;
        }
    } else {
        switch (sc) {
            case 0x2A: case 0x36: shift = 1; return;    /* L/R shift pressed  */
            case 0xAA: case 0xB6: shift = 0; return;    /* L/R shift released */
            case 0x1D: ctrl = 1; return;                /* left Ctrl pressed  */
            case 0x9D: ctrl = 0; return;                /* left Ctrl released */
            case 0x38: alt  = 1; return;                /* left Alt pressed   */
            case 0xB8: alt  = 0; return;                /* left Alt released  */
        }
    }
    if (sc & 0x80) return;                          /* any other key release */

    /* ---- the global shortcut table --------------------------------------
     * Consulted before anything else decides what this key means, so a bound
     * chord never reaches a window. Alt+Tab, Alt+F4, F12 and Ctrl+Alt+L used
     * to be `if`s right here; they are rows in kernel/keymap.c now, and so is
     * everything the compositor added since. */
    uint8_t lk = logical_key(sc, ext);
    if (lk) {
        int a = keymap_lookup(cur_mods(), lk);
        if (a != ACT_NONE) { action_push(a); win_used = 1; return; }
    }

    /* ---- ordinary delivery ----------------------------------------------- */
    if (ext) {
        char e = ext_map(sc);
        if (e) push(shift ? shift_nav(e) : e);
        return;
    }
    /* AltGr wins over Shift where a layout defines a third level, and falls
     * back to the shifted character where it does not -- so AltGr+Shift+7 on a
     * layout with nothing there still types a '/' rather than nothing. */
    char c = 0;
    if (altgr) c = layout_char(sc, 2);
    if (!c)    c = layout_char(sc, shift ? 1 : 0);
    if (!c) return;

    if (ctrl) {                                     /* Ctrl+key -> shortcut, else swallow */
        char s = ctrl_shortcut(c);
        if (s) push(s);
        return;
    }

    /* Dead keys. An accent is not a character on most European layouts, it is
     * a pending modification: press it, nothing appears, then the next letter
     * comes out accented. A pair that does not combine emits both, which is
     * what every other system does and what people expect -- and pressing the
     * accent twice gives the literal accent, which is how you type one. */
    uint8_t u = (uint8_t)c;
    if (dead_pending) {
        uint8_t acc = dead_pending;
        dead_pending = 0;
        if (u == ' ') { push((char)acc); return; }        /* accent + space = accent */
        uint8_t combined = kbd_dead_compose(acc, u);
        if (combined) { push((char)combined); return; }
        push((char)acc);                                   /* no pair: both, in order */
        push(c);
        return;
    }
    if (kbd_is_dead(u)) { dead_pending = u; return; }

    push(c);
}

int kbd_ctrl_down(void)  { return ctrl;  }
int kbd_alt_down(void)   { return alt;   }
int kbd_shift_down(void) { return shift; }
int kbd_win_down(void)   { return win;   }

/* Inject an already-decoded character into the input ring. Used by the USB HID
 * keyboard driver, which decodes boot-protocol reports itself and feeds the
 * same ring the PS/2 path uses, so the shell/GUI are transport-agnostic. */
void kbd_inject(char c) { push(c); }

int kbd_trygetc(void) {
    if (rhead == rtail) return -1;
    char c = rb[rhead];
    rhead = (uint8_t)(rhead + 1);
    return (int)(unsigned char)c;
}

char kbd_getc(void) {
    int c;
    while ((c = kbd_trygetc()) < 0) __asm__ volatile("hlt");
    return (char)c;
}

void keyboard_init(void) {
    /* The built-in bindings, so shortcuts work from the first keypress.
     * keymap_init() re-applies these and then loads /etc/keys.conf, which
     * cannot happen until there is a filesystem to read it from. */
    keymap_reset();
    rhead = rtail = 0;
    arhead = artail = 0;
    shift = 0;
    ctrl = 0;
    alt = 0;
    altgr = 0;
    win = 0;
    win_used = 0;
    extended = 0;
    dead_pending = 0;
    /* The built-in US layout, so keys work from the first press. kbd_layout_init()
     * re-applies it and then loads /etc/keyboard.conf, which cannot happen until
     * there is a filesystem to read it from. */
    kbd_layout_set(0);

    /* Drain the 8042 before installing the handler, and again after.
     *
     * A byte left in the controller by the firmware or the boot menu produced
     * its interrupt at a moment when there was nothing to take it, and the
     * 8042 will not raise another until that byte is read. One `inb` used to
     * be enough for the single byte the boot menu leaves; it is not enough for
     * somebody who taps a few keys while the machine is starting, and the
     * consequence was not a lost keystroke -- it was a keyboard that never
     * worked again until the next reboot. Which is a thing people do.
     *
     * So: loop, and consult the status register (bit 0 = output buffer full)
     * rather than reading blind. Bounded, because a controller that always
     * claims to have a byte should not stop the boot. Draining once more after
     * irq_install closes the window between the two. */
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) inb(0x60);
    irq_install(1, on_key);
    for (int i = 0; i < 64 && (inb(0x64) & 1); i++) inb(0x60);
}
