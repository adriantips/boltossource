/* ===========================================================================
 *  BoltOS TV  -  tv/osk.c
 *
 *  An on-screen keyboard for the apps that want a line of text: YouTube's
 *  search and Settings' engine address. A remote moves over the keys and OK
 *  types the one lit; a real keyboard just types, and its Enter is Done. The
 *  line being written sits in a glass field above the keys.
 * ===========================================================================*/
#include <stdint.h>
#include "tv.h"
#include "string.h"
#include "stdio.h"

static const char *const ROWS[4] = { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm./:" };
#define SPECIAL 4                          /* row 4: Clear, Space, Delete, Done */
static glasscache_t gc_keys, gc_field;

void osk_begin(osk_t *k, const char *initial, const char *title, const char *done) {
    memset(k, 0, sizeof *k);
    snprintf(k->text, sizeof k->text, "%s", initial ? initial : "");
    k->title = title;
    k->done = done ? done : "Done";
    k->r = 1;
    k->active = 1;
    tv_text_mode(1);
}

static void put(osk_t *k, const char *s) {
    size_t n = strlen(k->text);
    while (*s && n < sizeof k->text - 1) k->text[n++] = *s++;
    k->text[n] = 0;
}
static void erase(osk_t *k) { size_t n = strlen(k->text); if (n) k->text[n - 1] = 0; }
static int finish(osk_t *k, int r) { k->active = 0; tv_text_mode(0); return r; }

int osk_key(osk_t *k, tvkey_t key) {
    int typed = key.k == K_CHAR || key.k == K_ERASE, r = 0;
    switch (key.k) {
    case K_CHAR: { char s[2] = { (char)key.ch, 0 }; put(k, s); break; }
    case K_ERASE: erase(k); break;
    case K_BACK: sfx_back(); return finish(k, -1);
    case K_OK:
        /* Enter after typing is the end of the line; OK from a remote types the key lit */
        if (k->last_typed) { sfx_select(); return finish(k, 1); }
        sfx_select();
        if (k->r < SPECIAL) { char s[2] = { ROWS[k->r][k->c], 0 }; put(k, s); }
        else switch (k->c) {
            case 0: k->text[0] = 0; break;
            case 1: put(k, " "); break;
            case 2: erase(k); break;
            case 3: return finish(k, 1);
        }
        break;
    case K_UP:
        if (k->r == SPECIAL) k->c = k->c == 0 ? 0 : k->c == 1 ? 4 : k->c == 2 ? 7 : 9;
        if (k->r > 0) k->r--;
        sfx_move();
        break;
    case K_DOWN:
        if (k->r < SPECIAL) { k->r++; if (k->r == SPECIAL) k->c = k->c < 2 ? 0 : k->c < 7 ? 1 : k->c < 9 ? 2 : 3; }
        sfx_move();
        break;
    case K_LEFT:  if (k->c > 0) { k->c--; sfx_move(); } break;
    case K_RIGHT: if (k->c < (k->r == SPECIAL ? 3 : 9)) { k->c++; sfx_move(); } break;
    }
    k->last_typed = typed;
    return r;
}

void osk_draw(const osk_t *k, uint64_t now, int a) {
    int kw = S(92), kh = S(80), gap = S(10);
    int w = 10 * kw + 9 * gap + S(48), h = 5 * kh + 4 * gap + S(48);
    int x = (tv_w - w) / 2, y = tv_h - h - S(40);

    /* the line, above the keys */
    int fh = S(96), fy = y - fh - S(24);
    ui_glass(&gc_field, x, fy, w, fh, fh / 2, 190, a);
    int tx = x + S(40), cy = fy + fh / 2;
    if (k->title && !k->text[0]) ui_text(FONT_REGULAR, S(30), tx, cy, k->title, 0x9AA3B2, a, 0);
    else {
        int tw = font_width(FONT_MEDIUM, S(32), k->text, -1), room = w - S(80);
        const char *s = k->text;
        while (tw > room && *s) { s++; tw = font_width(FONT_MEDIUM, S(32), s, -1); }   /* keep the end in view */
        ui_text(FONT_MEDIUM, S(32), tx, cy, s, 0xFFFFFF, a, 0);
        if ((now / 530) & 1) gfx_fill_a(tx + tw + S(3), cy - S(20), S(3), S(40), 0xFFFFFF, a);
    }

    ui_glass(&gc_keys, x, y, w, h, S(36), 190, a);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 10; c++) {
            int kx = x + S(24) + c * (kw + gap), ky = y + S(24) + r * (kh + gap);
            int on = k->r == r && k->c == c;
            if (on) ui_focus(kx, ky, kw, kh, S(16), a);
            else gfx_rrect(kx, ky, kw, kh, S(16), 0xFFFFFF, 26 * a / 255);
            char s[2] = { ROWS[r][c], 0 };
            ui_text(FONT_MEDIUM, S(32), kx + kw / 2, ky + kh / 2, s, on ? 0x101318 : 0xFFFFFF, a, 1);
        }
    const char *sp[4] = { "Clear", "Space", "Delete", k->done };
    int spw[4] = { 2, 5, 2, 1 };
    int kx = x + S(24), ky = y + S(24) + 4 * (kh + gap);
    for (int i = 0; i < 4; i++) {
        int ww = spw[i] * kw + (spw[i] - 1) * gap;
        int on = k->r == SPECIAL && k->c == i;
        if (on) ui_focus(kx, ky, ww, kh, S(16), a);
        else gfx_rrect(kx, ky, ww, kh, S(16), i == 3 ? 0x0A84FF : 0xFFFFFF, (i == 3 ? 220 : 26) * a / 255);
        ui_text(FONT_SEMIBOLD, i == 3 && font_width(FONT_SEMIBOLD, S(26), sp[i], -1) > ww - S(12) ? S(20) : S(26),
                kx + ww / 2, ky + kh / 2, sp[i], on ? 0x101318 : 0xFFFFFF, a, 1);
        kx += ww + gap;
    }
}
