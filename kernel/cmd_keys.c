#include <stdint.h>
#include <stdio.h>
#include "commands.h"
#include "keymap.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_keys.c
 *  `keys` -- look at the global shortcuts, and change them.
 * ===========================================================================*/

static void usage(void) {
    kprintf("usage: keys                       list every shortcut\n"
            "       keys ACTION CHORD          bind one (e.g. keys lock Ctrl+Alt+K)\n"
            "       keys ACTION none           unbind one\n"
            "       keys reset [ACTION]        restore the default(s)\n"
            "       keys save                  write /etc/keys.conf now\n"
            "  A chord is modifiers joined by '+' then one key: Ctrl, Alt, Shift,\n"
            "  Win; Left/Right/Up/Down, Home/End, PgUp/PgDn, Del, Tab, Esc, Space,\n"
            "  Enter, Back, F1..F12, or a single character. Bare characters are\n"
            "  refused -- binding 'k' alone would take that letter from every\n"
            "  program on the machine. F-keys are allowed without a modifier.\n");
}

static void list(void) {
    kprintf("  %-16s %-20s %s\n", "action", "shortcut", "what it does");
    for (int a = 0; a < keymap_count(); a++) {
        char b[32], d[32];
        keymap_format(keymap_binding(a), b, sizeof b);
        keymap_format(keymap_default(a), d, sizeof d);
        int changed = strcmp(b, d) != 0;
        kprintf("%c %-16s %-20s %s\n", changed ? '*' : ' ',
                keymap_name(a), b, keymap_describe(a));
    }
    kprintf("  (* = changed from the default; kept in /etc/keys.conf)\n");
}

int cmd_keys(int argc, char **argv) {
    if (argc < 2) { list(); return 0; }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) { usage(); return 0; }
    if (strcmp(argv[1], "list") == 0) { list(); return 0; }

    if (strcmp(argv[1], "save") == 0) {
        if (keymap_save() != 0) { kprintf("keys: could not write /etc/keys.conf\n"); return 1; }
        kprintf("keys: saved to /etc/keys.conf\n");
        return 0;
    }

    if (strcmp(argv[1], "reset") == 0) {
        if (argc < 3) {
            keymap_reset();
            kprintf("keys: every shortcut back to its default\n");
        } else {
            int a = keymap_action_by_name(argv[2]);
            if (a == ACT_NONE) { kprintf("keys: no action '%s'\n", argv[2]); return 1; }
            int clash = -1;
            if (keymap_bind(a, keymap_default(a), &clash) == -2) {
                kprintf("keys: %s holds that chord; unbind it first\n", keymap_name(clash));
                return 1;
            }
            char b[32]; keymap_format(keymap_binding(a), b, sizeof b);
            kprintf("keys: %s = %s\n", keymap_name(a), b);
        }
        keymap_save();
        return 0;
    }

    if (argc < 3) { usage(); return 1; }

    int a = keymap_action_by_name(argv[1]);
    if (a == ACT_NONE) {
        kprintf("keys: no action '%s' (run 'keys' to list them)\n", argv[1]);
        return 1;
    }
    keybind_t b;
    int rc = keymap_parse(argv[2], &b);
    if (rc == -2) {
        kprintf("keys: '%s' has no modifier; that would take the key from every\n"
                "      program. Add Ctrl, Alt, Shift or Win, or use an F-key.\n", argv[2]);
        return 1;
    }
    if (rc != 0) { kprintf("keys: cannot read the chord '%s'\n", argv[2]); return 1; }

    int clash = -1;
    if (keymap_bind(a, b, &clash) == -2) {
        char c[32]; keymap_format(keymap_binding(clash), c, sizeof c);
        kprintf("keys: %s is already %s; unbind it first ('keys %s none')\n",
                keymap_name(clash), c, keymap_name(clash));
        return 1;
    }

    char shown[32]; keymap_format(b, shown, sizeof shown);
    kprintf("keys: %s = %s\n", keymap_name(a), shown);
    if (keymap_save() != 0) kprintf("keys: (not saved -- /etc/keys.conf is not writable)\n");
    return 0;
}
