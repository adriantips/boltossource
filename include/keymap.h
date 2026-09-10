#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Global keyboard shortcuts.
 *
 *  The desktop's shortcuts were spelled out in the code: Alt+Tab was baked
 *  into the keyboard driver as a synthetic byte, Win+D was an `if` in the
 *  compositor, Ctrl+Alt+L was a special case inside ctrl_shortcut(). All of
 *  them worked and none of them could be changed, which is a problem for
 *  anybody whose keyboard, layout or habits disagree -- and a bigger one for
 *  anybody whose hardware puts Super somewhere unreachable.
 *
 *  So the bindings are data. One table of named actions, each with a chord;
 *  the keyboard driver asks this table before it decides what a keypress
 *  means, and the compositor is handed an action rather than a keycode.
 *  `keys` edits them and /etc/keys.conf keeps them.
 * ========================================================================= */

/* Modifier bits. Left and right are not distinguished: nobody has ever wanted
 * a shortcut that fires on one Shift and not the other. */
#define KMOD_CTRL   0x01
#define KMOD_SHIFT  0x02
#define KMOD_ALT     0x04
#define KMOD_WIN    0x08

/* Logical keys, on top of the KEY_* codes in keyboard.h. Function keys have no
 * ASCII, so they get their own range above anything the driver delivers. */
#define KEYK_F1     0x90
#define KEYK_F12    0x9B
#define KEYK_TAB    0x09
#define KEYK_ESC    0x1B
#define KEYK_SPACE  0x20

/* What a chord can do. Add to the end; the names are the on-disk format. */
enum {
    ACT_NONE = -1,
    ACT_START_MENU = 0,
    ACT_CYCLE_WINDOW,
    ACT_CYCLE_WINDOW_BACK,
    ACT_CLOSE_WINDOW,
    ACT_SHOW_DESKTOP,
    ACT_LOCK,
    ACT_SCREENSHOT,
    ACT_SNAP_LEFT,
    ACT_SNAP_RIGHT,
    ACT_SNAP_MAX,
    ACT_SNAP_RESTORE,
    ACT_DESK_1, ACT_DESK_2, ACT_DESK_3, ACT_DESK_4,
    ACT_DESK_NEXT, ACT_DESK_PREV,
    ACT_DESK_MOVE_NEXT, ACT_DESK_MOVE_PREV,
    ACT_COUNT
};

/* An action delivered to the compositor arrives in the ordinary key ring as
 * this byte plus the action number, which is why ACT_COUNT must stay small.
 * 0xC0 is above every code the driver produces and every printable character. */
#define KEY_ACTION_BASE 0xC0
#define KEY_IS_ACTION(c) ((unsigned char)(c) >= KEY_ACTION_BASE && \
                          (unsigned char)(c) < KEY_ACTION_BASE + ACT_COUNT)
#define KEY_TO_ACTION(c) ((int)((unsigned char)(c) - KEY_ACTION_BASE))

typedef struct { uint8_t mods, key; } keybind_t;

void keymap_init(void);                  /* defaults, then /etc/keys.conf     */
int  keymap_load(void);                  /* re-read the file; 1 if it existed */
int  keymap_save(void);                  /* write it; 0 on success            */
void keymap_reset(void);                 /* back to the built-in defaults     */

/* The lookup the keyboard driver does on every keypress that carries a
 * modifier. ACT_NONE means "not a shortcut, deliver the key normally". */
int  keymap_lookup(uint8_t mods, uint8_t key);

/* A modifier tapped and released with no other key: the Super key's default.
 * ACT_NONE if nothing is bound to that modifier alone. */
int  keymap_tap(uint8_t mods);

/* Introspection, for `keys` and the Settings page. */
int         keymap_count(void);
const char *keymap_name(int action);         /* "show-desktop"          */
const char *keymap_describe(int action);     /* "Minimise every window" */
int         keymap_action_by_name(const char *name);   /* ACT_NONE if unknown */
keybind_t   keymap_binding(int action);
keybind_t   keymap_default(int action);

/* Set a binding. Returns 0, -1 for a bad action, or -2 if that chord is
 * already taken (by `clash`, which is filled in when non-NULL). A binding with
 * key == 0 is "unbound", and is always allowed. */
int  keymap_bind(int action, keybind_t b, int *clash);

/* "Ctrl+Alt+L" <-> a binding. format() writes at least 24 bytes; parse()
 * returns 0 on success. "none" parses to an unbound chord. */
void keymap_format(keybind_t b, char *out, uint32_t cap);
int  keymap_parse(const char *text, keybind_t *out);
