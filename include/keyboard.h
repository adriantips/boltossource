#pragma once
/* PS/2 keyboard driver. The IRQ1 handler decodes scancodes (with shift) into
 * ASCII and pushes them onto a ring buffer; the shell drains it below. */
/* Extended keys are delivered as otherwise-unused control bytes so the existing
 * "c >= 32 is printable" callers ignore them while editors can act on them. */
#define KEY_UP    0x11
#define KEY_DOWN  0x12
#define KEY_LEFT  0x13
#define KEY_RIGHT 0x14
#define KEY_HOME  0x02
#define KEY_END   0x05
#define KEY_PGUP  0x10
#define KEY_PGDN  0x0E
#define KEY_DEL   0x7F
#define KEY_SHOT  0x17   /* F12: capture a screenshot (handled globally by GUI) */
#define KEY_ALTTAB  0x0B /* Alt+Tab: cycle to next window (handled by GUI)      */
#define KEY_ALTTABR 0x0C /* Shift+Alt+Tab: cycle to previous window             */
#define KEY_ALTF4   0x04 /* Alt+F4: close the focused window (handled by GUI)   */
#define KEY_WINTAP  0x07 /* Super/Win tapped alone: toggle Start menu (GUI)     */

/* Clipboard / editing shortcuts. Emitted by the driver when Ctrl is held with
 * the matching letter. The values are the classic ASCII control codes and were
 * chosen to not collide with any KEY_* above. */
#define KEY_SELALL 0x01   /* Ctrl+A */
#define KEY_COPY   0x03   /* Ctrl+C */
#define KEY_PASTE  0x16   /* Ctrl+V */
#define KEY_CUT    0x18   /* Ctrl+X */
#define KEY_SAVE   0x19   /* Ctrl+S */
#define KEY_LOCK   0x1A   /* Ctrl+Alt+L: lock the session (handled by the GUI) */
#define KEY_CUTLINE 0x0F  /* Ctrl+K: cut the current line (the terminal editor) */

/* Shift+navigation keys, for extending a text selection. Distinct control bytes
 * so editors can tell "move caret" from "extend selection". */
#define KEY_SLEFT  0x1C
#define KEY_SRIGHT 0x1D
#define KEY_SUP    0x1E
#define KEY_SDOWN  0x1F
#define KEY_SHOME  0x06
#define KEY_SEND   0x15

/* Returns 1 if a keyboard answered ECHO on the PS/2 port. Zero is normal on a
 * modern board, where the keyboard is a USB device and reaches this driver
 * through kbd_inject() -- note that the 8042 itself usually still answers there
 * with nothing plugged into it, which is why these are two separate questions:
 * kbd_ps2_present() is the controller, kbd_ps2_keyboard() is a keyboard. */
int keyboard_init(void);
int kbd_ps2_present(void);
int kbd_ps2_keyboard(void);
int  kbd_ctrl_down(void);  /* 1 while either Ctrl key is held */
int  kbd_alt_down(void);   /* 1 while either Alt key is held  */
int  kbd_shift_down(void); /* 1 while either Shift key is held */
int  kbd_win_down(void);   /* 1 while either Super/Win key is held */
void kbd_inject(char c);   /* push a decoded char into the ring (USB HID path) */
int  kbd_trygetc(void);   /* next char, or -1 if the buffer is empty (non-blocking) */
char kbd_getc(void);      /* block (hlt) until a key is available, then return it */

/* Raw scancode tap for clients needing real key up/down (DOOM). When enabled,
 * every make/break is queued; kbd_raw_get() returns the next event or -1.
 * Encoding: bits0-6 scancode, 0x100 = release, 0x200 = extended (0xE0). */
void kbd_raw_enable(int on);
int  kbd_raw_get(void);

/* Is this byte a character somebody meant to type?
 *
 * Every text field in the OS was asking `c >= 32 && c < 127`, in about a dozen
 * places, and every one of them silently dropped everything a non-US keyboard
 * produces -- so a German user naming a file with an umlaut in it got the name
 * without the umlaut and no indication anything had happened. One predicate,
 * so the answer cannot differ between the terminal and the editor.
 *
 * 0x80..0x9F stays out: those are control codes in Latin-1 and no layout
 * produces them, with the single exception of the euro, which lives at 0x80
 * because it postdates the standard and there is nowhere else for it.
 *
 * This does NOT admit the low control bytes -- the key ring also carries
 * navigation codes there, and inserting a KEY_UP into a password field is
 * never what was meant. */
static inline int kbd_is_text(unsigned char c) {
    return (c >= 32 && c < 127) || c == 0x80 || c >= 0xA0;
}

/* The next resolved global shortcut, or -1 if none is pending. A parallel ring
 * to the character one: an action is not a character, and once the character
 * ring started carrying Latin-1 there was no byte left to pretend it was.
 * Returns an ACT_* value from keymap.h. */
int  kbd_action_get(void);
