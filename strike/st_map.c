/* ===========================================================================
 *  BoltStrike -- the map, and everything that answers a question about it.
 *
 *  The layout is written as 48 rows of text rather than as a table of numbers,
 *  because a map you cannot read is a map nobody will ever edit. It was drawn
 *  by a generator that flood-fills from CT spawn and refuses to emit anything
 *  with a stranded floor cell (the script lives in the commit that added this
 *  file); what is below is its output, checked, not typed.
 *
 *  Shape, not content: two bombsites, a long right-hand corridor, tunnels on
 *  the left and a mid lane through a doorway. Anyone who has played a defusal
 *  map will know where they are. None of the geometry is copied from one.
 * ===========================================================================*/
#include <stdint.h>
#include "strike.h"
#include "math.h"

uint8_t st_cell[ST_H][ST_W];
uint8_t st_floor[ST_H][ST_W];
uint8_t st_zone[ST_H][ST_W];

/*  ' ' solid rock   '#' sandstone   'B' brick   'W' plaster   'X' crate
 *  'D' door post    '.' dirt        's' sand    'c' concrete
 *  'T' T spawn      'C' CT spawn    'a' bombsite A   'b' bombsite B          */
static const char *const rows[ST_H] = {
    "                                                ",
    "                 WWWWWWWWWWWWWWW                ",
    "                 WTTTTTTTTTTTTTW                ",
    "                 WTTTTTTTTTTTTTW########        ",
    "             ####WTTTTTTTTTTTT.........#        ",
    "   BBBBBBBBBBB.....TTTTTTTTTTT.........#        ",
    "   Bbbbbbbbbbb.....TTTTTTTTTTT.........####     ",
    "   Bbbbbbbbbbb.....TTTTTTTTTTT......ssssss#     ",
    "   BbbbbbbbbbbbB#WTTTT.....TTTTW####ssssss#     ",
    "   BbbbbXXbbbbbB WWWWW.....WWWWW   #ssssss#     ",
    "   BbbbbXXbbbbbB     #.....#       #ssssss#     ",
    "   BbbbbbbbXXbbB    ##.....#       #ssssss#     ",
    "   BbbbbbbbXXbbB    #......#       #ssssss#     ",
    "   BbbbbbbbbbbbB    #......#       #ssssss#     ",
    "   BbssssssbbbbB    #......#       #ssXXss#     ",
    "   BBssssssBBBBB    #......#       #ssXXss#     ",
    "    #ssssss#        #......#       #ssssss#     ",
    "    #ssssss#        #......#       #ssssss#     ",
    "    #ssssss#        #......#       #ssssss#     ",
    "    #ssssss#        #......#       #ssssss#     ",
    "    #ssssss#        #cccccc#       #ssssss#     ",
    "   ##ssssss#        #ccDDcc#########ssssss#     ",
    "   #sssssss#        #ccccc..........ssssss#     ",
    "   #ssssss##        #ccccc..........ssssss#     ",
    "   #ssssss#         #...............ssssss#     ",
    "   #ssssss#         #...............ssssss#     ",
    "   #ssXXss#         #......#####....ssssss#     ",
    "   #ssXXss#         #...XX.#   #....ssssss###   ",
    "   #ssssss#         #...XX.#   #....ssssssss#   ",
    "   #ssssss#         #......# ###....ssssssss#   ",
    "   #ssssss#         #......# #......ssssssss#   ",
    "   #ssssss#         #......# #....aassssXXssB   ",
    "   #ssssss###########......###....aaaaaaXXaaB   ",
    "   #ssssscccccccccc##cccccccc#....aaXXaaaaaaB   ",
    "   #ssssscccccccccc##ccccccccc....caXXaaaaaaB   ",
    "   ######cccccccccc##ccccccccc....caaaaaaaaaB   ",
    "        #cccccccccc##ccccccccc....caaaaaaaaaB   ",
    "        #cccccccccc##ccccccccccccccaaaaaaaaaB   ",
    "        #ccccccccccWWccccccccccccccaaaaaaaaaB   ",
    "        ########WCCCCCCCCCCCCCCW##BBBBBBBBBBB   ",
    "                WCCCCCCCCCCCCCCW                ",
    "                WCCCCCCCCCCCCCCW                ",
    "                WCCCCCCCCCCCCCCW                ",
    "                WCCCCCCCCCCCCCCW                ",
    "                WCCCCCCCCCCCCCCW                ",
    "                WCCCCCCCCCCCCCCW                ",
    "                WWWWWWWWWWWWWWWW                ",
    "                                                ",
};

/* The zone centres, worked out once here because bot_goal() wants them every
 * tick and they only move when the map does. See st_zone_centre(). */
static float zc_x[5], zc_y[5];
static void zone_centre_slow(int zone, float *x, float *y);

void st_map_load(void) {
    for (int y = 0; y < ST_H; y++) {
        const char *r = rows[y];
        for (int x = 0; x < ST_W; x++) {
            char c = r[x] ? r[x] : ' ';       /* a short row is solid rock    */
            uint8_t cell = C_OPEN, fl = F_DIRT, zn = Z_NONE;
            switch (c) {
            case ' ': cell = C_SAND;    break;   /* off-map: still a wall      */
            case '#': cell = C_SAND;    break;
            case 'B': cell = C_BRICK;   break;
            case 'W': cell = C_PLASTER; break;
            case 'X': cell = C_CRATE;   break;
            case 'D': cell = C_DOOR;    break;
            case '.': fl = F_DIRT;      break;
            case 's': fl = F_SAND;      break;
            case 'c': fl = F_CONCRETE;  break;
            case 'T': fl = F_SPAWN_T;  zn = Z_SPAWN_T;  break;
            case 'C': fl = F_SPAWN_CT; zn = Z_SPAWN_CT; break;
            case 'a': fl = F_SITE_A;   zn = Z_SITE_A;   break;
            case 'b': fl = F_SITE_B;   zn = Z_SITE_B;   break;
            default:  cell = C_SAND;    break;
            }
            st_cell[y][x]  = cell;
            st_floor[y][x] = fl;
            st_zone[y][x]  = zn;
        }
    }

    for (int z = 0; z < 5; z++) zone_centre_slow(z, &zc_x[z], &zc_y[z]);
}

int st_solid(int cx, int cy) {
    if (cx < 0 || cy < 0 || cx >= ST_W || cy >= ST_H) return 1;
    return st_cell[cy][cx] != C_OPEN;
}

int st_solid_at(float x, float y) { return st_solid((int)x, (int)y); }

/* Line of sight, by the same grid walk the renderer uses -- which is the point.
 * If a bot decides it can see you using arithmetic the wall pass does not
 * agree with, it shoots you through a corner and the game feels broken in a
 * way that is very hard to find from the symptom. */
int st_los(float x0, float y0, float x1, float y1) {
    /* Walk the pair in a fixed order, whichever way round it was asked. The
     * grid walk accumulates float error differently in each direction, so a
     * ray that grazes a corner could be blocked one way and clear the other --
     * which, in a game, is a bot shooting you through a wall you are hiding
     * behind and no way at all to reproduce it. One comparison buys exact
     * symmetry; found by a host test firing 400 random pairs both ways. */
    if (x1 < x0 || (x1 == x0 && y1 < y0)) {
        float t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) return 1;
    float rx = dx / len, ry = dy / len;

    int cx = (int)x0, cy = (int)y0;
    const int tx = (int)x1, ty = (int)y1;
    if (st_solid(cx, cy)) return 0;

    float ddx = (rx == 0.0f) ? 1e30f : fabsf(1.0f / rx);
    float ddy = (ry == 0.0f) ? 1e30f : fabsf(1.0f / ry);
    int   sx, sy;
    float sdx, sdy;
    if (rx < 0) { sx = -1; sdx = (x0 - (float)cx) * ddx; }
    else        { sx =  1; sdx = ((float)cx + 1.0f - x0) * ddx; }
    if (ry < 0) { sy = -1; sdy = (y0 - (float)cy) * ddy; }
    else        { sy =  1; sdy = ((float)cy + 1.0f - y0) * ddy; }

    for (int guard = 0; guard < 4 * (ST_W + ST_H); guard++) {
        if (cx == tx && cy == ty) return 1;
        float travelled;
        if (sdx < sdy) { travelled = sdx; sdx += ddx; cx += sx; }
        else           { travelled = sdy; sdy += ddy; cy += sy; }
        if (travelled > len) return 1;          /* went past the target       */
        if (st_solid(cx, cy)) return 0;
    }
    return 0;
}

/* ---- navigation ---------------------------------------------------------- *
 *  A flow field rather than a path: 2304 cells is nothing to flood, and a bot
 *  that reads its next step out of an array cannot get stuck re-planning, walk
 *  into a wall it was told to walk into two seconds ago, or need its route
 *  patched when it gets shoved by a team mate. One build per goal, shared by
 *  everyone heading there. */
void st_flow_build(int gx, int gy, uint8_t *out) {
    for (int i = 0; i < ST_W * ST_H; i++) out[i] = 255;
    if (gx < 0 || gy < 0 || gx >= ST_W || gy >= ST_H || st_solid(gx, gy)) return;

    static uint16_t q[ST_W * ST_H];
    int head = 0, tail = 0;
    out[gy * ST_W + gx] = 0;
    q[tail++] = (uint16_t)(gy * ST_W + gx);

    while (head < tail) {
        int id = q[head++];
        int x = id % ST_W, y = id / ST_W;
        uint8_t d = out[id];
        if (d >= 254) continue;
        static const int ox[4] = { 1, -1, 0, 0 };
        static const int oy[4] = { 0, 0, 1, -1 };
        for (int k = 0; k < 4; k++) {
            int nx = x + ox[k], ny = y + oy[k];
            if (nx < 0 || ny < 0 || nx >= ST_W || ny >= ST_H) continue;
            if (st_solid(nx, ny)) continue;
            int nid = ny * ST_W + nx;
            if (out[nid] != 255) continue;
            out[nid] = (uint8_t)(d + 1);
            q[tail++] = (uint16_t)nid;
        }
    }
}

int st_flow_step(const uint8_t *field, int cx, int cy, int *nx, int *ny) {
    if (cx < 0 || cy < 0 || cx >= ST_W || cy >= ST_H) return 0;
    uint8_t here = field[cy * ST_W + cx];
    if (here == 255) return 0;
    if (here == 0) { *nx = cx; *ny = cy; return 1; }

    int best = here, bx = cx, by = cy;
    /* Diagonals are allowed only when both of their orthogonals are open, or a
     * bot cuts the corner of a crate and spends the round wedged on it. */
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= ST_W || y >= ST_H) continue;
            if (st_solid(x, y)) continue;
            if (dx && dy && (st_solid(cx + dx, cy) || st_solid(cx, cy + dy))) continue;
            int v = field[y * ST_W + x];
            if (v < best) { best = v; bx = x; by = y; }
        }
    }
    if (bx == cx && by == cy) return 0;
    *nx = bx; *ny = by;
    return 1;
}

/* The middle of a named area -- but a cell in it, not its centroid.
 *
 * A bombsite with a crate in the middle, or any shape that is not convex, has
 * a centroid that is not part of it. That reads as a rounding detail and is
 * not one: bot_goal() feeds this straight to st_flow_build(), and a goal on a
 * solid cell produces a flow field of 255s, which is a whole team standing in
 * spawn doing nothing for the rest of the round. So: average the cells, then
 * return the nearest cell that is actually in the zone and actually walkable.
 *
 * Two passes over 2,304 cells, which is fine once and was not fine at all when
 * bot_goal() was calling it: the desktop loop wakes on every PIT tick, so every
 * roaming bot was scanning the whole map a thousand times a second and the game
 * crawled from the moment a round went live. The answers only change when the
 * map does, so they are worked out once in st_map_load() and read out of a
 * table after that. */
static void zone_centre_slow(int zone, float *x, float *y) {
    long sx = 0, sy = 0, n = 0;
    for (int cy = 0; cy < ST_H; cy++)
        for (int cx = 0; cx < ST_W; cx++)
            if (st_zone[cy][cx] == zone) { sx += cx; sy += cy; n++; }
    if (!n) { *x = ST_W / 2.0f; *y = ST_H / 2.0f; return; }

    float ax = (float)sx / (float)n, ay = (float)sy / (float)n;
    int bx = -1, by = -1;
    float best = 1e30f;
    for (int cy = 0; cy < ST_H; cy++) {
        for (int cx = 0; cx < ST_W; cx++) {
            if (st_zone[cy][cx] != zone || st_solid(cx, cy)) continue;
            float dx = (float)cx - ax, dy = (float)cy - ay;
            float d = dx * dx + dy * dy;
            if (d < best) { best = d; bx = cx; by = cy; }
        }
    }
    if (bx < 0) { *x = ax + 0.5f; *y = ay + 0.5f; return; }
    *x = (float)bx + 0.5f;
    *y = (float)by + 0.5f;
}

void st_zone_centre(int zone, float *x, float *y) {
    if ((unsigned)zone >= 5) { *x = ST_W / 2.0f; *y = ST_H / 2.0f; return; }
    *x = zc_x[zone];
    *y = zc_y[zone];
}
