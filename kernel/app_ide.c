/* ===========================================================================
 *  BoltOS  -  kernel/app_ide.c
 *  Code: the IDE.
 *
 *  It used to be four buffers -- one per language -- and a Run button. That is
 *  enough to demonstrate a compiler and not enough to write anything with: a
 *  program worth writing lives in several files, and there was no way to see
 *  them, to have two of them open, or to build them together.
 *
 *  So the app is now built around a *project*: a directory shown as a tree on
 *  the left, files opened into tabs, and a Run button that builds the project's
 *  current *target* -- the file list written down in `build.txt` beside the
 *  sources (include/ccbuild.h). The shell's `build` command resolves the same
 *  target through the same code, so a project built from the desktop and the
 *  same project built over SSH are the same build.
 *
 *  What was there before is kept: the editor (caret, arrow/Home/End/Del,
 *  click-to-place, scroll, syntax highlighting, line numbers, auto-indent,
 *  bracket pairing, clipboard), the output console fed through
 *  console_set_sink(), and the four language samples -- which are now four
 *  unsaved scratch tabs, so switching language is switching tab.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "console.h"
#include "boltcc.h"
#include "boltpy.h"
#include "ccbuild.h"
#include "keyboard.h"
#include "clipboard.h"
#include "pit.h"
#include "string.h"
#include "kprintf.h"
#include "fs.h"
#include "vfs.h"
#include "pe.h"
#include "kheap.h"
#include <stdio.h>

#define IDE_DIR "/home/Documents"

#define BUFCAP 16384
#define CELLW  8
#define CELLH  16
#define PAD    8
#define TOOLH  38
#define TABH   24            /* the open-file tab strip                          */
#define TREEW  176           /* project tree sidebar                             */
#define GUTTER 38            /* line-number gutter width                         */
#define OCOLS  256
#define OROWS  400
#define MAXTAB 8
#define MAXEXP 8             /* directories held open in the tree                */
#define TREE_DEPTH 4

enum { LANG_C = 0, LANG_CPP, LANG_CS, LANG_PY, NLANG };

typedef struct {
    char *buf;               /* BUFCAP bytes, allocated when the tab is opened  */
    int   len, cur, top;     /* caret index, first visible editor line          */
} editor_t;

typedef struct {
    editor_t ed;
    char path[CCB_PATH_MAX]; /* on disk; empty for an unsaved scratch buffer    */
    char name[48];           /* what the tab shows                              */
    int  lang;
    int  dirty;
    int  open;
} tab_t;

enum { PR_NONE = 0, PR_SAVE, PR_NEW, PR_PROJ };

typedef struct {
    tab_t    tab[MAXTAB];
    int      ntab, cur;
    char     proj[CCB_PATH_MAX];        /* project root shown in the tree      */
    char     target[CCB_NAME_MAX];      /* selected target; "" = the first one */
    char     expand[MAXEXP][CCB_PATH_MAX];
    int      nexp;
    /* output console */
    char     og[OROWS][OCOLS];
    int      ox, oy;         /* output write cursor                             */
    int      otop;           /* first visible output row                        */
    int      running;
    /* the one-line input strip (save-as / new file / project root) */
    int      prompt;
    char     input[CCB_PATH_MAX];
    int      inlen;
    char     status[96];     /* one-line status under the toolbar               */
} ide_t;

static ide_t S;
static ide_t *out_active;    /* grid currently capturing kputc                  */

static const char *lang_label(int l) {
    return l == LANG_C ? "C" : l == LANG_CPP ? "C++" : l == LANG_CS ? "C#" : "Python";
}
/* append a non-negative integer to s at *p (caller ensures room) */
static void put_uint(char *s, int *p, int v) {
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) s[(*p)++] = t[--n];
}
static void sset(char *dst, int cap, const char *src) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}
static void set_status(const char *s) { sset(S.status, (int)sizeof S.status, s); }

/* ------------------------------------------------------------------ tabs --- */
static tab_t *TAB(void) { return &S.tab[S.cur]; }
static editor_t *ED(void) { return &S.tab[S.cur].ed; }

/* pick the editor language that matches a filename's extension */
static int lang_for_name(const char *name) {
    int n = (int)strlen(name);
    if (n >= 4 && strcmp(name + n - 4, ".cpp") == 0) return LANG_CPP;
    if (n >= 3 && strcmp(name + n - 3, ".cc")  == 0) return LANG_CPP;
    if (n >= 4 && strcmp(name + n - 4, ".cxx") == 0) return LANG_CPP;
    if (n >= 3 && strcmp(name + n - 3, ".cs")  == 0) return LANG_CS;
    if (n >= 3 && strcmp(name + n - 3, ".py")  == 0) return LANG_PY;
    return LANG_C;
}
static const char *lang_ext(int l) {
    return l == LANG_CPP ? ".cpp" : l == LANG_CS ? ".cs" : l == LANG_PY ? ".py" : ".c";
}
static const char *base_of(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/' && p[1]) b = p + 1;
    return b;
}

/* The project a file belongs to is the directory it is in -- that is where its
 * build.txt sits, and it is the answer that stays right when the tree is rooted
 * at a workspace holding several projects. With no file open, the tree root is
 * the best guess there is. */
static void active_projdir(char *out, int cap) {
    const char *p = S.tab[S.cur].path;
    if (!S.tab[S.cur].open || !p[0]) { sset(out, cap, S.proj); return; }
    const char *b = base_of(p);
    int n = (int)(b - p);
    while (n > 1 && p[n - 1] == '/') n--;          /* drop the separator */
    if (n <= 0) { sset(out, cap, "/"); return; }
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) out[i] = p[i];
    out[n] = 0;
}

/* A tab owns its buffer for as long as the app runs; opening one is the only
 * place that allocates, and a failed allocation must not leave a live tab
 * pointing at nothing. */
static int tab_alloc(tab_t *t) {
    if (t->ed.buf) return 1;
    t->ed.buf = (char *)kmalloc(BUFCAP);
    if (!t->ed.buf) return 0;
    t->ed.buf[0] = 0;
    return 1;
}

/* Find the tab already showing `path`, or -1. */
static int tab_with_path(const char *path) {
    for (int i = 0; i < S.ntab; i++)
        if (S.tab[i].open && S.tab[i].path[0] && !strcmp(S.tab[i].path, path)) return i;
    return -1;
}

/* Claim a tab slot: a free one, else the least recently touched clean tab,
 * else the first clean tab, else refuse (never silently drop unsaved work). */
static int tab_slot(void) {
    for (int i = 0; i < MAXTAB; i++) if (!S.tab[i].open) {
        if (i >= S.ntab) S.ntab = i + 1;
        return i;
    }
    for (int i = 0; i < MAXTAB; i++) if (!S.tab[i].dirty && i != S.cur) return i;
    return -1;
}

/* ------------------------------------------------------------ editor ops --- */
static void ed_insert(char c) {
    editor_t *e = ED();
    if (!e->buf || e->len >= BUFCAP - 1) return;
    for (int i = e->len; i > e->cur; i--) e->buf[i] = e->buf[i - 1];
    e->buf[e->cur++] = c; e->len++; e->buf[e->len] = 0;
    TAB()->dirty = 1;
}
static void ed_backspace(void) {
    editor_t *e = ED();
    if (!e->buf || e->cur <= 0) return;
    for (int i = e->cur - 1; i < e->len - 1; i++) e->buf[i] = e->buf[i + 1];
    e->len--; e->cur--; e->buf[e->len] = 0;
    TAB()->dirty = 1;
}
static void ed_delete(void) {
    editor_t *e = ED();
    if (!e->buf || e->cur >= e->len) return;
    for (int i = e->cur; i < e->len - 1; i++) e->buf[i] = e->buf[i + 1];
    e->len--; e->buf[e->len] = 0;
    TAB()->dirty = 1;
}
static int caret_col(void) {
    editor_t *e = ED(); int col = 0;
    for (int i = e->cur - 1; i >= 0 && e->buf[i] != '\n'; i--) col++;
    return col;
}
static int line_start(int pos) { editor_t *e = ED(); int i = pos; while (i > 0 && e->buf[i-1] != '\n') i--; return i; }
static int line_end(int pos)   { editor_t *e = ED(); int i = pos; while (i < e->len && e->buf[i] != '\n') i++; return i; }
static void ed_up(void) {
    editor_t *e = ED(); int col = caret_col(); int ls = line_start(e->cur);
    if (ls == 0) { e->cur = 0; return; }
    int prev = line_start(ls - 1); int plen = (ls - 1) - prev;
    e->cur = prev + (col < plen ? col : plen);
}
static void ed_down(void) {
    editor_t *e = ED(); int col = caret_col(); int le = line_end(e->cur);
    if (le >= e->len) { e->cur = e->len; return; }
    int next = le + 1; int nlen = line_end(next) - next;
    e->cur = next + (col < nlen ? col : nlen);
}

/* --------------------------------------------------------- output console -- */
static void out_clear(void) {
    memset(S.og, 0, sizeof(S.og));
    S.ox = S.oy = S.otop = 0;
}
static void out_scroll(void) {
    for (int r = 1; r < OROWS; r++) memcpy(S.og[r-1], S.og[r], OCOLS);
    memset(S.og[OROWS-1], 0, OCOLS);
    S.oy = OROWS - 1;
}
static void out_putc(char c) {
    if (c == '\r') { S.ox = 0; return; }
    if (c == '\n') { S.ox = 0; if (++S.oy >= OROWS) out_scroll(); return; }
    if (c == '\t') { int n = 4 - (S.ox & 3); while (n--) out_putc(' '); return; }
    if (c == '\b') { if (S.ox > 0) { S.ox--; S.og[S.oy][S.ox] = 0; } return; }
    if ((unsigned char)c < 32) return;
    if (S.ox < OCOLS - 1) S.og[S.oy][S.ox++] = c;
    else { S.ox = 0; if (++S.oy >= OROWS) out_scroll(); S.og[S.oy][S.ox++] = c; }
}
static void out_puts(const char *s) { while (*s) out_putc(*s++); }
static void out_int(int v) { char t[12]; int p = 0; put_uint(t, &p, v); t[p] = 0; out_puts(t); }
static void out_sink(char c) { if (out_active) out_putc(c); }

/* ------------------------------------------------------------- save/open --- */
/* Write a tab to its path. Returns 0 on success. */
static int tab_save(tab_t *t) {
    if (!t->path[0] || !t->ed.buf) return -1;
    file *f = vfs_open(t->path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) return -1;
    vfs_write(f, t->ed.buf, (uint64_t)t->ed.len);
    vfs_close(f);
    fs_sync();
    t->dirty = 0;
    return 0;
}

/* Open `path` into a tab (reusing one that already has it) and focus it. */
static int open_path(const char *path) {
    int existing = tab_with_path(path);
    if (existing >= 0) { S.cur = existing; return existing; }

    fs_node *n = fs_lookup(path);
    if (!n || n->is_dir) { set_status("Cannot open that"); return -1; }
    if (!fs_may(n, FS_PERM_READ)) { set_status("Permission denied"); return -1; }

    int i = tab_slot();
    if (i < 0) { set_status("Too many tabs open (save or close one)"); return -1; }
    tab_t *t = &S.tab[i];
    if (!tab_alloc(t)) { set_status("Out of memory"); return -1; }

    int len = (int)n->size; if (len > BUFCAP - 1) len = BUFCAP - 1;
    if (n->data && len > 0) memcpy(t->ed.buf, n->data, len);
    t->ed.buf[len] = 0;
    t->ed.len = len; t->ed.cur = 0; t->ed.top = 0;
    sset(t->path, CCB_PATH_MAX, path);
    sset(t->name, sizeof t->name, base_of(path));
    t->lang = lang_for_name(t->name);
    t->dirty = 0;
    t->open = 1;
    S.cur = i;

    char msg[120];
    snprintf(msg, sizeof msg, "Opened %s", t->name);
    set_status(msg);
    return i;
}

/* A new, empty file in the project. It exists on disk straight away, so the
 * tree shows it and a build can pick it up without a save-first dance. */
static void new_file(const char *name) {
    if (!name[0]) { set_status("New file: empty name"); return; }
    char dir[CCB_PATH_MAX], path[CCB_PATH_MAX];
    active_projdir(dir, sizeof dir);
    if (name[0] == '/') snprintf(path, sizeof path, "%s", name);
    else                snprintf(path, sizeof path, "%s/%s", dir, name);

    if (!fs_lookup(dir)) fs_create(dir, 1);
    fs_node *n = fs_lookup(path);
    if (!n) n = fs_create(path, 0);
    if (!n) { set_status("Cannot create that file"); return; }

    int i = open_path(path);
    if (i < 0) return;
    char msg[120];
    snprintf(msg, sizeof msg, "New file %s", S.tab[i].name);
    set_status(msg);
}

static void close_tab(int i) {
    if (i < 0 || i >= MAXTAB || !S.tab[i].open) return;
    if (S.tab[i].dirty) { set_status("Unsaved changes -- save first"); return; }
    S.tab[i].open = 0;
    S.tab[i].path[0] = 0;
    S.tab[i].name[0] = 0;
    S.tab[i].ed.len = S.tab[i].ed.cur = S.tab[i].ed.top = 0;
    if (S.tab[i].ed.buf) S.tab[i].ed.buf[0] = 0;
    while (S.ntab > 0 && !S.tab[S.ntab - 1].open) S.ntab--;
    if (S.cur >= S.ntab || !S.tab[S.cur].open) {
        S.cur = 0;
        for (int k = 0; k < S.ntab; k++) if (S.tab[k].open) { S.cur = k; break; }
    }
}

/* ------------------------------------------------------------- the build --- */
/* Everything dirty and named goes to disk first: the build reads files, not
 * buffers, and an IDE that compiles something other than what you are looking
 * at is worse than one that cannot compile at all. */
static int save_all(void) {
    int n = 0;
    for (int i = 0; i < S.ntab; i++)
        if (S.tab[i].open && S.tab[i].dirty && S.tab[i].path[0])
            if (tab_save(&S.tab[i]) == 0) n++;
    return n;
}

static int bcc_of(int lang) {
    return lang == LANG_CPP ? BCC_CPP : lang == LANG_CS ? BCC_CSHARP : BCC_C;
}

/* Resolve what Run should build. Returns a kmalloc'd unit, or 0 (with a reason
 * already printed to the console). `lang_out` is the dialect to compile it in.
 * A tab with no file behind it compiles alone -- that is what a scratch buffer
 * is for; a saved file builds its project's target. */
static char *resolve_unit(int *lang_out, char *label, int labcap) {
    tab_t *t = TAB();
    *lang_out = bcc_of(t->lang);

    if (!t->path[0]) {
        snprintf(label, labcap, "%s (scratch buffer)", t->name[0] ? t->name : "untitled");
        char *unit = (char *)kmalloc((uint64_t)t->ed.len + 1);
        if (!unit) { out_puts("out of memory\n"); return 0; }
        memcpy(unit, t->ed.buf, t->ed.len);
        unit[t->ed.len] = 0;
        return unit;
    }

    char pdir[CCB_PATH_MAX];
    active_projdir(pdir, sizeof pdir);
    ccb_target_t tg;
    char err[192];
    if (ccb_load(pdir, S.target, &tg, err, sizeof err) != 0) {
        out_puts("build: "); out_puts(err); out_puts("\n");
        return 0;
    }
    char *unit = ccb_unit(&tg, err, sizeof err);
    if (!unit) {
        out_puts("build: "); out_puts(err[0] ? err : "build failed"); out_puts("\n");
        return 0;
    }
    *lang_out = ccb_lang(&tg);
    snprintf(label, labcap, "target %s (%d file%s)", tg.name, tg.nsrc,
             tg.nsrc == 1 ? "" : "s");
    return unit;
}

static void ide_run(void) {
    tab_t *t = TAB();
    if (!t->ed.buf) return;
    t->ed.buf[t->ed.len] = 0;
    int saved = save_all();
    out_clear();

    /* Python has no project model: BoltPy runs one file. */
    if (t->lang == LANG_PY) {
        out_puts("[ Python ]  "); out_puts(t->name[0] ? t->name : "buffer"); out_puts("\n\n");
        S.running = 1; out_active = &S; console_set_sink(out_sink);
        int rc = bpy_run(t->ed.buf);
        console_set_sink(0); out_active = 0; S.running = 0;
        out_puts(rc ? "\n\n[ program exited with errors ]\n" : "\n\n[ program finished ]\n");
        S.otop = S.oy;
        return;
    }

    char label[96];
    int lang;
    S.running = 1; out_active = &S; console_set_sink(out_sink);
    char *unit = resolve_unit(&lang, label, sizeof label);
    if (!unit) {
        console_set_sink(0); out_active = 0; S.running = 0;
        set_status("Build failed");
        S.otop = S.oy;
        return;
    }
    out_puts("[ "); out_puts(boltcc_lang_name(lang)); out_puts(" ]  ");
    out_puts(label);
    if (saved) { out_puts("  -- saved "); out_int(saved); out_puts(" file"); if (saved != 1) out_puts("s"); }
    out_puts("\n\n");

    int rc = boltcc_run(lang, unit);
    kfree(unit);
    console_set_sink(0); out_active = 0; S.running = 0;

    out_puts(rc ? "\n\n[ program exited with errors ]\n" : "\n\n[ program finished ]\n");
    set_status(rc ? "Run failed" : "Run finished");
    S.otop = S.oy;
}

/* --------------------------------------------------- build a real .exe ----- */
/* Bake the same unit Run would compile into a genuine Windows PE32+ console
 * .exe (BoltCC / BoltPython live inside the boltrt template). The file runs on
 * Windows 11 and under BoltOS itself (`winrun NAME.exe`). */
static void ide_build(void) {
    tab_t *t = TAB();
    if (!t->ed.buf) return;
    t->ed.buf[t->ed.len] = 0;
    save_all();
    out_clear();

    char *unit = 0;
    uint32_t ulen = 0;
    char label[96];
    int owned = 0;

    if (t->lang == LANG_PY || !t->path[0]) {
        unit = t->ed.buf; ulen = (uint32_t)t->ed.len;
        snprintf(label, sizeof label, "%s", t->name[0] ? t->name : "buffer");
    } else {
        int lang;
        S.running = 1; out_active = &S; console_set_sink(out_sink);
        unit = resolve_unit(&lang, label, sizeof label);
        console_set_sink(0); out_active = 0; S.running = 0;
        if (!unit) { set_status("Build failed"); S.otop = S.oy; return; }
        ulen = (uint32_t)strlen(unit);
        owned = 1;
    }

    out_puts("[ build ]  "); out_puts(label);
    out_puts(" -> Windows PE32+ .exe ("); out_puts(lang_label(t->lang)); out_puts(")\n\n");

    /* IDE language ids line up with pe_build_exe's: 0=C 1=C++ 2=C# 3=Python */
    uint32_t sz = 0;
    uint8_t *img = pe_build_exe(t->lang, (const uint8_t *)unit, ulen, &sz);
    if (owned) kfree(unit);
    if (!img) {
        out_puts(21u + ulen + 1u > 65536u
                 ? "build failed: program too large (max ~64 KiB)\n"
                 : "build failed: out of memory or corrupt stub\n");
        set_status("Build failed");
        S.otop = S.oy;
        return;
    }

    /* output name: the tab's basename with the extension replaced */
    char name[64]; int p = 0;
    const char *src = t->name[0] ? t->name : "prog";
    int dot = -1;
    for (int i = 0; src[i]; i++) if (src[i] == '.') dot = i;
    for (int i = 0; src[i] && (dot < 0 || i < dot) && p < (int)sizeof(name) - 5; i++) name[p++] = src[i];
    if (!p) { name[p++] = 'p'; name[p++] = 'r'; name[p++] = 'o'; name[p++] = 'g'; }
    name[p++] = '.'; name[p++] = 'e'; name[p++] = 'x'; name[p++] = 'e'; name[p] = 0;

    char dir[CCB_PATH_MAX], path[CCB_PATH_MAX];
    active_projdir(dir, sizeof dir);
    if (!fs_lookup(dir)) fs_create(dir, 1);
    snprintf(path, sizeof path, "%s/%s", dir, name);

    file *fo = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!fo) { kfree(img); out_puts("build failed: cannot write output file\n"); set_status("Build failed"); S.otop = S.oy; return; }
    vfs_write(fo, img, (uint64_t)sz);
    vfs_close(fo);
    fs_sync();
    kfree(img);

    out_puts("wrote "); out_puts(path);
    out_puts("  ("); out_int((int)sz); out_puts(" bytes)\n\n");
    out_puts("  run here:    winrun "); out_puts(name); out_puts("\n");
    out_puts("  on Windows:  copy it out and double-click / run it\n");

    char msg[120];
    snprintf(msg, sizeof msg, "Built %s", name);
    set_status(msg);
    S.otop = S.oy;
}

/* ------------------------------------------------------------- the tree --- */
static int is_expanded(const char *path) {
    for (int i = 0; i < S.nexp; i++) if (!strcmp(S.expand[i], path)) return 1;
    return 0;
}
static void toggle_expand(const char *path) {
    for (int i = 0; i < S.nexp; i++)
        if (!strcmp(S.expand[i], path)) {
            for (int k = i; k < S.nexp - 1; k++) sset(S.expand[k], CCB_PATH_MAX, S.expand[k + 1]);
            S.nexp--;
            return;
        }
    if (S.nexp < MAXEXP) sset(S.expand[S.nexp++], CCB_PATH_MAX, path);
}

/* Point the tree at a different directory. */
static void set_project(const char *dir) {
    fs_node *n = fs_lookup(dir);
    if (!n || !n->is_dir) { set_status("No such directory"); return; }
    sset(S.proj, CCB_PATH_MAX, dir);
    S.target[0] = 0;
    S.nexp = 0;
    char msg[120];
    snprintf(msg, sizeof msg, "Project %s", S.proj);
    set_status(msg);
}

/* Cycle the selected build target. With no build file there is only one. */
static void cycle_target(void) {
    char pdir[CCB_PATH_MAX];
    active_projdir(pdir, sizeof pdir);
    char names[CCB_MAX_TGT][CCB_NAME_MAX];
    int n = ccb_list(pdir, names, CCB_MAX_TGT);
    if (n <= 1) {
        ccb_target_t t; char err[192];
        if (ccb_load(pdir, 0, &t, err, sizeof err) != 0) { set_status(err); return; }
        S.target[0] = 0;
        char msg[120];
        snprintf(msg, sizeof msg, "Target %s (%d file%s)", t.name, t.nsrc, t.nsrc == 1 ? "" : "s");
        set_status(msg);
        return;
    }
    int at = -1;
    for (int i = 0; i < n; i++) if (!strcmp(names[i], S.target)) { at = i; break; }
    at = (at + 1) % n;
    sset(S.target, CCB_NAME_MAX, names[at]);
    char msg[120];
    snprintf(msg, sizeof msg, "Target %s", S.target);
    set_status(msg);
}

/* The name the target button shows. */
static void target_label(char *out, int cap) {
    if (S.target[0]) { snprintf(out, cap, "%s", S.target); return; }
    char pdir[CCB_PATH_MAX];
    active_projdir(pdir, sizeof pdir);
    char names[CCB_MAX_TGT][CCB_NAME_MAX];
    int n = ccb_list(pdir, names, CCB_MAX_TGT);
    if (n > 0) { snprintf(out, cap, "%s", names[0]); return; }
    ccb_target_t t; char err[192];
    if (ccb_load(pdir, 0, &t, err, sizeof err) == 0) snprintf(out, cap, "%s", t.name);
    else snprintf(out, cap, "(none)");
}

/* ------------------------------------------------------------- highlight --- */
/* colour ids: 0 normal, 1 keyword, 2 string, 3 comment, 4 number, 5 builtin */
static uint32_t hl_color(int id) {
    switch (id) {
    case 1: return 0x569CD6;      /* keyword  - blue   */
    case 2: return 0xCE9178;      /* string   - salmon */
    case 3: return 0x6A9955;      /* comment  - green  */
    case 4: return 0xB5CEA8;      /* number   - pale   */
    case 5: return 0xDCDCAA;      /* builtin  - yellow */
    default: return 0xD4D4D4;
    }
}
static const char *KW[] = {
    "int","char","long","short","void","bool","float","double","unsigned","signed",
    "const","static","struct","class","public","private","protected","namespace",
    "using","return","if","else","while","for","do","break","continue","switch",
    "case","default","new","delete","this","true","false","null","nullptr",
    "def","import","from","print","elif","in","not","and","or","None","True","False",
    "string","var","include","define","sizeof","typedef","enum","goto","extern",0
};
static const char *BI[] = {
    "printf","puts","putchar","cout","endl","cin","Console","WriteLine","Write",
    "len","str","abs","min","max","chr","ord","pow","sqrt","gcd","upper","lower",
    "substr","range","main","Main",0
};
static int is_kw(const char *s, int n) {
    for (int i = 0; KW[i]; i++) {
        int L = (int)strlen(KW[i]);
        if (L == n && strncmp(s, KW[i], n) == 0) return 1;
    }
    return 0;
}
static int is_builtin(const char *s, int n) {
    for (int i = 0; BI[i]; i++) {
        int L = (int)strlen(BI[i]);
        if (L == n && strncmp(s, BI[i], n) == 0) return 1;
    }
    return 0;
}
static int isal(char c)  { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int isaln(char c) { return isal(c)||(c>='0'&&c<='9'); }
static int isdg(char c)  { return c>='0'&&c<='9'; }

static void compute_colors(editor_t *e, uint8_t *colr) {
    int py = (S.tab[S.cur].lang == LANG_PY);
    for (int i = 0; i < e->len; ) {
        char c = e->buf[i];
        if (!py && c == '/' && i + 1 < e->len && e->buf[i+1] == '/') {
            while (i < e->len && e->buf[i] != '\n') colr[i++] = 3;
        } else if (!py && c == '/' && i + 1 < e->len && e->buf[i+1] == '*') {
            colr[i++] = 3; colr[i++] = 3;
            while (i < e->len && !(e->buf[i] == '*' && i + 1 < e->len && e->buf[i+1] == '/')) colr[i++] = 3;
            if (i < e->len) { colr[i++] = 3; if (i < e->len) colr[i++] = 3; }
        } else if (py && c == '#') {
            while (i < e->len && e->buf[i] != '\n') colr[i++] = 3;
        } else if (c == '"' || c == '\'') {
            char q = c; colr[i++] = 2;
            while (i < e->len && e->buf[i] != q) { if (e->buf[i] == '\\' && i + 1 < e->len) colr[i++] = 2; colr[i++] = 2; }
            if (i < e->len) colr[i++] = 2;
        } else if (isdg(c) && (i == 0 || !isaln(e->buf[i-1]))) {
            while (i < e->len && (isaln(e->buf[i]) || e->buf[i] == '.')) colr[i++] = 4;
        } else if (isal(c)) {
            int st = i;
            while (i < e->len && isaln(e->buf[i])) i++;
            int id = is_kw(e->buf + st, i - st) ? 1 : is_builtin(e->buf + st, i - st) ? 5 : 0;
            for (int k = st; k < i; k++) colr[k] = (uint8_t)id;
        } else {
            colr[i++] = 0;
        }
    }
}

/* --------------------------------------------------------------- widgets --- */
typedef struct { int x, y, w, h, id, arg; } hot_t;
static hot_t hots[80]; static int nhot;
static int ide_ox, ide_oy;       /* client origin for client-local hot rects   */
enum { H_RUN = 1, H_BUILD, H_NEW, H_SAVE, H_PROJ, H_TARGET, H_TAB, H_TABX, H_TREE };

/* tree rows, filled each draw and consulted by the click handler */
static struct { fs_node *n; int isdir; } trow[64];
static int ntrow;

static void hot(int x, int y, int w, int h, int id, int arg) {
    if (nhot >= (int)(sizeof hots / sizeof hots[0])) return;
    hots[nhot].x = x - ide_ox; hots[nhot].y = y - ide_oy;
    hots[nhot].w = w; hots[nhot].h = h;
    hots[nhot].id = id; hots[nhot].arg = arg;
    nhot++;
}
static int btn(int x, int y, const char *label, int id, uint32_t bg, uint32_t fg) {
    int w = g_text_width(label, 1) + 22, h = 24;
    g_round(x, y, w, h, 6, bg, 255);
    g_text(x + 11, y + 5, label, fg, 1);
    hot(x, y, w, h, id, 0);
    return w;
}

/* editor text-area geometry (filled each draw, used by click handler) */
static int area_tx, area_ty, area_rows, area_cols;

/* Draw one directory's contents, recursing into the ones held open. `y` walks
 * down the sidebar; rows past the bottom are simply not drawn. */
static void tree_dir(const char *dirpath, int depth, int x, int *y, int w,
                     int ybot, int cx, int cy) {
    if (depth >= TREE_DEPTH) return;
    fs_node *d = fs_lookup(dirpath);
    if (!d || !d->is_dir) return;

    for (fs_node *c = d->child; c; c = c->next) {
        if (*y + 18 > ybot) return;
        char path[CCB_PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dirpath, c->name);
        int ind = x + 8 + depth * 10;
        int sel = (!c->is_dir && S.tab[S.cur].open && S.tab[S.cur].path[0] &&
                   !strcmp(S.tab[S.cur].path, path));
        if (sel) g_fill(x + 2, *y - 1, w - 4, 18, COL_ACCENT);
        uint32_t fg = sel ? 0xFFFFFF : c->is_dir ? COL_TEXT : COL_TEXT_DIM;
        if (c->is_dir) {
            g_text(ind, *y + 1, is_expanded(path) ? "-" : "+", fg, 1);
            gui_icon(ICON_FOLDER, ind + 10, *y, 1, fg);
        } else {
            gui_icon(ICON_FILE, ind + 10, *y, 1, fg);
        }
        g_text(ind + 28, *y + 2, c->name, fg, 1);
        if (ntrow < 64) {
            trow[ntrow].n = c; trow[ntrow].isdir = c->is_dir;
            hot(x, *y - 1, w, 18, H_TREE, ntrow);
            ntrow++;
        }
        *y += 18;
        if (c->is_dir && is_expanded(path))
            tree_dir(path, depth + 1, x, y, w, ybot, cx, cy);
    }
    (void)cx; (void)cy;
}

static void ide_draw(window_t *w, int cx, int cy, int cw, int ch) {
    nhot = 0; ntrow = 0; ide_ox = cx; ide_oy = cy;
    tab_t *t = TAB();
    editor_t *e = &t->ed;

    /* ---- toolbar ---- */
    g_fill(cx, cy, cw, TOOLH, COL_PANEL_2);
    g_hline(cx, cy + TOOLH, cw, COL_PANEL_3);
    int bx = cx + 8, by = cy + 7;

    char pl[64];
    snprintf(pl, sizeof pl, "%s", base_of(S.proj));
    bx += btn(bx, by, pl, H_PROJ, COL_PANEL_3, COL_TEXT) + 6;

    char tl[80], tb[96];
    target_label(tl, sizeof tl);
    snprintf(tb, sizeof tb, "target: %s", tl);
    bx += btn(bx, by, tb, H_TARGET, COL_PANEL_3, COL_ACCENT) + 6;

    /* live Ln/Col indicator after the buttons */
    {
        int ln = 1; for (int i = 0; i < e->cur; i++) if (e->buf && e->buf[i] == '\n') ln++;
        char lc[40]; int p = 0;
        lc[p++] = 'L'; lc[p++] = 'n'; lc[p++] = ' ';
        put_uint(lc, &p, ln);
        lc[p++] = ','; lc[p++] = ' '; lc[p++] = 'C'; lc[p++] = 'o'; lc[p++] = 'l'; lc[p++] = ' ';
        put_uint(lc, &p, caret_col() + 1);
        lc[p] = 0;
        g_text(bx + 6, by + 5, lc, COL_TEXT_DIM, 1);
    }

    int rx = cx + cw - 8;
    int rw = g_text_width("Run", 1) + 22;
    rx -= rw; btn(rx, by, "Run", H_RUN, 0x2EA043, 0xFFFFFF);
    int bw = g_text_width("Build .exe", 1) + 22;
    rx -= bw + 6; btn(rx, by, "Build .exe", H_BUILD, 0x0E639C, 0xFFFFFF);
    int nw = g_text_width("New", 1) + 22;
    rx -= nw + 6; btn(rx, by, "New", H_NEW, COL_PANEL_3, COL_TEXT);
    int sw = g_text_width("Save", 1) + 22;
    rx -= sw + 6; btn(rx, by, "Save", H_SAVE, COL_PANEL_3, COL_TEXT);

    int body_y = cy + TOOLH + 1;
    int body_h = ch - TOOLH - 1;

    /* ---- project tree ---- */
    int treew = cw > 640 ? TREEW : 0;      /* a narrow window keeps the editor */
    if (treew) {
        g_fill(cx, body_y, treew, body_h, 0x181820);
        g_vline(cx + treew, body_y, body_h, COL_PANEL_3);
        g_text(cx + 8, body_y + 6, "PROJECT", COL_TEXT_DIM, 1);
        int ty = body_y + 26;
        g_set_clip(cx, body_y + 22, treew, body_h - 22);
        tree_dir(S.proj, 0, cx, &ty, treew, body_y + body_h - 4, cx, cy);
        g_clear_clip();
        if (ty == body_y + 26)
            g_text(cx + 10, body_y + 28, "(empty)", COL_TEXT_DIM, 1);
    }
    int ex = cx + (treew ? treew + 1 : 0);
    int ew = cw - (treew ? treew + 1 : 0);

    /* ---- open-file tabs ---- */
    g_fill(ex, body_y, ew, TABH, COL_PANEL_2);
    g_hline(ex, body_y + TABH, ew, COL_PANEL_3);
    int tx0 = ex + 4;
    for (int i = 0; i < S.ntab; i++) {
        if (!S.tab[i].open) continue;
        const char *nm = S.tab[i].name[0] ? S.tab[i].name : "untitled";
        int tw = g_text_width(nm, 1) + 34;
        if (tx0 + tw > ex + ew - 4) break;
        int active = (i == S.cur);
        g_round(tx0, body_y + 2, tw, TABH - 4, 4, active ? 0x1E1E1E : COL_PANEL_3, 255);
        g_text(tx0 + 8, body_y + 6, nm, active ? COL_TEXT : COL_TEXT_DIM, 1);
        if (S.tab[i].dirty) g_fill(tx0 + tw - 18, body_y + 10, 5, 5, 0xE5C07B);
        else                g_text(tx0 + tw - 20, body_y + 6, "x", COL_TEXT_DIM, 1);
        hot(tx0, body_y + 2, tw - 22, TABH - 4, H_TAB, i);
        hot(tx0 + tw - 22, body_y + 2, 22, TABH - 4, H_TABX, i);
        tx0 += tw + 4;
    }

    /* ---- split: editor on top, output console at the bottom ---- */
    int edtop = body_y + TABH + 1;
    int rest  = body_h - TABH - 1;
    int out_h = rest / 3; if (out_h < 90) out_h = 90; if (out_h > rest - 80) out_h = rest - 80;
    int ed_h  = rest - out_h;

    g_fill(ex, edtop, ew, ed_h, 0x1E1E1E);
    g_fill(ex, edtop, GUTTER, ed_h, 0x181818);

    int tx = ex + GUTTER + 6, ty = edtop + 6;
    int rows = (ed_h - 12) / CELLH; if (rows < 1) rows = 1;
    int cols = (ex + ew - tx) / CELLW; if (cols < 1) cols = 1;
    area_tx = tx - cx; area_ty = ty - cy; area_rows = rows; area_cols = cols;

    int caret_line = 0;
    if (e->buf) for (int i = 0; i < e->cur; i++) if (e->buf[i] == '\n') caret_line++;
    if (caret_line < e->top) e->top = caret_line;
    if (caret_line >= e->top + rows) e->top = caret_line - rows + 1;

    static uint8_t colr[BUFCAP];
    if (e->buf) compute_colors(e, colr);

    int line = 0, col = 0, caret_px = tx, caret_py = ty;
    for (int r = 0; r < rows; r++) {
        int ln = e->top + r + 1;
        char num[8]; int v = ln, p = 0; char tmp[8]; int tn = 0;
        if (v == 0) tmp[tn++] = '0'; while (v) { tmp[tn++] = (char)('0'+v%10); v/=10; }
        while (tn) num[p++] = tmp[--tn]; num[p] = 0;
        g_text(ex + GUTTER - 6 - g_text_width(num,1), ty + r*CELLH, num, 0x5A5A5A, 1);
    }
    if (e->buf) for (int i = 0; i <= e->len; i++) {
        if (i == e->cur) { caret_px = tx + col*CELLW; caret_py = ty + (line - e->top)*CELLH; }
        if (i == e->len) break;
        char c = e->buf[i];
        if (c == '\n') { line++; col = 0; continue; }
        if (c == '\t') { col = (col + 4) & ~3; continue; }
        if (line >= e->top && line < e->top + rows && col < cols) {
            g_char(tx + col*CELLW, ty + (line - e->top)*CELLH, c, hl_color(colr[i]), 1);
        }
        col++;
    }
    if (gui_window_focused(w) && (pit_ticks()/500)%2 == 0 &&
        caret_py >= ty - 2 && caret_py < edtop + ed_h)
        g_fill(caret_px, caret_py, 2, CELLH - 2, 0xFFFFFF);

    /* ---- output console ---- */
    int oy0 = edtop + ed_h;
    g_hline(ex, oy0, ew, COL_PANEL_3);
    g_fill(ex, oy0 + 1, ew, out_h - 1, 0x0C0C12);
    g_text(ex + 8, oy0 + 5, S.running ? "Output  (running)" : "Output", COL_TEXT_DIM, 1);
    if (S.status[0] && !S.running)
        g_text(ex + 8 + g_text_width("Output", 1) + 16, oy0 + 5, S.status, 0x6A9955, 1);
    int otx = ex + 8, oty = oy0 + 22;
    int orows = (out_h - 26) / CELLH; if (orows < 1) orows = 1;
    int ocols = (ew - 16) / CELLW;    if (ocols > OCOLS) ocols = OCOLS;
    int start = S.oy - orows + 1; if (start < 0) start = 0;
    for (int r = 0; r < orows; r++) {
        int row = start + r; if (row >= OROWS) break;
        for (int c = 0; c < ocols; c++) {
            char g = S.og[row][c];
            if (g) g_char(otx + c*CELLW, oty + r*CELLH, g, 0xCDCDCD, 1);
        }
    }

    /* ---- the one-line input strip ---- */
    if (S.prompt != PR_NONE) {
        const char *title = S.prompt == PR_SAVE ? "Save as  (Enter to save, Esc to cancel)"
                          : S.prompt == PR_NEW  ? "New file  (name, Enter to create)"
                                                : "Project directory  (Enter to open)";
        int dw = 420, dh = 64;
        if (dw > cw - 20) dw = cw - 20;
        int dx = cx + (cw - dw) / 2, dy = cy + TOOLH + 8;
        g_round(dx, dy, dw, dh, 8, COL_PANEL_2, 255);
        g_text(dx + 12, dy + 8, title, COL_TEXT_DIM, 1);
        int fx = dx + 12, fy = dy + 30, fw = dw - 24, fh = 22;
        g_round(fx, fy, fw, fh, 5, 0x1E1E1E, 255);
        S.input[S.inlen] = 0;
        g_text(fx + 6, fy + 4, S.input, COL_TEXT, 1);
        if ((pit_ticks()/500)%2 == 0)
            g_fill(fx + 6 + g_text_width(S.input, 1), fy + 3, 2, fh - 6, 0xFFFFFF);
    }
}

/* --------------------------------------------------------------- input ----- */
static void begin_prompt(int kind, const char *prefill) {
    S.prompt = kind;
    sset(S.input, CCB_PATH_MAX, prefill ? prefill : "");
    S.inlen = (int)strlen(S.input);
}

static void do_save(void) {
    tab_t *t = TAB();
    if (t->path[0]) {
        char msg[120];
        if (tab_save(t) == 0) snprintf(msg, sizeof msg, "Saved %s", t->name);
        else                  snprintf(msg, sizeof msg, "Save failed: %s", t->name);
        set_status(msg);
        return;
    }
    char pre[80];
    snprintf(pre, sizeof pre, "%s", t->name[0] ? t->name : "prog");
    if (!t->name[0]) { int n = (int)strlen(pre); snprintf(pre + n, sizeof pre - n, "%s", lang_ext(t->lang)); }
    begin_prompt(PR_SAVE, pre);
}

static void commit_prompt(void) {
    S.input[S.inlen] = 0;
    int kind = S.prompt;
    S.prompt = PR_NONE;
    if (kind == PR_PROJ) { set_project(S.input); return; }
    if (kind == PR_NEW)  { new_file(S.input); return; }

    /* PR_SAVE: give the current buffer a name under the project, then write it */
    if (!S.inlen) { set_status("Save cancelled: empty name"); return; }
    tab_t *t = TAB();
    char dir[CCB_PATH_MAX], path[CCB_PATH_MAX];
    active_projdir(dir, sizeof dir);
    if (S.input[0] == '/') snprintf(path, sizeof path, "%s", S.input);
    else                   snprintf(path, sizeof path, "%s/%s", dir, S.input);
    if (!fs_lookup(dir)) fs_create(dir, 1);
    if (!fs_lookup(path) && !fs_create(path, 0)) { set_status("Save failed: cannot create"); return; }
    sset(t->path, CCB_PATH_MAX, path);
    sset(t->name, sizeof t->name, base_of(path));
    t->lang = lang_for_name(t->name);
    char msg[120];
    if (tab_save(t) == 0) snprintf(msg, sizeof msg, "Saved %s", t->name);
    else                  snprintf(msg, sizeof msg, "Save failed: %s", t->name);
    set_status(msg);
}

static void ide_key(window_t *w, char c) {
    (void)w;
    if (S.running) return;

    if (S.prompt != PR_NONE) {
        unsigned char u = (unsigned char)c;
        if (u == 27) { S.prompt = PR_NONE; }
        else if (c == '\n') commit_prompt();
        else if (c == '\b') { if (S.inlen > 0) S.input[--S.inlen] = 0; }
        else if (u >= 32 && u < 127 && S.inlen < CCB_PATH_MAX - 1) {
            S.input[S.inlen++] = c; S.input[S.inlen] = 0;
        }
        return;
    }

    editor_t *e = ED();
    if (!e->buf) return;

    switch ((unsigned char)c) {
    case KEY_LEFT:  if (e->cur > 0) e->cur--; break;
    case KEY_RIGHT: if (e->cur < e->len) e->cur++; break;
    case KEY_UP:    ed_up();   break;
    case KEY_DOWN:  ed_down(); break;
    case KEY_HOME:  e->cur = line_start(e->cur); break;
    case KEY_END:   e->cur = line_end(e->cur);   break;
    case KEY_DEL:   ed_delete(); break;
    case '\b':      ed_backspace(); break;
    case KEY_COPY: {                  /* copy the caret's line to the clipboard */
        int ls = line_start(e->cur), le = line_end(e->cur);
        clip_set(e->buf + ls, le - ls);
        break;
    }
    case KEY_CUT: {                   /* cut the caret's line (incl. trailing \n) */
        int ls = line_start(e->cur), le = line_end(e->cur);
        clip_set(e->buf + ls, le - ls);
        e->cur = ls;
        while (e->cur < e->len && e->buf[e->cur] != '\n') ed_delete();
        if (e->cur < e->len) ed_delete();          /* the newline too */
        break;
    }
    case KEY_PASTE: {                 /* insert clipboard text at the caret */
        const char *p = clip_get();
        for (int i = 0; p[i]; i++)
            if (p[i] != '\r' && (p[i] == '\n' || p[i] == '\t' || (unsigned char)p[i] >= 32))
                ed_insert(p[i]);
        break;
    }
    case KEY_SAVE:  do_save(); break;            /* Ctrl+S */
    case '\n': {
        /* auto-indent: copy the current line's leading whitespace, and add one
         * level if the line ends in an opening brace. Capture both from the
         * original buffer before inserting (insertion shifts later indices). */
        int ls = line_start(e->cur);
        int last = e->cur - 1;
        while (last >= ls && (e->buf[last] == ' ' || e->buf[last] == '\t')) last--;
        int brace = (last >= ls && e->buf[last] == '{');
        char ws[64]; int ind = 0;
        while (ls + ind < e->cur && ind < 63 &&
               (e->buf[ls + ind] == ' ' || e->buf[ls + ind] == '\t')) { ws[ind] = e->buf[ls + ind]; ind++; }
        ed_insert('\n');
        for (int i = 0; i < ind; i++) ed_insert(ws[i]);
        if (brace) { ed_insert(' '); ed_insert(' '); ed_insert(' '); ed_insert(' '); }
        break;
    }
    case '\t':
        /* soft tabs: insert four spaces, matching the auto-indent step */
        ed_insert(' '); ed_insert(' '); ed_insert(' '); ed_insert(' ');
        break;
    case '(': case '[': case '{': {
        /* auto-close brackets: insert the pair, leave the caret between them */
        char close = c == '(' ? ')' : c == '[' ? ']' : '}';
        ed_insert(c); ed_insert(close); e->cur--;
        break;
    }
    case ')': case ']': case '}': {
        /* step over an auto-inserted closer instead of duplicating it */
        if (e->cur < e->len && e->buf[e->cur] == c) e->cur++;
        else ed_insert(c);
        break;
    }
    default:
        if ((unsigned char)c >= 32) ed_insert(c);
        break;
    }
}

static void tree_click(int row) {
    if (row < 0 || row >= ntrow || !trow[row].n) return;
    char path[CCB_PATH_MAX];
    fs_abspath(trow[row].n, path, sizeof path);
    if (trow[row].isdir) toggle_expand(path);
    else                 open_path(path);
}

static void ide_click(window_t *w, int lx, int ly) {
    (void)w;
    if (S.prompt != PR_NONE) return;    /* type a name / Enter / Esc */

    for (int i = 0; i < nhot; i++) {
        hot_t *h = &hots[i];
        if (lx < h->x || lx >= h->x + h->w || ly < h->y || ly >= h->y + h->h) continue;
        switch (h->id) {
        case H_RUN:    ide_run();   break;
        case H_BUILD:  ide_build(); break;
        case H_NEW:    begin_prompt(PR_NEW, ""); break;
        case H_SAVE:   do_save();   break;
        case H_PROJ:   begin_prompt(PR_PROJ, S.proj); break;
        case H_TARGET: cycle_target(); break;
        case H_TAB:    if (S.tab[h->arg].open) S.cur = h->arg; break;
        case H_TABX:   close_tab(h->arg); break;
        case H_TREE:   tree_click(h->arg); break;
        default: break;
        }
        return;
    }
    /* click in the editor text area places the caret */
    if (lx >= area_tx && ly >= area_ty) {
        editor_t *e = ED();
        if (!e->buf) return;
        int row = (ly - area_ty) / CELLH; if (row < 0) row = 0;
        int colc = (lx - area_tx) / CELLW; if (colc < 0) colc = 0;
        int want = e->top + row;
        int li = 0, idx = 0;
        while (idx < e->len && li < want) { if (e->buf[idx] == '\n') li++; idx++; }
        int c = 0;
        while (idx < e->len && e->buf[idx] != '\n' && c < colc) { idx++; c++; }
        e->cur = idx;
    }
}

static void ide_scroll(window_t *w, int delta) {
    (void)w; editor_t *e = ED();
    if (!e->buf) return;
    e->top -= delta * 3;
    if (e->top < 0) e->top = 0;
    int lines = 0; for (int i = 0; i < e->len; i++) if (e->buf[i] == '\n') lines++;
    if (e->top > lines) e->top = lines;
}

static void ide_tick(window_t *w) { (void)w; gui_request_redraw(); }

/* --------------------------------------------------------- sample programs - */
static const char *SAMPLE[NLANG] = {
/* C */
"#include <stdio.h>\n\n"
"int fib(int n) {\n"
"    if (n < 2) return n;\n"
"    return fib(n - 1) + fib(n - 2);\n"
"}\n\n"
"int main() {\n"
"    printf(\"BoltCC: a real C compiler\\n\");\n"
"    for (int i = 0; i < 10; i++)\n"
"        printf(\"fib(%d) = %d\\n\", i, fib(i));\n"
"    printf(\"sqrt(144)=%d pow(2,10)=%d gcd(48,36)=%d\\n\",\n"
"           sqrt(144), pow(2, 10), gcd(48, 36));\n"
"    printf(\"upper=%s substr=%s\\n\", upper(\"bolt\"), substr(\"BoltOS\", 4, 2));\n"
"    return 0;\n"
"}\n",
/* C++ */
"#include <iostream>\n"
"using namespace std;\n\n"
"int main() {\n"
"    cout << \"Hello from C++\" << endl;\n"
"    int sum = 0;\n"
"    for (int i = 1; i <= 100; i++) sum += i;\n"
"    cout << \"sum 1..100 = \" << sum << endl;\n"
"    return 0;\n"
"}\n",
/* C# */
"using System;\n\n"
"class Program {\n"
"    static int Factorial(int n) {\n"
"        int r = 1;\n"
"        for (int i = 2; i <= n; i++) r *= i;\n"
"        return r;\n"
"    }\n\n"
"    static void Main() {\n"
"        Console.WriteLine(\"Hello from C#\");\n"
"        for (int i = 1; i <= 6; i++)\n"
"            Console.WriteLine(\"{0}! = {1}\", i, Factorial(i));\n"
"    }\n"
"}\n",
/* Python */
"print(\"Hello from BoltPython\")\n\n"
"def fib(n):\n"
"    return n if n < 2 else fib(n - 1) + fib(n - 2)\n\n"
"for i in range(10):\n"
"    print(\"fib(\", i, \") =\", fib(i))\n\n"
"print(\"pow(2,10) =\", pow(2, 10))\n"
"print(\"gcd(48,36) =\", gcd(48, 36))\n"
"print(\"divmod(17,5) =\", divmod(17, 5))\n",
};

static const char *SAMPLE_NAME[NLANG] = { "demo.c", "demo.cpp", "demo.cs", "demo.py" };

void ide_app_init(void) {
    memset(&S, 0, sizeof(S));
    sset(S.proj, CCB_PATH_MAX, IDE_DIR);

    /* The four language samples, as unsaved scratch tabs: the language
     * switcher this app used to have, now expressed as tabs like everything
     * else. They have no path, so Run compiles the buffer on its own. */
    for (int l = 0; l < NLANG; l++) {
        tab_t *t = &S.tab[l];
        if (!tab_alloc(t)) break;
        int n = (int)strlen(SAMPLE[l]); if (n > BUFCAP - 1) n = BUFCAP - 1;
        memcpy(t->ed.buf, SAMPLE[l], n); t->ed.buf[n] = 0;
        t->ed.len = n; t->ed.cur = 0; t->ed.top = 0;
        sset(t->name, sizeof t->name, SAMPLE_NAME[l]);
        t->lang = l;
        t->open = 1;
        S.ntab = l + 1;
    }
    S.cur = 0;

    out_clear();
    out_puts("BoltCC ready.  Open a file from the project tree, or edit a\n"
             "scratch tab, and press Run.  Run builds the project's target\n"
             "(build.txt) when the file you are in belongs to one.\n");

    window_t *win = gui_add_window("Code", 880, 600, 0x569CD6, ICON_CODE);
    if (!win) return;
    win->draw   = ide_draw;
    win->key    = ide_key;
    win->click  = ide_click;
    win->scroll = ide_scroll;
    win->tick   = ide_tick;
    win->min_w = 520; win->min_h = 360;
    win->x = 110; win->y = 50;
}
