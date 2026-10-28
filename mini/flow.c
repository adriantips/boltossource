/* ===========================================================================
 *  BoltOS Mini  -  mini/flow.c
 *
 *  Page layout (see flow.h). Block boxes stack; their inline content --
 *  text, inline elements, images, form controls, <br> -- is cut into words
 *  and packed into line boxes, each line aligned and set on one baseline.
 *  A block inside inline content ends the line and is laid out in place.
 *
 *  Tables measure each column's narrowest and widest content and share
 *  the width between them, as browsers do; a flex row places its children
 *  side by side and wraps them; a grid places them in its columns. Floats
 *  and positioning are not in the cascade this sits on, so those boxes
 *  simply flow. Elements a page hides for screen readers stay hidden.
 * ===========================================================================*/
#include <stdint.h>
#include "flow.h"
#include "font.h"
#include "kheap.h"
#include "string.h"

#define INK      0x202124
#define RULE     0xDADCE0

typedef struct { const char *href; uint8_t mono, pre; } ictx;

static flow_t *F;
static flow_img_fn IMG;
static void *UD;

int flow_px(int fscale) { return fscale >= 3 ? 26 : fscale == 2 ? 20 : 15; }

static fl_item *push(int kind) {
    if (F->n == F->cap) {
        int cap = F->cap ? F->cap * 2 : 4096;
        fl_item *n = kmalloc((uint64_t)cap * sizeof(fl_item));
        if (!n) return 0;
        if (F->it) { memcpy(n, F->it, (size_t)F->n * sizeof(fl_item)); kfree(F->it); }
        F->it = n; F->cap = cap;
    }
    fl_item *it = &F->it[F->n++];
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t)kind; it->img = -1;
    return it;
}

static const char *tag_of(layout_box *b) { return b->node && b->node->tag ? b->node->tag : ""; }

static int hidden(layout_box *b) {
    dom_node *n = b->node;
    if (!n || b->is_text) return 0;
    const char *v = dom_attr_get(n, "aria-hidden");
    if (v && !strcmp(v, "true")) return 1;
    if (dom_attr_get(n, "hidden")) return 1;
    const char *cl = dom_attr_get(n, "class");
    if (cl && (strstr(cl, "sr-only") || strstr(cl, "visually-hidden") || strstr(cl, "visuallyhidden") ||
               strstr(cl, "screen-reader") || strstr(cl, "skip-link") || strstr(cl, "skip-to") ||
               strstr(cl, "a11y-hidden"))) return 1;
    return 0;
}

static int is_block(layout_box *b) {
    if (b->is_text) return 0;
    int d = b->st.display;
    const char *t = tag_of(b);
    if (!strcmp(t, "br") || !strcmp(t, "img")) return 0;
    return d == DISP_BLOCK || d == DISP_LIST_ITEM || d == DISP_FLEX || d == DISP_GRID;
}

static int face_of(const computed_style *s, int mono) {
    if (mono) return s->bold ? FONT_MONO_BOLD : FONT_MONO;
    return s->bold ? (s->fscale > 1 ? FONT_BOLD : FONT_SEMIBOLD) : FONT_REGULAR;
}
static uint32_t color_of(const computed_style *s) { return (s->color & 0x1000000) ? (s->color & 0xFFFFFF) : INK; }

/* ---- line boxes --------------------------------------------------------------------- */
typedef struct {
    int x0, w, y;          /* the line's left, width, top                              */
    int pen, space;        /* where the next atom goes; whitespace is pending          */
    int start;             /* the current line's first item                            */
    int asc, desc;         /* its metrics so far                                       */
    int align, lines, first_base, prev_mb;
} IL;

static void il_init(IL *L, int x, int y, int w, int align) {
    memset(L, 0, sizeof *L);
    L->x0 = x; L->y = y; L->w = w < 16 ? 16 : w; L->align = align;
    L->start = F->n; L->first_base = -1;
}

static void il_break(IL *L, int force, int px) {
    if (L->pen == 0 && F->n == L->start && !force) return;
    if (L->asc == 0 && L->desc == 0) { L->asc = px * 12 / 10 + 2; L->desc = px * 4 / 10 + 2; }
    int base = L->y + L->asc;
    int dx = L->align == TA_CENTER ? (L->w - L->pen) / 2 : L->align == TA_RIGHT ? L->w - L->pen : 0;
    if (dx < 0) dx = 0;
    for (int i = L->start; i < F->n; i++) {
        fl_item *it = &F->it[i];
        it->x += dx;
        it->y = base - it->a;
    }
    if (L->first_base < 0) L->first_base = base;
    L->y = base + L->desc;
    L->pen = 0; L->space = 0; L->asc = L->desc = 0;
    L->start = F->n; L->lines++; L->prev_mb = 0;
}

static void il_atom(IL *L, fl_item *it, int asc, int desc) {
    it->a = asc;
    if (asc > L->asc) L->asc = asc;
    if (desc > L->desc) L->desc = desc;
}

static void il_word(IL *L, const char *s, int len, const computed_style *st, ictx c) {
    int face = face_of(st, c.mono), px = flow_px(st->fscale);
    int lead = px * 4 / 10;
    int asc = font_ascent(face, px) + lead / 2, desc = font_descent(face, px) + lead - lead / 2;
    int sp = (L->space && L->pen > 0) ? font_width(face, px, " ", 1) : 0;
    while (len > 0) {
        int ww = font_width(face, px, s, len);
        if (L->pen > 0 && L->pen + sp + ww > L->w) { il_break(L, 0, px); sp = 0; }
        int n = len;
        if (ww > L->w) {                                   /* longer than a line: cut it */
            n = font_fit(face, px, s, len, L->w - L->pen);
            if (n < 1) n = 1;
            ww = font_width(face, px, s, n);
        }
        L->pen += sp;
        fl_item *it = push(FL_TEXT);
        if (!it) return;
        it->face = (uint8_t)face; it->px = (uint8_t)px; it->color = color_of(st);
        it->flags = (uint8_t)((st->italic ? FLF_ITALIC : 0) | (c.href ? FLF_LINK : 0));
        it->x = L->x0 + L->pen; it->w = ww; it->h = asc + desc;
        it->s = s; it->len = n; it->href = c.href;
        il_atom(L, it, asc, desc);
        L->pen += ww;
        s += n; len -= n; sp = 0;
        if (len > 0) il_break(L, 0, px);
    }
    L->space = 0;
}

static void text_words(IL *L, layout_box *b, ictx c) {
    const char *p = b->text;
    if (!p) return;
    if (c.pre) {                                         /* whitespace kept, lines kept */
        while (*p) {
            const char *e = p;
            while (*e && *e != '\n') e++;
            if (e > p) { L->space = 0; il_word(L, p, (int)(e - p), &b->st, c); }
            if (*e == '\n') { il_break(L, 1, flow_px(b->st.fscale)); e++; }
            p = e;
        }
        return;
    }
    while (*p) {
        if (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') { L->space = 1; p++; continue; }
        const char *w = p;
        while (*p && *p != ' ' && *p != '\n' && *p != '\t' && *p != '\r') p++;
        il_word(L, w, (int)(p - w), &b->st, c);
        if (*p) L->space = 1;
    }
}

static int attr_num(dom_node *n, const char *name, int def) {
    const char *v = dom_attr_get(n, name);
    if (!v || *v < '0' || *v > '9') return def;
    int r = 0;
    while (*v >= '0' && *v <= '9') r = r * 10 + (*v++ - '0');
    return r;
}

static void atom_box(IL *L, int kind, int w, int h, dom_node *node, ictx c, int slot) {
    if (L->pen > 0 && L->pen + (L->space ? 4 : 0) + w > L->w) il_break(L, 0, 15);
    if (L->space && L->pen > 0) L->pen += 4;
    fl_item *it = push(kind);
    if (!it) return;
    it->x = L->x0 + L->pen; it->w = w; it->h = h;
    it->node = node; it->href = c.href; it->img = slot;
    il_atom(L, it, kind == FL_CONTROL ? h - 7 : h, kind == FL_CONTROL ? 7 : 2);
    L->pen += w;
    L->space = 0;
}

static void control(IL *L, layout_box *b, ictx c) {
    dom_node *n = b->node;
    const char *t = tag_of(b), *type = dom_attr_get(n, "type");
    int w = b->st.width > 0 ? b->st.width : 0, h = 30;
    char lbl[120] = "";
    if (!strcmp(t, "button") || (type && (!strcmp(type, "submit") || !strcmp(type, "button") || !strcmp(type, "reset")))) {
        if (!strcmp(t, "button")) dom_inner_text(n, lbl, sizeof lbl);
        else { const char *v = dom_attr_get(n, "value"); if (v) { int k = 0; while (v[k] && k < 119) { lbl[k] = v[k]; k++; } lbl[k] = 0; } }
        if (!lbl[0]) { lbl[0] = 'G'; lbl[1] = 'o'; lbl[2] = 0; }
        if (!w || w > 300) w = font_width(FONT_MEDIUM, 14, lbl, -1) + 28;
    } else if (!strcmp(t, "textarea")) { if (!w) w = 320; h = 70; }
    else if (!strcmp(t, "select")) { if (!w) w = 180; }
    else { if (!w) w = 240; int sz = attr_num(n, "size", 0); if (sz > 0 && sz < 80) w = sz * 9 + 16; }
    if (w > L->w) w = L->w;
    atom_box(L, FL_CONTROL, w, h, n, c, -1);
}

static void image(IL *L, layout_box *b, ictx c) {
    int w = 0, h = 0, slot = -1;
    if (IMG) IMG(b->node, L->w, &w, &h, &slot, UD);
    if (w > L->w) { h = h * L->w / w; w = L->w; }
    atom_box(L, FL_IMAGE, w, h, b->node, c, slot);
}

/* ---- measuring, for tables and flex rows ------------------------------------------------- */
static void measure(layout_box *b, ictx c, int *mn, int *mx, int depth);

static void measure_kids(layout_box *b, ictx c, int *mn, int *mx, int depth) {
    int line = 0, best = 0, least = 0;
    for (layout_box *k = b->first_child; k; k = k->next) {
        if (hidden(k)) continue;
        int a, z;
        measure(k, c, &a, &z, depth + 1);
        if (a > least) least = a;
        if (is_block(k)) { if (line > best) best = line; line = 0; if (z > best) best = z; }
        else line += z;
    }
    if (line > best) best = line;
    *mn = least; *mx = best;
}

static void measure(layout_box *b, ictx c, int *mn, int *mx, int depth) {
    *mn = *mx = 0;
    if (depth > 40) return;
    if (b->is_text) {
        int face = face_of(&b->st, c.mono), px = flow_px(b->st.fscale), sp = font_width(face, px, " ", 1);
        int total = 0, longest = 0;
        for (const char *p = b->text; p && *p; ) {
            while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
            const char *w = p;
            while (*p && *p != ' ' && *p != '\n' && *p != '\t' && *p != '\r') p++;
            if (p == w) break;
            int ww = font_width(face, px, w, (int)(p - w));
            if (ww > longest) longest = ww;
            total += ww + sp;
        }
        *mn = longest > 200 ? 200 : longest; *mx = total;
        return;
    }
    const char *t = tag_of(b);
    const computed_style *s = &b->st;
    int pads = s->padding[1] + s->padding[3] + s->border[1] + s->border[3] + s->margin[1] + s->margin[3];
    if (!strcmp(t, "img")) {
        int w = 0, h = 0, slot;
        if (IMG) IMG(b->node, 2000, &w, &h, &slot, UD);
        *mn = *mx = w + pads;
        return;
    }
    if (!strcmp(t, "input") || !strcmp(t, "button") || !strcmp(t, "select") || !strcmp(t, "textarea")) {
        *mn = *mx = (s->width > 0 ? s->width : 160) + pads;
        return;
    }
    if (!strcmp(t, "pre") || !strcmp(t, "code")) c.mono = 1;
    if (!strcmp(t, "table") || !strcmp(t, "tr") || !strcmp(t, "tbody") || !strcmp(t, "thead")) {
        /* a row's cells sit side by side */
        int least = 0, best = 0;
        for (layout_box *k = b->first_child; k; k = k->next) {
            int a, z;
            measure(k, c, &a, &z, depth + 1);
            if (!strcmp(t, "tr")) { least += a; best += z; }
            else { if (a > least) least = a; if (z > best) best = z; }
        }
        *mn = least + pads; *mx = best + pads;
        return;
    }
    measure_kids(b, c, mn, mx, depth);
    if (s->width > 0) { *mn = *mx = s->width; }
    *mn += pads; *mx += pads;
}

/* ---- blocks ---------------------------------------------------------------------------------- */
static int lay_block(layout_box *b, int x, int y, int w, ictx c, int *first_base);
static void flow_node(IL *L, layout_box *k, ictx c);

static void lay_inline_children(IL *L, layout_box *b, ictx c) {
    for (layout_box *k = b->first_child; k; k = k->next) flow_node(L, k, c);
}

static int lay_children(layout_box *b, int x, int y, int w, int align, ictx c, int *first_base) {
    IL L;
    il_init(&L, x, y, w, align);
    lay_inline_children(&L, b, c);
    il_break(&L, 0, 15);
    if (first_base) *first_base = L.first_base;
    return L.y - y;
}

/* Lay out any box (a block, or inline content on its own) at x, y in width w. */
static int lay_any(layout_box *k, int x, int y, int w, ictx c) {
    if (is_block(k)) return k->st.margin[0] + lay_block(k, x, y + k->st.margin[0], w, c, 0) + k->st.margin[2];
    IL L;
    il_init(&L, x, y, w, TA_LEFT);
    flow_node(&L, k, c);
    il_break(&L, 0, 15);
    return L.y - y;
}

static void flow_node(IL *L, layout_box *k, ictx c) {
    if (k->is_text) { text_words(L, k, c); return; }
    if (hidden(k)) return;
    const char *t = tag_of(k);
    if (!strcmp(t, "img")) { image(L, k, c); return; }
    if (!strcmp(t, "input") || !strcmp(t, "button") || !strcmp(t, "select") || !strcmp(t, "textarea")) { control(L, k, c); return; }
    if (!strcmp(t, "br")) { il_break(L, 1, flow_px(k->st.fscale)); return; }
    ictx cc = c;
    if (!strcmp(t, "a")) { const char *h = dom_attr_get(k->node, "href"); if (h && h[0] && strncmp(h, "javascript:", 11)) cc.href = h; }
    if (!strcmp(t, "code") || !strcmp(t, "kbd") || !strcmp(t, "samp") || !strcmp(t, "tt")) cc.mono = 1;
    if (!is_block(k)) {
        int padl = k->st.padding[3] + k->st.margin[3], padr = k->st.padding[1] + k->st.margin[1];
        if (padl > 0 && padl < 40) L->pen += padl;
        lay_inline_children(L, k, cc);
        if (padr > 0 && padr < 40) L->pen += padr;
        return;
    }
    /* a block in the flow: end the line, place it, carry on below it */
    il_break(L, 0, 15);
    int mt = k->st.margin[0], mb = k->st.margin[2];
    if (mt < 0) mt = 0;
    if (mb < 0) mb = 0;
    int top = L->y + (mt > L->prev_mb ? mt - L->prev_mb : 0);
    int h = lay_block(k, L->x0, top, L->w, cc, 0);
    L->y = top + h + mb;
    L->prev_mb = mb;
    L->space = 0;
    L->start = F->n;                   /* what the block drew is placed already */
}

static void collect_rows(layout_box *t, layout_box **rows, int *n, int cap) {
    for (layout_box *k = t->first_child; k && *n < cap; k = k->next) {
        const char *g = tag_of(k);
        if (!strcmp(g, "tr")) rows[(*n)++] = k;
        else if (!strcmp(g, "thead") || !strcmp(g, "tbody") || !strcmp(g, "tfoot")) collect_rows(k, rows, n, cap);
    }
}

#define MAXCOL 24
static int lay_table(layout_box *t, int x, int y, int w, ictx c) {
    static layout_box *rows[1500];
    int nr = 0;
    collect_rows(t, rows, &nr, 1500);
    if (!nr) return lay_children(t, x, y, w, TA_LEFT, c, 0);
    int ncol = 0, cmin[MAXCOL] = { 0 }, cmax[MAXCOL] = { 0 };
    for (int r = 0; r < nr; r++) {
        int i = 0;
        for (layout_box *k = rows[r]->first_child; k && i < MAXCOL; k = k->next) {
            if (k->is_text || hidden(k)) continue;
            int a, z;
            measure(k, c, &a, &z, 0);
            int span = attr_num(k->node, "colspan", 1);
            if (span <= 1) { if (a > cmin[i]) cmin[i] = a; if (z > cmax[i]) cmax[i] = z; }
            i += span < 1 ? 1 : span;
        }
        if (i > ncol) ncol = i < MAXCOL ? i : MAXCOL;
    }
    if (!ncol) return 0;
    int gap = attr_num(t->node, "cellspacing", 2), pad = attr_num(t->node, "cellpadding", 2);
    if (gap > 12) gap = 12;
    if (pad > 12) pad = 12;
    int avail = w - gap * (ncol - 1), smin = 0, smax = 0;
    for (int i = 0; i < ncol; i++) { cmin[i] += 2 * pad; cmax[i] += 2 * pad; if (cmax[i] < cmin[i]) cmax[i] = cmin[i]; smin += cmin[i]; smax += cmax[i]; }
    int cw[MAXCOL];
    const char *tw = dom_attr_get(t->node, "width");
    int target = smax;
    if (tw && strchr(tw, '%')) target = avail * attr_num(t->node, "width", 100) / 100;
    else if (t->st.width > 0) target = t->st.width;
    if (target > avail) target = avail;
    if (target < smin) target = smin < avail ? smin : avail;
    for (int i = 0; i < ncol; i++) {
        if (smax <= target) cw[i] = cmax[i] + (smax ? (int)((long)(target - smax) * cmax[i] / smax) : 0);
        else if (smin >= target) cw[i] = smin ? (int)((long)target * cmin[i] / smin) : target / ncol;
        else cw[i] = cmin[i] + (int)((long)(target - smin) * (cmax[i] - cmin[i]) / (smax - smin ? smax - smin : 1));
        if (cw[i] < 8) cw[i] = 8;
    }
    int yy = y;
    for (int r = 0; r < nr; r++) {
        layout_box *row = rows[r];
        int bgi = -1;
        uint32_t rbg = (row->st.bg & 0x1000000) ? row->st.bg : 0;
        const char *ba = dom_attr_get(row->node, "bgcolor");
        if (!rbg && ba) rbg = css_color(ba);
        if (rbg) { fl_item *it = push(FL_RECT); if (it) { it->x = x; it->y = yy; it->w = w; it->color = rbg & 0xFFFFFF; bgi = F->n - 1; } }
        int xx = x, i = 0, rh = 0;
        for (layout_box *k = row->first_child; k && i < ncol; k = k->next) {
            if (k->is_text || hidden(k)) continue;
            int span = attr_num(k->node, "colspan", 1), cwid = 0;
            if (span < 1) span = 1;
            for (int s = 0; s < span && i + s < ncol; s++) cwid += cw[i + s] + (s ? gap : 0);
            int h = lay_block(k, xx + pad, yy + pad, cwid - 2 * pad, c, 0) + 2 * pad;
            if (h > rh) rh = h;
            xx += cwid + gap;
            i += span;
        }
        if (bgi >= 0) F->it[bgi].h = rh;
        yy += rh + gap;
    }
    return yy - y;
}

static int lay_flexrow(layout_box *b, int x, int y, int w, ictx c) {
    int gap = b->st.gap > 0 ? b->st.gap : 8, px = 0, rowy = y, rowh = 0;
    for (layout_box *k = b->first_child; k; k = k->next) {
        if (hidden(k)) continue;
        if (k->is_text) { const char *p = k->text; while (p && (*p == ' ' || *p == '\n' || *p == '\t')) p++; if (!p || !*p) continue; }
        int mn, mx;
        measure(k, c, &mn, &mx, 0);
        int cw = mx + 1;
        if (k->st.flex_grow > 0 && !k->next) cw = w - px;             /* the last one takes the rest */
        if (cw > w) cw = w;
        if (cw < mn) cw = mn < w ? mn : w;
        if (px > 0 && px + cw > w) { rowy += rowh + gap; px = 0; rowh = 0; }
        int h = lay_any(k, x + px, rowy, cw, c);
        if (h > rowh) rowh = h;
        px += cw + gap;
    }
    return rowy + rowh - y;
}

static int lay_grid(layout_box *b, int x, int y, int w, ictx c) {
    int n = b->st.ngcols, gap = b->st.gap > 0 ? b->st.gap : 8, fixed = 0, fr = 0, cw[GRID_MAX_COLS];
    for (int i = 0; i < n; i++) { if (b->st.gcols[i] >= 0) fixed += b->st.gcols[i]; else fr += -b->st.gcols[i]; }
    int rest = w - fixed - gap * (n - 1);
    for (int i = 0; i < n; i++) cw[i] = b->st.gcols[i] >= 0 ? b->st.gcols[i] : (fr ? rest * -b->st.gcols[i] / fr : 0);
    int col = 0, xx = x, rowy = y, rowh = 0;
    for (layout_box *k = b->first_child; k; k = k->next) {
        if (hidden(k)) continue;
        if (k->is_text) { const char *p = k->text; while (p && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) p++; if (!p || !*p) continue; }
        int h = lay_any(k, xx, rowy, cw[col] > 20 ? cw[col] : 20, c);
        if (h > rowh) rowh = h;
        xx += cw[col] + gap;
        if (++col == n) { col = 0; xx = x; rowy += rowh + gap; rowh = 0; }
    }
    return rowy + rowh - y;
}

static int list_index(layout_box *li) {
    int i = 1;
    for (layout_box *p = li->parent ? li->parent->first_child : 0; p && p != li; p = p->next)
        if (!strcmp(tag_of(p), "li")) i++;
    return i;
}

/* A block at x, y (its border box's top, margins already taken), in width w. Returns its height. */
static int lay_block(layout_box *b, int x, int y, int w, ictx c, int *first_base) {
    const computed_style *s = &b->st;
    const char *t = tag_of(b);
    int ml = s->margin[3] > 0 && s->margin[3] < w / 2 ? s->margin[3] : 0;
    int mr = s->margin[1] > 0 && s->margin[1] < w / 2 ? s->margin[1] : 0;
    int bl = s->border[3], br = s->border[1], bt = s->border[0], bb = s->border[2];
    int pl = s->padding[3], pr = s->padding[1], pt = s->padding[0], pb = s->padding[2];
    if (pl > w / 4) pl = w / 4;
    if (pr > w / 4) pr = w / 4;
    int bx = x + ml, bw = w - ml - mr;
    int pads = bl + br + pl + pr;
    if (s->width > 0 && s->width + pads < bw && strcmp(t, "td") && strcmp(t, "th")) bw = s->width + pads;
    else if (s->width <= -1000 && s->width > -1100) bw = bw * (-s->width - 1000) / 100;   /* n% */
    uint32_t bg = (s->bg & 0x1000000) ? s->bg : 0;
    const char *ba = b->node ? dom_attr_get(b->node, "bgcolor") : 0;
    if (!bg && ba) bg = css_color(ba);
    int bgi = -1;
    if (bg && b->parent) {
        fl_item *it = push(FL_RECT);
        if (it) { it->x = bx; it->y = y; it->w = bw; it->color = bg & 0xFFFFFF; bgi = F->n - 1; }
    }
    int cx = bx + bl + pl, cw = bw - pads, cy = y + bt + pt;
    if (cw < 16) cw = 16;
    if (!strcmp(t, "pre")) { c.pre = 1; c.mono = 1; }
    int h, fb = -1;
    if (!strcmp(t, "hr")) {
        fl_item *it = push(FL_RECT);
        if (it) { it->x = cx; it->y = cy + 8; it->w = cw; it->h = 1; it->color = RULE; }
        h = 17;
    } else if (!strcmp(t, "table")) h = lay_table(b, cx, cy, cw, c);
    else if (s->display == DISP_FLEX && s->flex_dir != FLEX_COLUMN) h = lay_flexrow(b, cx, cy, cw, c);
    else if (s->display == DISP_GRID && s->ngcols >= 2) h = lay_grid(b, cx, cy, cw, c);
    else h = lay_children(b, cx, cy, cw, s->talign, c, &fb);
    if (s->display == DISP_LIST_ITEM && fb >= 0) {
        const char *pt2 = b->parent ? tag_of(b->parent) : "";
        int flexy = b->parent && (b->parent->st.display == DISP_FLEX || b->parent->st.display == DISP_GRID);
        if (!flexy && pl + ml + (b->parent ? b->parent->st.padding[3] : 0) >= 12) {
            fl_item *it = push(FL_BULLET);
            if (it) {
                it->color = color_of(s);
                it->y = fb; it->x = cx - 16; it->w = 16;
                if (!strcmp(pt2, "ol")) {
                    int v = list_index(b), o = 0;
                    char d[8]; int nd = 0;
                    do { d[nd++] = (char)('0' + v % 10); v /= 10; } while (v && nd < 5);
                    while (nd) it->num[o++] = d[--nd];
                    it->num[o++] = '.'; it->num[o] = 0;
                    it->len = o; it->x = cx - 8 - font_width(FONT_REGULAR, 15, it->num, o);
                }
            }
        }
    }
    if (first_base) *first_base = fb;
    if (s->height > 0 && s->height > h && (!strcmp(t, "div") || !strcmp(t, "td"))) h = s->height < h + 400 ? s->height : h;
    int total = bt + pt + h + pb + bb;
    if (bgi >= 0) F->it[bgi].h = total;
    if (bt || bb || bl || br) {
        fl_item *e;
        if (bt && (e = push(FL_RECT))) { e->x = bx; e->y = y; e->w = bw; e->h = 1; e->color = RULE; }
        if (bb && (e = push(FL_RECT))) { e->x = bx; e->y = y + total - 1; e->w = bw; e->h = 1; e->color = RULE; }
        if (bl && (e = push(FL_RECT))) { e->x = bx; e->y = y; e->w = 1; e->h = total; e->color = RULE; }
        if (br && (e = push(FL_RECT))) { e->x = bx + bw - 1; e->y = y; e->w = 1; e->h = total; e->color = RULE; }
    }
    return total;
}

int flow_layout(flow_t *f, layout_box *root, int width, flow_img_fn img, void *ud) {
    F = f; IMG = img; UD = ud;
    f->n = 0; f->doc_h = 0; f->paper = 0xFFFFFF;
    if (!root) return 0;
    /* The page's colour: <body>'s background, else <html>'s. */
    for (layout_box *b = root; b; ) {
        if (b->st.bg & 0x1000000) f->paper = b->st.bg & 0xFFFFFF;
        layout_box *next = 0;
        for (layout_box *k = b->first_child; k; k = k->next)
            if (!strcmp(tag_of(k), "html") || !strcmp(tag_of(k), "body")) { next = k; break; }
        b = next;
    }
    ictx c = { 0, 0, 0 };
    f->doc_h = lay_children(root, 0, 0, width, TA_LEFT, c, 0);
    return f->n;
}

void flow_free(flow_t *f) {
    if (f->it) kfree(f->it);
    f->it = 0; f->n = f->cap = 0; f->doc_h = 0;
}
