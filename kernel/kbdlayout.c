/* ===========================================================================
 *  BoltOS  -  kernel/kbdlayout.c
 *  The layouts, and the one resolved table the keyboard driver reads.
 *
 *  Every layout is written as a list of DIFFERENCES from US, because that is
 *  what a layout actually is. German is not a new keyboard, it is QWERTY with
 *  Y and Z swapped, three letters where the punctuation was, and a third level
 *  on the right Alt key -- and saying that in twenty lines is both shorter and
 *  far easier to check than a hand-typed 128-byte array where one transposed
 *  entry gives you a keyboard that is wrong in exactly one place.
 *
 *  Selecting a layout resolves it into three flat 128-byte tables, so the
 *  lookup on the interrupt path is one array index and no searching.
 * ========================================================================= */
#include <stdint.h>
#include <stdio.h>
#include "kbdlayout.h"
#include "fs.h"
#include "kprintf.h"
#include "string.h"

#define KBD_PATH "/etc/keyboard.conf"

/* Latin-1 bytes, named so the tables below read as what they produce. The euro
 * is at 0x80 the way CP1252 puts it, because it postdates Latin-1 and there is
 * nowhere else for it; kernel/font_latin1.c draws one there. */
#define EURO  0x80
#define NBSP  0xA0
#define IEXCL 0xA1
#define POUND 0xA3
#define CURR  0xA4
#define SECT  0xA7
#define DIAER 0xA8
#define FORD  0xAA
#define LAQUO 0xAB
#define NOT   0xAC
#define DEG   0xB0
#define SUP2  0xB2
#define SUP3  0xB3
#define ACUTE 0xB4
#define MICRO 0xB5
#define MIDOT 0xB7
#define MORD  0xBA
#define RAQUO 0xBB
#define HALF  0xBD
#define IQUES 0xBF
#define AGRAVE 0xE0
#define CCED  0xE7
#define CCEDU 0xC7
#define EACUTE 0xE9
#define EGRAVE 0xE8
#define IGRAVE 0xEC
#define NTILDE 0xF1
#define NTILDEU 0xD1
#define OGRAVE 0xF2
#define UGRAVE 0xF9
#define ADIAER 0xE4
#define ADIAERU 0xC4
#define ODIAER 0xF6
#define ODIAERU 0xD6
#define UDIAER 0xFC
#define UDIAERU 0xDC
#define ARING  0xE5
#define ARINGU 0xC5
#define SZLIG  0xDF

/* --- the US base ---------------------------------------------------------
 *  Lifted verbatim from what was baked into drivers/keyboard.c, so a machine
 *  that never touches `kbd` behaves exactly as it did.
 * ------------------------------------------------------------------------*/
static const uint8_t US_PLAIN[KBD_TABLE_SZ] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0, 'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0,'\\','z','x','c','v',
    'b','n','m',',','.','/', 0, '*', 0, ' ',
};
static const uint8_t US_SHIFT[KBD_TABLE_SZ] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0, 'A','S',
    'D','F','G','H','J','K','L',':','"','~', 0,'|','Z','X','C','V',
    'B','N','M','<','>','?', 0, '*', 0, ' ',
};

/* One key's three levels. `sc` is the scancode-set-1 make code. A zero in any
 * level means "this layout produces nothing there", which is different from
 * "same as US" -- an override row replaces all three. */
typedef struct { uint8_t sc, plain, shift, altgr; } kbd_key_t;

/* Scancode 0x56 is the extra key every non-US keyboard has between the left
 * Shift and Z, and that no US keyboard has. It is missing from the US tables
 * above for that reason, and present in every layout below.
 *
 * And the numbering trap, written down because I walked into it: the digit row
 * starts at 0x02, so the '1' key is 0x02 and the '3' key is 0x04. Every table
 * below is indexed by the key, not by the digit printed on it, and an
 * override one place out gives you a keyboard that is wrong in exactly one
 * spot -- the kind of bug that survives a test that only types "hello". */
#define SC_LT 0x56

/* --- dead keys -----------------------------------------------------------
 *  One table shared by every layout. A dead key is identified by the spacing
 *  character it would type on its own, so a layout declares one simply by
 *  putting ACUTE (or '^', or DIAER) in a level.
 * ------------------------------------------------------------------------*/
static const kbd_dead_t DEAD[] = {
    { ACUTE, "aeiouyAEIOUY",  "\xE1\xE9\xED\xF3\xFA\xFD\xC1\xC9\xCD\xD3\xDA\xDD" },
    { '`',   "aeiouAEIOU",    "\xE0\xE8\xEC\xF2\xF9\xC0\xC8\xCC\xD2\xD9" },
    { '^',   "aeiouAEIOU",    "\xE2\xEA\xEE\xF4\xFB\xC2\xCA\xCE\xD4\xDB" },
    { DIAER, "aeiouyAEIOU",   "\xE4\xEB\xEF\xF6\xFC\xFF\xC4\xCB\xCF\xD6\xDC" },
    { '~',   "anoANO",        "\xE3\xF1\xF5\xC3\xD1\xD5" },
    { 0, 0, 0 },
};

/* --- the layouts ---------------------------------------------------------- */

static const kbd_key_t K_UK[] = {
    { 0x03, '2', '"', 0 },
    { 0x04, '3', POUND, 0 },
    { 0x05, '4', '$', EURO },
    { 0x28, '\'', '@', 0 },
    { 0x29, '`', NOT, '|' },
    { 0x2B, '#', '~', 0 },
    { SC_LT, '\\', '|', 0 },
    { 0 }
};

static const kbd_key_t K_DE[] = {
    /* QWERTZ: the swap people notice in the first sentence they type. */
    { 0x15, 'z', 'Z', 0 },
    { 0x2C, 'y', 'Y', 0 },
    { 0x03, '2', '"', 0 },
    { 0x04, '3', SECT, 0 },
    { 0x07, '6', '&', 0 },
    { 0x08, '7', '/', '{' },
    { 0x09, '8', '(', '[' },
    { 0x0A, '9', ')', ']' },
    { 0x0B, '0', '=', '}' },
    { 0x0C, SZLIG, '?', '\\' },
    { 0x0D, ACUTE, '`', 0 },
    { 0x10, 'q', 'Q', '@' },
    { 0x12, 'e', 'E', EURO },
    { 0x1A, UDIAER, UDIAERU, 0 },
    { 0x1B, '+', '*', '~' },
    { 0x27, ODIAER, ODIAERU, 0 },
    { 0x28, ADIAER, ADIAERU, 0 },
    { 0x29, '^', DEG, 0 },
    { 0x2B, '#', '\'', 0 },
    { 0x32, 'm', 'M', MICRO },
    { 0x33, ',', ';', 0 },
    { 0x34, '.', ':', 0 },
    { 0x35, '-', '_', 0 },
    { SC_LT, '<', '>', '|' },
    { 0 }
};

static const kbd_key_t K_FR[] = {
    /* AZERTY. The top row is letters where the digits are, which is the thing
     * that surprises people: the digits need Shift. */
    { 0x02, '&', '1', 0 },
    { 0x03, EACUTE, '2', '~' },
    { 0x04, '"', '3', '#' },
    { 0x05, '\'', '4', '{' },
    { 0x06, '(', '5', '[' },
    { 0x07, '-', '6', '|' },
    { 0x08, EGRAVE, '7', '`' },
    { 0x09, '_', '8', '\\' },
    { 0x0A, CCED, '9', '^' },
    { 0x0B, AGRAVE, '0', '@' },
    { 0x0C, ')', DEG, ']' },
    { 0x0D, '=', '+', '}' },
    { 0x10, 'a', 'A', 0 },
    { 0x11, 'z', 'Z', 0 },
    { 0x12, 'e', 'E', EURO },
    { 0x1A, '^', DIAER, 0 },
    { 0x1B, '$', POUND, CURR },
    { 0x1E, 'q', 'Q', 0 },
    { 0x27, 'm', 'M', 0 },
    { 0x28, UGRAVE, '%', 0 },
    { 0x29, SUP2, 0, 0 },
    { 0x2B, '*', MICRO, 0 },
    { 0x2C, 'w', 'W', 0 },
    { 0x32, ',', '?', 0 },
    { 0x33, ';', '.', 0 },
    { 0x34, ':', '/', 0 },
    { 0x35, '!', SECT, 0 },
    { SC_LT, '<', '>', 0 },
    { 0 }
};

static const kbd_key_t K_ES[] = {
    { 0x02, '1', '!', '|' },
    { 0x03, '2', '"', '@' },
    { 0x04, '3', MIDOT, '#' },
    { 0x07, '6', '&', 0 },
    { 0x08, '7', '/', 0 },
    { 0x09, '8', '(', 0 },
    { 0x0A, '9', ')', 0 },
    { 0x0B, '0', '=', 0 },
    { 0x0C, '\'', '?', '\\' },
    { 0x0D, IEXCL, IQUES, 0 },
    { 0x12, 'e', 'E', EURO },
    { 0x1A, '`', '^', '[' },
    { 0x1B, '+', '*', ']' },
    { 0x27, NTILDE, NTILDEU, 0 },
    { 0x28, ACUTE, DIAER, '{' },
    { 0x29, MORD, FORD, '\\' },
    { 0x2B, CCED, CCEDU, '}' },
    { 0x33, ',', ';', 0 },
    { 0x34, '.', ':', 0 },
    { 0x35, '-', '_', 0 },
    { SC_LT, '<', '>', 0 },
    { 0 }
};

static const kbd_key_t K_IT[] = {
    { 0x03, '2', '"', 0 },
    { 0x04, '3', POUND, 0 },
    { 0x07, '6', '&', 0 },
    { 0x08, '7', '/', 0 },
    { 0x09, '8', '(', 0 },
    { 0x0A, '9', ')', 0 },
    { 0x0B, '0', '=', 0 },
    { 0x0C, '\'', '?', 0 },
    { 0x0D, IGRAVE, '^', 0 },
    { 0x12, 'e', 'E', EURO },
    { 0x1A, EGRAVE, EACUTE, '[' },
    { 0x1B, '+', '*', ']' },
    { 0x27, OGRAVE, CCED, '@' },
    { 0x28, AGRAVE, DEG, '#' },
    { 0x29, '\\', '|', 0 },
    { 0x2B, UGRAVE, SECT, 0 },
    { 0x33, ',', ';', 0 },
    { 0x34, '.', ':', 0 },
    { 0x35, '-', '_', 0 },
    { SC_LT, '<', '>', 0 },
    { 0 }
};

static const kbd_key_t K_SE[] = {
    { 0x03, '2', '"', '@' },
    { 0x04, '3', '#', POUND },
    { 0x05, '4', CURR, '$' },
    { 0x08, '7', '/', '{' },
    { 0x09, '8', '(', '[' },
    { 0x0A, '9', ')', ']' },
    { 0x0B, '0', '=', '}' },
    { 0x0C, '+', '?', '\\' },
    { 0x0D, ACUTE, '`', 0 },
    { 0x12, 'e', 'E', EURO },
    { 0x1A, ARING, ARINGU, 0 },
    { 0x1B, DIAER, '^', '~' },
    { 0x27, ODIAER, ODIAERU, 0 },
    { 0x28, ADIAER, ADIAERU, 0 },
    { 0x29, SECT, HALF, 0 },
    { 0x2B, '\'', '*', 0 },
    { 0x33, ',', ';', 0 },
    { 0x34, '.', ':', 0 },
    { 0x35, '-', '_', 0 },
    { SC_LT, '<', '>', '|' },
    { 0 }
};

/* US-International: the US keyboard people already own, with the punctuation
 * that nobody types twice in a row turned into dead keys. This is how most
 * English speakers who occasionally need an accent actually work, and it is
 * also the layout that teaches everybody what a dead key is, because typing a
 * quote before a vowel suddenly stops producing a quote. */
static const kbd_key_t K_USINTL[] = {
    { 0x28, ACUTE, DIAER, '\'' },
    { 0x29, '`', '~', '`' },
    { 0x12, 'e', 'E', EACUTE },
    { 0x1E, 'a', 'A', AGRAVE },
    { 0x18, 'o', 'O', ODIAER },
    { 0x16, 'u', 'U', UDIAER },
    { 0x31, 'n', 'N', NTILDE },
    { 0x2E, 'c', 'C', CCED },
    { 0x06, '5', '%', EURO },
    { 0 }
};

/* `dead` lists the accents that are DEAD on this layout, and it has to be
 * per-layout rather than a property of the character. '^' is a dead key on a
 * German keyboard and an ordinary Shift+6 on a US one; a check that just asked
 * "is this byte an accent?" would make it impossible to type a caret in C on
 * the layout most people are using. */
static const struct {
    const char *name, *label, *dead;
    const kbd_key_t *keys;
} LAYOUTS[] = {
    { "us",      "US English",        "",                 0        },
    { "us-intl", "US International",  "\xB4\xA8`~^",       K_USINTL },
    { "uk",      "United Kingdom",    "",                 K_UK     },
    { "de",      "German (QWERTZ)",   "\xB4`^",            K_DE     },
    { "fr",      "French (AZERTY)",   "^\xA8",             K_FR     },
    { "es",      "Spanish",           "\xB4\xA8`^",         K_ES     },
    { "it",      "Italian",           "",                 K_IT     },
    { "se",      "Swedish",           "\xB4\xA8`^",         K_SE     },
};
#define NLAYOUT ((int)(sizeof(LAYOUTS) / sizeof(LAYOUTS[0])))

/* The one resolved layout the driver reads, rebuilt on every change. */
static uint8_t cur_plain[KBD_TABLE_SZ];
static uint8_t cur_shift[KBD_TABLE_SZ];
static uint8_t cur_altgr[KBD_TABLE_SZ];
static int     cur = 0;

int         kbd_layout_count(void)     { return NLAYOUT; }
static int  ok(int i)                  { return i >= 0 && i < NLAYOUT; }
const char *kbd_layout_name(int i)     { return ok(i) ? LAYOUTS[i].name  : "?"; }
const char *kbd_layout_label(int i)    { return ok(i) ? LAYOUTS[i].label : "?"; }
int         kbd_layout_current(void)   { return cur; }

int kbd_layout_by_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < NLAYOUT; i++) if (strcmp(name, LAYOUTS[i].name) == 0) return i;
    return -1;
}

int kbd_layout_set(int i) {
    if (!ok(i)) return -1;
    memcpy(cur_plain, US_PLAIN, KBD_TABLE_SZ);
    memcpy(cur_shift, US_SHIFT, KBD_TABLE_SZ);
    memset(cur_altgr, 0, KBD_TABLE_SZ);
    for (const kbd_key_t *k = LAYOUTS[i].keys; k && k->sc; k++) {
        if (k->sc >= KBD_TABLE_SZ) continue;
        cur_plain[k->sc] = k->plain;
        cur_shift[k->sc] = k->shift;
        cur_altgr[k->sc] = k->altgr;
    }
    cur = i;
    return 0;
}

uint8_t kbd_layout_char(uint8_t sc, int level) {
    if (sc >= KBD_TABLE_SZ) return 0;
    if (level == 2) return cur_altgr[sc];
    return level ? cur_shift[sc] : cur_plain[sc];
}

int kbd_is_dead(uint8_t c) {
    if (!c) return 0;
    for (const char *p = LAYOUTS[cur].dead; *p; p++)
        if ((uint8_t)*p == c) return 1;
    return 0;
}

uint8_t kbd_dead_compose(uint8_t accent, uint8_t c) {
    for (const kbd_dead_t *d = DEAD; d->accent; d++) {
        if (d->accent != accent) continue;
        for (int i = 0; d->bases[i]; i++)
            if ((uint8_t)d->bases[i] == c) return (uint8_t)d->results[i];
        return 0;
    }
    return 0;
}

/* -------------------------------------------------------------- file ----- */
int kbd_layout_save(void) {
    char text[160];
    int n = snprintf(text, sizeof text,
                     "# BoltOS keyboard layout. `kbd` edits this.\n"
                     "layout = %s\n", LAYOUTS[cur].name);
    if (n <= 0) return -1;

    fs_node *f = fs_lookup(KBD_PATH);
    if (!f) {
        if (!fs_lookup("/etc")) fs_create("/etc", 1);
        f = fs_create(KBD_PATH, 0);
    }
    if (!f || f->is_dir) return -1;
    return fs_write(f, text, (uint32_t)n) == 0 ? 0 : -1;
}

int kbd_layout_load(void) {
    fs_node *f = fs_lookup(KBD_PATH);
    if (!f || f->is_dir || !f->data || !f->size) return 0;

    const char *p = (const char *)f->data;
    uint32_t left = f->size;
    while (left) {
        char line[64]; uint32_t n = 0;
        while (left && *p != '\n') { if (n < sizeof line - 1) line[n++] = *p; p++; left--; }
        if (left) { p++; left--; }
        line[n] = 0;
        for (uint32_t i = 0; i < n; i++) if (line[i] == '#') { line[i] = 0; n = i; break; }
        char *eq = 0;
        for (uint32_t i = 0; i < n; i++) if (line[i] == '=') { eq = line + i; break; }
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        for (char *e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*val == ' ' || *val == '\t') val++;
        for (char *e = val + strlen(val); e > val && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'); ) *--e = 0;
        if (strcmp(key, "layout") != 0) continue;
        int i = kbd_layout_by_name(val);
        if (i < 0) {
            kprintf("[--] kbd: %s names no layout '%s'; staying on %s\n",
                    KBD_PATH, val, LAYOUTS[cur].name);
            continue;
        }
        kbd_layout_set(i);
    }
    return 1;
}

void kbd_layout_init(void) {
    kbd_layout_set(0);
    kbd_layout_load();
}
