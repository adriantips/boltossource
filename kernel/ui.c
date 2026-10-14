/* ===========================================================================
 *  BoltOS  -  kernel/ui.c
 *
 *  The control kit (include/ui.h). Everything takes its colours from the live
 *  theme, so the same app is right in light and dark without knowing which
 *  it is in; the few things that must contrast with the accent (text on a
 *  primary button, a toggle's knob) are white in both.
 * ===========================================================================*/
#include <stdint.h>
#include "ui.h"
#include "gfx.h"
#include "string.h"
#include "pit.h"

static int in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

void ui_begin(ui_ctx_t *c, window_t *w, int cx, int cy) {
    c->n = 0; c->ox = cx; c->oy = cy; c->win = w;
}
void ui_hot(ui_ctx_t *c, int x, int y, int w, int h, int id) {
    if (c->n >= UI_MAXHOT) return;
    c->hot[c->n].x = x - c->ox; c->hot[c->n].y = y - c->oy;
    c->hot[c->n].w = w; c->hot[c->n].h = h; c->hot[c->n].id = id;
    c->n++;
}
int ui_hit(const ui_ctx_t *c, int lx, int ly) {
    for (int i = c->n - 1; i >= 0; i--)
        if (in_rect(lx, ly, c->hot[i].x, c->hot[i].y, c->hot[i].w, c->hot[i].h)) return c->hot[i].id;
    return -1;
}
int ui_hover(const ui_ctx_t *c, int x, int y, int w, int h) {
    if (c && c->win && !gui_window_focused(c->win)) {
        /* an unfocused window can still be hovered; only the cursor matters */
    }
    return in_rect(gui_mouse_x(), gui_mouse_y(), x, y, w, h);
}

/* ---- surfaces -------------------------------------------------------------- */
static uint32_t card_fill(void)   { return COL_PANEL_2; }
static uint32_t card_edge(void)   { return THEME_DARK ? 0x000000 : 0x000000; }
static int      card_edge_a(void) { return THEME_DARK ? 60 : 18; }
static uint32_t ctl_fill(void)    { return THEME_DARK ? gfx_shade(COL_PANEL_2, 125) : 0xFFFFFF; }

void ui_title(int x, int y, const char *s) { font_draw(FONT_SEMIBOLD, 22, x, y + 22, s, -1, COL_TEXT, 255); }
void ui_section(int x, int y, const char *s) { font_draw(FONT_SEMIBOLD, 14, x, y + 14, s, -1, COL_TEXT, 255); }

void ui_card(int x, int y, int w, int h) {
    gfx_rrect(x, y, w, h, 10, card_fill(), 255);
    gfx_rrect_stroke(x, y, w, h, 10, 1, card_edge(), card_edge_a());
    if (THEME_DARK) gfx_rrect_stroke2(x + 1, y + 1, w - 2, h - 2, 9, 0xFFFFFF, 14, 0);
}
void ui_card_row_sep(int x, int y, int w) { gfx_fill(x, y, w, 1, THEME_DARK ? 0x000000 : COL_SEP); }

void ui_label(int x, int cy, const char *s, uint32_t color) { text_l(x, cy, s, FONT_REGULAR, 13, color); }
void ui_label2(int x, int cy, const char *title, const char *sub) {
    if (sub && sub[0]) {
        text_l(x, cy - 8, title, FONT_REGULAR, 13, COL_TEXT);
        text_l(x, cy + 9, sub, FONT_REGULAR, 12, COL_TEXT_DIM);
    } else {
        text_l(x, cy, title, FONT_REGULAR, 13, COL_TEXT);
    }
}

/* ---- buttons ----------------------------------------------------------------- */
int ui_button_w(const char *label) { return font_width(FONT_MEDIUM, 13, label, -1) + 32; }

void ui_button(ui_ctx_t *c, int x, int y, int w, int h, const char *label, int style, int id) {
    int hot = ui_hover(c, x, y, w, h);
    uint32_t fill, txt = COL_TEXT;
    switch (style) {
    case UI_BTN_PRIMARY: fill = COL_ACCENT; txt = 0xFFFFFF; break;
    case UI_BTN_DANGER:  fill = COL_BAD;    txt = 0xFFFFFF; break;
    case UI_BTN_SUBTLE:  fill = COL_PANEL;  break;
    default:             fill = ctl_fill(); break;
    }
    if (hot) fill = gfx_shade(fill, style == UI_BTN_SECONDARY && !THEME_DARK ? 96 : 112);
    if (style == UI_BTN_SUBTLE && !hot) {
        /* nothing behind it until you point at it */
    } else {
        gfx_rrect(x, y, w, h, 8, fill, 255);
        if (style == UI_BTN_SECONDARY) {
            gfx_rrect_stroke(x, y, w, h, 8, 1, 0x000000, THEME_DARK ? 70 : 30);
            if (THEME_DARK) gfx_rrect_stroke2(x + 1, y + 1, w - 2, h - 2, 7, 0xFFFFFF, 26, 0);
        } else if (style != UI_BTN_SUBTLE) {
            gfx_rrect_stroke2(x, y, w, h, 8, 0xFFFFFF, 60, 0);
        }
    }
    text_c(x + w / 2, y + h / 2, label, FONT_MEDIUM, 13, txt);
    if (c) ui_hot(c, x, y, w, h, id);
}

void ui_button_icon(ui_ctx_t *c, int x, int y, int sz, int glyph, int id) {
    int hot = ui_hover(c, x, y, sz, sz);
    if (hot) gfx_rrect(x, y, sz, sz, 8, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 24 : 14);
    glyph_draw_c(glyph, x + sz / 2, y + sz / 2, sz >= 30 ? 20 : 16, COL_TEXT, 255);
    if (c) ui_hot(c, x, y, sz, sz, id);
}

/* ---- toggles, sliders, pickers -------------------------------------------------- */
void ui_toggle(ui_ctx_t *c, int x, int y, int on, int id) {
    int w = 40, h = 22;
    if (on) {
        gfx_rrect(x, y, w, h, h / 2, COL_ACCENT, 255);
    } else {
        gfx_rrect(x, y, w, h, h / 2, THEME_DARK ? 0x48484E : 0xE3E3E8, 255);
        gfx_rrect_stroke(x, y, w, h, h / 2, 1, 0x000000, THEME_DARK ? 40 : 22);
    }
    float kx = on ? (float)(x + w - h / 2) : (float)(x + h / 2);
    gfx_shadow((int)kx - 9, y + 2, 18, 18, 9, 4, 1, 0, 70);
    gfx_circle(kx, (float)(y + h / 2), 9.0f, 0xFFFFFF, 255);
    if (c) ui_hot(c, x - 4, y - 4, w + 8, h + 8, id);
}

void ui_slider(ui_ctx_t *c, int x, int y, int w, int value, int id) {
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    int hot = ui_hover(c, x - 8, y - 12, w + 16, 24);
    gfx_rrect(x, y - 2, w, 4, 2, THEME_DARK ? 0x5A5A62 : 0xD6D6DC, 255);
    int fw = w * value / 100;
    gfx_rrect(x, y - 2, fw, 4, 2, COL_ACCENT, 255);
    float kx = (float)(x + fw);
    gfx_shadow((int)kx - 9, y - 9, 18, 18, 9, 5, 1, 0, 80);
    gfx_circle(kx, (float)y, hot ? 10.0f : 9.0f, 0xFFFFFF, 255);
    gfx_ring(kx, (float)y, hot ? 10.0f : 9.0f, 1.0f, 0x000000, 26);
    if (c) ui_hot(c, x - 8, y - 12, w + 16, 24, id);
}
int ui_slider_value(int x, int w, int px) {
    int v = (px - x) * 100 / (w > 0 ? w : 1);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

void ui_segmented(ui_ctx_t *c, int x, int y, int w, const char **labels, int n, int sel, int id_base) {
    if (n <= 0) return;
    int h = 30;
    gfx_rrect(x, y, w, h, 8, THEME_DARK ? 0x000000 : 0x000000, THEME_DARK ? 60 : 14);
    int sw = (w - 4) / n;
    for (int i = 0; i < n; i++) {
        int sx = x + 2 + i * sw;
        if (i == sel) {
            gfx_shadow(sx, y + 2, sw, h - 4, 6, 4, 1, 0, 60);
            gfx_rrect(sx, y + 2, sw, h - 4, 6, THEME_DARK ? 0x5A5A62 : 0xFFFFFF, 255);
        } else if (ui_hover(c, sx, y, sw, h)) {
            gfx_rrect(sx, y + 2, sw, h - 4, 6, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 16 : 8);
        }
        if (i > 0 && i != sel && i - 1 != sel)
            gfx_fill(sx, y + 8, 1, h - 16, THEME_DARK ? 0x5A5A62 : 0xC7C7CC);
        text_c(sx + sw / 2, y + h / 2, labels[i], i == sel ? FONT_SEMIBOLD : FONT_REGULAR, 12, COL_TEXT);
        if (c) ui_hot(c, sx, y, sw, h, id_base + i);
    }
}

void ui_swatch(ui_ctx_t *c, int cx, int cy, int r, uint32_t color, int selected, int id) {
    if (selected) gfx_ring((float)cx, (float)cy, (float)r + 4.0f, 2.0f, color, 255);
    gfx_circle((float)cx, (float)cy, (float)r, color, 255);
    gfx_ring((float)cx, (float)cy, (float)r, 1.0f, 0x000000, 40);
    if (selected) glyph_draw_c(GLYPH_CHECK, cx, cy, 16, 0xFFFFFF, 255);
    if (c) ui_hot(c, cx - r - 4, cy - r - 4, 2 * r + 8, 2 * r + 8, id);
}

void ui_checkbox(ui_ctx_t *c, int x, int y, int on, int id) {
    if (on) {
        gfx_rrect(x, y, 18, 18, 5, COL_ACCENT, 255);
        glyph_draw_c(GLYPH_CHECK, x + 9, y + 9, 16, 0xFFFFFF, 255);
    } else {
        gfx_rrect(x, y, 18, 18, 5, ctl_fill(), 255);
        gfx_rrect_stroke(x, y, 18, 18, 5, 1, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 60 : 50);
    }
    if (c) ui_hot(c, x - 4, y - 4, 26, 26, id);
}

void ui_radio(ui_ctx_t *c, int x, int y, int on, int id) {
    float cx = (float)x + 9.0f, cy = (float)y + 9.0f;
    if (on) {
        gfx_circle(cx, cy, 9.0f, COL_ACCENT, 255);
        gfx_circle(cx, cy, 3.5f, 0xFFFFFF, 255);
    } else {
        gfx_circle(cx, cy, 9.0f, ctl_fill(), 255);
        gfx_ring(cx, cy, 9.0f, 1.0f, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 60 : 50);
    }
    if (c) ui_hot(c, x - 4, y - 4, 26, 26, id);
}

void ui_progress(int x, int y, int w, int pct, uint32_t color) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    gfx_rrect(x, y, w, 6, 3, THEME_DARK ? 0x48484E : 0xE3E3E8, 255);
    if (pct) gfx_rrect(x, y, w * pct / 100 < 6 ? 6 : w * pct / 100, 6, 3, color ? color : COL_ACCENT, 255);
}

void ui_textfield(ui_ctx_t *c, int x, int y, int w, int h, const char *text,
                  const char *placeholder, int focused, int id) {
    gfx_rrect(x, y, w, h, 8, THEME_DARK ? 0x141416 : 0xFFFFFF, 255);
    if (focused) {
        gfx_rrect_stroke(x - 1, y - 1, w + 2, h + 2, 9, 2, COL_ACCENT, 255);
    } else {
        gfx_rrect_stroke(x, y, w, h, 8, 1, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 30 : 40);
    }
    gfx_clip_push(x + 6, y, w - 12, h);
    int tw = 0;
    if (text && text[0]) tw = text_l(x + 10, y + h / 2, text, FONT_REGULAR, 13, COL_TEXT);
    else if (placeholder) text_l(x + 10, y + h / 2, placeholder, FONT_REGULAR, 13, COL_TEXT_MUTE);
    if (focused && (pit_ticks() / 530) % 2 == 0) gfx_fill(x + 11 + tw, y + 7, 2, h - 14, COL_ACCENT);
    gfx_clip_pop();
    if (c) ui_hot(c, x, y, w, h, id);
}

void ui_sidebar_item(ui_ctx_t *c, int x, int y, int w, int glyph, const char *label, int selected, int id) {
    int h = 34;
    int hot = ui_hover(c, x, y, w, h);
    if (selected) {
        gfx_rrect(x, y, w, h, 8, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 22 : 12);
        gfx_rrect(x, y + 9, 3, h - 18, 2, COL_ACCENT, 255);
    } else if (hot) {
        gfx_rrect(x, y, w, h, 8, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 12 : 7);
    }
    if (glyph >= 0) glyph_draw_c(glyph, x + 22, y + h / 2, 16, selected ? COL_ACCENT : COL_TEXT_DIM, 255);
    text_l(x + (glyph >= 0 ? 42 : 14), y + h / 2, label, selected ? FONT_MEDIUM : FONT_REGULAR, 13, COL_TEXT);
    if (c) ui_hot(c, x, y, w, h, id);
}

void ui_list_row(ui_ctx_t *c, int x, int y, int w, int h, int selected, int id) {
    int hot = ui_hover(c, x, y, w, h);
    if (selected)  gfx_rrect(x, y, w, h, 8, COL_ACCENT, THEME_DARK ? 200 : 230);
    else if (hot)  gfx_rrect(x, y, w, h, 8, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 14 : 8);
    if (c) ui_hot(c, x, y, w, h, id);
}

void ui_scrollbar(int x, int y, int h, int content, int view, int pos) {
    if (content <= view || h <= 0) return;
    int kh = h * view / content;
    if (kh < 24) kh = 24;
    int range = content - view;
    int ky = y + (h - kh) * (pos < 0 ? 0 : pos > range ? range : pos) / (range ? range : 1);
    gfx_rrect(x, ky, 5, kh, 3, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 70 : 60);
}

void ui_badge(int x, int cy, const char *s, uint32_t bg, uint32_t fg) {
    int w = font_width(FONT_SEMIBOLD, 11, s, -1) + 14;
    gfx_rrect(x, cy - 10, w, 20, 10, bg, 255);
    text_c(x + w / 2, cy, s, FONT_SEMIBOLD, 11, fg);
}

void ui_tooltip(int x, int y, const char *s) {
    int w = font_width(FONT_REGULAR, 12, s, -1) + 18;
    gfx_shadow(x, y, w, 26, 6, 10, 3, 0, 80);
    gfx_rrect(x, y, w, 26, 6, THEME_DARK ? 0x3A3A40 : 0xFFFFFF, 255);
    gfx_rrect_stroke(x, y, w, 26, 6, 1, 0x000000, THEME_DARK ? 90 : 30);
    text_l(x + 9, y + 13, s, FONT_REGULAR, 12, COL_TEXT);
}
