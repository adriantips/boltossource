/* ===========================================================================
 *  BoltOS Mini  -  mini/app_code.c
 *
 *  Code: an editor for Python, C and C++ that runs what it edits. Files
 *  are kept on the SD card in /Code (fs.c), listed on the left; open ones
 *  are tabs; F5 (or Ctrl+R) saves and runs the one in front on a spare core
 *  (rt.c), its output and its input() at the bottom, and an error's line
 *  marked in the text.
 *
 *  The editing is what's expected of one: syntax colours, line numbers,
 *  selection with Shift or the mouse, undo and redo, copy and paste (shared
 *  with the Mac through Screen Sharing), find and replace, go to line,
 *  indentation that follows the code, comment toggling, moving and copying
 *  lines, and the bracket that matches the one at the cursor. Ctrl and the
 *  Mac's Command key both work for the shortcuts.
 *
 *  A buffer is one array of text; an edit moves the tail along, which at
 *  the sizes programs are is quicker than anything cleverer. Line starts
 *  and where comments and strings carry over lines are worked out again
 *  after each edit, in one pass.
 * ===========================================================================*/
#include <stdint.h>
#include <stdarg.h>
#include "ui.h"
#include "fs.h"
#include "rt.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define HOME_DIR  "/Code"
#define MAXBUF    10
#define MAXFILES  200
#define UNDO_MAX  4000

/* ---- buffers --------------------------------------------------------------------------------------- */
typedef struct {
    int pos;
    char *old; int olen;
    char *new_; int nlen;
    int cur_before, cur_after;
    uint32_t group;
} undo_t;

typedef struct {
    char path[FS_PATH];
    char *t; int len, cap;
    int cur, anc;                        /* the cursor, and where the selection began (== cur: none) */
    int top, left;                       /* first line shown, first column shown                     */
    int goal;                            /* the column up and down aim for, -1 none                  */
    int dirty, lang, err_line;
    int *ls; int nl, lcap, lines_ok;     /* where each line starts                                   */
    uint8_t *lst;                        /* the colouring state each line starts in                  */
    undo_t *u; int nu, ucap, uat;
    uint32_t group; uint64_t last_ms; int last_kind, last_end;
} buf_t;

static buf_t *bufs[MAXBUF];
static int nbufs, cb = -1;               /* the one in front */

enum { F_EDIT, F_FILES, F_PROMPT, F_FIND, F_INPUT };
static int focus = F_EDIT;
static int started, card_ok;
static char card_why[96];

/* the file list */
static char dir[FS_PATH] = HOME_DIR;
static fs_ent_t *files; static int nfiles, fsel, ftop;
static int show_files = 1;

/* the output panel */
typedef struct { char *s; int n, cap; uint8_t col; } oline_t;
#define OUT_LINES 2000
static oline_t olines[OUT_LINES];
static int ohead, ocount, oscroll;
static int out_open;
static char run_path[FS_PATH];
static int run_seen_state, was_running;
static char inbuf[512]; static int in_len;
enum { OC_TEXT, OC_ERR, OC_INFO, OC_OK };

/* the prompt (a name, a line number, a yes/no) and find */
enum { PR_NONE, PR_NEWFILE, PR_NEWDIR, PR_RENAME, PR_SAVEAS, PR_GOTO, PR_DELETE, PR_CLOSE };
static int prompt_kind;
static char prompt_text[FS_PATH]; static int prompt_len;
static char prompt_label[160];
static char find_q[128], repl_q[128]; static int find_len, repl_len, find_field, find_hits, find_idx;

static int cw = 8, lh = 18, px = 14;     /* the mono font's cell, worked out when drawing */
static int adv100 = 800;                 /* a hundred of its characters, in pixels: columns land on fractions */
static int colx(int col) { return col * adv100 / 100; }
static int view_rows = 20, view_cols = 80;
static int ed_x, ed_y, ed_h, gut_w;      /* where the text was last drawn (for the mouse)   */
static int sb_x, sb_y, sb_w, tab_y, tab_h, out_y, run_bx, run_bw;
static uint64_t blink0;
static char status_msg[96]; static uint64_t status_at;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(status_msg, sizeof status_msg, fmt, ap);
    va_end(ap);
    status_at = now_ms();
    shell_request_frame();
}

static buf_t *B(void) { return cb >= 0 && cb < nbufs ? bufs[cb] : 0; }

/* ---- lines and colours ---------------------------------------------------------------------------- */
enum { ST_NORMAL, ST_BLOCK, ST_TRIPLE1, ST_TRIPLE2 };

static void lines(buf_t *b) {
    if (b->lines_ok) return;
    int n = 1;
    for (int i = 0; i < b->len; i++) n += b->t[i] == '\n';
    if (n + 1 > b->lcap) {
        kfree(b->ls); kfree(b->lst);
        b->lcap = n + 64;
        b->ls = kmalloc(sizeof(int) * (size_t)b->lcap);
        b->lst = kmalloc((size_t)b->lcap);
    }
    int k = 0, st = ST_NORMAL;
    b->ls[k] = 0; b->lst[k++] = 0;
    int py = b->lang == LANG_PY;
    for (int i = 0; i < b->len; i++) {
        char c = b->t[i];
        /* only what carries over a line matters here: block comments, triple quotes */
        if (st == ST_NORMAL) {
            if (py) {
                if (c == '#') { while (i < b->len && b->t[i] != '\n') i++; if (i >= b->len) break; c = '\n'; }
                else if ((c == '\'' || c == '"') && i + 2 < b->len && b->t[i + 1] == c && b->t[i + 2] == c) { st = c == '\'' ? ST_TRIPLE1 : ST_TRIPLE2; i += 2; continue; }
                else if (c == '\'' || c == '"') {
                    char q = c; i++;
                    while (i < b->len && b->t[i] != q && b->t[i] != '\n') { if (b->t[i] == '\\') i++; i++; }
                    if (i >= b->len) break;
                    c = b->t[i];
                }
            } else if (b->lang == LANG_C || b->lang == LANG_CPP) {
                if (c == '/' && i + 1 < b->len && b->t[i + 1] == '*') { st = ST_BLOCK; i++; continue; }
                if (c == '/' && i + 1 < b->len && b->t[i + 1] == '/') { while (i < b->len && b->t[i] != '\n') i++; if (i >= b->len) break; c = '\n'; }
                else if (c == '"' || c == '\'') {
                    char q = c; i++;
                    while (i < b->len && b->t[i] != q && b->t[i] != '\n') { if (b->t[i] == '\\') i++; i++; }
                    if (i >= b->len) break;
                    c = b->t[i];
                }
            }
        } else if (st == ST_BLOCK) {
            if (c == '*' && i + 1 < b->len && b->t[i + 1] == '/') { st = ST_NORMAL; i++; continue; }
        } else {
            char q = st == ST_TRIPLE1 ? '\'' : '"';
            if (c == '\\') { i++; continue; }
            if (c == q && i + 2 < b->len && b->t[i + 1] == q && b->t[i + 2] == q) { st = ST_NORMAL; i += 2; continue; }
        }
        if (c == '\n') { b->ls[k] = i + 1; b->lst[k++] = (uint8_t)st; }
    }
    b->nl = k;
    b->lines_ok = 1;
}

static int line_of(buf_t *b, int pos) {
    lines(b);
    int lo = 0, hi = b->nl - 1;
    while (lo < hi) { int m = (lo + hi + 1) / 2; if (b->ls[m] <= pos) lo = m; else hi = m - 1; }
    return lo;
}
static int line_start(buf_t *b, int l) { lines(b); return b->ls[l]; }
static int line_end(buf_t *b, int l) { lines(b); return l + 1 < b->nl ? b->ls[l + 1] - 1 : b->len; }

/* columns as shown: a tab goes to the next multiple of four */
static int vcol(buf_t *b, int pos) {
    int s = line_start(b, line_of(b, pos)), c = 0;
    for (int i = s; i < pos; i++) c = b->t[i] == '\t' ? (c / 4 + 1) * 4 : c + 1;
    return c;
}
static int pos_at_col(buf_t *b, int l, int col) {
    int s = line_start(b, l), e = line_end(b, l), c = 0, i = s;
    for (; i < e; i++) {
        int nc = b->t[i] == '\t' ? (c / 4 + 1) * 4 : c + 1;
        if (nc > col) { if (col - c > nc - col) i++; break; }
        c = nc;
    }
    return i;
}

/* ---- syntax --------------------------------------------------------------------------------------- */
enum { K_TEXT, K_KW, K_TYPE, K_STR, K_NUM, K_COM, K_PRE, K_FN, K_BUILTIN };

static const char *const PY_KW[] = { "False", "None", "True", "and", "as", "assert", "async", "await", "break", "class",
    "continue", "def", "del", "elif", "else", "except", "finally", "for", "from", "global", "if", "import", "in", "is",
    "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try", "while", "with", "yield", "match", "case", 0 };
static const char *const PY_BI[] = { "print", "len", "range", "int", "str", "float", "list", "dict", "set", "tuple", "bool",
    "input", "open", "abs", "min", "max", "sum", "sorted", "enumerate", "zip", "map", "filter", "any", "all", "round",
    "isinstance", "type", "super", "repr", "format", "chr", "ord", "hex", "bin", "iter", "next", "object", "self", "cls",
    "reversed", "divmod", "pow", "hash", "getattr", "setattr", "hasattr", "callable", "property", "staticmethod",
    "classmethod", "Exception", "ValueError", "TypeError", "KeyError", "IndexError", "ZeroDivisionError", "RuntimeError",
    "StopIteration", "NameError", "AttributeError", "FileNotFoundError", "__init__", "__name__", "__main__", 0 };
static const char *const C_KW[] = { "auto", "break", "case", "const", "continue", "default", "do", "else", "enum", "extern",
    "for", "goto", "if", "inline", "register", "restrict", "return", "sizeof", "static", "struct", "switch", "typedef",
    "union", "volatile", "while", "true", "false", "NULL", 0 };
static const char *const CPP_KW[] = { "class", "namespace", "template", "typename", "public", "private", "protected",
    "virtual", "override", "new", "delete", "this", "using", "try", "catch", "throw", "nullptr", "const_cast",
    "static_cast", "dynamic_cast", "reinterpret_cast", "friend", "operator", "explicit", "noexcept", "constexpr",
    "final", "mutable", 0 };
static const char *const C_TYPES[] = { "int", "char", "float", "double", "void", "long", "short", "unsigned", "signed",
    "bool", "size_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "FILE",
    "string", "vector", "map", "set", "pair", "auto", "std", "cout", "cin", "endl", "cerr", "unordered_map", "queue",
    "stack", "deque", "list", "array", "unordered_set", "ostream", "istream", 0 };

static int in_list(const char *const *l, const char *s, int n) {
    for (; *l; l++) if ((int)strlen(*l) == n && !memcmp(*l, s, (size_t)n)) return 1;
    return 0;
}
static int idch(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

typedef void (*tok_cb)(int start, int len, int kind, void *ctx);

/* The tokens of one line, from the state it starts in. */
static void tokens(buf_t *b, int l, tok_cb cb, void *ctx) {
    int s = line_start(b, l), e = line_end(b, l), st = b->lst[l], i = s;
    int py = b->lang == LANG_PY, c_ = b->lang == LANG_C || b->lang == LANG_CPP;
    const char *t = b->t;
    if (!py && !c_) { cb(s, e - s, K_TEXT, ctx); return; }
    if (st == ST_BLOCK) {
        int j = i;
        while (j < e && !(t[j] == '*' && j + 1 < e && t[j + 1] == '/')) j++;
        if (j < e) j += 2;
        cb(i, j - i, K_COM, ctx); i = j;
    } else if (st == ST_TRIPLE1 || st == ST_TRIPLE2) {
        char q = st == ST_TRIPLE1 ? '\'' : '"';
        int j = i;
        while (j < e) {
            if (t[j] == '\\') { j += 2; continue; }
            if (t[j] == q && j + 2 < b->len && t[j + 1] == q && t[j + 2] == q) { j += 3; break; }
            j++;
        }
        if (j > e) j = e;
        cb(i, j - i, K_STR, ctx); i = j;
    }
    /* a preprocessor line is one colour up to a comment */
    if (c_ && st == ST_NORMAL) {
        int k = s; while (k < e && (t[k] == ' ' || t[k] == '\t')) k++;
        if (k < e && t[k] == '#') {
            int j = k;
            while (j < e && !(t[j] == '/' && j + 1 < e && (t[j + 1] == '/' || t[j + 1] == '*'))) j++;
            /* #include <x>: the header like a string */
            cb(s, j - s, K_PRE, ctx);
            i = j;
        }
    }
    while (i < e) {
        char c = t[i];
        int j = i + 1;
        if (py && c == '#') { cb(i, e - i, K_COM, ctx); return; }
        if (c_ && c == '/' && i + 1 < e && t[i + 1] == '/') { cb(i, e - i, K_COM, ctx); return; }
        if (c_ && c == '/' && i + 1 < e && t[i + 1] == '*') {
            j = i + 2;
            while (j < e && !(t[j] == '*' && j + 1 < e && t[j + 1] == '/')) j++;
            j = j < e ? j + 2 : e;
            cb(i, j - i, K_COM, ctx); i = j; continue;
        }
        if (c == '"' || c == '\'') {
            if (py && i + 2 < e && t[i + 1] == c && t[i + 2] == c) {
                j = i + 3;
                while (j < e) {
                    if (t[j] == '\\') { j += 2; continue; }
                    if (t[j] == c && j + 2 < b->len && t[j + 1] == c && t[j + 2] == c) { j += 3; break; }
                    j++;
                }
                if (j > e) j = e;
            } else {
                while (j < e && t[j] != c) { if (t[j] == '\\') j++; j++; }
                if (j < e) j++;
                if (j > e) j = e;
            }
            /* a string prefix (f, r, b) is part of it */
            cb(i, j - i, K_STR, ctx); i = j; continue;
        }
        if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < e && t[i + 1] >= '0' && t[i + 1] <= '9')) {
            while (j < e && (idch((unsigned char)t[j]) || t[j] == '.' || ((t[j] == '+' || t[j] == '-') && (t[j - 1] | 32) == 'e'))) j++;
            cb(i, j - i, K_NUM, ctx); i = j; continue;
        }
        if (idch((unsigned char)c)) {
            while (j < e && idch((unsigned char)t[j])) j++;
            int n = j - i, k = K_TEXT;
            /* f"..." and friends */
            if (py && j < e && (t[j] == '"' || t[j] == '\'') && n <= 2) {
                int pfx = 1;
                for (int q = i; q < j; q++) if (!strchr("fFrRbBuU", t[q])) pfx = 0;
                if (pfx) { int q = j + 1; char qc = t[j]; while (q < e && t[q] != qc) { if (t[q] == '\\') q++; q++; } if (q < e) q++; if (q > e) q = e; cb(i, q - i, K_STR, ctx); i = q; continue; }
            }
            if (py) { if (in_list(PY_KW, t + i, n)) k = K_KW; else if (in_list(PY_BI, t + i, n)) k = K_BUILTIN; }
            else {
                if (in_list(C_KW, t + i, n) || (b->lang == LANG_CPP && in_list(CPP_KW, t + i, n))) k = K_KW;
                else if (in_list(C_TYPES, t + i, n)) k = K_TYPE;
            }
            if (k == K_TEXT) {
                int q = j; while (q < e && t[q] == ' ') q++;
                if (q < e && t[q] == '(') k = K_FN;
                /* `class Name` / `def name`: the name stands out too */
                if (i >= 4 + s && py && (!memcmp(t + i - 4, "def ", 4) || (i >= 6 + s && !memcmp(t + i - 6, "class ", 6)))) k = K_FN;
            }
            cb(i, n, k, ctx); i = j; continue;
        }
        while (j < e && !idch((unsigned char)t[j]) && t[j] != '"' && t[j] != '\'' && t[j] != '#' && t[j] != '/' && !(t[j] >= '0' && t[j] <= '9')) j++;
        cb(i, j - i, K_TEXT, ctx); i = j;
    }
}

static uint32_t kind_color(int k) {
    int d = th.dark;
    switch (k) {
    case K_KW: return d ? th.accent : gfx_shade(th.accent, 78);
    case K_TYPE: return d ? 0x4FB8A8 : 0x157A72;
    case K_STR: return d ? 0x9CC37A : 0x3B7A24;
    case K_NUM: return d ? 0xE0A25E : 0xA05A10;
    case K_COM: return th.faint;
    case K_PRE: return d ? 0xC48ADB : 0x8A3CA6;
    case K_FN: return d ? 0xE2D08C : 0x765A00;
    case K_BUILTIN: return d ? 0x7DB4E8 : 0x1F5C9E;
    }
    return th.text;
}

/* ---- editing ---------------------------------------------------------------------------------------- */
static int sel_lo(buf_t *b) { return b->cur < b->anc ? b->cur : b->anc; }
static int sel_hi(buf_t *b) { return b->cur > b->anc ? b->cur : b->anc; }
static int has_sel(buf_t *b) { return b->cur != b->anc; }

static int reserve(buf_t *b, int need) {
    if (need + 1 <= b->cap) return 0;
    int nc = b->cap ? b->cap : 1024;
    while (nc < need + 1) nc *= 2;
    char *n = kmalloc((size_t)nc);
    if (!n) { say("Out of memory"); return -1; }
    if (b->t) { memcpy(n, b->t, (size_t)b->len + 1); kfree(b->t); }
    b->t = n; b->cap = nc;
    return 0;
}

static char *dup_text(const char *s, int n) {
    char *d = kmalloc((size_t)(n ? n : 1));
    if (d && n) memcpy(d, s, (size_t)n);
    return d;
}

static void undo_free(undo_t *u) { kfree(u->old); kfree(u->new_); }

/* Replace [pos, pos+del) with ins. The one place text changes; with `record`, undo can take it back. */
static void replace(buf_t *b, int pos, int del, const char *ins, int nins, int record, int cur_after) {
    if (pos < 0) pos = 0;
    if (pos + del > b->len) del = b->len - pos;
    if (reserve(b, b->len - del + nins)) return;
    if (record) {
        /* drop what could have been redone, then keep within the limit */
        for (int i = b->uat; i < b->nu; i++) undo_free(&b->u[i]);
        b->nu = b->uat;
        if (b->nu == UNDO_MAX) { undo_free(&b->u[0]); memmove(b->u, b->u + 1, sizeof(undo_t) * (UNDO_MAX - 1)); b->nu--; }
        if (b->nu == b->ucap) {
            int nc = b->ucap ? b->ucap * 2 : 64;
            undo_t *n = kmalloc(sizeof(undo_t) * (size_t)nc);
            if (n) { if (b->u) { memcpy(n, b->u, sizeof(undo_t) * (size_t)b->nu); kfree(b->u); } b->u = n; b->ucap = nc; }
        }
        if (b->nu < b->ucap) {
            undo_t *u = &b->u[b->nu++];
            u->pos = pos; u->old = dup_text(b->t + pos, del); u->olen = del;
            u->new_ = dup_text(ins, nins); u->nlen = nins;
            u->cur_before = b->cur; u->cur_after = cur_after; u->group = b->group;
            b->uat = b->nu;
        }
    }
    memmove(b->t + pos + nins, b->t + pos + del, (size_t)(b->len - pos - del + 1));
    if (nins) memcpy(b->t + pos, ins, (size_t)nins);
    b->len += nins - del;
    b->dirty = 1;
    b->lines_ok = 0;
    b->err_line = 0;
    b->cur = b->anc = cur_after;
    b->goal = -1;
}

/* Typing joins the last edit's group when it follows on from it. */
enum { E_TYPE = 1, E_DEL, E_OTHER };
static void begin(buf_t *b, int kind, int at) {
    uint64_t t = now_ms();
    if (kind != b->last_kind || kind == E_OTHER || t - b->last_ms > 1200 || at != b->last_end) b->group++;
    b->last_kind = kind; b->last_ms = t;
}

static void insert_text(buf_t *b, const char *s, int n, int kind) {
    int lo = sel_lo(b), hi = sel_hi(b);
    begin(b, has_sel(b) ? E_OTHER : kind, lo);
    replace(b, lo, hi - lo, s, n, 1, lo + n);
    b->last_end = lo + n;
}

static void erase(buf_t *b, int lo, int hi) {
    if (lo < 0) lo = 0;
    if (hi > b->len) hi = b->len;
    if (lo >= hi) return;
    begin(b, E_DEL, hi);
    replace(b, lo, hi - lo, 0, 0, 1, lo);
    b->last_end = lo;
}

static void undo(buf_t *b) {
    if (!b->uat) { say("Nothing to undo"); return; }
    uint32_t g = b->u[b->uat - 1].group;
    while (b->uat && b->u[b->uat - 1].group == g) {
        undo_t *u = &b->u[--b->uat];
        replace(b, u->pos, u->nlen, u->old, u->olen, 0, u->cur_before);
    }
    b->group++;
    b->last_kind = 0;
}

static void redo(buf_t *b) {
    if (b->uat >= b->nu) { say("Nothing to redo"); return; }
    uint32_t g = b->u[b->uat].group;
    while (b->uat < b->nu && b->u[b->uat].group == g) {
        undo_t *u = &b->u[b->uat++];
        replace(b, u->pos, u->olen, u->new_, u->nlen, 0, u->cur_after);
    }
    b->group++;
    b->last_kind = 0;
}

static void buf_free(buf_t *b) {
    for (int i = 0; i < b->nu; i++) undo_free(&b->u[i]);
    kfree(b->u); kfree(b->t); kfree(b->ls); kfree(b->lst);
    kfree(b);
}

/* ---- files ---------------------------------------------------------------------------------------- */
static void list_files(void) {
    if (!files) files = kmalloc(sizeof(fs_ent_t) * MAXFILES);
    if (!files) return;
    nfiles = card_ok ? fs_list(dir, files, MAXFILES) : 0;
    if (nfiles < 0) { nfiles = 0; snprintf(dir, sizeof dir, "%s", HOME_DIR); }
    if (fsel > nfiles) fsel = nfiles;
}

static int find_buf(const char *path) {
    for (int i = 0; i < nbufs; i++) if (!strcasecmp(bufs[i]->path, path)) return i;
    return -1;
}

static buf_t *new_buf(const char *path, const char *text, int len) {
    if (nbufs == MAXBUF) {
        /* the oldest without unsaved changes makes room */
        int k = -1;
        for (int i = 0; i < nbufs && k < 0; i++) if (!bufs[i]->dirty && i != cb) k = i;
        if (k < 0) { say("Too many files open: close one (Ctrl+W)"); return 0; }
        buf_free(bufs[k]);
        memmove(bufs + k, bufs + k + 1, sizeof(buf_t *) * (size_t)(nbufs - k - 1));
        nbufs--;
        if (cb > k) cb--;
    }
    buf_t *b = kmalloc(sizeof(buf_t));
    if (!b) return 0;
    memset(b, 0, sizeof *b);
    snprintf(b->path, sizeof b->path, "%s", path);
    b->lang = lang_of(path);
    b->goal = -1;
    if (reserve(b, len)) { kfree(b); return 0; }
    /* \r\n read as \n */
    int o = 0;
    for (int i = 0; i < len; i++) { if (text[i] == '\r' && i + 1 < len && text[i + 1] == '\n') continue; b->t[o++] = text[i]; }
    b->t[o] = 0; b->len = o;
    bufs[nbufs] = b;
    cb = nbufs++;
    return b;
}

static void remember_open(void) {
    buf_t *b = B();
    if (b && b->path[0]) keep_put("code", b->path, (int)strlen(b->path));
}

static int open_path(const char *path) {
    int k = find_buf(path);
    if (k >= 0) { cb = k; focus = F_EDIT; remember_open(); return 0; }
    char *d; uint32_t n;
    if (fs_read(path, &d, &n)) return -1;
    for (uint32_t i = 0; i < n && i < 8192; i++)
        if (!d[i]) { kfree(d); snprintf(card_why, sizeof card_why, "%s isn't a text file", fs_basename(path)); say("%s isn't a text file", fs_basename(path)); return -1; }
    buf_t *b = new_buf(path, d, (int)n);
    kfree(d);
    if (!b) return -1;
    focus = F_EDIT;
    remember_open();
    return 0;
}

static int save(buf_t *b) {
    if (!card_ok) { say("No SD card: %s", card_why); return -1; }
    if (fs_write(b->path, b->t, (uint32_t)b->len)) { say("Couldn't save: %s", fs_why()); return -1; }
    b->dirty = 0;
    say("Saved %s", fs_basename(b->path));
    list_files();
    return 0;
}

static void close_buf(int i) {
    buf_free(bufs[i]);
    memmove(bufs + i, bufs + i + 1, sizeof(buf_t *) * (size_t)(nbufs - i - 1));
    nbufs--;
    if (cb >= nbufs) cb = nbufs - 1;
    remember_open();
}

/* The examples a new card starts with. */
static const char EX_PY[] =
    "# Python on BoltOS Mini. F5 runs it; the output is below.\n"
    "\n"
    "def greet(name):\n"
    "    return f\"Hello, {name}!\"\n"
    "\n"
    "class Counter:\n"
    "    def __init__(self):\n"
    "        self.counts = {}\n"
    "\n"
    "    def add(self, word):\n"
    "        self.counts[word] = self.counts.get(word, 0) + 1\n"
    "\n"
    "    def top(self, n=3):\n"
    "        return sorted(self.counts.items(), key=lambda kv: -kv[1])[:n]\n"
    "\n"
    "name = input(\"What's your name? \")\n"
    "print(greet(name or \"world\"))\n"
    "\n"
    "c = Counter()\n"
    "for w in \"the cat and the hat and the bat\".split():\n"
    "    c.add(w)\n"
    "print(\"Most common:\", c.top())\n"
    "\n"
    "squares = [n * n for n in range(1, 11)]\n"
    "print(\"Squares:\", squares, \"sum\", sum(squares))\n"
    "print(\"2 ** 100 =\", 2 ** 100)\n";
static const char EX_C[] =
    "// C on BoltOS Mini. F5 runs it.\n"
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "\n"
    "typedef struct {\n"
    "    char name[32];\n"
    "    int score;\n"
    "} Player;\n"
    "\n"
    "static int by_score(const void *a, const void *b) {\n"
    "    return ((const Player *)b)->score - ((const Player *)a)->score;\n"
    "}\n"
    "\n"
    "int main(void) {\n"
    "    Player team[] = { {\"Ada\", 92}, {\"Linus\", 85}, {\"Grace\", 97} };\n"
    "    int n = sizeof team / sizeof team[0];\n"
    "    qsort(team, n, sizeof(Player), by_score);\n"
    "    for (int i = 0; i < n; i++)\n"
    "        printf(\"%d. %-6s %3d\\n\", i + 1, team[i].name, team[i].score);\n"
    "\n"
    "    int *fib = malloc(20 * sizeof(int));\n"
    "    fib[0] = 0; fib[1] = 1;\n"
    "    for (int i = 2; i < 20; i++) fib[i] = fib[i - 1] + fib[i - 2];\n"
    "    printf(\"fib(19) = %d, average score %.2f\\n\", fib[19], (92 + 85 + 97) / 3.0);\n"
    "    free(fib);\n"
    "    return 0;\n"
    "}\n";
static const char EX_CPP[] =
    "// C++ on BoltOS Mini. F5 runs it.\n"
    "#include <iostream>\n"
    "#include <vector>\n"
    "#include <string>\n"
    "#include <algorithm>\n"
    "using namespace std;\n"
    "\n"
    "class Shape {\n"
    "public:\n"
    "    virtual double area() const = 0;\n"
    "    virtual string name() const = 0;\n"
    "};\n"
    "\n"
    "class Circle : public Shape {\n"
    "    double r;\n"
    "public:\n"
    "    Circle(double r) : r(r) {}\n"
    "    double area() const override { return 3.14159265 * r * r; }\n"
    "    string name() const override { return \"circle\"; }\n"
    "};\n"
    "\n"
    "class Square : public Shape {\n"
    "    double s;\n"
    "public:\n"
    "    Square(double s) : s(s) {}\n"
    "    double area() const override { return s * s; }\n"
    "    string name() const override { return \"square\"; }\n"
    "};\n"
    "\n"
    "int main() {\n"
    "    vector<Shape*> shapes = { new Circle(1.5), new Square(2), new Circle(0.5) };\n"
    "    for (Shape* s : shapes)\n"
    "        cout << s->name() << \" has area \" << s->area() << endl;\n"
    "\n"
    "    vector<int> v = {5, 3, 9, 1, 7};\n"
    "    sort(v.begin(), v.end());\n"
    "    cout << \"sorted:\";\n"
    "    for (int x : v) cout << \" \" << x;\n"
    "    cout << endl;\n"
    "    return 0;\n"
    "}\n";

static void first_open(void) {
    card_ok = fs_mount() == 0;
    if (!card_ok) snprintf(card_why, sizeof card_why, "%s", fs_why());
    if (card_ok) {
        fs_ent_t st, one;
        if (fs_stat(HOME_DIR, &st) != 0 || fs_list(HOME_DIR, &one, 1) == 0) {
            fs_mkdir(HOME_DIR);
            fs_write(HOME_DIR "/hello.py", EX_PY, sizeof EX_PY - 1);
            fs_write(HOME_DIR "/hello.c", EX_C, sizeof EX_C - 1);
            fs_write(HOME_DIR "/hello.cpp", EX_CPP, sizeof EX_CPP - 1);
        }
        list_files();
        char last[FS_PATH]; int n = keep_get("code", last, sizeof last - 1);
        if (n > 0) { last[n] = 0; if (open_path(last) == 0) return; }
        if (open_path(HOME_DIR "/hello.py") == 0) return;
        for (int i = 0; i < nfiles; i++) if (!files[i].dir) { char p[FS_PATH]; snprintf(p, sizeof p, "%s/%s", dir, files[i].name); if (!open_path(p)) return; }
    }
    /* no card: a scratch buffer that can still run */
    new_buf(HOME_DIR "/scratch.py", EX_PY, sizeof EX_PY - 1);
    bufs[cb]->dirty = 0;
}

/* ---- the output panel ------------------------------------------------------------------------------- */
static oline_t *oline_new(int col) {
    int i = (ohead + ocount) % OUT_LINES;
    if (ocount == OUT_LINES) { kfree(olines[ohead].s); olines[ohead].s = 0; ohead = (ohead + 1) % OUT_LINES; i = (ohead + ocount - 1) % OUT_LINES; }
    else ocount++;
    oline_t *o = &olines[i];
    o->s = 0; o->n = o->cap = 0; o->col = (uint8_t)col;
    return o;
}
static oline_t *oline_last(void) { return ocount ? &olines[(ohead + ocount - 1) % OUT_LINES] : 0; }
static int out_partial;                  /* the last line hasn't ended */

static void oline_add(oline_t *o, const char *s, int n) {
    if (o->n + n > 4000) n = 4000 - o->n;
    if (n <= 0) return;
    if (o->n + n + 1 > o->cap) {
        int nc = o->cap ? o->cap * 2 : 64;
        while (nc < o->n + n + 1) nc *= 2;
        char *x = kmalloc((size_t)nc);
        if (!x) return;
        if (o->s) { memcpy(x, o->s, (size_t)o->n); kfree(o->s); }
        o->s = x; o->cap = nc;
    }
    memcpy(o->s + o->n, s, (size_t)n);
    o->n += n;
}

static void out_text(const char *s, int n, int col) {
    for (int i = 0; i < n; ) {
        oline_t *o = out_partial ? oline_last() : 0;
        if (!o || o->col != col) { o = oline_new(col); out_partial = 1; }
        int j = i;
        while (j < n && s[j] != '\n' && s[j] != '\r' && s[j] != 27) j++;
        oline_add(o, s + i, j - i);
        if (j >= n) break;
        if (s[j] == '\n') out_partial = 0;
        else if (s[j] == '\r') { if (j + 1 < n && s[j + 1] == '\n') { j++; out_partial = 0; } else o->n = 0; }
        else {                                       /* ESC [ ... letter: colours we don't do */
            j++;
            if (j < n && s[j] == '[') { j++; while (j < n && !((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z'))) j++; }
        }
        i = j + 1;
    }
    oscroll = 0;
}
static void out_line(int col, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void out_line(int col, const char *fmt, ...) {
    char b[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (out_partial) out_text("\n", 1, OC_TEXT);
    if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
    out_text(b, n, col);
    out_text("\n", 1, col);
}
static void out_clear(void) {
    for (int i = 0; i < ocount; i++) kfree(olines[(ohead + i) % OUT_LINES].s);
    ohead = ocount = 0; out_partial = 0; oscroll = 0;
}

/* ---- running ------------------------------------------------------------------------------------------ */
static int running(void) { int s = run_state(); return s == RUN_STARTING || s == RUN_RUNNING || s == RUN_INPUT; }

static void run_current(void) {
    buf_t *b = B();
    if (!b) return;
    if (running()) { say("It's still running: Esc or Shift+F5 stops it"); return; }
    int lang = b->lang;
    if (lang == LANG_NONE) { say("Only .py, .c and .cpp files run"); return; }
    if (b->dirty && card_ok) save(b);
    out_open = 1;
    out_clear();
    b->err_line = 0;
    snprintf(run_path, sizeof run_path, "%s", b->path);
    out_line(OC_INFO, TXT_RARROW " %s  (%s)", fs_basename(b->path), lang_name(lang));
    if (run_start(lang, b->t, (uint32_t)b->len, b->path)) { out_line(OC_ERR, "%s", run_message()); return; }
    was_running = 1;
    in_len = 0;
    blink0 = now_ms();
}

/* Called every loop while the app is open, and from the shell's poll. */
static int run_follow(void) {
    int news = run_poll();
    char buf[2048];
    int n;
    while ((n = run_output(buf, sizeof buf)) > 0) { out_text(buf, n, OC_TEXT); news = 1; }
    int st = run_state();
    if (st == RUN_INPUT && focus != F_INPUT && focus != F_PROMPT) { focus = F_INPUT; out_open = 1; news = 1; }
    if (st != RUN_INPUT && focus == F_INPUT) focus = F_EDIT;
    if (was_running && st == RUN_DONE && !running()) {
        was_running = 0;
        if (run_ok()) out_line(OC_OK, "%s", run_message());
        else {
            const char *m = run_message();
            if (out_partial) out_text("\n", 1, OC_TEXT);
            out_text(m, (int)strlen(m), OC_ERR);
            out_text("\n", 1, OC_ERR);
            int line = run_error_line();
            int k = find_buf(run_path);
            if (line > 0 && k >= 0) {
                buf_t *b = bufs[k];
                b->err_line = line;
                lines(b);
                if (line <= b->nl && k == cb) {
                    if (line - 1 < b->top || line - 1 >= b->top + view_rows) b->top = line - 1 - view_rows / 2;
                    if (b->top < 0) b->top = 0;
                }
            }
        }
        list_files();
        news = 1;
    }
    if (st != run_seen_state) { run_seen_state = st; news = 1; }
    return news;
}

/* ---- the view follows the cursor --------------------------------------------------------------------- */
static void follow(buf_t *b) {
    int l = line_of(b, b->cur);
    if (l < b->top) b->top = l;
    if (l >= b->top + view_rows) b->top = l - view_rows + 1;
    if (b->top < 0) b->top = 0;
    int c = vcol(b, b->cur);
    int cols = view_cols > 8 ? view_cols : 8;
    if (c < b->left) b->left = c > 4 ? c - 4 : 0;
    if (c >= b->left + cols - 1) b->left = c - cols + 4;
    if (b->left < 0) b->left = 0;
}

/* ---- commands -------------------------------------------------------------------------------------------- */
static void copy_sel(buf_t *b, int cut) {
    if (!has_sel(b)) {
        /* nothing chosen: the whole line, as editors do */
        int l = line_of(b, b->cur), s = line_start(b, l), e = line_end(b, l);
        char *tmp = kmalloc((size_t)(e - s + 2));
        if (!tmp) return;
        memcpy(tmp, b->t + s, (size_t)(e - s)); tmp[e - s] = '\n';
        clip_set(tmp, e - s + 1);
        kfree(tmp);
        if (cut) { begin(b, E_OTHER, 0); replace(b, s, (e < b->len ? e + 1 : e) - s, 0, 0, 1, s); }
        say(cut ? "Cut the line" : "Copied the line");
        return;
    }
    int lo = sel_lo(b), hi = sel_hi(b);
    clip_set(b->t + lo, hi - lo);
    if (cut) { begin(b, E_OTHER, 0); replace(b, lo, hi - lo, 0, 0, 1, lo); }
    else say("Copied %d characters", hi - lo);
}

static void paste(buf_t *b) {
    int n; const char *s = clip_get(&n);
    if (!n) { say("The clipboard is empty"); return; }
    begin(b, E_OTHER, 0);
    insert_text(b, s, n, E_OTHER);
    b->group++;
}

static int indent_of(buf_t *b, int l) {
    int s = line_start(b, l), e = line_end(b, l), c = 0;
    for (int i = s; i < e && (b->t[i] == ' ' || b->t[i] == '\t'); i++) c = b->t[i] == '\t' ? (c / 4 + 1) * 4 : c + 1;
    return c;
}

static void newline(buf_t *b) {
    int l = line_of(b, sel_lo(b)), s = line_start(b, l);
    int ind = indent_of(b, l);
    int at = sel_lo(b);
    /* what's before the cursor on this line, without trailing blanks */
    int k = at - 1;
    while (k >= s && (b->t[k] == ' ' || b->t[k] == '\t')) k--;
    char last = k >= s ? b->t[k] : 0;
    if (ind > at - s) ind = at - s;                   /* Enter inside the indentation */
    int more = 0, between = 0;
    if (b->lang == LANG_PY && last == ':') more = 4;
    if ((b->lang == LANG_C || b->lang == LANG_CPP) && (last == '{' || last == '(' || last == '[')) more = 4;
    if (b->lang == LANG_PY && last != ':') {
        /* after return / pass / break / continue / raise, the next line comes back out */
        int ws = s; while (ws < at && (b->t[ws] == ' ' || b->t[ws] == '\t')) ws++;
        static const char *const outs[] = { "return", "pass", "break", "continue", "raise", 0 };
        for (int i = 0; outs[i]; i++) {
            int n = (int)strlen(outs[i]);
            if (at - ws >= n && !memcmp(b->t + ws, outs[i], (size_t)n) && (at - ws == n || !idch((unsigned char)b->t[ws + n]))) { more = ind >= 4 ? -4 : -ind; break; }
        }
    }
    int hi = sel_hi(b);
    if (more == 4 && hi < b->len && (b->t[hi] == '}' || b->t[hi] == ')' || b->t[hi] == ']') && (last == '{' || last == '(' || last == '[')) between = 1;
    char buf[300]; int n = 0;
    buf[n++] = '\n';
    for (int i = 0; i < ind + more && n < 200; i++) buf[n++] = ' ';
    int cur_at = n;
    if (between) { buf[n++] = '\n'; for (int i = 0; i < ind && n < 290; i++) buf[n++] = ' '; }
    begin(b, E_OTHER, 0);
    int lo = sel_lo(b);
    replace(b, lo, hi - lo, buf, n, 1, lo + cur_at);
    b->group++;
}

/* Lines [l0, l1]: indent (dir 1) or take an indent off (dir -1). */
static void shift_lines(buf_t *b, int l0, int l1, int dir) {
    begin(b, E_OTHER, 0);
    int cur_l = line_of(b, b->cur), anc_l = line_of(b, b->anc);
    int cur_c = b->cur - line_start(b, cur_l), anc_c = b->anc - line_start(b, anc_l);
    for (int l = l0; l <= l1; l++) {
        int s = line_start(b, l), e = line_end(b, l);
        if (dir > 0) { if (e > s) replace(b, s, 0, "    ", 4, 1, b->cur); if (l == cur_l) cur_c += 4; if (l == anc_l) anc_c += 4; }
        else {
            int k = 0;
            while (k < 4 && s + k < e && b->t[s + k] == ' ') k++;
            if (!k && s < e && b->t[s] == '\t') k = 1;
            if (k) { replace(b, s, k, 0, 0, 1, b->cur); if (l == cur_l) cur_c -= k; if (l == anc_l) anc_c -= k; }
        }
    }
    if (cur_c < 0) cur_c = 0;
    if (anc_c < 0) anc_c = 0;
    int ce = line_end(b, cur_l) - line_start(b, cur_l), ae = line_end(b, anc_l) - line_start(b, anc_l);
    b->cur = line_start(b, cur_l) + (cur_c < ce ? cur_c : ce);
    b->anc = line_start(b, anc_l) + (anc_c < ae ? anc_c : ae);
    b->group++;
}

static void toggle_comment(buf_t *b) {
    const char *mark = b->lang == LANG_PY ? "# " : "// ";
    int ml = (int)strlen(mark);
    int l0 = line_of(b, sel_lo(b)), l1 = line_of(b, sel_hi(b));
    if (l1 > l0 && sel_hi(b) == line_start(b, l1)) l1--;
    /* all of them commented: take it off; else put it on, at the shallowest indent */
    int all = 1, minind = 1 << 30;
    for (int l = l0; l <= l1; l++) {
        int s = line_start(b, l), e = line_end(b, l), i = s;
        while (i < e && b->t[i] == ' ') i++;
        if (i == e) continue;
        if (i - s < minind) minind = i - s;
        if (e - i < ml - 1 || memcmp(b->t + i, mark, (size_t)ml - 1)) all = 0;
    }
    if (minind == 1 << 30) return;
    begin(b, E_OTHER, 0);
    int keep_cur = b->cur, keep_anc = b->anc, delta = 0;
    for (int l = l0; l <= l1; l++) {
        int s = line_start(b, l), e = line_end(b, l), i = s;
        while (i < e && b->t[i] == ' ') i++;
        if (i == e) continue;
        if (all) {
            int k = (e - i >= ml && !memcmp(b->t + i, mark, (size_t)ml)) ? ml : ml - 1;
            replace(b, i, k, 0, 0, 1, keep_cur); delta -= k;
        } else { replace(b, s + minind, 0, mark, ml, 1, keep_cur); delta += ml; }
    }
    b->cur = keep_cur + (l0 == l1 ? delta : 0); b->anc = l0 == l1 ? b->cur : keep_anc;
    if (l0 != l1) { b->anc = line_start(b, l0); b->cur = line_end(b, l1); }
    if (b->cur > b->len) b->cur = b->len;
    if (b->cur < 0) b->cur = 0;
    if (b->anc > b->len) b->anc = b->len;
    b->group++;
}

static void dup_line(buf_t *b) {
    int l = line_of(b, b->cur), s = line_start(b, l), e = line_end(b, l), col = b->cur - s;
    char *tmp = kmalloc((size_t)(e - s + 1));
    if (!tmp) return;
    tmp[0] = '\n'; memcpy(tmp + 1, b->t + s, (size_t)(e - s));
    begin(b, E_OTHER, 0);
    replace(b, e, 0, tmp, e - s + 1, 1, e + 1 + col);
    kfree(tmp);
    b->group++;
}

static void delete_line(buf_t *b) {
    int l = line_of(b, b->cur), s = line_start(b, l), e = line_end(b, l);
    begin(b, E_OTHER, 0);
    if (e < b->len) replace(b, s, e - s + 1, 0, 0, 1, s);
    else if (s > 0) replace(b, s - 1, e - s + 1, 0, 0, 1, s - 1 > 0 ? line_start(b, line_of(b, s - 1)) : 0);
    else replace(b, s, e - s, 0, 0, 1, s);
    b->group++;
}

static void move_line(buf_t *b, int dir) {
    int l = line_of(b, b->cur);
    lines(b);
    int other = l + dir;
    if (other < 0 || other >= b->nl) return;
    int a = dir < 0 ? other : l, c = a + 1;            /* swap lines a and c */
    int as = line_start(b, a), ae = line_end(b, a), cs = line_start(b, c), ce = line_end(b, c);
    int col = b->cur - line_start(b, l);
    int la = ae - as, lc = ce - cs;
    char *tmp = kmalloc((size_t)(la + lc + 2));
    if (!tmp) return;
    memcpy(tmp, b->t + cs, (size_t)lc); tmp[lc] = '\n'; memcpy(tmp + lc + 1, b->t + as, (size_t)la);
    begin(b, E_OTHER, 0);
    int newcur = dir < 0 ? as + col : as + lc + 1 + col;
    replace(b, as, ce - as, tmp, la + lc + 1, 1, newcur);
    kfree(tmp);
    b->group++;
}

/* The bracket matching the one at (or just before) the cursor. */
static int match_bracket(buf_t *b, int *at) {
    static const char open_[] = "([{", close_[] = ")]}";
    for (int k = 0; k < 2; k++) {
        int p = b->cur - k;
        if (p < 0 || p >= b->len) continue;
        char c = b->t[p];
        const char *o = strchr(open_, c), *cl = strchr(close_, c);
        if (!c || (!o && !cl)) continue;
        char want = o ? close_[o - open_] : open_[cl - close_];
        int dir = o ? 1 : -1, depth = 0;
        for (int i = p, steps = 0; i >= 0 && i < b->len && steps < 20000; i += dir, steps++) {
            if (b->t[i] == c) depth++;
            else if (b->t[i] == want && --depth == 0) { at[0] = p; at[1] = i; return 1; }
        }
    }
    return 0;
}

/* ---- find ------------------------------------------------------------------------------------------------ */
static int smart_eq(char a, char b, int exact) { if (exact) return a == b; return (a | 32) == (b | 32) && ((a | 32) >= 'a' && (a | 32) <= 'z') ? 1 : a == b; }
static int find_exact(void) { for (int i = 0; i < find_len; i++) if (find_q[i] >= 'A' && find_q[i] <= 'Z') return 1; return 0; }
static int match_at(buf_t *b, int p) {
    if (p + find_len > b->len) return 0;
    int ex = find_exact();
    for (int i = 0; i < find_len; i++) if (!smart_eq(b->t[p + i], find_q[i], ex)) return 0;
    return 1;
}
static void count_hits(buf_t *b) {
    find_hits = find_idx = 0;
    if (!find_len) return;
    for (int p = 0; p + find_len <= b->len; p++)
        if (match_at(b, p)) { find_hits++; if (p < sel_lo(b)) find_idx = find_hits; }
    if (has_sel(b) && match_at(b, sel_lo(b))) find_idx++;
}
static int find_next(buf_t *b, int dir, int from) {
    if (!find_len) return 0;
    for (int k = 0; k < b->len + 1; k++) {
        int p = dir > 0 ? (from + k) % (b->len + 1) : ((from - k) % (b->len + 1) + b->len + 1) % (b->len + 1);
        if (match_at(b, p)) { b->anc = p; b->cur = p + find_len; follow(b); count_hits(b); return 1; }
    }
    count_hits(b);
    return 0;
}

/* ---- prompts -------------------------------------------------------------------------------------------- */
static void ask(int kind, const char *label, const char *init) {
    prompt_kind = kind;
    snprintf(prompt_label, sizeof prompt_label, "%s", label);
    snprintf(prompt_text, sizeof prompt_text, "%s", init ? init : "");
    prompt_len = (int)strlen(prompt_text);
    focus = F_PROMPT;
}

static const char *selected_path(char *out) {
    if (fsel == 0) return 0;
    snprintf(out, FS_PATH, "%s/%s", strcmp(dir, "/") ? dir : "", files[fsel - 1].name);
    return out;
}

static void prompt_done(int ok) {
    int kind = prompt_kind;
    prompt_kind = PR_NONE;
    focus = F_EDIT;
    buf_t *b = B();
    char path[FS_PATH], sp[FS_PATH];
    if (!ok) {
        if (kind == PR_CLOSE && b && (prompt_text[0] == 'n' || prompt_text[0] == 'N')) { close_buf(cb); return; }
        if (kind == PR_NEWFILE || kind == PR_NEWDIR || kind == PR_RENAME || kind == PR_DELETE) focus = F_FILES;
        return;
    }
    switch (kind) {
    case PR_NEWFILE: case PR_SAVEAS: {
        if (!prompt_len) return;
        if (fs_join(dir, prompt_text, path, sizeof path)) return;
        if (!card_ok) { say("No SD card: %s", card_why); return; }
        fs_ent_t st;
        if (kind == PR_NEWFILE) {
            if (fs_stat(path, &st) == 0) { if (!st.dir) open_path(path); else say("%s is a folder", prompt_text); return; }
            if (!strchr(fs_basename(path), '.')) strncat(path, ".py", sizeof path - strlen(path) - 1);
            const char *start = lang_of(path) == LANG_PY ? "" : lang_of(path) == LANG_C ?
                "#include <stdio.h>\n\nint main(void) {\n    \n    return 0;\n}\n" : lang_of(path) == LANG_CPP ?
                "#include <iostream>\nusing namespace std;\n\nint main() {\n    \n    return 0;\n}\n" : "";
            if (fs_write(path, start, (uint32_t)strlen(start))) { say("Couldn't make it: %s", fs_why()); return; }
            list_files();
            if (open_path(path) == 0 && (b = B()) && b->len) {
                /* the cursor inside main */
                char *q = strstr(b->t, "    \n");
                if (q) b->cur = b->anc = (int)(q - b->t) + 4;
            }
        } else if (b) {
            snprintf(b->path, sizeof b->path, "%s", path);
            b->lang = lang_of(path); b->lines_ok = 0;
            save(b);
            remember_open();
        }
        return;
    }
    case PR_NEWDIR:
        if (!prompt_len || fs_join(dir, prompt_text, path, sizeof path)) return;
        if (fs_mkdir(path)) say("Couldn't make it: %s", fs_why());
        list_files();
        focus = F_FILES;
        return;
    case PR_RENAME:
        if (!selected_path(sp) || !prompt_len) return;
        if (fs_join(dir, prompt_text, path, sizeof path)) return;
        if (fs_rename(sp, path)) { say("Couldn't rename it: %s", fs_why()); focus = F_FILES; return; }
        { int k = find_buf(sp); if (k >= 0) { snprintf(bufs[k]->path, FS_PATH, "%s", path); bufs[k]->lang = lang_of(path); bufs[k]->lines_ok = 0; } }
        list_files();
        focus = F_FILES;
        return;
    case PR_DELETE:
        if (!selected_path(sp)) return;
        if (fs_remove(sp)) say("Couldn't remove it: %s", fs_why());
        else { int k = find_buf(sp); if (k >= 0) close_buf(k); say("Removed %s", fs_basename(sp)); }
        list_files();
        focus = F_FILES;
        return;
    case PR_GOTO: {
        if (!b) return;
        int l = atoi(prompt_text);
        lines(b);
        if (l < 1) l = 1;
        if (l > b->nl) l = b->nl;
        b->cur = b->anc = line_start(b, l - 1);
        b->top = l - 1 - view_rows / 2; if (b->top < 0) b->top = 0;
        follow(b);
        return;
    }
    case PR_CLOSE:
        if (!b) return;
        if (prompt_text[0] == 'y' || prompt_text[0] == 'Y' || !prompt_len) { if (save(b) == 0) close_buf(cb); }
        else if (prompt_text[0] == 'n' || prompt_text[0] == 'N') close_buf(cb);
        return;
    }
}

static int line_edit(char *s, int *n, int cap, const input_ev_t *e) {
    if (e->key == KEY_CHAR && e->ch >= 32 && !(e->mods & (MOD_CTRL | MOD_SUPER))) { if (*n < cap - 1) { s[(*n)++] = (char)e->ch; s[*n] = 0; } return 1; }
    if (e->key == KEY_BACKSPACE) { if (*n) s[--(*n)] = 0; return 1; }
    if (e->key == KEY_CHAR && (e->mods & (MOD_CTRL | MOD_SUPER)) && (e->ch | 32) == 'v') {
        int cn; const char *c = clip_get(&cn);
        for (int i = 0; i < cn && *n < cap - 1 && c[i] != '\n'; i++) s[(*n)++] = c[i];
        s[*n] = 0;
        return 1;
    }
    return 0;
}

/* ---- keys ---------------------------------------------------------------------------------------------- */
static int is_cmd(const input_ev_t *e) { return (e->mods & (MOD_CTRL | MOD_SUPER)) != 0; }

static void move_to(buf_t *b, int pos, int extend) {
    if (pos < 0) pos = 0;
    if (pos > b->len) pos = b->len;
    b->cur = pos;
    if (!extend) b->anc = pos;
}

static void files_key(const input_ev_t *e) {
    int n = nfiles + 1;                    /* row 0 is ".." */
    char p[FS_PATH];
    switch (e->key) {
    case KEY_UP: if (fsel > 0) fsel--; break;
    case KEY_DOWN: if (fsel < n - 1) fsel++; break;
    case KEY_PGUP: fsel = fsel > 10 ? fsel - 10 : 0; break;
    case KEY_PGDN: fsel = fsel + 10 < n ? fsel + 10 : n - 1; break;
    case KEY_HOME: fsel = 0; break;
    case KEY_END: fsel = n - 1; break;
    case KEY_TAB: focus = F_EDIT; break;
    case KEY_BACKSPACE: goto up;
    case KEY_ENTER:
        if (fsel == 0) {
        up:
            if (!strcmp(dir, "/")) break;
            {
                char *s = strrchr(dir, '/');
                char was[FS_NAME]; snprintf(was, sizeof was, "%s", s + 1);
                if (s == dir) dir[1] = 0; else *s = 0;
                list_files();
                fsel = 0;
                for (int i = 0; i < nfiles; i++) if (!strcasecmp(files[i].name, was)) fsel = i + 1;
            }
            break;
        }
        selected_path(p);
        if (files[fsel - 1].dir) { snprintf(dir, sizeof dir, "%s", p); list_files(); fsel = 0; ftop = 0; }
        else if (open_path(p)) say("Couldn't open it: %s", fs_why());
        break;
    case KEY_DELETE:
        if (fsel > 0) { char q[200]; snprintf(q, sizeof q, "Remove %s%s? Return removes it, Esc keeps it", files[fsel - 1].name, files[fsel - 1].dir ? " and all in it" : ""); ask(PR_DELETE, q, ""); }
        break;
    case KEY_F2:
        if (fsel > 0) ask(PR_RENAME, "Rename to", files[fsel - 1].name);
        break;
    case KEY_CHAR:
        if (is_cmd(e)) break;
        if (e->ch == 'n') ask(PR_NEWFILE, "New file (name.py, name.c or name.cpp)", "");
        else if (e->ch == 'N' || e->ch == 'f') ask(PR_NEWDIR, "New folder", "");
        else if (e->ch == 'r' && fsel > 0) ask(PR_RENAME, "Rename to", files[fsel - 1].name);
        else if (e->ch == 'd' && fsel > 0) { char q[200]; snprintf(q, sizeof q, "Remove %s? Return removes it, Esc keeps it", files[fsel - 1].name); ask(PR_DELETE, q, ""); }
        break;
    }
}

static void edit_key(buf_t *b, const input_ev_t *e) {
    int shift = (e->mods & MOD_SHIFT) != 0, cmd = is_cmd(e), alt = (e->mods & MOD_ALT) != 0;
    lines(b);
    int l = line_of(b, b->cur);
    switch (e->key) {
    case KEY_CHAR: {
        int ch = e->ch;
        if (cmd) {
            switch (ch | 32) {
            case 'a': b->anc = 0; b->cur = b->len; return;
            case 'c': copy_sel(b, 0); return;
            case 'x': copy_sel(b, 1); break;
            case 'v': paste(b); break;
            case 'z': if (shift || ch == 'Z') redo(b); else undo(b); break;
            case 'y': redo(b); break;
            case 'd': dup_line(b); break;
            case 'k': if (shift || ch == 'K') delete_line(b); break;
            case 'g': ask(PR_GOTO, "Go to line", ""); return;
            case '/': toggle_comment(b); break;
            default:
                if (ch == '/' || ch == '?' || ch == '7') toggle_comment(b);
                return;
            }
            break;
        }
        if (ch < 32) return;
        char c = (char)ch;
        /* a closing brace on a blank line comes out one level */
        if ((c == '}' || c == ']' || c == ')') && (b->lang == LANG_C || b->lang == LANG_CPP) && !has_sel(b)) {
            int s = line_start(b, l), blank = 1;
            for (int i = s; i < b->cur; i++) if (b->t[i] != ' ') blank = 0;
            if (blank && b->cur - s >= 4 && c == '}') { begin(b, E_OTHER, 0); replace(b, b->cur - 4, 4, "}", 1, 1, b->cur - 3); b->group++; break; }
        }
        insert_text(b, &c, 1, E_TYPE);
        break;
    }
    case KEY_ENTER: newline(b); break;
    case KEY_TAB: {
        int l0 = line_of(b, sel_lo(b)), l1 = line_of(b, sel_hi(b));
        if (shift) { shift_lines(b, l0, l1, -1); break; }
        if (has_sel(b) && l1 > l0) { if (sel_hi(b) == line_start(b, l1)) l1--; shift_lines(b, l0, l1, 1); break; }
        int c = vcol(b, b->cur), k = 4 - c % 4;
        insert_text(b, "    ", k, E_OTHER);
        break;
    }
    case KEY_BACKSPACE:
        if (has_sel(b)) { erase(b, sel_lo(b), sel_hi(b)); break; }
        if (cmd || alt) { int p = b->cur; while (p > 0 && !idch((unsigned char)b->t[p - 1]) && b->t[p - 1] != '\n') p--; while (p > 0 && idch((unsigned char)b->t[p - 1])) p--; if (p == b->cur && p > 0) p--; erase(b, p, b->cur); break; }
        {
            /* in the indentation: back to the last multiple of four */
            int s = line_start(b, l), blank = 1;
            for (int i = s; i < b->cur; i++) if (b->t[i] != ' ') blank = 0;
            int c = b->cur - s;
            if (blank && c > 0) { int k = c % 4 ? c % 4 : 4; erase(b, b->cur - k, b->cur); }
            else erase(b, b->cur - 1, b->cur);
        }
        break;
    case KEY_DELETE:
        if (has_sel(b)) erase(b, sel_lo(b), sel_hi(b));
        else erase(b, b->cur, b->cur + 1);
        break;
    case KEY_LEFT:
        if (has_sel(b) && !shift) { move_to(b, sel_lo(b), 0); break; }
        if (cmd || alt) { int p = b->cur; while (p > 0 && !idch((unsigned char)b->t[p - 1])) p--; while (p > 0 && idch((unsigned char)b->t[p - 1])) p--; move_to(b, p, shift); }
        else move_to(b, b->cur - 1, shift);
        b->goal = -1;
        break;
    case KEY_RIGHT:
        if (has_sel(b) && !shift) { move_to(b, sel_hi(b), 0); break; }
        if (cmd || alt) { int p = b->cur; while (p < b->len && !idch((unsigned char)b->t[p])) p++; while (p < b->len && idch((unsigned char)b->t[p])) p++; move_to(b, p, shift); }
        else move_to(b, b->cur + 1, shift);
        b->goal = -1;
        break;
    case KEY_UP: case KEY_DOWN: {
        int dir = e->key == KEY_UP ? -1 : 1;
        if (alt) { move_line(b, dir); break; }
        if (cmd) { b->top += dir; if (b->top < 0) b->top = 0; if (b->top >= b->nl) b->top = b->nl - 1; return; }
        if (b->goal < 0) b->goal = vcol(b, b->cur);
        int nl2 = l + dir;
        if (nl2 < 0) { move_to(b, 0, shift); break; }
        if (nl2 >= b->nl) { move_to(b, b->len, shift); break; }
        int g = b->goal;
        move_to(b, pos_at_col(b, nl2, g), shift);
        b->goal = g;
        break;
    }
    case KEY_PGUP: case KEY_PGDN: {
        int dir = e->key == KEY_PGUP ? -1 : 1;
        if (b->goal < 0) b->goal = vcol(b, b->cur);
        int nl2 = l + dir * (view_rows - 2);
        if (nl2 < 0) nl2 = 0;
        if (nl2 >= b->nl) nl2 = b->nl - 1;
        int g = b->goal;
        move_to(b, pos_at_col(b, nl2, g), shift);
        b->goal = g;
        b->top += dir * (view_rows - 2);
        if (b->top > b->nl - view_rows) b->top = b->nl - view_rows;
        if (b->top < 0) b->top = 0;
        break;
    }
    case KEY_HOME:
        if (cmd) { move_to(b, 0, shift); break; }
        {
            /* the first non-blank, then the line's start */
            int s = line_start(b, l), k = s;
            while (k < line_end(b, l) && (b->t[k] == ' ' || b->t[k] == '\t')) k++;
            move_to(b, b->cur == k ? s : k, shift);
        }
        b->goal = -1;
        break;
    case KEY_END:
        if (cmd) { move_to(b, b->len, shift); break; }
        move_to(b, line_end(b, l), shift);
        b->goal = -1;
        break;
    default: return;
    }
    follow(b);
}

static void c_key(const input_ev_t *e) {
    if (!started) return;
    buf_t *b = B();
    blink0 = now_ms();
    int cmd = is_cmd(e), shift = (e->mods & MOD_SHIFT) != 0;
    /* everywhere in the app */
    if (e->key == KEY_F5 || (cmd && e->key == KEY_CHAR && (e->ch | 32) == 'r')) {
        if (shift || running()) { if (running()) { run_stop(); say("Stopping"); } }
        else run_current();
        return;
    }
    if (focus == F_PROMPT) {
        if (e->key == KEY_ENTER) prompt_done(1);
        else if (prompt_kind == PR_CLOSE && e->key == KEY_CHAR && (e->ch == 'n' || e->ch == 'N')) { prompt_text[0] = 'n'; prompt_done(0); }
        else if (prompt_kind == PR_CLOSE && e->key == KEY_CHAR && (e->ch == 'y' || e->ch == 'Y')) { prompt_text[0] = 'y'; prompt_len = 1; prompt_done(1); }
        else if (prompt_kind != PR_DELETE && prompt_kind != PR_CLOSE) line_edit(prompt_text, &prompt_len, (int)sizeof prompt_text, e);
        return;
    }
    if (focus == F_INPUT) {
        if (e->key == KEY_ENTER) { inbuf[in_len] = 0; run_input(inbuf); in_len = 0; return; }
        if (line_edit(inbuf, &in_len, (int)sizeof inbuf, e)) return;
        if (!cmd) return;
    }
    if (cmd && e->key == KEY_CHAR) {
        switch (e->ch | 32) {
        case 's':
            if (!b) return;
            if (shift || e->ch == 'S') ask(PR_SAVEAS, "Save as", fs_basename(b->path));
            else save(b);
            return;
        case 'n':
            if (shift || e->ch == 'N') ask(PR_NEWDIR, "New folder", "");
            else ask(PR_NEWFILE, "New file (name.py, name.c or name.cpp)", "");
            return;
        case 'o': case 'p': show_files = 1; focus = F_FILES; list_files(); return;
        case 'b': show_files = !show_files; if (!show_files && focus == F_FILES) focus = F_EDIT; return;
        case 'j': out_open = !out_open; return;
        case 'w':
            if (!b) return;
            if (b->dirty) { char q[200]; snprintf(q, sizeof q, "Save %s first? Y saves, N throws the changes away, Esc keeps it open", fs_basename(b->path)); ask(PR_CLOSE, q, ""); }
            else close_buf(cb);
            return;
        case 'f': case 'h':
            if (!b) return;
            if (has_sel(b) && sel_hi(b) - sel_lo(b) < (int)sizeof find_q && !memchr(b->t + sel_lo(b), '\n', (size_t)(sel_hi(b) - sel_lo(b)))) {
                find_len = sel_hi(b) - sel_lo(b); memcpy(find_q, b->t + sel_lo(b), (size_t)find_len); find_q[find_len] = 0;
            }
            focus = F_FIND; find_field = (e->ch | 32) == 'h' ? 1 : 0;
            count_hits(b);
            return;
        case 'l': if (focus == F_INPUT || out_open) { out_clear(); return; } break;
        }
    }
    if (e->key == KEY_CHAR && cmd && e->ch == '\t') { if (nbufs) { cb = (cb + 1) % nbufs; remember_open(); } return; }
    if (cmd && (e->key == KEY_PGDN || e->key == KEY_PGUP)) { if (nbufs) { cb = (cb + (e->key == KEY_PGDN ? 1 : nbufs - 1)) % nbufs; remember_open(); } return; }
    if (e->key == KEY_F3 && b) { find_next(b, shift ? -1 : 1, shift ? sel_lo(b) - 1 : sel_hi(b)); return; }
    if (e->key == KEY_F2 && focus == F_FILES) { files_key(e); return; }
    if (focus == F_FIND && b) {
        if (e->key == KEY_TAB) { find_field = !find_field; return; }
        if (e->key == KEY_ENTER) {
            if (find_field == 1 && has_sel(b) && match_at(b, sel_lo(b)) && sel_hi(b) - sel_lo(b) == find_len) {
                if (cmd) {
                    /* every one of them */
                    int n = 0;
                    begin(b, E_OTHER, 0);
                    for (int p = 0; p + find_len <= b->len; ) {
                        if (match_at(b, p)) { replace(b, p, find_len, repl_q, repl_len, 1, p + repl_len); p += repl_len; n++; }
                        else p++;
                    }
                    b->group++;
                    say("Replaced %d", n);
                    count_hits(b);
                    return;
                }
                begin(b, E_OTHER, 0);
                int lo = sel_lo(b);
                replace(b, lo, find_len, repl_q, repl_len, 1, lo + repl_len);
                b->group++;
            }
            find_next(b, shift ? -1 : 1, shift ? sel_lo(b) - 1 : sel_hi(b));
            return;
        }
        if (find_field == 0) {
            if (line_edit(find_q, &find_len, (int)sizeof find_q, e)) { count_hits(b); if (find_len) find_next(b, 1, sel_lo(b)); return; }
        } else if (line_edit(repl_q, &repl_len, (int)sizeof repl_q, e)) return;
        if (e->key == KEY_UP || e->key == KEY_DOWN) { find_next(b, e->key == KEY_UP ? -1 : 1, e->key == KEY_UP ? sel_lo(b) - 1 : sel_hi(b)); return; }
        return;
    }
    if (focus == F_FILES) { files_key(e); return; }
    if (b) edit_key(b, e);
}

/* Esc: closes what's open, stops a program, comes back from the files; else Home. */
static int c_esc(void) {
    if (focus == F_PROMPT) { prompt_done(0); return 1; }
    if (focus == F_FIND) { focus = F_EDIT; return 1; }
    if (running()) { run_stop(); say("Stopping"); return 1; }
    if (focus == F_FILES) { focus = F_EDIT; return 1; }
    buf_t *b = B();
    if (b && has_sel(b)) { b->anc = b->cur; return 1; }
    return 0;
}

/* ---- the mouse ----------------------------------------------------------------------------------------- */
static int mouse_down, drag_sel;
static uint64_t last_click; static int last_cx, last_cy, clicks;

static int pos_at_xy(buf_t *b, int x, int y) {
    lines(b);
    int row = (y - ed_y) / lh + b->top;
    if (row < 0) row = 0;
    if (row >= b->nl) return b->len;
    int col = ((x - ed_x - gut_w) * 100 + adv100 / 2) / adv100 + b->left;
    if (col < 0) col = 0;
    return pos_at_col(b, row, col);
}

static void c_pointer(const input_ev_t *e, int ax, int ay, int aw, int ah) {
    (void)ax; (void)ay; (void)aw; (void)ah;
    if (!started) return;
    buf_t *b = B();
    int press = (e->buttons & 1) && !mouse_down;
    int x = e->x, y = e->y;
    if (e->wheel) {
        if (y >= out_y && out_open) { oscroll += e->wheel * 3; if (oscroll < 0) oscroll = 0; if (oscroll > ocount) oscroll = ocount; }
        else if (show_files && x < sb_x + sb_w) { ftop -= e->wheel * 3; if (ftop < 0) ftop = 0; }
        else if (b) { lines(b); b->top -= e->wheel * 3; if (b->top > b->nl - 1) b->top = b->nl - 1; if (b->top < 0) b->top = 0; }
        return;
    }
    if (!(e->buttons & 1)) { mouse_down = 0; drag_sel = 0; return; }
    if (!press) {
        if (drag_sel && b) { b->cur = pos_at_xy(b, x, y); follow(b); }
        return;
    }
    mouse_down = 1;
    uint64_t t = now_ms();
    clicks = (t - last_click < 400 && abs(x - last_cx) < 6 && abs(y - last_cy) < 6) ? clicks + 1 : 1;
    last_click = t; last_cx = x; last_cy = y;
    blink0 = t;
    /* the run button, the tabs */
    if (y >= tab_y && y < tab_y + tab_h) {
        if (x >= run_bx && x < run_bx + run_bw) { if (running()) run_stop(); else run_current(); return; }
        int tx = ed_x;
        for (int i = 0; i < nbufs; i++) {
            int w = font_width(FONT_MEDIUM, S(12), fs_basename(bufs[i]->path), -1) + S(34);
            if (x >= tx && x < tx + w) {
                if (x >= tx + w - S(18) && i == cb && !bufs[i]->dirty) close_buf(i);
                else { cb = i; remember_open(); }
                focus = F_EDIT;
                return;
            }
            tx += w;
        }
        return;
    }
    if (show_files && x >= sb_x && x < sb_x + sb_w && y >= sb_y) {
        int row = (y - sb_y - S(34)) / S(24) + ftop;
        if (row >= 0 && row <= nfiles) {
            fsel = row;
            focus = F_FILES;
            if (clicks >= 2 || (row > 0 && !files[row - 1].dir) || row == 0) { input_ev_t k = { .type = EV_KEY, .key = KEY_ENTER }; files_key(&k); }
        } else focus = F_FILES;
        return;
    }
    if (out_open && y >= out_y) { if (run_state() == RUN_INPUT) focus = F_INPUT; return; }
    if (!b || y < ed_y) return;
    focus = F_EDIT;
    int p = pos_at_xy(b, x, y);
    if (clicks == 2) {
        int s = p, en = p;
        while (s > 0 && idch((unsigned char)b->t[s - 1])) s--;
        while (en < b->len && idch((unsigned char)b->t[en])) en++;
        b->anc = s; b->cur = en;
    } else if (clicks >= 3) {
        int l = line_of(b, p);
        b->anc = line_start(b, l); b->cur = l + 1 < b->nl ? line_start(b, l + 1) : b->len;
    } else {
        b->cur = p;
        if (!(e->mods & MOD_SHIFT)) b->anc = p;
        drag_sel = 1;
    }
    b->goal = -1;
}

/* ---- drawing ---------------------------------------------------------------------------------------------- */
typedef struct { buf_t *b; int y, base, x0; int lo, hi; } draw_ctx;

static void draw_tok(int start, int len, int kind, void *vctx) {
    draw_ctx *c = vctx;
    buf_t *b = c->b;
    /* its columns, tabs worked out */
    int ls = line_start(b, line_of(b, start)), col = 0;
    for (int i = ls; i < start; i++) col = b->t[i] == '\t' ? (col / 4 + 1) * 4 : col + 1;
    uint32_t color = kind_color(kind);
    int i = start, end = start + len;
    while (i < end) {
        if (b->t[i] == '\t') { col = (col / 4 + 1) * 4; i++; continue; }
        int j = i;
        while (j < end && b->t[j] != '\t') j++;
        int vis0 = col - b->left, n = j - i, skip = 0;
        if (vis0 < 0) { skip = -vis0 < n ? -vis0 : n; vis0 = 0; }
        if (vis0 < view_cols + 1 && n - skip > 0) {
            int take = n - skip;
            if (vis0 + take > view_cols + 1) take = view_cols + 1 - vis0;
            if (take > 0) font_draw(FONT_MONO, px, c->x0 + colx(vis0), c->base, b->t + i + skip, take, color, 255);
        }
        col += n; i = j;
    }
}

static void draw_files(int x, int y, int w, int h) {
    sb_x = x; sb_y = y; sb_w = w;
    gfx_fill(x, y, w, h, th.surface);
    gfx_fill(x + w - 1, y, 1, h, th.line);
    int pad = S(12);
    const char *dn = !strcmp(dir, "/") ? "SD card" : fs_basename(dir);
    text_fit_l(x + pad, y + S(17), dn, w - 2 * pad, FONT_SEMIBOLD, S(12), focus == F_FILES ? th.text : th.sub);
    gfx_fill(x, y + S(33), w - 1, 1, th.line);
    int rh = S(24), ry = y + S(34);
    int rows = (h - S(34) - S(24)) / rh;
    if (rows < 1) rows = 1;
    if (fsel < ftop) ftop = fsel;
    if (fsel >= ftop + rows) ftop = fsel - rows + 1;
    if (!card_ok) {
        text_wrap(x + pad, ry + S(10), w - 2 * pad, card_why[0] ? card_why : "No SD card", FONT_REGULAR, S(12), S(17), 6, th.faint);
        return;
    }
    buf_t *b = B();
    for (int r = 0; r < rows && ftop + r <= nfiles; r++) {
        int i = ftop + r;
        int yy = ry + r * rh;
        int on = i == fsel && focus == F_FILES;
        if (on) gfx_fill(x, yy, w - 1, rh, th.sel);
        if (on) gfx_fill(x, yy, S(2), rh, th.accent);
        if (i == 0) {
            ui_glyph(G_FOLDER, x + pad + S(7), yy + rh / 2, S(15), th.faint);
            text_l(x + pad + S(20), yy + rh / 2, "..", FONT_MEDIUM, S(12), th.sub);
            continue;
        }
        fs_ent_t *f = &files[i - 1];
        char p[FS_PATH]; snprintf(p, sizeof p, "%s/%s", strcmp(dir, "/") ? dir : "", f->name);
        int open_ = b && !strcasecmp(b->path, p);
        ui_glyph(f->dir ? G_FOLDER : G_FILE, x + pad + S(7), yy + rh / 2, S(15), f->dir ? th.sub : open_ ? th.accent : th.faint);
        text_fit_l(x + pad + S(20), yy + rh / 2, f->name, w - pad * 2 - S(22), open_ ? FONT_SEMIBOLD : FONT_REGULAR, S(12),
                   open_ ? th.text : th.sub);
    }
    const char *hint = focus == F_FILES ? "n new  f folder  r rename  Del remove" : "Ctrl+O files   Ctrl+B hide";
    text_fit_l(x + pad, y + h - S(12), hint, w - 2 * pad, FONT_REGULAR, S(10), th.faint);
}

static void draw_bar_input(int x, int y, int w, const char *label, const char *text, int tlen, int active) {
    int hgt = S(34);
    gfx_fill(x, y, w, hgt, th.surface);
    gfx_fill(x, y + hgt - 1, w, 1, th.line);
    int lw = text_l(x + S(12), y + hgt / 2, label, FONT_MEDIUM, S(12), th.sub);
    int fx = x + S(12) + lw + S(10);
    font_draw(FONT_MONO, px, fx, y + hgt / 2 + font_ascent(FONT_MONO, px) / 2 - S(1), text, tlen, th.text, 255);
    if (active && ((now_ms() - blink0) / 530) % 2 == 0) gfx_fill(fx + font_width(FONT_MONO, px, text, tlen), y + S(8), S(2), hgt - S(16), th.accent);
}

static void draw_output(int x, int y, int w, int h) {
    out_y = y;
    gfx_fill(x, y, w, h, th.dark ? gfx_shade(th.bg, 88) : th.surface);
    gfx_fill(x, y, w, 1, th.line);
    int hh = S(26), pad = S(12);
    int st = run_state();
    const char *label = "Output";
    text_l(x + pad, y + hh / 2, label, FONT_SEMIBOLD, S(12), th.sub);
    char m[96];
    uint32_t mc = th.faint;
    if (running()) {
        uint64_t ms = run_elapsed_ms();
        snprintf(m, sizeof m, "%s " TXT_BULLET " %lu.%lu s", st == RUN_INPUT ? "waiting for input" : "running", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 100));
        mc = th.accent;
    } else if (st == RUN_DONE) {
        snprintf(m, sizeof m, "%s", run_ok() ? run_message() : run_error_line() ? "stopped with an error" : run_message());
        if (strlen(m) > 60) snprintf(m, sizeof m, "stopped with an error");
        mc = run_ok() ? 0x4CC27A : 0xE5534B;
    } else m[0] = 0;
    text_l(x + pad + font_width(FONT_SEMIBOLD, S(12), label, -1) + S(12), y + hh / 2, m, FONT_REGULAR, S(12), mc);
    text_r(x + w - pad, y + hh / 2, running() ? "Esc stop   Ctrl+J hide" : "F5 run   Ctrl+L clear   Ctrl+J hide", FONT_REGULAR, S(10), th.faint);
    /* the text, wrapped, newest at the bottom */
    int top = y + hh, bottom = y + h - S(6);
    int cols = (w - 2 * pad) * 100 / adv100;
    if (cols < 10) cols = 10;
    uint32_t colr[4] = { th.text, 0xE5534B, th.faint, 0x4CC27A };
    int yy = bottom;
    int skip = oscroll;
    int input_here = st == RUN_INPUT;
    gfx_clip_push(x, top, w, bottom - top);
    for (int k = ocount - 1; k >= -1 && yy > top; k--) {
        oline_t *o;
        oline_t fake = { 0, 0, 0, 0 };
        if (k == -1) break;
        o = &olines[(ohead + k) % OUT_LINES];
        int n = o->n;
        int is_last = k == ocount - 1;
        /* the line input() is typed into: the prompt, then what's typed */
        char tmp[4600];
        const char *s = o->s ? o->s : "";
        if (is_last && input_here && out_partial) {
            int c = n < 4000 ? n : 4000;
            memcpy(tmp, s, (size_t)c);
            memcpy(tmp + c, inbuf, (size_t)in_len);
            n = c + in_len; s = tmp;
        }
        (void)fake;
        int rows = n ? (n + cols - 1) / cols : 1;
        for (int r = rows - 1; r >= 0 && yy > top; r--) {
            if (skip) { skip--; continue; }
            yy -= lh;
            int a = r * cols, len = n - a < cols ? n - a : cols;
            if (len > 0) font_draw(FONT_MONO, px, x + pad, yy + font_ascent(FONT_MONO, px) + S(2), s + a, len, colr[o->col & 3], 255);
            if (is_last && input_here && out_partial && r == rows - 1 && ((now_ms() - blink0) / 530) % 2 == 0 && focus == F_INPUT)
                gfx_fill(x + pad + colx(len > 0 ? len : 0), yy + S(3), S(2), lh - S(4), th.accent);
        }
    }
    if (input_here && !out_partial) {
        /* input() with no prompt: a line of its own */
        int iy = bottom - lh;
        font_draw(FONT_MONO, px, x + pad, iy + font_ascent(FONT_MONO, px) + S(2), inbuf, in_len, th.text, 255);
        if (((now_ms() - blink0) / 530) % 2 == 0) gfx_fill(x + pad + colx(in_len), iy + S(3), S(2), lh - S(4), th.accent);
    }
    gfx_clip_pop();
    if (!ocount && !input_here) text_l(x + pad, top + S(16), "Press F5 to run the file. What it prints shows here.", FONT_REGULAR, S(12), th.faint);
}

static void c_draw(int x, int y, int w, int h) {
    if (!started) return;
    px = S(14); lh = S(19);
    static int adv_px;
    if (adv_px != px) {
        char hundred[101]; memset(hundred, 'M', 100); hundred[100] = 0;
        adv100 = font_width(FONT_MONO, px, hundred, 100);
        adv_px = px;
    }
    cw = (adv100 + 99) / 100;
    if (cw < 1) cw = 8;
    buf_t *b = B();
    int fw = show_files ? S(186) : 0;
    if (show_files) draw_files(x, y, fw, h);
    else sb_w = 0;
    int ex = x + fw, ew = w - fw;
    /* the tabs, and on the right the run button and where the cursor is */
    tab_y = y; tab_h = S(30);
    gfx_fill(ex, y, ew, tab_h, th.bar);
    gfx_fill(ex, y + tab_h - 1, ew, 1, th.line);
    int tx = ex;
    for (int i = 0; i < nbufs; i++) {
        const char *nm = fs_basename(bufs[i]->path);
        int tw = font_width(FONT_MEDIUM, S(12), nm, -1) + S(34);
        if (tx + tw > ex + ew - S(260)) break;
        int on = i == cb;
        if (on) { gfx_fill(tx, y, tw, tab_h - 1, th.bg); gfx_fill(tx, y, tw, S(2), th.accent); }
        gfx_fill(tx + tw - 1, y + S(6), 1, tab_h - S(12), th.line);
        text_l(tx + S(12), y + tab_h / 2, nm, FONT_MEDIUM, S(12), on ? th.text : th.sub);
        if (bufs[i]->dirty) gfx_circle((float)(tx + tw - S(13)), (float)(y + tab_h / 2), (float)S(3), on ? th.accent : th.sub, 255);
        else if (on) ui_glyph(G_CLOSE, tx + tw - S(13), y + tab_h / 2, S(14), th.faint);
        tx += tw;
    }
    /* run / stop */
    int run_on = running();
    run_bw = S(64);
    run_bx = ex + ew - run_bw - S(8);
    int by = y + S(5), bh = tab_h - S(10);
    gfx_rrect(run_bx, by, run_bw, bh, S(4), run_on ? 0xE5534B : th.accent, 255);
    ui_glyph(run_on ? G_STOP : G_PLAY, run_bx + S(15), by + bh / 2, S(14), 0xFFFFFF);
    text_l(run_bx + S(26), by + bh / 2, run_on ? "Stop" : "Run", FONT_SEMIBOLD, S(12), 0xFFFFFF);
    if (b) {
        lines(b);
        char info[96];
        int l = line_of(b, b->cur);
        if (status_msg[0] && now_ms() - status_at < 2500) snprintf(info, sizeof info, "%s", status_msg);
        else snprintf(info, sizeof info, "%s   Ln %d, Col %d%s", lang_name(b->lang), l + 1, vcol(b, b->cur) + 1, b->dirty ? "   " TXT_BULLET " unsaved" : "");
        text_r(run_bx - S(12), y + tab_h / 2, info, FONT_REGULAR, S(11), th.sub);
    }
    int ty = y + tab_h;
    int body_h = h - tab_h;
    int oh = out_open ? body_h * 38 / 100 : 0;
    if (oh && oh < S(110)) oh = S(110);
    int bar_h = (focus == F_PROMPT || focus == F_FIND) ? S(34) : 0;
    if (focus == F_PROMPT) draw_bar_input(ex, ty, ew, prompt_label, prompt_text, prompt_len, 1);
    if (focus == F_FIND) {
        char lab[64];
        if (find_len) snprintf(lab, sizeof lab, "Find (%d of %d)", find_idx, find_hits); else snprintf(lab, sizeof lab, "Find");
        int half = ew / 2;
        draw_bar_input(ex, ty, half, lab, find_q, find_len, find_field == 0);
        draw_bar_input(ex + half, ty, ew - half, "Replace", repl_q, repl_len, find_field == 1);
        gfx_fill(ex + half, ty, 1, S(34), th.line);
    }
    ed_x = ex; ed_y = ty + bar_h; ed_h = body_h - bar_h - oh;
    if (out_open) draw_output(ex, ed_y + ed_h, ew, oh);
    else out_y = y + h;
    if (!b) {
        text_c(ex + ew / 2, ed_y + ed_h / 2, "No file open. Ctrl+N makes one; Ctrl+O shows the files.", FONT_REGULAR, S(13), th.faint);
        return;
    }
    /* the text */
    view_rows = (ed_h - S(8)) / lh;
    if (view_rows < 1) view_rows = 1;
    int digits = b->nl >= 1000 ? 4 : 3;
    gut_w = digits * cw + S(22);
    view_cols = (ew - gut_w - S(10)) * 100 / adv100;
    if (b->top > b->nl - 1) b->top = b->nl - 1;
    if (b->top < 0) b->top = 0;
    gfx_clip_push(ex, ed_y, ew, ed_h);
    int cl = line_of(b, b->cur), lo = sel_lo(b), hi = sel_hi(b);
    int br[2] = { -1, -1 };
    int have_br = !has_sel(b) && match_bracket(b, br);
    uint32_t sel_c = th.dark ? gfx_lerp(th.bg, th.accent, 70, 255) : gfx_lerp(th.bg, th.accent, 60, 255);
    for (int r = 0; r < view_rows + 1; r++) {
        int l = b->top + r;
        if (l >= b->nl) break;
        int yy = ed_y + S(4) + r * lh;
        int base = yy + (lh + font_ascent(FONT_MONO, px) - font_descent(FONT_MONO, px)) / 2;
        int s = line_start(b, l), e = line_end(b, l);
        if (l == cl && focus != F_FILES) gfx_fill(ex + gut_w - S(6), yy, ew - gut_w + S(6), lh, th.dark ? gfx_lerp(th.bg, th.text, 10, 255) : gfx_lerp(th.bg, th.text, 8, 255));
        if (b->err_line == l + 1) {
            gfx_fill(ex, yy, ew, lh, gfx_lerp(th.bg, 0xE5534B, 50, 255));
            gfx_fill(ex, yy, S(3), lh, 0xE5534B);
        }
        /* the selection on this line */
        if (lo < hi && lo <= e && hi >= s) {
            int a = lo > s ? lo : s, z = hi < e ? hi : e;
            int ca = vcol(b, a) - b->left, cz = vcol(b, z) - b->left + (hi > e ? 1 : 0);
            if (ca < 0) ca = 0;
            if (cz > ca) gfx_fill(ex + gut_w + colx(ca), yy, colx(cz) - colx(ca), lh, sel_c);
        }
        if (find_len && focus == F_FIND) {
            for (int p = s; p + find_len <= e; p++) if (match_at(b, p)) {
                int ca = vcol(b, p) - b->left;
                if (ca >= 0) gfx_rrect_stroke(ex + gut_w + colx(ca) - 1, yy + 1, colx(ca + find_len) - colx(ca) + 2, lh - 2, S(2), 1, th.accent, 255);
            }
        }
        if (have_br) for (int k = 0; k < 2; k++) if (br[k] >= s && br[k] <= e) {
            int ca = vcol(b, br[k]) - b->left;
            if (ca >= 0) gfx_rrect_stroke(ex + gut_w + colx(ca) - 1, yy + 1, colx(ca + 1) - colx(ca) + 2, lh - 2, S(2), 1, th.sub, 255);
        }
        char num[12]; snprintf(num, sizeof num, "%d", l + 1);
        uint32_t nc = l == cl ? th.sub : th.faint;
        if (b->err_line == l + 1) nc = 0xE5534B;
        font_draw(FONT_MONO, S(12), ex + gut_w - S(12) - font_width(FONT_MONO, S(12), num, -1), base, num, -1, nc, 255);
        draw_ctx dc = { b, yy, base, ex + gut_w, lo, hi };
        tokens(b, l, draw_tok, &dc);
        if (l == cl && focus == F_EDIT && ((now_ms() - blink0) / 530) % 2 == 0) {
            int cc = vcol(b, b->cur) - b->left;
            if (cc >= 0 && cc <= view_cols) gfx_fill(ex + gut_w + colx(cc) - 1, yy + S(1), S(2), lh - S(2), th.accent);
        }
    }
    gfx_fill(ex + gut_w - S(8), ed_y, 1, ed_h, th.line);
    /* where we are in the file */
    if (b->nl > view_rows) {
        int th_h = ed_h * view_rows / b->nl; if (th_h < S(16)) th_h = S(16);
        int ty2 = ed_y + (ed_h - th_h) * b->top / (b->nl - view_rows > 0 ? b->nl - view_rows : 1);
        gfx_rrect(ex + ew - S(6), ty2, S(4), th_h, S(2), th.line, 255);
    }
    gfx_clip_pop();
}

/* ---- the app's life ------------------------------------------------------------------------------------- */
static void c_open(void) {
    if (!started) { started = 1; first_open(); }
    else if (card_ok) list_files();
    blink0 = now_ms();
}

static int c_tick(void) {
    static uint64_t phase;
    int news = run_follow();
    uint64_t p = (now_ms() - blink0) / 530;
    if (p != phase) { phase = p; news = 1; }
    if (running()) {
        static uint64_t last;
        if (now_ms() - last >= 100) { last = now_ms(); news = 1; }      /* the running time */
    }
    if (status_msg[0] && now_ms() - status_at >= 2500 && now_ms() - status_at < 2600) news = 1;
    return news;
}

/* The Terminal's `edit` and `run`. */
int code_open_path(const char *path, int run) {
    shell_open(&app_code);
    fs_ent_t st;
    if (fs_stat(path, &st) != 0) {
        /* a new one */
        if (fs_write(path, "", 0)) return -1;
        list_files();
    } else if (st.dir) {
        snprintf(dir, sizeof dir, "%s", path); list_files(); show_files = 1; focus = F_FILES; fsel = 0;
        return 0;
    }
    if (open_path(path)) return -1;
    /* the list shows the file's folder */
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl && sl != dir) *sl = 0; else strcpy(dir, "/");
    list_files();
    if (run) run_current();
    return 0;
}

/* Programs keep going while the app isn't in front: their output still gathers. */
int code_background(void);
int code_background(void) {
    if (shell_current() == &app_code) return 0;
    if (!was_running) { run_poll(); return 0; }      /* someone else's run (the web page's): its files */
    run_follow();
    return 0;
}

const app_t app_code = { "Code", 0x4C8DF6, G_CODE, c_open, c_key, c_pointer, c_draw, c_tick, c_esc };
