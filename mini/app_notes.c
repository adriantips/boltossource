/* ===========================================================================
 *  BoltOS Mini  -  mini/app_notes.c
 *
 *  A page to type on. It lives in RAM: there is no filesystem on Mini yet,
 *  and the page says so rather than pretending otherwise.
 * ===========================================================================*/
#include <stdint.h>
#include "ui.h"
#include "string.h"

#define CAP 32768
static char text[CAP] = "Notes\n\nA quiet page for lists and thoughts.\n"
                        "Everything here stays until the Pi restarts.\n";
static int len = -1, cur, top_line;

static void ensure(void) { if (len < 0) { len = (int)strlen(text); cur = len; } }

static int line_start(int i) { while (i > 0 && text[i - 1] != '\n') i--; return i; }
static int line_end(int i)   { while (i < len && text[i] != '\n') i++; return i; }

static void n_key(const input_ev_t *e) {
    ensure();
    switch (e->key) {
    case KEY_CHAR:
        if (len < CAP - 1 && e->ch >= 32 && !(e->mods & MOD_CTRL)) {
            memmove(text + cur + 1, text + cur, (size_t)(len - cur + 1));
            text[cur++] = (char)e->ch; len++;
        }
        break;
    case KEY_ENTER:
        if (len < CAP - 1) { memmove(text + cur + 1, text + cur, (size_t)(len - cur + 1)); text[cur++] = '\n'; len++; }
        break;
    case KEY_TAB:
        for (int i = 0; i < 4 && len < CAP - 1; i++) { memmove(text + cur + 1, text + cur, (size_t)(len - cur + 1)); text[cur++] = ' '; len++; }
        break;
    case KEY_BACKSPACE:
        if (cur > 0) { memmove(text + cur - 1, text + cur, (size_t)(len - cur + 1)); cur--; len--; }
        break;
    case KEY_DELETE:
        if (cur < len) { memmove(text + cur, text + cur + 1, (size_t)(len - cur)); len--; }
        break;
    case KEY_LEFT:  if (cur > 0) cur--; break;
    case KEY_RIGHT: if (cur < len) cur++; break;
    case KEY_HOME:  cur = line_start(cur); break;
    case KEY_END:   cur = line_end(cur); break;
    case KEY_UP: {
        int s = line_start(cur), col = cur - s;
        if (s == 0) { cur = 0; break; }
        int ps = line_start(s - 1);
        cur = ps + (col < s - 1 - ps ? col : s - 1 - ps);
        break;
    }
    case KEY_DOWN: {
        int s = line_start(cur), col = cur - s, e2 = line_end(cur);
        if (e2 >= len) { cur = len; break; }
        int ne = line_end(e2 + 1);
        cur = e2 + 1 + (col < ne - e2 - 1 ? col : ne - e2 - 1);
        break;
    }
    case KEY_PGUP: for (int i = 0; i < 10; i++) { int s = line_start(cur); if (!s) { cur = 0; break; } cur = line_start(s - 1); } break;
    case KEY_PGDN: for (int i = 0; i < 10; i++) { int e2 = line_end(cur); if (e2 >= len) { cur = len; break; } cur = e2 + 1; } break;
    }
}

static void n_draw(int x, int y, int w, int h) {
    (void)w;
    ensure();
    int pad = S(28), px = S(16), lh = S(26);
    int rows = (h - 2 * pad - S(20)) / lh;
    /* which line is the cursor on? */
    int cl = 0;
    for (int i = 0; i < cur; i++) if (text[i] == '\n') cl++;
    if (cl < top_line) top_line = cl;
    if (cl >= top_line + rows) top_line = cl - rows + 1;
    int line = 0, i = 0, yy = y + pad;
    while (i <= len && line < top_line + rows) {
        int e = line_end(i);
        if (line >= top_line) {
            int face = (line == 0) ? FONT_SEMIBOLD : FONT_REGULAR;
            int size = (line == 0) ? S(22) : px;
            int base = yy + font_ascent(face, size);
            font_draw(face, size, x + pad, base, text + i, e - i, line == 0 ? th.text : th.text, 255);
            if (cur >= i && cur <= e && (now_ms() / 530) % 2 == 0) {
                int cx = x + pad + font_width(face, size, text + i, cur - i);
                gfx_fill(cx, yy + S(3), S(2), lh - S(6), th.accent);
            }
            yy += (line == 0) ? S(34) : lh;
        }
        line++;
        i = e + 1;
        if (e >= len) break;
    }
    text_l(x + pad, y + h - S(22), "Kept in memory until the Pi restarts", FONT_REGULAR, S(11), th.faint);
}

static int n_tick(void) {
    static uint64_t phase;
    uint64_t p = now_ms() / 530;
    if (p != phase) { phase = p; return 1; }
    return 0;
}

const app_t app_notes = { "Notes", 0xE2A93B, G_NOTES, 0, n_key, 0, n_draw, n_tick };
