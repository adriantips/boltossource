/* Every texture in the game, generated at startup.
 *
 * Nothing is embedded: `download` refuses anything over 256 KiB and the
 * templates are already 45 KiB of libc and fonts, so a folder of 16x16 PNGs
 * baked into .rodata is a bad trade when a few hundred lines of arithmetic
 * produce the same thing in under a millisecond.
 *
 * All sixteen-pixel tiles, all tileable -- every lattice and cell index wraps
 * at 16, so a wall of the same block has no visible grid.
 */
#include "craft.h"

uint32_t g_tex[B_COUNT][3][TEXW * TEXW];

static inline uint32_t rgb(int r, int g, int b) {
    return 0xFF000000u | ((uint32_t)clampi(r, 0, 255) << 16)
                       | ((uint32_t)clampi(g, 0, 255) << 8)
                       |  (uint32_t)clampi(b, 0, 255);
}
static inline uint32_t argb(int a, int r, int g, int b) {
    return ((uint32_t)clampi(a, 0, 255) << 24) | ((uint32_t)clampi(r, 0, 255) << 16)
         | ((uint32_t)clampi(g, 0, 255) << 8)  |  (uint32_t)clampi(b, 0, 255);
}

/* signed noise in [-amp, amp] */
static int nz(int x, int y, uint32_t salt, int amp) {
    uint32_t h = hash3(x & 15, y & 15, 0, salt);
    return (int)((h & 255u) * (uint32_t)(2 * amp) / 255u) - amp;
}

#define FOR_TEXEL for (int y = 0; y < TEXW; y++) for (int x = 0; x < TEXW; x++)
#define T(x, y)   t[(y) * TEXW + (x)]

static void gen_flat(uint32_t *t, int r, int g, int b, int fine, int coarse,
                     uint32_t salt) {
    FOR_TEXEL {
        int v = nz(x, y, salt, fine) + nz(x >> 2, y >> 2, salt + 1, coarse);
        T(x, y) = rgb(r + v, g + v, b + v);
    }
}

static void gen_dirt(uint32_t *t, uint32_t salt) {
    gen_flat(t, 134, 96, 67, 13, 7, salt);
    FOR_TEXEL {
        if ((hash3(x, y, 1, salt) & 15) < 2) {          /* pebbles */
            int v = -24 + nz(x, y, salt + 3, 6);
            T(x, y) = rgb(134 + v, 96 + v, 67 + v);
        }
    }
}

static void gen_grass_top(uint32_t *t, uint32_t salt) {
    FOR_TEXEL {
        int v = nz(x, y, salt, 15) + nz(x >> 1, y >> 1, salt + 1, 9);
        int r = 104 + v, g = 168 + v + (v > 8 ? 6 : 0), b = 62 + v / 2;
        T(x, y) = rgb(r, g, b);
    }
}

/* Dirt with the grass hanging over the top edge. The fringe is per-column so
 * the seam is ragged; a straight line across every block gives the whole
 * landscape a corrugated look you cannot unsee. */
static void gen_grass_side(uint32_t *t, uint32_t salt) {
    gen_dirt(t, salt + 40);
    for (int x = 0; x < TEXW; x++) {
        uint32_t h = hash3(x, 9, 0, salt);
        int cap = 3 + (int)(h & 1) + (((h >> 1) & 3) == 0 ? 1 : 0);
        for (int y = 0; y < cap; y++) {
            int v = nz(x, y, salt + 2, 13);
            T(x, y) = rgb(104 + v, 166 + v, 60 + v / 2);
        }
        if ((h >> 4) & 1) {
            int v = nz(x, cap, salt + 2, 10);
            T(x, cap) = rgb(96 + v, 152 + v, 56 + v / 2);
        }
    }
}

/* Cheap Voronoi on a 4x4-pixel grid, wrapped so it tiles. `second - best` is
 * small along a cell border, which is where the mortar goes. */
static void gen_cobble(uint32_t *t, uint32_t salt) {
    FOR_TEXEL {
        float best = 1e9f, second = 1e9f;
        uint32_t id = 0;
        for (int oy = -1; oy <= 1; oy++)
            for (int ox = -1; ox <= 1; ox++) {
                int gx = (x >> 2) + ox, gy = (y >> 2) + oy;
                uint32_t h = hash3(gx & 3, gy & 3, 0, salt);
                float cx = (float)(gx * 4 + 1 + (int)(h & 3));
                float cy = (float)(gy * 4 + 1 + (int)((h >> 3) & 3));
                float dx = cx - (float)x, dy = cy - (float)y;
                float d = dx * dx + dy * dy;
                if (d < best) { second = best; best = d; id = h; }
                else if (d < second) second = d;
            }
        float edge = fsqrt(second) - fsqrt(best);
        if (edge < 0.9f) {
            int v = nz(x, y, salt + 5, 6);
            T(x, y) = rgb(74 + v, 74 + v, 78 + v);
        } else {
            int base = 112 + (int)((id >> 6) & 31) + nz(x, y, salt + 6, 8);
            T(x, y) = rgb(base, base, base + 4);
        }
    }
}

static void gen_log_side(uint32_t *t, uint32_t salt) {
    FOR_TEXEL {
        uint32_t h = hash3(x, 0, 0, salt);
        int col = (int)((h & 31u)) - 15;
        int v = col + nz(x, y, salt + 1, 6);
        if ((h & 7) == 0) v -= 20;                       /* bark groove */
        T(x, y) = rgb(101 + v, 80 + v, 48 + v / 2);
    }
}

static void gen_log_top(uint32_t *t, uint32_t salt) {
    FOR_TEXEL {
        float dx = (float)x - 7.5f, dy = (float)y - 7.5f;
        float d = fsqrt(dx * dx + dy * dy);
        int v = nz(x, y, salt, 6);
        if (d > 7.0f) { T(x, y) = rgb(84 + v, 66 + v, 40 + v); continue; }
        int ring = (int)(d * 1.7f) & 1;
        if (ring) T(x, y) = rgb(158 + v, 126 + v, 80 + v);
        else      T(x, y) = rgb(132 + v, 104 + v, 66 + v);
    }
}

/* Alpha 0 is a real hole -- the renderer walks straight through it and you can
 * see sky through a canopy. Clustered slightly, because scattered single-pixel
 * holes just look like the texture is broken. */
static void gen_leaves(uint32_t *t, uint32_t salt) {
    FOR_TEXEL {
        uint32_t hh = hash3(x, y, 0, salt);
        uint32_t hc = hash3(x >> 1, y >> 1, 0, salt + 1);
        if ((hc & 7) < 2 && (hh & 3) != 0) { T(x, y) = 0; continue; }
        int v = nz(x, y, salt + 2, 22) + nz(x >> 1, y >> 1, salt + 3, 12);
        T(x, y) = rgb(54 + v, 116 + v, 38 + v / 2);
    }
}

static void gen_water(uint32_t *t, uint32_t salt, int dim) {
    FOR_TEXEL {
        float a = (float)(x * 2 + y * 3) * (6.2831853f / 16.0f);
        float b = (float)(x * 3 - y) * (6.2831853f / 16.0f);
        int w = (int)(7.0f * sinf(a) + 5.0f * sinf(b)) + nz(x, y, salt, 4);
        T(x, y) = rgb(44 + w - dim, 98 + w - dim, 176 + w - dim);
    }
}

static void gen_plank(uint32_t *t, uint32_t salt) {
    FOR_TEXEL {
        int row = y >> 2;
        int v = nz(x, row, salt, 11) + nz(x, y, salt + 1, 5);
        int r = 162 + v, g = 130 + v, b = 84 + v / 2;
        if ((y & 3) == 0) { r -= 42; g -= 36; b -= 24; }              /* seam */
        int seam = (row * 5 + 3) & 15;
        if (x == seam) { r -= 46; g -= 40; b -= 28; }
        T(x, y) = rgb(r, g, b);
    }
}

static void gen_glass(uint32_t *t, uint32_t salt) {
    (void)salt;
    FOR_TEXEL {
        int edge = (x == 0 || y == 0 || x == TEXW - 1 || y == TEXW - 1);
        if (edge)                       T(x, y) = argb(255, 198, 222, 234);
        else if (((x + y) & 15) < 2)    T(x, y) = argb(74,  228, 244, 250);
        else if (x == 1 && y < 5)       T(x, y) = argb(120, 236, 248, 252);
        else                            T(x, y) = argb(26,  200, 228, 238);
    }
}

static void gen_brick(uint32_t *t, uint32_t salt) {
    FOR_TEXEL {
        int row = y >> 2;
        int off = (row & 1) * 4;
        int bx = (x + off) & 15;
        if ((y & 3) == 0 || (bx & 7) == 0) {
            int v = nz(x, y, salt, 6);
            T(x, y) = rgb(170 + v, 163 + v, 155 + v);
        } else {
            int bh = (int)(hash3((x + off) >> 3, row, 0, salt) & 15u) - 7;
            int v = bh + nz(x, y, salt + 1, 8);
            T(x, y) = rgb(150 + v, 72 + v, 58 + v / 2);
        }
    }
}

static void gen_ore(uint32_t *t, uint32_t salt) {
    gen_flat(t, 128, 128, 132, 13, 8, salt);
    FOR_TEXEL {
        for (int oy = -1; oy <= 1; oy++)
            for (int ox = -1; ox <= 1; ox++) {
                int gx = (x >> 3) + ox, gy = (y >> 3) + oy;
                uint32_t h = hash3(gx & 1, gy & 1, 0, salt + 9);
                if ((h & 3) == 0) continue;               /* this cell is bare */
                float cx = (float)(gx * 8 + 2 + (int)(h & 3));
                float cy = (float)(gy * 8 + 2 + (int)((h >> 3) & 3));
                float dx = cx - (float)x, dy = cy - (float)y;
                float d = fsqrt(dx * dx + dy * dy);
                float r = 1.9f + (float)((h >> 6) & 1) * 0.7f;
                if (d > r) continue;
                int v = nz(x, y, salt + 11, 7);
                if (d > r - 0.9f) T(x, y) = rgb(72 + v, 70 + v, 76 + v);
                else              T(x, y) = rgb(38 + v, 37 + v, 44 + v);
            }
    }
}

static void gen_snow(uint32_t *t, uint32_t salt) {
    FOR_TEXEL {
        int v = nz(x, y, salt, 5) + nz(x >> 2, y >> 2, salt + 1, 4);
        if ((hash3(x, y, 2, salt) & 31) < 3)
            T(x, y) = rgb(220 + v, 230 + v, 246 + v);
        else
            T(x, y) = rgb(238 + v, 243 + v, 250 + v);
    }
}

static void gen_bedrock(uint32_t *t, uint32_t salt) {
    FOR_TEXEL {
        int v = nz(x, y, salt, 26) + nz(x >> 1, y >> 1, salt + 1, 20);
        T(x, y) = rgb(68 + v, 68 + v, 72 + v);
    }
}

void tex_build(void) {
    for (int f = 0; f < 3; f++) {
        uint32_t s = 0x9000u + (uint32_t)f * 137u;

        gen_dirt(g_tex[B_DIRT][f], s);
        gen_flat(g_tex[B_STONE][f], 126, 126, 130, 14, 9, s + 20);
        gen_flat(g_tex[B_SAND][f], 219, 207, 163, 9, 5, s + 30);
        gen_cobble(g_tex[B_COBBLE][f], s + 50);
        gen_leaves(g_tex[B_LEAVES][f], s + 60);
        gen_plank(g_tex[B_PLANK][f], s + 70);
        gen_glass(g_tex[B_GLASS][f], s + 80);
        gen_brick(g_tex[B_BRICK][f], s + 90);
        gen_ore(g_tex[B_ORE][f], s + 100);
        gen_snow(g_tex[B_SNOW][f], s + 110);
        gen_bedrock(g_tex[B_BEDROCK][f], s + 120);
        gen_water(g_tex[B_WATER][f], s + 130, f == F_TOP ? 0 : 10);
    }

    /* The three blocks that actually care which way is up. */
    gen_grass_top (g_tex[B_GRASS][F_TOP],  0x6001);
    gen_grass_side(g_tex[B_GRASS][F_SIDE], 0x6002);
    gen_dirt      (g_tex[B_GRASS][F_BOT],  0x6003);

    gen_log_top (g_tex[B_LOG][F_TOP],  0x7001);
    gen_log_side(g_tex[B_LOG][F_SIDE], 0x7002);
    gen_log_top (g_tex[B_LOG][F_BOT],  0x7003);

    /* B_AIR is never sampled, but leaving it as whatever malloc'd garbage
     * .bss happens to hold is the sort of thing that bites during a refactor. */
    for (int f = 0; f < 3; f++)
        for (int i = 0; i < TEXW * TEXW; i++) g_tex[B_AIR][f][i] = 0xFFFF00FFu;
}

const char * const blk_name[B_COUNT] = {
    "air", "grass", "dirt", "stone", "cobblestone", "sand",
    "oak log", "leaves", "planks", "glass", "bricks", "coal ore",
    "snow", "water", "bedrock"
};

const uint8_t blk_opaque[B_COUNT] = {
/*  air gr di st co sa lo le pl gl br or sn wa be */
     0,  1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1
};
const uint8_t blk_solid[B_COUNT] = {
     0,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1
};
const uint8_t blk_seethru[B_COUNT] = {
     0,  0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0
};
