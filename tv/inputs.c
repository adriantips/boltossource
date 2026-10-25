/* ===========================================================================
 *  BoltOS TV  -  tv/inputs.c
 *
 *  The inputs: HDMI 1, and the Apple TV on HDMI 2. Choosing one is what a
 *  television does when you press Input -- the menus go, and you get either
 *  the picture from that socket or the card every set shows when nothing is
 *  coming in.
 *
 *  BoltOS TV runs on a computer plugged into the television, and a computer
 *  has HDMI *outputs*. Nothing in this kernel can receive a picture, so on
 *  this hardware these screens always show "No signal", and they say so
 *  honestly rather than pretending: the hint tells you to use the TV's own
 *  Input button for a box plugged into the TV. The card drifts slowly after a
 *  few seconds, as real sets do, so it never burns into an OLED.
 * ===========================================================================*/
#include <stdint.h>
#include "tv.h"
#include "string.h"
#include "stdio.h"
#include "math.h"
#include "kheap.h"

static int which;                      /* 0 HDMI 1, 1 Apple TV (HDMI 2) */
static int sel;                        /* switcher focus                */
static uint64_t t0, last_draw;
static glasscache_t gc_card, gc_bar;

static const struct { const char *name, *port, *hint; int tile; } IN[2] = {
    { "HDMI 1",   "HDMI 1",
      "Nothing is coming in on HDMI 1. Check that the device is switched on and its cable is in "
      "all the way. If it is plugged into the television itself, use the Input button on the TV's "
      "own remote to switch to it.", ART_HDMI },
    { "Apple TV", "HDMI 2",
      "Nothing is coming in from the Apple TV. Wake it with its remote. If it is plugged into the "
      "television itself rather than into this box, press Input on the TV's own remote and choose "
      "its HDMI port.", ART_APPLETV },
};

/* the switcher along the bottom */
static const char *const SW[] = { "Live TV", "Apple TV", "HDMI 1" };
static int sw_input(int i) { return i == 1 ? 1 : i == 2 ? 0 : -1; }

void input_select(int w) {
    which = w ? 1 : 0;
    sel = which ? 1 : 2;
    t0 = tv_now_ms();
}

static void inputs_enter(void) { t0 = tv_now_ms(); }

static void go(int i) {
    int inp = sw_input(i);
    sfx_select();
    if (inp < 0) { tv_pop(); tv_push(&view_live); return; }
    input_select(inp);
    tv_redraw();
}

static void inputs_key(tvkey_t k) {
    switch (k.k) {
    case K_LEFT:  if (sel > 0) { sel--; sfx_move(); } break;
    case K_RIGHT: if (sel < 2) { sel++; sfx_move(); } break;
    case K_OK:    go(sel); break;
    case K_BACK:  sfx_back(); tv_pop(); break;
    case K_CHUP: case K_CHDN: tv_pop(); tv_push(&view_live); break;
    }
}

static void bar_geom(int *x, int *y, int *w, int *h) {
    *w = S(660); *h = S(84); *x = (tv_w - *w) / 2; *y = tv_h - S(72) - *h;
}

static void inputs_pointer(int x, int y, int click, int wheel) {
    (void)wheel;
    int bx, by, bw, bh;
    bar_geom(&bx, &by, &bw, &bh);
    for (int i = 0; i < 3; i++) {
        int cw = bw / 3;
        if (ui_hit(x, y, bx + i * cw, by, cw, bh)) { sel = i; if (click) go(i); }
    }
}

static void inputs_tick(uint64_t now) {
    if (now - last_draw >= 66) tv_redraw();      /* a slow drift: 15 fps is plenty */
}

static void glow(int cx, int cy, int r, uint32_t c, int a) { ui_glow(cx, cy, r, r, c, a); }

static void inputs_draw(uint64_t now) {
    last_draw = now;
    float t = (float)(now - t0) / 1000.0f;
    gfx_fill(0, 0, tv_w, tv_h, 0x030406);
    /* two slow lights behind the card, so the glass has something to frost */
    glow(tv_w / 2 + (int)(sinf(t * 0.21f) * S(380)), tv_h / 2 + (int)(cosf(t * 0.17f) * S(160)),
         S(560), which ? 0x6A5CFF : 0x2F8CFF, 150);
    glow(tv_w / 2 + (int)(cosf(t * 0.13f) * S(420)), tv_h / 2 + (int)(sinf(t * 0.19f) * S(200)),
         S(480), which ? 0xFF4F7B : 0x22C3B0, 120);

    /* after eight seconds the card starts to wander, as on any set */
    float wander = t > 8.0f ? (t - 8.0f > 3.0f ? 1.0f : (t - 8.0f) / 3.0f) : 0.0f;
    int dx = (int)(sinf(t * 0.11f) * S(220) * wander), dy = (int)(sinf(t * 0.07f + 1.3f) * S(120) * wander);
    int w = S(860), h = S(500);
    int x = (tv_w - w) / 2 + dx, y = (tv_h - h) / 2 - S(40) + dy;
    float in = t < 0.35f ? t / 0.35f : 1.0f;
    int a = (int)(255 * in);
    ui_glass(&gc_card, x, y, w, h, S(44), 120, a);
    art_tile(IN[which].tile, x + (w - S(120)) / 2, y + S(48), S(120), a);
    ui_text(FONT_SEMIBOLD, S(52), x + w / 2, y + S(222), IN[which].name, 0xFFFFFF, a, 1);
    if (which) {
        int pw2 = font_width(FONT_SEMIBOLD, S(20), IN[which].port, -1) + S(26);
        gfx_rrect(x + (w - pw2) / 2, y + S(258), pw2, S(34), S(9), 0xFFFFFF, a * 40 / 255);
        ui_text(FONT_SEMIBOLD, S(20), x + w / 2, y + S(275), IN[which].port, 0xE6E9F0, a, 1);
    }
    /* status: a breathing amber dot and "No signal" */
    int pulse = 190 + (int)(sinf(t * 3.2f) * 65);
    int sy = y + S(330);
    int sw = font_width(FONT_MEDIUM, S(30), "No signal", -1);
    int sx = x + (w - sw - S(30)) / 2;
    gfx_circle((float)(sx + S(8)), (float)sy, (float)S(9), 0xFFC233, pulse * a / 255);
    gfx_circle((float)(sx + S(8)), (float)sy, (float)S(18), 0xFFC233, pulse * a / 255 / 4);
    ui_text(FONT_MEDIUM, S(30), sx + S(30), sy, "No signal", 0xFFFFFF, a, 0);
    ui_wrap(FONT_REGULAR, S(21), x + S(70), y + S(368), w - S(140), S(30), 3, IN[which].hint, 0xAEB5C2, a);

    /* the input switcher */
    int bx, by, bw, bh;
    bar_geom(&bx, &by, &bw, &bh);
    ui_glass(&gc_bar, bx, by, bw, bh, bh / 2, 150, a);
    int cw = bw / 3;
    ui_focus(bx + sel * cw + S(8), by + S(8), cw - S(16), bh - S(16), (bh - S(16)) / 2, a);
    for (int i = 0; i < 3; i++) {
        int active = sw_input(i) == which;
        uint32_t ink = i == sel ? 0x101318 : 0xFFFFFF;
        int tw = ui_text(FONT_SEMIBOLD, S(24), bx + i * cw + cw / 2, by + bh / 2, SW[i], ink, a, 1);
        if (active) gfx_circle((float)(bx + i * cw + cw / 2 - tw / 2 - S(16)), (float)(by + bh / 2),
                               (float)S(5), i == sel ? 0x101318 : 0x34C759, a);
    }
}

const view_t view_input = {
    .name = "input", .backdrop = BG_NONE,
    .enter = inputs_enter, .key = inputs_key, .pointer = inputs_pointer,
    .draw = inputs_draw, .tick = inputs_tick,
};
