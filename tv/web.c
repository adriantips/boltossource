/* ===========================================================================
 *  BoltOS TV  -  tv/web.c
 *
 *  A web browser for a sofa three metres from the screen.
 *
 *  Every page is shown the way a reader view shows it: one column of text
 *  at the size a television's own menus use, headings that look like
 *  headings, pictures between the paragraphs, and links in blue. That is a
 *  deliberate choice for the distance, not a limitation worked around -- a
 *  three-column news site laid out at desktop size is unreadable from a
 *  sofa, and the same page as a column of 28-pixel text is not. The tree
 *  comes from the kernel's own parsers: BoltDOM first, so the ad blocker can
 *  take ad slots out by id and class, then the HTML run parser (kernel/html.c)
 *  over what is left, which is what gets laid out here.
 *
 *  A remote has no pointer, so the page is navigated link by link: Down and
 *  Up step to the next and previous link and scroll to keep it in view,
 *  Channel +/- page through, OK follows. Up from the top of a page reaches
 *  the toolbar. A mouse or air-remote works too.
 *
 *  Nothing here waits on the network. The page, its stylesheets and its
 *  pictures are fetched by tv/netsvc.c's thread, one at a time, while this
 *  screen draws a spinner and the film behind it keeps playing; the ad
 *  blocker sees every one of those requests first.
 * ===========================================================================*/
#include <stdint.h>
#include "tv.h"
#include "dom.h"
#include "html.h"
#include "image.h"
#include "kheap.h"
#include "string.h"
#include "stdio.h"
#include "kprintf.h"

#define HTML_CAP   (1536 * 1024)
#define IMG_CAP    (2 * 1024 * 1024)
#define MAXIMG     24
#define MAXLINK    1024
#define MAXHIST    24
#define MAXITEMS   40000

typedef struct { int x, y, w, h; int href; } link_t;

typedef struct {
    char        src[512];                  /* resolved URL                    */
    image_t    *im;                        /* premultiplied once decoded      */
    int         tried;
} img_t;

/* One thing to draw: a word, a picture, a rule or a form field. y is the top
 * of its line on the page (canvas pixels from the top of the document), and
 * `base` is how far down that line its baseline sits. */
enum { IK_WORD, IK_IMG, IK_RULE, IK_FIELD };
typedef struct {
    int         x, y, w, h, base;
    const char *s; int len;
    int         link;                      /* index into doc->hrefs, or -1   */
    uint32_t    color;
    uint8_t     kind, face, italic, px;
    int16_t     img;
} item_t;

static struct {
    char          url[1024];
    char          title[160];
    html_doc     *doc;
    item_t       *items; int nitems, cap;
    img_t         imgs[MAXIMG]; int nimg;
    link_t        links[MAXLINK]; int nlink;
    int           doc_h;
    int           hidden;                  /* ad slots removed                */
    int           blocked;                 /* requests refused on this page   */
    int           bytes;
} pg;

static char hist[MAXHIST][1024];
static int  nhist;

/* loading */
enum { L_IDLE, L_PAGE, L_IMAGES };
static int      lstage;
static fetch_t *job;
static int      redirects;
static char     pending_url[1024];
static int      img_i;
static char     status[160];
static uint64_t relayout_at;

/* view state */
static int   start_page = 1;               /* the tiles, not a web page       */
static int   area;                         /* 0 page, 1 toolbar               */
static int   tb;                           /* toolbar focus: 0 back 1 reload 2 address 3 home */
static int   lfocus = -1, tile = 0;
static float scroll, scroll_to;
static uint64_t last_tick;
static glasscache_t gc_bar, gc_osk, gc_tiles;

/* address editing */
static int  editing;
static char edit[512];
static int  osk_r, osk_c;
static int  last_typed;                    /* the last key was a typed one: Enter means Go */


/* ---- URLs ---------------------------------------------------------------- */
static int is_remote(const char *u) { return !strncmp(u, "http://", 7) || !strncmp(u, "https://", 8); }

static void url_origin(const char *url, char *out, int cap) {
    const char *h = strstr(url, "://");
    int n = h ? (int)(h - url) + 3 : 0;
    while (url[n] && url[n] != '/' && url[n] != '?' && url[n] != '#') n++;
    if (n >= cap) n = cap - 1;
    memcpy(out, url, (size_t)n);
    out[n] = 0;
}

static void resolve(const char *href, char *out, int cap) {
    while (*href == ' ' || *href == '\n' || *href == '\t') href++;
    if (is_remote(href)) { snprintf(out, (size_t)cap, "%s", href); return; }
    if (href[0] == '/' && href[1] == '/') {
        snprintf(out, (size_t)cap, "%s%s", strncmp(pg.url, "https", 5) ? "http:" : "https:", href);
        return;
    }
    char origin[512];
    url_origin(pg.url, origin, sizeof origin);
    if (href[0] == '/') { snprintf(out, (size_t)cap, "%s%s", origin, href); return; }
    if (href[0] == '#' || !href[0]) { snprintf(out, (size_t)cap, "%s", pg.url); return; }
    /* relative to the current directory */
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", pg.url);
    char *q = strchr(dir + strlen(origin), '?'); if (q) *q = 0;
    char *slash = strrchr(dir + strlen(origin), '/');
    if (slash) slash[1] = 0; else snprintf(dir, sizeof dir, "%s/", origin);
    snprintf(out, (size_t)cap, "%s%s", dir, href);
}

static int hexv(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* DuckDuckGo's result links bounce through /l/?uddg=<target>, answered with
 * a script redirect a browser without JavaScript cannot follow. */
static int unwrap_ddg(const char *url, char *out, int cap) {
    if (!strstr(url, "duckduckgo.com/l/")) return 0;
    const char *q = strstr(url, "uddg=");
    if (!q) return 0;
    q += 5;
    int o = 0;
    while (*q && *q != '&' && o < cap - 1) {
        if (*q == '%' && hexv(q[1]) >= 0 && hexv(q[2]) >= 0) { out[o++] = (char)(hexv(q[1]) * 16 + hexv(q[2])); q += 3; }
        else out[o++] = *q++;
    }
    out[o] = 0;
    return o > 0;
}

static void search_url(const char *q, char *out, int cap) {
    static const char hex[] = "0123456789ABCDEF";
    int o = snprintf(out, (size_t)cap, "https://html.duckduckgo.com/html/?q=");
    for (const unsigned char *p = (const unsigned char *)q; *p && o + 4 < cap; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '.' || *p == '_') out[o++] = (char)*p;
        else if (*p == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15]; }
    }
    out[o] = 0;
}

/* What was typed into the address bar: a URL, a bare host, or a search. */
static void typed_to_url(const char *t, char *out, int cap) {
    while (*t == ' ') t++;
    if (is_remote(t)) { snprintf(out, (size_t)cap, "%s", t); return; }
    int dot = 0, space = 0;
    for (const char *p = t; *p; p++) { if (*p == '.') dot = 1; if (*p == ' ') space = 1; }
    if (dot && !space) snprintf(out, (size_t)cap, "https://%s", t);
    else search_url(t, out, cap);
}

/* ---- page lifetime ------------------------------------------------------- */
static void free_page(void) {
    if (pg.doc) html_free(pg.doc);
    if (pg.items) kfree(pg.items);
    for (int i = 0; i < pg.nimg; i++) if (pg.imgs[i].im) image_free(pg.imgs[i].im);
    memset(&pg, 0, sizeof pg);
    lfocus = -1;
    scroll = scroll_to = 0;
}

static void cancel_job(void) {
    if (job) { netsvc_cancel(job); job = 0; }
}

static int start_fetch(const char *url, uint32_t cap) {
    cancel_job();
    fetch_t *f = (fetch_t *)kmalloc(sizeof *f);
    char *b = (char *)kmalloc(cap + 1);
    if (!f || !b) { if (f) kfree(f); if (b) kfree(b); return -1; }
    memset(f, 0, sizeof *f);
    snprintf(f->url, sizeof f->url, "%s", url);
    f->buf = b;
    f->cap = cap;
    if (netsvc_fetch(f) != 0) { kfree(b); kfree(f); return -1; }
    job = f;
    return 0;
}

static void navigate(const char *url, int push) {
    char u[1024];
    if (unwrap_ddg(url, u, sizeof u)) url = u;
    if (push && pg.url[0] && !start_page) {
        if (nhist == MAXHIST) { memmove(hist[0], hist[1], sizeof hist[0] * (MAXHIST - 1)); nhist--; }
        snprintf(hist[nhist++], sizeof hist[0], "%s", pg.url);
    }
    snprintf(pending_url, sizeof pending_url, "%s", url);
    redirects = 0;
    start_page = 0;
    area = 0;
    if (start_fetch(url, HTML_CAP) == 0) {
        lstage = L_PAGE;
        snprintf(status, sizeof status, "Loading\x85");
    } else {
        lstage = L_IDLE;
        snprintf(status, sizeof status, "Could not start loading that page.");
    }
    tv_redraw();
}

void web_open(const char *url) { navigate(url, 1); }

/* ---- ad slots ------------------------------------------------------------ */
/* Take the ad slots out of the tree, so the space they held closes up; and
 * the things this renderer has no use for, so they are not parsed twice. */
/* A first pass over the raw bytes, before any parser sees them. Real pages
 * put kilobytes of JSON in single-quoted data-* attributes, '>' included
 * (Wikipedia's data-mw), and a tokenizer that ends a tag at the first '>'
 * turns the rest of the page into text. This walks tags properly -- quotes
 * respected -- and writes back only the attributes the reader can use,
 * double-quoted and escaped, with comments, scripts and styles gone. The
 * page usually comes out a third of the size, which the two parsers after
 * it are grateful for. */
static int keep_attr(const char *n, int len) {
    static const char *const KEEP[] = { "href", "src", "alt", "width", "height", "value", "placeholder",
                                        "type", "name", "id", "class", "role", "colspan" };
    for (unsigned i = 0; i < sizeof KEEP / sizeof KEEP[0]; i++)
        if ((int)strlen(KEEP[i]) == len && !strncasecmp(n, KEEP[i], (size_t)len)) return 1;
    return 0;
}

static uint32_t sanitize(const char *in, uint32_t n, char *out) {
    uint32_t i = 0, o = 0;
    while (i < n) {
        if (in[i] != '<') { out[o++] = in[i++]; continue; }
        if (i + 3 < n && !strncmp(in + i, "<!--", 4)) {            /* comment */
            const char *e = memmem(in + i + 4, n - i - 4, "-->", 3);
            i = e ? (uint32_t)(e - in) + 3 : n;
            continue;
        }
        int raw = (i + 7 < n && !strncasecmp(in + i, "<script", 7)) ? 1 :
                  (i + 6 < n && !strncasecmp(in + i, "<style", 6)) ? 2 : 0;
        if (raw) {                                                   /* drop element and body */
            const char *close = raw == 1 ? "</script" : "</style";
            uint32_t j = i + 1;
            while (j + 8 <= n && strncasecmp(in + j, close, strlen(close))) j++;
            while (j < n && in[j] != '>') j++;
            i = j < n ? j + 1 : n;
            continue;
        }
        /* a tag: its name, then attributes one at a time */
        out[o++] = in[i++];
        while (i < n && in[i] != '>' && in[i] != ' ' && in[i] != '\t' && in[i] != '\n' && in[i] != '\r' &&
               !(in[i] == '/' && i + 1 < n && in[i + 1] == '>'))
            out[o++] = in[i++];
        for (;;) {
            while (i < n && (in[i] == ' ' || in[i] == '\t' || in[i] == '\n' || in[i] == '\r')) i++;
            if (i >= n) break;
            if (in[i] == '>') { out[o++] = in[i++]; break; }
            if (in[i] == '/') { i++; continue; }
            uint32_t ns = i;
            while (i < n && in[i] != '=' && in[i] != '>' && in[i] != ' ' && in[i] != '\t' &&
                   in[i] != '\n' && in[i] != '\r' && in[i] != '/') i++;
            uint32_t nl = i - ns;
            while (i < n && (in[i] == ' ' || in[i] == '\t' || in[i] == '\n' || in[i] == '\r')) i++;
            uint32_t vs = 0, vl = 0;
            int has = 0;
            if (i < n && in[i] == '=') {
                has = 1;
                i++;
                while (i < n && (in[i] == ' ' || in[i] == '\t' || in[i] == '\n' || in[i] == '\r')) i++;
                if (i < n && (in[i] == '"' || in[i] == '\'')) {
                    char q = in[i++];
                    vs = i;
                    while (i < n && in[i] != q) i++;
                    vl = i - vs;
                    if (i < n) i++;
                } else {
                    vs = i;
                    while (i < n && in[i] != '>' && in[i] != ' ' && in[i] != '\t' && in[i] != '\n' && in[i] != '\r') i++;
                    vl = i - vs;
                }
            }
            if (nl == 0) { if (i < n && in[i] != '>') i++; continue; }
            if (!keep_attr(in + ns, (int)nl)) continue;
            out[o++] = ' ';
            memcpy(out + o, in + ns, nl); o += nl;
            if (has) {
                out[o++] = '='; out[o++] = '"';
                for (uint32_t k = 0; k < vl; k++) {
                    char c = in[vs + k];
                    out[o++] = (c == '"' || c == '<' || c == '>') ? '\'' : c;
                }
                out[o++] = '"';
            }
        }
    }
    out[o] = 0;
    return o;
}

/* Only the attributes the reader uses survive to the second parse. Pages
 * carry kilobytes of JSON in data-* attributes, quotes and all, and the
 * serializer writes values as they came -- one stray quote and the rest of
 * the page parses as attribute soup. */
static void trim_attrs(dom_node *e) {
    static const char *const KEEP[] = { "href", "src", "alt", "width", "height", "value", "placeholder",
                                        "type", "name", "colspan" };
    dom_attr **pp = &e->attrs;
    while (*pp) {
        dom_attr *a = *pp;
        int keep = 0;
        for (unsigned i = 0; i < sizeof KEEP / sizeof KEEP[0] && !keep; i++) keep = a->name && !strcmp(a->name, KEEP[i]);
        if (keep && a->value) for (char *v = a->value; *v; v++) if (*v == '"' || *v == '<' || *v == '>') *v = '\'';
        if (keep) pp = &a->next;
        else *pp = a->next;                   /* bump-allocated: nothing to free */
    }
}

static int prune(dom_node *n) {
    int removed = 0;
    for (dom_node *c = n->first_child; c; ) {
        dom_node *next = c->next;
        if (c->type == DOM_ELEMENT && c->tag) {
            if (tvs.adblock && adblock_element_hidden(c->tag, dom_attr_get(c, "id"), dom_attr_get(c, "class"))) {
                dom_remove(c);
                removed++;
            } else if (!strcmp(c->tag, "nav") || !strcmp(c->tag, "aside") ||
                       (dom_attr_get(c, "role") && (!strcmp(dom_attr_get(c, "role"), "navigation") ||
                                                    !strcmp(dom_attr_get(c, "role"), "complementary")))) {
                dom_remove(c);                 /* menus and sidebars: not what anyone sat down to read */
            } else if (!strcmp(c->tag, "script") || !strcmp(c->tag, "style") || !strcmp(c->tag, "iframe") ||
                       !strcmp(c->tag, "svg") || !strcmp(c->tag, "template") || !strcmp(c->tag, "canvas") ||
                       !strcmp(c->tag, "video") || !strcmp(c->tag, "audio") || !strcmp(c->tag, "object") ||
                       !strcmp(c->tag, "embed") || !strcmp(c->tag, "noscript")) {
                dom_remove(c);
            } else { removed += prune(c); trim_attrs(c); }
        }
        c = next;
    }
    return removed;
}

/* The part of the page that is the page: <main>, or role="main", or the one
 * <article> if there is exactly one, or an element whose id says so. A page
 * with none of those is shown whole. */
static dom_node *find_tag(dom_node *n, const char *tag, int *count, dom_node **first) {
    for (dom_node *c = n->first_child; c; c = c->next) {
        if (c->type != DOM_ELEMENT || !c->tag) continue;
        if (!strcmp(c->tag, tag)) { if (!*first) *first = c; (*count)++; }
        find_tag(c, tag, count, first);
    }
    return *first;
}
static dom_node *find_attr(dom_node *n, const char *attr, const char *val) {
    for (dom_node *c = n->first_child; c; c = c->next) {
        if (c->type != DOM_ELEMENT) continue;
        const char *v = dom_attr_get(c, attr);
        if (v && !strcmp(v, val)) return c;
        dom_node *r = find_attr(c, attr, val);
        if (r) return r;
    }
    return 0;
}
static dom_node *main_content(dom_document *d) {
    int n = 0; dom_node *first = 0;
    if (find_tag(d->root, "main", &n, &first)) return first;
    dom_node *m = find_attr(d->root, "role", "main");
    if (m) return m;
    n = 0; first = 0;
    find_tag(d->root, "article", &n, &first);
    if (n == 1) return first;
    static const char *const IDS[] = { "content", "main", "main-content", "maincontent", "article" };
    for (unsigned i = 0; i < sizeof IDS / sizeof IDS[0]; i++)
        if ((m = find_attr(d->root, "id", IDS[i]))) return m;
    return 0;
}

/* ---- reader layout ------------------------------------------------------- */
static int content_w(void);
static int content_x(void);

static item_t *push_item(void) {
    if (pg.nitems >= pg.cap) {
        if (pg.cap >= MAXITEMS) return 0;
        int nc = pg.cap ? pg.cap * 2 : 2048;
        if (nc > MAXITEMS) nc = MAXITEMS;
        item_t *n = (item_t *)kmalloc((uint64_t)nc * sizeof(item_t));
        if (!n) return 0;
        if (pg.items) { memcpy(n, pg.items, (uint64_t)pg.nitems * sizeof(item_t)); kfree(pg.items); }
        pg.items = n;
        pg.cap = nc;
    }
    item_t *it = &pg.items[pg.nitems++];
    memset(it, 0, sizeof *it);
    it->link = -1;
    it->img = -1;
    return it;
}

typedef struct { int face, px, italic; uint32_t color; int gap_before; } rstyle_t;

static rstyle_t style_of(const html_run *r) {
    rstyle_t s = { FONT_REGULAR, S(28), 0, 0x1D1D1F, 0 };
    switch (r->style) {
    case HSTYLE_H1:     s.face = FONT_BOLD;     s.px = S(52); s.gap_before = S(18); break;
    case HSTYLE_H2:     s.face = FONT_SEMIBOLD; s.px = S(42); s.gap_before = S(14); break;
    case HSTYLE_H3:     s.face = FONT_SEMIBOLD; s.px = S(34); s.gap_before = S(8);  break;
    case HSTYLE_BOLD:   s.face = FONT_SEMIBOLD; break;
    case HSTYLE_ITALIC: s.italic = 1; break;
    case HSTYLE_PRE: case HSTYLE_CODE: s.face = FONT_MONO; s.px = S(24); s.color = 0x2B2F36; break;
    }
    if (r->style == HSTYLE_NORMAL && r->fscale >= 3) { s.face = FONT_SEMIBOLD; s.px = S(40); }
    else if (r->style == HSTYLE_NORMAL && r->fscale == 2) { s.face = FONT_SEMIBOLD; s.px = S(33); }
    if (r->link >= 0) { s.color = 0x0B63CE; if (s.face == FONT_REGULAR) s.face = FONT_MEDIUM; }
    return s;
}

/* Lines are built a word at a time and settled when they end: every item on
 * a line shares its top, and each sits on the line's deepest baseline. */
static int L_y, L_x, L_start, L_asc, L_desc, L_x0;

static void line_end(int min_h) {
    int h = L_asc + L_desc;
    if (pg.nitems > L_start) {
        for (int i = L_start; i < pg.nitems; i++) {
            item_t *it = &pg.items[i];
            if (it->kind == IK_WORD) { it->y = L_y; it->base = L_asc; it->h = h; }
        }
        L_y += h + h * 45 / 100 - h * 25 / 100;      /* leading: ~1.2 line height */
    } else if (min_h) L_y += min_h;
    L_start = pg.nitems;
    L_x = L_x0;
    L_asc = L_desc = 0;
}

static void reflow(void) {
    pg.nitems = 0;
    pg.nlink = 0;
    if (!pg.doc) return;
    int cw = content_w(), x0 = content_x();
    L_y = S(60); L_x0 = L_x = x0; L_start = 0; L_asc = L_desc = 0;
    int indent = 0;
    for (int ri = 0; ri < pg.doc->nruns; ri++) {
        const html_run *r = &pg.doc->runs[ri];
        rstyle_t st = style_of(r);
        int ind = r->indent * S(44);
        if (r->brk) {
            int para = r->brk >= 2;
            line_end(para ? 0 : S(8));
            if (para && pg.nitems) L_y += S(18) + st.gap_before;
            indent = ind;
            L_x0 = L_x = x0 + indent;
        }
        if (r->kind == HRUN_IMG) {
            line_end(0);
            img_t *im = 0;
            for (int i = 0; i < pg.nimg; i++) {
                char u[512];
                if (r->img < 0 || r->img >= pg.doc->nimgs) break;
                resolve(pg.doc->imgs[r->img], u, sizeof u);
                if (!strcmp(pg.imgs[i].src, u)) { im = &pg.imgs[i]; break; }
            }
            int w = 0, h = 0;
            if (im && im->im) {
                w = im->im->w * 2; h = im->im->h * 2;     /* CSS pixels look right doubled at 1080p */
                if (w > cw) { h = h * cw / w; w = cw; }
                if (h > S(900)) { w = w * S(900) / h; h = S(900); }
            } else if (r->iw > 0 && r->ih > 0 && (!im || !im->tried)) {
                w = r->iw * 2; h = r->ih * 2;
                if (w > cw) { h = h * cw / w; w = cw; }
            }
            if (w > S(24) && h > S(24)) {
                item_t *it = push_item();
                if (!it) break;
                it->kind = IK_IMG; it->x = x0 + (cw - w) / 2; it->y = L_y; it->w = w; it->h = h;
                it->img = im ? (int16_t)(im - pg.imgs) : -1;
                it->link = r->link;
                L_y += h + S(24);
                L_start = pg.nitems;
            }
            continue;
        }
        if (r->kind == HRUN_INPUT) {
            if (!r->text || strlen(r->text) < 3 || r->text[0] == '[') continue;   /* hidden fields, checkboxes */
            int fw = r->img == 2 ? cw / 2 : S(420), fh = S(56);
            if (L_x + fw > x0 + cw) line_end(0);
            item_t *it = push_item();
            if (!it) break;
            it->kind = IK_FIELD; it->x = L_x; it->w = fw; it->h = fh; it->s = r->text; it->len = (int)strlen(r->text);
            int asc = fh * 3 / 4, desc = fh / 4;
            if (asc > L_asc) L_asc = asc;
            if (desc > L_desc) L_desc = desc;
            L_x += fw + S(16);
            continue;
        }
        const char *t = r->text;
        if (!t) continue;
        /* html.c's renderings of <hr> and <li>, shown as a rule and a bullet */
        if (!strncmp(t, "----------------", 16)) {
            line_end(0);
            item_t *it = push_item();
            if (!it) break;
            it->kind = IK_RULE; it->x = x0; it->y = L_y + S(10); it->w = cw; it->h = 1;
            L_y += S(30);
            L_start = pg.nitems;
            continue;
        }
        int asc = font_ascent(st.face, st.px), desc = font_descent(st.face, st.px);
        int sp = font_width(st.face, st.px, " ", 1);
        if (!strcmp(t, "- ")) t = "\x95 ";
        for (const char *p = t; *p; ) {
            int lead = 0;
            while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') { lead = 1; p++; }
            if (!*p) break;
            const char *w = p;
            int wl = 0;
            while (*p && *p != ' ' && *p != '\n' && *p != '\t' && *p != '\r') { p++; wl++; }
            int ww = font_width(st.face, st.px, w, wl);
            /* A space between runs even where the markup had none: real pages
             * lean on CSS for that gap ("<a>user</a><span>3 hours ago</span>"),
             * and a word run into the next is worse than an extra space --
             * except against punctuation that hugs, "(" before, ")" after. */
            int boundary = w == t, glue = 0;
            if (boundary && pg.nitems > L_start && pg.items[pg.nitems - 1].kind == IK_WORD) {
                const item_t *pv = &pg.items[pg.nitems - 1];
                glue = strchr("([{/\"'$#@", pv->s[pv->len - 1]) || strchr(")]},.;:!?%'\"", w[0]);
            }
            int gap = (L_x > L_x0 && (lead || (boundary && !glue))) ? sp : 0;
            if (L_x > L_x0 && L_x + gap + ww > x0 + cw) { line_end(0); gap = 0; }
            if (ww > cw - indent) ww = cw - indent;       /* one enormous word: clip it */
            item_t *it = push_item();
            if (!it) goto done;
            L_x += gap;
            it->kind = IK_WORD; it->x = L_x; it->w = ww; it->s = w; it->len = wl;
            it->face = (uint8_t)st.face; it->px = (uint8_t)(st.px > 255 ? 255 : st.px);
            it->italic = (uint8_t)st.italic; it->color = st.color; it->link = r->link;
            if (asc > L_asc) L_asc = asc;
            if (desc > L_desc) L_desc = desc;
            L_x += ww;
        }
    }
done:
    line_end(0);
    pg.doc_h = L_y + S(80);
    /* links: consecutive items of one link on one line make one target */
    for (int i = 0; i < pg.nitems && pg.nlink < MAXLINK; i++) {
        item_t *it = &pg.items[i];
        if (it->link < 0) continue;
        link_t *l = pg.nlink ? &pg.links[pg.nlink - 1] : 0;
        if (l && l->href == it->link && l->y == it->y && it->x - (l->x + l->w) < S(30)) {
            l->w = it->x + it->w - l->x;
            if (it->h > l->h) l->h = it->h;
            continue;
        }
        pg.links[pg.nlink++] = (link_t){ it->x, it->y, it->w, it->h, it->link };
    }
    if (lfocus >= pg.nlink) lfocus = pg.nlink - 1;
}

static const char *link_href(int i) {
    if (!pg.doc || i < 0 || i >= pg.nlink) return 0;
    int h = pg.links[i].href;
    return (h >= 0 && h < pg.doc->nhrefs) ? pg.doc->hrefs[h] : 0;
}

static void page_arrived(const char *body, int len, const char *url) {
    free_page();
    snprintf(pg.url, sizeof pg.url, "%s", url);
    pg.bytes = len;
    const char *dot = strrchr(url, '.');
    int text = dot && (!strcmp(dot, ".txt") || !strcmp(dot, ".md"));
    if (text) {
        pg.doc = html_parse_text(body, (uint32_t)len);
    } else {
        /* DOM first, for the ad blocker; then the run parser over what is left */
        char *san = (char *)kmalloc((uint64_t)len + 1);
        if (san) { len = (int)sanitize(body, (uint32_t)len, san); body = san; }
        dom_document *dom = dom_parse(body, (uint32_t)len);
        char *clean = dom ? (char *)kmalloc((uint64_t)len + 65536) : 0;
        if (dom && clean) {
            pg.hidden = prune(dom->root);
            adblock_note_hidden(pg.hidden);
            /* keep the title, which lives outside <main> */
            char title[160] = "";
            int tc = 0; dom_node *tn = 0;
            if (find_tag(dom->root, "title", &tc, &tn)) dom_inner_text(tn, title, sizeof title);
            dom_node *body = main_content(dom);
            uint32_t cap = (uint32_t)len + 65536;
            int o = snprintf(clean, cap, "<html><head><title>%s</title></head><body>", title);
            o += dom_inner_html(body ? body : dom->root, clean + o, cap - (uint32_t)o - 32);
            o += snprintf(clean + o, cap - (uint32_t)o, "</body></html>");
            pg.doc = html_parse(clean, (uint32_t)o);
        } else {
            pg.doc = html_parse(body, (uint32_t)len);
        }
        if (clean) kfree(clean);
        if (dom) dom_free(dom);
        if (san) kfree(san);
    }
    if (!pg.doc) { snprintf(status, sizeof status, "That page could not be read."); return; }
    if (pg.doc->title) snprintf(pg.title, sizeof pg.title, "%s", pg.doc->title);
    for (int i = 0; i < pg.doc->nimgs && pg.nimg < MAXIMG; i++) {
        const char *src = pg.doc->imgs[i];
        if (!src || !src[0] || !strncmp(src, "data:", 5)) continue;
        char u[512];
        resolve(src, u, sizeof u);
        int dup = 0;
        for (int k = 0; k < pg.nimg; k++) if (!strcmp(pg.imgs[k].src, u)) dup = 1;
        if (!dup) snprintf(pg.imgs[pg.nimg++].src, sizeof pg.imgs[0].src, "%s", u);
    }
    reflow();
    status[0] = 0;
    img_i = 0;
    lstage = pg.nimg ? L_IMAGES : L_IDLE;
    if (lstage == L_IMAGES) start_fetch(pg.imgs[0].src, IMG_CAP);
}

static void premultiply(image_t *im) {
    for (int i = 0; i < im->w * im->h; i++) {
        uint32_t c = im->px[i], a = c >> 24;
        if (a == 255) continue;
        im->px[i] = (a << 24) | ((((c >> 16) & 255) * a / 255) << 16) | ((((c >> 8) & 255) * a / 255) << 8) |
                    ((c & 255) * a / 255);
    }
}

/* Poll the job in flight and move the load along. */
static void load_tick(uint64_t now) {
    if (relayout_at && now >= relayout_at) {
        relayout_at = 0;
        reflow();
        tv_redraw();
    }
    if (!job || (job->state != FETCH_DONE && job->state != FETCH_FAILED)) return;
    fetch_t *j = job;
    job = 0;
    int ok = j->state == FETCH_DONE;
    if (lstage == L_PAGE) {
        if (ok && j->status >= 300 && j->status < 400 && j->location[0] && redirects < 6) {
            char u[1024];
            snprintf(pg.url, sizeof pg.url, "%s", j->url);   /* resolve against the old URL */
            resolve(j->location, u, sizeof u);
            redirects++;
            snprintf(pending_url, sizeof pending_url, "%s", u);
            netsvc_cancel(j);
            if (start_fetch(u, HTML_CAP) != 0) lstage = L_IDLE;
            return;
        }
        if (!ok) {
            const char *why = j->status == -3 ? "The address could not be found."
                            : j->status == -4 ? "The site did not answer."
                            : j->status == -5 ? "A secure connection could not be made."
                            : j->status == -6 ? "There is no network connection."
                            : j->status == -7 ? "The network is still starting. Try again in a moment."
                            : j->status == -8 ? "The Ad Blocker stopped this address."
                            : "That page could not be loaded.";
            snprintf(status, sizeof status, "%s", why);
            snprintf(pg.url, sizeof pg.url, "%s", pending_url);
            lstage = L_IDLE;
        } else {
            page_arrived(j->buf, j->len, pending_url);
        }
        netsvc_cancel(j);
        tv_redraw();
        return;
    }
    if (lstage == L_IMAGES) {
        img_t *im = &pg.imgs[img_i];
        im->tried = 1;
        if (ok && j->len > 0 && j->status < 300) {
            im->im = image_decode_upto((const uint8_t *)j->buf, (uint32_t)j->len, 1400);
            if (im->im) { premultiply(im->im); relayout_at = tv_now_ms() + 500; }
        } else if (j->status == -8) pg.blocked++;
        netsvc_cancel(j);
        if (++img_i < pg.nimg) start_fetch(pg.imgs[img_i].src, IMG_CAP);
        else { lstage = L_IDLE; if (!relayout_at) relayout_at = tv_now_ms() + 50; }
        tv_redraw();
    }
}

/* ---- geometry ------------------------------------------------------------ */
static int bar_y(void) { return S(36); }
static int bar_h(void) { return S(84); }
static int page_x(void) { return S(56); }
static int page_y(void) { return S(148); }
static int page_w(void) { return tv_w - S(112); }
static int page_h(void) { return tv_h - page_y() - S(40); }
static int doc_h(void)  { return pg.doc_h; }
static int content_w(void) { int w = page_w() - S(200); return w > S(1360) ? S(1360) : w; }
static int content_x(void) { return (page_w() - content_w()) / 2; }

static void clamp_scroll(void) {
    int maxs = doc_h() - page_h();
    if (maxs < 0) maxs = 0;
    if (scroll_to > maxs) scroll_to = (float)maxs;
    if (scroll_to < 0) scroll_to = 0;
}

static void link_into_view(int i) {
    if (i < 0 || i >= pg.nlink) return;
    int y = pg.links[i].y, h = pg.links[i].h;
    int top = (int)scroll_to, ph = page_h();
    if (y < top + ph / 6) scroll_to = (float)(y - ph / 3);
    else if (y + h > top + ph - ph / 6) scroll_to = (float)(y + h - ph * 2 / 3);
    clamp_scroll();
}

static int link_visible(int i, float s) {
    int y = pg.links[i].y;
    return y >= (int)s && y < (int)s + page_h();
}

/* ---- the start page ------------------------------------------------------ */
static const struct { const char *name, *what, *url; uint32_t c; } SITES[] = {
    { "Wikipedia",   "The free encyclopedia",           "https://en.m.wikipedia.org/wiki/Main_Page", 0x6E7681 },
    { "Hacker News", "What programmers are reading",    "https://news.ycombinator.com",              0xFF6600 },
    { "NPR",         "News, text only and fast",        "https://text.npr.org",                      0xD62021 },
    { "CNN Lite",    "Headlines without the weight",    "https://lite.cnn.com",                      0xCC0000 },
    { "DuckDuckGo",  "Search, privately",               "https://html.duckduckgo.com/html/",         0xDE5833 },
    { "68k.news",    "Today's news for small browsers", "http://68k.news",                           0x2F6FDE },
    { "Wiby",        "A search engine for the old web", "https://wiby.me",                           0x15A06B },
    { "BoltOS TV",   "How to put this on your TV",      "https://raw.githubusercontent.com/adriantips/boltossource/main/TV.md", 0x7C6CFF },
};
#define NSITES ((int)(sizeof SITES / sizeof SITES[0]))

/* ---- the on-screen keyboard ---------------------------------------------- */
static const char *const OSK[4] = { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm./:" };
#define OSK_SPECIAL 4                     /* row 4: .com, space, delete, go */

static void begin_edit(void) {
    editing = 1;
    last_typed = 0;
    snprintf(edit, sizeof edit, "%s", start_page ? "" : pg.url);
    osk_r = 1; osk_c = 0;
    tv_text_mode(1);
}
static void end_edit(int go) {
    editing = 0;
    tv_text_mode(0);
    if (go && edit[0]) {
        char u[1024];
        typed_to_url(edit, u, sizeof u);
        navigate(u, 1);
    }
}
static void edit_put(const char *s) {
    size_t n = strlen(edit);
    while (*s && n < sizeof edit - 1) edit[n++] = *s++;
    edit[n] = 0;
}
static void osk_press(void) {
    if (osk_r < OSK_SPECIAL) { char s[2] = { OSK[osk_r][osk_c], 0 }; edit_put(s); return; }
    switch (osk_c) {
    case 0: edit_put(".com"); break;
    case 1: edit_put(" "); break;
    case 2: { size_t n = strlen(edit); if (n) edit[n - 1] = 0; break; }
    case 3: end_edit(1); break;
    }
}

static void edit_key(tvkey_t k) {
    int typed = k.k == K_CHAR || k.k == K_ERASE;
    switch (k.k) {
    case K_CHAR: { char s[2] = { (char)k.ch, 0 }; edit_put(s); break; }
    case K_ERASE: { size_t n = strlen(edit); if (n) edit[n - 1] = 0; break; }
    /* OK on a remote types the highlighted key; Enter after typing on a
     * keyboard is the end of the address. They are the same key code, so
     * which one it was is told by what came before it. */
    case K_OK: if (last_typed) end_edit(1); else osk_press(); break;
    case K_BACK: end_edit(0); break;
    case K_UP:
        if (osk_r == OSK_SPECIAL) osk_c = osk_c == 0 ? 0 : osk_c == 1 ? 4 : osk_c == 2 ? 7 : 9;  /* back onto a key above */
        if (osk_r > 0) osk_r--;
        sfx_move();
        break;
    case K_DOWN:
        if (osk_r < OSK_SPECIAL) { osk_r++; if (osk_r == OSK_SPECIAL) osk_c = osk_c < 2 ? 0 : osk_c < 7 ? 1 : osk_c < 9 ? 2 : 3; }
        sfx_move();
        break;
    case K_LEFT:  if (osk_c > 0) osk_c--; sfx_move(); break;
    case K_RIGHT: if (osk_c < (osk_r == OSK_SPECIAL ? 3 : 9)) osk_c++; sfx_move(); break;
    }
    last_typed = typed;
}

/* ---- input --------------------------------------------------------------- */
static void go_back(void) {
    if (nhist > 0) {
        char u[1024];
        snprintf(u, sizeof u, "%s", hist[--nhist]);
        navigate(u, 0);
    } else if (!start_page) {
        cancel_job();
        free_page();
        start_page = 1;
        lstage = L_IDLE;
        status[0] = 0;
    } else {
        tv_pop();
    }
}

static void tb_activate(int i) {
    sfx_select();
    switch (i) {
    case 0: go_back(); break;
    case 1: if (!start_page && pg.url[0]) navigate(pg.url, 0); break;
    case 2: begin_edit(); break;
    case 3: cancel_job(); free_page(); start_page = 1; lstage = L_IDLE; status[0] = 0; nhist = 0; break;
    }
}

static void web_key(tvkey_t k) {
    if (editing) { edit_key(k); return; }
    if (k.k == K_CHAR) { begin_edit(); edit[0] = 0; edit_key(k); return; }   /* just start typing */
    if (area == 1) {
        switch (k.k) {
        case K_LEFT:  if (tb > 0) { tb--; sfx_move(); } break;
        case K_RIGHT: if (tb < 3) { tb++; sfx_move(); } break;
        case K_DOWN:  area = 0; sfx_move(); break;
        case K_OK:    tb_activate(tb); break;
        case K_BACK:  sfx_back(); go_back(); break;
        }
        return;
    }
    if (start_page) {
        int cols = 4;
        switch (k.k) {
        case K_LEFT:  if (tile % cols) { tile--; sfx_move(); } break;
        case K_RIGHT: if (tile % cols < cols - 1 && tile < NSITES - 1) { tile++; sfx_move(); } break;
        case K_UP:    if (tile >= cols) { tile -= cols; sfx_move(); } else { area = 1; tb = 2; sfx_move(); } break;
        case K_DOWN:  if (tile + cols < NSITES) { tile += cols; sfx_move(); } break;
        case K_OK:    sfx_select(); navigate(SITES[tile].url, 1); break;
        case K_BACK:  sfx_back(); tv_pop(); break;
        }
        return;
    }
    switch (k.k) {
    case K_DOWN: {
        int next = -1;
        for (int i = lfocus + 1; i < pg.nlink; i++) {
            int y = pg.links[i].y;
            if (y > (int)scroll_to + page_h() * 3 / 2) break;
            if (y >= (int)scroll_to - S(10)) { next = i; break; }
        }
        if (next >= 0) { lfocus = next; link_into_view(lfocus); }
        else { scroll_to += page_h() * 0.4f; clamp_scroll(); }
        sfx_move();
        break;
    }
    case K_UP: {
        int prev = -1;
        for (int i = (lfocus < 0 ? pg.nlink : lfocus) - 1; i >= 0; i--) {
            int y = pg.links[i].y;
            if (y < (int)scroll_to - page_h() / 2) break;
            if (y <= (int)scroll_to + page_h()) { prev = i; break; }
        }
        if (prev >= 0) { lfocus = prev; link_into_view(lfocus); }
        else if (scroll_to <= 0) { area = 1; tb = 2; }
        else { scroll_to -= page_h() * 0.4f; clamp_scroll(); }
        sfx_move();
        break;
    }
    case K_RIGHT: if (lfocus + 1 < pg.nlink) { lfocus++; link_into_view(lfocus); sfx_move(); } break;
    case K_LEFT:  if (lfocus > 0) { lfocus--; link_into_view(lfocus); sfx_move(); } break;
    case K_CHDN:  scroll_to += page_h() * 0.85f; clamp_scroll(); break;
    case K_CHUP:  scroll_to -= page_h() * 0.85f; clamp_scroll(); break;
    case K_OK:
        if (lfocus >= 0 && lfocus < pg.nlink && link_visible(lfocus, scroll_to) && link_href(lfocus)) {
            char u[1024];
            resolve(link_href(lfocus), u, sizeof u);
            sfx_select();
            navigate(u, 1);
        }
        break;
    case K_BACK: sfx_back(); go_back(); break;
    }
}

static void web_pointer(int x, int y, int click, int wheel) {
    if (wheel) { scroll_to -= wheel * S(140); clamp_scroll(); return; }
    if (y < page_y() - S(8)) {
        /* toolbar: back, reload, address, home */
        int bx = page_x();
        int xs[4] = { bx, bx + S(92), bx + S(184), tv_w - S(56) - S(84) };
        int ws[4] = { S(84), S(84), tv_w - S(56) - S(84) - S(16) - (bx + S(184)), S(84) };
        for (int i = 0; i < 4; i++)
            if (ui_hit(x, y, xs[i], bar_y(), ws[i], bar_h())) { area = 1; tb = i; if (click) tb_activate(i); }
        return;
    }
    if (start_page) {
        int gw = page_w(), cols = 4, cw = (gw - S(40) * 3) / cols, ch = S(200);
        int gy = page_y() + S(150);
        for (int i = 0; i < NSITES; i++) {
            int tx = page_x() + (i % cols) * (cw + S(40)), ty = gy + (i / cols) * (ch + S(40));
            if (ui_hit(x, y, tx, ty, cw, ch)) { area = 0; tile = i; if (click) { sfx_select(); navigate(SITES[i].url, 1); } }
        }
        return;
    }
    for (int i = 0; i < pg.nlink; i++) {
        link_t *l = &pg.links[i];
        int lx = page_x() + l->x, ly = page_y() + l->y - (int)scroll;
        if (ui_hit(x, y, lx, ly, l->w, l->h)) {
            area = 0; lfocus = i;
            if (click && link_href(i)) { char u[1024]; resolve(link_href(i), u, sizeof u); sfx_select(); navigate(u, 1); }
            return;
        }
    }
}

static void web_enter(void) { }

static void web_leave(void) {
    if (editing) end_edit(0);
}

static void web_tick(uint64_t now) {
    uint64_t dt = last_tick ? now - last_tick : 16;
    last_tick = now;
    load_tick(now);
    float ns = ui_ease(scroll, scroll_to, dt, 180);
    if (ns != scroll) { scroll = ns; tv_redraw(); }
    if (lstage != L_IDLE || tv_view_t() < 1.0f) tv_redraw();
}

/* ---- drawing ------------------------------------------------------------- */
static void tb_button(int x, int y, int s, int glyph, int on, int a, int enabled) {
    if (on) ui_focus(x, y, s, s, s / 2, a);
    art_glyph(glyph, x + s / 2, y + s / 2, S(32), on ? 0x101318 : 0xFFFFFF, enabled ? a : a / 3);
}

static void draw_toolbar(uint64_t now, int a) {
    int x = page_x(), y = bar_y(), h = bar_h(), w = page_w();
    ui_glass(&gc_bar, x, y, w, h, h / 2, 165, a);
    int b = h - S(16);
    tb_button(x + S(8), y + S(8), b, GL_BACK, area == 1 && tb == 0, a, 1);
    tb_button(x + S(8) + b + S(8), y + S(8), b, GL_RELOAD, area == 1 && tb == 1, a, !start_page);
    int fx = x + S(8) + 2 * (b + S(8)), fw = w - (fx - x) - b - S(24);
    int on = area == 1 && tb == 2;
    gfx_rrect(fx, y + S(10), fw, h - S(20), (h - S(20)) / 2, on ? 0xFFFFFF : 0x000000, on ? 235 : 60);
    uint32_t ink = on ? 0x101318 : 0xFFFFFF;
    const char *shown = editing ? edit : start_page ? "Search or type a web address" : pg.url;
    int secure = !start_page && !strncmp(pg.url, "https://", 8);
    art_glyph(start_page || editing ? GL_SEARCH : secure ? GL_LOCK : GL_GLOBE, fx + S(36), y + h / 2, S(26),
              ink, a * (start_page && !editing ? 170 : 230) / 255);
    const char *disp = shown;
    if (!editing && !start_page) { disp = strstr(shown, "://"); disp = disp ? disp + 3 : shown; }
    int tw = font_draw_fit(on || editing ? FONT_MEDIUM : FONT_REGULAR, S(25), fx + S(64), y + h / 2 + S(9), disp,
                           fw - S(64) - S(180), ink, start_page && !editing ? a * 160 / 255 : a);
    if (editing && (now / 500) % 2) gfx_fill(fx + S(66) + tw, y + h / 2 - S(15), S(3), S(30), ink);
    /* right end of the field: a spinner while loading, else the blocker's count */
    int rx = fx + fw - S(28);
    if (lstage != L_IDLE) ui_spinner(rx - S(10), y + h / 2, S(14), now, ink);
    else if (!start_page && tvs.adblock && (pg.hidden || pg.blocked)) {
        char c[24];
        snprintf(c, sizeof c, "%d", pg.hidden + pg.blocked);
        int cw = ui_text(FONT_SEMIBOLD, S(21), rx, y + h / 2, c, on ? 0x0E8A50 : 0x7CE3A6, a, 2);
        art_glyph(GL_SHIELD, rx - cw - S(20), y + h / 2, S(24), on ? 0x0E8A50 : 0x7CE3A6, a);
    }
    tb_button(x + w - b - S(8), y + S(8), b, GL_HOME, area == 1 && tb == 3, a, 1);
}

static void draw_start(int a) {
    int x = page_x(), y = page_y(), w = page_w();
    ui_text_shadow(FONT_DISPLAY, S(64), x + S(8), y + S(52), "Browser", 0xFFFFFF, a, 0);
    char sub[128];
    snprintf(sub, sizeof sub, "Ads and trackers are %s.", tvs.adblock ? "blocked on every site" : "not being blocked");
    ui_text_shadow(FONT_REGULAR, S(26), x + S(10), y + S(112), sub, 0xE6E9F0, a * 220 / 255, 0);
    int cols = 4, cw = (w - S(40) * 3) / cols, ch = S(200);
    int gy = y + S(150);
    for (int i = 0; i < NSITES; i++) {
        int tx = x + (i % cols) * (cw + S(40)), ty = gy + (i / cols) * (ch + S(40));
        int on = area == 0 && tile == i;
        if (on) ui_focus(tx - S(6), ty - S(6), cw + S(12), ch + S(12), S(34), a);
        else ui_glass(&gc_tiles, tx, ty, cw, ch, S(30), 150, a);
        uint32_t ink = on ? 0x101318 : 0xFFFFFF, dim = on ? 0x3A404C : 0xC2C8D2;
        int r = S(34);
        gfx_circle((float)(tx + S(30) + r), (float)(ty + S(30) + r), (float)r, SITES[i].c, a);
        char m[2] = { SITES[i].name[0], 0 };
        if (m[0] >= 'a' && m[0] <= 'z') m[0] = (char)(m[0] - 32);
        ui_text(FONT_BOLD, S(32), tx + S(30) + r, ty + S(30) + r, m, 0xFFFFFF, a, 1);
        ui_text(FONT_SEMIBOLD, S(30), tx + S(30), ty + S(128), SITES[i].name, ink, a, 0);
        font_draw_fit(FONT_REGULAR, S(21), tx + S(30), ty + S(172), SITES[i].what, cw - S(60), dim, a);
    }
    ui_text_shadow(FONT_REGULAR, S(21), x + S(10), tv_h - S(64),
                   "OK opens a site  \x95  Up reaches the address bar  \x95  Start typing to search",
                   0xC9CED8, a * 200 / 255, 0);
}

static void draw_page(uint64_t now, int a) {
    int x = page_x(), y = page_y(), w = page_w(), h = page_h();
    gfx_shadow(x, y, w, h, S(28), S(40), S(14), 0, 110 * a / 255);
    gfx_rrect(x, y, w, h, S(28), 0xFFFFFF, a);
    if (!pg.doc) {
        if (lstage != L_IDLE) {
            ui_spinner(x + w / 2, y + h / 2 - S(30), S(30), now, 0x5F6368);
            ui_text(FONT_REGULAR, S(26), x + w / 2, y + h / 2 + S(40), "Loading\x85", 0x5F6368, a, 1);
        } else if (status[0]) {
            art_glyph(GL_GLOBE, x + w / 2, y + h / 2 - S(70), S(64), 0x9AA0AA, a);
            ui_text(FONT_SEMIBOLD, S(32), x + w / 2, y + h / 2 + S(10), status, 0x202124, a, 1);
            ui_text(FONT_REGULAR, S(23), x + w / 2, y + h / 2 + S(60), pending_url, 0x5F6368, a, 1);
        }
        return;
    }
    gfx_clip_push(x, y, w, h);
    int top = (int)scroll, oy = y - top;
    for (int i = 0; i < pg.nitems; i++) {
        item_t *it = &pg.items[i];
        int iy = oy + it->y;
        if (iy + it->h < y || iy > y + h) continue;
        int ix = x + it->x;
        switch (it->kind) {
        case IK_WORD:
            (it->italic ? font_draw_italic : font_draw)(it->face, it->px, ix, iy + it->base, it->s, it->len,
                                                          it->color, 255);
            break;
        case IK_IMG: {
            image_t *im = it->img >= 0 ? pg.imgs[it->img].im : 0;
            if (im) gfx_image_rr(ix, iy, it->w, it->h, im->px, im->w, im->h, S(14), 255);
            else gfx_rrect(ix, iy, it->w, it->h, S(14), 0xEEF0F4, 255);
            break;
        }
        case IK_RULE:
            gfx_fill(ix, iy, it->w, 1, 0xD5D9E0);
            break;
        case IK_FIELD:
            gfx_rrect(ix, iy + S(4), it->w, it->h - S(8), (it->h - S(8)) / 2, 0xF1F3F6, 255);
            gfx_rrect_stroke(ix, iy + S(4), it->w, it->h - S(8), (it->h - S(8)) / 2, 1, 0xC9CED6, 255);
            if (it->len) font_draw_fit(FONT_REGULAR, S(22), ix + S(20), iy + it->h / 2 + S(8), it->s,
                                       it->w - S(40), 0x6B7280, 255);
            break;
        }
    }
    /* the focused link */
    if (area == 0 && lfocus >= 0 && lfocus < pg.nlink) {
        link_t *l = &pg.links[lfocus];
        int lx = x + l->x - S(8), ly = oy + l->y - S(2), lw = l->w + S(16), lh = l->h + S(4);
        gfx_rrect(lx, ly, lw, lh, S(10), 0x0B63CE, 34);
        gfx_rrect_stroke(lx, ly, lw, lh, S(10), S(3), 0x0B63CE, 235);
    }
    int dh = doc_h();
    if (dh > h) {
        int th = h * h / dh, ty = y + (int)((float)(h - th) * scroll / (float)(dh - h));
        gfx_rrect(x + w - S(14), ty + S(6), S(8), th - S(12), S(4), 0x000000, 70);
    }
    gfx_clip_pop();
    if (status[0] && lstage == L_IDLE) {
        int sw = font_width(FONT_MEDIUM, S(22), status, -1) + S(48);
        gfx_rrect(x + (w - sw) / 2, y + h - S(70), sw, S(48), S(24), 0x202124, 230);
        ui_text(FONT_MEDIUM, S(22), x + w / 2, y + h - S(46), status, 0xFFFFFF, a, 1);
    }
}

static void draw_osk(uint64_t now) {
    (void)now;
    int kw = S(92), kh = S(80), gap = S(10);
    int w = 10 * kw + 9 * gap + S(48), h = 5 * kh + 4 * gap + S(48);
    int x = (tv_w - w) / 2, y = tv_h - h - S(40);
    ui_glass(&gc_osk, x, y, w, h, S(36), 190, 255);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 10; c++) {
            int kx = x + S(24) + c * (kw + gap), ky = y + S(24) + r * (kh + gap);
            int on = osk_r == r && osk_c == c;
            if (on) ui_focus(kx, ky, kw, kh, S(16), 255);
            else gfx_rrect(kx, ky, kw, kh, S(16), 0xFFFFFF, 26);
            char s[2] = { OSK[r][c], 0 };
            ui_text(FONT_MEDIUM, S(32), kx + kw / 2, ky + kh / 2, s, on ? 0x101318 : 0xFFFFFF, 255, 1);
        }
    static const char *const SP[4] = { ".com", "Space", "Delete", "Go" };
    int spw[4] = { 2, 5, 2, 1 };
    int kx = x + S(24), ky = y + S(24) + 4 * (kh + gap);
    for (int i = 0; i < 4; i++) {
        int ww = spw[i] * kw + (spw[i] - 1) * gap;
        int on = osk_r == OSK_SPECIAL && osk_c == i;
        if (on) ui_focus(kx, ky, ww, kh, S(16), 255);
        else gfx_rrect(kx, ky, ww, kh, S(16), i == 3 ? 0x0A84FF : 0xFFFFFF, i == 3 ? 220 : 26);
        ui_text(FONT_SEMIBOLD, S(26), kx + ww / 2, ky + kh / 2, SP[i], on ? 0x101318 : 0xFFFFFF, 255, 1);
        kx += ww + gap;
    }
}

static void web_draw(uint64_t now) {
    float vt = tv_view_t();
    int a = (int)(vt * 255);
    if (start_page) draw_start(a);
    else draw_page(now, a);
    draw_toolbar(now, a);
    if (editing) draw_osk(now);
}

const view_t view_web = {
    .name = "web", .backdrop = BG_FILM_DIM,
    .enter = web_enter, .leave = web_leave, .key = web_key, .pointer = web_pointer,
    .draw = web_draw, .tick = web_tick,
};
