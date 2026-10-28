/* ===========================================================================
 *  BoltOS Mini  -  mini/app_browser.c
 *
 *  The web, on the Pi's own connection. One address bar: an address opens
 *  the page, anything else is a search. A search has three tabs --
 *
 *    All      DuckDuckGo's results (its HTML endpoint, which needs no JS)
 *    Images   DuckDuckGo's image search, as a grid of thumbnails
 *    Videos   YouTube's, as a grid; a video opens on a watch page and plays
 *             (mini/player.c)
 *
 *  -- and a page is the desktop's engine: the DOM (kernel/dom.c), the CSS
 *  cascade and box layout (kernel/layout.c) and the image decoders
 *  (kernel/image.c), painted here in Mini's own type. Text appears first;
 *  images arrive one per frame after it, nearest the view first.
 *
 *  Loading blocks: a navigation is queued, the next frame says "Loading",
 *  and the frame after that does the work, while mini_pump keeps the links
 *  and VNC alive underneath.
 *
 *  Keys: type to search or enter an address (Ctrl+L too). Tab and arrows
 *  move between results; Return opens; Backspace goes back; Space and the
 *  page keys scroll; Shift+Left/Right change the search tab; Esc leaves an
 *  address being typed, a picture, or the browser.
 * ===========================================================================*/
#include <stdint.h>
#include "ui.h"
#include "web.h"
#include "dom.h"
#include "layout.h"
#include "flow.h"
#include "image.h"
#include "http.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

#define PAGE_CAP   (3 * 1024 * 1024)   /* a YouTube results page is 1.7 MB    */
#define CSS_CAP    (256 * 1024)   /* what the cascade reads, all sheets together   */
#define SHEET_CAP  (1024 * 1024)  /* one sheet as it arrives: it is unzipped in place,
                                   * and a short buffer leaves it zipped           */
#define IMG_CAP    (2 * 1024 * 1024)
#define MAXITEM    100
#define MAXHIT     1600
#define MAXWIMG    160
#define MAXHIST    32

#define PAPER      0xFFFFFF
#define INK        0x202124
#define INK_DIM    0x5F6368
#define INK_LINK   0x1A0DAB
#define RULE       0xDADCE0

enum { V_START, V_WEB, V_RESULTS, V_IMAGES, V_VIDEOS, V_WATCH, V_MESSAGE };
enum { T_ALL, T_IMAGES, T_VIDEOS };
enum { H_LINK = 1, H_INPUT, H_SUBMIT, H_ITEM, H_QUICK };

typedef struct {
    char title[200], sub[160], text[360], badge[24];
    char href[600], thumb[600], full[600];
    int  aw, ah;                        /* the picture's shape (images tab)        */
    image_t *pix; int pstate;           /* 0 not yet, 1 loaded, -1 failed          */
    int x, y, w, h;                     /* where the last frame put it, page coords */
} item_t;

typedef struct { int x, y, w, h, kind, item; const char *href; dom_node *node; } hit_t;
typedef struct { int view, tab, scroll; char url[600], query[200]; } hist_t;

static struct {
    int  view, tab;
    char url[1024], query[200], title[200];
    char addr[600]; int alen, acur, editing, asel;   /* asel: all of it selected */
    int  pending, drawn_loading, push;
    hist_t next;
    char status[200];
    int  scroll, doc_h, view_h;
    int  focus;                         /* a hit index, -1 for none                */
    int  picked;                        /* images: the picture open over the grid  */
    /* the page */
    dom_document *dom; layout_tree *lt; flow_t fl; char *html; char *css; int css_len, laid_w, relayout;
    uint32_t paper;
    dom_node *field;                    /* the text field being typed in           */
    /* native views */
    item_t *items; int nitems;
    char vid[16];                       /* the watch page's video                  */
    char desc[2400], meta[200];
    hist_t hist[MAXHIST]; int nhist;
} B;

static hit_t hits[MAXHIT];
static int nhits;
static struct { char *url; image_t *im; int state, near; } wimg[MAXWIMG];
static int nwimg;
static struct http_conn *img_sess;
static char img_sess_host[160];

/* The player (mini/player.c) */
int  player_open(const char *video_id, char *title, int title_cap, char *meta, int meta_cap);
void player_close(void);
int  player_tick(void);
const surface_t *player_frame(void);
int  player_status(char *msg, int cap);         /* 0 idle, 1 loading, 2 playing, 3 paused, -1 error */
void player_toggle(void);
void player_seek(int secs);
int  player_pos_ms(void);
int  player_len_ms(void);

static void set_status(const char *fmt, const char *a) {
    snprintf(B.status, sizeof B.status, fmt, a ? a : "");
}

/* ---- images ------------------------------------------------------------------- */
static void to_white(image_t *im) {               /* composite onto the page once     */
    for (int i = 0; i < im->w * im->h; i++) {
        uint32_t c = im->px[i], a = c >> 24;
        if (a == 255) continue;
        uint32_t r = ((c >> 16 & 255) * a + 255 * (255 - a)) / 255;
        uint32_t g = ((c >> 8 & 255) * a + 255 * (255 - a)) / 255;
        uint32_t b = ((c & 255) * a + 255 * (255 - a)) / 255;
        im->px[i] = 0xFF000000u | r << 16 | g << 8 | b;
    }
}

/* One image, over a kept-alive connection when the host repeats (a page's
 * images, a grid of thumbnails: one TLS handshake, not one each). */
static image_t *fetch_image(const char *url) {
    if (strncmp(url, "http", 4)) return 0;
    uint8_t *buf = kmalloc(IMG_CAP);
    if (!buf) return 0;
    char host[160]; url_host(url, host, sizeof host);
    if (img_sess && (strcmp(host, img_sess_host) || !http_conn_can_reuse(img_sess, url))) { http_close_conn(img_sess); img_sess = 0; }
    int code = 0, n = -1;
    char loc[600] = "";
    for (int attempt = 0; attempt < 2 && n < 0; attempt++) {
        if (!img_sess) { int st; img_sess = http_open(url, &st); snprintf(img_sess_host, sizeof img_sess_host, "%s", host); }
        if (!img_sess) break;
        n = http_fetch(img_sess, url, (char *)buf, IMG_CAP, &code, loc, sizeof loc);
        if (n < 0) { http_close_conn(img_sess); img_sess = 0; }
    }
    if (n >= 0 && code >= 300 && code < 400 && loc[0]) {   /* one hop, on its own connection */
        char to[600]; url_resolve(url, loc, to, sizeof to);
        n = web_request("GET", to, 0, 0, 0, (char *)buf, IMG_CAP, &code, 0, 0);
    }
    image_t *im = (n > 0 && code == 200) ? image_decode(buf, (uint32_t)n) : 0;
    if (!im) {
        static int told;
        if (told++ < 6) kprintf("[web] image %d, %d bytes (%02x %02x %02x %02x): %.90s\n", code, n,
                                n > 3 ? buf[0] : 0, n > 3 ? buf[1] : 0, n > 3 ? buf[2] : 0, n > 3 ? buf[3] : 0, url);
    }
    kfree(buf);
    if (im) to_white(im);
    return im;
}

static void draw_image(image_t *im, int x, int y, int w, int h, int r) {
    surface_t s = { im->px, im->w, im->h, im->w };
    gfx_surface_scaled(x, y, w, h, &s, r, 255);
}

/* The picture cropped to fill w x h (thumbnails of any shape in a 16:9 tile). */
static void draw_image_cover(image_t *im, int x, int y, int w, int h, int r) {
    int sw = im->w, sh = im->h;
    if ((long)sw * h > (long)sh * w) sw = (int)((long)sh * w / h); else sh = (int)((long)sw * h / w);
    if (sw < 1 || sh < 1) return;
    surface_t s = { im->px + (im->h - sh) / 2 * im->w + (im->w - sw) / 2, sw, sh, im->w };
    gfx_surface_scaled(x, y, w, h, &s, r, 255);
}

/* ---- leaving a page ---------------------------------------------------------------- */
static void free_page(void) {
    if (B.lt) { layout_free(B.lt); B.lt = 0; }
    flow_free(&B.fl);
    if (B.dom) { dom_free(B.dom); B.dom = 0; }
    if (B.html) { kfree(B.html); B.html = 0; }
    if (B.css) { kfree(B.css); B.css = 0; }
    B.css_len = 0; B.laid_w = -1; B.field = 0;
    for (int i = 0; i < nwimg; i++) { if (wimg[i].im) image_free(wimg[i].im); kfree(wimg[i].url); }
    nwimg = 0;
    if (B.items) {
        for (int i = 0; i < B.nitems; i++) if (B.items[i].pix) image_free(B.items[i].pix);
        kfree(B.items); B.items = 0;
    }
    B.nitems = 0; B.picked = -1;
    if (B.view == V_WATCH) player_close();
    B.title[0] = 0; B.desc[0] = 0; B.meta[0] = 0; B.vid[0] = 0;
    nhits = 0;
}

static item_t *new_item(void) {
    if (!B.items) { B.items = kmalloc(MAXITEM * sizeof(item_t)); if (!B.items) return 0; }
    if (B.nitems >= MAXITEM) return 0;
    item_t *it = &B.items[B.nitems++];
    memset(it, 0, sizeof *it);
    return it;
}

/* ---- navigation ---------------------------------------------------------------------- */
static void go(int view, int tab, const char *url, const char *query, int push) {
    B.pending = 1; B.drawn_loading = 0; B.push = push;
    B.next.view = view; B.next.tab = tab; B.next.scroll = 0;
    snprintf(B.next.url, sizeof B.next.url, "%s", url ? url : "");
    snprintf(B.next.query, sizeof B.next.query, "%s", query ? query : "");
    B.editing = 0;
    set_status("Loading %s" "\x85", query && query[0] ? query : url);
    shell_request_frame();
}

static int host_is(const char *url, const char *h) {
    char host[160]; url_host(url, host, sizeof host);
    int n = (int)strlen(host), k = (int)strlen(h);
    return n >= k && !strcmp(host + n - k, h) && (n == k || host[n - k - 1] == '.');
}

/* An address that is really a search, a result redirect, or a video. */
static void open_url(const char *url, int push) {
    char q[200], target[600];
    if (host_is(url, "duckduckgo.com") && strstr(url, "/l/") && url_param(url, "uddg", target, sizeof target)) url = target;
    if ((host_is(url, "google.com") || host_is(url, "bing.com") || host_is(url, "duckduckgo.com")) &&
        url_param(url, "q", q, sizeof q) && q[0]) { go(V_RESULTS, T_ALL, 0, q, push); return; }
    if (host_is(url, "youtube.com") || host_is(url, "youtu.be")) {
        char v[32] = "";
        if (host_is(url, "youtu.be")) { const char *p = strstr(url, "youtu.be/"); if (p) snprintf(v, sizeof v, "%.11s", p + 9); }
        else url_param(url, "v", v, sizeof v);
        if (strlen(v) == 11) { go(V_WATCH, T_VIDEOS, v, 0, push); return; }
        if (url_param(url, "search_query", q, sizeof q)) { go(V_VIDEOS, T_VIDEOS, 0, q, push); return; }
    }
    go(V_WEB, B.tab, url, 0, push);
}

static void submit_address(void) {
    char a[600];
    int s = 0;
    while (B.addr[s] == ' ') s++;
    snprintf(a, sizeof a, "%s", B.addr + s);
    int n = (int)strlen(a);
    while (n && a[n - 1] == ' ') a[--n] = 0;
    if (!n) return;
    const char *dot = strchr(a, '.'), *sp = strchr(a, ' ');
    if (!strncmp(a, "http://", 7) || !strncmp(a, "https://", 8)) { open_url(a, 1); return; }
    if (dot && !sp && dot[1] && dot != a) {
        char u[620]; snprintf(u, sizeof u, "https://%s", a);
        open_url(u, 1);
        return;
    }
    int tab = B.view == V_START ? B.tab : (B.view == V_IMAGES ? T_IMAGES : B.view == V_VIDEOS || B.view == V_WATCH ? T_VIDEOS : T_ALL);
    go(tab == T_IMAGES ? V_IMAGES : tab == T_VIDEOS ? V_VIDEOS : V_RESULTS, tab, 0, a, 1);
}

static void go_back(void) {
    if (B.picked >= 0) { B.picked = -1; return; }
    if (!B.nhist) { if (B.view != V_START) { free_page(); B.view = V_START; B.editing = 1; B.alen = B.acur = 0; B.addr[0] = 0; } return; }
    hist_t h = B.hist[--B.nhist];
    go(h.view, h.tab, h.url, h.query, 0);
    B.next.scroll = h.scroll;
}

static void set_tab(int tab) {
    if (B.view == V_START) { B.tab = tab; return; }
    if (!B.query[0]) return;
    go(tab == T_IMAGES ? V_IMAGES : tab == T_VIDEOS ? V_VIDEOS : V_RESULTS, tab, 0, B.query, 1);
}

/* ---- the web page ------------------------------------------------------------------------ */
static void decode_tree(dom_document *d, dom_node *n, int raw) {
    for (; n; n = n->next) {
        if (n->type == DOM_TEXT) { if (!raw && n->text) entities_decode(n->text); continue; }
        for (dom_attr *a = n->attrs; a; a = a->next) if (a->value) entities_decode(a->value);
        int r = raw || (n->tag && (!strcmp(n->tag, "script") || !strcmp(n->tag, "style")));
        /* What the engine would draw wrongly or not at all. */
        if (n->tag && (!strcmp(n->tag, "svg") || !strcmp(n->tag, "template") || !strcmp(n->tag, "iframe") ||
                       !strcmp(n->tag, "video") || !strcmp(n->tag, "audio") || !strcmp(n->tag, "canvas") ||
                       !strcmp(n->tag, "object") || !strcmp(n->tag, "embed") || !strcmp(n->tag, "dialog")))
            dom_attr_set(d, n, "style", "display:none");
        if (n->tag && !strcmp(n->tag, "input")) {
            const char *t = dom_attr_get(n, "type");
            if (t && (!strcmp(t, "hidden") || !strcmp(t, "checkbox") || !strcmp(t, "radio")))
                dom_attr_set(d, n, "style", "display:none");
        }
        decode_tree(d, n->first_child, r);
    }
}

static void gather_css(void) {
    B.css = kmalloc(CSS_CAP);
    if (!B.css) return;
    int n = 0;
    dom_node *st[32];
    int k = dom_by_tag(B.dom->root, "style", st, 32);
    for (int i = 0; i < k && n < CSS_CAP - 2; i++) n += dom_inner_text(st[i], B.css + n, (uint32_t)(CSS_CAP - 1 - n)), B.css[n++] = '\n';
    dom_node *ln[24];
    k = dom_by_tag(B.dom->root, "link", ln, 24);
    char *tmp = kmalloc(SHEET_CAP);
    for (int i = 0, got = 0; tmp && i < k && got < 4 && n < CSS_CAP - 2; i++) {
        const char *rel = dom_attr_get(ln[i], "rel"), *href = dom_attr_get(ln[i], "href");
        const char *media = dom_attr_get(ln[i], "media");
        if (!rel || !href || !strstr(rel, "stylesheet") || (media && strstr(media, "print"))) continue;
        char u[700]; url_resolve(B.url, href, u, sizeof u);
        int code, m = web_request("GET", u, 0, 0, 0, tmp, SHEET_CAP, &code, 0, 0);
        if (m > 0 && code == 200) {
            if (m > CSS_CAP - 2 - n) m = CSS_CAP - 2 - n;
            memcpy(B.css + n, tmp, (size_t)m); n += m; B.css[n++] = '\n';
            got++;
        }
    }
    if (tmp) kfree(tmp);
    B.css[n] = 0;
    B.css_len = n;
}

static void load_web(const char *url) {
    char *buf = kmalloc(PAGE_CAP);
    if (!buf) { B.view = V_MESSAGE; snprintf(B.title, sizeof B.title, "Out of memory"); return; }
    int code = 0;
    uint64_t t0 = now_ms();
    int n = web_request("GET", url, 0, 0, 0, buf, PAGE_CAP, &code, B.url, sizeof B.url);
    if (n < 0) {
        kfree(buf);
        B.view = V_MESSAGE;
        snprintf(B.title, sizeof B.title, "%s", code == -3 ? "That address wasn't found" :
                 code == -5 ? "A secure connection couldn't be made" : "The page didn't answer");
        snprintf(B.desc, sizeof B.desc, "%s", url);
        set_status("%s", code == -3 ? "DNS lookup failed" : code == -5 ? "TLS handshake failed" : "Connection failed");
        return;
    }
    /* A picture, sent on its own. */
    const uint8_t *u8 = (const uint8_t *)buf;
    if (n > 8 && ((u8[0] == 0xFF && u8[1] == 0xD8) || !memcmp(buf, "\x89PNG", 4) || !memcmp(buf, "GIF8", 4))) {
        item_t *it = new_item();
        if (it) {
            it->pix = image_decode((const uint8_t *)buf, (uint32_t)n);
            it->pstate = it->pix ? 1 : -1;
            snprintf(it->full, sizeof it->full, "%s", B.url);
            snprintf(it->title, sizeof it->title, "%s", B.url);
        }
        kfree(buf);
        B.view = V_IMAGES; B.picked = 0;
        return;
    }
    int plain = 1;
    for (int i = 0; i < n && i < 1024; i++) if (buf[i] == '<') { plain = 0; break; }
    B.html = kmalloc((uint64_t)n + 64);
    if (!B.html) { kfree(buf); return; }
    int hl;
    if (plain) { strcpy(B.html, "<pre>"); hl = 5 + utf8_to_cp1252(buf, n, B.html + 5, n + 1); strcpy(B.html + hl, "</pre>"); hl += 6; }
    else hl = utf8_to_cp1252(buf, n, B.html, n + 1);
    kfree(buf);
    B.dom = dom_parse(B.html, (uint32_t)hl);
    if (!B.dom) { B.view = V_MESSAGE; snprintf(B.title, sizeof B.title, "The page couldn't be read"); return; }
    decode_tree(B.dom, B.dom->root, 0);
    dom_node *t[1];
    if (dom_by_tag(B.dom->root, "title", t, 1) == 1) {
        dom_inner_text(t[0], B.title, sizeof B.title);
        entities_decode(B.title);
    }
    gather_css();
    B.laid_w = -1;
    char kb[40]; snprintf(kb, sizeof kb, "%d KB in %lu ms", n / 1024, (unsigned long)(now_ms() - t0));
    set_status("%s", kb);
}

static int wimg_find(const char *url, int add) {
    for (int i = 0; i < nwimg; i++) if (!strcmp(wimg[i].url, url)) return i;
    if (!add || nwimg >= MAXWIMG) return -1;
    int n = (int)strlen(url);
    wimg[nwimg].url = kmalloc((uint64_t)n + 1);
    if (!wimg[nwimg].url) return -1;
    memcpy(wimg[nwimg].url, url, (size_t)n + 1);
    wimg[nwimg].im = 0; wimg[nwimg].state = 0; wimg[nwimg].near = 0;
    return nwimg++;
}

static const char *img_src(dom_node *n) {
    const char *s = dom_attr_get(n, "src");
    if (!s || !s[0] || !strncmp(s, "data:", 5)) {
        const char *d = dom_attr_get(n, "data-src");
        if (!d) d = dom_attr_get(n, "data-lazy-src");
        if (d && d[0]) s = d;
    }
    return s && s[0] && strncmp(s, "data:", 5) ? s : 0;
}

/* flow.c asks how big an <img> is: its picture's size once it has arrived,
 * else what the page says, else nothing yet (it is laid out again then). */
static void img_size(dom_node *n, int maxw, int *w, int *h, int *slot, void *ud) {
    (void)ud;
    *w = *h = 0; *slot = -1;
    const char *src = img_src(n);
    int aw = 0, ah = 0;
    const char *v = dom_attr_get(n, "width");
    if (v && *v >= '0' && *v <= '9' && !strchr(v, '%')) while (*v >= '0' && *v <= '9') aw = aw * 10 + (*v++ - '0');
    v = dom_attr_get(n, "height");
    if (v && *v >= '0' && *v <= '9' && !strchr(v, '%')) while (*v >= '0' && *v <= '9') ah = ah * 10 + (*v++ - '0');
    if (src) { char u[700]; url_resolve(B.url, src, u, sizeof u); *slot = wimg_find(u, 1); }
    image_t *im = *slot >= 0 ? wimg[*slot].im : 0;
    if (im) {
        if (aw && ah) { *w = aw; *h = ah; }
        else if (aw) { *w = aw; *h = aw * im->h / im->w; }
        else if (ah) { *h = ah; *w = ah * im->w / im->h; }
        else { *w = im->w; *h = im->h; }
    } else if (aw && ah) { *w = aw; *h = ah; }
    else if (*slot >= 0 && wimg[*slot].state < 0) { *w = *h = 0; }
    if (*w > maxw && *w > 0) { *h = *h * maxw / *w; *w = maxw; }
}

/* The cascade once per page; the layout again when the width changes or a
 * picture arrives with its real size. */
static void ensure_layout(int w) {
    if (!B.dom) return;
    if (!B.lt) {
        layout_set_media_width(w);                 /* @media answers for this width */
        B.lt = layout_styles(B.dom, B.css, (uint32_t)B.css_len);
        B.laid_w = -1;
    }
    if (!B.lt || (B.laid_w == w && !B.relayout)) return;
    flow_layout(&B.fl, B.lt->root, w, img_size, 0);
    B.paper = B.fl.paper;
    B.laid_w = w; B.relayout = 0;
}

static void add_hit(int x, int y, int w, int h, int kind, const char *href, dom_node *node, int item) {
    if (nhits >= MAXHIT) return;
    hits[nhits++] = (hit_t){ x, y, w, h, kind, item, href, node };
}

/* Paint the laid-out page with its (0,0) at ox, oy, showing rows top..bottom. */
static void paint_page(int ox, int oy, int top, int bottom) {
    for (int i = 0; i < nwimg; i++) wimg[i].near = 0;
    for (int i = 0; i < B.fl.n; i++) {
        fl_item *it = &B.fl.it[i];
        int sx = ox + it->x, sy = oy + it->y;
        int vis = sy + it->h >= top && sy <= bottom;
        switch (it->kind) {
        case FL_RECT:
            if (vis) gfx_fill(sx, sy, it->w, it->h, it->color);
            break;
        case FL_TEXT:
            if (vis) {
                int base = sy + it->a;
                if (it->flags & FLF_ITALIC) font_draw_italic(it->face, it->px, sx, base, it->s, it->len, it->color, 255);
                else font_draw(it->face, it->px, sx, base, it->s, it->len, it->color, 255);
                if (B.focus >= 0 && hits[B.focus].href == it->href && it->href) gfx_fill(sx, base + 2, it->w, 2, th.accent);
            }
            break;
        case FL_BULLET:
            if (vis) {
                if (it->len) font_draw(FONT_REGULAR, 15, sx, sy, it->num, it->len, it->color, 255);
                else gfx_circle((float)(sx + 6), (float)(sy - 5), 2.6f, it->color, 255);
            }
            break;
        case FL_IMAGE:
            if (it->img >= 0 && sy + it->h >= top - 700 && sy <= bottom + 700) wimg[it->img].near = 1;
            if (vis && it->w > 0 && it->h > 0) {
                if (it->img >= 0 && wimg[it->img].im) draw_image(wimg[it->img].im, sx, sy, it->w, it->h, 0);
                else gfx_fill(sx, sy, it->w, it->h, 0xF1F3F4);
            }
            break;
        case FL_CONTROL: {
            if (!vis) break;
            dom_node *n = it->node;
            const char *tag = n->tag, *type = dom_attr_get(n, "type");
            int button = !strcmp(tag, "button") || (type && (!strcmp(type, "submit") || !strcmp(type, "button") || !strcmp(type, "reset")));
            int focused = B.field == n;
            gfx_rrect(sx, sy, it->w, it->h, 6, button ? 0xF1F3F4 : 0xFFFFFF, 255);
            gfx_rrect_stroke(sx, sy, it->w, it->h, 6, focused ? 2 : 1, focused ? th.accent : 0xC4C7C5, 255);
            char lbl[200] = "";
            const char *val = dom_attr_get(n, "value"), *ph = dom_attr_get(n, "placeholder");
            if (!strcmp(tag, "button")) dom_inner_text(n, lbl, sizeof lbl);
            else if (!strcmp(tag, "select")) { dom_node *o = dom_query(n, "option"); if (o) dom_inner_text(o, lbl, sizeof lbl); }
            else if (val && val[0]) snprintf(lbl, sizeof lbl, "%s", val);
            else if (ph) snprintf(lbl, sizeof lbl, "%s", ph);
            else if (button) snprintf(lbl, sizeof lbl, "Go");
            int dim = !button && !(val && val[0]) && strcmp(tag, "select");
            int base = sy + (it->h + font_capheight(FONT_REGULAR, 14)) / 2;
            int tw = font_draw_fit(button ? FONT_MEDIUM : FONT_REGULAR, 14, sx + (button ? 14 : 9), base, lbl, it->w - 18, dim ? INK_DIM : INK, 255);
            if (focused && (now_ms() / 530) % 2 == 0) gfx_fill(sx + 9 + (dim ? 0 : tw) + 1, sy + 6, 2, it->h - 12, th.accent);
            break;
        }
        }
        if (it->kind == FL_CONTROL) {
            const char *type = dom_attr_get(it->node, "type");
            int button = !strcmp(it->node->tag, "button") || (type && (!strcmp(type, "submit") || !strcmp(type, "button")));
            add_hit(it->x, it->y, it->w, it->h, button ? H_SUBMIT : H_INPUT, 0, it->node, -1);
        } else if (it->href && (it->kind == FL_TEXT || it->kind == FL_IMAGE) && it->w > 0)
            add_hit(it->x, it->y, it->w, it->h, H_LINK, it->href, 0, -1);
    }
}

/* ---- forms ----------------------------------------------------------------------------- */
static void collect_fields(dom_node *n, char *q, int cap, int *o) {
    for (; n; n = n->next) {
        if (n->type == DOM_ELEMENT && n->tag && (!strcmp(n->tag, "input") || !strcmp(n->tag, "textarea") || !strcmp(n->tag, "select"))) {
            const char *name = dom_attr_get(n, "name"), *type = dom_attr_get(n, "type");
            const char *val = dom_attr_get(n, "value");
            int skip = !name || (type && (!strcmp(type, "submit") || !strcmp(type, "button") || !strcmp(type, "image") ||
                                          ((!strcmp(type, "checkbox") || !strcmp(type, "radio")) && !dom_attr_get(n, "checked"))));
            if (!skip) {
                char en[300], ev[600];
                url_encode(name, en, sizeof en); url_encode(val ? val : "", ev, sizeof ev);
                *o += snprintf(q + *o, (size_t)(cap - *o), "%s%s=%s", *o ? "&" : "", en, ev);
            }
        }
        collect_fields(n->first_child, q, cap, o);
    }
}

static void submit_form(dom_node *from) {
    dom_node *f = from;
    while (f && !(f->tag && !strcmp(f->tag, "form"))) f = f->parent;
    if (!f) return;
    char q[1200]; int o = 0;
    collect_fields(f->first_child, q, sizeof q, &o);
    const char *action = dom_attr_get(f, "action"), *method = dom_attr_get(f, "method");
    char base[700]; url_resolve(B.url, action && action[0] ? action : B.url, base, sizeof base);
    char *qm = strchr(base, '?'); if (qm && !(method && !strcmp(method, "post"))) *qm = 0;
    char u[1400]; snprintf(u, sizeof u, "%s?%s", base, q);
    open_url(u, 1);
}

/* ---- DuckDuckGo: results, and images ----------------------------------------------------- */
static int ddg_results(const char *html, int n) {
    dom_document *d = dom_parse(html, (uint32_t)n);
    if (!d) return 0;
    decode_tree(d, d->root, 0);
    dom_node *res[64];
    int k = dom_query_all(d->root, ".result", res, 64);
    for (int i = 0; i < k; i++) {
        if (dom_has_class(res[i], "result--ad")) continue;
        dom_node *a = dom_query(res[i], ".result__a");
        if (!a) continue;
        item_t *it = new_item();
        if (!it) break;
        dom_inner_text(a, it->title, sizeof it->title);
        const char *href = dom_attr_get(a, "href");
        char tgt[600];
        if (href && strstr(href, "uddg=") && url_param(href, "uddg", tgt, sizeof tgt)) snprintf(it->href, sizeof it->href, "%s", tgt);
        else if (href) url_resolve("https://duckduckgo.com/", href, it->href, sizeof it->href);
        dom_node *sn = dom_query(res[i], ".result__snippet"), *un = dom_query(res[i], ".result__url");
        if (sn) dom_inner_text(sn, it->text, sizeof it->text);
        if (un) dom_inner_text(un, it->sub, sizeof it->sub);
        char *t = it->sub; while (*t == ' ' || *t == '\n') t++;
        memmove(it->sub, t, strlen(t) + 1);
        for (char *c = it->text; *c; c++) if (*c == '\n') *c = ' ';
    }
    dom_free(d);
    return B.nitems;
}

static void search_web(const char *q) {
    char eq[600], u[700];
    url_encode(q, eq, sizeof eq);
    snprintf(u, sizeof u, "https://html.duckduckgo.com/html/?q=%s", eq);
    snprintf(B.url, sizeof B.url, "%s", u);
    char *buf = kmalloc(512 * 1024);
    if (!buf) return;
    int code, n = web_request("GET", u, 0, 0, 0, buf, 512 * 1024, &code, 0, 0);
    if (n > 0) {
        char *c = kmalloc((uint64_t)n + 1);
        if (c) { int m = utf8_to_cp1252(buf, n, c, n + 1); ddg_results(c, m); kfree(c); }
    }
    kfree(buf);
    if (!B.nitems) { set_status("%s", n < 0 ? "DuckDuckGo didn't answer" : "No results"); }
    else { char s[40]; snprintf(s, sizeof s, "%d results", B.nitems); set_status("%s", s); }
}

static void search_images(const char *q) {
    char eq[600], u[900];
    url_encode(q, eq, sizeof eq);
    char *buf = kmalloc(1024 * 1024);
    if (!buf) return;
    /* The page carries a token (vqd) that the image API wants back. */
    snprintf(u, sizeof u, "https://duckduckgo.com/?q=%s&iax=images&ia=images", eq);
    int code, n = web_request("GET", u, 0, 0, 0, buf, 1024 * 1024, &code, 0, 0);
    char vqd[80] = "";
    if (n > 0) {
        buf[n] = 0;
        const char *p = strstr(buf, "vqd=");
        if (!p) p = strstr(buf, "vqd\":");
        if (p) {
            p = strchr(p, '=') ? strchr(p, '=') + 1 : p;
            while (*p == '"' || *p == '\'' || *p == ':') p++;
            int k = 0;
            while ((*p == '-' || (*p >= '0' && *p <= '9')) && k < 79) vqd[k++] = *p++;
            vqd[k] = 0;
        }
    }
    if (!vqd[0]) { kfree(buf); set_status("%s", "DuckDuckGo didn't give an image search"); return; }
    snprintf(u, sizeof u, "https://duckduckgo.com/i.js?l=us-en&o=json&q=%s&vqd=%s&f=,,,,,&p=1", eq, vqd);
    n = web_request("GET", u, "Referer: https://duckduckgo.com/\r\n", 0, 0, buf, 1024 * 1024, &code, 0, 0);
    jarena *ar = 0;
    jv *root = n > 0 ? json_parse(buf, n, &ar) : 0;
    jv *res = json_get(root, "results");
    /* Asking for JSON in an Accept header gets a bot challenge (202) instead. */
    if (!res) kprintf("[web] images: i.js answered %d: %.80s\n", code, n > 0 ? buf : "");
    for (jv *r = res ? res->kid : 0; r; r = r->next) {
        item_t *it = new_item();
        if (!it) break;
        const char *t = json_s(json_get(r, "title"));
        utf8_to_cp1252(t, (int)strlen(t), it->title, sizeof it->title);
        entities_decode(it->title);
        snprintf(it->thumb, sizeof it->thumb, "%s", json_s(json_get(r, "thumbnail")));
        snprintf(it->full, sizeof it->full, "%s", json_s(json_get(r, "image")));
        snprintf(it->href, sizeof it->href, "%s", json_s(json_get(r, "url")));
        url_host(it->href, it->sub, sizeof it->sub);
        it->aw = (int)json_i(json_get(r, "width")); it->ah = (int)json_i(json_get(r, "height"));
        if (it->aw <= 0 || it->ah <= 0) { it->aw = 4; it->ah = 3; }
        snprintf(it->badge, sizeof it->badge, "%d \xD7 %d", it->aw, it->ah);
    }
    json_free(ar);
    kfree(buf);
    char s[40]; snprintf(s, sizeof s, "%d images", B.nitems);
    set_status("%s", B.nitems ? s : "No images");
}

/* ---- YouTube ------------------------------------------------------------------------------- */
static int yt_video(const jv *v, void *ud) {
    (void)ud;
    const char *id = json_s(json_get(v, "videoId"));
    if (strlen(id) != 11) return 0;
    for (int i = 0; i < B.nitems; i++) if (!strcmp(B.items[i].href, id)) return 0;
    item_t *it = new_item();
    if (!it) return 1;
    snprintf(it->href, sizeof it->href, "%s", id);
    json_text(json_get(v, "title"), it->title, sizeof it->title);
    json_text(json_get(v, "ownerText"), it->sub, sizeof it->sub);
    char views[60] = "", when[60] = "";
    json_text(json_get(v, "shortViewCountText"), views, sizeof views);
    json_text(json_get(v, "publishedTimeText"), when, sizeof when);
    snprintf(it->text, sizeof it->text, "%s%s%s", views, views[0] && when[0] ? " \xB7 " : "", when);
    json_text(json_get(v, "lengthText"), it->badge, sizeof it->badge);
    if (!it->badge[0] && strstr(it->text, "watching")) snprintf(it->badge, sizeof it->badge, "LIVE");
    snprintf(it->thumb, sizeof it->thumb, "https://i.ytimg.com/vi/%s/mqdefault.jpg", id);
    return 0;
}

static void search_videos(const char *q) {
    char eq[600], u[700];
    url_encode(q, eq, sizeof eq);
    snprintf(u, sizeof u, "https://www.youtube.com/results?search_query=%s&hl=en&gl=US", eq);
    snprintf(B.url, sizeof B.url, "%s", u);
    char *buf = kmalloc(PAGE_CAP);
    if (!buf) return;
    int code, n = web_request("GET", u, 0, 0, 0, buf, PAGE_CAP, &code, 0, 0);
    if (n > 0) {
        buf[n] = 0;
        const char *p = strstr(buf, "ytInitialData = ");
        if (p) {
            p += 16;
            jarena *ar = 0;
            jv *root = json_parse(p, n - (int)(p - buf), &ar);
            if (root) json_each(root, "videoRenderer", yt_video, 0);
            json_free(ar);
        }
    }
    kfree(buf);
    char s[40]; snprintf(s, sizeof s, "%d videos", B.nitems);
    set_status("%s", B.nitems ? s : n < 0 ? "YouTube didn't answer" : "No videos");
}

static void open_watch(const char *id) {
    snprintf(B.vid, sizeof B.vid, "%.11s", id);
    snprintf(B.url, sizeof B.url, "https://www.youtube.com/watch?v=%s", B.vid);
    item_t *it = new_item();
    if (it) snprintf(it->thumb, sizeof it->thumb, "https://i.ytimg.com/vi/%s/hqdefault.jpg", B.vid);
    player_open(B.vid, B.title, sizeof B.title, B.meta, sizeof B.meta);
}

/* ---- loading, for real --------------------------------------------------------------------- */
static void do_load(void) {
    hist_t n = B.next;
    if (B.push && B.view != V_START && B.view != V_MESSAGE) {
        if (B.nhist == MAXHIST) { memmove(B.hist, B.hist + 1, sizeof(hist_t) * (MAXHIST - 1)); B.nhist--; }
        hist_t *h = &B.hist[B.nhist++];
        h->view = B.view; h->tab = B.tab; h->scroll = B.scroll;
        snprintf(h->url, sizeof h->url, "%s", B.view == V_WATCH ? B.vid : B.url);
        snprintf(h->query, sizeof h->query, "%s", B.query);
    }
    free_page();
    if (img_sess) { http_close_conn(img_sess); img_sess = 0; }
    B.view = n.view; B.tab = n.tab; B.scroll = n.scroll; B.focus = -1; B.picked = -1;
    snprintf(B.query, sizeof B.query, "%s", n.query);
    switch (n.view) {
    case V_WEB:     snprintf(B.url, sizeof B.url, "%s", n.url); load_web(n.url); break;
    case V_RESULTS: search_web(n.query); break;
    case V_IMAGES:  search_images(n.query); break;
    case V_VIDEOS:  search_videos(n.query); break;
    case V_WATCH:   open_watch(n.url); break;
    }
    if (B.view == V_WEB || B.view == V_MESSAGE) B.query[0] = 0;
    snprintf(B.addr, sizeof B.addr, "%s", B.query[0] && B.view != V_WATCH ? B.query : B.url);
    B.alen = B.acur = (int)strlen(B.addr);
    shell_request_frame();
}

/* ---- drawing ------------------------------------------------------------------------------- */
static int wrap_lines(const char *s, int face, int px, int w, int max, int draw_x, int draw_y, int lh, uint32_t col) {
    int lines = 0, len = (int)strlen(s);
    const char *p = s;
    while (len > 0 && lines < max) {
        while (*p == ' ') { p++; len--; }
        if (!len) break;
        int fit = font_fit(face, px, p, len, w);
        if (fit < len) {
            int k = fit;
            while (k > 0 && p[k] != ' ') k--;
            if (k > 0) fit = k;
        }
        if (fit <= 0) fit = 1;
        if (draw_y > -10000) {
            if (lines == max - 1 && fit < len) font_draw_fit(face, px, draw_x, draw_y + lines * lh, p, w, col, 255);
            else font_draw(face, px, draw_x, draw_y + lines * lh, p, fit, col, 255);
        }
        p += fit; len -= fit; lines++;
    }
    return lines;
}

static void draw_bar(int x, int y, int w) {
    int h = S(38), r = h / 2;
    int bx = x;
    if (B.nhist || B.view != V_START) {                         /* back */
        gfx_rrect(bx, y, h, h, r, th.dark ? 0xFFFFFF : 0x000000, th.dark ? 22 : 14);
        gfx_line((float)(bx + h / 2 + S(3)), (float)(y + h / 2 - S(6)), (float)(bx + h / 2 - S(3)), (float)(y + h / 2), (float)S(2), th.text, 255);
        gfx_line((float)(bx + h / 2 - S(3)), (float)(y + h / 2), (float)(bx + h / 2 + S(3)), (float)(y + h / 2 + S(6)), (float)S(2), th.text, 255);
        bx += h + S(8);
    }
    int aw = x + w - bx;
    gfx_rrect(bx, y, aw, h, r, th.dark ? 0xFFFFFF : 0x000000, B.editing ? (th.dark ? 34 : 22) : (th.dark ? 22 : 14));
    if (B.editing) gfx_rrect_stroke(bx, y, aw, h, r, S(2) > 1 ? 2 : 1, th.accent, 200);
    int tx = bx + S(16), cy = y + h / 2, px = S(14);
    int lock = !strncmp(B.url, "https://", 8) && !B.editing && B.view == V_WEB;
    if (lock) {
        gfx_rrect_stroke(tx, cy - S(1), S(10), S(8), S(2), 1, th.sub, 255);
        gfx_arc((float)(tx + S(5)), (float)(cy - S(2)), (float)S(7) / 2, 1.4f, 180, 360, th.sub, 255);
        tx += S(16);
    }
    const char *shown = B.addr;
    char disp[600];
    if (!B.editing && B.view == V_WEB) {                     /* the host, quietly the rest */
        const char *s = strstr(B.url, "://"); s = s ? s + 3 : B.url;
        snprintf(disp, sizeof disp, "%s", s);
        int n = (int)strlen(disp);
        if (n && disp[n - 1] == '/') disp[n - 1] = 0;
        shown = disp;
    }
    if (!shown[0] && B.editing) text_l(tx, cy, "Search or enter an address", FONT_REGULAR, px, th.faint);
    gfx_clip_push(tx, y, aw - (tx - bx) - S(14), h);
    int off = 0;
    if (B.editing) {
        int cw = font_width(FONT_REGULAR, px, shown, B.acur), room = aw - (tx - bx) - S(24);
        if (cw > room) off = cw - room;
    }
    if (B.editing && B.asel) gfx_rrect(tx - S(3), cy - S(11), font_width(FONT_REGULAR, px, shown, -1) + S(6), S(22), S(4), th.accent, 110);
    text_l(tx - off, cy, shown, FONT_REGULAR, px, B.editing ? th.text : th.sub);
    if (B.editing && (now_ms() / 530) % 2 == 0) {
        int cx = tx - off + font_width(FONT_REGULAR, px, shown, B.acur);
        gfx_fill(cx, cy - S(9), S(2), S(18), th.accent);
    }
    gfx_clip_pop();
}

static const char *const tab_names[3] = { "All", "Images", "Videos" };

static int tab_x(int x, int i) {
    int tx = x;
    for (int k = 0; k < i; k++) tx += font_width(FONT_MEDIUM, S(13), tab_names[k], -1) + S(26);
    return tx;
}

static void draw_tabs(int x, int y) {
    for (int i = 0; i < 3; i++) {
        int tx = tab_x(x, i), tw = font_width(FONT_MEDIUM, S(13), tab_names[i], -1);
        int on = B.tab == i;
        text_l(tx, y + S(10), tab_names[i], on ? FONT_SEMIBOLD : FONT_MEDIUM, S(13), on ? th.text : th.sub);
        if (on) gfx_rrect(tx, y + S(22), tw, S(3), S(1), th.accent, 255);
    }
}

static const struct { const char *name, *url; uint32_t c; } quick[] = {
    { "DuckDuckGo", "https://html.duckduckgo.com/html/", 0xDE5833 },
    { "Wikipedia", "https://en.m.wikipedia.org/wiki/Main_Page", 0x6E6E6E },
    { "YouTube", "https://www.youtube.com/", 0xE62117 },
    { "Hacker News", "https://news.ycombinator.com/", 0xFF6600 },
    { "CNN Lite", "https://lite.cnn.com/", 0xCC0000 },
    { "NPR Text", "https://text.npr.org/", 0x3B6FB6 },
};

static void draw_start(int x, int y, int w, int h) {
    int cx = x + w / 2, top = y + h * 26 / 100;
    text_c(cx, top, "Where to?", FONT_DISPLAY, S(36), th.text);
    int bw = w * 62 / 100, bx = cx - bw / 2, by = top + S(36);
    draw_bar(bx, by, bw);
    /* where a search goes */
    int tw = tab_x(0, 3) - S(26);
    draw_tabs(cx - tw / 2, by + S(52));
    int n = (int)(sizeof quick / sizeof quick[0]), ts = S(54), gap = S(34);
    int total = n * ts + (n - 1) * gap, qx = cx - total / 2, qy = by + S(118);
    for (int i = 0; i < n; i++) {
        int tx = qx + i * (ts + gap);
        int f = B.focus == i;
        gfx_rrect(tx, qy, ts, ts, ts * 26 / 100, quick[i].c, 255);
        if (f) gfx_rrect_stroke(tx - S(4), qy - S(4), ts + S(8), ts + S(8), ts * 30 / 100, 2, th.accent, 255);
        char l[2] = { quick[i].name[0], 0 };
        text_c(tx + ts / 2, qy + ts / 2, l, FONT_SEMIBOLD, S(22), 0xFFFFFF);
        text_c(tx + ts / 2, qy + ts + S(16), quick[i].name, FONT_MEDIUM, S(11), th.sub);
        add_hit(tx, qy, ts, ts + S(26), H_QUICK, quick[i].url, 0, i);
    }
    text_c(cx, y + h - S(22), "Type to search  \xB7  Tab picks a site  \xB7  Shift+\x8D/\x81 where it goes", FONT_REGULAR, S(11), th.faint);
}

static void draw_results(int x, int w, int top, int bottom) {
    int yy = 0, pad = S(4);
    for (int i = 0; i < B.nitems; i++) {
        item_t *it = &B.items[i];
        int lines = wrap_lines(it->text, FONT_REGULAR, S(13), w - 2 * pad, 3, 0, -20000, 0, 0);
        int ih = S(24) + S(20) + lines * S(19) + S(18);
        it->x = 0; it->y = yy; it->w = w; it->h = ih;
        int sy = top + yy - B.scroll;
        if (sy + ih >= top && sy <= bottom) {
            if (B.focus >= 0 && hits[B.focus].item == i)
                gfx_rrect(x - S(8), sy - S(6), w + S(16), ih - S(6), S(12), th.accent, th.dark ? 46 : 36);
            font_draw_fit(FONT_REGULAR, S(12), x + pad, sy + S(12), it->sub, w - 2 * pad, th.dark ? 0x7FD49B : 0x188038, 255);
            font_draw_fit(FONT_MEDIUM, S(17), x + pad, sy + S(36), it->title, w - 2 * pad, th.dark ? 0x9DB8FF : 0x1A0DAB, 255);
            wrap_lines(it->text, FONT_REGULAR, S(13), w - 2 * pad, 3, x + pad, sy + S(58), S(19), th.sub);
        }
        add_hit(x, yy, w, ih, H_ITEM, it->href, 0, i);
        yy += ih;
    }
    B.doc_h = yy;
}

static void draw_images(int x, int y, int w, int top, int bottom) {
    (void)y;
    int rh = S(150), gap = S(8), yy = 0;
    for (int i = 0; i < B.nitems; ) {
        /* a justified row: as many as fit at height rh, then stretched to the width */
        int j = i, sum = 0;
        while (j < B.nitems) {
            int iw = rh * B.items[j].aw / B.items[j].ah;
            if (iw < rh / 2) iw = rh / 2;
            if (iw > rh * 3) iw = rh * 3;
            if (j > i && sum + gap + iw > w) break;
            sum += (j > i ? gap : 0) + iw; j++;
        }
        int gaps = (j - i - 1) * gap, raw = sum - gaps;
        int h = (j < B.nitems && raw > 0) ? rh * (w - gaps) / raw : rh;
        if (h > rh * 3 / 2) h = rh;
        int xx = 0;
        for (int k = i; k < j; k++) {
            item_t *it = &B.items[k];
            int iw = h * it->aw / it->ah;
            if (iw < h / 2) iw = h / 2;
            if (iw > h * 3) iw = h * 3;
            if (k == j - 1 && j < B.nitems) iw = w - xx;
            it->x = xx; it->y = yy; it->w = iw; it->h = h;
            int sy = top + yy - B.scroll;
            if (sy + h >= top && sy <= bottom) {
                if (it->pix) draw_image_cover(it->pix, x + xx, sy, iw, h, S(8));
                else gfx_rrect(x + xx, sy, iw, h, S(8), th.dark ? 0xFFFFFF : 0x000000, th.dark ? 18 : 12);
                if (B.focus >= 0 && hits[B.focus].item == k) gfx_rrect_stroke(x + xx - 2, sy - 2, iw + 4, h + 4, S(10), 3, th.accent, 255);
            }
            add_hit(x + xx, yy, iw, h, H_ITEM, it->href, 0, k);
            xx += iw + gap;
        }
        yy += h + gap;
        i = j;
    }
    B.doc_h = yy;
}

static void draw_videos(int x, int y, int w, int top, int bottom) {
    (void)y;
    int cols = 3, gap = S(18);
    int cw = (w - (cols - 1) * gap) / cols, thh = cw * 9 / 16, ch = thh + S(86), yy = 0;
    for (int i = 0; i < B.nitems; i++) {
        item_t *it = &B.items[i];
        int c = i % cols, cx = c * (cw + gap);
        if (c == 0 && i) yy += ch;
        it->x = cx; it->y = yy; it->w = cw; it->h = ch;
        int sy = top + yy - B.scroll;
        if (sy + ch >= top && sy <= bottom) {
            int f = B.focus >= 0 && hits[B.focus].item == i;
            if (f) gfx_rrect(x + cx - S(6), sy - S(6), cw + S(12), ch - S(4), S(14), th.accent, th.dark ? 50 : 40);
            if (it->pix) draw_image_cover(it->pix, x + cx, sy, cw, thh, S(10));
            else gfx_rrect(x + cx, sy, cw, thh, S(10), th.dark ? 0xFFFFFF : 0x000000, th.dark ? 18 : 12);
            if (it->badge[0]) {
                int bw = font_width(FONT_SEMIBOLD, S(11), it->badge, -1) + S(10);
                int live = !strcmp(it->badge, "LIVE");
                gfx_rrect(x + cx + cw - bw - S(6), sy + thh - S(24), bw, S(18), S(4), live ? 0xCC0000 : 0x000000, live ? 255 : 190);
                text_l(x + cx + cw - bw - S(1), sy + thh - S(15), it->badge, FONT_SEMIBOLD, S(11), 0xFFFFFF);
            }
            wrap_lines(it->title, FONT_MEDIUM, S(13), cw, 2, x + cx, sy + thh + S(20), S(18), th.text);
            font_draw_fit(FONT_REGULAR, S(11), x + cx, sy + thh + S(58), it->sub, cw, th.sub, 255);
            font_draw_fit(FONT_REGULAR, S(11), x + cx, sy + thh + S(74), it->text, cw, th.faint, 255);
        }
        add_hit(x + cx, yy, cw, ch - S(10), H_ITEM, it->href, 0, i);
    }
    B.doc_h = B.nitems ? yy + ch : 0;
}

static void draw_watch(int x, int y, int w, int h) {
    int pw = w, ph = pw * 9 / 16;
    if (ph > h - S(110)) { ph = h - S(110); pw = ph * 16 / 9; }
    int px = x + (w - pw) / 2, py = y;
    gfx_rrect(px, py, pw, ph, S(12), 0x000000, 255);
    const surface_t *f = player_frame();
    char msg[160] = "";
    int st = player_status(msg, sizeof msg);
    if (f && f->px) {
        int fw = pw, fh = fw * f->h / f->w;
        if (fh > ph) { fh = ph; fw = fh * f->w / f->h; }
        gfx_surface_scaled(px + (pw - fw) / 2, py + (ph - fh) / 2, fw, fh, f, 0, 255);
    } else if (B.nitems && B.items[0].pix) draw_image_cover(B.items[0].pix, px, py, pw, ph, S(12));
    if (st != 2 || !f) {                                        /* the state, over the picture */
        gfx_rrect(px, py, pw, ph, S(12), 0x000000, f || (B.nitems && B.items[0].pix) ? 110 : 0);
        int cx = px + pw / 2, cy = py + ph / 2;
        if (st == 3 || st == 0) {                               /* a play button */
            gfx_circle((float)cx, (float)cy, (float)S(30), 0xFFFFFF, 230);
            float k = (float)S(1);
            for (float t = 0; t <= 22 * k; t += 0.7f)
                gfx_line(cx - 7 * k + t * 0.85f, cy - 11 * k + t / 2, cx - 7 * k + t * 0.85f, cy + 11 * k - t / 2, 1.0f, 0x111111, 255);
        }
        if (msg[0]) text_c(cx, cy + S(st == 3 || st == 0 ? 52 : 0), msg, FONT_MEDIUM, S(14), 0xFFFFFF);
    }
    int len = player_len_ms(), pos = player_pos_ms();
    if (len > 0) {                                               /* progress */
        int bw = pw - S(32), bx = px + S(16), byy = py + ph - S(16);
        gfx_rrect(bx, byy, bw, S(4), S(2), 0xFFFFFF, 90);
        gfx_rrect(bx, byy, (int)((long)bw * pos / len), S(4), S(2), 0xFF3B30, 255);
        char t[40]; snprintf(t, sizeof t, "%d:%02d / %d:%02d", pos / 60000, pos / 1000 % 60, len / 60000, len / 1000 % 60);
        text_r(px + pw - S(16), byy - S(12), t, FONT_MEDIUM, S(11), 0xFFFFFF);
    }
    int ty = py + ph + S(28);
    font_draw_fit(FONT_SEMIBOLD, S(18), px, ty, B.title[0] ? B.title : "YouTube", pw, th.text, 255);
    if (B.meta[0]) font_draw_fit(FONT_REGULAR, S(12), px, ty + S(22), B.meta, pw, th.sub, 255);
    text_l(px, ty + S(46), "Space plays and pauses  \xB7  \x8D \x81 ten seconds  \xB7  Backspace goes back", FONT_REGULAR, S(11), th.faint);
}

static void draw_picture(int x, int y, int w, int h) {
    item_t *it = &B.items[B.picked];
    gfx_rrect(x, y, w, h, S(14), 0x000000, 200);
    image_t *im = it->pix;
    if (im) {
        int iw = w - S(40), ih = iw * im->h / im->w;
        if (ih > h - S(90)) { ih = h - S(90); iw = ih * im->w / im->h; }
        draw_image(im, x + (w - iw) / 2, y + S(20), iw, ih, S(6));
    }
    font_draw_fit(FONT_SEMIBOLD, S(14), x + S(20), y + h - S(44), it->title, w - S(40), 0xFFFFFF, 255);
    char l[300]; snprintf(l, sizeof l, "%s  \xB7  %s  \xB7  Return opens the page, Backspace goes back", it->sub, it->badge);
    font_draw_fit(FONT_REGULAR, S(11), x + S(20), y + h - S(22), l, w - S(40), 0xD0D0D8, 255);
}

static void b_draw(int x, int y, int w, int h) {
    nhits = 0;
    int pad = S(16);
    if (B.view == V_START) {
        draw_start(x, y, w, h);
    } else {
        draw_bar(x + pad, y + pad, w - 2 * pad);
        int top = y + pad + S(46);
        if (B.view == V_RESULTS || B.view == V_IMAGES || B.view == V_VIDEOS) {
            draw_tabs(x + pad + S(6), top);
            top += S(34);
        }
        int bottom = y + h - S(24);
        int cx = x + pad, cw = w - 2 * pad;
        B.view_h = bottom - top;
        gfx_clip_push(x, top, w, bottom - top);
        switch (B.view) {
        case V_WEB:
            if (B.dom) {
                int pw = cw;
                ensure_layout(pw - 2 * S(14));
                gfx_rrect(cx, top, pw, bottom - top, S(12), B.paper, 255);
                gfx_clip_push(cx, top, pw, bottom - top);
                paint_page(cx + S(14), top + S(10) - B.scroll, top, bottom);
                B.doc_h = B.fl.doc_h + S(30);
                for (int i = 0; i < nhits; i++) { hits[i].x += cx + S(14); hits[i].y += S(10); }
                gfx_clip_pop();
            }
            break;
        case V_RESULTS: draw_results(cx + S(8), cw - S(16), top + S(8), bottom); break;
        case V_IMAGES:  draw_images(cx, top, cw, top + S(4), bottom); break;
        case V_VIDEOS:  draw_videos(cx + S(4), top, cw - S(8), top + S(10), bottom); break;
        case V_WATCH:   draw_watch(cx, top, cw, bottom - top); B.doc_h = 0; break;
        case V_MESSAGE:
            text_c(x + w / 2, top + (bottom - top) / 2 - S(14), B.title, FONT_SEMIBOLD, S(20), th.text);
            text_c(x + w / 2, top + (bottom - top) / 2 + S(16), B.desc, FONT_REGULAR, S(13), th.sub);
            B.doc_h = 0;
            break;
        }
        if (B.view == V_IMAGES && B.picked >= 0 && B.picked < B.nitems) draw_picture(cx, top, cw, bottom - top);
        gfx_clip_pop();
        /* the scroll position, as a thin thumb */
        if (B.doc_h > B.view_h && B.view != V_WATCH) {
            int th_h = (bottom - top) * B.view_h / B.doc_h, th_y = top + (int)((long)(bottom - top - th_h) * B.scroll / (B.doc_h - B.view_h));
            gfx_rrect(x + w - S(7), th_y, S(3), th_h < S(20) ? S(20) : th_h, S(2), th.text, 60);
        }
        if (B.pending) text_l(x + pad, y + h - S(12), B.status, FONT_MEDIUM, S(11), th.accent);
        else {
            char s[260];
            snprintf(s, sizeof s, "%s%s%s", B.title[0] && B.view == V_WEB ? B.title : "", B.title[0] && B.view == V_WEB && B.status[0] ? "  \xB7  " : "", B.status);
            font_draw_fit(FONT_REGULAR, S(11), x + pad, y + h - S(8), s, w - 2 * pad, th.faint, 255);
        }
    }
}

/* ---- work between frames ------------------------------------------------------------------- */
static int b_tick(void) {
    static uint64_t blink;
    if (B.pending) {
        if (!B.drawn_loading) { B.drawn_loading = 1; return 1; }
        B.pending = 0;
        do_load();
        return 1;
    }
    if (B.view == V_WATCH && player_tick()) return 1;
    /* One picture per frame: the visible ones first. */
    if (B.view == V_WEB) {
        for (int i = 0; i < nwimg; i++)
            if (!wimg[i].state && wimg[i].near) {
                wimg[i].im = fetch_image(wimg[i].url);
                wimg[i].state = wimg[i].im ? 1 : -1;
                if (wimg[i].im) B.relayout = 1;
                return 1;
            }
    } else if (B.items) {
        int vt = B.scroll - S(200), vb = B.scroll + B.view_h + S(300);
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < B.nitems; i++) {
                item_t *it = &B.items[i];
                if (it->pstate || !it->thumb[0]) continue;
                if (pass == 0 && (it->y + it->h < vt || it->y > vb)) continue;
                it->pix = fetch_image(it->thumb);
                it->pstate = it->pix ? 1 : -1;
                return 1;
            }
        if (B.picked >= 0 && B.picked < B.nitems) {             /* the full picture */
            item_t *it = &B.items[B.picked];
            if (it->full[0] && it->pstate != 2) {
                image_t *im = fetch_image(it->full);
                if (im) { if (it->pix) image_free(it->pix); it->pix = im; }
                it->pstate = 2;
                return 1;
            }
        }
    }
    if (B.editing || B.field) {
        uint64_t p = now_ms() / 530;
        if (p != blink) { blink = p; return 1; }
    }
    return 0;
}

/* ---- input ---------------------------------------------------------------------------------- */
static void scroll_by(int d) {
    B.scroll += d;
    int max = B.doc_h - B.view_h;
    if (B.scroll > max) B.scroll = max;
    if (B.scroll < 0) B.scroll = 0;
}

static void reveal(int i) {
    if (i < 0 || i >= nhits) return;
    if (hits[i].y < B.scroll) B.scroll = hits[i].y - S(10);
    else if (hits[i].y + hits[i].h > B.scroll + B.view_h) B.scroll = hits[i].y + hits[i].h - B.view_h + S(10);
    scroll_by(0);
}

static void activate(int i) {
    if (i < 0 || i >= nhits) return;
    hit_t *h = &hits[i];
    if (h->kind == H_QUICK) { open_url(h->href, 1); return; }
    if (h->kind == H_INPUT) { B.field = h->node; return; }
    if (h->kind == H_SUBMIT) { submit_form(h->node); return; }
    if (h->kind == H_ITEM) {
        item_t *it = &B.items[h->item];
        if (B.view == V_IMAGES) { if (B.picked == h->item) open_url(it->href, 1); else { B.picked = h->item; it->pstate = it->pix ? 1 : 0; } return; }
        if (B.view == V_VIDEOS) { go(V_WATCH, T_VIDEOS, it->href, 0, 1); return; }
        open_url(it->href, 1);
        return;
    }
    if (h->kind == H_LINK && h->href) {
        char u[1024]; url_resolve(B.url, h->href, u, sizeof u);
        if (h->href[0] == '#') { B.scroll = 0; return; }
        open_url(u, 1);
    }
}

static int focusable(int i) { return hits[i].kind != H_INPUT || B.view == V_WEB; }

static void move_focus(int dir) {
    if (!nhits) return;
    int i = B.focus;
    for (int k = 0; k < nhits; k++) {
        i = i < 0 ? (dir > 0 ? 0 : nhits - 1) : (i + dir + nhits) % nhits;
        if (focusable(i) && (i == 0 || hits[i].href != hits[i - 1].href || hits[i].kind != H_LINK)) break;
    }
    B.focus = i;
    reveal(i);
}

/* Arrows in a grid: the nearest item above, below or beside. */
static void grid_focus(int dx, int dy) {
    if (B.focus < 0) { move_focus(1); return; }
    hit_t *c = &hits[B.focus];
    int cx = c->x + c->w / 2, cy = c->y + c->h / 2, best = -1;
    long bd = 0;
    for (int i = 0; i < nhits; i++) {
        if (i == B.focus || hits[i].kind != H_ITEM) continue;
        int x = hits[i].x + hits[i].w / 2, y = hits[i].y + hits[i].h / 2;
        int ok = dx > 0 ? x > cx + 4 && y > c->y - 4 && y < c->y + c->h + 4 :
                 dx < 0 ? x < cx - 4 && y > c->y - 4 && y < c->y + c->h + 4 :
                 dy > 0 ? y > c->y + c->h - 4 : y < c->y + 4;
        if (!ok) continue;
        long d = (long)(x - cx) * (x - cx) * (dy ? 4 : 1) + (long)(y - cy) * (y - cy) * (dx ? 4 : 1);
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    if (best >= 0) { B.focus = best; reveal(best); }
}

static void edit_field(const input_ev_t *e) {
    const char *v = dom_attr_get(B.field, "value");
    char buf[600]; snprintf(buf, sizeof buf, "%s", v ? v : "");
    int n = (int)strlen(buf);
    if (e->key == KEY_CHAR && e->ch >= 32 && n < (int)sizeof buf - 1) { buf[n++] = (char)e->ch; buf[n] = 0; }
    else if (e->key == KEY_BACKSPACE && n) buf[--n] = 0;
    else if (e->key == KEY_ENTER) { submit_form(B.field); B.field = 0; return; }
    else if (e->key == KEY_TAB) { B.field = 0; move_focus(e->mods & MOD_SHIFT ? -1 : 1); return; }
    else return;
    dom_attr_set(B.dom, B.field, "value", buf);
}

static void edit_address(const input_ev_t *e) {
    if (B.asel) {                                   /* typing replaces a selected address */
        B.asel = 0;
        if ((e->key == KEY_CHAR && e->ch >= 32) || e->key == KEY_BACKSPACE || e->key == KEY_DELETE) {
            B.addr[0] = 0; B.alen = B.acur = 0;
            if (e->key != KEY_CHAR) return;
        }
    }
    switch (e->key) {
    case KEY_CHAR:
        if (e->ch >= 32 && B.alen < (int)sizeof B.addr - 1 && !(e->mods & MOD_CTRL)) {
            memmove(B.addr + B.acur + 1, B.addr + B.acur, (size_t)(B.alen - B.acur + 1));
            B.addr[B.acur++] = (char)e->ch; B.alen++;
        }
        break;
    case KEY_BACKSPACE:
        if (B.acur) { memmove(B.addr + B.acur - 1, B.addr + B.acur, (size_t)(B.alen - B.acur + 1)); B.acur--; B.alen--; }
        break;
    case KEY_DELETE:
        if (B.acur < B.alen) { memmove(B.addr + B.acur, B.addr + B.acur + 1, (size_t)(B.alen - B.acur)); B.alen--; }
        break;
    case KEY_LEFT:  if (e->mods & MOD_SHIFT) { if (B.view == V_START) B.tab = (B.tab + 2) % 3; } else if (B.acur) B.acur--; break;
    case KEY_RIGHT: if (e->mods & MOD_SHIFT) { if (B.view == V_START) B.tab = (B.tab + 1) % 3; } else if (B.acur < B.alen) B.acur++; break;
    case KEY_HOME:  B.acur = 0; break;
    case KEY_END:   B.acur = B.alen; break;
    case KEY_ENTER: submit_address(); break;
    case KEY_TAB:
        if (B.view == V_START) { B.editing = 0; move_focus(e->mods & MOD_SHIFT ? -1 : 1); }
        break;
    case KEY_DOWN:
        if (B.view != V_START) { B.editing = 0; move_focus(1); }
        break;
    }
}

static void start_editing(int clear) {
    B.editing = 1; B.field = 0; B.asel = 0;
    if (clear) { B.addr[0] = 0; B.alen = B.acur = 0; }
    else { B.alen = B.acur = (int)strlen(B.addr); B.asel = B.alen > 0; }
}

static void b_key(const input_ev_t *e) {
    if (B.pending) return;
    if (e->key == KEY_CHAR && (e->mods & MOD_CTRL) && (e->ch == 'l' || e->ch == 'L')) { start_editing(0); return; }
    if (e->key == KEY_CHAR && (e->mods & MOD_CTRL) && (e->ch == 'r' || e->ch == 'R')) {
        if (B.view != V_START) go(B.view, B.tab, B.view == V_WATCH ? B.vid : B.url, B.query, 0);
        return;
    }
    if (B.editing) { edit_address(e); return; }
    if (B.field && B.view == V_WEB) { edit_field(e); return; }
    if ((e->mods & MOD_SHIFT) && (e->key == KEY_LEFT || e->key == KEY_RIGHT) &&
        (B.view == V_RESULTS || B.view == V_IMAGES || B.view == V_VIDEOS || B.view == V_START)) {
        set_tab((B.tab + (e->key == KEY_LEFT ? 2 : 1)) % 3);
        return;
    }
    if ((e->mods & MOD_ALT) && e->key == KEY_LEFT) { go_back(); return; }
    int grid = B.view == V_IMAGES || B.view == V_VIDEOS || B.view == V_START;
    int list = B.view == V_RESULTS;
    if (B.view == V_WATCH) {
        if (e->key == KEY_CHAR && (e->ch == ' ' || e->ch == 'k')) { player_toggle(); return; }
        if (e->key == KEY_LEFT) { player_seek(-10); return; }
        if (e->key == KEY_RIGHT) { player_seek(10); return; }
    }
    switch (e->key) {
    case KEY_BACKSPACE: go_back(); return;
    case KEY_TAB:   move_focus(e->mods & MOD_SHIFT ? -1 : 1); return;
    case KEY_ENTER:
        if (B.view == V_IMAGES && B.picked >= 0) { open_url(B.items[B.picked].href, 1); return; }
        activate(B.focus); return;
    case KEY_UP:    if (grid) grid_focus(0, -1); else if (list) move_focus(-1); else scroll_by(-S(48)); return;
    case KEY_DOWN:  if (grid) grid_focus(0, 1); else if (list) move_focus(1); else scroll_by(S(48)); return;
    case KEY_LEFT:  if (grid) grid_focus(-1, 0); return;
    case KEY_RIGHT: if (grid) grid_focus(1, 0); return;
    case KEY_PGUP:  scroll_by(-(B.view_h - S(40))); return;
    case KEY_PGDN:  scroll_by(B.view_h - S(40)); return;
    case KEY_HOME:  B.scroll = 0; return;
    case KEY_END:   scroll_by(1 << 28); return;
    case KEY_CHAR:
        if (e->ch == ' ') { scroll_by((e->mods & MOD_SHIFT) ? -(B.view_h - S(40)) : B.view_h - S(40)); return; }
        if (e->ch > ' ' && !(e->mods & (MOD_CTRL | MOD_ALT))) {      /* type to search */
            start_editing(1);
            edit_address(e);
        }
        return;
    }
}

static void b_pointer(const input_ev_t *e, int x, int y, int w, int h) {
    static int last_btn;
    int click = (e->buttons & 1) && !(last_btn & 1);
    last_btn = e->buttons;
    if (e->wheel) { scroll_by(-e->wheel * S(60)); return; }
    if (!click || B.pending) return;
    int pad = S(16);
    if (B.view == V_START) {
        int cx = x + w / 2, top = y + h * 26 / 100, bw = w * 62 / 100, by = top + S(36);
        if (ui_hit(e->x, e->y, cx - bw / 2, by, bw, S(38))) { start_editing(0); return; }
        int tw = tab_x(0, 3) - S(26), tx0 = cx - tw / 2;
        for (int i = 0; i < 3; i++)
            if (ui_hit(e->x, e->y, tab_x(tx0, i) - S(6), by + S(52), font_width(FONT_MEDIUM, S(13), tab_names[i], -1) + S(12), S(28))) { B.tab = i; return; }
        for (int i = 0; i < nhits; i++) if (ui_hit(e->x, e->y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) { activate(i); return; }
        return;
    }
    int bar_y = y + pad, back_w = S(38) + S(8);
    if (e->y >= bar_y && e->y < bar_y + S(38)) {
        if (e->x < x + pad + back_w) go_back(); else start_editing(0);
        return;
    }
    B.editing = 0;
    int top = y + pad + S(46);
    if (B.view == V_RESULTS || B.view == V_IMAGES || B.view == V_VIDEOS) {
        for (int i = 0; i < 3; i++)
            if (ui_hit(e->x, e->y, tab_x(x + pad + S(6), i) - S(6), top, font_width(FONT_MEDIUM, S(13), tab_names[i], -1) + S(12), S(28))) { set_tab(i); return; }
        top += S(34);
    }
    if (B.view == V_IMAGES && B.picked >= 0) { B.picked = -1; return; }
    if (B.view == V_WATCH) { player_toggle(); return; }
    int origin = (B.view == V_RESULTS ? S(8) : B.view == V_VIDEOS ? S(10) : B.view == V_IMAGES ? S(4) : 0);
    int py = e->y - top - origin + B.scroll;
    B.field = 0;
    for (int i = nhits - 1; i >= 0; i--)
        if (e->x >= hits[i].x && e->x < hits[i].x + hits[i].w && py >= hits[i].y && py < hits[i].y + hits[i].h) {
            B.focus = i;
            activate(i);
            return;
        }
}

/* Esc: leave the address being typed, a text field, a picture -- else the shell
 * takes the browser home (shell.c asks this first, as it does the Terminal). */
int browser_esc(void) {
    if (B.editing && B.view != V_START) { B.editing = 0; snprintf(B.addr, sizeof B.addr, "%s", B.query[0] ? B.query : B.url); return 1; }
    if (B.field) { B.field = 0; return 1; }
    if (B.view == V_IMAGES && B.picked >= 0) { B.picked = -1; return 1; }
    return 0;
}

static void b_open(void) {
    static int once;
    if (!once) {
        once = 1;
        B.view = V_START; B.focus = -1; B.picked = -1;
        start_editing(1);
    }
    if (B.view == V_START) B.editing = 1;
}

const app_t app_browser = { "Browser", 0x3478F6, G_GLOBE, b_open, b_key, b_pointer, b_draw, b_tick };
