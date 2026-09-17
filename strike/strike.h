#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltStrike -- shared definitions.
 *
 *  A round-based defusal shooter. The world is a 48x48 grid of cells with one
 *  wall height, which is why the renderer can be a per-column raycast instead
 *  of the per-pixel one boltcraft needs: with uniform walls a whole screen
 *  column is one texture strip, so the cost is 320 rays a frame and not
 *  64,000. That is the entire reason this runs at a playable rate under an
 *  emulator with no acceleration.
 *
 *  Nothing here is Valve's. The map is original geometry in a familiar shape,
 *  every texture and sprite is generated at startup (see st_tex.c), and every
 *  sound is synthesised (st_sfx.c). The binary carries no art at all.
 * ===========================================================================*/

#define ST_W 48
#define ST_H 48

/* Cell contents. Anything but C_OPEN stops a bullet and a player. */
enum {
    C_OPEN = 0,
    C_SAND,        /* sandstone block -- the outdoor walls        */
    C_BRICK,       /* the bombsites                               */
    C_PLASTER,     /* spawn buildings                             */
    C_CRATE,       /* wooden crate; cover, not architecture       */
    C_DOOR,        /* the posts of the mid doorway                */
    C_COUNT
};

/* Floor kinds, used only for the colour under your feet. */
enum { F_DIRT = 0, F_SAND, F_CONCRETE, F_SITE_A, F_SITE_B, F_SPAWN_T, F_SPAWN_CT, F_COUNT };

/* Which named area a cell belongs to. Bots navigate by these. */
enum { Z_NONE = 0, Z_SPAWN_T, Z_SPAWN_CT, Z_SITE_A, Z_SITE_B };

#define TEXW 64                    /* wall textures are TEXW square           */

extern uint8_t  st_cell[ST_H][ST_W];    /* C_*  -- 0 is walkable              */
extern uint8_t  st_floor[ST_H][ST_W];   /* F_*                                */
extern uint8_t  st_zone[ST_H][ST_W];    /* Z_*                                */

void st_map_load(void);
int  st_solid(int cx, int cy);
int  st_solid_at(float x, float y);
/* True if nothing solid lies on the segment between the two points. Steps the
 * same DDA the renderer uses, so what a bot can shoot is what you can see. */
int  st_los(float x0, float y0, float x1, float y1);
/* Breadth-first flow field towards one cell, so a bot can walk anywhere from
 * anywhere by reading one byte. 2304 cells, rebuilt only when the goal moves. */
void st_flow_build(int gx, int gy, uint8_t *out);
/* Next step from (cx,cy) along a field; returns 0 if there is no route. */
int  st_flow_step(const uint8_t *field, int cx, int cy, int *nx, int *ny);
void st_zone_centre(int zone, float *x, float *y);

/* ---- textures and sprites (st_tex.c) ----------------------------------- *
 *  Wall textures are ARGB with no alpha; sprites use alpha 0 for "not drawn",
 *  which is most of a sprite's box. All generated once, on the heap, because
 *  eight 64x64 walls plus the sprite sheet is about 400 KiB and none of it
 *  belongs in the kernel image. */
extern uint32_t *st_wall_tex[C_COUNT];     /* [C_*] -> TEXW*TEXW, or 0        */

#define SPR_W 40
#define SPR_H 56
/* Five drawn views (front, front-quarter, side, back-quarter, back), mirrored
 * to make eight facings; two teams; two walk frames plus a death frame. */
enum { SPR_VIEWS = 5, SPR_FRAMES = 3 };
extern uint32_t *st_ply_spr[2][SPR_VIEWS][SPR_FRAMES];

#define ITEM_W 24
#define ITEM_H 24
enum { IT_BOMB = 0, IT_WEAPON, IT_COUNT };
extern uint32_t *st_item_spr[IT_COUNT];

/* The gun in your hands, drawn over everything else. One image per weapon. */
#define VIEW_W 160
#define VIEW_H 100
extern uint32_t *st_view_spr[16];

int  st_tex_init(void);            /* 0 on success, -1 if the heap said no */
void st_tex_free(void);

/* ---- weapons ------------------------------------------------------------ */
enum {
    W_KNIFE = 0, W_GK9, W_M45, W_DE50, W_MP9, W_SG12, W_AR47, W_AR15C, W_BR338,
    W_COUNT
};
enum { WK_MELEE = 0, WK_PISTOL, WK_SMG, WK_SHOTGUN, WK_RIFLE, WK_SNIPER };

typedef struct {
    const char *name;
    const char *nick;
    int   price;
    int   kind;
    int   dmg;            /* per pellet, at the muzzle                        */
    int   pellets;
    int   rpm;            /* rounds per minute                                */
    int   magsz;
    int   reserve;
    int   full_auto;
    int   armor_pen;      /* percent of damage that still lands through kevlar*/
    int   reload_ms;
    float spread;         /* radians, standing still                          */
    float recoil;
    float falloff;        /* damage multiplier lost per cell of distance      */
    int   kill_award;
} st_weapon;

extern const st_weapon st_weap[W_COUNT];

/* ---- entities ----------------------------------------------------------- */
#define ST_MAX_ENT 12
#define BOMB_FUSE_MS   40000
#define PLANT_MS        3000
#define DEFUSE_MS       9500
#define DEFUSE_KIT_MS   4800
#define ROUND_MS      115000
#define FREEZE_MS       7000

enum { T_SIDE = 0, CT_SIDE = 1 };

typedef struct {
    int   used, alive, bot;
    int   team;
    float x, y, ang;
    float vx, vy;
    int   hp, armor, helmet;
    int   money;
    int   wep;                     /* W_* currently held                      */
    int   has[W_COUNT];
    int   mag[W_COUNT], res[W_COUNT];
    int   fire_cd, reload_cd, flash_ms;
    int   has_bomb, has_kit;
    int   kills, deaths;
    float recoil, bob;
    int   step_acc;
    int   skill;                   /* 0..3; aim error and reaction time       */
    int   bstate, bcool, btarget, bsee;
    int   bgx, bgy;                /* the cell the bot is walking towards     */
    uint8_t *field;                /* its flow field, owned                   */
    int   noise_ms;                /* time since it last heard a shot         */
    float nx, ny;                  /* where that noise was                    */
    char  name[16];
} st_ent;

/* ---- round state -------------------------------------------------------- */
enum { R_FREEZE = 0, R_LIVE, R_PLANTED, R_OVER };

typedef struct {
    st_ent ent[ST_MAX_ENT];
    int    nent;
    int    phase, phase_ms;
    int    round, score_t, score_ct;
    int    bomb_down, bomb_ms;
    float  bomb_x, bomb_y;
    int    bomb_site;              /* Z_SITE_A / Z_SITE_B                     */
    int    defusing, defuse_ms, defuser;
    int    planting, plant_ms;
    int    winner;                 /* T_SIDE / CT_SIDE, valid in R_OVER       */
    int    use_held;               /* slot 0 is holding the use key           */
    const char *win_why;
    uint32_t seed;
} st_world;

extern st_world st_w;

void st_game_init(int bots_per_side, int skill);
void st_game_free(void);
void st_round_start(void);
void st_step(int dt_ms);                  /* one tick of everything but you   */
st_ent *st_you(void);

/* Fire the given entity's weapon; returns 1 if a shot left the barrel. */
int  st_fire(st_ent *e);
void st_reload(st_ent *e);
void st_buy(st_ent *e, int wep);
int  st_can_buy(st_ent *e);
void st_switch(st_ent *e, int wep);
void st_buy_armor(st_ent *e, int helmet);
/* Move, sliding along anything solid. Shared, because the bots and you have
 * to catch on exactly the same corners or the map is two different maps. */
void st_move(st_ent *e, float dx, float dy);
/* Damage with armour, distance falloff and a head multiplier already applied. */
void st_hurt(st_ent *victim, st_ent *by, int dmg, int headshot);

/* ---- tracers and blood, drawn as short-lived sprites --------------------- */
#define ST_MAX_FX 48
typedef struct { int ms; float x, y; int kind; } st_fx;
extern st_fx st_fx_list[ST_MAX_FX];
void st_fx_add(float x, float y, int kind);
enum { FX_BLOOD = 0, FX_SPARK, FX_SMOKE };

/* ---- rendering (st_render.c) -------------------------------------------- */
#define RW 320
#define RH 200
extern uint32_t st_frame[RW * RH];
void st_render(const st_ent *eye, float pitch, int zoomed);

/* ---- sound (st_sfx.c) --------------------------------------------------- */
enum { S_SHOT_PISTOL = 0, S_SHOT_RIFLE, S_SHOT_SNIPER, S_SHOT_SHOTGUN, S_KNIFE,
       S_RELOAD, S_STEP, S_HIT, S_DIE, S_BEEP, S_PLANT, S_DEFUSE, S_WIN, S_LOSE,
       S_BUY, S_EMPTY, S_COUNT };
int  st_sfx_init(void);
void st_sfx_free(void);
/* vol 0..255, pan from the listener's point of view (-128 left .. 127 right) */
void st_sfx_play(int id, int vol, int pan);
void st_sfx_pump(void);            /* push the next buffer if the codec drained */
void st_sfx_silence(void);
