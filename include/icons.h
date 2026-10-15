#pragma once
#include <stdint.h>
#include "glyphs.h"

/* ===========================================================================
 *  BoltOS  -  include/icons.h
 *
 *  Two kinds of picture. Icons (ICON_* in gui.h) are full colour and drawn
 *  by mkicons.py into atlases at 16, 20, 24, 32, 40, 48, 64 and 96 px; asking for a
 *  size in between scales the next one up down. Glyphs (GLYPH_*, glyphs.h)
 *  are monochrome UI symbols -- Lucide's -- at 16, 20 and 24, tinted to
 *  whatever colour the caller passes.
 *
 *  Atlases are decoded the first time a size is used, not at boot: most of a
 *  session only ever touches three of the six.
 * ===========================================================================*/

void icon_draw(int id, int x, int y, int size, int alpha);
void glyph_draw(int id, int x, int y, int size, uint32_t color, int alpha);
/* Centred on (cx, cy), which is what nearly every caller wants. */
void icon_draw_c(int id, int cx, int cy, int size, int alpha);
void glyph_draw_c(int id, int cx, int cy, int size, uint32_t color, int alpha);
/* Decode the sizes the desktop needs before it is on screen, so the first
 * frame does not pause on PNG inflate. */
void icons_preload(void);
/* A 32x32 premultiplied cursor image (CURSOR_*, glyphs.h); 0 if missing. */
int  cursor_image(int kind, const uint32_t **px, int *stride);
