#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS  -  include/font.h
 *
 *  Anti-aliased text from outline fonts. The UI face is Inter (four weights
 *  and a light display cut for big numerals), the monospace one is DejaVu Sans
 *  Mono. mkuifont.py turns them into .bfnt blobs at build time; this side
 *  rasterises each glyph once per size and keeps it.
 *
 *  Strings are Windows-1252, which is what the keyboard already produces --
 *  0x80 is the euro, 0x85 an ellipsis, 0x95 a bullet, 0x96/0x97 the dashes.
 *  Five bytes cp1252 leaves undefined are borrowed for arrows and a check:
 *  0x81 right, 0x8D left, 0x8F up, 0x90 down, 0x9D check mark.
 * ===========================================================================*/

enum {
    FONT_REGULAR = 0,       /* Inter 400                                   */
    FONT_MEDIUM,            /* Inter 500                                   */
    FONT_SEMIBOLD,          /* Inter 600                                   */
    FONT_BOLD,              /* Inter 700                                   */
    FONT_DISPLAY,           /* Inter 300, display optical size: big clocks */
    FONT_MONO,              /* DejaVu Sans Mono                            */
    FONT_MONO_BOLD,
    FONT_FACES
};

#define TXT_ELLIPSIS "\x85"
#define TXT_BULLET   "\x95"
#define TXT_ENDASH   "\x96"
#define TXT_EMDASH   "\x97"
#define TXT_RARROW   "\x81"
#define TXT_LARROW   "\x8D"
#define TXT_CHECK    "\x9D"

int  font_init(void);
int  font_ready(void);

/* Draw `s` (len < 0: up to the NUL) with its baseline at y. Returns the pen
 * advance in pixels, rounded up. Kerned, positioned to a quarter pixel. */
int  font_draw(int face, int px, int x, int y, const char *s, int len, uint32_t color, int alpha);
int  font_width(int face, int px, const char *s, int len);
/* The same with a slant, for the browser's <em>. */
int  font_draw_italic(int face, int px, int x, int y, const char *s, int len, uint32_t color, int alpha);

/* Truncate with an ellipsis so the string fits in max_w. Returns the width. */
int  font_draw_fit(int face, int px, int x, int y, const char *s, int max_w, uint32_t color, int alpha);
/* How many leading bytes of s fit in max_w (word-wrapping helpers). */
int  font_fit(int face, int px, const char *s, int len, int max_w);

int  font_ascent(int face, int px);    /* pixels above the baseline   */
int  font_descent(int face, int px);   /* pixels below it, positive   */
int  font_capheight(int face, int px);
int  font_has(int face, unsigned char c);

/* Layout helpers most callers want: left/centre/right of x, the text's caps
 * centred vertically on cy. */
int  text_l(int x, int cy, const char *s, int face, int px, uint32_t color);
int  text_c(int cx, int cy, const char *s, int face, int px, uint32_t color);
int  text_r(int rx, int cy, const char *s, int face, int px, uint32_t color);
int  text_fit_l(int x, int cy, const char *s, int max_w, int face, int px, uint32_t color);
/* Word-wrapped paragraph from (x, top) inside width w, at most max_lines.
 * Returns the number of lines drawn. line_h is the baseline-to-baseline step. */
int  text_wrap(int x, int top, int w, const char *s, int face, int px, int line_h,
               int max_lines, uint32_t color);
