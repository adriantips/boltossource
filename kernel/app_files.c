/* ===========================================================================
 *  BoltOS  -  kernel/app_files.c
 *  File Explorer: a Windows-style file manager over the RAM filesystem. A left
 *  navigation pane (Quick access / This PC / Recycle Bin), a toolbar with
 *  back/forward/up + a clickable breadcrumb + New folder / Rename / Delete, an
 *  icon grid of the current directory (folders first), inline rename, a built-in
 *  text/hex viewer and a status bar. Items single-click to select, double-click
 *  to open, and drag onto the desktop to drop a shortcut (see gui.c DnD).
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "fs.h"
#include "string.h"
#include "pit.h"
#include "mouse.h"
#include "keyboard.h"     /* KEY_COPY/CUT/PASTE/DEL */
#include "clipboard.h"
#include "png_enc.h"
#include "kheap.h"
#include "commands.h"     /* sh_human, sh_utoa */
#include "gfx.h"
#include "font.h"
#include "icons.h"

#define TOOLBAR_H  52
#define NAV_W     196
#define STATUS_H   28
#define SBW        10
#define CELL_W     98
#define CELL_H     98
#define ROW_H      32          /* nav-pane row height */
#define DCLICK_MS 400          /* double-click window (PIT @ 1000 Hz) */

#define MAXITEM   512
#define MAXBTN     12
#define MAXNAVR    16
#define MAXCRUMB   FS_MAX_DEPTH

/* toolbar / view actions */
enum { A_BACK = 1, A_FWD, A_UP, A_NEW, A_REN, A_DEL, A_VIEWCLOSE, A_COPY, A_CUT, A_PASTE, A_OPEN,
       A_NEWTXT, A_NEWCSV };

typedef struct { int x, y, w, h; fs_node *node; } irect;   /* grid item / breadcrumb */
typedef struct { int x, y, w, h; int action; } brect;      /* toolbar button         */
typedef struct { int x, y, w, h; fs_node *dir; } nrect;    /* nav-pane entry         */

typedef struct {
    fs_node *dir;                       /* current directory                       */
    fs_node *sel;                       /* selected node, or 0                      */
    int      scroll, content_h;
    int      cw, ch;

    fs_node *back[64]; int nback;
    fs_node *fwd[64];  int nfwd;

    irect    items[MAXITEM]; int nitems;       /* hit rects, client-local */
    brect    btns[MAXBTN];   int nbtn;
    nrect    navs[MAXNAVR];  int nnav;
    irect    crumbs[MAXCRUMB]; int ncrumb;

    int      renaming; fs_node *rnode; char rbuf[FS_NAME_MAX]; int rlen;

    fs_node *view; int view_scroll, view_h;    /* in-window file viewer */

    uint64_t last_click; fs_node *last_node;    /* double-click tracking */

    int      menu_open, menu_x, menu_y;         /* right-click context menu (client-local) */
} files_t;

/* context-menu rows: label + action id, built per open */
typedef struct { const char *label; int action; } mrow;
#define MENU_W 190
#define MENU_RH 28

static files_t  F;
static window_t *FW;

/* left navigation pane shortcuts (resolved against the live fs each frame) */
static const struct { const char *path, *label; int icon, section; } NAV[] = {
    { "/home",           "Home",           ICON_HOME,      0 },
    { "/home/Desktop",   "Desktop",        ICON_DESKTOPF,  0 },
    { "/home/Documents", "Documents",      ICON_DOCS,      0 },
    { "/home/Downloads", "Downloads",      ICON_DOWNLOADS, 0 },
    { "/home/Pictures",  "Pictures",       ICON_PICTURES,  0 },
    { "/home/Music",     "Music",          ICON_MUSICF,    0 },
    { "/",               "Local Disk (C:)",ICON_DRIVE,     1 },
    { "/.trash",         "Recycle Bin",    ICON_TRASH,     1 },
};
#define NAV_COUNT ((int)(sizeof(NAV) / sizeof(NAV[0])))

/* ---- small helpers ------------------------------------------------------ */
static int  is_printable(uint8_t c) { return (c >= 32 && c < 127) || c == '\n' || c == '\t'; }
static int  name_ends(const char *name, const char *suf);
/* The well-known folders wear their own icons, here and in the sidebar. */
static int  icon_for(fs_node *n) {
    if (n->is_dir) {
        if (strcmp(n->name, ".trash") == 0) return n->child ? ICON_TRASH_FULL : ICON_TRASH;
        if (n->parent && strcmp(n->parent->name, "home") == 0 && n->parent->parent == fs_root()) {
            if (!strcmp(n->name, "Desktop"))   return ICON_DESKTOPF;
            if (!strcmp(n->name, "Documents")) return ICON_DOCS;
            if (!strcmp(n->name, "Downloads")) return ICON_DOWNLOADS;
            if (!strcmp(n->name, "Pictures"))  return ICON_PICTURES;
            if (!strcmp(n->name, "Music"))     return ICON_MUSICF;
            if (!strcmp(n->name, "Videos"))    return ICON_VIDEOS;
        }
        if (n == fs_root()) return ICON_DRIVE;
        if (!strcmp(n->name, "home") && n->parent == fs_root()) return ICON_HOME;
        return ICON_FOLDER;
    }
    if (name_ends(n->name, ".png") || name_ends(n->name, ".jpg") || name_ends(n->name, ".jpeg") ||
        name_ends(n->name, ".gif") || name_ends(n->name, ".bmp") || name_ends(n->name, ".tga"))
        return ICON_FILE_IMAGE;
    if (name_ends(n->name, ".csv")) return ICON_FILE_SHEET;
    if (name_ends(n->name, ".wav") || name_ends(n->name, ".mp3")) return ICON_FILE_AUDIO;
    if (name_ends(n->name, ".html") || name_ends(n->name, ".htm") || name_ends(n->name, ".css")) return ICON_FILE_WEB;
    if (name_ends(n->name, ".c") || name_ends(n->name, ".h") || name_ends(n->name, ".js") ||
        name_ends(n->name, ".py") || name_ends(n->name, ".asm") || name_ends(n->name, ".sh"))
        return ICON_FILE_CODE;
    if (name_ends(n->name, ".exe") || name_ends(n->name, ".elf")) return ICON_FILE_EXE;
    if (name_ends(n->name, ".zip") || name_ends(n->name, ".tar") || name_ends(n->name, ".gz") ||
        name_ends(n->name, ".bpk")) return ICON_FILE_ARCHIVE;
    return ICON_FILE_TEXT;
}
static fs_node *trash_dir(void) {
    fs_node *t = fs_lookup("/.trash");
    return t ? t : fs_create("/.trash", 1);
}

/* build "<dir>/<name>" and create the node */
static fs_node *make_in(fs_node *dir, const char *name, int isdir) {
    char path[FS_PATH_MAX];
    fs_abspath(dir, path, sizeof(path));
    uint32_t L = (uint32_t)strlen(path);
    if (L && path[L - 1] != '/') kstrlcat(path, "/", sizeof(path));
    kstrlcat(path, name, sizeof(path));
    return fs_create(path, isdir);
}

/* ---- navigation --------------------------------------------------------- */
static void reset_pane(files_t *st) { st->sel = 0; st->scroll = 0; st->view = 0; st->renaming = 0; }

static void go(files_t *st, fs_node *d, int record) {
    if (!d || !d->is_dir || d == st->dir) { if (d == st->dir) { st->view = 0; st->scroll = 0; } return; }
    if (record && st->dir) { if (st->nback < 64) st->back[st->nback++] = st->dir; st->nfwd = 0; }
    st->dir = d;
    reset_pane(st);
}
static void go_back(files_t *st) {
    if (st->nback <= 0) return;
    if (st->nfwd < 64) st->fwd[st->nfwd++] = st->dir;
    st->dir = st->back[--st->nback]; reset_pane(st);
}
static void go_fwd(files_t *st) {
    if (st->nfwd <= 0) return;
    if (st->nback < 64) st->back[st->nback++] = st->dir;
    st->dir = st->fwd[--st->nfwd]; reset_pane(st);
}
static void go_up(files_t *st) { if (st->dir && st->dir->parent) go(st, st->dir->parent, 1); }

/* case-insensitive "does name end with suffix" (suffix lower-case) */
static int name_ends(const char *name, const char *suf) {
    int ln = (int)strlen(name), ls = (int)strlen(suf);
    if (ls > ln) return 0;
    const char *p = name + ln - ls;
    for (int i = 0; i < ls; i++) {
        char c = p[i]; if (c >= 'A' && c <= 'Z') c += 32;
        if (c != suf[i]) return 0;
    }
    return 1;
}

static void open_item(files_t *st, fs_node *n) {
    if (!n) return;
    if (n->is_dir) { go(st, n, 1); return; }
    /* file associations: hand known document types to their editor app */
    if (name_ends(n->name, ".csv")) {
        char p[FS_PATH_MAX]; fs_abspath(n, p, sizeof(p)); sheets_open_path(p); return;
    }
    if (name_ends(n->name, ".png") || name_ends(n->name, ".jpg") ||
        name_ends(n->name, ".jpeg") || name_ends(n->name, ".gif") ||
        name_ends(n->name, ".bmp") || name_ends(n->name, ".tga")) {
        char p[FS_PATH_MAX]; fs_abspath(n, p, sizeof(p)); imgview_open_path(p); return;
    }
    if (name_ends(n->name, ".wav") || name_ends(n->name, ".mp3")) {
        char p[FS_PATH_MAX]; fs_abspath(n, p, sizeof(p)); media_open_path(p); return;
    }
    if (name_ends(n->name, ".txt") || name_ends(n->name, ".md") ||
        name_ends(n->name, ".log") || name_ends(n->name, ".ini") ||
        name_ends(n->name, ".cfg") || name_ends(n->name, ".sh")) {
        char p[FS_PATH_MAX]; fs_abspath(n, p, sizeof(p)); notes_open_path(p); return;
    }
    st->view = n; st->view_scroll = 0;            /* everything else: built-in viewer */
}

/* ---- create / rename / delete ------------------------------------------- */
static void begin_rename(files_t *st, fs_node *n) {
    if (!n) return;
    st->renaming = 1; st->rnode = n;
    strncpy(st->rbuf, n->name, sizeof(st->rbuf));
    st->rlen = (int)strlen(st->rbuf);
}
static void commit_rename(files_t *st) {
    if (st->renaming && st->rnode && st->rlen > 0 && strcmp(st->rbuf, st->rnode->name) != 0)
        fs_move(st->rnode, st->rnode->parent, st->rbuf);   /* fails silently on clash */
    st->renaming = 0; st->rnode = 0;
}

static void new_folder(files_t *st) {
    if (!st->dir) return;
    char name[FS_NAME_MAX]; strncpy(name, "New folder", sizeof(name));
    int k = 1;
    while (fs_child(st->dir, name)) {
        char num[8]; sh_utoa((uint64_t)++k, num);
        name[0] = 0;
        kstrlcat(name, "New folder (", sizeof(name));
        kstrlcat(name, num, sizeof(name));
        kstrlcat(name, ")", sizeof(name));
    }
    fs_node *n = make_in(st->dir, name, 1);
    if (n) { st->sel = n; begin_rename(st, n); }
}

/* Create a new document with a unique "<base> [(k)]<ext>" name in the current
 * folder, then open it via the file association (so .csv lands in Sheets, a
 * text file in Notepad). */
static void new_file(files_t *st, const char *base, const char *ext) {
    if (!st->dir) return;
    char name[FS_NAME_MAX];
    name[0] = 0; kstrlcat(name, base, sizeof(name)); kstrlcat(name, ext, sizeof(name));
    int k = 1;
    while (fs_child(st->dir, name)) {
        char num[8]; sh_utoa((uint64_t)++k, num);
        name[0] = 0; kstrlcat(name, base, sizeof(name));
        kstrlcat(name, " (", sizeof(name)); kstrlcat(name, num, sizeof(name)); kstrlcat(name, ")", sizeof(name));
        kstrlcat(name, ext, sizeof(name));
    }
    fs_node *n = make_in(st->dir, name, 0);
    if (n) { st->sel = n; open_item(st, n); }
}

static void delete_sel(files_t *st) {
    fs_node *n = st->sel;
    if (!n || n == fs_root() || strcmp(n->name, ".trash") == 0) return;

    if (n->parent && strcmp(n->parent->name, ".trash") == 0) {
        fs_remove_node(n);                                  /* already trashed -> purge */
    } else {
        fs_node *tr = trash_dir();
        if (!tr) return;
        char orig[FS_TRASH_MAX]; fs_abspath(n, orig, sizeof(orig));
        char nm[FS_NAME_MAX]; strncpy(nm, n->name, sizeof(nm));
        int suf = 1;
        while (fs_child(tr, nm)) {
            char num[8]; sh_utoa((uint64_t)suf++, num);
            nm[0] = 0; kstrlcat(nm, n->name, sizeof(nm));
            kstrlcat(nm, "~", sizeof(nm)); kstrlcat(nm, num, sizeof(nm));
        }
        if (fs_move(n, tr, nm) == 0) strncpy(n->tpath, orig, sizeof(n->tpath));
    }
    st->sel = 0;
}

/* ---- copy / cut / paste ------------------------------------------------- *
 * A one-slot file clipboard: remember a node and whether the pending op moves
 * it (cut) or duplicates it (copy). Paste lands the result in the current dir
 * under a non-clashing name. */
static fs_node *fclip;
static int      fclip_cut;

/* Copying here also puts the path on the *system* clipboard, so a file copied
 * in File Explorer can be pasted as text into Notes or a Terminal. fclip is
 * kept alongside it because a cut has to remember the node itself -- a move is
 * not a path operation. */
static void publish_selection(files_t *st) {
    if (!st->sel) return;
    char path[FS_PATH_MAX];
    fs_abspath(st->sel, path, sizeof path);
    clip_set_files(path, (int)strlen(path));
}
static void copy_sel(files_t *st) {
    if (st->sel && st->sel != fs_root()) { fclip = st->sel; fclip_cut = 0; publish_selection(st); }
}
static void cut_sel (files_t *st) {
    if (st->sel && st->sel != fs_root()) { fclip = st->sel; fclip_cut = 1; publish_selection(st); }
}

/* true if `anc` is `n` or an ancestor of `n` (guards pasting a dir into itself) */
static int is_ancestor(fs_node *anc, fs_node *n) {
    for (fs_node *p = n; p; p = p->parent) if (p == anc) return 1;
    return 0;
}
/* pick a name unique within `dir`, seeded from `base` (adds " copy"/"~N"). */
static void unique_name(fs_node *dir, const char *base, char *out, int cap) {
    strncpy(out, base, cap); out[cap - 1] = 0;
    if (!fs_child(dir, out)) return;
    char cand[FS_NAME_MAX];
    cand[0] = 0; kstrlcat(cand, base, sizeof(cand)); kstrlcat(cand, " copy", sizeof(cand));
    int suf = 1;
    while (fs_child(dir, cand)) {
        char num[8]; sh_utoa((uint64_t)++suf, num);
        cand[0] = 0; kstrlcat(cand, base, sizeof(cand));
        kstrlcat(cand, " copy ", sizeof(cand)); kstrlcat(cand, num, sizeof(cand));
    }
    strncpy(out, cand, cap); out[cap - 1] = 0;
}
/* deep-copy `src` into `dir` as `name`; recurses through subdirectories. */
static fs_node *copy_node_into(fs_node *dir, fs_node *src, const char *name) {
    char path[FS_PATH_MAX]; fs_abspath(dir, path, sizeof(path));
    if (path[0] && path[strlen(path) - 1] != '/') kstrlcat(path, "/", sizeof(path));
    kstrlcat(path, name, sizeof(path));
    fs_node *dst = fs_create(path, src->is_dir);
    if (!dst) return 0;
    if (src->is_dir) {
        for (fs_node *c = src->child; c; c = c->next)
            copy_node_into(dst, c, c->name);
    } else if (src->data && src->size) {
        fs_write(dst, src->data, src->size);
    }
    return dst;
}
/* Build the context-menu rows for the current selection. Returns the count. */
static int build_menu(files_t *st, mrow *out, int max) {
    int n = 0;
    if (st->sel) {
        if (n < max) out[n++] = (mrow){ "Open",   A_OPEN };
        if (n < max) out[n++] = (mrow){ "Copy",   A_COPY };
        if (n < max) out[n++] = (mrow){ "Cut",    A_CUT  };
        if (n < max) out[n++] = (mrow){ "Rename", A_REN  };
        if (n < max) out[n++] = (mrow){ "Delete", A_DEL  };
    } else {
        if (n < max) out[n++] = (mrow){ "New folder", A_NEW };
        if (n < max) out[n++] = (mrow){ "New text",   A_NEWTXT };
        if (n < max) out[n++] = (mrow){ "New sheet",  A_NEWCSV };
    }
    if ((fclip || clip_kind() == CLIP_FILES || clip_kind() == CLIP_IMAGE) && n < max)
        out[n++] = (mrow){ "Paste", A_PASTE };
    return n;
}
static fs_node *item_at(files_t *st, int lx, int ly) {
    for (int i = 0; i < st->nitems; i++) {
        irect *r = &st->items[i];
        if (lx >= r->x && lx < r->x + r->w && ly >= r->y && ly < r->y + r->h) return r->node;
    }
    return 0;
}

/* Paste an image off the system clipboard as a PNG in this directory. The OS
 * grew a PNG writer for screenshots; this is the same one, so "copy a picture,
 * paste it into a folder" costs nothing but the wiring. */
static int paste_clip_image(files_t *st) {
    int w = 0, h = 0;
    const uint32_t *px = clip_image(&w, &h);
    if (!px) return 0;
    uint8_t *png = 0; uint32_t len = 0;
    if (png_encode(px, w, h, &png, &len) != 0 || !png) return 0;
    char nm[FS_NAME_MAX];
    unique_name(st->dir, "pasted.png", nm, sizeof nm);
    char path[FS_PATH_MAX];
    fs_abspath(st->dir, path, sizeof path);
    if (path[0] && path[strlen(path) - 1] != '/') kstrlcat(path, "/", sizeof path);
    kstrlcat(path, nm, sizeof path);
    fs_node *f = fs_create(path, 0);
    int ok = f && fs_write(f, png, len) == 0;
    kfree(png);
    if (ok) st->sel = f;
    return ok;
}

/* Paste paths off the system clipboard -- what somebody copied in another
 * window, or typed with `clip set`. */
static int paste_clip_files(files_t *st) {
    int n = clip_file_count(), done = 0;
    for (int i = 0; i < n; i++) {
        char p[FS_PATH_MAX];
        if (!clip_file_at(i, p, sizeof p)) continue;
        fs_node *src = fs_lookup(p);
        if (!src || src == fs_root() || is_ancestor(src, st->dir)) continue;
        char nm[FS_NAME_MAX];
        unique_name(st->dir, src->name, nm, sizeof nm);
        /* TODO: copy_node_into recurses per fs_node with no size budget, so a
         * clipboard full of paths pointing at something big (a whole Pictures
         * folder) blocks the redraw loop for as long as the copy takes. Fine
         * for the trees this OS ships with; would want a background copy if
         * that ever stops being true. */
        fs_node *made = copy_node_into(st->dir, src, nm);
        if (made) { st->sel = made; done++; }
    }
    return done;
}

static void paste_into(files_t *st) {
    /* Nothing cut or copied in this window? Then whatever is on the system
     * clipboard is what the user means. */
    if (!fclip && st->dir) {
        if (clip_kind() == CLIP_IMAGE) { paste_clip_image(st); return; }
        if (clip_kind() == CLIP_FILES) { paste_clip_files(st); return; }
    }
    if (!fclip || !st->dir) return;
    if (fclip_cut && is_ancestor(fclip, st->dir)) return;   /* no move into own subtree */
    char nm[FS_NAME_MAX];
    unique_name(st->dir, fclip->name, nm, sizeof(nm));
    if (fclip_cut) {
        if (fs_move(fclip, st->dir, nm) == 0) st->sel = fclip;
        fclip = 0;                                          /* a cut is consumed once */
    } else {
        fs_node *n = copy_node_into(st->dir, fclip, nm);
        if (n) st->sel = n;
    }
}

/* ---- drawing primitives ------------------------------------------------- */
static int hover(int x, int y, int w, int h) {       /* desktop coordinates */
    int mx = gui_mouse_x(), my = gui_mouse_y();
    return mx >= x && mx < x + w && my >= y && my < y + h;
}
static uint32_t wash(void)   { return THEME_DARK ? 0xFFFFFF : 0x000000; }
static int      wash_a(void) { return THEME_DARK ? 22 : 12; }

static const char *TIP[] = { "", "Back", "Forward", "Up", "New folder", "Rename", "Delete", "Close",
                             "Copy", "Cut", "Paste", "Open", "New text document", "New sheet" };
static int tip_action, tip_x, tip_y;

/* a glyph button in the command bar; records its hit rect client-local */
static void tbutton(files_t *st, int cx, int cy, int x, int y, int sz, int glyph, int action, int enabled) {
    int hot = enabled && hover(x, y, sz, sz);
    if (hot) { gfx_rrect(x, y, sz, sz, 8, wash(), wash_a()); tip_action = action; tip_x = x; tip_y = y + sz + 6; }
    glyph_draw_c(glyph, x + sz / 2, y + sz / 2, 20, enabled ? COL_TEXT : COL_TEXT_MUTE, enabled ? 255 : 160);
    if (enabled && st->nbtn < MAXBTN)
        st->btns[st->nbtn++] = (brect){ x - cx, y - cy, sz, sz, action };
}

/* ---- toolbar + breadcrumb ---------------------------------------------- */
static void draw_toolbar(files_t *st, window_t *w, int cx, int cy, int cw) {
    (void)w;
    gfx_fill(cx, cy, cw, TOOLBAR_H, COL_PANEL);
    gfx_fill(cx, cy + TOOLBAR_H - 1, cw, 1, COL_SEP);
    tip_action = 0;
    int by = cy + 9, sz = 34;
    int bx = cx + 10;
    tbutton(st, cx, cy, bx, by, sz, GLYPH_ARROW_L, A_BACK, st->nback > 0);  bx += sz + 2;
    tbutton(st, cx, cy, bx, by, sz, GLYPH_ARROW_R, A_FWD,  st->nfwd  > 0);  bx += sz + 2;
    tbutton(st, cx, cy, bx, by, sz, GLYPH_ARROW_U, A_UP,   st->dir && st->dir->parent); bx += sz + 10;

    /* the command group on the right */
    int rx = cx + cw - 10;
    rx -= sz; tbutton(st, cx, cy, rx, by, sz, GLYPH_TRASH,  A_DEL,   st->sel != 0);
    rx -= sz; tbutton(st, cx, cy, rx, by, sz, GLYPH_RENAME, A_REN,   st->sel != 0);
    rx -= sz + 6;
    gfx_fill(rx + sz + 2, by + 8, 1, sz - 16, COL_SEP);
    tbutton(st, cx, cy, rx, by, sz, GLYPH_PASTE, A_PASTE, fclip != 0 || clip_kind() == CLIP_FILES || clip_kind() == CLIP_IMAGE);
    rx -= sz; tbutton(st, cx, cy, rx, by, sz, GLYPH_COPY,  A_COPY,  st->sel != 0);
    rx -= sz; tbutton(st, cx, cy, rx, by, sz, GLYPH_CUT,   A_CUT,   st->sel != 0);
    rx -= sz + 6;
    gfx_fill(rx + sz + 2, by + 8, 1, sz - 16, COL_SEP);
    tbutton(st, cx, cy, rx, by, sz, GLYPH_NEW_FOLDER, A_NEW, 1);

    /* the address bar between them */
    int ax = bx, aw = rx - 10 - bx; if (aw < 80) aw = 80;
    gfx_rrect(ax, by, aw, sz, 9, THEME_DARK ? 0x000000 : 0xFFFFFF, THEME_DARK ? 70 : 255);
    gfx_rrect_stroke(ax, by, aw, sz, 9, 1, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 20 : 30);
    gfx_clip_push(ax + 1, by + 1, aw - 2, sz - 2);
    fs_node *chain[MAXCRUMB]; int n = 0;
    for (fs_node *p = st->dir; p && n < MAXCRUMB; p = p->parent) { chain[n++] = p; if (p == fs_root()) break; }
    st->ncrumb = 0;
    int px = ax + 10, cyb = by + sz / 2;
    icon_draw(st->dir ? icon_for(st->dir) : ICON_FOLDER, px, cyb - 8, 16, 255);
    px += 24;
    for (int i = n - 1; i >= 0; i--) {
        const char *nm = (chain[i] == fs_root()) ? "This PC" : chain[i]->name;
        int tw = font_width(i == 0 ? FONT_MEDIUM : FONT_REGULAR, 13, nm, -1);
        int hot = hover(px - 4, by + 4, tw + 8, sz - 8);
        if (hot) gfx_rrect(px - 4, by + 5, tw + 8, sz - 10, 6, wash(), wash_a());
        text_l(px, cyb, nm, i == 0 ? FONT_MEDIUM : FONT_REGULAR, 13, i == 0 ? COL_TEXT : COL_TEXT_DIM);
        if (st->ncrumb < MAXCRUMB)
            st->crumbs[st->ncrumb++] = (irect){ px - 4 - cx, by - cy, tw + 8, sz, chain[i] };
        px += tw;
        if (i > 0) { glyph_draw_c(GLYPH_CHEV_R, px + 10, cyb, 16, COL_TEXT_MUTE, 255); px += 20; }
    }
    gfx_clip_pop();
}

/* ---- navigation pane ---------------------------------------------------- */
static void draw_nav(files_t *st, int cx, int cy, int ch) {
    int x = cx, y = cy + TOOLBAR_H, h = ch - TOOLBAR_H - STATUS_H;
    uint32_t side = THEME_DARK ? gfx_shade(COL_PANEL, 82) : gfx_shade(COL_PANEL, 98);
    gfx_fill(x, y, NAV_W, h, side);
    gfx_fill(x + NAV_W, y, 1, h, COL_SEP);

    st->nnav = 0;
    int ry = y + 12, sect = -1;
    const char *titles[] = { "Favourites", "This PC" };
    for (int i = 0; i < NAV_COUNT; i++) {
        if (NAV[i].section != sect) {
            sect = NAV[i].section;
            if (sect) ry += 8;
            text_l(x + 16, ry + 8, titles[sect], FONT_SEMIBOLD, 11, COL_TEXT_MUTE);
            ry += 22;
        }
        fs_node *d = fs_lookup(NAV[i].path);
        if (!d) continue;
        int sel = d == st->dir;
        int hot = hover(x + 8, ry, NAV_W - 16, ROW_H - 2);
        if (sel)      gfx_rrect(x + 8, ry, NAV_W - 16, ROW_H - 2, 8, COL_ACCENT, THEME_DARK ? 70 : 46);
        else if (hot) gfx_rrect(x + 8, ry, NAV_W - 16, ROW_H - 2, 8, wash(), wash_a());
        icon_draw(NAV[i].icon == ICON_TRASH ? icon_for(d) : NAV[i].icon, x + 18, ry + (ROW_H - 2) / 2 - 10, 20, 255);
        text_fit_l(x + 46, ry + (ROW_H - 2) / 2, NAV[i].label, NAV_W - 62, sel ? FONT_MEDIUM : FONT_REGULAR, 13, COL_TEXT);
        if (st->nnav < MAXNAVR) st->navs[st->nnav++] = (nrect){ (x + 8) - cx, ry - cy, NAV_W - 16, ROW_H - 2, d };
        ry += ROW_H;
    }
}

/* ---- scrollbar (shared by listing + viewer) ----------------------------- */
static void scrollbar(int x, int y, int h, int content_h, int scroll) {
    if (content_h <= h) return;
    int th = h * h / content_h; if (th < 28) th = 28;
    int maxs = content_h - h;
    int ty = y + 4 + (h - 8 - th) * scroll / (maxs ? maxs : 1);
    gfx_rrect(x + 2, ty, 5, th, 3, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 60 : 50);
}

/* ---- file grid ---------------------------------------------------------- */
static void draw_listing(files_t *st, window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    int X = cx + NAV_W + 1, Y = cy + TOOLBAR_H, CW = cw - NAV_W - 1, CH = ch - TOOLBAR_H - STATUS_H;
    gfx_fill(X, Y, CW, CH, COL_PANEL);
    gfx_clip_push(X, Y, CW - SBW, CH);

    int cols = (CW - 20 - SBW) / CELL_W; if (cols < 1) cols = 1;
    st->nitems = 0;
    int k = 0;
    /* directories first, then files, the way Explorer groups them */
    for (int pass = 0; pass < 2; pass++) {
        for (fs_node *c = st->dir ? st->dir->child : 0; c; c = c->next) {
            if ((pass == 0) != (c->is_dir != 0)) continue;
            int col = k % cols, row = k / cols;
            int ix = X + 12 + col * CELL_W;
            int iy = Y + 10 + row * CELL_H - st->scroll;
            k++;
            if (iy + CELL_H < Y || iy > Y + CH) continue;     /* fully scrolled out */

            int selnode = (c == st->sel);
            if (selnode) {
                gfx_rrect(ix + 4, iy + 2, CELL_W - 8, CELL_H - 6, 10, COL_ACCENT, THEME_DARK ? 64 : 40);
                gfx_rrect_stroke(ix + 4, iy + 2, CELL_W - 8, CELL_H - 6, 10, 1, COL_ACCENT, 150);
            } else if (hover(ix + 4, iy + 2, CELL_W - 8, CELL_H - 6)) {
                gfx_rrect(ix + 4, iy + 2, CELL_W - 8, CELL_H - 6, 10, wash(), wash_a());
            }
            icon_draw(icon_for(c), ix + (CELL_W - 48) / 2, iy + 10, 48, c->name[0] == '.' ? 150 : 255);

            int base = iy + 76, lcx = ix + CELL_W / 2;
            if (st->renaming && st->rnode == c) {
                int bw = CELL_W - 8;
                gfx_rrect(ix + 4, base - 15, bw, 22, 6, THEME_DARK ? 0x141416 : 0xFFFFFF, 255);
                gfx_rrect_stroke(ix + 3, base - 16, bw + 2, 24, 7, 2, COL_ACCENT, 255);
                gfx_clip_push(ix + 7, base - 15, bw - 6, 22);
                int tw = font_width(FONT_REGULAR, 12, st->rbuf, -1);
                int tx = tw > bw - 12 ? ix + bw - 2 - tw : lcx - tw / 2;
                font_draw(FONT_REGULAR, 12, tx, base, st->rbuf, -1, COL_TEXT, 255);
                if ((pit_ticks() / 530) % 2 == 0) gfx_fill(tx + tw + 1, base - 11, 2, 14, COL_ACCENT);
                gfx_clip_pop();
            } else {
                int tw = font_width(FONT_REGULAR, 12, c->name, -1);
                if (tw > CELL_W - 12) tw = CELL_W - 12;
                font_draw_fit(FONT_REGULAR, 12, lcx - tw / 2, base, c->name, CELL_W - 12,
                              c->name[0] == '.' ? COL_TEXT_DIM : COL_TEXT, 255);
            }
            if (st->nitems < MAXITEM)
                st->items[st->nitems++] = (irect){ ix - cx, iy - cy, CELL_W, CELL_H, c };
        }
    }
    gfx_clip_pop();

    if (k == 0) {
        glyph_draw_c(GLYPH_FOLDER_OPEN, X + CW / 2, Y + CH / 2 - 18, 24, COL_TEXT_MUTE, 255);
        text_c(X + CW / 2, Y + CH / 2 + 14, "This folder is empty", FONT_MEDIUM, 13, COL_TEXT_DIM);
    }
    int rows = (k + cols - 1) / cols;
    st->content_h = rows * CELL_H + 20;
    scrollbar(X + CW - SBW, Y, CH, st->content_h, st->scroll);
}

/* ---- text/hex viewer ---------------------------------------------------- */
static void draw_viewer(files_t *st, window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    fs_node *f = st->view;
    int X = cx + NAV_W + 1, Y = cy + TOOLBAR_H, CW = cw - NAV_W - 1, CH = ch - TOOLBAR_H - STATUS_H;
    gfx_fill(X, Y, CW, CH, COL_PANEL);

    /* header */
    gfx_fill(X, Y + 35, CW, 1, COL_SEP);
    icon_draw(icon_for(f), X + 12, Y + 8, 20, 255);
    text_fit_l(X + 40, Y + 18, f->name, CW - 90, FONT_SEMIBOLD, 13, COL_TEXT);
    int clx = X + CW - 34, cly = Y + 4;
    if (hover(clx, cly, 28, 28)) gfx_rrect(clx, cly, 28, 28, 7, wash(), wash_a());
    glyph_draw_c(GLYPH_CLOSE, clx + 14, cly + 14, 16, COL_TEXT, 255);
    if (st->nbtn < MAXBTN) st->btns[st->nbtn++] = (brect){ clx - cx, cly - cy, 28, 28, A_VIEWCLOSE };

    /* detect binary in the first chunk */
    int binary = 0;
    uint32_t scan = f->size < 256 ? f->size : 256;
    for (uint32_t i = 0; i < scan; i++) if (!is_printable(f->data[i])) { binary = 1; break; }

    int bX = X + 16, bY = Y + 46, bW = CW - 32 - SBW;
    gfx_clip_push(X, Y + 36, CW - SBW, CH - 36);
    int cols = bW / 8; if (cols < 8) cols = 8;
    int line_h = 17, py = bY - st->view_scroll, curcol = 0;

    if (binary) {
        /* hex dump: "OOOOOO  XX XX ... " 16 bytes per line */
        for (uint32_t off = 0; off < f->size; off += 16) {
            int ry = py + (int)(off / 16) * line_h;
            if (ry + line_h < Y + 36 || ry > Y + CH) continue;
            char ln[80]; int p = 0; char hx[8];
            sh_utoa(off, hx); for (const char *q = hx; *q; q++) ln[p++] = *q; ln[p++] = ' '; ln[p++] = ' ';
            for (uint32_t j = 0; j < 16 && off + j < f->size && p < 70; j++) {
                uint8_t v = f->data[off + j];
                const char *H = "0123456789abcdef";
                ln[p++] = H[v >> 4]; ln[p++] = H[v & 0xF]; ln[p++] = ' ';
            }
            ln[p] = 0;
            font_draw(FONT_MONO, 13, bX, ry + 12, ln, -1, THEME_DARK ? 0x9FE0A0 : 0x1F7A3A, 255);
        }
        st->view_h = (int)((f->size + 15) / 16) * line_h + 16;
    } else {
        for (uint32_t i = 0; i < f->size; i++) {
            uint8_t c = f->data[i];
            if (c == '\n') { py += line_h; curcol = 0; continue; }
            if (c == '\t') { curcol = (curcol + 4) & ~3; if (curcol >= cols) { py += line_h; curcol = 0; } continue; }
            if (!is_printable(c)) c = '.';
            if (curcol >= cols) { py += line_h; curcol = 0; }
            if (py + line_h >= Y + 36 && py <= Y + CH) g_char(bX + curcol * 8, py, (char)c, COL_TEXT, 1);
            curcol++;
        }
        st->view_h = (py + st->view_scroll - bY) + line_h + 16;
    }
    gfx_clip_pop();
    scrollbar(X + CW - SBW, Y + 36, CH - 36, st->view_h, st->view_scroll);
}

/* ---- status bar --------------------------------------------------------- */
static void draw_status(files_t *st, int cx, int cy, int cw, int ch) {
    int y = cy + ch - STATUS_H;
    gfx_fill(cx, y, cw, STATUS_H, COL_PANEL);
    gfx_fill(cx, y, cw, 1, COL_SEP);
    int my = y + STATUS_H / 2;
    if (!fs_persist_active() || fs_persist_readonly()) {
        glyph_draw_c(GLYPH_WARN, cx + 20, my, 16, COL_WARN, 255);
        text_l(cx + 34, my, "Changes will not be saved to disk", FONT_REGULAR, 12, COL_WARN);
        return;
    }
    int items = 0;
    for (fs_node *c = st->dir ? st->dir->child : 0; c; c = c->next) items++;
    char line[64], num[12];
    sh_utoa((uint64_t)items, num);
    line[0] = 0; kstrlcat(line, num, sizeof(line)); kstrlcat(line, items == 1 ? " item" : " items", sizeof(line));
    text_l(cx + 14, my, line, FONT_REGULAR, 12, COL_TEXT_DIM);
    if (st->sel) {
        char r[96]; r[0] = 0;
        kstrlcat(r, st->sel->name, sizeof(r));
        if (!st->sel->is_dir) {
            char sz[12]; sh_human(st->sel->size, sz);
            kstrlcat(r, "  " TXT_BULLET "  ", sizeof(r)); kstrlcat(r, sz, sizeof(r));
        }
        text_r(cx + cw - 14, my, r, FONT_REGULAR, 12, COL_TEXT_DIM);
    }
}

/* ---- top-level draw ----------------------------------------------------- */
/* context menu, drawn last so it floats above the listing */
static void draw_menu(files_t *st, int cx, int cy) {
    if (!st->menu_open) return;
    mrow rows[8]; int nr = build_menu(st, rows, 8);
    int mx = cx + st->menu_x, my = cy + st->menu_y, mh = nr * MENU_RH + 10;
    gfx_shadow(mx, my, MENU_W, mh, 10, 22, 8, 0, THEME_DARK ? 120 : 70);
    gfx_rrect(mx, my, MENU_W, mh, 10, THEME_DARK ? gfx_shade(COL_PANEL_2, 110) : 0xFFFFFF, 255);
    gfx_rrect_stroke(mx, my, MENU_W, mh, 10, 1, 0x000000, THEME_DARK ? 90 : 30);
    if (THEME_DARK) gfx_rrect_stroke2(mx + 1, my + 1, MENU_W - 2, mh - 2, 9, 0xFFFFFF, 24, 0);
    for (int i = 0; i < nr; i++) {
        int ry = my + 5 + i * MENU_RH;
        int hot = hover(mx, ry, MENU_W, MENU_RH);
        if (hot) gfx_rrect(mx + 5, ry, MENU_W - 10, MENU_RH, 6, rows[i].action == A_DEL ? COL_BAD : COL_ACCENT, 255);
        text_l(mx + 16, ry + MENU_RH / 2, rows[i].label, FONT_REGULAR, 13,
               hot ? 0xFFFFFF : (rows[i].action == A_DEL ? COL_BAD : COL_TEXT));
    }
}

static void files_draw(window_t *w, int cx, int cy, int cw, int ch) {
    files_t *st = &F;
    st->cw = cw; st->ch = ch;
    if (!st->dir) st->dir = fs_lookup("/home");
    if (!st->dir) st->dir = fs_root();

    gfx_fill(cx, cy, cw, ch, COL_PANEL);
    st->nbtn = 0;
    draw_toolbar(st, w, cx, cy, cw);
    draw_nav(st, cx, cy, ch);
    if (st->view) draw_viewer(st, w, cx, cy, cw, ch);
    else          draw_listing(st, w, cx, cy, cw, ch);
    draw_status(st, cx, cy, cw, ch);
    draw_menu(st, cx, cy);
    if (tip_action > 0 && tip_action < (int)(sizeof(TIP) / sizeof(TIP[0])) && !st->menu_open) {
        int tw = font_width(FONT_REGULAR, 12, TIP[tip_action], -1) + 18;
        int tx = tip_x; if (tx + tw > cx + cw - 6) tx = cx + cw - 6 - tw;
        gfx_shadow(tx, tip_y, tw, 26, 7, 12, 4, 0, 90);
        gfx_rrect(tx, tip_y, tw, 26, 7, THEME_DARK ? 0x3A3A40 : 0xFFFFFF, 255);
        gfx_rrect_stroke(tx, tip_y, tw, 26, 7, 1, 0x000000, THEME_DARK ? 90 : 30);
        text_l(tx + 9, tip_y + 13, TIP[tip_action], FONT_REGULAR, 12, COL_TEXT);
    }
}

/* ---- input -------------------------------------------------------------- */
static void clamp_scroll(files_t *st) {
    int CH = st->ch - TOOLBAR_H - STATUS_H;
    int maxs = st->content_h - CH; if (maxs < 0) maxs = 0;
    if (st->scroll < 0) st->scroll = 0;
    if (st->scroll > maxs) st->scroll = maxs;
}
static void clamp_view(files_t *st) {
    int CH = st->ch - TOOLBAR_H - STATUS_H - 36;
    int maxs = st->view_h - CH; if (maxs < 0) maxs = 0;
    if (st->view_scroll < 0) st->view_scroll = 0;
    if (st->view_scroll > maxs) st->view_scroll = maxs;
}

static void files_key(window_t *w, char c) {
    (void)w;
    files_t *st = &F;

    if (st->renaming) {
        if      (c == '\n') commit_rename(st);
        else if (c == 27)   { st->renaming = 0; st->rnode = 0; }
        else if (c == '\b') { if (st->rlen > 0) st->rbuf[--st->rlen] = 0; }
        else if ((unsigned char)c >= 32 && c != '/' && st->rlen < FS_NAME_MAX - 1) {
            st->rbuf[st->rlen++] = c; st->rbuf[st->rlen] = 0;
        }
        return;
    }

    if (st->view) {                                  /* viewer scrolling */
        int page = (st->ch - TOOLBAR_H - STATUS_H - 36) - 24;
        if      (c == 27)  st->view = 0;
        else if (c == 'j') st->view_scroll += 32;
        else if (c == 'k') st->view_scroll -= 32;
        else if (c == ' ') st->view_scroll += page;
        clamp_view(st);
        return;
    }

    int page = (st->ch - TOOLBAR_H - STATUS_H) - CELL_H;
    if      (c == 'j') st->scroll += 32;
    else if (c == 'k') st->scroll -= 32;
    else if (c == ' ') st->scroll += page;
    else if (c == '\b') go_up(st);                   /* Backspace = Up */
    else if ((unsigned char)c == KEY_COPY)  copy_sel(st);
    else if ((unsigned char)c == KEY_CUT)   cut_sel(st);
    else if ((unsigned char)c == KEY_PASTE) paste_into(st);
    else if ((unsigned char)c == KEY_DEL)   delete_sel(st);
    else if (c == '\n' && st->sel) open_item(st, st->sel);
    clamp_scroll(st);
}

static void files_scroll(window_t *w, int delta) {
    (void)w;
    files_t *st = &F;
    if (st->view) { st->view_scroll += delta * 40; clamp_view(st); }
    else          { st->scroll      += delta * 40; clamp_scroll(st); }
}

/* right-click: select the item under the cursor (if any) and pop the context
 * menu there. Right-clicking empty space gives the New-folder/Paste menu. */
static void files_rclick(window_t *w, int lx, int ly) {
    (void)w;
    files_t *st = &F;
    if (st->renaming || st->view) return;
    fs_node *n = item_at(st, lx, ly);
    if (n) st->sel = n; else st->sel = 0;
    /* keep the menu fully inside the client area */
    mrow rows[8]; int nr = build_menu(st, rows, 8);
    int mx = lx, my = ly;
    if (mx + MENU_W > st->cw) mx = st->cw - MENU_W;
    if (my + nr * MENU_RH + 10 > st->ch) my = st->ch - nr * MENU_RH - 10;
    if (mx < 0) mx = 0; if (my < 0) my = 0;
    st->menu_x = mx; st->menu_y = my; st->menu_open = 1;
}

/* run a toolbar / context-menu action by id */
static void do_action(files_t *st, int action) {
    switch (action) {
    case A_BACK: go_back(st); break;
    case A_FWD:  go_fwd(st);  break;
    case A_UP:   go_up(st);   break;
    case A_NEW:  new_folder(st); break;
    case A_NEWTXT: new_file(st, "New document", ".txt"); break;
    case A_NEWCSV: new_file(st, "New sheet", ".csv");    break;
    case A_REN:  begin_rename(st, st->sel); break;
    case A_DEL:  delete_sel(st); break;
    case A_COPY: copy_sel(st); break;
    case A_CUT:  cut_sel(st);  break;
    case A_PASTE: paste_into(st); break;
    case A_OPEN: if (st->sel) open_item(st, st->sel); break;
    case A_VIEWCLOSE: st->view = 0; break;
    }
}

static void files_click(window_t *w, int lx, int ly) {
    (void)w;
    files_t *st = &F;

    if (st->renaming) { commit_rename(st); return; }   /* clicking away commits */

    /* an open context menu eats the next click: run the hit row, else close */
    if (st->menu_open) {
        mrow rows[8]; int nr = build_menu(st, rows, 8);
        int hit = -1;
        if (lx >= st->menu_x && lx < st->menu_x + MENU_W) {
            int rel = ly - st->menu_y;
            rel -= 5;
            if (rel >= 0 && rel < nr * MENU_RH) hit = rel / MENU_RH;
        }
        st->menu_open = 0;
        if (hit >= 0) {
            if (rows[hit].action == A_OPEN && st->sel) open_item(st, st->sel);
            else do_action(st, rows[hit].action);
        }
        return;
    }

    for (int i = 0; i < st->nbtn; i++) {
        brect *b = &st->btns[i];
        if (lx < b->x || lx >= b->x + b->w || ly < b->y || ly >= b->y + b->h) continue;
        do_action(st, b->action);
        return;
    }
    for (int i = 0; i < st->ncrumb; i++) {
        irect *r = &st->crumbs[i];
        if (lx >= r->x && lx < r->x + r->w && ly >= r->y && ly < r->y + r->h) { go(st, r->node, 1); return; }
    }
    for (int i = 0; i < st->nnav; i++) {
        nrect *r = &st->navs[i];
        if (lx >= r->x && lx < r->x + r->w && ly >= r->y && ly < r->y + r->h) { go(st, r->dir, 1); return; }
    }

    if (st->view) return;                              /* viewer body: nothing else */

    for (int i = 0; i < st->nitems; i++) {
        irect *r = &st->items[i];
        if (lx < r->x || lx >= r->x + r->w || ly < r->y || ly >= r->y + r->h) continue;
        uint64_t now = pit_ticks();
        if (st->last_node == r->node && now - st->last_click < DCLICK_MS) {
            st->last_node = 0; open_item(st, r->node);
        } else {
            st->sel = r->node; st->last_node = r->node; st->last_click = now;
            gui_begin_item_drag(r->node, r->node->name, icon_for(r->node));   /* allow drag to desktop */
        }
        return;
    }

    /* scrollbar in the content area -> jump */
    int CW = st->cw - NAV_W, CH = st->ch - TOOLBAR_H - STATUS_H;
    if (lx >= NAV_W + CW - SBW && ly >= TOOLBAR_H && ly < TOOLBAR_H + CH) {
        int maxs = st->content_h - CH; if (maxs < 0) maxs = 0;
        int rel = ly - TOOLBAR_H; if (rel < 0) rel = 0; if (rel > CH) rel = CH;
        st->scroll = maxs * rel / (CH ? CH : 1);
        return;
    }

    st->sel = 0;                                       /* empty area: deselect */
}

/* ---- filesystem seed (a homely tree so the explorer isn't bare) --------- */
static void seed_dir(const char *p)  { if (!fs_lookup(p)) fs_create(p, 1); }
static void seed_file(const char *p, const char *text) {
    if (fs_lookup(p)) return;
    fs_node *f = fs_create(p, 0);
    if (f) fs_write(f, text, (uint32_t)strlen(text));
}
/* Generate a small 24-bit BMP gradient so the Pictures folder has real content
 * for the Photos viewer. BMP is bottom-up BGR with rows padded to 4 bytes. */
static void seed_sample_bmp(const char *path) {
    if (fs_lookup(path)) return;
    enum { IW = 192, IH = 128, ROW = IW * 3, PAD = (4 - (ROW & 3)) & 3, STRIDE = ROW + PAD };
    static uint8_t bmp[54 + STRIDE * IH];
    uint32_t fsz = sizeof(bmp);
    memset(bmp, 0, sizeof(bmp));
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2] = (uint8_t)fsz; bmp[3] = (uint8_t)(fsz >> 8); bmp[4] = (uint8_t)(fsz >> 16); bmp[5] = (uint8_t)(fsz >> 24);
    bmp[10] = 54;                                   /* pixel data offset */
    bmp[14] = 40;                                   /* DIB header size   */
    bmp[18] = (uint8_t)IW; bmp[19] = (uint8_t)(IW >> 8);
    bmp[22] = (uint8_t)IH; bmp[23] = (uint8_t)(IH >> 8);
    bmp[26] = 1;                                    /* planes */
    bmp[28] = 24;                                   /* bpp    */
    for (int y = 0; y < IH; y++) {
        uint8_t *row = bmp + 54 + (IH - 1 - y) * STRIDE;   /* bottom-up */
        for (int x = 0; x < IW; x++) {
            uint8_t r = (uint8_t)(x * 255 / (IW - 1));
            uint8_t g = (uint8_t)(y * 255 / (IH - 1));
            uint8_t b = (uint8_t)(255 - x * 255 / (IW - 1));
            row[x * 3 + 0] = b; row[x * 3 + 1] = g; row[x * 3 + 2] = r;
        }
    }
    fs_node *f = fs_create(path, 0);
    if (f) fs_write(f, bmp, sizeof(bmp));
}

static void files_seed(void) {
    seed_dir("/home/Desktop");
    seed_dir("/home/Documents");
    seed_dir("/home/Downloads");
    seed_dir("/home/Pictures");
    seed_dir("/home/Music");
    seed_file("/home/Desktop/welcome.txt",
              "Welcome to BoltOS File Explorer!\n\n"
              "- Double-click a folder to open it.\n"
              "- Double-click a file to view it.\n"
              "- Drag any item onto the desktop to drop a shortcut.\n"
              "- New folder / Rename / Delete live in the toolbar.\n");
    seed_file("/home/Documents/notes.txt",
              "Project notes\n=============\n\n"
              "The explorer is drawn entirely with the GUI vector primitives;\n"
              "folders, files and the recycle bin have hand-drawn icons.\n");
    seed_file("/home/Documents/todo.txt",
              "[ ] polish the breadcrumb\n[x] inline rename\n[x] drag to desktop\n");
    seed_file("/home/Downloads/boltos-guide.txt",
              "BoltOS quick guide\n\nType 'help' in the Terminal for the full command list.\n");
    seed_file("/home/Pictures/readme.txt",
              "Double-click an image (PNG/JPG/GIF/BMP) to open it in Photos.\n");
    seed_sample_bmp("/home/Pictures/welcome.bmp");
    trash_dir();
}

/* open a node from elsewhere (e.g. a desktop-icon double-click) */
/* Open a node the way a double-click in the explorer does: a known document
 * type goes to its app, anything else to the explorer's own viewer. The Start
 * menu's file results come through here rather than carrying a second copy of
 * the association table that would drift from this one. */
void files_launch(fs_node *n) {
    if (!n || !FW) return;
    F.view = 0;
    open_item(&F, n);
    if (n->is_dir || F.view) gui_open(FW);
}

void files_open_node(fs_node *n) {
    if (!n || !FW) return;
    if (n->is_dir) go(&F, n, 1);
    else { F.view = n; F.view_scroll = 0; }
    gui_open(FW);
}

void files_app_init(void) {
    memset(&F, 0, sizeof(F));
    files_seed();

    window_t *w = gui_add_window("File Explorer", 820, 540, 0x3A8DDE, ICON_FILES);
    if (!w) return;
    FW = w;
    w->track_hover = 1;
    w->draw = files_draw;
    w->key  = files_key;
    w->click = files_click;
    w->rclick = files_rclick;
    w->scroll = files_scroll;
    w->min_w = 540; w->min_h = 340;
    w->x = 120; w->y = 90;

    F.dir = fs_lookup("/home");
    if (!F.dir) F.dir = fs_root();

    /* seed the desktop with a few shortcuts so drag-to-desktop has company */
    gui_desktop_add(fs_lookup("/"),                 "This PC",     ICON_PC);
    gui_desktop_add(fs_lookup("/home"),             "Home",        ICON_HOME);
    gui_desktop_add(fs_lookup("/home/Documents"),   "Documents",   ICON_DOCS);
    gui_desktop_add(trash_dir(),                    "Recycle Bin", ICON_TRASH);
}
