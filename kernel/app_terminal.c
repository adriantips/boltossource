/* ===========================================================================
 *  BoltOS  -  kernel/app_terminal.c
 *  A terminal window. Keeps its own character grid, edits a command line from
 *  the focused-window key stream, and runs each line through the real BoltShell
 *  dispatcher with kputc redirected into this grid (see console_set_sink).
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "console.h"
#include "shell.h"
#include "string.h"
#include "pit.h"
#include "fs.h"
#include "keyboard.h"
#include "clipboard.h"

#define TCOLS 220
#define TROWS 80
#define PAD   12
#define CELLW 8
#define CELLH 16          /* a little air between lines; 14 was cramped */
#define TERM_BG 0x141417

typedef struct {
    char    ch [TROWS][TCOLS];
    uint8_t col[TROWS][TCOLS];
    int     cols, rows;
    int     cx, cy;
    uint8_t curcol;
    char    line[256];
    int     len;
    int     pos;                       /* caret within the editable line */
    char    saved[256];                /* the line being typed, while browsing */
    int     hpos;                      /* 0 = typing, N = N entries back       */
} term_t;

static term_t  term;
static term_t *active;                 /* terminal currently capturing kputc */

/* palette index -> colour. Fixed rather than themed: the terminal stays dark
 * in the light theme too, like every terminal people actually use. */
static uint32_t pal(uint8_t i) {
    switch (i) {
    case 1:  return 0x64D2FF;          /* prompt / accent */
    case 2:  return 0x8A8A96;          /* dim             */
    case 3:  return 0x5DE38A;
    case 4:  return 0xFF6B60;
    default: return 0xE6E6EB;          /* normal          */
    }
}

static void term_scroll(term_t *t) {
    for (int r = 1; r < t->rows; r++)
        for (int c = 0; c < TCOLS; c++) { t->ch[r - 1][c] = t->ch[r][c]; t->col[r - 1][c] = t->col[r][c]; }
    for (int c = 0; c < TCOLS; c++) { t->ch[t->rows - 1][c] = 0; t->col[t->rows - 1][c] = 0; }
    t->cy = t->rows - 1;
}
static void term_newline(term_t *t) { t->cx = 0; if (++t->cy >= t->rows) term_scroll(t); }

static void term_putc(term_t *t, char c) {
    if (t->cols <= 0 || t->rows <= 0) return;
    if (c == '\n') { term_newline(t); return; }
    if (c == '\r') { t->cx = 0; return; }
    if (c == '\t') { int n = 4 - (t->cx & 3); while (n--) term_putc(t, ' '); return; }
    if (c == '\b') {
        if (t->cx > 0) t->cx--;
        else if (t->cy > 0) { t->cy--; t->cx = t->cols - 1; }
        t->ch[t->cy][t->cx] = 0;
        return;
    }
    if ((unsigned char)c < 32) return;
    t->ch[t->cy][t->cx]  = c;
    t->col[t->cy][t->cx] = t->curcol;
    if (++t->cx >= t->cols) term_newline(t);
}

static void term_puts(term_t *t, const char *s, uint8_t color) {
    uint8_t save = t->curcol; t->curcol = color;
    while (*s) term_putc(t, *s++);
    t->curcol = save;
}

static void term_prompt(term_t *t) {
    char path[FS_PATH_MAX];
    fs_abspath(fs_cwd(), path, sizeof(path));
    term_puts(t, "bolt:", 1);
    term_puts(t, path, 1);
    term_puts(t, "> ", 1);
}

/* kputc sink while a command runs */
static void term_sink(char c) { if (active) term_putc(active, c); }

static void term_exec(term_t *t) {
    t->line[t->len] = 0;
    if (t->len == 0) { term_prompt(t); return; }
    sh_history_add(t->line);
    active = t; t->curcol = 0;
    console_set_sink(term_sink);
    shell_exec_line(t->line);
    console_set_sink(0);
    active = 0;
    term_prompt(t);
}

/* Replace the visible input line with `text`. The grid has no CSI handling, so
 * the old characters are backspaced away one at a time -- which is what the
 * caret does anyway, and keeps this window's editing honest to what it draws. */
static void term_setline(term_t *t, const char *text) {
    while (t->len > 0) { t->len--; term_putc(t, '\b'); }
    for (int i = 0; text && text[i] && t->len < (int)sizeof(t->line) - 1; i++) {
        t->line[t->len++] = text[i];
        term_putc(t, text[i]);
    }
    t->line[t->len] = 0;
    t->pos = t->len;
}

static void term_key(window_t *w, char c) {
    (void)w;
    term_t *t = &term;
    if (c == '\n') {
        term_putc(t, '\n');
        term_exec(t);
        t->len = 0;
        t->pos = 0;
        t->line[0] = 0;
        t->hpos = 0;
    } else if (c == '\b') {
        if (t->pos > 0) {
            char edited[256]; strcpy(edited, t->line);
            int pos = t->pos - 1;
            memmove(edited + pos, edited + pos + 1, (uint32_t)(t->len - pos));
            term_setline(t, edited); t->pos = pos;
        }
    } else if ((unsigned char)c == KEY_LEFT) {
        if (t->pos) t->pos--;
    } else if ((unsigned char)c == KEY_RIGHT) {
        if (t->pos < t->len) t->pos++;
    } else if ((unsigned char)c == KEY_HOME) {
        t->pos = 0;
    } else if ((unsigned char)c == KEY_END) {
        t->pos = t->len;
    } else if ((unsigned char)c == KEY_DEL) {
        if (t->pos < t->len) {
            char edited[256]; strcpy(edited, t->line);
            int pos = t->pos;
            memmove(edited + pos, edited + pos + 1, (uint32_t)(t->len - pos));
            term_setline(t, edited); t->pos = pos;
        }
    } else if ((unsigned char)c == KEY_UP || (unsigned char)c == KEY_DOWN) {
        /* The same history the text shell keeps, in the same order. */
        int want = ((unsigned char)c == KEY_UP) ? t->hpos + 1 : t->hpos - 1;
        if (want < 0 || want > sh_history_count()) return;
        if (t->hpos == 0 && want > 0) {
            t->line[t->len] = 0;
            strncpy(t->saved, t->line, sizeof t->saved - 1);
            t->saved[sizeof t->saved - 1] = 0;
        }
        t->hpos = want;
        term_setline(t, want ? sh_history_get(want) : t->saved);
    } else if (c == '\t') {
        if (t->pos != t->len) return;
        t->line[t->len] = 0;
        int printed = 0, old = t->len;
        /* sh_complete prints its candidate list through kputc, so the sink has
         * to be in place or the list would land on the serial console instead
         * of in the window that asked for it. */
        active = t; console_set_sink(term_sink);
        int n = sh_complete(t->line, (int)sizeof t->line, t->len, &printed);
        console_set_sink(0); active = 0;
        char full[256];
        strncpy(full, t->line, sizeof full - 1);
        full[sizeof full - 1] = 0;
        if (printed) {                        /* the list ended with a newline */
            term_prompt(t);
            t->len = 0; t->line[0] = 0;
            term_setline(t, full);
        } else {
            t->len = n;
            t->pos = n;
            t->line[n] = 0;
            for (int i = old; i < n; i++) term_putc(t, t->line[i]);
        }
    } else if ((unsigned char)c == KEY_COPY) {
        clip_set(t->line, t->len);               /* copy the current input line */
    } else if ((unsigned char)c == KEY_PASTE) {
        const char *p = clip_get();              /* paste: replay each char (\n runs it) */
        for (int i = 0; p[i]; i++)
            if (p[i] != '\r') term_key(w, p[i]);
    } else if (kbd_is_text((unsigned char)c) && t->len < (int)sizeof(t->line) - 1) {
        if (t->pos == t->len) {
            t->line[t->len++] = c; t->line[t->len] = 0;
            t->pos = t->len; term_putc(t, c);
        } else {
            char edited[256]; strcpy(edited, t->line);
            int pos = t->pos;
            memmove(edited + pos + 1, edited + pos, (uint32_t)(t->len - pos + 1));
            edited[pos] = c;
            term_setline(t, edited); t->pos = pos + 1;
        }
    }
}

static void term_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    term_t *t = &term;
    g_fill(cx, cy, cw, ch, TERM_BG);             /* terminal background */

    t->cols = (cw - 2 * PAD) / CELLW; if (t->cols > TCOLS) t->cols = TCOLS; if (t->cols < 1) t->cols = 1;
    t->rows = (ch - 2 * PAD) / CELLH; if (t->rows > TROWS) t->rows = TROWS; if (t->rows < 1) t->rows = 1;
    if (t->cx >= t->cols) t->cx = t->cols - 1;
    if (t->cy >= t->rows) t->cy = t->rows - 1;

    for (int r = 0; r < t->rows; r++) {
        int py = cy + PAD + r * CELLH;
        for (int c = 0; c < t->cols; c++) {
            char g = t->ch[r][c];
            if (g) g_char(cx + PAD + c * CELLW, py, g, pal(t->col[r][c]), 1);
        }
    }

    /* blinking caret (only when focused) -- t->cx/cy track the write cursor
     * (end of the line), so mid-line editing has to walk the caret back by
     * how far behind the end t->pos is. wrapped lines make this an
     * approximation near the wrap point, but nobody types a 220-char command. */
    if (gui_window_focused(w) && (pit_ticks() / 530) % 2 == 0) {
        int cell = t->cy * t->cols + t->cx - (t->len - t->pos);
        if (cell < 0) cell = 0;
        int px = cx + PAD + (cell % t->cols) * CELLW;
        int py = cy + PAD + (cell / t->cols) * CELLH;
        g_round(px, py + 1, 2, CELLH - 2, 1, 0x64D2FF, 255);
    }
}

void terminal_app_init(void) {
    memset(&term, 0, sizeof(term));
    term.cols = 88; term.rows = 29;              /* default grid until first draw */
    window_t *w = gui_add_window("Terminal", 720, 460, COL_ACCENT, ICON_TERMINAL);
    if (!w) return;
    w->draw = term_draw;
    w->key  = term_key;

    term_puts(&term, "BoltOS Terminal  -  BoltShell\n", 1);
    term_puts(&term, "Type 'help' for the full command list.\n\n", 2);
    term_prompt(&term);
}
