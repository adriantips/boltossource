#pragma once
#include <stdint.h>
#include "gui.h"
#include "font.h"
#include "icons.h"

/* ===========================================================================
 *  BoltOS  -  include/ui.h
 *
 *  The controls apps are built from: buttons, toggles, sliders, segmented
 *  pickers, cards, sidebars, text fields, list rows. Immediate mode, because
 *  that is how every app here already worked: you call these while drawing,
 *  each interactive one records a hit rectangle under an id you choose, and
 *  the click handler asks ui_hit() which id was under the pointer. The table
 *  is rebuilt every draw, so it can never disagree with what is on screen.
 *
 *  Coordinates are desktop coordinates, like everything else a draw callback
 *  sees; the table stores them relative to the client origin passed to
 *  ui_begin(), because that is what click callbacks are given.
 *
 *  Hover needs the window to redraw as the pointer moves: set
 *  win->track_hover = 1 for an app built on these.
 * ===========================================================================*/

#define UI_MAXHOT 160

typedef struct { int x, y, w, h, id; } ui_hot_t;
typedef struct {
    ui_hot_t hot[UI_MAXHOT];
    int      n;
    int      ox, oy;              /* client origin this draw          */
    window_t *win;
} ui_ctx_t;

enum { UI_BTN_SECONDARY = 0, UI_BTN_PRIMARY, UI_BTN_SUBTLE, UI_BTN_DANGER };

void ui_begin(ui_ctx_t *c, window_t *w, int cx, int cy);
void ui_hot(ui_ctx_t *c, int x, int y, int w, int h, int id);
int  ui_hit(const ui_ctx_t *c, int lx, int ly);           /* client-local -> id, -1 */
int  ui_hover(const ui_ctx_t *c, int x, int y, int w, int h);

/* the page itself */
void ui_title(int x, int y, const char *s);               /* 22 px page title    */
void ui_section(int x, int y, const char *s);             /* 14 px section head  */
void ui_card(int x, int y, int w, int h);                  /* raised group box    */
void ui_card_row_sep(int x, int y, int w);                 /* hairline inside one */
void ui_label(int x, int cy, const char *s, uint32_t color);
void ui_label2(int x, int cy, const char *title, const char *sub); /* two-line row label */

/* controls */
int  ui_button_w(const char *label);
void ui_button(ui_ctx_t *c, int x, int y, int w, int h, const char *label, int style, int id);
void ui_button_icon(ui_ctx_t *c, int x, int y, int sz, int glyph, int id);
void ui_toggle(ui_ctx_t *c, int x, int y, int on, int id);              /* 40 x 22 */
void ui_slider(ui_ctx_t *c, int x, int y, int w, int value, int id);    /* 0..100, y = centre */
int  ui_slider_value(int x, int w, int lx_desktop);                      /* pointer -> 0..100 */
void ui_segmented(ui_ctx_t *c, int x, int y, int w, const char **labels, int n, int sel, int id_base);
void ui_swatch(ui_ctx_t *c, int cx, int cy, int r, uint32_t color, int selected, int id);
void ui_checkbox(ui_ctx_t *c, int x, int y, int on, int id);            /* 18 x 18 */
void ui_radio(ui_ctx_t *c, int x, int y, int on, int id);
void ui_progress(int x, int y, int w, int pct, uint32_t color);
void ui_textfield(ui_ctx_t *c, int x, int y, int w, int h, const char *text,
                  const char *placeholder, int focused, int id);
void ui_sidebar_item(ui_ctx_t *c, int x, int y, int w, int glyph, const char *label, int selected, int id);
void ui_list_row(ui_ctx_t *c, int x, int y, int w, int h, int selected, int id);
void ui_scrollbar(int x, int y, int h, int content, int view, int pos);
void ui_badge(int x, int cy, const char *s, uint32_t bg, uint32_t fg);
void ui_tooltip(int x, int y, const char *s);
