/* ===========================================================================
 *  BoltStrike -- the renderer.
 *
 *  One DDA per screen column, not per pixel. Every wall in this game is the
 *  same height, so a column is a single texture strip and the whole scene
 *  costs 320 rays instead of 64,000 -- which is the difference between a game
 *  and a slideshow on a machine with no acceleration. Floors are cast
 *  afterwards, per row, by the standard trick of walking a horizontal line at
 *  a constant world distance, so the bombsite markings are really on the
 *  ground and not painted on afterwards.
 *
 *  Drawn in this order: sky, floor, walls, sprites (back to front, z-tested
 *  against the wall pass), then the thing in your hands. The frame is 320x200
 *  and the window scales it; text and the HUD are drawn at full window
 *  resolution by app_strike.c afterwards, so they stay sharp.
 * ===========================================================================*/
#include <stdint.h>
#include "strike.h"
#include "math.h"
#include "string.h"

uint32_t st_frame[RW * RH];

static float zbuf[RW];
/* What the wall pass covered, per column, so the sky and the floor can stop
 * painting pixels a wall is about to sit on. wbot < wtop means no wall in that
 * column. It is one compare per pixel and it takes a third off the frame in a
 * bombsite, where the walls are close and used to cover the lot. */
static int16_t wtop[RW], wbot[RW];

#define PI_F 3.14159265f

static uint32_t shade(uint32_t c, int num, int den) {
    if (den <= 0) return c;
    if (num > den) num = den;
    if (num < 0) num = 0;
    uint32_t r = (((c >> 16) & 0xFF) * (uint32_t)num) / (uint32_t)den;
    uint32_t g = (((c >> 8)  & 0xFF) * (uint32_t)num) / (uint32_t)den;
    uint32_t b = ((c & 0xFF)         * (uint32_t)num) / (uint32_t)den;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Distance fog, in 1/256ths. Everything past about 22 cells is night-dark,
 * which is not realism -- it is how you stop the far end of long being a
 * pixel-hunt and how the draw stays cheap. */
static int fog_num(float dist) {
    int n = 256 - (int)(dist * 9.0f);
    if (n < 46) n = 46;
    return n;
}

static const uint32_t floor_col[F_COUNT] = {
    0x8A7A5C,   /* dirt      */
    0xB39B6A,   /* sand      */
    0x6E6E70,   /* concrete  */
    0x9A7A4A,   /* bombsite A -- warmer, and you can tell at a glance */
    0x4E7A78,   /* bombsite B -- colder */
    0x7A6A54,   /* T spawn   */
    0x5A6474,   /* CT spawn  */
};

/* ---- sky ----------------------------------------------------------------- */
static void draw_sky(int horizon) {
    for (int y = 0; y < horizon && y < RH; y++) {
        /* A late-afternoon gradient: pale and hot at the horizon, deeper up top */
        int t = horizon > 0 ? (y * 255) / horizon : 0;
        int r = 92 + (t * 120) / 255;
        int g = 120 + (t * 96) / 255;
        int b = 168 - (t * 44) / 255;
        uint32_t c = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        uint32_t *row = &st_frame[y * RW];
        for (int x = 0; x < RW; x++)
            if (y < wtop[x] || y > wbot[x]) row[x] = c;
    }
}

/* ---- floors -------------------------------------------------------------- */
static void draw_floor(float px, float py, float dxl, float dyl, float dxr, float dyr,
                       int horizon) {
    for (int y = horizon + 1; y < RH; y++) {
        /* Measured from the horizon, not from the middle of the frame. Getting
         * that wrong is invisible until you look up or down, and then the floor
         * slides out from under the walls -- the wall pass puts its bottom edge
         * at the horizon and this used to put the matching floor row somewhere
         * else entirely. */
        int p = y - horizon;
        if (p <= 0) continue;
        /* Distance to the row: the camera is half a wall high, hence 0.5. */
        float rowdist = (0.5f * (float)RH) / (float)p;
        if (rowdist > 40.0f) rowdist = 40.0f;

        float stepx = rowdist * (dxr - dxl) / (float)RW;
        float stepy = rowdist * (dyr - dyl) / (float)RW;
        float fx = px + rowdist * dxl;
        float fy = py + rowdist * dyl;

        int num = fog_num(rowdist);
        uint32_t *row = &st_frame[y * RW];
        for (int x = 0; x < RW; x++) {
            if (y <= wbot[x] && y >= wtop[x]) { fx += stepx; fy += stepy; continue; }
            int cx = (int)fx, cy = (int)fy;
            uint32_t base;
            if ((unsigned)cx < (unsigned)ST_W && (unsigned)cy < (unsigned)ST_H) {
                base = floor_col[st_floor[cy][cx]];
                /* One darker pixel each side of a cell edge. Cheap, and without
                 * it a big floor is a flat wash with no sense of speed. */
                float ex = fx - (float)cx, ey = fy - (float)cy;
                if (ex < 0.03f || ex > 0.97f || ey < 0.03f || ey > 0.97f)
                    base = (base >> 1) & 0x7F7F7F;
                else if (((cx * 7 + cy * 13) & 7) == 0)
                    base += 0x060606;
            } else {
                base = floor_col[F_DIRT];
            }
            row[x] = shade(0xFF000000u | base, num, 256);
            fx += stepx; fy += stepy;
        }
    }
}

/* ---- walls --------------------------------------------------------------- */
static void draw_walls(float px, float py, float dirx, float diry,
                       float planex, float planey, int horizon) {
    for (int x = 0; x < RW; x++) {
        float camx = 2.0f * (float)x / (float)RW - 1.0f;
        float rx = dirx + planex * camx;
        float ry = diry + planey * camx;

        int mapx = (int)px, mapy = (int)py;
        float ddx = (rx == 0.0f) ? 1e30f : fabsf(1.0f / rx);
        float ddy = (ry == 0.0f) ? 1e30f : fabsf(1.0f / ry);
        int stepx, stepy;
        float sdx, sdy;
        if (rx < 0) { stepx = -1; sdx = (px - (float)mapx) * ddx; }
        else        { stepx =  1; sdx = ((float)mapx + 1.0f - px) * ddx; }
        if (ry < 0) { stepy = -1; sdy = (py - (float)mapy) * ddy; }
        else        { stepy =  1; sdy = ((float)mapy + 1.0f - py) * ddy; }

        wtop[x] = RH; wbot[x] = -1;                 /* nothing covered yet     */

        int side = 0, hit = 0, guard = 0;
        while (!hit && guard++ < 128) {
            if (sdx < sdy) { sdx += ddx; mapx += stepx; side = 0; }
            else           { sdy += ddy; mapy += stepy; side = 1; }
            if ((unsigned)mapx >= (unsigned)ST_W || (unsigned)mapy >= (unsigned)ST_H) break;
            if (st_cell[mapy][mapx] != C_OPEN) hit = 1;
        }
        if (!hit) { zbuf[x] = 1e30f; continue; }

        float dist = side == 0 ? (sdx - ddx) : (sdy - ddy);
        if (dist < 0.02f) dist = 0.02f;
        zbuf[x] = dist;

        int lineh = (int)((float)RH / dist);
        int y0 = horizon - lineh / 2;
        int y1 = horizon + lineh / 2;
        int cy0 = y0 < 0 ? 0 : y0;
        int cy1 = y1 >= RH ? RH - 1 : y1;
        if (cy1 < cy0) continue;

        const uint32_t *tex = st_wall_tex[st_cell[mapy][mapx]];
        if (!tex) continue;

        float wallx = side == 0 ? py + dist * ry : px + dist * rx;
        wallx -= floorf(wallx);
        int texx = (int)(wallx * (float)TEXW);
        if ((side == 0 && rx > 0) || (side == 1 && ry < 0)) texx = TEXW - texx - 1;
        if (texx < 0) texx = 0;
        if (texx >= TEXW) texx = TEXW - 1;

        /* A face you see edge-on is darker. One multiply, and it is most of
         * what makes a grid of cubes look like architecture. */
        int num = fog_num(dist);
        if (side == 1) num = (num * 178) / 256;

        /* 16.16 fixed point: the texture row advances by a constant, so the
         * inner loop has no divide in it. */
        wtop[x] = (int16_t)cy0;
        wbot[x] = (int16_t)cy1;

        uint32_t tstep = (uint32_t)((TEXW << 16) / (lineh > 0 ? lineh : 1));
        uint32_t tpos  = (uint32_t)((cy0 - y0) * (int)tstep);
        uint32_t *dst  = &st_frame[cy0 * RW + x];
        for (int y = cy0; y <= cy1; y++) {
            uint32_t ty = (tpos >> 16) & (TEXW - 1);
            *dst = shade(tex[ty * TEXW + texx], num, 256);
            tpos += tstep;
            dst += RW;
        }
    }
}

/* ---- sprites ------------------------------------------------------------- *
 *  A billboard. `img` is the pixels; when it is null the entry is an effect
 *  and `col`/`radius` describe a disc instead, which is all an effect has ever
 *  been. Both the mirror and the disc used to be built into one shared scratch
 *  buffer at gather time -- and the whole list is gathered before any of it is
 *  drawn, so every mirrored soldier in the frame ended up wearing the last
 *  one's pose and every puff of blood came out the colour of whatever the last
 *  effect was. Nothing is baked now; the flag and the two numbers travel with
 *  the entry and the blit reads them. */
typedef struct {
    float    d;                      /* squared distance, for the sort        */
    float    x, y;
    const uint32_t *img;             /* null for an effect                    */
    int      w, h, lift, scale;
    int      mirror;
    uint32_t col;                    /* effects: the colour of the disc       */
    int      radius;                 /* effects: in the disc's own 16x16 box  */
    int      fade;                   /* 0..256; 256 is fully opaque           */
} vis_t;

static void blit_sprite(const vis_t *v, float px, float py,
                        float dirx, float diry, float planex, float planey, int horizon) {
    float rx = v->x - px, ry = v->y - py;
    float inv = 1.0f / (planex * diry - dirx * planey);
    float tx = inv * (diry * rx - dirx * ry);
    float ty = inv * (-planey * rx + planex * ry);      /* depth               */
    if (ty < 0.12f) return;

    int scr = (int)(((float)RW / 2.0f) * (1.0f + tx / ty));
    int h = (int)fabsf((float)RH / ty) * v->scale / 100;
    int w = h * v->w / v->h;
    if (h <= 0 || w <= 0) return;

    /* lift is in hundredths of a wall, measured from the floor line */
    int base = horizon + (int)((float)RH / ty) / 2;
    int y1 = base - (int)(((float)RH / ty) * (float)v->lift / 100.0f);
    int y0 = y1 - h;

    int x0 = scr - w / 2, x1 = x0 + w;
    int cx0 = x0 < 0 ? 0 : x0, cx1 = x1 > RW ? RW : x1;
    int cy0 = y0 < 0 ? 0 : y0, cy1 = y1 > RH ? RH : y1;
    if (cx1 <= cx0 || cy1 <= cy0) return;

    int num = fog_num(ty);
    if (v->fade < 256) num = (num * v->fade) / 256;

    /* The source column for each screen column, worked out once. The loop that
     * follows walks rows on the outside so the stores run along the frame
     * rather than down it; it was the other way round and every pixel of a
     * soldier landed in a different cache line. */
    static int16_t colmap[RW];
    for (int x = cx0; x < cx1; x++) {
        int sx = ((x - x0) * v->w) / w;
        if (v->mirror) sx = v->w - 1 - sx;
        colmap[x] = (int16_t)((unsigned)sx < (unsigned)v->w ? sx : -1);
    }

    for (int y = cy0; y < cy1; y++) {
        int sy = ((y - y0) * v->h) / h;
        if ((unsigned)sy >= (unsigned)v->h) continue;
        const uint32_t *src = v->img ? &v->img[sy * v->w] : 0;
        uint32_t *row = &st_frame[y * RW];
        for (int x = cx0; x < cx1; x++) {
            if (ty >= zbuf[x]) continue;                /* behind a wall       */
            int sx = colmap[x];
            if (sx < 0) continue;
            uint32_t c;
            if (src) {
                c = src[sx];
                if (!(c & 0xFF000000u)) continue;       /* alpha 0 = not drawn */
            } else {
                int dx = sx - v->w / 2, dy = sy - v->h / 2;
                if (dx * dx + dy * dy > v->radius * v->radius) continue;
                c = v->col;
            }
            row[x] = shade(c, num, 256);
        }
    }
}

/* Which of the five drawn views, and whether to mirror it. 0 is facing you. */
static int pick_view(const st_ent *e, float px, float py, int *mirror) {
    float to_cam = atan2f(py - e->y, px - e->x);
    float rel = e->ang - to_cam;
    while (rel < 0.0f)        rel += 2.0f * PI_F;
    while (rel >= 2.0f * PI_F) rel -= 2.0f * PI_F;
    int oct = (int)((rel + PI_F / 8.0f) / (PI_F / 4.0f)) & 7;
    static const int view_of[8]   = { 0, 1, 2, 3, 4, 3, 2, 1 };
    static const int mirror_of[8] = { 0, 0, 0, 0, 0, 1, 1, 1 };
    *mirror = mirror_of[oct];
    return view_of[oct];
}

static uint32_t fx_colour(int kind) {
    switch (kind) {
    case FX_SPARK: return 0xFFFFD070;
    case FX_SMOKE: return 0xFF9A9A96;
    default:       return 0xFFB01818;
    }
}

/* ---- the gun in your hands ---------------------------------------------- */
static void draw_viewmodel(const st_ent *eye) {
    const uint32_t *img = st_view_spr[eye->wep];
    if (!img) return;

    int bobx = (int)(sinf(eye->bob) * 5.0f);
    int boby = (int)(fabsf(cosf(eye->bob)) * 4.0f);
    int kick = (int)(eye->recoil * 16.0f);

    int dx0 = RW - VIEW_W - 6 + bobx;
    int dy0 = RH - VIEW_H - 14 + boby + kick;

    for (int y = 0; y < VIEW_H; y++) {
        int ty = dy0 + y;
        if ((unsigned)ty >= (unsigned)RH) continue;
        const uint32_t *s = &img[y * VIEW_W];
        uint32_t *d = &st_frame[ty * RW];
        for (int x = 0; x < VIEW_W; x++) {
            uint32_t c = s[x];
            if (!(c & 0xFF000000u)) continue;
            int tx = dx0 + x;
            if ((unsigned)tx < (unsigned)RW) d[tx] = c;
        }
    }

    /* Muzzle flash: a blown-out blob at the end of the barrel for two frames.
     * It also lights the wall in front of you, which is done by brightening
     * the middle of the frame rather than by anything resembling lighting. */
    if (eye->flash_ms > 0) {
        int mx = dx0 + 12, my = dy0 + 52;
        for (int y = -10; y <= 10; y++) {
            for (int x = -14; x <= 14; x++) {
                if (x * x + y * y * 2 > 190) continue;
                int tx = mx + x, ty = my + y;
                if ((unsigned)tx >= (unsigned)RW || (unsigned)ty >= (unsigned)RH) continue;
                st_frame[ty * RW + tx] = (x * x + y * y * 2 < 70) ? 0xFFFFF0C0 : 0xFFFFC040;
            }
        }
    }
}

static void draw_crosshair(int zoomed) {
    uint32_t c = 0xFF60FF70;
    int cx = RW / 2, cy = RH / 2;
    if (zoomed) {
        for (int x = 0; x < RW; x++) st_frame[cy * RW + x] = 0xFF101010;
        for (int y = 0; y < RH; y++) st_frame[y * RW + cx] = 0xFF101010;
        return;
    }
    for (int i = 3; i <= 8; i++) {
        st_frame[cy * RW + cx + i] = c;
        st_frame[cy * RW + cx - i] = c;
        st_frame[(cy + i) * RW + cx] = c;
        st_frame[(cy - i) * RW + cx] = c;
    }
}

/* The scope: black out everything outside a circle and draw the reticle. */
static void draw_scope(void) {
    int cx = RW / 2, cy = RH / 2, r = RH / 2 - 4;
    for (int y = 0; y < RH; y++) {
        for (int x = 0; x < RW; x++) {
            int dx = x - cx, dy = y - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 > r * r) st_frame[y * RW + x] = 0xFF000000;
            else if (d2 > (r - 2) * (r - 2)) st_frame[y * RW + x] = 0xFF181818;
        }
    }
}

/* -------------------------------------------------------------------------- */
void st_render(const st_ent *eye, float pitch, int zoomed) {
    float fov = zoomed ? 0.26f : 0.72f;
    float dirx = cosf(eye->ang), diry = sinf(eye->ang);
    float planex = -diry * fov, planey = dirx * fov;
    float px = eye->x, py = eye->y;

    int horizon = RH / 2 + (int)pitch;
    if (horizon < 24) horizon = 24;
    if (horizon > RH - 24) horizon = RH - 24;

    draw_sky(horizon);
    draw_floor(px, py, dirx - planex, diry - planey, dirx + planex, diry + planey, horizon);
    draw_walls(px, py, dirx, diry, planex, planey, horizon);

    /* Gather what is worth drawing, then sort far-to-near. A dozen entities and
     * four dozen effects is a small enough list that an insertion sort is the
     * right sort, and it is stable, which keeps two things at the same distance
     * from swapping places between frames. */
    static vis_t vis[ST_MAX_ENT + ST_MAX_FX + 2];
    int nv = 0;

    for (int i = 0; i < st_w.nent; i++) {
        const st_ent *e = &st_w.ent[i];
        if (!e->used || e == eye) continue;
        /* A body lies there for CORPSE_MS and then is not there. Leaving them
         * for the whole round was not a decision, it was an `if` with an empty
         * body -- and by the time the bomb went down there were nine of them in
         * the draw list, all of them facing the wrong way because the mirror
         * was skipped for the dead. */
        if (!e->alive && e->corpse_ms <= 0) continue;
        int mirror = 0;
        int view = pick_view(e, px, py, &mirror);
        int frame = !e->alive ? 2 : ((e->step_acc / 260) & 1);
        const uint32_t *img = st_ply_spr[e->team][view][frame];
        if (!img) continue;
        float dx = e->x - px, dy = e->y - py;
        vis[nv] = (vis_t){ 0 };
        vis[nv].d = dx * dx + dy * dy;
        vis[nv].x = e->x; vis[nv].y = e->y;
        vis[nv].img = img; vis[nv].w = SPR_W; vis[nv].h = SPR_H;
        vis[nv].mirror = mirror;
        vis[nv].scale = e->alive ? 74 : 40;
        vis[nv].fade = 256;
        /* The last second of a corpse is spent going dark, so bodies leave
         * rather than blink out from under you. */
        if (!e->alive && e->corpse_ms < 1000) vis[nv].fade = e->corpse_ms / 4 + 6;
        nv++;
    }

    if ((st_w.bomb_down || st_w.bomb_loose) && st_item_spr[IT_BOMB]) {
        float dx = st_w.bomb_x - px, dy = st_w.bomb_y - py;
        vis[nv] = (vis_t){ 0 };
        vis[nv].d = dx * dx + dy * dy;
        vis[nv].x = st_w.bomb_x; vis[nv].y = st_w.bomb_y;
        vis[nv].img = st_item_spr[IT_BOMB];
        vis[nv].w = ITEM_W; vis[nv].h = ITEM_H; vis[nv].scale = 26;
        vis[nv].fade = 256;
        nv++;
    }

    for (int i = 0; i < ST_MAX_FX && nv < (int)(sizeof(vis) / sizeof(vis[0])); i++) {
        if (st_fx_list[i].ms <= 0) continue;
        float dx = st_fx_list[i].x - px, dy = st_fx_list[i].y - py;
        int age = 700 - st_fx_list[i].ms;
        int r = 3 + age / 120;
        vis[nv] = (vis_t){ 0 };
        vis[nv].d = dx * dx + dy * dy;
        vis[nv].x = st_fx_list[i].x; vis[nv].y = st_fx_list[i].y;
        vis[nv].img = 0;
        vis[nv].col = fx_colour(st_fx_list[i].kind);
        vis[nv].radius = r > 7 ? 7 : r;
        vis[nv].w = 16; vis[nv].h = 16; vis[nv].lift = 30; vis[nv].scale = 22;
        vis[nv].fade = 256;
        nv++;
    }

    for (int i = 1; i < nv; i++) {
        vis_t k = vis[i];
        int j = i - 1;
        while (j >= 0 && vis[j].d < k.d) { vis[j + 1] = vis[j]; j--; }
        vis[j + 1] = k;
    }
    for (int i = 0; i < nv; i++)
        blit_sprite(&vis[i], px, py, dirx, diry, planex, planey, horizon);

    if (zoomed) draw_scope();
    else if (eye->alive) draw_viewmodel(eye);
    if (eye->alive) draw_crosshair(zoomed);

    /* Taking a hit washes the frame red, briefly and not subtly.
     *
     * "Briefly" is the fix: this keyed off hp alone, so one graze at the start
     * of a round left the screen tinted and re-tinted -- 64,000 read-modify-
     * writes a frame -- until you died. It is a flash now, driven by the timer
     * st_hurt sets, and the tint is worth something again because it means you
     * are being shot at right now. */
    if (eye->hurt_ms > 0 && eye->hp > 0) {
        int hurt = (100 - eye->hp) / 3;
        if (hurt > 26) hurt = 26;
        hurt = hurt * eye->hurt_ms / HURT_FLASH_MS;
        if (hurt < 1) hurt = 1;
        for (int y = 0; y < RH; y++) {
            int edge = (y < 12 || y > RH - 12) ? hurt : hurt / 3;
            uint32_t *row = &st_frame[y * RW];
            for (int x = 0; x < RW; x++) {
                int e2 = (x < 16 || x > RW - 16) ? hurt : edge;
                uint32_t c = row[x];
                int r = (int)((c >> 16) & 0xFF) + e2 * 3;
                int g = (int)((c >> 8) & 0xFF) - e2;
                int b = (int)(c & 0xFF) - e2;
                if (r > 255) r = 255;
                if (g < 0) g = 0;
                if (b < 0) b = 0;
                row[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }
}
