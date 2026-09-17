/* BoltStrike's map and rules, on the host, with no kernel underneath.
 *
 * Two things about a game like this cannot be checked by looking at it, and
 * both of them are the kind of bug you only meet after twenty minutes of
 * play: a round that never ends, and a bot that cannot get where it is going.
 * Neither needs a framebuffer to test, so neither is tested with one. The only
 * substitution is the heap.
 *
 * The real value here is the round loop at the bottom. It plays several
 * hundred rounds of six bots in a couple of seconds and asserts the things
 * that must be true of every one of them -- it ended, nobody finished inside
 * a wall, nobody went into debt -- which is a workload no human will sit
 * through and exactly the one that finds a stuck flow field. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <math.h>

#include "kheap.h"
void *kmalloc(uint64_t n) { return malloc((size_t)n); }
void  kfree(void *p) { free(p); }
/* The kernel has the BSD pair; mingw does not. */
size_t strlcpy(char *d, const char *s, size_t cap) {
    size_t n = strlen(s);
    if (cap) { size_t c = n < cap - 1 ? n : cap - 1; memcpy(d, s, c); d[c] = 0; }
    return n;
}

#include "../../strike/st_map.c"
#include "../../strike/st_game.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                       printf("\n"); fails++; } } while (0)

/* ---- the map ------------------------------------------------------------- */
static void map_is_connected(void) {
    st_map_load();

    int floor = 0;
    for (int y = 0; y < ST_H; y++)
        for (int x = 0; x < ST_W; x++)
            if (!st_solid(x, y)) floor++;
    CHECK(floor > 600, "only %d walkable cells; the map is mostly wall", floor);

    /* Flood from CT spawn and insist every walkable cell was reached. A map
     * with an island in it plays perfectly until the one round a bot spawns
     * on the island. */
    float cx, cy;
    st_zone_centre(Z_SPAWN_CT, &cx, &cy);
    uint8_t *field = malloc(ST_W * ST_H);
    st_flow_build((int)cx, (int)cy, field);

    int stranded = 0;
    for (int y = 0; y < ST_H; y++)
        for (int x = 0; x < ST_W; x++)
            if (!st_solid(x, y) && field[y * ST_W + x] == 255) stranded++;
    CHECK(stranded == 0, "%d walkable cells cannot be reached from CT spawn", stranded);

    /* And that the four named areas exist and are among them. */
    static const int zones[4] = { Z_SPAWN_T, Z_SPAWN_CT, Z_SITE_A, Z_SITE_B };
    static const char *names[4] = { "T spawn", "CT spawn", "site A", "site B" };
    for (int i = 0; i < 4; i++) {
        int n = 0, reach = 0;
        for (int y = 0; y < ST_H; y++)
            for (int x = 0; x < ST_W; x++)
                if (st_zone[y][x] == zones[i]) {
                    n++;
                    if (field[y * ST_W + x] != 255) reach++;
                }
        CHECK(n >= 20, "%s is only %d cells", names[i], n);
        CHECK(reach == n, "%s has %d cells unreachable from CT spawn", names[i], n - reach);
    }

    /* The two spawns should not be next door to each other, and neither
     * should be on top of a bombsite. */
    float tx, ty;
    st_zone_centre(Z_SPAWN_T, &tx, &ty);
    float d = sqrtf((tx - cx) * (tx - cx) + (ty - cy) * (ty - cy));
    CHECK(d > 20.0f, "the spawns are %.1f cells apart, which is no map at all", d);

    free(field);
}

static void los_agrees_with_walls(void) {
    st_map_load();
    float cx, cy;
    st_zone_centre(Z_SPAWN_CT, &cx, &cy);

    CHECK(st_los(cx, cy, cx, cy), "a point cannot see itself");
    CHECK(st_los(cx, cy, cx + 0.3f, cy + 0.3f), "a point cannot see the next inch");

    /* Nothing inside rock can see out, and nothing outside can see in. */
    int wx = -1, wy = -1;
    for (int y = 0; y < ST_H && wx < 0; y++)
        for (int x = 0; x < ST_W && wx < 0; x++)
            if (st_solid(x, y) && x > 2 && y > 2) { wx = x; wy = y; }
    CHECK(wx >= 0, "the map has no walls in it");
    CHECK(!st_los(wx + 0.5f, wy + 0.5f, cx, cy), "a wall cell can see out of itself");

    /* Opposite corners of the map must not see each other; if they do the DDA
     * is walking past walls rather than into them. */
    CHECK(!st_los(2.5f, 2.5f, (float)ST_W - 2.5f, (float)ST_H - 2.5f),
          "opposite corners of the map have line of sight");

    /* Symmetry: sight is not one-way. This has caught an off-by-one in the
     * starting cell twice in other raycasters. */
    int asym = 0;
    for (int i = 0; i < 400; i++) {
        float ax = 1.0f + (float)(rand() % (ST_W - 2)) + 0.5f;
        float ay = 1.0f + (float)(rand() % (ST_H - 2)) + 0.5f;
        float bx = 1.0f + (float)(rand() % (ST_W - 2)) + 0.5f;
        float by = 1.0f + (float)(rand() % (ST_H - 2)) + 0.5f;
        if (st_solid_at(ax, ay) || st_solid_at(bx, by)) continue;
        if (st_los(ax, ay, bx, by) != st_los(bx, by, ax, ay)) asym++;
    }
    CHECK(asym == 0, "%d pairs of points could see each other one way only", asym);
}

/* ---- the rules ----------------------------------------------------------- */
static void weapons_are_sane(void) {
    for (int i = 0; i < W_COUNT; i++) {
        const st_weapon *w = &st_weap[i];
        CHECK(w->name && w->name[0], "weapon %d has no name", i);
        CHECK(w->rpm > 0, "%s has no rate of fire, so 60000/rpm divides by zero", w->name);
        CHECK(w->magsz > 0, "%s has no magazine", w->name);
        CHECK(w->dmg > 0, "%s does no damage", w->name);
        CHECK(w->armor_pen > 0 && w->armor_pen <= 100,
              "%s has %d%% armour penetration", w->name, w->armor_pen);
    }
}

static void armour_actually_helps(void) {
    st_game_init(3, 2);
    st_ent *a = &st_w.ent[1], *b = &st_w.ent[2];

    a->hp = 100; a->armor = 0;   a->helmet = 0; a->alive = 1;
    b->hp = 100; b->armor = 100; b->helmet = 0; b->alive = 1;
    st_ent *shooter = &st_w.ent[0];
    shooter->wep = W_AR47;

    st_hurt(a, shooter, 40, 0);
    st_hurt(b, shooter, 40, 0);
    CHECK(b->hp > a->hp, "armour did nothing: %d hp vs %d", b->hp, a->hp);
    CHECK(b->armor < 100, "armour absorbed damage without wearing down");

    /* A head shot has to be worse than a body shot, with or without a helmet. */
    st_ent *c = &st_w.ent[3];
    c->hp = 100; c->armor = 0; c->helmet = 0; c->alive = 1;
    st_hurt(c, shooter, 20, 1);
    CHECK(c->hp < 100 - 20, "a head shot did no more than a body shot");

    st_game_free();
}

static void money_stays_in_bounds(void) {
    st_game_init(5, 2);
    for (int i = 0; i < st_w.nent; i++) st_w.ent[i].money = 15900;
    for (int r = 0; r < 4000; r++) st_step(30);
    for (int i = 0; i < st_w.nent; i++) {
        CHECK(st_w.ent[i].money >= 0, "%s went into debt (%d)",
              st_w.ent[i].name, st_w.ent[i].money);
        CHECK(st_w.ent[i].money <= 16000, "%s has %d, over the cap",
              st_w.ent[i].name, st_w.ent[i].money);
    }
    st_game_free();
}

/* ---- the thing this file exists for -------------------------------------- */
static void rounds_end_and_bots_move(void) {
    st_game_init(5, 2);
    /* Slot 0 is the human, and a human who is a T is handed the bomb. Leaving
     * it inert means every round of the second half is a guaranteed "Time
     * expired" with the bomb still in a mannequin's hands -- which is a fact
     * about this harness and not about the game, and it was quietly costing
     * the Ts every round they were measured on. Let it play. */
    st_w.ent[0].bot = 1;

    int rounds_seen = 0, moved_total = 0;

    float px[ST_MAX_ENT], py[ST_MAX_ENT];
    float travelled[ST_MAX_ENT];
    memset(travelled, 0, sizeof(travelled));
    for (int i = 0; i < st_w.nent; i++) { px[i] = st_w.ent[i].x; py[i] = st_w.ent[i].y; }

    int last_round = st_w.round;
    int ms_in_round = 0;
    /* Three quarters of an hour of game time at a 30 ms step. It used to be ten
     * minutes, which is eight rounds, and eight rounds is not a sample: the map
     * is CT-sided (they defend, and whoever is already looking at the doorway
     * wins the fight), so at roughly a third of rounds to the Ts an eight-round
     * window comes up all-CT about once in thirty runs. Thirty-odd rounds makes
     * that vanishingly unlikely without weakening what is being asserted, and
     * it still runs in about a second. */
    for (int t = 0; t < 90000; t++) {
        st_step(30);
        ms_in_round += 30;

        for (int i = 0; i < st_w.nent; i++) {
            st_ent *e = &st_w.ent[i];
            float dx = e->x - px[i], dy = e->y - py[i];
            travelled[i] += sqrtf(dx * dx + dy * dy);
            px[i] = e->x; py[i] = e->y;

            CHECK(!st_solid_at(e->x, e->y) || !e->alive,
                  "%s is alive inside a wall at %.1f,%.1f", e->name, e->x, e->y);
            CHECK(e->hp >= 0 && e->hp <= 100, "%s has %d hp", e->name, e->hp);
        }

        if (st_w.round != last_round) {
            rounds_seen++;
            last_round = st_w.round;
            /* A round cannot outlast its own clock plus the fuse plus the
             * five seconds of standing around afterwards. */
            CHECK(ms_in_round < FREEZE_MS + ROUND_MS + BOMB_FUSE_MS + 20000,
                  "round %d took %d ms, which means it did not end on its own",
                  st_w.round - 1, ms_in_round);
            ms_in_round = 0;
        }
    }

    CHECK(rounds_seen >= 20, "only %d rounds in forty-five minutes of game time", rounds_seen);
    CHECK(st_w.score_t + st_w.score_ct == rounds_seen,
          "%d rounds played but the scoreboard says %d",
          rounds_seen, st_w.score_t + st_w.score_ct);

    /* Every bot has to have gone somewhere. One that never moves is a bot
     * whose flow field came back empty, and it will look exactly like a bot
     * that is "holding an angle" until you watch it for a whole round. */
    for (int i = 1; i < st_w.nent; i++) {
        if (!st_w.ent[i].used) continue;
        if (travelled[i] > 30.0f) moved_total++;
        CHECK(travelled[i] > 30.0f, "%s moved %.1f cells in forty-five minutes",
              st_w.ent[i].name, travelled[i]);
    }
    CHECK(moved_total >= 4, "only %d bots ever moved", moved_total);

    /* Both teams should be winning sometimes. A game one side always wins is
     * usually a rule that never fires, not a hard map. */
    CHECK(st_w.score_t > 0, "the Ts never won a round in %d", rounds_seen);
    CHECK(st_w.score_ct > 0, "the CTs never won a round in %d", rounds_seen);

    printf("  %d rounds, T %d : CT %d, %d bots walked\n",
           rounds_seen, st_w.score_t, st_w.score_ct, moved_total);
    st_game_free();
}

/* The bomb has to be plantable, or half the rules never run. The human slot
 * does nothing in this harness, so this drives the carrier directly. */
static void bomb_can_be_planted_and_defused(void) {
    st_game_init(2, 1);

    int carrier = -1;
    for (int i = 0; i < st_w.nent; i++)
        if (st_w.ent[i].used && st_w.ent[i].has_bomb) carrier = i;
    CHECK(carrier >= 0, "nobody was given the bomb");
    if (carrier < 0) { st_game_free(); return; }

    /* Skip the freeze, put the carrier on the site, hold the trigger. */
    while (st_w.phase == R_FREEZE) st_step(100);

    float sx, sy;
    st_zone_centre(st_w.bomb_site, &sx, &sy);
    st_w.ent[carrier].x = sx;
    st_w.ent[carrier].y = sy;
    CHECK(st_zone[(int)sy][(int)sx] == st_w.bomb_site,
          "the centre of the bombsite is not in the bombsite");

    for (int t = 0; t < 200 && !st_w.bomb_down; t++) {
        st_w.ent[carrier].x = sx;            /* it wants to walk off; don't let it */
        st_w.ent[carrier].y = sy;
        st_step(50);
    }
    CHECK(st_w.bomb_down, "the bomb was never planted from the middle of the site");
    CHECK(st_w.phase == R_PLANTED, "the bomb is down but the round is in phase %d",
          st_w.phase);

    if (st_w.bomb_down) {
        /* Now defuse it, through the path a player uses -- slot 0 holding the
         * use key -- because that code runs in st_step() and nothing else in
         * this file would ever reach it.
         *
         * The Ts go first. A bot standing on a live bomb with an enemy in
         * front of it goes to B_FIGHT and stays there, which is correct and
         * makes the defuse rule untestable while anyone is shooting. */
        for (int i = 0; i < st_w.nent; i++)
            if (st_w.ent[i].team == T_SIDE) st_w.ent[i].alive = 0;

        st_ent *you = st_you();
        you->alive = 1;
        you->has_kit = 1;
        CHECK(you->team == CT_SIDE, "slot 0 is meant to be a CT");

        for (int t = 0; t < 300 && st_w.phase == R_PLANTED; t++) {
            you->x = st_w.bomb_x;
            you->y = st_w.bomb_y;
            st_w.use_held = 1;
            st_step(50);
        }
        st_w.use_held = 0;
        CHECK(st_w.phase == R_OVER, "held use on a live bomb and nothing happened");
        CHECK(st_w.winner == CT_SIDE, "the bomb was defused and the Ts won");

        /* And it must not defuse itself: letting go has to reset the timer. */
        st_game_init(2, 1);
        while (st_w.phase == R_FREEZE) st_step(100);
        st_w.bomb_down = 1; st_w.bomb_ms = 30000; st_w.phase = R_PLANTED;
        st_w.bomb_x = st_you()->x; st_w.bomb_y = st_you()->y;
        /* Everyone but you: the Ts so nothing shoots, and the CT bots because
         * they defuse it themselves given fifteen seconds and no opposition,
         * which is correct behaviour and would fail this check for the wrong
         * reason. */
        for (int i = 1; i < st_w.nent; i++) st_w.ent[i].alive = 0;
        for (int t = 0; t < 300 && st_w.phase == R_PLANTED; t++) {
            st_you()->x = st_w.bomb_x; st_you()->y = st_w.bomb_y;
            st_w.use_held = 0;
            st_step(50);
        }
        CHECK(st_w.phase == R_PLANTED || st_w.winner == T_SIDE,
              "the bomb defused itself with nobody holding the key");
    }
    st_game_free();
}

/* A round with the fuse left alone has to end in an explosion, not in a
 * timer that quietly stops. */
static void bomb_explodes_when_nobody_defuses(void) {
    st_game_init(2, 0);
    while (st_w.phase == R_FREEZE) st_step(100);

    st_w.bomb_down = 1;
    st_w.bomb_ms = 4000;
    st_w.bomb_x = 10.5f; st_w.bomb_y = 10.5f;
    st_w.phase = R_PLANTED;
    /* Kill every CT so nobody wanders over and defuses it by accident. */
    for (int i = 0; i < st_w.nent; i++)
        if (st_w.ent[i].team == CT_SIDE) st_w.ent[i].alive = 0;

    for (int t = 0; t < 400 && st_w.phase == R_PLANTED; t++) st_step(50);
    CHECK(st_w.phase == R_OVER, "the fuse ran out and the round kept going");
    CHECK(st_w.winner == T_SIDE, "the bomb went off and the CTs won");
    st_game_free();
}

/* Kill the carrier and the bomb has to still be in the round.
 *
 * st_hurt dropped it where the carrier fell and there was nothing anywhere that
 * could pick it up: bomb_down stayed 0, so the renderer did not draw it and no
 * rule looked at it. The first thing any CT does is shoot the man with the
 * bomb, and doing that quietly removed the objective for the rest of the round.
 */
static void a_dropped_bomb_comes_back(void) {
    st_game_init(3, 1);
    while (st_w.phase == R_FREEZE) st_step(100);

    int carrier = -1;
    for (int i = 0; i < st_w.nent; i++)
        if (st_w.ent[i].used && st_w.ent[i].has_bomb) carrier = i;
    CHECK(carrier >= 0, "nobody was given the bomb");
    if (carrier < 0) { st_game_free(); return; }

    /* Put a second T right beside the carrier, then kill the carrier. */
    int mate = -1;
    for (int i = 0; i < st_w.nent; i++)
        if (st_w.ent[i].used && st_w.ent[i].team == T_SIDE && i != carrier) mate = i;
    CHECK(mate >= 0, "the T side is one man");
    if (mate < 0) { st_game_free(); return; }

    float bx = st_w.ent[carrier].x, by = st_w.ent[carrier].y;
    st_w.ent[mate].x = bx; st_w.ent[mate].y = by;
    st_hurt(&st_w.ent[carrier], 0, 500, 0);

    CHECK(st_w.bomb_loose, "the carrier died and the bomb went with him");
    CHECK(!st_w.ent[carrier].has_bomb, "a dead man is still carrying the bomb");

    st_step(16);
    CHECK(st_w.ent[mate].has_bomb, "a T stood on the dropped bomb and did not take it");
    CHECK(!st_w.bomb_loose, "the bomb was picked up and is still lying there");

    /* A CT must not be able to pick it up, and must not be able to defuse
     * something that was never planted. */
    st_game_init(3, 1);
    while (st_w.phase == R_FREEZE) st_step(100);
    st_w.bomb_loose = 1;
    st_w.bomb_x = st_you()->x; st_w.bomb_y = st_you()->y;
    CHECK(st_you()->team == CT_SIDE, "slot 0 is meant to start as a CT");
    for (int t = 0; t < 40; t++) { st_w.use_held = 1; st_step(16); }
    st_w.use_held = 0;
    CHECK(!st_you()->has_bomb, "a CT picked up the bomb");
    CHECK(st_w.phase != R_OVER, "a CT defused a bomb that was never planted");
    st_game_free();
}

/* Sides have to change ends, or the plant key never does anything for the one
 * player in the game who is not a bot. */
static void sides_swap_at_halftime(void) {
    st_game_init(3, 1);
    st_w.ent[0].bot = 1;
    int first = st_you()->team;
    CHECK(first == CT_SIDE, "slot 0 is meant to start as a CT");

    int swaps = 0, was = first, saw_t = 0;
    for (int t = 0; t < 400000 && st_w.round <= HALF_ROUNDS * 2 + 1; t++) {
        st_step(30);
        if (st_you()->team != was) { swaps++; was = st_you()->team; }
        if (st_you()->team == T_SIDE) saw_t = 1;
    }
    CHECK(swaps >= 1, "%d rounds went by and nobody changed ends", st_w.round);
    CHECK(saw_t, "slot 0 was never a Terrorist, so it can never plant anything");

    /* And nobody takes their old rifle across the line with them. */
    st_game_free();
}

/* A crowd on one bomb defuses it once, not once per person. Everybody used to
 * add their own dt to the same timer. */
static void a_crowd_defuses_at_one_speed(void) {
    st_game_init(5, 0);
    while (st_w.phase == R_FREEZE) st_step(100);
    st_w.bomb_down = 1; st_w.bomb_ms = 60000; st_w.phase = R_PLANTED;
    st_w.bomb_x = 10.5f; st_w.bomb_y = 10.5f;
    for (int i = 0; i < st_w.nent; i++)
        if (st_w.ent[i].team == T_SIDE) st_w.ent[i].alive = 0;
    /* Every CT standing on it, none of them with a kit. */
    for (int i = 0; i < st_w.nent; i++) {
        if (st_w.ent[i].team != CT_SIDE) continue;
        st_w.ent[i].x = st_w.bomb_x; st_w.ent[i].y = st_w.bomb_y;
        st_w.ent[i].has_kit = 0;
    }

    int ms = 0;
    for (int t = 0; t < 2000 && st_w.phase == R_PLANTED; t++) { st_step(20); ms += 20; }
    CHECK(st_w.phase == R_OVER, "a pile of CTs on a live bomb never defused it");
    CHECK(ms > DEFUSE_MS - 400,
          "five CTs defused in %d ms, and one takes %d", ms, DEFUSE_MS);
    st_game_free();
}

int main(void) {
    printf("strike: map\n");            map_is_connected();
    printf("strike: line of sight\n");  los_agrees_with_walls();
    printf("strike: weapons\n");        weapons_are_sane();
    printf("strike: armour\n");         armour_actually_helps();
    printf("strike: economy\n");        money_stays_in_bounds();
    printf("strike: bomb\n");           bomb_can_be_planted_and_defused();
    printf("strike: fuse\n");           bomb_explodes_when_nobody_defuses();
    printf("strike: dropped bomb\n");   a_dropped_bomb_comes_back();
    printf("strike: halftime\n");       sides_swap_at_halftime();
    printf("strike: defuse crowd\n");   a_crowd_defuses_at_one_speed();
    printf("strike: rounds\n");         rounds_end_and_bots_move();

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("strike host tests passed\n");
    return 0;
}
