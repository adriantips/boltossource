/* ===========================================================================
 *  BoltOS  -  kernel/app_strike.c
 *  BoltStrike: the window, the input, and everything drawn in two dimensions.
 *
 *  The game itself is under strike/ and knows nothing about windows. This file
 *  owns the three things that have to be here: the desktop-loop hook that
 *  drives a frame, the raw keyboard and mouse (a game needs held keys and
 *  relative motion, neither of which a window callback can give it), and the
 *  HUD, which is drawn at the window's own resolution so the numbers stay
 *  sharp however coarse the 320x200 render underneath is.
 *
 *  Mouse capture is the fiddly part. While you are playing, the cursor is
 *  warped back to the middle of the client area every frame: relative motion
 *  still arrives, so you can turn for ever, but the pointer can never walk off
 *  the window and start clicking on the desktop behind it. Escape gives it
 *  back.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "pit.h"
#include "keyboard.h"
#include "mouse.h"
#include "stdio.h"
#include "string.h"
#include "math.h"
#include "../strike/strike.h"

/* Set-1 scancodes. Spelled out because a game reads about twenty of them and
 * a table of names is worth more here than a table of numbers. */
#define SC_ESC 0x01
#define SC_1   0x02
#define SC_TAB 0x0F
#define SC_Q   0x10
#define SC_W   0x11
#define SC_E   0x12
#define SC_R   0x13
#define SC_A   0x1E
#define SC_S   0x1F
#define SC_D   0x20
#define SC_G   0x22
#define SC_LSH 0x2A
#define SC_B   0x30
#define SC_N   0x31
#define SC_M   0x32
#define SC_SPC 0x39
#define SC_LCT 0x1D

#define FRAME_MS 33     /* ~30 fps of drawing  */
#define STEP_MS  16     /* ~60 Hz of thinking  */

static window_t *g_win;
static int   captured;              /* the mouse belongs to the game          */
static int   started;               /* st_game_init has run                   */
static int   failed;                /* the heap said no; say so and stop      */
static int   buymenu, scoreboard;
static int   zoomed;
static float pitch;
static uint64_t last_frame, last_step;
static int   prev_down[256];
static char  feed[64];              /* the last thing that happened           */
static int   feed_ms;
static int   diff = 1;              /* bot skill 0..3                         */
static int   nbots = 4;

/* ---- one place that knows what a client rect looks like ------------------ */
static int strike_active(void) {
    return g_win && g_win->open && !g_win->minimized && gui_window_focused(g_win);
}

static int edge(int sc) {
    int now = kbd_key_down(sc);
    int was = prev_down[sc & 0xFF];
    prev_down[sc & 0xFF] = now;
    return now && !was;
}

static void say(const char *s) {
    strlcpy(feed, s, sizeof(feed));
    feed_ms = 2600;
}

/* Volume and pan for a sound happening at (x,y), heard from where you stand
 * and facing the way you are. Volume falls off with the square of the distance
 * because that is roughly right and costs one multiply; the pan is how far the
 * source is along your own right-hand vector. */
static void sfx_at(int id, float x, float y) {
    st_ent *you = st_you();
    float dx = x - you->x, dy = y - you->y;
    float d2 = dx * dx + dy * dy;
    int vol = 255 - (int)(d2 * 1.6f);
    if (vol <= 6) return;
    if (vol > 255) vol = 255;

    int pan = 0;
    float len = sqrtf(d2);
    if (len > 0.05f) {
        float rx = -sinf(you->ang), ry = cosf(you->ang);
        float side = (dx * rx + dy * ry) / len;      /* -1 left .. +1 right    */
        pan = (int)(side * 110.0f);
    }
    st_sfx_play(id, vol, pan);
}

/* ---- the round, watched from outside ------------------------------------ *
 *  The game core does not call back into anything, so the sounds and the kill
 *  feed are produced by noticing that a number changed. That is a deliberately
 *  dumb arrangement and it has one real advantage: strike/ stays testable
 *  without a desktop attached. */
static int  seen_phase = -1, seen_round = -1;
static int  seen_hp = 100, seen_alive[ST_MAX_ENT];
static int  seen_mag = -1;
static int  seen_loose, seen_carry;
static int  beep_acc;

static void watch(int dt) {
    st_ent *you = st_you();

    if (st_w.round != seen_round) {
        seen_round = st_w.round;
        seen_phase = -1;
        for (int i = 0; i < ST_MAX_ENT; i++) seen_alive[i] = st_w.ent[i].alive;
        seen_hp = you->hp;
        seen_loose = st_w.bomb_loose;
        seen_carry = you->has_bomb;
        if (st_w.swapped)
            say(you->team == T_SIDE ? "Halftime -- you are a Terrorist now"
                                    : "Halftime -- you are a Counter-Terrorist now");
    }
    if (st_w.phase != seen_phase) {
        if (st_w.phase == R_LIVE) say("Go!");
        if (st_w.phase == R_PLANTED) {
            say("The bomb has been planted.");
            st_sfx_play(S_PLANT, 220, 0);
        }
        if (st_w.phase == R_OVER) {
            int won = st_w.winner == you->team;
            char msg[64];
            snprintf(msg, sizeof(msg), "%s -- %s",
                     st_w.winner == T_SIDE ? "Terrorists win" : "Counter-Terrorists win",
                     st_w.win_why);
            say(msg);
            st_sfx_play(won ? S_WIN : S_LOSE, 200, 0);
        }
        seen_phase = st_w.phase;
    }

    for (int i = 0; i < st_w.nent; i++) {
        if (!st_w.ent[i].used) continue;
        if (seen_alive[i] && !st_w.ent[i].alive) {
            sfx_at(S_DIE, st_w.ent[i].x, st_w.ent[i].y);
            if (i == 0) say("You were killed.");
            else if (st_w.ent[i].team != you->team) say("Enemy down.");
        }
        seen_alive[i] = st_w.ent[i].alive;
    }

    if (you->hp < seen_hp && you->alive) st_sfx_play(S_HIT, 200, 0);
    seen_hp = you->hp;

    /* The bomb changing hands is the single most important thing that can
     * happen without anybody shooting, and until the drop worked at all there
     * was nothing here to say so. */
    if (st_w.bomb_loose != seen_loose) {
        if (st_w.bomb_loose) say("The bomb has been dropped.");
        seen_loose = st_w.bomb_loose;
    }
    if (you->has_bomb != seen_carry) {
        if (you->has_bomb) say("You have the bomb -- hold E on a site to plant.");
        seen_carry = you->has_bomb;
    }

    if (you->mag[you->wep] != seen_mag) {
        if (seen_mag >= 0 && you->mag[you->wep] > seen_mag) st_sfx_play(S_RELOAD, 170, 0);
        seen_mag = you->mag[you->wep];
    }

    /* The bomb's beep speeds up as the fuse burns down, which is the only
     * countdown anybody actually listens to. */
    if (st_w.phase == R_PLANTED) {
        beep_acc += dt;
        int period = 180 + (st_w.bomb_ms * 820) / BOMB_FUSE_MS;
        if (beep_acc >= period) {
            beep_acc = 0;
            sfx_at(S_BEEP, st_w.bomb_x, st_w.bomb_y);
        }
    }
}

/* ---- input --------------------------------------------------------------- */
static void do_input(int dt) {
    st_ent *you = st_you();

    if (edge(SC_ESC)) {
        if (buymenu)        buymenu = 0;
        else if (captured)  captured = 0;
        return;
    }
    if (edge(SC_B) && st_can_buy(you)) buymenu = !buymenu;
    scoreboard = kbd_key_down(SC_TAB);

    if (buymenu) {
        /* 1..7 buy; the list is built in draw_buymenu and indexed the same way */
        for (int i = 0; i < 7; i++) {
            if (!edge(SC_1 + i)) continue;
            switch (i) {
            case 0: st_buy(you, W_DE50);  break;
            case 1: st_buy(you, W_MP9);   break;
            case 2: st_buy(you, W_SG12);  break;
            case 3: st_buy(you, you->team == T_SIDE ? W_AR47 : W_AR15C); break;
            case 4: st_buy(you, W_BR338); break;
            case 5: st_buy_armor(you, 0); break;
            case 6: st_buy_armor(you, 1); break;
            }
            st_sfx_play(S_BUY, 150, 0);
        }
        return;                               /* no walking while shopping     */
    }

    if (!you->alive) return;

    /* weapon slots */
    if (edge(SC_1)) {
        for (int w = W_COUNT - 1; w > 0; w--)
            if (st_weap[w].kind >= WK_SMG && you->has[w]) { st_switch(you, w); break; }
    }
    if (edge(SC_1 + 1)) st_switch(you, you->has[W_DE50] ? W_DE50
                                     : (you->team == T_SIDE ? W_GK9 : W_M45));
    if (edge(SC_1 + 2)) st_switch(you, W_KNIFE);
    if (edge(SC_R)) st_reload(you);

    st_w.use_held = kbd_key_down(SC_E);

    /* movement: an impulse each frame, with shift to walk quietly */
    float speed = kbd_key_down(SC_LSH) ? 1.5f : 3.4f;
    if (st_w.phase == R_FREEZE) speed = 0.0f;
    float fx = cosf(you->ang), fy = sinf(you->ang);
    float mx = 0.0f, my = 0.0f;
    if (kbd_key_down(SC_W)) { mx += fx; my += fy; }
    if (kbd_key_down(SC_S)) { mx -= fx; my -= fy; }
    if (kbd_key_down(SC_A)) { mx += fy; my -= fx; }
    if (kbd_key_down(SC_D)) { mx -= fy; my += fx; }
    float ml = sqrtf(mx * mx + my * my);
    if (ml > 0.001f) {
        float s = speed * (float)dt / 1000.0f;
        st_move(you, mx / ml * s, my / ml * s);
        you->vx = mx / ml * speed;
        you->vy = my / ml * speed;
        you->bob += (float)dt / 130.0f;
        you->step_acc += dt;
        /* A footstep every stride, and only when you are not sneaking -- the
         * bots do not hear it, but you should. */
        static int stepacc;
        stepacc += dt;
        if (stepacc > (kbd_key_down(SC_LSH) ? 620 : 380)) {
            stepacc = 0;
            if (!kbd_key_down(SC_LSH)) st_sfx_play(S_STEP, 70, 0);
        }
    } else {
        you->vx = you->vy = 0.0f;
    }

    /* look */
    if (captured) {
        int dx = 0, dy = 0;
        mouse_take_delta(&dx, &dy);
        /* A frame's worth of hand, and no more. If the desktop stalls -- a
         * disk flush, a window being dragged over us -- the accumulator keeps
         * filling and the whole backlog arrives in one frame, which reads as
         * the view being flung somewhere at random. */
        if (dx >  180) dx =  180;
        if (dx < -180) dx = -180;
        if (dy >  120) dy =  120;
        if (dy < -120) dy = -120;
        float sens = zoomed ? 0.0012f : 0.0038f;
        you->ang += (float)dx * sens;
        pitch -= (float)dy * (zoomed ? 0.25f : 0.60f);
        if (pitch < -60.0f) pitch = -60.0f;
        if (pitch > 60.0f)  pitch = 60.0f;

        uint8_t b = mouse_buttons();
        zoomed = (b & MOUSE_RIGHT) && st_weap[you->wep].kind == WK_SNIPER;

        /* The latch is about the button, not the gun: a semi-automatic fires
         * once per press, an automatic fires for as long as it is down. */
        static int held;
        if (b & MOUSE_LEFT) {
            const st_weapon *w = &st_weap[you->wep];
            if (w->full_auto || !held) {
                if (you->mag[you->wep] <= 0 && w->magsz > 1) {
                    if (you->fire_cd <= 0) { st_sfx_play(S_EMPTY, 120, 0); you->fire_cd = 250; }
                } else if (st_fire(you)) {
                    int s = w->kind == WK_SNIPER  ? S_SHOT_SNIPER
                          : w->kind == WK_SHOTGUN ? S_SHOT_SHOTGUN
                          : w->kind == WK_RIFLE   ? S_SHOT_RIFLE
                          : w->kind == WK_MELEE   ? S_KNIFE : S_SHOT_PISTOL;
                    st_sfx_play(s, 230, 0);
                }
            }
            held = 1;
        } else {
            held = 0;
        }
    } else {
        /* Not captured: the arrow keys still turn, so the game is watchable
         * (and testable) without taking the pointer away from anybody. */
        if (kbd_key_down(0xCB)) you->ang -= 2.2f * (float)dt / 1000.0f;
        if (kbd_key_down(0xCD)) you->ang += 2.2f * (float)dt / 1000.0f;
        zoomed = 0;
    }
}

/* ---- HUD ----------------------------------------------------------------- */
static void bar(int x, int y, int w, int h, int pct, uint32_t col) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_fill(x, y, w, h, 0x181818);
    g_fill(x + 1, y + 1, (w - 2) * pct / 100, h - 2, col);
    g_rect(x, y, w, h, 0x404040);
}

static void draw_hud(int cx, int cy, int cw, int ch) {
    char buf[96];
    st_ent *you = st_you();
    int sc = cw >= 700 ? 2 : 1;
    int fh = g_font_height(sc);

    /* top: score and the clock */
    int secs = st_w.phase == R_PLANTED ? st_w.bomb_ms / 1000 : st_w.phase_ms / 1000;
    if (secs < 0) secs = 0;
    snprintf(buf, sizeof(buf), "T %d   %d:%02d   %d CT",
             st_w.score_t, secs / 60, secs % 60, st_w.score_ct);
    uint32_t clock_col = st_w.phase == R_PLANTED ? 0xFF4030
                       : (secs <= 10 ? 0xFFC040 : COL_TEXT);
    g_blend(cx + cw / 2 - 90, cy + 4, 180, fh + 6, 0x000000, 170);
    g_text(cx + cw / 2 - g_text_width(buf, sc) / 2, cy + 7, buf, clock_col, sc);

    if (st_w.phase == R_FREEZE) {
        const char *m = st_can_buy(you) ? "Freeze time  -  B to buy"
                                        : "Freeze time";
        g_text(cx + cw / 2 - g_text_width(m, sc) / 2, cy + 12 + fh, m, COL_TEXT_DIM, sc);
    }

    /* bottom left: health and armour.
     *
     * Everything below sits on a dark plate, because green numbers at scale 1
     * on a concrete floor are no contrast at all, and the HUD is the one thing
     * that has to stay legible whatever happens to be behind it.
     *
     * Two lines tall, not one: the label and the bar under it. */
    g_blend(cx, cy + ch - 2 * fh - 20, cw, 2 * fh + 20, 0x000000, 120);
    int by = cy + ch - 2 * fh - 14;
    snprintf(buf, sizeof(buf), "%3d HP", you->hp);
    g_text(cx + 14, by, buf, you->hp > 35 ? COL_GOOD : COL_BAD, sc);
    bar(cx + 14, by + fh + 2, 110, 6, you->hp, you->hp > 35 ? 0x40C060 : 0xC04040);
    if (you->armor > 0) {
        snprintf(buf, sizeof(buf), "%3d AR%s", you->armor, you->helmet ? " +H" : "");
        g_text(cx + 140, by, buf, 0x70A0E0, sc);
        bar(cx + 140, by + fh + 2, 110, 6, you->armor, 0x4070C0);
    }

    /* bottom right: the gun */
    const st_weapon *w = &st_weap[you->wep];
    if (w->magsz > 1) snprintf(buf, sizeof(buf), "%s   %d / %d", w->name,
                               you->mag[you->wep], you->res[you->wep]);
    else              snprintf(buf, sizeof(buf), "%s", w->name);
    g_text(cx + cw - 16 - g_text_width(buf, sc), by, buf,
           you->reload_cd > 0 ? COL_WARN : COL_TEXT, sc);
    snprintf(buf, sizeof(buf), "$%d", you->money);
    g_text(cx + cw - 16 - g_text_width(buf, sc), by + fh + 2, buf, 0x60C060, sc);

    if (you->has_bomb) {
        const char *m = "You have the bomb";
        g_text(cx + cw / 2 - g_text_width(m, sc) / 2, by + fh + 2, m, 0xE08040, sc);
    }

    /* plant / defuse progress */
    if (st_w.planting || st_w.defusing) {
        int total = st_w.planting ? PLANT_MS
                  : (st_w.defuser >= 0 && st_w.ent[st_w.defuser].has_kit
                     ? DEFUSE_KIT_MS : DEFUSE_MS);
        int got = st_w.planting ? st_w.plant_ms : st_w.defuse_ms;
        const char *m = st_w.planting ? "Planting..." : "Defusing...";
        int bw = 240;
        g_text(cx + cw / 2 - g_text_width(m, sc) / 2, cy + ch / 2 + 40, m, COL_TEXT, sc);
        bar(cx + cw / 2 - bw / 2, cy + ch / 2 + 42 + fh, bw, 10,
            total ? got * 100 / total : 0, st_w.planting ? 0xE08030 : 0x3090E0);
    }

    if (!you->alive && st_w.phase != R_OVER) {
        const char *m = "Dead  -  watching";
        g_text(cx + cw / 2 - g_text_width(m, 2) / 2, cy + ch / 2 - 40, m, COL_BAD, 2);
    }

    if (feed_ms > 0)
        g_text(cx + 14, cy + 10 + fh, feed, COL_TEXT, sc);

    if (!captured) {
        const char *m = "Click to play  -  Esc releases the mouse";
        int tw = g_text_width(m, sc);
        g_blend(cx + cw / 2 - tw / 2 - 8, cy + ch - 3 * fh - 26, tw + 16, fh + 8, 0x000000, 170);
        g_text(cx + cw / 2 - tw / 2, cy + ch - 3 * fh - 24, m, COL_ACCENT, sc);
    }
}

typedef struct { const char *name; int price; } buyrow;

static void draw_buymenu(int cx, int cy, int cw, int ch) {
    st_ent *you = st_you();
    buyrow rows[7];
    rows[0] = (buyrow){ st_weap[W_DE50].name, st_weap[W_DE50].price };
    rows[1] = (buyrow){ st_weap[W_MP9].name,  st_weap[W_MP9].price };
    rows[2] = (buyrow){ st_weap[W_SG12].name, st_weap[W_SG12].price };
    int rifle = you->team == T_SIDE ? W_AR47 : W_AR15C;
    rows[3] = (buyrow){ st_weap[rifle].name,  st_weap[rifle].price };
    rows[4] = (buyrow){ st_weap[W_BR338].name, st_weap[W_BR338].price };
    rows[5] = (buyrow){ "Kevlar vest", 650 };
    rows[6] = (buyrow){ "Kevlar + helmet", 1000 };

    int w = 320, h = 40 + 7 * 26;
    int x = cx + cw / 2 - w / 2, y = cy + ch / 2 - h / 2;
    g_blend(x, y, w, h, 0x000000, 210);
    g_rect(x, y, w, h, COL_ACCENT);

    char buf[80];
    snprintf(buf, sizeof(buf), "BUY   $%d", you->money);
    g_text(x + 14, y + 10, buf, COL_ACCENT, 2);

    for (int i = 0; i < 7; i++) {
        int ry = y + 38 + i * 26;
        int afford = you->money >= rows[i].price;
        snprintf(buf, sizeof(buf), "%d  %-18s $%d", i + 1, rows[i].name, rows[i].price);
        g_text(x + 14, ry, buf, afford ? COL_TEXT : COL_TEXT_DIM, 1);
    }
    g_text(x + 14, y + h - 18, "B or Esc closes", COL_TEXT_DIM, 1);
}

static void draw_scoreboard(int cx, int cy, int cw, int ch) {
    int w = 380, h = 44 + st_w.nent * 20;
    int x = cx + cw / 2 - w / 2, y = cy + ch / 2 - h / 2;
    g_blend(x, y, w, h, 0x000000, 215);
    g_rect(x, y, w, h, COL_ACCENT);
    g_text(x + 14, y + 10, "NAME", COL_TEXT_DIM, 1);
    g_text(x + 180, y + 10, "TEAM", COL_TEXT_DIM, 1);
    g_text(x + 250, y + 10, "K", COL_TEXT_DIM, 1);
    g_text(x + 290, y + 10, "D", COL_TEXT_DIM, 1);
    g_text(x + 320, y + 10, "$", COL_TEXT_DIM, 1);

    char buf[32];
    int row = 0;
    for (int side = 0; side < 2; side++) {
        for (int i = 0; i < st_w.nent; i++) {
            st_ent *e = &st_w.ent[i];
            if (!e->used || e->team != side) continue;
            int ry = y + 32 + row * 20;
            uint32_t c = e->alive ? (side == T_SIDE ? 0xE0A050 : 0x70A0E0) : 0x606060;
            g_text(x + 14, ry, e->name, c, 1);
            g_text(x + 180, ry, side == T_SIDE ? "T" : "CT", c, 1);
            snprintf(buf, sizeof(buf), "%d", e->kills);  g_text(x + 250, ry, buf, c, 1);
            snprintf(buf, sizeof(buf), "%d", e->deaths); g_text(x + 290, ry, buf, c, 1);
            snprintf(buf, sizeof(buf), "%d", e->money);  g_text(x + 320, ry, buf, c, 1);
            row++;
        }
    }
}

/* A corner map, because a defusal map you cannot learn is a maze. Shows the
 * walls, the sites, you, and whichever team mates are alive -- never the
 * enemy, which would make the whole game trivial. */
static void draw_minimap(int cx, int cy) {
    const int px = 2, pad = 8;
    int x0 = cx + pad, y0 = cy + pad;
    g_blend(x0 - 2, y0 - 2, ST_W * px + 4, ST_H * px + 4, 0x000000, 160);
    /* One fill per run of same-coloured cells, not one per cell. A 48x48 grid
     * is 2,304 calls into the clipper every frame if you do it the obvious way,
     * and this map is mostly long stretches of rock and sand, so the runs take
     * it down to about three hundred. */
    for (int y = 0; y < ST_H; y++) {
        int run = 0;
        uint32_t runc = 0;
        for (int x = 0; x <= ST_W; x++) {
            uint32_t c = 0;
            if (x < ST_W) {
                if (st_cell[y][x] != C_OPEN)      c = 0x2A2A2E;
                else if (st_zone[y][x] == Z_SITE_A) c = 0x7A5A28;
                else if (st_zone[y][x] == Z_SITE_B) c = 0x2A5A58;
                else                                c = 0x585048;
            }
            if (run && (x == ST_W || c != runc)) {
                g_fill(x0 + (x - run) * px, y0 + y * px, run * px, px, runc);
                run = 0;
            }
            if (x < ST_W) { if (!run) runc = c; run++; }
        }
    }
    for (int i = 0; i < st_w.nent; i++) {
        st_ent *e = &st_w.ent[i];
        if (!e->used || !e->alive) continue;
        if (e->team != st_you()->team) continue;
        uint32_t c = (i == 0) ? 0xFFFFFF : 0x50C070;
        g_fill(x0 + (int)(e->x * px) - 1, y0 + (int)(e->y * px) - 1, 3, 3, c);
    }
    /* Planted is red and urgent; dropped is amber and only says "over here".
     * Both show to both sides -- the bomb is the round, and hiding where it is
     * makes a dropped one impossible to find in a map this size. */
    if (st_w.bomb_down || st_w.bomb_loose)
        g_fill(x0 + (int)(st_w.bomb_x * px) - 1, y0 + (int)(st_w.bomb_y * px) - 1, 3, 3,
               st_w.bomb_down ? 0xFF4040 : 0xE0A030);
}

/* -------------------------------------------------------------------------- */
static void strike_draw(window_t *win, int cx, int cy, int cw, int ch) {
    (void)win;
    if (failed) {
        g_fill(cx, cy, cw, ch, 0x101014);
        const char *m = "BoltStrike could not allocate its textures.";
        g_text(cx + (cw - g_text_width(m, 2)) / 2, cy + ch / 2 - 8, m, COL_BAD, 2);
        return;
    }
    if (!started) {
        g_fill(cx, cy, cw, ch, 0x101014);
        const char *m = "Loading BoltStrike...";
        g_text(cx + (cw - g_text_width(m, 2)) / 2, cy + ch / 2 - 8, m, COL_ACCENT, 2);
        return;
    }

    g_blit(cx, cy, cw, ch, st_frame, RW, RH);
    draw_minimap(cx, cy);
    draw_hud(cx, cy, cw, ch);
    if (buymenu)    draw_buymenu(cx, cy, cw, ch);
    if (scoreboard) draw_scoreboard(cx, cy, cw, ch);

    if (st_w.phase == R_OVER) {
        const char *m = st_w.winner == st_you()->team ? "ROUND WON" : "ROUND LOST";
        uint32_t c = st_w.winner == st_you()->team ? COL_GOOD : COL_BAD;
        g_text(cx + cw / 2 - g_text_width(m, 3) / 2, cy + ch / 2 - 60, m, c, 3);
    }
}

/* Clicking the client area takes the mouse. */
static void strike_click(window_t *win, int lx, int ly) {
    (void)win; (void)lx; (void)ly;
    if (!started || failed) return;
    if (buymenu) return;
    /* Throw away everything the hand did before it was ours. The driver
     * accumulates relative motion whether or not anybody is reading it, and
     * nobody reads it until the mouse is captured -- so the first frame of
     * play used to receive every pixel the pointer had travelled since the
     * desktop came up, in one go. You clicked to play and the camera snapped
     * to the sky and stayed there, which looked like the renderer was broken
     * and was really one unread accumulator. */
    int dx, dy;
    mouse_take_delta(&dx, &dy);
    captured = 1;
}

void strike_pump(void) {
    if (!g_win || failed) return;

    if (!strike_active()) {
        if (captured) { captured = 0; kbd_keys_clear(); }
        if (started) st_sfx_silence();
        return;
    }

    if (!started) {
        /* One frame of "loading" first, then build the world -- generating
         * every texture and sprite is not long, but it is long enough to
         * notice, and a black window looks like a crash. */
        static int announced;
        if (!announced) { announced = 1; gui_request_redraw(); return; }
        if (st_tex_init() != 0) { failed = 1; gui_request_redraw(); return; }
        st_sfx_init();                       /* no codec is not a failure      */
        st_game_init(nbots, diff);
        started = 1;
        last_frame = last_step = pit_ticks();
        gui_request_redraw();
        return;
    }

    uint64_t now = pit_ticks();
    int dt = (int)(now - last_step);

    /* The desktop loop wakes on every PIT tick and the PIT is at 1000 Hz, so
     * this is called a thousand times a second. Only the draw used to be
     * throttled, which meant the bots -- nine of them, each reading the map --
     * thought thirty times harder than anything on screen could show. Under an
     * emulator that was most of the machine. One simulation step per STEP_MS,
     * one draw per FRAME_MS, and the frame the mouse deltas arrive in is the
     * frame they are applied in either way, because the driver accumulates
     * them between reads. */
    if (dt < STEP_MS) return;
    last_step = now;
    if (dt > 150) dt = 150;

    do_input(dt);
    st_step(dt);
    watch(dt);
    if (feed_ms > 0) feed_ms -= dt;
    st_sfx_pump();

    if (captured) {
        /* Recenter. Done after the deltas were taken, so this costs nothing. */
        int wx = g_win->x + g_win->w / 2;
        int wy = g_win->y + g_win->h / 2;
        mouse_warp(wx, wy);
    }

    if (now - last_frame < FRAME_MS) return;
    last_frame = now;
    st_render(st_you(), pitch, zoomed);
    gui_request_redraw();
}

void strike_app_init(void) {
    window_t *win = gui_add_window("BoltStrike", RW * 2, RH * 2 + 32, 0xC8801A, ICON_STRIKE);
    if (!win) return;
    win->draw  = strike_draw;
    win->click = strike_click;
    win->min_w = 420;
    win->min_h = 300;
    win->x = 90; win->y = 30;
    g_win = win;

    kbd_raw_enable(1);     /* shared with DOOM; the held-key state is separate */
}
