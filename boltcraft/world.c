/* Terrain. A heightmap from four octaves of periodic value noise, worm caves
 * cut through it afterwards, then trees scattered on whatever grass survived.
 *
 * "Periodic" is the load-bearing word. The noise lattice wraps at 256, the
 * block index wraps at 256, so walking east far enough brings you back where
 * you started and no ray ever finds an edge. That is the entire chunk system:
 * there isn't one.
 */
#include "craft.h"

uint8_t  *g_world;
uint8_t   g_top[WX * WZ];
int       g_ymax;
uint32_t  g_seed;

void world_alloc(void) {
    g_world = (uint8_t *)malloc((unsigned long)WX * WZ * WY);
}

/* ----------------------------------------------------------------- noise -- */

/* One lattice point. `step` is the spacing, so the lattice is WX/step wide and
 * the wrap is an AND -- which is why step has to stay a power of two. */
static float lattice(int xi, int zi, int step, uint32_t salt) {
    int m = (WX / step) - 1;
    uint32_t h = hash3(xi & m, 0, zi & m, salt);
    return (float)(h >> 8) * (1.0f / 8388608.0f) - 1.0f;
}

static float vnoise(float x, float z, int step, uint32_t salt) {
    float fx = x * (1.0f / step), fz = z * (1.0f / step);
    int x0 = ifloor(fx), z0 = ifloor(fz);
    float tx = fx - (float)x0, tz = fz - (float)z0;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);

    float a = lattice(x0,     z0,     step, salt);
    float b = lattice(x0 + 1, z0,     step, salt);
    float c = lattice(x0,     z0 + 1, step, salt);
    float d = lattice(x0 + 1, z0 + 1, step, salt);

    float ab = a + (b - a) * tx;
    float cd = c + (d - c) * tx;
    return ab + (cd - ab) * tz;
}

static int height_at(int x, int z, uint32_t s) {
    float fx = (float)x, fz = (float)z;
    float h = 28.0f
            + 13.0f * vnoise(fx, fz, 64, s + 1)
            +  6.5f * vnoise(fx, fz, 32, s + 2)
            +  3.0f * vnoise(fx, fz, 16, s + 3)
            +  1.5f * vnoise(fx, fz,  8, s + 4);

    /* Mountains, gated so most of the map stays walkable. Without the gate the
     * whole world is lumpy at the same scale and nothing reads as a landmark. */
    float ridge = vnoise(fx, fz, 64, s + 9);
    if (ridge > 0.15f) h += (ridge - 0.15f) * 44.0f;

    int ih = (int)h;
    return clampi(ih, 2, WY - 6);
}

/* ------------------------------------------------------------ generation -- */

static void put(int x, int y, int z, uint8_t b) {
    if ((unsigned)y >= WY) return;
    g_world[WIDX(x, y, z)] = b;
}

static void carve_caves(uint32_t s) {
    const int worms = 110;
    for (int w = 0; w < worms; w++) {
        uint32_t h = hash3(w, 77, 13, s);
        float cx = (float)(h & WXM);
        float cz = (float)((h >> 8) & WZM);
        float cy = 8.0f + (float)((h >> 16) & 31);
        float yaw = (float)((h >> 21) & 1023) * (6.2831853f / 1024.0f);
        float pit = -0.35f + (float)((h >> 4) & 63) * (0.7f / 64.0f);

        int len = 120 + (int)((h >> 12) & 127);
        for (int i = 0; i < len; i++) {
            uint32_t t = hash3(w, i, 5, s + 31);
            yaw += ((float)(t & 255) - 127.5f) * (1.0f / 900.0f);
            pit += ((float)((t >> 8) & 255) - 127.5f) * (1.0f / 2600.0f);
            pit = fclampf(pit, -0.55f, 0.45f);

            cx += cosf(pit) * sinf(yaw);
            cz += cosf(pit) * cosf(yaw);
            cy += sinf(pit) * 0.7f;
            if (cy < 3.0f)  { cy = 3.0f;  pit = 0.2f; }
            if (cy > 44.0f) { cy = 44.0f; pit = -0.2f; }

            float r = 2.0f + (float)((t >> 16) & 3) * 0.45f;
            int ir = (int)r + 1;
            int bx = ifloor(cx), by = ifloor(cy), bz = ifloor(cz);
            for (int dy = -ir; dy <= ir; dy++)
                for (int dz = -ir; dz <= ir; dz++)
                    for (int dx = -ir; dx <= ir; dx++) {
                        if ((float)(dx*dx + dy*dy + dz*dz) > r * r) continue;
                        int y = by + dy;
                        if (y < 1 || y >= WY) continue;
                        uint8_t o = g_world[WIDX(bx + dx, y, bz + dz)];
                        /* Leave water where it is. A cave that eats the sea
                         * floor drains the ocean into a hole you cannot see,
                         * which looked like a generator bug for an hour. */
                        if (o == B_AIR || o == B_WATER || o == B_BEDROCK) continue;
                        put(bx + dx, y, bz + dz, B_AIR);
                    }
        }
    }
}

static void plant_tree(int x, int z, int y, uint32_t r) {
    int trunk = 4 + (int)(r & 3);
    if (y + trunk + 3 >= WY) return;

    for (int i = 0; i < trunk; i++) put(x, y + i, z, B_LOG);

    int top = y + trunk;
    /* Two wide layers with the corners knocked off, a small cap, then a plus.
     * Straight out of the shape everyone recognises; there is no cleverness
     * here and it would look wrong if there were. */
    for (int ly = 0; ly < 2; ly++) {
        int cy = top - 2 + ly;
        for (int dz = -2; dz <= 2; dz++)
            for (int dx = -2; dx <= 2; dx++) {
                int ax = dx < 0 ? -dx : dx, az = dz < 0 ? -dz : dz;
                if (ax == 2 && az == 2 && (r >> (ly + 4) & 1)) continue;
                if (g_world[WIDX(x + dx, cy, z + dz)] == B_AIR)
                    put(x + dx, cy, z + dz, B_LEAVES);
            }
    }
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++)
            if (g_world[WIDX(x + dx, top, z + dz)] == B_AIR)
                put(x + dx, top, z + dz, B_LEAVES);
    put(x, top + 1, z, B_LEAVES);
    put(x + 1, top, z, B_LEAVES);
    put(x - 1, top, z, B_LEAVES);
    put(x, top, z + 1, B_LEAVES);
    put(x, top, z - 1, B_LEAVES);
}

void world_retop(int x, int z) {
    int y = WY - 1;
    while (y > 0 && g_world[WIDX(x, y, z)] == B_AIR) y--;
    g_top[((z & WZM) << 8) | (x & WXM)] = (uint8_t)(y + 1);
    if (y + 1 > g_ymax) g_ymax = y + 1;
}

void world_generate(uint32_t seed) {
    g_seed = seed;
    fill_bytes(g_world, B_AIR, (unsigned long)WX * WZ * WY);

    for (int z = 0; z < WZ; z++) {
        for (int x = 0; x < WX; x++) {
            int top = height_at(x, z, seed);
            int snow = 41 + (int)(4.0f * vnoise((float)x, (float)z, 16, seed + 11));
            int beach = (top <= WATER_Y + 1);

            for (int y = 0; y <= top; y++) {
                uint8_t b;
                if (y == 0)            b = B_BEDROCK;
                else if (y == top)     b = beach ? B_SAND : (top >= snow ? B_SNOW : B_GRASS);
                else if (y > top - 4)  b = beach ? B_SAND : B_DIRT;
                else {
                    /* Ore in 2x2x2 clumps, and only down where you would have
                     * to dig for it. A single speckled block reads as noise. */
                    b = B_STONE;
                    if (y < 34 && (hash3(x >> 1, y >> 1, z >> 1, seed + 7) & 255) < 5)
                        b = B_ORE;
                }
                g_world[WIDX(x, y, z)] = b;
            }
            for (int y = top + 1; y <= WATER_Y; y++)
                g_world[WIDX(x, y, z)] = B_WATER;
        }
    }

    carve_caves(seed);

    for (int z = 0; z < WZ; z++)
        for (int x = 0; x < WX; x++) {
            uint32_t r = hash3(x, 3, z, seed + 5);
            if ((r & 511) >= 6) continue;
            int y = WY - 1;
            while (y > 0 && g_world[WIDX(x, y, z)] == B_AIR) y--;
            if (g_world[WIDX(x, y, z)] != B_GRASS) continue;
            if (y < WATER_Y) continue;
            plant_tree(x, z, y + 1, r >> 9);
        }

    g_ymax = 0;
    for (int z = 0; z < WZ; z++)
        for (int x = 0; x < WX; x++) world_retop(x, z);
}

void world_set(int x, int y, int z, uint8_t b) {
    if ((unsigned)y >= WY) return;
    g_world[WIDX(x, y, z)] = b;
    world_retop(x, z);
}

/* Spiral out from the middle. Pass 0 insists on grass in the low country,
 * which is where the trees and the coastline are; pass 1 will take anything
 * that is not sea, for the seeds where the centre of the map is a glacier.
 * The first version had only pass 1 and dropped me on a snow peak at y=53,
 * staring at white in every direction. */
int world_spawn(float *px, float *py, float *pz) {
    for (int pass = 0; pass < 2; pass++) {
        for (int r = 0; r < 128; r++) {
            for (int i = -r; i <= r; i++) {
                int cand[4][2] = { {128 + i, 128 - r}, {128 + i, 128 + r},
                                   {128 - r, 128 + i}, {128 + r, 128 + i} };
                for (int k = 0; k < 4; k++) {
                    int x = cand[k][0] & WXM, z = cand[k][1] & WZM;
                    int t = g_top[((z & WZM) << 8) | x];
                    if (t <= WATER_Y + 1 || t >= WY - 3) continue;
                    uint8_t b = g_world[WIDX(x, t - 1, z)];
                    if (pass == 0) {
                        if (b != B_GRASS || t > WATER_Y + 9) continue;
                    } else if (b != B_GRASS && b != B_SAND && b != B_SNOW) continue;
                    *px = (float)x + 0.5f;
                    *py = (float)t;
                    *pz = (float)z + 0.5f;
                    return 1;
                }
            }
        }
    }
    *px = 128.5f; *py = (float)(WATER_Y + 2); *pz = 128.5f;
    return 0;
}

/* ------------------------------------------------------------ save/load --- */
/* Run-length, because a voxel world is nothing but runs: 4 MiB of blocks comes
 * out somewhere around 80 KiB and writes in well under a second even on ramfs.
 * Header is magic, seed and the five camera floats, then (block, count) pairs
 * to the end of the file. */

#define WLD_MAGIC 0x31574342u   /* "BCW1" */

static int wr(int fd, const void *p, unsigned long n) {
    const uint8_t *b = (const uint8_t *)p;
    while (n) {
        unsigned long c = n > 32768 ? 32768 : n;
        long got = write(fd, b, c);
        if (got <= 0) return -1;
        b += got; n -= (unsigned long)got;
    }
    return 0;
}

int world_save(const char *path, const struct camera *cam) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;

    unsigned long cap = 1u << 20;
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) { close(fd); return -1; }
    unsigned long n = 0;

    uint32_t magic = WLD_MAGIC;
    memcpy(out + n, &magic, 4);     n += 4;
    memcpy(out + n, &g_seed, 4);    n += 4;
    memcpy(out + n, &cam->x, 4);    n += 4;
    memcpy(out + n, &cam->y, 4);    n += 4;
    memcpy(out + n, &cam->z, 4);    n += 4;
    memcpy(out + n, &cam->yaw, 4);  n += 4;
    memcpy(out + n, &cam->pitch, 4);n += 4;

    unsigned long total = (unsigned long)WX * WZ * WY, i = 0;
    while (i < total) {
        uint8_t b = g_world[i];
        unsigned long j = i + 1;
        while (j < total && g_world[j] == b) j++;
        uint32_t run = (uint32_t)(j - i);

        if (n + 5 > cap) {
            if (wr(fd, out, n) != 0) { free(out); close(fd); return -1; }
            n = 0;
        }
        out[n++] = b;
        memcpy(out + n, &run, 4); n += 4;
        i = j;
    }
    int rc = wr(fd, out, n);
    free(out);
    close(fd);
    return rc;
}

int world_load(const char *path, struct camera *cam) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    stat_t st;
    if (fstat(fd, &st) != 0 || st.size < 32) { close(fd); return -1; }

    uint8_t *in = (uint8_t *)malloc((unsigned long)st.size);
    if (!in) { close(fd); return -1; }
    unsigned long got = 0;
    while (got < st.size) {
        long r = read(fd, in + got, (unsigned long)st.size - got);
        if (r <= 0) break;
        got += (unsigned long)r;
    }
    close(fd);
    if (got != st.size) { free(in); return -1; }

    uint32_t magic;
    memcpy(&magic, in, 4);
    if (magic != WLD_MAGIC) { free(in); return -1; }

    unsigned long n = 4;
    memcpy(&g_seed, in + n, 4);    n += 4;
    memcpy(&cam->x, in + n, 4);    n += 4;
    memcpy(&cam->y, in + n, 4);    n += 4;
    memcpy(&cam->z, in + n, 4);    n += 4;
    memcpy(&cam->yaw, in + n, 4);  n += 4;
    memcpy(&cam->pitch, in + n, 4);n += 4;

    unsigned long total = (unsigned long)WX * WZ * WY, i = 0;
    while (i < total && n + 5 <= got) {
        uint8_t b = in[n++];
        uint32_t run;
        memcpy(&run, in + n, 4); n += 4;
        if (run > total - i) run = (uint32_t)(total - i);
        fill_bytes(g_world + i, b, run);
        i += run;
    }
    free(in);
    if (i != total) return -1;

    g_ymax = 0;
    for (int z = 0; z < WZ; z++)
        for (int x = 0; x < WX; x++) world_retop(x, z);
    return 0;
}
