/* ===========================================================================
 *  BoltStrike -- every pixel in the game, made at startup.
 *
 *  There is no art file anywhere in this tree and there is not going to be
 *  one. Partly that is taste -- boltcraft does the same and it is why it fits
 *  in 90 KiB -- and partly it is the only honest way to build a game in this
 *  shape at all: the obvious textures to use here belong to someone else.
 *  So: sandstone is banded value noise with mortar courses, brick is an offset
 *  grid, crates are planks with nails in them, and the soldiers are built out
 *  of a dozen rounded boxes each.
 *
 *  About 430 KiB of ARGB when it is all resident, which is why it is on the
 *  heap and freed when the window closes.
 * ===========================================================================*/
#include <stdint.h>
#include "strike.h"
#include "kheap.h"
#include "math.h"
#include "string.h"

uint32_t *st_wall_tex[C_COUNT];
uint32_t *st_ply_spr[2][SPR_VIEWS][SPR_FRAMES];
uint32_t *st_item_spr[IT_COUNT];
uint32_t *st_view_spr[16];

/* ---- small deterministic noise ------------------------------------------ *
 *  Not rand(): these have to come out the same every launch, and the shared
 *  generator's state is not mine to disturb. */
static uint32_t hash2(int x, int y, uint32_t s) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + s * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float hnoise(int x, int y, uint32_t s) { return (float)(hash2(x, y, s) & 0xFFFF) / 65535.0f; }

/* Bilinear value noise on a lattice of the given cell size. */
static float vnoise(float x, float y, int cell, uint32_t s) {
    float fx = x / (float)cell, fy = y / (float)cell;
    int   ix = (int)fx, iy = (int)fy;
    float tx = fx - (float)ix, ty = fy - (float)iy;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    float a = hnoise(ix, iy, s),       b = hnoise(ix + 1, iy, s);
    float c = hnoise(ix, iy + 1, s),   d = hnoise(ix + 1, iy + 1, s);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

static uint32_t rgb(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static uint32_t shade(uint32_t c, float f) {
    return rgb((int)(((c >> 16) & 0xFF) * f),
               (int)(((c >> 8)  & 0xFF) * f),
               (int)(( c        & 0xFF) * f));
}

/* ---- drawing into a sprite box ------------------------------------------ */
typedef struct { uint32_t *p; int w, h; } canvas;

static void px(canvas *c, int x, int y, uint32_t col) {
    if ((unsigned)x < (unsigned)c->w && (unsigned)y < (unsigned)c->h) c->p[y * c->w + x] = col;
}
static void boxf(canvas *c, int x0, int y0, int x1, int y1, uint32_t col) {
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) px(c, x, y, col);
}
/* A filled ellipse inscribed in the box. Every rounded thing in here is one. */
static void ellf(canvas *c, int x0, int y0, int x1, int y1, uint32_t col) {
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    float rx = (x1 - x0) * 0.5f + 0.5f, ry = (y1 - y0) * 0.5f + 0.5f;
    if (rx < 0.5f) rx = 0.5f;
    if (ry < 0.5f) ry = 0.5f;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = ((float)x - cx) / rx, dy = ((float)y - cy) / ry;
            if (dx * dx + dy * dy <= 1.0f) px(c, x, y, col);
        }
    }
}
/* Rounded box: the shape almost every body part actually wants. */
static void rbox(canvas *c, int x0, int y0, int x1, int y1, int r, uint32_t col) {
    if (r <= 0) { boxf(c, x0, y0, x1, y1, col); return; }
    boxf(c, x0 + r, y0, x1 - r, y1, col);
    boxf(c, x0, y0 + r, x1, y1 - r, col);
    ellf(c, x0, y0, x0 + 2 * r, y0 + 2 * r, col);
    ellf(c, x1 - 2 * r, y0, x1, y0 + 2 * r, col);
    ellf(c, x0, y1 - 2 * r, x0 + 2 * r, y1, col);
    ellf(c, x1 - 2 * r, y1 - 2 * r, x1, y1, col);
}

/* ---- walls --------------------------------------------------------------- */
static void tex_sandstone(uint32_t *t) {
    canvas c = { t, TEXW, TEXW };
    for (int y = 0; y < TEXW; y++) {
        for (int x = 0; x < TEXW; x++) {
            float n = vnoise((float)x, (float)y, 7, 11) * 0.55f
                    + vnoise((float)x, (float)y, 3, 23) * 0.30f
                    + hnoise(x, y, 31) * 0.15f;
            int base = 150 + (int)(n * 46.0f);
            /* Courses 16 high, blocks 32 wide, every other course offset --
             * the offset is the whole difference between masonry and a grid. */
            int course = y / 16;
            int off    = (course & 1) ? 16 : 0;
            int inY    = y % 16, inX = (x + off) % 32;
            uint32_t col = rgb(base, (int)(base * 0.86f), (int)(base * 0.62f));
            if (inY == 0 || inY == 15 || inX == 0 || inX == 31)
                col = rgb((int)(base * 0.62f), (int)(base * 0.55f), (int)(base * 0.42f));
            else if (inY == 1)
                col = rgb((int)(base * 1.12f), (int)(base * 0.97f), (int)(base * 0.72f));
            px(&c, x, y, col);
        }
    }
}

static void tex_brick(uint32_t *t) {
    canvas c = { t, TEXW, TEXW };
    for (int y = 0; y < TEXW; y++) {
        for (int x = 0; x < TEXW; x++) {
            int course = y / 8;
            int off    = (course & 1) ? 8 : 0;
            int inY = y % 8, inX = (x + off) % 16;
            float n = hnoise(x / 2, y / 2, 5) * 0.4f + vnoise((float)x, (float)y, 5, 9) * 0.6f;
            /* Brick colour varies per brick, not per pixel, or it looks like
             * gravel instead of a wall. */
            float bn = hnoise(x / 16 + off, course, 77);
            int r = 132 + (int)(bn * 46.0f) + (int)(n * 16.0f);
            uint32_t col = rgb(r, (int)(r * 0.48f), (int)(r * 0.38f));
            if (inY == 0 || inX == 0)
                col = rgb(146 + (int)(n * 20.0f), 140 + (int)(n * 20.0f), 126 + (int)(n * 20.0f));
            px(&c, x, y, col);
        }
    }
}

static void tex_plaster(uint32_t *t) {
    canvas c = { t, TEXW, TEXW };
    for (int y = 0; y < TEXW; y++) {
        for (int x = 0; x < TEXW; x++) {
            float n = vnoise((float)x, (float)y, 11, 3) * 0.7f + hnoise(x, y, 41) * 0.3f;
            int v = 176 + (int)(n * 40.0f);
            px(&c, x, y, rgb(v, (int)(v * 0.95f), (int)(v * 0.84f)));
        }
    }
    /* Two cracks, walked rather than drawn straight. */
    for (int k = 0; k < 2; k++) {
        int x = 12 + k * 30, y = 0;
        while (y < TEXW) {
            px(&c, x, y, rgb(96, 88, 76));
            px(&c, x + 1, y, rgb(132, 124, 110));
            y++;
            x += (int)(hnoise(x, y, 60 + k) * 3.0f) - 1;
            if (x < 1) x = 1;
            if (x > TEXW - 3) x = TEXW - 3;
        }
    }
    boxf(&c, 0, 44, TEXW - 1, 45, rgb(120, 112, 100));   /* a dado line       */
}

static void tex_crate(uint32_t *t) {
    canvas c = { t, TEXW, TEXW };
    for (int y = 0; y < TEXW; y++) {
        for (int x = 0; x < TEXW; x++) {
            float grain = vnoise((float)x * 3.0f, (float)y * 0.4f, 6, 17);
            int v = 128 + (int)(grain * 42.0f);
            uint32_t col = rgb(v, (int)(v * 0.72f), (int)(v * 0.42f));
            if (y % 16 == 0 || y % 16 == 15) col = shade(col, 0.62f);   /* plank gaps */
            px(&c, x, y, col);
        }
    }
    /* frame */
    boxf(&c, 0, 0, TEXW - 1, 3, rgb(112, 78, 42));
    boxf(&c, 0, TEXW - 4, TEXW - 1, TEXW - 1, rgb(112, 78, 42));
    boxf(&c, 0, 0, 3, TEXW - 1, rgb(112, 78, 42));
    boxf(&c, TEXW - 4, 0, TEXW - 1, TEXW - 1, rgb(112, 78, 42));
    for (int i = 0; i < 4; i++) {                         /* nails            */
        int nx = (i & 1) ? TEXW - 8 : 6, ny = (i & 2) ? TEXW - 8 : 6;
        ellf(&c, nx, ny, nx + 2, ny + 2, rgb(190, 188, 180));
    }
}

static void tex_door(uint32_t *t) {
    canvas c = { t, TEXW, TEXW };
    for (int y = 0; y < TEXW; y++)
        for (int x = 0; x < TEXW; x++) {
            int v = 96 + (int)(vnoise((float)x, (float)y, 9, 13) * 26.0f);
            /* vertical brushing, because a flat grey panel reads as a bug */
            v += (x % 3 == 0) ? 6 : 0;
            px(&c, x, y, rgb(v, v + 4, v + 10));
        }
    boxf(&c, 30, 0, 33, TEXW - 1, rgb(58, 60, 68));       /* the seam         */
    for (int i = 0; i < 6; i++) {                          /* rivets           */
        ellf(&c, 6, 6 + i * 10, 9, 9 + i * 10, rgb(150, 152, 160));
        ellf(&c, TEXW - 10, 6 + i * 10, TEXW - 7, 9 + i * 10, rgb(150, 152, 160));
    }
    boxf(&c, 0, 0, TEXW - 1, 2, rgb(52, 54, 60));
    boxf(&c, 0, TEXW - 3, TEXW - 1, TEXW - 1, rgb(52, 54, 60));
}

/* ---- the soldiers -------------------------------------------------------- *
 *  Five views, mirrored to eight facings. Each is a stack of rounded boxes:
 *  boots, legs, webbing, torso, arms, head, and whatever is in the hands. The
 *  quarter views are the front view with the shoulders narrowed and the gun
 *  swung across, which is not how a real sprite sheet is drawn and is entirely
 *  good enough at the size these are ever seen. */
static void draw_soldier(uint32_t *buf, int team, int view, int frame) {
    canvas c = { buf, SPR_W, SPR_H };
    for (int i = 0; i < SPR_W * SPR_H; i++) buf[i] = 0;    /* alpha 0 = nothing */

    uint32_t skin  = rgb(196, 152, 116);
    uint32_t cloth = team == T_SIDE ? rgb(148, 122, 74) : rgb(52, 62, 92);
    uint32_t cloth2= team == T_SIDE ? rgb(118,  96, 56) : rgb(38, 46, 70);
    uint32_t vest  = team == T_SIDE ? rgb(92, 76, 48)   : rgb(34, 40, 58);
    uint32_t boot  = rgb(46, 40, 34);
    uint32_t head  = team == T_SIDE ? rgb(58, 52, 48) : rgb(44, 54, 78);  /* mask / helmet */
    uint32_t gun   = rgb(48, 48, 52);
    uint32_t gunw  = rgb(92, 66, 40);

    int cx = SPR_W / 2;

    if (frame == 2) {                                      /* a body on the floor */
        rbox(&c, 4, SPR_H - 16, SPR_W - 5, SPR_H - 6, 4, cloth);
        rbox(&c, 6, SPR_H - 14, SPR_W - 12, SPR_H - 9, 2, vest);
        ellf(&c, SPR_W - 13, SPR_H - 15, SPR_W - 5, SPR_H - 8, head);
        boxf(&c, 8, SPR_H - 7, SPR_W - 14, SPR_H - 5, boot);
        return;
    }

    int narrow = (view == 1 || view == 3) ? 3 : (view == 2 ? 6 : 0);
    int swing  = frame == 0 ? 2 : -2;                      /* the walk cycle     */

    /* legs */
    rbox(&c, cx - 8 + narrow / 2, 34, cx - 2 + narrow / 2, 50 + swing, 2, cloth2);
    rbox(&c, cx + 1 - narrow / 2, 34, cx + 7 - narrow / 2, 50 - swing, 2, cloth2);
    boxf(&c, cx - 9 + narrow / 2, 49 + swing, cx - 1 + narrow / 2, 52 + swing, boot);
    boxf(&c, cx + 0 - narrow / 2, 49 - swing, cx + 8 - narrow / 2, 52 - swing, boot);

    /* torso + webbing */
    rbox(&c, cx - 9 + narrow, 18, cx + 9 - narrow, 37, 4, cloth);
    rbox(&c, cx - 7 + narrow, 21, cx + 7 - narrow, 33, 3, vest);
    boxf(&c, cx - 7 + narrow, 26, cx + 7 - narrow, 27, shade(vest, 0.7f));
    /* pouches, so the silhouette is not a rectangle */
    boxf(&c, cx - 6 + narrow, 29, cx - 3 + narrow, 33, shade(vest, 1.25f));
    boxf(&c, cx + 2 - narrow, 29, cx + 5 - narrow, 33, shade(vest, 1.25f));

    /* head: mask for T, helmet for CT, and a face only when facing you */
    ellf(&c, cx - 6, 6, cx + 6, 19, head);
    if (view <= 1) {
        ellf(&c, cx - 4, 11, cx + 4, 16, skin);
        px(&c, cx - 2, 13, rgb(28, 24, 22));
        px(&c, cx + 2, 13, rgb(28, 24, 22));
    }
    if (team == CT_SIDE) {                                  /* helmet brim      */
        boxf(&c, cx - 7, 8, cx + 7, 10, shade(head, 1.2f));
        ellf(&c, cx - 7, 4, cx + 7, 12, shade(head, 1.1f));
    }

    /* arms and the weapon. On the back views the gun is hidden by the body,
     * which is the cheapest possible occlusion and reads correctly. */
    if (view <= 2) {
        rbox(&c, cx - 12 + narrow, 20, cx - 7 + narrow, 32, 2, cloth);
        rbox(&c, cx + 6 - narrow, 20, cx + 11 - narrow, 32, 2, cloth);
        int gx = cx + 2 - narrow;
        boxf(&c, gx, 24, gx + 14 - narrow, 27, gun);        /* barrel           */
        boxf(&c, gx + 2, 27, gx + 5, 32, gunw);             /* grip             */
        boxf(&c, gx - 3, 23, gx + 1, 28, shade(gun, 1.3f)); /* receiver         */
    } else {
        rbox(&c, cx - 11 + narrow, 20, cx - 6 + narrow, 32, 2, cloth);
        rbox(&c, cx + 5 - narrow, 20, cx + 10 - narrow, 32, 2, cloth);
        boxf(&c, cx - 3, 22, cx + 3, 34, vest);             /* a pack           */
    }
}

static void draw_bomb(uint32_t *buf) {
    canvas c = { buf, ITEM_W, ITEM_H };
    for (int i = 0; i < ITEM_W * ITEM_H; i++) buf[i] = 0;
    rbox(&c, 3, 7, ITEM_W - 4, ITEM_H - 3, 2, rgb(38, 38, 42));
    boxf(&c, 5, 9, ITEM_W - 6, 13, rgb(20, 60, 24));        /* the display      */
    boxf(&c, 6, 10, 8, 12, rgb(80, 240, 90));
    boxf(&c, 9, 10, 11, 12, rgb(80, 240, 90));
    ellf(&c, ITEM_W - 9, 16, ITEM_W - 6, 19, rgb(230, 60, 40));   /* the light  */
    boxf(&c, 5, 16, 12, 18, rgb(120, 88, 40));              /* taped wires      */
    boxf(&c, 5, 19, 14, 20, rgb(160, 40, 40));
}

static void draw_weapon_pickup(uint32_t *buf) {
    canvas c = { buf, ITEM_W, ITEM_H };
    for (int i = 0; i < ITEM_W * ITEM_H; i++) buf[i] = 0;
    boxf(&c, 2, 12, ITEM_W - 3, 15, rgb(48, 48, 52));
    boxf(&c, 6, 15, 10, ITEM_H - 6, rgb(92, 66, 40));
    boxf(&c, 12, 10, 16, 14, rgb(70, 70, 76));
}

/* ---- the gun in your hands ---------------------------------------------- *
 *  Drawn from the shooter's eye: the receiver bottom-right, the barrel going
 *  away from you towards the middle of the screen, and a hand on the grip. */
static void draw_viewmodel(uint32_t *buf, int wep) {
    canvas c = { buf, VIEW_W, VIEW_H };
    for (int i = 0; i < VIEW_W * VIEW_H; i++) buf[i] = 0;

    uint32_t steel = rgb(62, 64, 70), steel2 = rgb(86, 90, 98);
    uint32_t wood  = rgb(104, 74, 42);
    uint32_t poly  = rgb(44, 44, 48);
    uint32_t skin  = rgb(188, 146, 112);
    uint32_t glove = rgb(52, 46, 42);

    int k = st_weap[wep].kind;

    if (k == WK_MELEE) {
        /* a blade held low and angled, because a knife with no angle is a stick */
        for (int i = 0; i < 46; i++) {
            int x = 70 + i, y = 70 - i / 2;
            boxf(&c, x, y, x + 2, y + 5 - i / 12, steel2);
        }
        rbox(&c, 60, 66, 78, 80, 3, glove);
        boxf(&c, 56, 70, 64, 78, poly);
        return;
    }

    int bl = (k == WK_SNIPER) ? 96 : (k == WK_RIFLE ? 78 : (k == WK_SMG ? 58 : 34));
    int bx = 118 - bl;                       /* muzzle end                     */

    /* barrel, on a dark outline -- without it a grey gun over a grey floor is
     * a rectangle nobody can see the edges of */
    boxf(&c, bx, 55, 118, 63, rgb(24, 24, 28));
    boxf(&c, bx, 56, 118, 62, steel);
    boxf(&c, bx, 56, 118, 57, steel2);
    /* receiver */
    rbox(&c, 103, 51, 141, 75, 3, rgb(22, 22, 26));
    rbox(&c, 104, 52, 140, 74, 3, k == WK_RIFLE ? poly : steel);
    /* slide serrations and a trigger guard: six short lines and a bar, and the
     * difference between a gun and a box */
    for (int i = 0; i < 6; i++)
        boxf(&c, 112 + i * 4, 55, 113 + i * 4, 66, shade(steel, 0.72f));
    boxf(&c, 106, 68, 126, 70, shade(steel, 0.6f));
    /* furniture */
    if (k == WK_RIFLE || k == WK_SHOTGUN) boxf(&c, bx + 6, 62, bx + 34, 68, wood);
    if (k == WK_SNIPER) {                    /* scope                          */
        rbox(&c, 106, 38, 142, 50, 5, rgb(30, 30, 34));
        ellf(&c, 136, 39, 143, 49, rgb(18, 26, 40));
    }
    /* magazine */
    if (k != WK_PISTOL) rbox(&c, 112, 74, 126, 94, 2, poly);
    /* grip and hand */
    rbox(&c, 126, 72, 142, 96, 3, poly);
    rbox(&c, 122, 74, 140, 92, 4, glove);
    for (int f = 0; f < 3; f++) ellf(&c, 120, 76 + f * 5, 128, 80 + f * 5, skin);
    /* the forward hand, only on something long enough to need one */
    if (k >= WK_SMG) {
        rbox(&c, bx + 10, 60, bx + 30, 78, 4, glove);
        for (int f = 0; f < 3; f++) ellf(&c, bx + 12 + f * 5, 60, bx + 16 + f * 5, 66, skin);
    }
    /* front sight */
    boxf(&c, bx + 2, 50, bx + 4, 57, steel2);
}

/* -------------------------------------------------------------------------- */
static int alloc_all(void) {
    for (int i = 1; i < C_COUNT; i++) {
        st_wall_tex[i] = kmalloc(TEXW * TEXW * 4);
        if (!st_wall_tex[i]) return -1;
    }
    for (int t = 0; t < 2; t++)
        for (int v = 0; v < SPR_VIEWS; v++)
            for (int f = 0; f < SPR_FRAMES; f++) {
                st_ply_spr[t][v][f] = kmalloc(SPR_W * SPR_H * 4);
                if (!st_ply_spr[t][v][f]) return -1;
            }
    for (int i = 0; i < IT_COUNT; i++) {
        st_item_spr[i] = kmalloc(ITEM_W * ITEM_H * 4);
        if (!st_item_spr[i]) return -1;
    }
    for (int i = 0; i < W_COUNT; i++) {
        st_view_spr[i] = kmalloc(VIEW_W * VIEW_H * 4);
        if (!st_view_spr[i]) return -1;
    }
    return 0;
}

int st_tex_init(void) {
    if (st_wall_tex[C_SAND]) return 0;                 /* already built        */
    if (alloc_all() != 0) { st_tex_free(); return -1; }

    tex_sandstone(st_wall_tex[C_SAND]);
    tex_brick    (st_wall_tex[C_BRICK]);
    tex_plaster  (st_wall_tex[C_PLASTER]);
    tex_crate    (st_wall_tex[C_CRATE]);
    tex_door     (st_wall_tex[C_DOOR]);

    for (int t = 0; t < 2; t++)
        for (int v = 0; v < SPR_VIEWS; v++)
            for (int f = 0; f < SPR_FRAMES; f++)
                draw_soldier(st_ply_spr[t][v][f], t, v, f);

    draw_bomb(st_item_spr[IT_BOMB]);
    draw_weapon_pickup(st_item_spr[IT_WEAPON]);
    for (int i = 0; i < W_COUNT; i++) draw_viewmodel(st_view_spr[i], i);
    return 0;
}

void st_tex_free(void) {
    for (int i = 0; i < C_COUNT; i++) { kfree(st_wall_tex[i]); st_wall_tex[i] = 0; }
    for (int t = 0; t < 2; t++)
        for (int v = 0; v < SPR_VIEWS; v++)
            for (int f = 0; f < SPR_FRAMES; f++) {
                kfree(st_ply_spr[t][v][f]); st_ply_spr[t][v][f] = 0;
            }
    for (int i = 0; i < IT_COUNT; i++) { kfree(st_item_spr[i]); st_item_spr[i] = 0; }
    for (int i = 0; i < 16; i++) { kfree(st_view_spr[i]); st_view_spr[i] = 0; }
}
