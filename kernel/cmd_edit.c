#include <stdint.h>
#include "commands.h"
#include "fs.h"
#include "keyboard.h"
#include "xhci.h"
#include "console.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include <stdio.h>

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_edit.c
 *  `edit` -- a full-screen text editor for the terminal.
 *
 *  The Notes app can edit a file, but only from the desktop. Anything done
 *  from the text shell -- safe mode, a serial console, the recovery shell,
 *  which are exactly the situations where a file needs fixing -- had no way to
 *  change a file at all beyond overwriting it wholesale with `write`. That is
 *  not a text editor; it is a way to lose the rest of the file.
 *
 *  Nano-style rather than modal: no modes to be in the wrong one of, and the
 *  keys are listed on screen, which matters for the one editor on a machine
 *  where there is nothing else to fall back to.
 *
 *  Drawing goes through ANSI escapes (see the CSI handling in kernel/console.c)
 *  rather than a private console call, so the same code paints the framebuffer
 *  console, a serial terminal, and anything else that speaks terminal.
 * ===========================================================================*/

#define MAXLINES  4096
#define MAXCOL    1024
#define TABW      4

typedef struct {
    char    **line;          /* line[i] is a NUL-terminated, heap-owned string */
    int      *len;
    int       nlines;
    int       cap;
    int       cy, cx;        /* caret: line index, column                     */
    int       top;           /* first visible line                            */
    int       dirty;
    int       rows, cols;    /* usable text area                              */
    char      path[FS_PATH_MAX];
    char      msg[96];
} editor;

/* ------------------------------------------------------------- line store --*/
static int ed_grow(editor *e, int want) {
    if (want <= e->cap) return 0;
    int ncap = e->cap ? e->cap * 2 : 128;
    while (ncap < want) ncap *= 2;
    if (ncap > MAXLINES) ncap = MAXLINES;
    if (want > ncap) return -1;
    char **nl = (char **)kmalloc((uint32_t)ncap * sizeof(char *));
    int   *ni = (int *)kmalloc((uint32_t)ncap * sizeof(int));
    if (!nl || !ni) { if (nl) kfree(nl); if (ni) kfree(ni); return -1; }
    for (int i = 0; i < e->nlines; i++) { nl[i] = e->line[i]; ni[i] = e->len[i]; }
    if (e->line) kfree(e->line);
    if (e->len)  kfree(e->len);
    e->line = nl; e->len = ni; e->cap = ncap;
    return 0;
}

static char *dupn(const char *src, int n) {
    char *d = (char *)kmalloc((uint32_t)n + 1);
    if (!d) return 0;
    for (int i = 0; i < n; i++) d[i] = src[i];
    d[n] = 0;
    return d;
}

static int ed_insert_line(editor *e, int at, const char *text, int n) {
    if (e->nlines >= MAXLINES) return -1;
    if (ed_grow(e, e->nlines + 1) != 0) return -1;
    char *s = dupn(text, n);
    if (!s) return -1;
    for (int i = e->nlines; i > at; i--) { e->line[i] = e->line[i-1]; e->len[i] = e->len[i-1]; }
    e->line[at] = s; e->len[at] = n;
    e->nlines++;
    return 0;
}

static void ed_remove_line(editor *e, int at) {
    if (at < 0 || at >= e->nlines) return;
    kfree(e->line[at]);
    for (int i = at; i < e->nlines - 1; i++) { e->line[i] = e->line[i+1]; e->len[i] = e->len[i+1]; }
    e->nlines--;
}

/* Replace line `at` with `n` bytes of `text`. */
static int ed_set_line(editor *e, int at, const char *text, int n) {
    char *s = dupn(text, n);
    if (!s) return -1;
    kfree(e->line[at]);
    e->line[at] = s; e->len[at] = n;
    return 0;
}

static void ed_free(editor *e) {
    for (int i = 0; i < e->nlines; i++) kfree(e->line[i]);
    if (e->line) kfree(e->line);
    if (e->len)  kfree(e->len);
    e->line = 0; e->len = 0; e->nlines = e->cap = 0;
}

/* ------------------------------------------------------------- load/save --*/
static int ed_load(editor *e, const char *path) {
    fs_node *f = fs_lookup(path);
    if (!f) {                                    /* a new file: one empty line */
        snprintf(e->msg, sizeof e->msg, "new file");
        return ed_insert_line(e, 0, "", 0);
    }
    if (f->is_dir) { snprintf(e->msg, sizeof e->msg, "that is a directory"); return -1; }
    if (!fs_may(f, FS_PERM_READ)) {
        snprintf(e->msg, sizeof e->msg, "permission denied");
        return -1;
    }

    const char *d = (const char *)f->data;
    uint32_t n = f->size, start = 0;
    for (uint32_t i = 0; i <= n; i++) {
        if (i == n || d[i] == '\n') {
            uint32_t l = i - start;
            if (l && d[start + l - 1] == '\r') l--;      /* tolerate CRLF */
            if (l > MAXCOL) l = MAXCOL;
            if (i == n && l == 0 && start == n && n) break;   /* trailing NL */
            if (ed_insert_line(e, e->nlines, d + start, (int)l) != 0) return -1;
            start = i + 1;
        }
    }
    if (!e->nlines) ed_insert_line(e, 0, "", 0);
    snprintf(e->msg, sizeof e->msg, "%d line%s", e->nlines, e->nlines == 1 ? "" : "s");
    return 0;
}

static int ed_save(editor *e) {
    uint32_t total = 0;
    for (int i = 0; i < e->nlines; i++) total += (uint32_t)e->len[i] + 1;

    char *out = (char *)kmalloc(total ? total : 1);
    if (!out) { snprintf(e->msg, sizeof e->msg, "out of memory"); return -1; }
    uint32_t o = 0;
    for (int i = 0; i < e->nlines; i++) {
        for (int j = 0; j < e->len[i]; j++) out[o++] = e->line[i][j];
        out[o++] = '\n';
    }

    fs_node *f = fs_lookup(e->path);
    if (!f) f = fs_create(e->path, 0);
    if (!f) { kfree(out); snprintf(e->msg, sizeof e->msg, "cannot create %s", e->path); return -1; }
    if (!fs_may(f, FS_PERM_WRITE)) {
        kfree(out);
        snprintf(e->msg, sizeof e->msg, "permission denied");
        return -1;
    }
    int rc = fs_write(f, out, o);
    kfree(out);
    if (rc != 0) { snprintf(e->msg, sizeof e->msg, "write failed"); return -1; }
    e->dirty = 0;
    snprintf(e->msg, sizeof e->msg, "wrote %u bytes to %s", o, e->path);
    return 0;
}

/* ----------------------------------------------------------------- paint --*/
static void ed_paint(editor *e) {
    kprintf("\x1b[H\x1b[2J");                    /* home, then clear */

    kprintf("  BoltOS edit -- %s%s\n", e->path, e->dirty ? " *" : "");
    for (int i = 0; i < e->cols && i < 78; i++) kputc('-');
    kputc('\n');

    for (int r = 0; r < e->rows; r++) {
        int i = e->top + r;
        if (i >= e->nlines) { kputc('\n'); continue; }
        /* The caret line is marked, since a terminal cursor cannot be relied on
         * once the screen is repainted wholesale. */
        kprintf("%c", i == e->cy ? '>' : ' ');
        int n = e->len[i];
        int room = e->cols - 2;
        for (int c = 0; c < n && c < room; c++) {
            char ch = e->line[i][c];
            kputc((unsigned char)ch < 32 ? '?' : ch);
        }
        if (n > room) kprintf("$");
        kputc('\n');
    }

    for (int i = 0; i < e->cols && i < 78; i++) kputc('-');
    kputc('\n');
    kprintf("  line %d/%d  col %d   %s\n", e->cy + 1, e->nlines, e->cx + 1, e->msg);
    kprintf("  ^S save   ^X exit   ^K cut line   arrows/PgUp/PgDn/Home/End move\n");

    /* Park the terminal cursor where the caret is, for terminals that show one.
     * Rows are 1-based and the text area starts on row 3. */
    int scr_row = 3 + (e->cy - e->top);
    int scr_col = 2 + (e->cx < e->cols - 2 ? e->cx : e->cols - 3);
    kprintf("\x1b[%d;%dH", scr_row, scr_col);
}

static void ed_scroll(editor *e) {
    if (e->cy < e->top) e->top = e->cy;
    if (e->cy >= e->top + e->rows) e->top = e->cy - e->rows + 1;
    if (e->top < 0) e->top = 0;
}

/* --------------------------------------------------------------- editing --*/
static void clamp_cx(editor *e) {
    if (e->cx > e->len[e->cy]) e->cx = e->len[e->cy];
    if (e->cx < 0) e->cx = 0;
}

static void ins_char(editor *e, char c) {
    int n = e->len[e->cy];
    if (n >= MAXCOL) return;
    char *tmp = (char *)kmalloc((uint32_t)n + 2);
    if (!tmp) return;
    for (int i = 0; i < e->cx; i++) tmp[i] = e->line[e->cy][i];
    tmp[e->cx] = c;
    for (int i = e->cx; i < n; i++) tmp[i + 1] = e->line[e->cy][i];
    ed_set_line(e, e->cy, tmp, n + 1);
    kfree(tmp);
    e->cx++;
    e->dirty = 1;
}

static void split_line(editor *e) {
    int n = e->len[e->cy];
    const char *src = e->line[e->cy];
    if (ed_insert_line(e, e->cy + 1, src + e->cx, n - e->cx) != 0) return;
    ed_set_line(e, e->cy, src, e->cx);
    e->cy++; e->cx = 0;
    e->dirty = 1;
}

static void backspace(editor *e) {
    if (e->cx > 0) {
        int n = e->len[e->cy];
        char *tmp = (char *)kmalloc((uint32_t)n + 1);
        if (!tmp) return;
        int o = 0;
        for (int i = 0; i < n; i++) if (i != e->cx - 1) tmp[o++] = e->line[e->cy][i];
        ed_set_line(e, e->cy, tmp, o);
        kfree(tmp);
        e->cx--;
        e->dirty = 1;
    } else if (e->cy > 0) {
        /* Join with the line above, and land the caret where the join happened. */
        int prev = e->cy - 1;
        int pl = e->len[prev], cl = e->len[e->cy];
        char *tmp = (char *)kmalloc((uint32_t)(pl + cl) + 1);
        if (!tmp) return;
        for (int i = 0; i < pl; i++) tmp[i] = e->line[prev][i];
        for (int i = 0; i < cl; i++) tmp[pl + i] = e->line[e->cy][i];
        ed_set_line(e, prev, tmp, pl + cl);
        kfree(tmp);
        ed_remove_line(e, e->cy);
        e->cy = prev; e->cx = pl;
        e->dirty = 1;
    }
}

static void del_forward(editor *e) {
    if (e->cx < e->len[e->cy]) { e->cx++; backspace(e); }
    else if (e->cy < e->nlines - 1) {                 /* pull the next line up */
        int save = e->cx;
        e->cy++; e->cx = 0;
        backspace(e);
        e->cx = save;
    }
}

static void cut_line(editor *e) {
    if (e->nlines == 1) { ed_set_line(e, 0, "", 0); e->cx = 0; e->dirty = 1; return; }
    ed_remove_line(e, e->cy);
    if (e->cy >= e->nlines) e->cy = e->nlines - 1;
    e->cx = 0;
    e->dirty = 1;
}

/* A tiny prompt on the status line, for "go to line" and "save as". */
static int ed_prompt(editor *e, const char *label, char *out, int cap) {
    int n = 0;
    for (;;) {
        kprintf("\x1b[H\x1b[2J");
        kprintf("  %s", label);
        for (int i = 0; i < n; i++) kputc(out[i]);
        kprintf("\n  (Enter to accept, Esc to cancel)\n");
        xhci_poll();
        char c = kbd_getc();
        if (c == '\n') { out[n] = 0; return n; }
        if (c == 0x1B) { (void)e; return -1; }
        if (c == '\b') { if (n) n--; continue; }
        if ((unsigned char)c >= 32 && n < cap - 1) out[n++] = c;
    }
}

/* ------------------------------------------------------------------ main --*/
int cmd_edit(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: edit FILE\n"
                "  A full-screen editor. ^S saves, ^X exits, ^K cuts a line.\n");
        return 1;
    }

    editor e;
    memset(&e, 0, sizeof e);
    /* fs_lookup/fs_create resolve a relative path against the working
     * directory themselves, as they do for every other command. */
    strncpy(e.path, argv[1], sizeof e.path - 1);
    e.path[sizeof e.path - 1] = 0;
    e.rows = 18;                     /* leaves room for the header and footer */
    e.cols = 78;
    e.msg[0] = 0;

    if (ed_load(&e, e.path) != 0) {
        kprintf("edit: %s: %s\n", e.path, e.msg[0] ? e.msg : "cannot open");
        ed_free(&e);
        return 1;
    }

    int running = 1;
    while (running) {
        ed_scroll(&e);
        ed_paint(&e);

        xhci_poll();                              /* pump a USB keyboard */
        char c = kbd_getc();
        e.msg[0] = 0;

        switch (c) {
        case KEY_UP:    if (e.cy > 0) { e.cy--; clamp_cx(&e); } break;
        case KEY_DOWN:  if (e.cy < e.nlines - 1) { e.cy++; clamp_cx(&e); } break;
        case KEY_LEFT:
            if (e.cx > 0) e.cx--;
            else if (e.cy > 0) { e.cy--; e.cx = e.len[e.cy]; }
            break;
        case KEY_RIGHT:
            if (e.cx < e.len[e.cy]) e.cx++;
            else if (e.cy < e.nlines - 1) { e.cy++; e.cx = 0; }
            break;
        case KEY_HOME:  e.cx = 0; break;
        case KEY_END:   e.cx = e.len[e.cy]; break;
        case KEY_PGUP:  e.cy -= e.rows; if (e.cy < 0) e.cy = 0; clamp_cx(&e); break;
        case KEY_PGDN:  e.cy += e.rows; if (e.cy >= e.nlines) e.cy = e.nlines - 1; clamp_cx(&e); break;
        case KEY_DEL:   del_forward(&e); break;
        case '\b':      backspace(&e); break;
        case '\n':      split_line(&e); break;
        case '\t':      for (int i = 0; i < TABW; i++) ins_char(&e, ' '); break;

        case KEY_SAVE:  ed_save(&e); break;       /* ^S */
        case KEY_CUTLINE: cut_line(&e); break;    /* ^K */

        case 0x18: {                              /* ^X: exit */
            if (!e.dirty) { running = 0; break; }
            char b[8];
            int n = ed_prompt(&e, "save before leaving? [y/n] ", b, sizeof b);
            if (n < 0) break;                      /* Esc: stay in the editor */
            if (b[0] == 'y' || b[0] == 'Y') {
                if (ed_save(&e) != 0) break;       /* a failed save must not lose the buffer */
            }
            running = 0;
            break;
        }

        default:
            if (kbd_is_text((unsigned char)c)) ins_char(&e, c);
            break;
        }
    }

    kprintf("\x1b[H\x1b[2J");
    kprintf("edit: %s%s\n", e.path, e.dirty ? " (changes discarded)" : " saved");
    ed_free(&e);
    return 0;
}
