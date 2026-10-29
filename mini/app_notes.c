/* ===========================================================================
 *  BoltOS Mini  -  mini/app_notes.c
 *
 *  A page to type on, kept on the SD card as /Notes/notes.txt: saved a
 *  moment after typing stops, and when Notes is left. With no card it stays
 *  in memory until the Pi restarts, and says so.
 * ===========================================================================*/
#include <stdint.h>
#include "ui.h"
#include "fs.h"
#include "kheap.h"
#include "string.h"

#define CAP 32768
#define PATH "/Notes/notes.txt"
static char text[CAP] = "Notes\n\nA quiet page for lists and thoughts.\n"
                        "It's kept on the SD card, in Notes/notes.txt.\n";
static int len = -1, cur, top_line;
static int on_card, changed;
static uint64_t changed_at;

static void ensure(void) {
    if (len >= 0) return;
    on_card = fs_mount() == 0;
    char *d; uint32_t n;
    if (on_card && fs_read(PATH, &d, &n) == 0) {
        if (n > CAP - 1) n = CAP - 1;
        memcpy(text, d, n); text[n] = 0;
        kfree(d);
    }
    len = (int)strlen(text); cur = len;
}

static void save(void) {
    if (!changed || !on_card) return;
    if (fs_mkdir("/Notes") == 0 && fs_write(PATH, text, (uint32_t)len) == 0) changed = 0;
}

static int line_start(int i) { while (i > 0 && text[i - 1] != '\n') i--; return i; }
static int line_end(int i)   { while (i < len && text[i] != '\n') i++; return i; }

static void n_key(const input_ev_t *e) {
    ensure();
    int before = len;
    char first = len ? text[0] : 0;
    static int last_cur;
    last_cur = cur;
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
    if (len != before || (len && text[0] != first)) { changed = 1; changed_at = now_ms(); }
    (void)last_cur;
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
    text_l(x + pad, y + h - S(22), !on_card ? "No SD card: kept in memory until the Pi restarts" :
           changed ? "Editing" TXT_ELLIPSIS : "Saved on the SD card, in Notes/notes.txt", FONT_REGULAR, S(11), th.faint);
}

static int n_tick(void) {
    static uint64_t phase;
    if (changed && now_ms() - changed_at > 1500) { save(); return 1; }
    uint64_t p = now_ms() / 530;
    if (p != phase) { phase = p; return 1; }
    return 0;
}

/* Notes isn't in front: what was typed last is still saved (the shell calls this) */
int notes_background(void);
int notes_background(void) { if (changed && now_ms() - changed_at > 1500) save(); return 0; }

/* Esc on the way out: what's typed is saved now */
static int n_esc(void) { save(); return 0; }

const app_t app_notes = { "Notes", 0xE2A93B, G_NOTES, 0, n_key, 0, n_draw, n_tick, n_esc };
