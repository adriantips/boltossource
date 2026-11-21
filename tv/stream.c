/* ===========================================================================
 *  BoltOS TV  -  tv/stream.c
 *
 *  Netflix, Prime Video, Disney+ and HBO Max. Each is its own website,
 *  played in a real Firefox on a computer on the network (tv/engine.c has
 *  why, webengine/ is that Firefox), and shown here full screen: the page
 *  at 1280x720, which is the most any of them gives Firefox on Linux anyway,
 *  drawn up to the panel.
 *
 *  A remote has no pointer, so it brings one: the arrows move it -- faster
 *  the longer they're held -- and OK clicks. Channel +/- scroll. Back goes
 *  back a page, and out of the app from its first. Two keys change what the
 *  rest do: Info ('i') turns the arrows and OK into arrow keys and Enter, for
 *  a player's own keys and menus; Tab hands a keyboard to the page, for
 *  typing a search or a password, until Esc. A mouse just works.
 * ===========================================================================*/
#include <stdint.h>
#include "tv.h"
#include "engine.h"
#include "string.h"
#include "stdio.h"

#define PAGE_W 1280
#define PAGE_H 720

static const struct { const char *name, *url, *line; int tile; } SVC[SVC_COUNT] = {
    { "Netflix",     "https://www.netflix.com/browse",  "Films and series, in Firefox on your engine", ART_NETFLIX },
    { "Prime Video", "https://www.primevideo.com/",     "Amazon's films and series",                 ART_PRIME },
    { "Disney+",     "https://www.disneyplus.com/",     "Disney, Pixar, Marvel and Star Wars",       ART_DISNEY },
    { "HBO Max",     "https://play.hbomax.com/",        "HBO's series and Warner Bros. films",       ART_HBOMAX },
};

static int svc;
static int px = PAGE_W / 2, py = PAGE_H / 2;        /* the pointer, page pixels   */
static int keys_mode, typing;
static uint64_t last_arrow, hint_t0, last_draw;
static int speed;
static osk_t kb;
static glasscache_t gc_card, gc_hint;

void stream_select(int s) { svc = s >= 0 && s < SVC_COUNT ? s : 0; }

static void hint(void) { hint_t0 = tv_now_ms(); tv_redraw(); }

static void connect(void) {
    if (!engine_configured()) return;
    engine_start(SVC[svc].url, PAGE_W, PAGE_H);
}

static void stream_enter(void) {
    if (tv_top() != &view_stream) return;
    keys_mode = typing = 0;
    px = PAGE_W / 2; py = PAGE_H / 2;
    connect();
    hint();
}

static void stream_leave(void) {
    if (typing) tv_text_mode(0);
    typing = 0;
    kb.active = 0;
    engine_stop();
}

/* ---- keys ---------------------------------------------------------------- */
static uint32_t ch_keysym(int c) { return (unsigned)c < 0x100 ? (uint32_t)(unsigned char)c : 0; }

static void move(int dx, int dy) {
    uint64_t now = tv_now_ms();
    speed = now - last_arrow < 160 ? (speed < 64 ? speed + 6 : 64) : 14;   /* held: it gathers pace */
    last_arrow = now;
    px += dx * speed; py += dy * speed;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= PAGE_W) px = PAGE_W - 1;
    if (py >= PAGE_H) py = PAGE_H - 1;
    engine_mouse(px, py, 0);
}

static void click(void) { engine_mouse(px, py, 1); engine_mouse(px, py, 0); }

static void stream_key(tvkey_t k) {
    if (kb.active) {                                 /* the engine's address */
        int r = osk_key(&kb, k);
        if (r == 1) { engine_set_address(kb.text); connect(); }
        tv_redraw();
        return;
    }
    int st = engine_state();
    if (st != ENG_LIVE) {
        if (k.k == K_BACK) { sfx_back(); tv_pop(); }
        else if (k.k == K_OK) {
            sfx_select();
            if (!engine_configured() || st == ENG_ERROR) {
                char a[96]; engine_get_address(a, sizeof a);
                osk_begin(&kb, a, "Engine address, like 192.168.1.20", "Connect");
            }
        }
        return;
    }
    if (typing) {                                    /* the keyboard is the page's */
        switch (k.k) {
        case K_BACK:  typing = 0; tv_text_mode(0); hint(); break;
        case K_CHAR:  engine_keysym(ch_keysym(k.ch), 0); break;
        case K_ERASE: engine_keysym(0xFF08, 0); break;
        case K_OK:    engine_keysym(0xFF0D, 0); break;
        case K_TAB:   engine_keysym(0xFF09, 0); break;
        case K_UP:    engine_keysym(0xFF52, 0); break;
        case K_DOWN:  engine_keysym(0xFF54, 0); break;
        case K_LEFT:  engine_keysym(0xFF51, 0); break;
        case K_RIGHT: engine_keysym(0xFF53, 0); break;
        default: break;
        }
        return;
    }
    switch (k.k) {
    case K_UP:    if (keys_mode) engine_keysym(0xFF52, 0); else move(0, -1); break;
    case K_DOWN:  if (keys_mode) engine_keysym(0xFF54, 0); else move(0, 1); break;
    case K_LEFT:  if (keys_mode) engine_keysym(0xFF51, 0); else move(-1, 0); break;
    case K_RIGHT: if (keys_mode) engine_keysym(0xFF53, 0); else move(1, 0); break;
    case K_OK:    if (keys_mode) engine_keysym(0xFF0D, 0); else click(); break;
    case K_CHUP:  engine_wheel(3); break;
    case K_CHDN:  engine_wheel(-3); break;
    case K_INFO:  keys_mode = !keys_mode; sfx_select(); hint(); break;
    case K_TAB:   typing = 1; tv_text_mode(1); sfx_select(); hint(); break;
    case K_CHAR:  engine_keysym(ch_keysym(k.ch), 0); break;
    case K_BACK: {
        int back;
        engine_page(0, 0, 0, 0, 0, &back, 0);
        sfx_back();
        if (back) engine_back(); else tv_pop();
        break;
    }
    default: break;
    }
}

static void stream_pointer(int x, int y, int clk, int wheel) {
    if (engine_state() != ENG_LIVE || kb.active) {
        if (clk) stream_key((tvkey_t){ K_OK, 0 });
        return;
    }
    px = x * PAGE_W / tv_w; py = y * PAGE_H / tv_h;
    engine_mouse(px, py, 0);
    if (clk) click();
    if (wheel) engine_wheel(wheel > 0 ? 3 : -3);
}

static void stream_tick(uint64_t now) {
    if (engine_tick()) tv_redraw();
    if (engine_state() != ENG_LIVE && now - last_draw > 40) tv_redraw();     /* the spinner */
    if (now - hint_t0 > 5000 && now - hint_t0 < 5600) tv_redraw();
}

/* ---- drawing ------------------------------------------------------------- */
static void draw_page(const surface_t *p) {
    static int colmap[4096];
    for (int i = 0; i < tv_w && i < 4096; i++) colmap[i] = i * p->w / tv_w;
    surface_t *t = gfx_target(0, 0);
    for (int j = 0; j < tv_h; j++) {
        const uint32_t *s = p->px + (j * p->h / tv_h) * p->stride;
        uint32_t *d = t->px + (int64_t)j * t->stride;
        for (int i = 0; i < tv_w; i++) d[i] = s[colmap[i]];
    }
}

static void draw_card(uint64_t now) {
    int st = engine_state();
    int w = S(900), h = S(440), x = (tv_w - w) / 2, y = (tv_h - h) / 2 - S(30);
    ui_glass(&gc_card, x, y, w, h, S(44), 130, 255);
    art_tile(SVC[svc].tile, x + (w - S(112)) / 2, y + S(44), S(112), 255);
    ui_text(FONT_SEMIBOLD, S(46), x + w / 2, y + S(206), SVC[svc].name, 0xFFFFFF, 255, 1);
    const char *msg, *act = 0;
    char a[96];
    engine_get_address(a, sizeof a);
    if (!engine_configured()) {
        msg = "This plays in Firefox on a computer on your network running the BoltOS Web Engine "
              "(see TV.md). Tell the TV where it is and it connects from then on.";
        act = "OK: enter the engine's address";
    } else if (st == ENG_ERROR) {
        msg = engine_message();
        act = "OK: change the address  \xB7  Back: Home";
    } else {
        msg = engine_message()[0] ? engine_message() : "Connecting\x85";
        ui_spinner(x + w / 2, y + h - S(56), S(20), now, 0xFFFFFF);
    }
    ui_wrap(FONT_REGULAR, S(23), x + S(70), y + S(250), w - S(140), S(32), 3, msg, 0xC9CED8, 255);
    if (act) ui_text(FONT_MEDIUM, S(22), x + w / 2, y + h - S(52), act, 0xFFFFFF, 220, 1);
}

static void draw_hint(uint64_t now) {
    uint64_t since = now - hint_t0;
    if (since > 5600 && !typing) return;
    int a = typing || since < 5000 ? 255 : (int)(255 * (5600 - since) / 600);
    const char *t = typing ? "The keyboard types into the page  \xB7  Esc: stop" :
                    keys_mode ? "Arrows and OK go to the page as keys  \xB7  i: pointer" :
                    "Arrows move the pointer  \xB7  OK clicks  \xB7  i: arrow keys  \xB7  Tab: type";
    int w = font_width(FONT_MEDIUM, S(21), t, -1) + S(56), h = S(56);
    int x = (tv_w - w) / 2, y = tv_h - h - S(36);
    ui_glass(&gc_hint, x, y, w, h, h / 2, 175, a);
    ui_text(FONT_MEDIUM, S(21), tv_w / 2, y + h / 2, t, 0xFFFFFF, a, 1);
}

static void draw_pointer(void) {
    int cw, ch, hx, hy;
    const uint32_t *c = engine_cursor(&cw, &ch, &hx, &hy);
    int x = px * tv_w / PAGE_W, y = py * tv_h / PAGE_H;
    int k = tv_w * 100 / PAGE_W;                          /* the page's scale, percent */
    if (c) gfx_image(x - hx * k / 100, y - hy * k / 100, cw * k / 100, ch * k / 100, c, cw, ch, 255);
    else { gfx_circle((float)x, (float)y, (float)S(12), 0xFFFFFF, 235); gfx_ring((float)x, (float)y, (float)S(12), 2.0f, 0, 70); }
}

static void stream_draw(uint64_t now) {
    last_draw = now;
    const surface_t *p = engine_picture();
    if (p) {
        draw_page(p);
        if (!keys_mode && !typing && !tv_pointer_visible()) draw_pointer();
        draw_hint(now);
    } else {
        gfx_fill(0, 0, tv_w, tv_h, 0x05070B);
        ui_glow(tv_w / 2, tv_h / 2, S(700), S(400), 0x3B2A80, 110);
        if (!kb.active) draw_card(now);
    }
    if (kb.active) osk_draw(&kb, now, 255);
}

const view_t view_stream = {
    .name = "stream", .backdrop = BG_NONE,
    .enter = stream_enter, .leave = stream_leave, .key = stream_key, .pointer = stream_pointer,
    .draw = stream_draw, .tick = stream_tick,
};
