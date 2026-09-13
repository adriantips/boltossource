#include <stdint.h>
#include <stdio.h>
#include "commands.h"
#include "kbdlayout.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_kbd.c
 *  `kbd` -- which keyboard is in front of you.
 *
 *  `kbd show` exists because a layout is the one setting you cannot check by
 *  trying it: if the layout is wrong, every attempt to type the thing you want
 *  to test produces the wrong character, including the command you would use to
 *  fix it. So it prints the rows, and you compare them with the key caps.
 * ===========================================================================*/

static void usage(void) {
    kprintf("usage: kbd                 list the layouts, marking the active one\n"
            "       kbd NAME            switch to one (kbd de, kbd fr, ...)\n"
            "       kbd show [NAME]     print what each key produces\n"
            "       kbd save            write /etc/keyboard.conf now\n");
}

static void list(void) {
    for (int i = 0; i < kbd_layout_count(); i++)
        kprintf("%c %-9s %s\n", i == kbd_layout_current() ? '*' : ' ',
                kbd_layout_name(i), kbd_layout_label(i));
    kprintf("  (* = active; kept in /etc/keyboard.conf and reloaded at boot)\n");
    kprintf("  Latin-1 only. Polish, Czech, Turkish, Greek and Cyrillic need\n"
            "  UTF-8 through the console, the editors and the filesystem first,\n"
            "  so they are absent rather than present and typing blanks.\n");
}

/* One byte as something you can read in a terminal. The high half of Latin-1
 * has no name most people know, so it is printed as the character AND its
 * value -- on a console that cannot draw it, the number is still the answer. */
static void put(uint8_t c) {
    if (!c)            kprintf("   .  ");
    else if (c == ' ') kprintf("  sp  ");
    else if (c < 32)   kprintf(" ^%c   ", '@' + c);
    else if (c < 127)  kprintf("  %c   ", (char)c);
    else               kprintf(" %c/%02X ", (char)c, c);
}

/* The rows in the order they sit on the keyboard, by scancode. */
static const uint8_t ROW1[] = { 0x29,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0 };
static const uint8_t ROW2[] = { 0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,0 };
static const uint8_t ROW3[] = { 0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x2B,0 };
static const uint8_t ROW4[] = { 0x56,0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x32,0x33,0x34,0x35,0 };

static void show_row(const uint8_t *row, int level) {
    for (const uint8_t *p = row; *p; p++) put(kbd_layout_char(*p, level));
    kprintf("\n");
}

static void show(void) {
    static const char *LEVEL[] = { "plain", "shift", "altgr" };
    kprintf("layout: %s (%s)\n", kbd_layout_name(kbd_layout_current()),
            kbd_layout_label(kbd_layout_current()));
    for (int lv = 0; lv < 3; lv++) {
        kprintf("-- %s --\n", LEVEL[lv]);
        show_row(ROW1, lv);
        show_row(ROW2, lv);
        show_row(ROW3, lv);
        show_row(ROW4, lv);
    }
    kprintf("A '.' is a key this layout leaves empty at that level. AltGr is the\n"
            "RIGHT Alt key, which is not Alt: on de/fr/es/it it is the only way\n"
            "to type @ and the braces.\n");

    /* Dead keys are the thing people report as a broken keyboard, so say so. */
    int any = 0;
    for (int sc = 0; sc < 0x60; sc++) {
        for (int lv = 0; lv < 2; lv++) {
            uint8_t c = kbd_layout_char((uint8_t)sc, lv);
            if (!c || !kbd_is_dead(c)) continue;
            if (!any) {
                kprintf("dead keys: ");
                any = 1;
            }
            kprintf("%c ", (char)c);
        }
    }
    if (any)
        kprintf("\n  A dead key types nothing on its own -- the next letter comes\n"
                "  out accented. Press it twice, or follow it with a space, for\n"
                "  the accent itself.\n");
}

int cmd_kbd(int argc, char **argv) {
    if (argc < 2) { list(); return 0; }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) { usage(); return 0; }
    if (strcmp(argv[1], "list") == 0) { list(); return 0; }

    if (strcmp(argv[1], "save") == 0) {
        if (kbd_layout_save() != 0) { kprintf("kbd: could not write /etc/keyboard.conf\n"); return 1; }
        kprintf("kbd: saved to /etc/keyboard.conf\n");
        return 0;
    }

    if (strcmp(argv[1], "show") == 0) {
        int back = kbd_layout_current();
        if (argc > 2) {
            int i = kbd_layout_by_name(argv[2]);
            if (i < 0) { kprintf("kbd: no layout '%s'\n", argv[2]); return 1; }
            kbd_layout_set(i);
        }
        show();
        /* Looking at a layout must not leave you typing on it. */
        if (argc > 2) kbd_layout_set(back);
        return 0;
    }

    int i = kbd_layout_by_name(argv[1]);
    if (i < 0) {
        kprintf("kbd: no layout '%s' (run 'kbd' to list them)\n", argv[1]);
        return 1;
    }
    kbd_layout_set(i);
    if (kbd_layout_save() != 0)
        kprintf("kbd: switched, but /etc/keyboard.conf could not be written\n");
    kprintf("kbd: %s (%s)\n", kbd_layout_name(i), kbd_layout_label(i));
    return 0;
}
