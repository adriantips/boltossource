/* ===========================================================================
 *  BoltStrike -- rounds, money, guns, damage, and the bots.
 *
 *  Everything here is deliberately arithmetic rather than physics. A round is
 *  a small state machine, a shot is a ray and a list of circles, and a bot is
 *  four states and a flow field. That is not a shortcut taken under protest:
 *  the whole game has to fit in the spare time of a desktop compositor loop
 *  running under an emulator, and none of it can afford to be clever.
 * ===========================================================================*/
#include <stdint.h>
#include "strike.h"
#include "kheap.h"
#include "math.h"
#include "string.h"

st_world st_w;
st_fx    st_fx_list[ST_MAX_FX];

#define PI_F 3.14159265f
#define PLAYER_R 0.34f          /* how fat you are, for walls and for bullets */

/* name                nick         price kind         dmg pel rpm  mag  res auto pen reload  spread recoil falloff award */
const st_weapon st_weap[W_COUNT] = {
 { "Knife",  "blade",        0, WK_MELEE,    55, 1,  180,  1,   1, 0, 100,  600, 0.000f, 0.00f, 0.000f, 1500 },
 { "GK-9",   "sidearm",    200, WK_PISTOL,   26, 1,  400, 20,  80, 1,  55, 1500, 0.012f, 0.30f, 0.020f,  300 },
 { "M-45",   "sidearm",    200, WK_PISTOL,   32, 1,  340, 12,  72, 0,  60, 1600, 0.010f, 0.34f, 0.018f,  300 },
 { "DE-50",  "hand cannon",700, WK_PISTOL,   58, 1,  240,  7,  35, 0,  85, 2100, 0.015f, 0.62f, 0.014f,  300 },
 { "MP-9",   "SMG",       1250, WK_SMG,      26, 1,  850, 30, 120, 1,  62, 2100, 0.020f, 0.22f, 0.030f,  600 },
 { "SG-12",  "shotgun",   1700, WK_SHOTGUN,  17, 8,  100,  8,  32, 0,  70, 3200, 0.075f, 0.80f, 0.090f,  900 },
 { "AR-47",  "rifle",     2700, WK_RIFLE,    36, 1,  600, 30,  90, 1,  78, 2450, 0.016f, 0.45f, 0.012f,  300 },
 { "AR-15C", "carbine",   3100, WK_RIFLE,    33, 1,  666, 30,  90, 1,  72, 3100, 0.014f, 0.38f, 0.012f,  300 },
 { "BR-338", "bolt rifle",4750, WK_SNIPER,  115, 1,   41, 10,  30, 0,  97, 3700, 0.004f, 1.20f, 0.004f,  100 },
};

/* Bot names. Short, so they fit the scoreboard, and forgettable on purpose --
 * these are opponents, not characters. */
static const char *const botnames[] = {
    "Vex", "Ramp", "Kilo", "Doss", "Nine", "Halt", "Prow", "Ash",
    "Mox", "Crate", "Sarge", "Pike", "Juno", "Rook", "Bly", "Tess"
};

/* ---- a private prng, so the round does not disturb anyone else's ---------- */
static uint32_t rnd(void) {
    st_w.seed = st_w.seed * 1664525u + 1013904223u;
    return st_w.seed >> 8;
}
static float frnd(void) { return (float)(rnd() & 0xFFFF) / 65535.0f; }
static float srnd(void) { return frnd() * 2.0f - 1.0f; }

st_ent *st_you(void) { return &st_w.ent[0]; }

/* -------------------------------------------------------------------------- */
void st_fx_add(float x, float y, int kind) {
    for (int i = 0; i < ST_MAX_FX; i++) {
        if (st_fx_list[i].ms > 0) continue;
        st_fx_list[i].ms = 700;
        st_fx_list[i].x = x; st_fx_list[i].y = y;
        st_fx_list[i].kind = kind;
        return;
    }
}

static int alive_count(int team) {
    int n = 0;
    for (int i = 0; i < st_w.nent; i++)
        if (st_w.ent[i].used && st_w.ent[i].alive && st_w.ent[i].team == team) n++;
    return n;
}

/* Put an entity somewhere walkable inside its spawn, spread out a bit so ten
 * of them do not start inside each other. */
static void place_spawn(st_ent *e, int idx) {
    int zone = e->team == T_SIDE ? Z_SPAWN_T : Z_SPAWN_CT;
    static int cells[192][2];
    int n = 0;
    for (int y = 0; y < ST_H && n < 192; y++)
        for (int x = 0; x < ST_W && n < 192; x++)
            if (st_zone[y][x] == zone && !st_solid(x, y)) { cells[n][0] = x; cells[n][1] = y; n++; }
    if (!n) { e->x = ST_W / 2.0f; e->y = ST_H / 2.0f; return; }
    int k = (idx * 7 + (int)(rnd() % 5)) % n;
    e->x = (float)cells[k][0] + 0.5f;
    e->y = (float)cells[k][1] + 0.5f;
    /* face the middle of the map, which is the direction anyone would walk */
    e->ang = atan2f(ST_H / 2.0f - e->y, ST_W / 2.0f - e->x);
}

void st_switch(st_ent *e, int wep) {
    if (wep < 0 || wep >= W_COUNT || !e->has[wep]) return;
    e->wep = wep;
    e->reload_cd = 0;
}

/* The best gun this entity owns, for when the one in its hands runs dry. */
static int best_gun(const st_ent *e) {
    static const int order[] = { W_BR338, W_AR15C, W_AR47, W_SG12, W_MP9, W_DE50, W_M45, W_GK9, W_KNIFE };
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        int w = order[i];
        if (e->has[w] && (e->mag[w] > 0 || e->res[w] > 0 || w == W_KNIFE)) return w;
    }
    return W_KNIFE;
}

static void give(st_ent *e, int wep) {
    e->has[wep] = 1;
    e->mag[wep] = st_weap[wep].magsz;
    e->res[wep] = st_weap[wep].reserve;
}

int st_can_buy(st_ent *e) {
    if (!e->alive) return 0;
    if (st_w.phase != R_FREEZE) return 0;
    int cx = (int)e->x, cy = (int)e->y;
    if ((unsigned)cx >= ST_W || (unsigned)cy >= ST_H) return 0;
    return st_zone[cy][cx] == (e->team == T_SIDE ? Z_SPAWN_T : Z_SPAWN_CT);
}

void st_buy(st_ent *e, int wep) {
    if (wep < 0 || wep >= W_COUNT) return;
    if (!st_can_buy(e)) return;
    const st_weapon *w = &st_weap[wep];
    if (w->price <= 0 || e->money < w->price) return;
    /* A primary replaces a primary; pistols stack alongside it. */
    if (w->kind >= WK_SMG)
        for (int i = 0; i < W_COUNT; i++)
            if (st_weap[i].kind >= WK_SMG) e->has[i] = 0;
    e->money -= w->price;
    give(e, wep);
    st_switch(e, wep);
}

/* Armour is its own purchase and does not live in the weapon table. */
void st_buy_armor(st_ent *e, int helmet) {
    int price = helmet ? 1000 : 650;
    if (!st_can_buy(e) || e->money < price) return;
    if (e->armor >= 100 && (e->helmet || !helmet)) return;
    e->money -= price;
    e->armor = 100;
    if (helmet) e->helmet = 1;
}

void st_reload(st_ent *e) {
    const st_weapon *w = &st_weap[e->wep];
    if (w->magsz <= 1) return;
    if (e->mag[e->wep] >= w->magsz || e->res[e->wep] <= 0) return;
    if (e->reload_cd > 0) return;
    e->reload_cd = w->reload_ms;
}

/* ---- damage -------------------------------------------------------------- */
void st_hurt(st_ent *v, st_ent *by, int dmg, int head) {
    if (!v->alive) return;
    if (head) dmg = v->helmet ? (dmg * 175) / 100 : dmg * 4;

    if (v->armor > 0) {
        int pen = by ? st_weap[by->wep].armor_pen : 100;
        int through = (dmg * (50 + pen / 2)) / 100;
        v->armor -= (dmg - through) / 2;
        if (v->armor < 0) v->armor = 0;
        dmg = through;
    }

    v->hp -= dmg;
    st_fx_add(v->x, v->y, FX_BLOOD);
    if (v->hp > 0) return;

    v->alive = 0;
    v->hp = 0;
    v->deaths++;
    /* Whatever it was carrying is gone with it, except the bomb, which drops
     * where it fell -- otherwise killing the carrier ends the round by
     * accident and nobody can work out why. */
    if (v->has_bomb) {
        v->has_bomb = 0;
        if (!st_w.bomb_down) { st_w.bomb_x = v->x; st_w.bomb_y = v->y; }
    }
    if (by && by != v) {
        by->kills++;
        by->money += st_weap[by->wep].kill_award;
        if (by->money > 16000) by->money = 16000;
    }
}

/* ---- shooting ------------------------------------------------------------ *
 *  Hitscan against circles. For each pellet we walk the entity list, drop
 *  anything behind us or out of range, and keep the nearest whose centre is
 *  within its own radius of the ray. Walls are checked once, at the end,
 *  against the winner -- checking every candidate would be the same answer for
 *  more work. */
static int shoot_ray(st_ent *e, float ang, int dmg, float falloff) {
    float rx = cosf(ang), ry = sinf(ang);
    st_ent *best = 0;
    float bestd = 1e30f, bestoff = 0.0f;

    for (int i = 0; i < st_w.nent; i++) {
        st_ent *t = &st_w.ent[i];
        if (!t->used || !t->alive || t == e) continue;
        if (t->team == e->team) continue;
        float dx = t->x - e->x, dy = t->y - e->y;
        float along = dx * rx + dy * ry;
        if (along <= 0.2f || along > 48.0f) continue;
        float off = fabsf(dx * ry - dy * rx);      /* perpendicular distance   */
        if (off > PLAYER_R) continue;
        if (along < bestd) { bestd = along; best = t; bestoff = off; }
    }

    if (!best) {
        /* Nothing hit: put a spark where the wall is, so a miss is legible. */
        float d = 0.5f;
        while (d < 40.0f && !st_solid_at(e->x + rx * d, e->y + ry * d)) d += 0.25f;
        if (d < 40.0f) st_fx_add(e->x + rx * (d - 0.2f), e->y + ry * (d - 0.2f), FX_SPARK);
        return 0;
    }
    if (!st_los(e->x, e->y, best->x, best->y)) return 0;

    int d = dmg - (int)(bestd * falloff * (float)dmg);
    if (d < 4) d = 4;
    /* A shot through the middle of the silhouette is a head shot. There is no
     * vertical axis in this game, so this is the only honest way to have them:
     * reward precision instead of pretending to model a neck. */
    int head = bestoff < PLAYER_R * 0.22f;
    st_hurt(best, e, d, head);
    return 1;
}

/* The knife reaches about a body-width and a half and does not care about
 * walls at that range, because at that range there are not any. */
static void knife_swing(st_ent *e) {
    float rx = cosf(e->ang), ry = sinf(e->ang);
    for (int i = 0; i < st_w.nent; i++) {
        st_ent *t = &st_w.ent[i];
        if (!t->used || !t->alive || t == e || t->team == e->team) continue;
        float dx = t->x - e->x, dy = t->y - e->y;
        float along = dx * rx + dy * ry;
        if (along <= 0.0f || along > 1.6f) continue;
        if (fabsf(dx * ry - dy * rx) > 0.7f) continue;
        st_hurt(t, e, st_weap[W_KNIFE].dmg, 0);
        return;
    }
}

int st_fire(st_ent *e) {
    if (!e->alive || st_w.phase == R_FREEZE) return 0;
    if (e->fire_cd > 0 || e->reload_cd > 0) return 0;

    const st_weapon *w = &st_weap[e->wep];
    if (w->kind == WK_MELEE) {
        e->fire_cd = 60000 / w->rpm;
        e->flash_ms = 0;
        knife_swing(e);
        return 1;
    }
    if (e->mag[e->wep] <= 0) { st_reload(e); return 0; }

    e->mag[e->wep]--;
    e->fire_cd = 60000 / w->rpm;
    e->flash_ms = 60;

    /* Spread grows while you are moving, which is the one movement rule that
     * matters and the reason standing still is worth anything. */
    float speed = sqrtf(e->vx * e->vx + e->vy * e->vy);
    float spread = w->spread * (1.0f + speed * 5.0f) + e->recoil * 0.05f;

    for (int p = 0; p < w->pellets; p++)
        shoot_ray(e, e->ang + srnd() * spread, w->dmg, w->falloff);

    e->recoil += w->recoil;
    if (e->recoil > 3.0f) e->recoil = 3.0f;
    return 1;
}

/* ---- rounds -------------------------------------------------------------- */
static void reset_entity(st_ent *e, int idx) {
    e->alive = 1;
    e->hp = 100;
    e->vx = e->vy = 0.0f;
    e->fire_cd = e->reload_cd = e->flash_ms = 0;
    e->recoil = 0.0f;
    e->bstate = 0; e->bcool = 0; e->btarget = -1; e->bsee = 0;
    e->bgx = e->bgy = -1;
    e->has_bomb = 0;
    e->noise_ms = 0;
    place_spawn(e, idx);

    /* Reload everything it still owns; a round starts with full magazines. */
    for (int i = 0; i < W_COUNT; i++)
        if (e->has[i]) { e->mag[i] = st_weap[i].magsz; e->res[i] = st_weap[i].reserve; }
    if (!e->has[W_KNIFE]) give(e, W_KNIFE);
    if (!e->has[W_GK9] && !e->has[W_M45])
        give(e, e->team == T_SIDE ? W_GK9 : W_M45);
    st_switch(e, best_gun(e));
}

void st_round_start(void) {
    st_w.phase = R_FREEZE;
    st_w.phase_ms = FREEZE_MS;
    st_w.bomb_down = 0;
    st_w.bomb_ms = 0;
    st_w.planting = st_w.plant_ms = 0;
    st_w.defusing = st_w.defuse_ms = 0;
    st_w.defuser = -1;
    st_w.winner = -1;
    st_w.win_why = "";
    st_w.round++;
    st_w.bomb_site = (rnd() & 1) ? Z_SITE_A : Z_SITE_B;

    for (int i = 0; i < ST_MAX_FX; i++) st_fx_list[i].ms = 0;

    for (int i = 0; i < st_w.nent; i++)
        if (st_w.ent[i].used) reset_entity(&st_w.ent[i], i);

    /* One T gets the bomb. The human gets it when the human is a T, because
     * being handed the objective is more interesting than being told about it. */
    int carrier = -1;
    for (int i = 0; i < st_w.nent; i++)
        if (st_w.ent[i].used && st_w.ent[i].team == T_SIDE) { carrier = i; break; }
    if (st_w.ent[0].used && st_w.ent[0].team == T_SIDE) carrier = 0;
    if (carrier >= 0) st_w.ent[carrier].has_bomb = 1;

    /* One CT in three carries a kit. */
    for (int i = 0; i < st_w.nent; i++)
        if (st_w.ent[i].used && st_w.ent[i].team == CT_SIDE)
            st_w.ent[i].has_kit = (rnd() % 3) == 0;
}

static void end_round(int winner, const char *why) {
    if (st_w.phase == R_OVER) return;
    st_w.phase = R_OVER;
    st_w.phase_ms = 5000;
    st_w.winner = winner;
    st_w.win_why = why;
    if (winner == T_SIDE) st_w.score_t++; else st_w.score_ct++;

    for (int i = 0; i < st_w.nent; i++) {
        st_ent *e = &st_w.ent[i];
        if (!e->used) continue;
        e->money += (e->team == winner) ? 3250 : 1900;
        if (e->money > 16000) e->money = 16000;
    }
}

/* ---- bots ---------------------------------------------------------------- */
enum { B_ROAM = 0, B_FIGHT, B_PLANT, B_DEFUSE, B_HUNT };

/* Where does this bot want to be standing? */
static void bot_goal(st_ent *e, int *gx, int *gy) {
    float fx, fy;
    if (st_w.bomb_down) {
        *gx = (int)st_w.bomb_x; *gy = (int)st_w.bomb_y;
        return;
    }
    if (e->team == T_SIDE) {
        st_zone_centre(st_w.bomb_site, &fx, &fy);
    } else {
        /* Split the CTs between the two sites by index, so both get covered
         * without anybody coordinating anything. */
        int idx = (int)(e - st_w.ent);
        st_zone_centre((idx & 1) ? Z_SITE_A : Z_SITE_B, &fx, &fy);
    }
    *gx = (int)fx; *gy = (int)fy;
}

static st_ent *bot_spot(st_ent *e) {
    st_ent *best = 0;
    float bestd = 1e30f;
    for (int i = 0; i < st_w.nent; i++) {
        st_ent *t = &st_w.ent[i];
        if (!t->used || !t->alive || t->team == e->team) continue;
        float dx = t->x - e->x, dy = t->y - e->y;
        float d = dx * dx + dy * dy;
        if (d > 900.0f) continue;
        /* A cone, not a sphere -- a bot that shoots you from behind its own
         * head is the single fastest way to make a game feel cheap. */
        float to = atan2f(dy, dx) - e->ang;
        while (to >  PI_F) to -= 2.0f * PI_F;
        while (to < -PI_F) to += 2.0f * PI_F;
        if (fabsf(to) > 1.25f) continue;
        if (!st_los(e->x, e->y, t->x, t->y)) continue;
        if (d < bestd) { bestd = d; best = t; }
    }
    return best;
}

static void move_towards(st_ent *e, float tx, float ty, float speed, int dt);

static void bot_think(st_ent *e, int dt) {
    if (!e->alive) return;

    if (st_w.phase == R_FREEZE) {
        /* Buy, once, at the start of freeze. Bots buy the best thing they can
         * afford and always take armour first, because losing a round to a
         * bot with a rifle and no vest is not the game anybody wants. */
        if (e->bcool == 0) {
            e->bcool = 1;
            if (e->money >= 1650) st_buy_armor(e, 0);
            static const int wish[] = { W_BR338, W_AR15C, W_AR47, W_SG12, W_MP9, W_DE50 };
            for (unsigned i = 0; i < sizeof(wish) / sizeof(wish[0]); i++) {
                int w = wish[i];
                if (e->team == T_SIDE && w == W_AR15C) continue;   /* team flavour */
                if (e->team == CT_SIDE && w == W_AR47) continue;
                /* leave a little for the next round rather than going all in */
                if (e->money - st_weap[w].price >= 0 && (int)(rnd() % 100) < 78) {
                    st_buy(e, w);
                    break;
                }
            }
            if (e->money >= 1000 && !e->helmet) st_buy_armor(e, 1);
        }
        return;
    }

    e->bcool = 0;
    st_ent *tgt = bot_spot(e);

    if (tgt) {
        e->bstate = B_FIGHT;
        e->btarget = (int)(tgt - st_w.ent);
        e->nx = tgt->x; e->ny = tgt->y;
        e->noise_ms = 4000;
    } else if (e->bstate == B_FIGHT) {
        e->bstate = e->noise_ms > 0 ? B_HUNT : B_ROAM;
    }

    if (e->noise_ms > 0) e->noise_ms -= dt;

    switch (e->bstate) {
    case B_FIGHT: {
        st_ent *t = &st_w.ent[e->btarget];
        if (!t->alive) { e->bstate = B_ROAM; break; }

        float want = atan2f(t->y - e->y, t->x - e->x);
        float err = want - e->ang;
        while (err >  PI_F) err -= 2.0f * PI_F;
        while (err < -PI_F) err += 2.0f * PI_F;

        /* Turn rate and residual aim error are the whole difficulty setting. */
        float rate = (2.6f + e->skill * 2.2f) * (float)dt / 1000.0f;
        if (err >  rate) err =  rate;
        if (err < -rate) err = -rate;
        e->ang += err;

        float dx = t->x - e->x, dy = t->y - e->y;
        float dist = sqrtf(dx * dx + dy * dy);
        float aim = fabsf(want - e->ang);
        float tol = 0.05f + (3 - e->skill) * 0.035f + dist * 0.004f;

        if (aim < tol) {
            if (e->mag[e->wep] <= 0) st_reload(e);
            else if (st_weap[e->wep].kind != WK_MELEE || dist < 1.6f) st_fire(e);
        }
        /* Close if it is holding a knife, otherwise hold position and strafe. */
        if (st_weap[e->wep].kind == WK_MELEE || dist > 14.0f)
            move_towards(e, t->x, t->y, 2.4f, dt);
        else {
            float s = ((int)(e - st_w.ent) & 1) ? 1.0f : -1.0f;
            move_towards(e, e->x - dy * 0.3f * s, e->y + dx * 0.3f * s, 1.5f, dt);
        }
        break;
    }
    case B_HUNT:
        if (e->noise_ms <= 0) { e->bstate = B_ROAM; break; }
        move_towards(e, e->nx, e->ny, 2.6f, dt);
        break;
    default: {
        int gx, gy;
        bot_goal(e, &gx, &gy);

        /* Plant, if this is the carrier and it is standing on the site. */
        if (e->has_bomb && st_zone[(int)e->y][(int)e->x] == st_w.bomb_site && !st_w.bomb_down) {
            st_w.planting = (int)(e - st_w.ent) + 1;
            st_w.plant_ms += dt;
            e->vx = e->vy = 0.0f;
            break;
        }
        /* Defuse, if this is a CT standing on a live bomb. */
        if (e->team == CT_SIDE && st_w.bomb_down) {
            float dx = st_w.bomb_x - e->x, dy = st_w.bomb_y - e->y;
            if (dx * dx + dy * dy < 1.4f) {
                st_w.defusing = 1;
                st_w.defuser = (int)(e - st_w.ent);
                st_w.defuse_ms += dt;
                e->vx = e->vy = 0.0f;
                break;
            }
        }

        if (e->bgx != gx || e->bgy != gy || !e->field) {
            if (!e->field) e->field = kmalloc(ST_W * ST_H);
            if (e->field) { st_flow_build(gx, gy, e->field); e->bgx = gx; e->bgy = gy; }
        }
        if (e->field) {
            int nx, ny;
            if (st_flow_step(e->field, (int)e->x, (int)e->y, &nx, &ny))
                move_towards(e, (float)nx + 0.5f, (float)ny + 0.5f, 2.6f, dt);
        }
        break;
    }
    }
}

/* ---- movement ------------------------------------------------------------ *
 *  Slide along walls rather than stopping dead at them: try the full move,
 *  then each axis on its own. Three checks, and it is the difference between
 *  a corridor you can run down and one you keep catching on. */
static int try_move(st_ent *e, float nx, float ny) {
    if (st_solid_at(nx + PLAYER_R, ny + PLAYER_R) || st_solid_at(nx - PLAYER_R, ny + PLAYER_R) ||
        st_solid_at(nx + PLAYER_R, ny - PLAYER_R) || st_solid_at(nx - PLAYER_R, ny - PLAYER_R))
        return 0;
    e->x = nx; e->y = ny;
    return 1;
}

void st_move(st_ent *e, float dx, float dy) {
    if (try_move(e, e->x + dx, e->y + dy)) return;
    if (try_move(e, e->x + dx, e->y)) return;
    try_move(e, e->x, e->y + dy);
}

static void move_towards(st_ent *e, float tx, float ty, float speed, int dt) {
    float dx = tx - e->x, dy = ty - e->y;
    float d = sqrtf(dx * dx + dy * dy);
    if (d < 0.05f) { e->vx = e->vy = 0.0f; return; }
    float s = speed * (float)dt / 1000.0f;
    if (s > d) s = d;
    e->vx = dx / d * speed;
    e->vy = dy / d * speed;
    st_move(e, dx / d * s, dy / d * s);
    e->step_acc += dt;
    /* Face where you are going, unless you are busy facing something else. */
    if (e->bstate != B_FIGHT) {
        float want = atan2f(dy, dx);
        float err = want - e->ang;
        while (err >  PI_F) err -= 2.0f * PI_F;
        while (err < -PI_F) err += 2.0f * PI_F;
        float rate = 6.0f * (float)dt / 1000.0f;
        if (err >  rate) err =  rate;
        if (err < -rate) err = -rate;
        e->ang += err;
    }
}

/* What slot 0 is doing with the use key, which app_strike.c sets each frame.
 * It is a held state and not an event: a plant is nine tenths of a second of
 * deciding not to let go. */
static void human_use(int dt) {
    st_ent *e = st_you();
    if (!e->used || !e->alive || !st_w.use_held) return;
    int cx = (int)e->x, cy = (int)e->y;
    if ((unsigned)cx >= ST_W || (unsigned)cy >= ST_H) return;

    if (e->team == T_SIDE && e->has_bomb && !st_w.bomb_down &&
        st_zone[cy][cx] == st_w.bomb_site) {
        st_w.planting = 1;
        st_w.plant_ms += dt;
        return;
    }
    if (e->team == CT_SIDE && st_w.bomb_down) {
        float dx = st_w.bomb_x - e->x, dy = st_w.bomb_y - e->y;
        if (dx * dx + dy * dy < 1.4f) {
            st_w.defusing = 1;
            st_w.defuser = 0;
            st_w.defuse_ms += dt;
        }
    }
}

/* -------------------------------------------------------------------------- */
void st_step(int dt_ms) {
    if (dt_ms <= 0) return;
    if (dt_ms > 120) dt_ms = 120;         /* a long stall is not a long frame  */

    for (int i = 0; i < ST_MAX_FX; i++)
        if (st_fx_list[i].ms > 0) st_fx_list[i].ms -= dt_ms;

    /* Cooldowns for everybody, human included. */
    for (int i = 0; i < st_w.nent; i++) {
        st_ent *e = &st_w.ent[i];
        if (!e->used) continue;
        if (e->fire_cd > 0)  e->fire_cd -= dt_ms;
        if (e->flash_ms > 0) e->flash_ms -= dt_ms;
        if (e->reload_cd > 0) {
            e->reload_cd -= dt_ms;
            if (e->reload_cd <= 0) {
                const st_weapon *w = &st_weap[e->wep];
                int need = w->magsz - e->mag[e->wep];
                if (need > e->res[e->wep]) need = e->res[e->wep];
                e->mag[e->wep] += need;
                e->res[e->wep] -= need;
            }
        }
        e->recoil -= (float)dt_ms / 260.0f;
        if (e->recoil < 0.0f) e->recoil = 0.0f;
        if (e->alive && e->has[e->wep] == 0) st_switch(e, best_gun(e));
    }

    st_w.planting = 0;
    st_w.defusing = 0;

    /* You first, so a bot walking onto the same bomb cannot take the
     * defuse out from under you in the frame you started it. */
    human_use(dt_ms);

    for (int i = 0; i < st_w.nent; i++)
        if (st_w.ent[i].used && st_w.ent[i].bot) bot_think(&st_w.ent[i], dt_ms);

    /* The human's plant/defuse progress is driven from app_strike.c, which
     * sets planting/defusing before calling in; the bots set theirs above.
     * Either way the timers are settled here, in one place. */
    if (!st_w.planting) st_w.plant_ms = 0;
    if (!st_w.defusing) { st_w.defuse_ms = 0; st_w.defuser = -1; }

    switch (st_w.phase) {
    case R_FREEZE:
        st_w.phase_ms -= dt_ms;
        if (st_w.phase_ms <= 0) { st_w.phase = R_LIVE; st_w.phase_ms = ROUND_MS; }
        break;

    case R_LIVE:
        st_w.phase_ms -= dt_ms;
        if (st_w.plant_ms >= PLANT_MS && st_w.planting) {
            st_ent *p = &st_w.ent[st_w.planting - 1];
            p->has_bomb = 0;
            p->money += 800;
            st_w.bomb_down = 1;
            st_w.bomb_x = p->x; st_w.bomb_y = p->y;
            st_w.bomb_ms = BOMB_FUSE_MS;
            st_w.phase = R_PLANTED;
            st_w.plant_ms = 0;
            break;
        }
        if (!alive_count(T_SIDE))  { end_round(CT_SIDE, "Terrorists eliminated"); break; }
        if (!alive_count(CT_SIDE)) { end_round(T_SIDE,  "Counter-Terrorists eliminated"); break; }
        if (st_w.phase_ms <= 0)    { end_round(CT_SIDE, "Time expired"); }
        break;

    case R_PLANTED: {
        st_w.bomb_ms -= dt_ms;
        int kit = st_w.defuser >= 0 && st_w.ent[st_w.defuser].has_kit;
        if (st_w.defusing && st_w.defuse_ms >= (kit ? DEFUSE_KIT_MS : DEFUSE_MS)) {
            if (st_w.defuser >= 0) st_w.ent[st_w.defuser].money += 300;
            end_round(CT_SIDE, "Bomb defused");
            break;
        }
        if (st_w.bomb_ms <= 0) {
            st_fx_add(st_w.bomb_x, st_w.bomb_y, FX_SMOKE);
            end_round(T_SIDE, "Bomb detonated");
            break;
        }
        /* With every CT dead the bomb is going off; call it now rather than
         * making everyone watch forty seconds of nothing. */
        if (!alive_count(CT_SIDE)) end_round(T_SIDE, "Counter-Terrorists eliminated");
        break;
    }

    case R_OVER:
        st_w.phase_ms -= dt_ms;
        if (st_w.phase_ms <= 0) st_round_start();
        break;
    }
}

/* -------------------------------------------------------------------------- */
void st_game_init(int bots_per_side, int skill) {
    st_game_free();
    memset(&st_w, 0, sizeof(st_w));

    st_w.seed = 0x5C0FFEE1u;
    st_map_load();

    if (bots_per_side < 1) bots_per_side = 1;
    if (bots_per_side > 5) bots_per_side = 5;

    int n = 0;
    /* Slot 0 is always you, and always a CT -- defending is the friendlier
     * side to learn a map from, because the map comes to you. */
    st_ent *you = &st_w.ent[n++];
    you->used = 1; you->bot = 0; you->team = CT_SIDE; you->money = 800;
    strlcpy(you->name, "You", sizeof(you->name));

    for (int t = 0; t < 2; t++) {
        int want = bots_per_side - (t == CT_SIDE ? 1 : 0);
        for (int i = 0; i < want && n < ST_MAX_ENT; i++) {
            st_ent *e = &st_w.ent[n];
            e->used = 1; e->bot = 1; e->team = t; e->money = 800;
            e->skill = skill;
            strlcpy(e->name, botnames[(n * 3 + t) % (int)(sizeof(botnames) / sizeof(botnames[0]))],
                     sizeof(e->name));
            n++;
        }
    }
    st_w.nent = n;

    st_w.round = 0;
    st_round_start();
}

void st_game_free(void) {
    for (int i = 0; i < ST_MAX_ENT; i++) {
        kfree(st_w.ent[i].field);
        st_w.ent[i].field = 0;
    }
}
