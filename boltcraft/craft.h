#pragma once
/* boltcraft -- shared definitions.
 *
 * The world is a flat byte array, 256x256x64, wrapped horizontally so there is
 * no edge to walk off and no chunk loader to write. 4 MiB, allocated once and
 * never resized. Everything else in here is either a block property table or a
 * small helper the inner render loop wants inlined.
 */
#include "ulibc.h"
#include "string.h"
#include "math.h"

/* Powers of two, because the horizontal wrap is an AND and the index is three
 * shifts. Changing these to non-powers breaks WIDX and the noise lattice. */
#define WX   256
#define WZ   256
#define WY   64
#define WXM  (WX - 1)
#define WZM  (WZ - 1)

#define WATER_Y   26        /* sea level; terrain is generated around it */

#define WIDX(x, y, z) ( ((unsigned)(y) << 16) \
                      | (((unsigned)(z) & WZM) << 8) \
                      | ((unsigned)(x) & WXM) )

enum {
    B_AIR = 0, B_GRASS, B_DIRT, B_STONE, B_COBBLE, B_SAND,
    B_LOG, B_LEAVES, B_PLANK, B_GLASS, B_BRICK, B_ORE,
    B_SNOW, B_WATER, B_BEDROCK, B_COUNT
};

/* face kinds -- a block has three textures, not six. Nothing here needs to
 * tell north from south. */
#define F_TOP  0
#define F_SIDE 1
#define F_BOT  2

#define TEXW 16

extern uint8_t  *g_world;
extern uint8_t   g_top[WX * WZ];   /* highest non-air y in the column, +1 */
extern int       g_ymax;           /* highest non-air y anywhere; sky rays use it */
extern uint32_t  g_seed;

/* Textures are ARGB: the alpha byte is opacity, and only blocks with
 * blk_seethru set are ever asked for it. Leaves have real holes in them and
 * glass is mostly hole, which is the whole reason the renderer composites
 * instead of stopping at the first hit. */
extern uint32_t g_tex[B_COUNT][3][TEXW * TEXW];

extern const char * const blk_name[B_COUNT];
extern const uint8_t blk_opaque[B_COUNT];   /* stops a ray dead */
extern const uint8_t blk_solid[B_COUNT];    /* stops the player */
extern const uint8_t blk_seethru[B_COUNT];  /* texture alpha is meaningful */

static inline uint8_t blk_at(int x, int y, int z) {
    if ((unsigned)y >= WY) return B_AIR;
    return g_world[WIDX(x, y, z)];
}

static inline int ifloor(float v) {
    int i = (int)v;
    return (v < 0.0f && (float)i != v) ? i - 1 : i;
}

/* clang will happily call out to libm's sqrtf to get errno right on a negative
 * input, and this is in the per-pixel path. One instruction is the point. */
static inline float fsqrt(float x) {
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

static inline float fclampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The mixer everything shades with. `t` is 0..256 in eighths of a percent --
 * integer blending all the way down, since the colours never leave 8 bits. */
static inline uint32_t mix32(uint32_t a, uint32_t b, int t) {
    int it = 256 - t;
    uint32_t rb = ((((a & 0x00FF00FFu) * it) + ((b & 0x00FF00FFu) * t)) >> 8) & 0x00FF00FFu;
    uint32_t g  = ((((a & 0x0000FF00u) * it) + ((b & 0x0000FF00u) * t)) >> 8) & 0x0000FF00u;
    return rb | g;
}

static inline uint32_t rgb_pack(int r, int g, int b) {
    return ((uint32_t)clampi(r, 0, 255) << 16)
         | ((uint32_t)clampi(g, 0, 255) << 8)
         |  (uint32_t)clampi(b, 0, 255);
}

static inline uint32_t scale32(uint32_t c, int s) {      /* s is 0..256 */
    uint32_t rb = (((c & 0x00FF00FFu) * s) >> 8) & 0x00FF00FFu;
    uint32_t g  = (((c & 0x0000FF00u) * s) >> 8) & 0x0000FF00u;
    return rb | g;
}

/* libc's memset is a byte loop (libc/string.c). Clearing the four-megabyte
 * world with it is four million single-byte stores, which is a second and a
 * half of a new world on real hardware and most of half a minute inside an
 * emulator. `rep stos` is one instruction and QEMU has a fast path for it. */
static inline void fill_bytes(void *dst, uint8_t v, unsigned long n) {
    uint64_t w = 0x0101010101010101ull * v;
    unsigned long q = n >> 3;
    uint8_t *tail = (uint8_t *)dst + (q << 3);
    __asm__ volatile("rep stosq" : "+D"(dst), "+c"(q) : "a"(w) : "memory");
    for (unsigned long i = 0; i < (n & 7); i++) tail[i] = v;
}

/* one hash to rule the lot: terrain, textures, tree placement, ore veins */
static inline uint32_t hash3(int x, int y, int z, uint32_t salt) {
    uint32_t h = (uint32_t)x * 0x27D4EB2Du
               ^ (uint32_t)y * 0x165667B1u
               ^ (uint32_t)z * 0x9E3779B1u ^ salt;
    h ^= h >> 15; h *= 0x2C1B3C6Du;
    h ^= h >> 12; h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

struct camera {
    float x, y, z;        /* eye position, not feet */
    float yaw, pitch;
};

struct hit {
    int  bx, by, bz;      /* the block the crosshair is on */
    int  nx, ny, nz;      /* face normal, pointing back at the player */
    int  ok;
};

/* world.c */
void  world_alloc(void);
void  world_generate(uint32_t seed);
void  world_set(int x, int y, int z, uint8_t b);
void  world_retop(int x, int z);
int   world_spawn(float *px, float *py, float *pz);
int   world_save(const char *path, const struct camera *cam);
int   world_load(const char *path, struct camera *cam);

/* tex.c */
void  tex_build(void);

/* render.c */
void     render_init(uint32_t *rgb_buf, int fbw, int fbh);
void     render_set_scale(int s);
int      render_get_scale(void);
void     render_set_dist(int d);
int      render_get_dist(void);
void     render_world(const struct camera *cam, float daylight, float phase,
                      const struct hit *sel);
void     render_blit(uint32_t *fb, int fbw, int fbh);
int      ray_pick(const struct camera *cam, float reach, struct hit *out);

/* hud.c -- drawn at panel resolution, after the upscale, so the text stays
 * crisp no matter how coarse the 3D is */
void  hud_rect(uint32_t *fb, int fbw, int fbh, int x, int y, int w, int h, uint32_t c);
void  hud_text(uint32_t *fb, int fbw, int fbh, int x, int y, const char *s,
               uint32_t c, int scale);
void  hud_text_shadow(uint32_t *fb, int fbw, int fbh, int x, int y, const char *s,
                      uint32_t c, int scale);
void  hud_block_icon(uint32_t *fb, int fbw, int fbh, int x, int y, int side, uint8_t b);
