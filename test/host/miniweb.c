/* BoltOS Mini's page layout on the host: an HTML file in, a PNG-ready PPM out.
 *
 * The same code the Pi runs -- kernel/dom.c, the cascade in kernel/layout.c,
 * mini/flow.c, the Inter faces through kernel/font.c and kernel/gfx.c -- with
 * the heap and the log stood in for. It paints what flow.c laid out the way
 * mini/app_browser.c does (images as grey boxes), so a layout change can be
 * looked at in a second instead of a boot of the Pi.
 *
 *   miniweb PAGE.html [CSS] [WIDTH] [HEIGHT] [SCROLL] > out.ppm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include "dom.h"
#include "layout.h"
#include "../../mini/flow.h"
#include "../../mini/web.h"
#include "gfx.h"
#include "font.h"

void *kmalloc(uint64_t n) { return malloc(n ? (size_t)n : 1); }
void  kfree(void *p) { free(p); }
void  kprintf(const char *fmt, ...) { (void)fmt; }
void  gui_request_redraw(void) { }

static char *slurp(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)*len + 64);
    fread(b, 1, (size_t)*len, f); fclose(f);
    b[*len] = 0;
    return b;
}

static dom_document *D;
static void decode(dom_node *n, int raw) {
    for (; n; n = n->next) {
        if (n->type == DOM_TEXT) { if (!raw && n->text) entities_decode(n->text); continue; }
        for (dom_attr *a = n->attrs; a; a = a->next) if (a->value) entities_decode(a->value);
        int r = raw || (n->tag && (!strcmp(n->tag, "script") || !strcmp(n->tag, "style")));
        if (n->tag && (!strcmp(n->tag, "svg") || !strcmp(n->tag, "template") || !strcmp(n->tag, "iframe")))
            dom_attr_set(D, n, "style", "display:none");
        if (n->tag && !strcmp(n->tag, "input")) {
            const char *t = dom_attr_get(n, "type");
            if (t && (!strcmp(t, "hidden") || !strcmp(t, "checkbox") || !strcmp(t, "radio"))) dom_attr_set(D, n, "style", "display:none");
        }
        decode(n->first_child, r);
    }
}

static void img_size(dom_node *n, int maxw, int *w, int *h, int *slot, void *ud) {
    (void)ud;
    *slot = -1; *w = *h = 0;
    const char *a = dom_attr_get(n, "width"), *b = dom_attr_get(n, "height");
    if (a && b) { *w = atoi(a); *h = atoi(b); }
    if (*w > maxw) { *h = *h * maxw / *w; *w = maxw; }
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: miniweb PAGE.html [CSS] [W] [H] [SCROLL]\n"); return 2; }
    long hl = 0, cl = 0;
    char *raw = slurp(argv[1], &hl);
    char *css = argc > 2 && strcmp(argv[2], "-") ? slurp(argv[2], &cl) : 0;
    int W = argc > 3 ? atoi(argv[3]) : 948, H = argc > 4 ? atoi(argv[4]) : 2000, scroll = argc > 5 ? atoi(argv[5]) : 0;
    if (!raw) return 1;
    char *html = malloc((size_t)hl + 1);
    int n = utf8_to_cp1252(raw, (int)hl, html, (int)hl + 1);
    font_init();
    D = dom_parse(html, (uint32_t)n);
    decode(D->root, 0);
    layout_set_media_width(W);
    layout_tree *lt = layout_styles(D, css, (uint32_t)cl);
    flow_t f = { 0 };
    flow_layout(&f, lt->root, W - 28, img_size, 0);
    FILE *log = fopen("/dev/stderr", "w");                 /* the kernel's stdio.h has no stderr */
    if (log) {
        fprintf(log, "%d items, %d px tall\n", f.n, f.doc_h);
        const char *want = getenv("WORD");
        for (int i = 0; want && i < f.n; i++)
            if (f.it[i].kind == FL_TEXT && f.it[i].len == (int)strlen(want) && !memcmp(f.it[i].s, want, (size_t)f.it[i].len))
                fprintf(log, "%s: colour %06x, face %d, %d px, at %d,%d\n", want, f.it[i].color, f.it[i].face, f.it[i].px, f.it[i].x, f.it[i].y);
        fclose(log);
    }

    static uint32_t px[1200 * 4000];
    surface_t s = { px, W, H, W };
    gfx_bind(&s, 0, 0, 0, 0, W, H);
    gfx_fill(0, 0, W, H, f.paper);
    int ox = 14, oy = 10 - scroll;
    for (int i = 0; i < f.n; i++) {
        fl_item *it = &f.it[i];
        int sx = ox + it->x, sy = oy + it->y;
        switch (it->kind) {
        case FL_RECT:  gfx_fill(sx, sy, it->w, it->h, it->color); break;
        case FL_TEXT:
            if (it->flags & FLF_ITALIC) font_draw_italic(it->face, it->px, sx, sy + it->a, it->s, it->len, it->color, 255);
            else font_draw(it->face, it->px, sx, sy + it->a, it->s, it->len, it->color, 255);
            break;
        case FL_BULLET:
            if (it->len) font_draw(FONT_REGULAR, 15, sx, sy, it->num, it->len, it->color, 255);
            else gfx_circle((float)(sx + 6), (float)(sy - 5), 2.6f, it->color, 255);
            break;
        case FL_IMAGE:   gfx_fill(sx, sy, it->w, it->h, 0xDDE3EA); break;
        case FL_CONTROL: gfx_rrect(sx, sy, it->w, it->h, 6, 0xF1F3F4, 255); gfx_rrect_stroke(sx, sy, it->w, it->h, 6, 1, 0xC4C7C5, 255); break;
        }
    }
    printf("P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) { putchar((px[i] >> 16) & 255); putchar((px[i] >> 8) & 255); putchar(px[i] & 255); }
    return 0;
}
