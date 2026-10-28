#pragma once
#include <stdint.h>
#include "layout.h"

/* ===========================================================================
 *  BoltOS Mini  -  mini/flow.h
 *
 *  Mini's page layout. The desktop's engine (kernel/layout.c) gives the
 *  cascade -- each element's display, colours, margins, size class -- and
 *  this lays the styled boxes out the way pages expect: words flowing into
 *  line boxes across links and inline styles, tables in columns, lists with
 *  their markers, a flex row side by side. The result is a flat list of
 *  things to draw, in page coordinates, which the browser paints and
 *  hit-tests.
 * ===========================================================================*/
enum { FL_TEXT, FL_RECT, FL_IMAGE, FL_CONTROL, FL_BULLET };
enum { FLF_ITALIC = 1, FLF_LINK = 2 };

typedef struct {
    uint8_t     kind, face, flags, px;
    uint32_t    color;
    int         x, y, w, h;
    int         a;                     /* text: the baseline is y + a              */
    const char *s; int len;            /* text, or a control's label                */
    const char *href;                  /* the link it is part of                    */
    dom_node   *node;                  /* an image's or a control's element         */
    int         img;                   /* the browser's image slot, or -1           */
    char        num[8];                /* a numbered list item's "12."             */
} fl_item;

typedef struct {
    fl_item *it; int n, cap;
    int      doc_h;
    uint32_t paper;                    /* the page's own background                 */
} flow_t;

/* Sizes an <img>: its slot in the browser's image list, and the size to lay
 * it out at no wider than maxw (0 x 0 while it is unknown). */
typedef void (*flow_img_fn)(dom_node *n, int maxw, int *w, int *h, int *slot, void *ud);

int  flow_layout(flow_t *f, layout_box *root, int width, flow_img_fn img, void *ud);
void flow_free(flow_t *f);
int  flow_px(int fscale);              /* the type size a CSS size class draws at  */
