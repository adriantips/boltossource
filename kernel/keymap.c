/* ===========================================================================
 *  BoltOS  -  kernel/keymap.c
 *  The global shortcut table: names, defaults, text form, and the file.
 *
 *  The defaults are exactly what was hard-coded before, so nothing changes for
 *  anyone who does not go looking. What changes is that they can now be looked
 *  at, and changed, and that the change survives a reboot.
 * ========================================================================= */
#include <stdint.h>
#include <stdio.h>
#include "keymap.h"
#include "keyboard.h"
#include "fs.h"
#include "kprintf.h"
#include "string.h"

#define KEYS_PATH "/etc/keys.conf"

typedef struct {
    const char *name;
    const char *desc;
    keybind_t   def;
} action_def;

/* NB: order must match the enum. */
static const action_def ACTIONS[ACT_COUNT] = {
 { "start-menu",     "Open or close the Start menu",        { KMOD_WIN, 0 } },
 { "cycle-window",   "Focus the next window",               { KMOD_ALT, KEYK_TAB } },
 { "cycle-back",     "Focus the previous window",           { KMOD_ALT | KMOD_SHIFT, KEYK_TAB } },
 { "close-window",   "Close the focused window",            { KMOD_ALT, KEYK_F1 + 3 } },
 { "show-desktop",   "Minimise everything, or put it back", { KMOD_WIN, 'd' } },
 { "lock",           "Lock the session",                    { KMOD_CTRL | KMOD_ALT, 'l' } },
 { "screenshot",     "Capture the screen to a PNG",         { 0, KEYK_F12 } },
 { "snap-left",      "Snap the window to the left half",    { KMOD_WIN, KEY_LEFT } },
 { "snap-right",     "Snap the window to the right half",   { KMOD_WIN, KEY_RIGHT } },
 { "snap-max",       "Maximise the window",                 { KMOD_WIN, KEY_UP } },
 { "snap-restore",   "Restore or minimise the window",      { KMOD_WIN, KEY_DOWN } },
 { "desk-1",         "Go to desktop 1",                     { KMOD_WIN, '1' } },
 { "desk-2",         "Go to desktop 2",                     { KMOD_WIN, '2' } },
 { "desk-3",         "Go to desktop 3",                     { KMOD_WIN, '3' } },
 { "desk-4",         "Go to desktop 4",                     { KMOD_WIN, '4' } },
 { "desk-next",      "Go to the next desktop",              { KMOD_CTRL | KMOD_ALT, KEY_RIGHT } },
 { "desk-prev",      "Go to the previous desktop",          { KMOD_CTRL | KMOD_ALT, KEY_LEFT } },
 { "desk-move-next", "Take the window to the next desktop", { KMOD_WIN | KMOD_SHIFT, KEY_RIGHT } },
 { "desk-move-prev", "Take the window to the previous one",  { KMOD_WIN | KMOD_SHIFT, KEY_LEFT } },
 { "record",         "Start or stop recording the screen",   { KMOD_CTRL, KEYK_F12 } },
};

static keybind_t bindings[ACT_COUNT];

int         keymap_count(void) { return ACT_COUNT; }
static int  valid(int a) { return a >= 0 && a < ACT_COUNT; }
const char *keymap_name(int a)     { return valid(a) ? ACTIONS[a].name : "?"; }
const char *keymap_describe(int a) { return valid(a) ? ACTIONS[a].desc : ""; }
keybind_t   keymap_binding(int a)  { keybind_t z = { 0, 0 }; return valid(a) ? bindings[a] : z; }
keybind_t   keymap_default(int a)  { keybind_t z = { 0, 0 }; return valid(a) ? ACTIONS[a].def : z; }

int keymap_action_by_name(const char *name) {
    if (!name) return ACT_NONE;
    for (int i = 0; i < ACT_COUNT; i++) if (strcmp(name, ACTIONS[i].name) == 0) return i;
    return ACT_NONE;
}

void keymap_reset(void) {
    for (int i = 0; i < ACT_COUNT; i++) bindings[i] = ACTIONS[i].def;
}

/* The Start menu's default is the Super key tapped on its own, which is a
 * chord with no key at all. Everything else needs one. */
int keymap_lookup(uint8_t mods, uint8_t key) {
    if (!key) return ACT_NONE;
    for (int i = 0; i < ACT_COUNT; i++)
        if (bindings[i].key == key && bindings[i].mods == mods) return i;
    return ACT_NONE;
}

int keymap_tap(uint8_t mods) {
    for (int i = 0; i < ACT_COUNT; i++)
        if (!bindings[i].key && bindings[i].mods == mods) return i;
    return ACT_NONE;
}

int keymap_bind(int action, keybind_t b, int *clash) {
    if (!valid(action)) return -1;
    if (b.key) {
        for (int i = 0; i < ACT_COUNT; i++) {
            if (i == action) continue;
            if (bindings[i].key == b.key && bindings[i].mods == b.mods) {
                if (clash) *clash = i;
                return -2;
            }
        }
    }
    bindings[action] = b;
    return 0;
}

/* ------------------------------------------------------------ text form --- */
typedef struct { const char *name; uint8_t code; } keyname;

/* Only keys that make sense as a shortcut. A binding to a letter with no
 * modifier would swallow that letter everywhere, so the parser refuses it. */
static const keyname KEYNAMES[] = {
    { "Left",  KEY_LEFT },  { "Right", KEY_RIGHT }, { "Up",    KEY_UP },
    { "Down",  KEY_DOWN },  { "Home",  KEY_HOME },  { "End",   KEY_END },
    { "PgUp",  KEY_PGUP },  { "PgDn",  KEY_PGDN },  { "Del",   KEY_DEL },
    { "Tab",   KEYK_TAB },  { "Esc",   KEYK_ESC },  { "Space", KEYK_SPACE },
    { "Enter", '\n' },      { "Back",  '\b' },
    { 0, 0 }
};

void keymap_format(keybind_t b, char *out, uint32_t cap) {
    if (!cap) return;
    if (!b.key && !b.mods) { snprintf(out, cap, "none"); return; }
    char buf[32]; uint32_t n = 0;
    buf[0] = 0;
    #define ADD(s) do { for (const char *p = (s); *p && n < sizeof buf - 1; p++) buf[n++] = *p; buf[n] = 0; } while (0)
    if (b.mods & KMOD_CTRL)  ADD("Ctrl+");
    if (b.mods & KMOD_ALT)   ADD("Alt+");
    if (b.mods & KMOD_SHIFT) ADD("Shift+");
    if (b.mods & KMOD_WIN)   ADD("Win+");
    if (!b.key) {                       /* a modifier tapped on its own */
        if (n) buf[n - 1] = 0;          /* drop the trailing '+' */
        snprintf(out, cap, "%s", buf);
        return;
    }
    if (b.key >= KEYK_F1 && b.key <= KEYK_F12) {
        char f[6]; snprintf(f, sizeof f, "F%d", b.key - KEYK_F1 + 1);
        ADD(f);
    } else {
        const keyname *k = KEYNAMES;
        for (; k->name; k++) if (k->code == b.key) { ADD(k->name); break; }
        if (!k->name) {
            char one[2] = { (char)(b.key >= 'a' && b.key <= 'z' ? b.key - 32 : b.key), 0 };
            ADD(one);
        }
    }
    #undef ADD
    snprintf(out, cap, "%s", buf);
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return !*a && !*b;
}

int keymap_parse(const char *text, keybind_t *out) {
    if (!text || !out) return -1;
    keybind_t b = { 0, 0 };
    if (ieq(text, "none")) { *out = b; return 0; }

    char part[24];
    const char *p = text;
    for (;;) {
        uint32_t n = 0;
        while (*p && *p != '+' && n < sizeof part - 1) part[n++] = *p++;
        part[n] = 0;
        int last = (*p != '+');
        if (*p == '+') p++;

        if      (ieq(part, "ctrl"))  b.mods |= KMOD_CTRL;
        else if (ieq(part, "alt"))   b.mods |= KMOD_ALT;
        else if (ieq(part, "shift")) b.mods |= KMOD_SHIFT;
        else if (ieq(part, "win") || ieq(part, "super") || ieq(part, "meta"))
                                     b.mods |= KMOD_WIN;
        else {
            /* the key itself, which must be the last component */
            if (!last) return -1;
            if ((part[0] == 'f' || part[0] == 'F') && part[1] >= '0' && part[1] <= '9') {
                int fn = part[1] - '0';
                if (part[2] >= '0' && part[2] <= '9') fn = fn * 10 + (part[2] - '0');
                else if (part[2]) return -1;
                if (fn < 1 || fn > 12) return -1;
                b.key = (uint8_t)(KEYK_F1 + fn - 1);
            } else {
                const keyname *k = KEYNAMES;
                for (; k->name; k++) if (ieq(part, k->name)) { b.key = k->code; break; }
                if (!k->name) {
                    if (n != 1) return -1;
                    char c = part[0];
                    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                    if (c < 32 || c > 126) return -1;
                    b.key = (uint8_t)c;
                }
            }
        }
        if (last) break;
    }

    /* A bare key with no modifier would take that character away from every
     * program on the machine. Function keys are the exception -- nothing types
     * an F key. */
    if (b.key && !b.mods && !(b.key >= KEYK_F1 && b.key <= KEYK_F12)) return -2;
    *out = b;
    return 0;
}

/* ------------------------------------------------------------- the file --- */
int keymap_save(void) {
    char text[1400];
    uint32_t n = 0;
    #define OUT(...) do { n += (uint32_t)snprintf(text + n, sizeof text - n, __VA_ARGS__); \
                          if (n >= sizeof text) return -1; } while (0)
    OUT("# BoltOS global shortcuts. `keys` edits this; `keys reset` restores\n"
        "# the defaults. One action per line: NAME = CHORD, or NAME = none.\n");
    for (int i = 0; i < ACT_COUNT; i++) {
        char b[32]; keymap_format(bindings[i], b, sizeof b);
        OUT("%s = %s\n", ACTIONS[i].name, b);
    }
    #undef OUT

    fs_node *f = fs_lookup(KEYS_PATH);
    if (!f) {
        if (!fs_lookup("/etc")) fs_create("/etc", 1);
        f = fs_create(KEYS_PATH, 0);
    }
    if (!f || f->is_dir) return -1;
    return fs_write(f, text, n) == 0 ? 0 : -1;
}

int keymap_load(void) {
    fs_node *f = fs_lookup(KEYS_PATH);
    if (!f || f->is_dir || !f->data || !f->size) return 0;

    const char *p = (const char *)f->data;
    uint32_t left = f->size;
    while (left) {
        /* one line */
        char line[96]; uint32_t n = 0;
        while (left && *p != '\n') { if (n < sizeof line - 1) line[n++] = *p; p++; left--; }
        if (left) { p++; left--; }
        line[n] = 0;

        char *eq = 0;
        for (uint32_t i = 0; i < n; i++) if (line[i] == '#') { line[i] = 0; n = i; break; }
        for (uint32_t i = 0; i < n; i++) if (line[i] == '=') { eq = line + i; break; }
        if (!eq) continue;
        *eq = 0;
        char *name = line, *val = eq + 1;
        while (*name == ' ' || *name == '\t') name++;
        for (char *e = name + strlen(name); e > name && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*val == ' ' || *val == '\t') val++;
        for (char *e = val + strlen(val); e > val && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'); ) *--e = 0;

        int a = keymap_action_by_name(name);
        keybind_t b;
        if (a == ACT_NONE || keymap_parse(val, &b) != 0) {
            kprintf("[--] keys: %s ignores '%s = %s'\n", KEYS_PATH, name, val);
            continue;
        }
        /* A file that binds two actions to one chord is the user's problem to
         * see, not a reason to refuse to boot: the later line loses. */
        int clash = -1;
        if (keymap_bind(a, b, &clash) == -2)
            kprintf("[--] keys: %s already uses that chord; %s left alone\n",
                    ACTIONS[clash].name, name);
    }
    return 1;
}

void keymap_init(void) {
    keymap_reset();
    keymap_load();
}
