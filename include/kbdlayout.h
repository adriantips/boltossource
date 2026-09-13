#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS  -  include/kbdlayout.h
 *  Which character a key produces, which is not the same question as which
 *  key it is.
 *
 *  drivers/keyboard.c had two 128-byte tables baked into it, unshifted and
 *  shifted, US only. That is fine until somebody sits down at a German
 *  keyboard and finds that Z types Y, that the key marked ä types an
 *  apostrophe, and that there is no way at all to type the @ printed on the Q
 *  key -- because it needs AltGr, and AltGr was being treated as Alt.
 *
 *  So a layout is data: three levels per scancode (plain, shifted, AltGr) plus
 *  a dead-key table, and the driver looks the key up instead of knowing what
 *  it means.
 *
 *  --- AltGr ---------------------------------------------------------------
 *  The right Alt key on a non-US keyboard is not Alt. It is a third-level
 *  shift, and on German, French, Spanish and Italian layouts it is the only
 *  way to type @, the currency signs and the bracket family. The PS/2 driver
 *  was folding it into Alt, so AltGr+Q fired whatever Alt+Q was bound to and
 *  produced nothing. It is a separate modifier now.
 *
 *  --- Dead keys -----------------------------------------------------------
 *  On most European layouts an accent is not a character, it is a *pending
 *  modification*: press ´ and nothing appears, then press e and you get é.
 *  Press ´ then space and you get a literal ´; press ´ then z, which has no
 *  accented form, and you get both characters, which is what every other
 *  system does and what people expect.
 *
 *  This is the "input method groundwork" part, and it is groundwork rather
 *  than an input method: it composes one dead key with one letter, which
 *  covers every Latin-script layout and none of the CJK ones.
 *
 *  --- The honest limit ----------------------------------------------------
 *  Everything here is Latin-1 (ISO 8859-1) bytes, because that is what this
 *  machine's text path carries: the console, the editors, the clipboard and
 *  the filesystem all move `char`, not code points. So the layouts are the
 *  ones Latin-1 can spell. Polish, Czech, Turkish, Greek and Cyrillic need
 *  UTF-8 through all of that first, which is a real piece of work and a
 *  different one. `kbd` says so rather than offering a layout that would type
 *  blanks.
 * ========================================================================= */

#define KBD_MAX_LAYOUTS 8
#define KBD_TABLE_SZ    128        /* scancode set 1, make codes only */

/* A dead key: the accent the key produced, and what it composes with. */
typedef struct {
    uint8_t  accent;      /* the spacing character it types on its own    */
    const char *bases;    /* letters it can modify, in order              */
    const char *results;  /* the composed byte for each, same order       */
} kbd_dead_t;

typedef struct {
    const char *name;        /* "de" -- the on-disk spelling              */
    const char *label;       /* "German (QWERTZ)"                          */
    const uint8_t *plain;    /* scancode -> byte, 128 entries              */
    const uint8_t *shift;
    const uint8_t *altgr;    /* NULL if the layout has no third level      */
    const kbd_dead_t *dead;  /* NULL-terminated by a zero accent, or NULL  */
} kbd_layout_t;

void kbd_layout_init(void);            /* defaults, then /etc/keyboard.conf */
int  kbd_layout_load(void);            /* re-read the file; 1 if it existed */
int  kbd_layout_save(void);            /* write it; 0 on success            */

int         kbd_layout_count(void);
const char *kbd_layout_name(int i);    /* "de"                              */
const char *kbd_layout_label(int i);   /* "German (QWERTZ)"                 */
int         kbd_layout_current(void);
int         kbd_layout_by_name(const char *name);   /* -1 if unknown        */
int         kbd_layout_set(int i);                  /* 0, or -1 for a bad index */

/* The lookup the driver does. `level` is 0 plain, 1 shift, 2 AltGr. Returns
 * the byte to deliver, or 0 for a key this layout does not produce. */
uint8_t kbd_layout_char(uint8_t scancode, int level);

/* Dead-key composition. Called with the byte a lookup produced:
 *   returns 1 and sets *pending if this byte is a dead key on this layout.
 * Then, for the NEXT character, kbd_dead_compose(pending, c) returns the
 * combined byte, or 0 if the pair does not combine (the caller then emits the
 * accent and the character separately, which is what everyone else does). */
int     kbd_is_dead(uint8_t c);
uint8_t kbd_dead_compose(uint8_t accent, uint8_t c);
