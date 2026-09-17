/* The raycaster.
 *
 * One Amanatides-and-Woo DDA per pixel, at a fraction of the panel resolution,
 * then a nearest-neighbour upscale. Rendering at 1024x768 directly is about
 * seven times the rays and BoltOS has no GPU path to spend them on; a 1/3
 * buffer blown up looks like the era the game is quoting anyway, and the HUD
 * is drawn afterwards at full resolution so the text stays sharp.
 *
 * The ray does not stop at the first block it touches. Leaves have holes in
 * their texture and glass is mostly hole, so a sample with alpha composites
 * and carries on; water is treated as a volume rather than a surface, because
 * compositing every face of a twelve-deep ocean turns it into ink.
 */
#include "craft.h"

static uint32_t *rbuf;
static int FBW, FBH;
static int SCALE = 3;
static int RW, RH;
static int VIEWDIST = 72;

/* face brightness, 0..256. Top full, bottom darkest, and the two horizontal
 * axes deliberately unequal so a corner reads as a corner. */
#define SH_TOP 256
#define SH_X   202
#define SH_Z   164
#define SH_BOT 124

#define CM 64
static float cloudmap[CM * CM];

/* ------------------------------------------------------------------ sky --- */

static float cl_lattice(int x, int z, int step, uint32_t salt) {
    int m = (CM / step) - 1;
    uint32_t h = hash3(x & m, 0, z & m, salt);
    return (float)(h >> 8) * (1.0f / 8388608.0f) - 1.0f;
}
static float cl_noise(float x, float z, int step, uint32_t salt) {
    float fx = x * (1.0f / step), fz = z * (1.0f / step);
    int x0 = ifloor(fx), z0 = ifloor(fz);
    float tx = fx - (float)x0, tz = fz - (float)z0;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    float a = cl_lattice(x0, z0, step, salt),     b = cl_lattice(x0 + 1, z0, step, salt);
    float c = cl_lattice(x0, z0 + 1, step, salt), d = cl_lattice(x0 + 1, z0 + 1, step, salt);
    float ab = a + (b - a) * tx, cd = c + (d - c) * tx;
    return ab + (cd - ab) * tz;
}

static void clouds_build(void) {
    for (int z = 0; z < CM; z++)
        for (int x = 0; x < CM; x++) {
            float v = 0.55f * cl_noise((float)x, (float)z, 16, 0xC1)
                    + 0.30f * cl_noise((float)x, (float)z,  8, 0xC2)
                    + 0.15f * cl_noise((float)x, (float)z,  4, 0xC3);
            cloudmap[z * CM + x] = v;
        }
}

static float cloud_at(float x, float z) {
    int x0 = ifloor(x), z0 = ifloor(z);
    float tx = x - (float)x0, tz = z - (float)z0;
    int a = ((z0 & (CM-1)) * CM) + (x0 & (CM-1));
    int b = ((z0 & (CM-1)) * CM) + ((x0 + 1) & (CM-1));
    int c = (((z0 + 1) & (CM-1)) * CM) + (x0 & (CM-1));
    int d = (((z0 + 1) & (CM-1)) * CM) + ((x0 + 1) & (CM-1));
    float ab = cloudmap[a] + (cloudmap[b] - cloudmap[a]) * tx;
    float cd = cloudmap[c] + (cloudmap[d] - cloudmap[c]) * tx;
    return ab + (cd - ab) * tz;
}

/* Set once per frame so the per-pixel sky is a handful of lerps. */
static uint32_t sky_zen, sky_hor;
static float    sun_x, sun_y, sun_z;
static int      night;                /* 0..256: how much of the star field shows */
static float    cam_x, cam_y, cam_z;

static void sky_setup(float daylight, float phase) {
    /* phase drives the sun around; daylight is what the shader multiplies by,
     * and the two are not the same curve -- the light lags the geometry a
     * little, which is what makes dusk last longer than it should. */
    sun_y = sinf(phase);
    sun_x = cosf(phase) * 0.40f;
    sun_z = cosf(phase) * 0.92f;
    float l = fsqrt(sun_x*sun_x + sun_y*sun_y + sun_z*sun_z);
    sun_x /= l; sun_y /= l; sun_z /= l;

    int d = (int)(fclampf(daylight, 0.0f, 1.0f) * 256.0f);
    sky_zen = mix32(0x04060Fu, 0x3D77CCu, d);
    sky_hor = mix32(0x0D1424u, 0xAFCDF2u, d);

    /* Sunset: only when the sun is near the horizon, and only in the warm
     * direction. Cheap version -- the whole horizon band goes orange rather
     * than just the part near the sun. Nobody has complained. */
    float low = 1.0f - fclampf(fabsf(sun_y) * 3.2f, 0.0f, 1.0f);
    if (low > 0.0f && daylight > 0.10f)
        sky_hor = mix32(sky_hor, 0xE8843Cu, (int)(low * 150.0f));

    night = 256 - d;
    if (night > 256) night = 256;
}

static uint32_t sky_grad(float dy) {
    float t = fclampf(dy * 1.35f + 0.18f, 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);
    return mix32(sky_hor, sky_zen, (int)(t * 256.0f));
}

static uint32_t sky_full(float dx, float dy, float dz) {
    uint32_t c = sky_grad(dy);

    if (night > 90 && dy > 0.02f) {
        uint32_t h = hash3((int)(dx * 340.0f), (int)(dy * 340.0f), (int)(dz * 340.0f), 0x57A5);
        if ((h & 2047u) < 3u) {
            int b = 130 + (int)((h >> 12) & 125u);
            c = mix32(c, rgb_pack(b, b, b + 10), night - 60);
        }
    }

    float sd = dx * sun_x + dy * sun_y + dz * sun_z;
    if (sd > 0.0f) {
        float g = sd * sd; g *= g; g *= g;            /* sd^8  */
        float g2 = g * g; g2 *= g2;                   /* sd^32 */
        int glow = (int)(g2 * 150.0f + g * 34.0f);
        uint32_t warm = sun_y > 0.20f ? 0xFFF2C0u : 0xFF8C3Cu;
        if (glow > 0) c = mix32(c, warm, clampi(glow, 0, 256));
        if (sd > 0.99965f) c = sun_y > 0.20f ? 0xFFFBE8u : 0xFFB05Cu;
    }

    if (dy > 0.015f) {
        float t = (118.0f - cam_y) / dy;
        if (t > 0.0f && t < 5200.0f) {
            float cx = (cam_x + dx * t) * 0.016f;
            float cz = (cam_z + dz * t) * 0.016f;
            float v = cloud_at(cx, cz);
            float cov = fclampf((v - 0.06f) * 3.4f, 0.0f, 1.0f);
            cov *= fclampf(dy * 7.0f, 0.0f, 1.0f);        /* thin at the horizon */
            if (cov > 0.004f) {
                /* Clouds lit by the sun, not by the fact that they are clouds.
                 * At a flat 150 they glowed all through the night. */
                int lit = 26 + (int)(fclampf(sun_y * 1.6f + 0.15f, 0.0f, 1.0f) * 224.0f);
                uint32_t cc = rgb_pack(lit, lit, lit + 6);
                c = mix32(c, cc, (int)(cov * 232.0f));
            }
        }
    }
    return c;
}

/* --------------------------------------------------------------- the ray -- */

static uint32_t trace(float ox, float oy, float oz,
                      float dx, float dy, float dz,
                      int lightf, const struct hit *sel)
{
    int ix = ifloor(ox), iy = ifloor(oy), iz = ifloor(oz);
    int sx = dx > 0.0f ? 1 : -1, sy = dy > 0.0f ? 1 : -1, sz = dz > 0.0f ? 1 : -1;

    float tdx = dx != 0.0f ? (dx > 0.0f ? 1.0f / dx : -1.0f / dx) : 1e30f;
    float tdy = dy != 0.0f ? (dy > 0.0f ? 1.0f / dy : -1.0f / dy) : 1e30f;
    float tdz = dz != 0.0f ? (dz > 0.0f ? 1.0f / dz : -1.0f / dz) : 1e30f;

    float tmx = dx != 0.0f ? (dx > 0.0f ? ((float)(ix + 1) - ox) : (ox - (float)ix)) * tdx : 1e30f;
    float tmy = dy != 0.0f ? (dy > 0.0f ? ((float)(iy + 1) - oy) : (oy - (float)iy)) * tdy : 1e30f;
    float tmz = dz != 0.0f ? (dz > 0.0f ? ((float)(iz + 1) - oz) : (oz - (float)iz)) * tdz : 1e30f;

    float maxd = (float)VIEWDIST;
    float fog0 = maxd * 0.45f, fogk = 1.0f / (maxd - maxd * 0.45f);

    int ar = 0, ag = 0, ab = 0, trans = 256;

    int   inwater = 0, wsurf = 0;
    float wt0 = 0.0f, wdist = 0.0f;
    uint32_t wsurfc = 0;
    int   wsurfa = 0;

    if (blk_at(ix, iy, iz) == B_WATER) { inwater = 1; wt0 = 0.0f; }

    float t = 0.0f;
    int steps = VIEWDIST * 3 + 24;

    /* TODO: this walks every voxel of empty sky between here and the far side
     * of a valley. A coarse 8x8x8 occupancy grid to skip over would be worth
     * maybe a third of the frame, and is the obvious next thing if anyone
     * wants detail 1/2 to be playable on a machine without virtualisation. */
    while (steps--) {
        int axis;
        if (tmx < tmy) {
            if (tmx < tmz) { t = tmx; ix += sx; tmx += tdx; axis = 0; }
            else           { t = tmz; iz += sz; tmz += tdz; axis = 2; }
        } else {
            if (tmy < tmz) { t = tmy; iy += sy; tmy += tdy; axis = 1; }
            else           { t = tmz; iz += sz; tmz += tdz; axis = 2; }
        }
        if (t >= maxd) break;
        if (iy < 0) break;
        if (iy >= g_ymax) { if (sy > 0) break; continue; }

        uint8_t b = g_world[WIDX(ix, iy, iz)];

        if (b == B_WATER) {
            if (!inwater) {
                inwater = 1; wt0 = t;
                if (!wsurf) {
                    wsurf = 1;
                    int face = axis == 1 ? (sy > 0 ? F_BOT : F_TOP) : F_SIDE;
                    float hx = ox + dx * t, hy = oy + dy * t, hz = oz + dz * t;
                    float u, v;
                    if (axis == 0)      { u = hz - (float)ifloor(hz); v = 1.0f - (hy - (float)ifloor(hy)); }
                    else if (axis == 1) { u = hx - (float)ifloor(hx); v = hz - (float)ifloor(hz); }
                    else                { u = hx - (float)ifloor(hx); v = 1.0f - (hy - (float)ifloor(hy)); }
                    int tu = (int)(u * 16.0f) & 15, tv = (int)(v * 16.0f) & 15;
                    wsurfc = g_tex[B_WATER][face][tv * 16 + tu] & 0x00FFFFFFu;
                    wsurfc = scale32(wsurfc, (face == F_TOP ? SH_TOP : SH_Z) * lightf >> 8);
                    wsurfa = face == F_TOP ? 122 : 86;
                }
            }
            continue;
        }
        if (inwater) { inwater = 0; wdist += t - wt0; }
        if (b == B_AIR) continue;

        /* ---- a real sample ------------------------------------------- */
        int face, shade;
        if (axis == 0)      { face = F_SIDE; shade = SH_X; }
        else if (axis == 2) { face = F_SIDE; shade = SH_Z; }
        else if (sy > 0)    { face = F_BOT;  shade = SH_BOT; }
        else                { face = F_TOP;  shade = SH_TOP; }

        float hx = ox + dx * t, hy = oy + dy * t, hz = oz + dz * t;
        float u, v;
        if (axis == 0)      { u = hz - (float)ifloor(hz); v = 1.0f - (hy - (float)ifloor(hy)); }
        else if (axis == 1) { u = hx - (float)ifloor(hx); v = hz - (float)ifloor(hz); }
        else                { u = hx - (float)ifloor(hx); v = 1.0f - (hy - (float)ifloor(hy)); }

        int tu = (int)(u * 16.0f) & 15, tv = (int)(v * 16.0f) & 15;
        uint32_t texel = g_tex[b][face][tv * 16 + tu];
        int alpha = blk_seethru[b] ? (int)(texel >> 24) : 255;
        if (alpha == 0) continue;

        /* How buried the air cell in front of this face is. One lookup buys
         * caves that go dark and a canopy that casts something like shade. */
        /* The air cell the ray came out of: one step back along whichever axis
         * moved. Cheaper than carrying the previous voxel through every step
         * of the loop for the one step in a hundred that needs it. */
        int px = axis == 0 ? ix - sx : ix;
        int py = axis == 1 ? iy - sy : iy;
        int pz = axis == 2 ? iz - sz : iz;

        int colv = g_top[((pz & WZM) << 8) | (px & WXM)];
        int buried = colv - py;
        int dfac = 256;
        if (buried > 0) {
            dfac = 256 - clampi(buried, 0, 14) * 13;
            /* Whatever you are carrying. Real block lighting means a flood
             * fill on every edit and a light value per voxel; this is a
             * falloff around the camera, it costs one multiply, and it is the
             * difference between a cave you can explore and a black screen. */
            int torch = 190 - (int)(t * 13.0f);
            if (torch > 0) { dfac += torch; if (dfac > 256) dfac = 256; }
        }

        int sh = ((shade * dfac) >> 8) * lightf >> 8;
        uint32_t c = scale32(texel & 0x00FFFFFFu, sh);

        if (sel->ok && ix == sel->bx && iy == sel->by && iz == sel->bz) {
            float eu = u < 0.5f ? u : 1.0f - u;
            float ev = v < 0.5f ? v : 1.0f - v;
            if (eu < 0.035f || ev < 0.035f) c = mix32(c, 0x101014u, 200);
        }

        float fog = (t - fog0) * fogk;
        if (fog > 0.0f) c = mix32(c, sky_grad(dy), (int)(fclampf(fog, 0.0f, 1.0f) * 256.0f));

        int contrib = alpha == 255 ? trans : (trans * alpha) >> 8;
        ar += (int)((c >> 16) & 255u) * contrib >> 8;
        ag += (int)((c >> 8)  & 255u) * contrib >> 8;
        ab += (int)( c        & 255u) * contrib >> 8;
        trans -= contrib;
        if (trans <= 3) { trans = 0; break; }
    }

    if (inwater) wdist += t - wt0;

    if (trans > 0) {
        uint32_t s = sky_full(dx, dy, dz);
        ar += (int)((s >> 16) & 255u) * trans >> 8;
        ag += (int)((s >> 8)  & 255u) * trans >> 8;
        ab += (int)( s        & 255u) * trans >> 8;
    }

    uint32_t out = rgb_pack(ar, ag, ab);

    /* Absorption, not a surface colour. Two blocks of water already hides most
     * of the sand under it -- at the 1/(1+d/8) I started with, a shallow bay
     * looked like wet concrete. */
    if (wdist > 0.0f) {
        int a = (int)(wdist / (wdist + 3.0f) * 236.0f);
        out = mix32(out, scale32(0x1A5A9Eu, lightf), a);
    }
    if (wsurf) out = mix32(out, wsurfc, wsurfa);
    return out;
}

/* ------------------------------------------------------------ the frame -- */

void render_init(uint32_t *buf, int fbw, int fbh) {
    rbuf = buf; FBW = fbw; FBH = fbh;
    clouds_build();
    render_set_scale(SCALE);
}

void render_set_scale(int s) {
    SCALE = clampi(s, 1, 10);
    RW = (FBW + SCALE - 1) / SCALE;
    RH = (FBH + SCALE - 1) / SCALE;
}
int  render_get_scale(void) { return SCALE; }
void render_set_dist(int d) { VIEWDIST = clampi(d, 20, 160); }
int  render_get_dist(void)  { return VIEWDIST; }

void render_world(const struct camera *cam, float daylight, float phase,
                  const struct hit *sel)
{
    cam_x = cam->x; cam_y = cam->y; cam_z = cam->z;
    sky_setup(daylight, phase);

    int lightf = 46 + (int)(fclampf(daylight, 0.0f, 1.0f) * 210.0f);

    float cp = cosf(cam->pitch), sp = sinf(cam->pitch);
    float cy = cosf(cam->yaw),   sy = sinf(cam->yaw);

    float fx = cp * sy, fy = sp, fz = cp * cy;
    float rx = cy,      ry = 0.0f, rz = -sy;
    float ux = -sy * sp, uy = cp,  uz = -cy * sp;

    const float tanh2 = 0.7002f;                   /* 70 degrees vertical */
    float aspect = (float)FBW / (float)FBH;
    float sw = tanh2 * aspect, sh = tanh2;

    for (int py = 0; py < RH; py++) {
        float b = (1.0f - 2.0f * ((float)py + 0.5f) / (float)RH) * sh;
        float bx = fx + ux * b, by = fy + uy * b, bz = fz + uz * b;
        uint32_t *row = rbuf + (size_t)py * RW;

        for (int px = 0; px < RW; px++) {
            float a = (2.0f * ((float)px + 0.5f) / (float)RW - 1.0f) * sw;
            float dx = bx + rx * a, dy = by + ry * a, dz = bz + rz * a;
            float inv = 1.0f / fsqrt(dx * dx + dy * dy + dz * dz);
            dx *= inv; dy *= inv; dz *= inv;
            row[px] = trace(cam->x, cam->y, cam->z, dx, dy, dz, lightf, sel);
        }
    }
}

/* libc's memcpy is a byte loop (libc/string.c), and duplicating rows with it
 * costs two megabytes of single-byte stores a frame. One instruction instead,
 * which QEMU's interpreter also has a fast path for. */
static inline void copy_qwords(void *d, const void *s, unsigned long q) {
    __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(q) :: "memory");
}

void render_blit(uint32_t *fb, int fbw, int fbh) {
    int s = SCALE;
    if (s == 1) {
        copy_qwords(fb, rbuf, ((unsigned long)fbw * fbh) >> 1);
        if ((fbw * fbh) & 1) fb[fbw * fbh - 1] = rbuf[fbw * fbh - 1];
        return;
    }
    for (int ry = 0; ry < RH; ry++) {
        int y0 = ry * s;
        if (y0 >= fbh) break;
        const uint32_t *src = rbuf + (size_t)ry * RW;
        uint32_t *d0 = fb + (size_t)y0 * fbw;

        int w = 0;
        for (int rx = 0; rx < RW && w < fbw; rx++) {
            uint32_t c = src[rx];
            for (int k = 0; k < s && w < fbw; k++) d0[w++] = c;
        }
        int rows = fbh - y0; if (rows > s) rows = s;
        for (int r = 1; r < rows; r++) {
            uint32_t *dst = fb + (size_t)(y0 + r) * fbw;
            copy_qwords(dst, d0, (unsigned long)fbw >> 1);
            if (fbw & 1) dst[fbw - 1] = d0[fbw - 1];
        }
    }
}

/* ---------------------------------------------------------- what you hit -- */

int ray_pick(const struct camera *cam, float reach, struct hit *out)
{
    float cp = cosf(cam->pitch), sp = sinf(cam->pitch);
    float cy = cosf(cam->yaw),   sy = sinf(cam->yaw);
    float dx = cp * sy, dy = sp, dz = cp * cy;

    out->ok = 0;
    float ox = cam->x, oy = cam->y, oz = cam->z;
    int ix = ifloor(ox), iy = ifloor(oy), iz = ifloor(oz);
    int sx = dx > 0.0f ? 1 : -1, ssy = dy > 0.0f ? 1 : -1, sz = dz > 0.0f ? 1 : -1;

    float tdx = dx != 0.0f ? (dx > 0.0f ? 1.0f / dx : -1.0f / dx) : 1e30f;
    float tdy = dy != 0.0f ? (dy > 0.0f ? 1.0f / dy : -1.0f / dy) : 1e30f;
    float tdz = dz != 0.0f ? (dz > 0.0f ? 1.0f / dz : -1.0f / dz) : 1e30f;
    float tmx = dx != 0.0f ? (dx > 0.0f ? ((float)(ix + 1) - ox) : (ox - (float)ix)) * tdx : 1e30f;
    float tmy = dy != 0.0f ? (dy > 0.0f ? ((float)(iy + 1) - oy) : (oy - (float)iy)) * tdy : 1e30f;
    float tmz = dz != 0.0f ? (dz > 0.0f ? ((float)(iz + 1) - oz) : (oz - (float)iz)) * tdz : 1e30f;

    for (int i = 0; i < 64; i++) {
        int axis; float t;
        if (tmx < tmy) {
            if (tmx < tmz) { t = tmx; ix += sx;  tmx += tdx; axis = 0; }
            else           { t = tmz; iz += sz;  tmz += tdz; axis = 2; }
        } else {
            if (tmy < tmz) { t = tmy; iy += ssy; tmy += tdy; axis = 1; }
            else           { t = tmz; iz += sz;  tmz += tdz; axis = 2; }
        }
        if (t > reach) return 0;
        if (iy < 0 || iy >= WY) return 0;

        uint8_t b = g_world[WIDX(ix, iy, iz)];
        if (!blk_solid[b]) continue;

        out->bx = ix; out->by = iy; out->bz = iz;
        out->nx = axis == 0 ? -sx : 0;
        out->ny = axis == 1 ? -ssy : 0;
        out->nz = axis == 2 ? -sz : 0;
        out->ok = 1;
        return 1;
    }
    return 0;
}
