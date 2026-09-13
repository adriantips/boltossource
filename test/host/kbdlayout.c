/* kernel/kbdlayout.c on the host, where every layout can be typed at once.
 *
 * A keyboard layout is a table, and a table with one entry in the wrong place
 * is the most quietly annoying bug an OS can ship: everything works, and then
 * one key on one layout does the wrong thing, and the person it happens to
 * assumes they are holding it wrong. Booting a VM and typing at it can check a
 * handful of keys on one layout in about a minute. This checks every key on
 * every layout in about a millisecond, which is the difference between
 * "probably fine" and "checked".
 *
 * The oracle is what is printed on the key caps. Each layout below lists the
 * characters its top letter row produces, the punctuation people actually
 * reach for, and the third level -- and those are facts about real keyboards
 * that can be looked up rather than derived, which is exactly what a test
 * wants.
 *
 * The scancode numbering is the trap this was written after walking into: the
 * digit row starts at 0x02, so the key marked '3' is scancode 0x04. Half the
 * layouts had their digit-row overrides one place out, which produced a
 * keyboard that was wrong in exactly one spot per layout.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "kbdlayout.h"
#include "fs.h"

/* The filesystem and the log, stubbed. kbd_layout_save()/load() are the only
 * things here that touch either, and what they do with a file is checked in
 * QEMU by test/t_kbd.py, where there is a real one. On the host they are asked
 * to do nothing and must not crash doing it -- a load() that faults on a
 * missing file would take the boot down on a fresh install. */
fs_node *fs_lookup(const char *path) { (void)path; return 0; }
fs_node *fs_create(const char *path, int is_dir) { (void)path; (void)is_dir; return 0; }
int fs_write(fs_node *n, const void *data, uint32_t len) {
    (void)n; (void)data; (void)len; return -1;
}
void kprintf(const char *fmt, ...) { (void)fmt; }

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

/* Scancodes of the three letter rows and the digit row, in keyboard order. */
static const uint8_t DIGITS[] = { 0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D };
static const uint8_t ROW_Q[]  = { 0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19 };
static const uint8_t ROW_A[]  = { 0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26 };
static const uint8_t ROW_Z[]  = { 0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x32 };

static void use(const char *name) {
    int i = kbd_layout_by_name(name);
    if (i < 0) { printf("FAIL: no layout %s\n", name); fails++; return; }
    kbd_layout_set(i);
}

static void row(const char *lay, const char *what, const uint8_t *sc, int n,
                int level, const char *expect) {
    char got[16];
    for (int i = 0; i < n; i++) got[i] = (char)kbd_layout_char(sc[i], level);
    got[n] = 0;
    CHECK(memcmp(got, expect, n) == 0,
          "%s %s: got \"%s\", the key caps say \"%s\"", lay, what, got, expect);
}

static void key(const char *lay, uint8_t sc, int level, uint8_t expect, const char *why) {
    uint8_t got = kbd_layout_char(sc, level);
    CHECK(got == expect, "%s: scancode 0x%02X level %d gave 0x%02X (%c), "
                         "expected 0x%02X (%c) -- %s",
          lay, sc, level, got, got >= 32 && got < 127 ? got : '?',
          expect, expect >= 32 && expect < 127 ? expect : '?', why);
}

int main(void) {
    kbd_layout_init();

    /* ---- every layout, whatever it is ---------------------------------- */
    CHECK(kbd_layout_count() >= 6, "only %d layouts", kbd_layout_count());
    for (int i = 0; i < kbd_layout_count(); i++) {
        kbd_layout_set(i);
        const char *n = kbd_layout_name(i);
        /* Space, enter, backspace and tab are not negotiable on any keyboard
         * ever made, and a layout that dropped one would be unusable in a way
         * no per-layout assertion below would catch. */
        key(n, 0x39, 0, ' ',  "space");
        key(n, 0x1C, 0, '\n', "enter");
        key(n, 0x0E, 0, '\b', "backspace");
        key(n, 0x0F, 0, '\t', "tab");
        key(n, 0x01, 0, 27,   "escape");
        /* Every letter must be reachable unshifted somewhere on the board. */
        int seen[26] = { 0 };
        for (int sc = 0; sc < 0x60; sc++) {
            uint8_t c = kbd_layout_char((uint8_t)sc, 0);
            if (c >= 'a' && c <= 'z') seen[c - 'a'] = 1;
        }
        for (int l = 0; l < 26; l++)
            CHECK(seen[l], "%s cannot type the letter '%c' at all", n, 'a' + l);
        /* And shift must produce its capital, or half the alphabet is lost. */
        for (int sc = 0; sc < 0x60; sc++) {
            uint8_t lo = kbd_layout_char((uint8_t)sc, 0);
            if (lo < 'a' || lo > 'z') continue;
            uint8_t up = kbd_layout_char((uint8_t)sc, 1);
            CHECK(up == lo - 32, "%s: shift on the '%c' key gives 0x%02X, not '%c'",
                  n, lo, up, lo - 32);
        }
    }

    /* ---- US: unchanged, which is the point ----------------------------- */
    use("us");
    row("us", "qwerty row", ROW_Q, 10, 0, "qwertyuiop");
    row("us", "digit row",  DIGITS, 12, 0, "1234567890-=");
    row("us", "shifted digits", DIGITS, 12, 1, "!@#$%^&*()_+");
    key("us", 0x07, 1, '^', "Shift+6 is a caret");
    CHECK(!kbd_is_dead('^'), "'^' must not be a dead key on US -- you could "
                             "not type a caret in C");
    CHECK(!kbd_is_dead('\''), "the apostrophe must not be dead on US -- every "
                              "contraction would lose a letter");

    /* ---- US-International: the same board, accents on the punctuation --- */
    use("us-intl");
    row("us-intl", "qwerty row", ROW_Q, 10, 0, "qwertyuiop");
    CHECK(kbd_is_dead(0xB4), "us-intl: the apostrophe key should be a dead acute");
    CHECK(kbd_is_dead('~'),  "us-intl: shift-backtick should be a dead tilde");
    key("us-intl", 0x28, 2, '\'', "AltGr rescues the literal apostrophe");
    key("us-intl", 0x06, 2, 0x80, "AltGr+5 is the euro");

    /* ---- UK ------------------------------------------------------------- */
    use("uk");
    key("uk", 0x04, 1, 0xA3, "Shift+3 is a pound sign, not a hash");
    key("uk", 0x03, 1, '"',  "Shift+2 is a quote, not an at-sign");
    key("uk", 0x28, 1, '@',  "the at-sign moved to Shift+'");
    key("uk", 0x2B, 0, '#',  "the hash key is next to Enter");
    key("uk", 0x29, 1, 0xAC, "Shift+backtick is a not-sign");
    key("uk", 0x56, 0, '\\', "the extra key left of Z is a backslash");

    /* ---- German: QWERTZ, three umlauts, and AltGr ----------------------- */
    use("de");
    row("de", "qwertz row", ROW_Q, 10, 0, "qwertzuiop");
    key("de", 0x2C, 0, 'y',  "the key where Z is on QWERTY types y");
    key("de", 0x15, 0, 'z',  "...and the Y key types z");
    key("de", 0x0C, 0, 0xDF, "the key right of 0 is sharp s");
    key("de", 0x1A, 0, 0xFC, "u with diaeresis is where [ is");
    key("de", 0x27, 0, 0xF6, "o with diaeresis is where ; is");
    key("de", 0x28, 0, 0xE4, "a with diaeresis is where ' is");
    key("de", 0x10, 2, '@',  "AltGr+Q is the ONLY way to type @ here");
    key("de", 0x08, 2, '{',  "AltGr+7 is a brace");
    key("de", 0x09, 2, '[',  "AltGr+8 is a bracket");
    key("de", 0x0C, 2, '\\', "AltGr+sharp-s is a backslash");
    key("de", 0x32, 2, 0xB5, "AltGr+M is micro");
    key("de", 0x04, 1, 0xA7, "Shift+3 is a section sign");
    key("de", 0x0B, 1, '=',  "Shift+0 is equals");
    CHECK(kbd_is_dead('^'), "de: the key left of 1 is a dead circumflex");
    CHECK(kbd_is_dead(0xB4), "de: the key right of sharp-s is a dead acute");

    /* ---- French: AZERTY, and digits that need Shift --------------------- */
    use("fr");
    row("fr", "azerty row", ROW_Q, 10, 0, "azertyuiop");
    row("fr", "home row",   ROW_A,  9, 0, "qsdfghjkl");
    key("fr", 0x2C, 0, 'w',  "the key where Z is on QWERTY types w");
    key("fr", 0x02, 0, '&',  "the 1 key types an ampersand unshifted");
    key("fr", 0x02, 1, '1',  "...and the digit needs Shift");
    key("fr", 0x03, 0, 0xE9, "the 2 key types e-acute");
    key("fr", 0x0B, 2, '@',  "AltGr+0 is the at-sign");
    key("fr", 0x32, 0, ',',  "the M key on QWERTY is a comma here");
    key("fr", 0x27, 0, 'm',  "...and M moved next to L");

    /* ---- Spanish -------------------------------------------------------- */
    use("es");
    key("es", 0x27, 0, 0xF1, "n-tilde has its own key, next to L");
    key("es", 0x27, 1, 0xD1, "and its capital");
    key("es", 0x0D, 0, 0xA1, "inverted exclamation right of the digits");
    key("es", 0x0D, 1, 0xBF, "inverted question on shift");
    key("es", 0x03, 2, '@',  "AltGr+2 is the at-sign");
    key("es", 0x02, 2, '|',  "AltGr+1 is a pipe");
    CHECK(kbd_is_dead(0xB4), "es: the key right of n-tilde is a dead acute");

    /* ---- Italian -------------------------------------------------------- */
    use("it");
    key("it", 0x28, 0, 0xE0, "a-grave has its own key");
    key("it", 0x1A, 0, 0xE8, "e-grave where [ is");
    key("it", 0x2B, 0, 0xF9, "u-grave next to Enter");
    key("it", 0x27, 2, '@',  "AltGr+o-grave is the at-sign");
    key("it", 0x04, 1, 0xA3, "Shift+3 is a pound sign");
    CHECK(!kbd_is_dead('^'), "it: Italian has no dead keys -- the accented "
                             "letters are on their own keys");

    /* ---- Swedish -------------------------------------------------------- */
    use("se");
    key("se", 0x1A, 0, 0xE5, "a-ring where [ is");
    key("se", 0x27, 0, 0xF6, "o-diaeresis where ; is");
    key("se", 0x28, 0, 0xE4, "a-diaeresis where ' is");
    key("se", 0x03, 2, '@',  "AltGr+2 is the at-sign");
    key("se", 0x08, 2, '{',  "AltGr+7 is a brace");

    /* ---- dead-key composition ------------------------------------------ */
    use("de");
    CHECK(kbd_dead_compose(0xB4, 'e') == 0xE9, "acute + e should be e-acute");
    CHECK(kbd_dead_compose(0xB4, 'E') == 0xC9, "acute + E should be E-acute");
    CHECK(kbd_dead_compose('`',  'a') == 0xE0, "grave + a should be a-grave");
    CHECK(kbd_dead_compose('^',  'o') == 0xF4, "circumflex + o should be o-circumflex");
    CHECK(kbd_dead_compose(0xA8, 'u') == 0xFC, "diaeresis + u should be u-diaeresis");
    CHECK(kbd_dead_compose('~',  'n') == 0xF1, "tilde + n should be n-tilde");
    /* The pair that does not exist. The driver emits both characters here, and
     * it can only do that if the compose says no rather than guessing. */
    CHECK(kbd_dead_compose(0xB4, 'z') == 0, "there is no z-acute in Latin-1");
    CHECK(kbd_dead_compose('~',  'e') == 0, "there is no e-tilde in Latin-1");
    CHECK(kbd_dead_compose(0xB4, ' ') == 0, "accent + space is the driver's job");

    /* ---- switching back ------------------------------------------------- */
    use("us");
    row("us", "qwerty row after a round trip", ROW_Q, 10, 0, "qwertyuiop");
    key("us", 0x10, 2, 0, "US has no third level at all");
    (void)ROW_Z;

    if (fails) {
        printf("%d layout check%s failed\n", fails, fails == 1 ? "" : "s");
        return 1;
    }
    printf("kbdlayout: %d layouts, every key checked\n", kbd_layout_count());
    return 0;
}
