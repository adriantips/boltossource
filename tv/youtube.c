/* ===========================================================================
 *  BoltOS TV  -  tv/youtube.c
 *
 *  YouTube, as an app: a search field and a few topics along the top, the
 *  videos under them in a grid of thumbnails, and a video full screen when
 *  one is chosen. Arrows move, OK plays, Back goes back; typing on a
 *  keyboard starts a search. In the player OK pauses, Left and Right jump ten
 *  seconds, Channel +/- a minute.
 *
 *  The videos themselves are tv/ytplay.c's: found, fetched and played by the
 *  TV, with no account and nothing to sign in to.
 * ===========================================================================*/
#include <stdint.h>
#include "tv.h"
#include "yt.h"
#include "image.h"
#include "kheap.h"
#include "string.h"
#include "stdio.h"
#include "kprintf.h"

static const char *const TOPICS[] = { "Music", "Nature 4K", "News", "Science", "Cooking", "Travel", "Comedy" };
#define NTOPICS ((int)(sizeof TOPICS / sizeof TOPICS[0]))
#define COLS 4

/* thumbnails, one per result, fetched a few at a time */
typedef struct { fetch_t *job; image_t *im; int tried; } thumb_t;
static thumb_t thumbs[YT_MAX_RESULTS];

static osk_t kb;
static char  query[256] = "Music";
static int   topic = 0;                /* the chip for the query, -1 a typed one */
static int   searching, searched;
static int   row = -1, col = 0;        /* row -1: the search field and the chips  */
static float scroll_y, scroll_to;
static uint64_t last_tick;
static glasscache_t gc_bar;

/* ---- thumbnails ---------------------------------------------------------- */
static void thumbs_free(void) {
    for (int i = 0; i < YT_MAX_RESULTS; i++) {
        if (thumbs[i].job) netsvc_cancel(thumbs[i].job);
        if (thumbs[i].im) image_free(thumbs[i].im);
    }
    memset(thumbs, 0, sizeof thumbs);
}

static void thumbs_tick(void) {
    int n, busy = 0;
    const yt_item *it = yt_results(&n);
    for (int i = 0; i < n; i++) {
        thumb_t *t = &thumbs[i];
        if (!t->job) continue;
        int st = t->job->state;
        if (st == FETCH_QUEUED || st == FETCH_RUNNING) { busy++; continue; }
        if (st == FETCH_DONE && t->job->len > 0) {
            t->im = image_decode((const uint8_t *)t->job->buf, (uint32_t)t->job->len);
            if (t->im) for (int k = 0; k < t->im->w * t->im->h; k++) t->im->px[k] |= 0xFF000000u;
            tv_redraw();
        }
        netsvc_cancel(t->job);           /* frees the job and its buffer */
        t->job = 0;
    }
    /* the visible rows first, three at a time */
    int first = row < 0 ? 0 : (row > 0 ? row - 1 : 0) * COLS;
    for (int i = first; i < n && busy < 3; i++) {
        thumb_t *t = &thumbs[i];
        if (t->tried) continue;
        fetch_t *f = kmalloc(sizeof *f);
        char *b = kmalloc(64 * 1024);
        if (!f || !b) { if (f) kfree(f); if (b) kfree(b); return; }
        memset(f, 0, sizeof *f);
        snprintf(f->url, sizeof f->url, "https://i.ytimg.com/vi/%s/mqdefault.jpg", it[i].id);
        f->buf = b; f->cap = 64 * 1024;
        if (netsvc_fetch(f) < 0) { kfree(f); kfree(b); return; }
        t->tried = 1; t->job = f; busy++;
    }
}

/* ---- search -------------------------------------------------------------- */
static void search(const char *q) {
    if (!q[0]) return;
    snprintf(query, sizeof query, "%s", q);
    thumbs_free();
    if (yt_search(q) == 0) { searching = 1; searched = 0; }
    row = -1; scroll_to = 0;
    tv_redraw();
}

static void yt_enter(void) {
    if (!searched && !searching) search(query);
    last_tick = tv_now_ms();
}

static void yt_leave(void) { if (tv_top() == &view_home) thumbs_free(); }

/* ---- geometry (design px) ------------------------------------------------ */
#define MX 96                       /* side margin                            */
#define TOP 92                      /* the search row's centre                */
#define GRID_Y 200                  /* the grid's top                         */
static int card_w(void) { return (1080 * 100 / UI_DENSITY * 16 / 9 - 2 * MX - (COLS - 1) * 36) / COLS; }
static int card_h(void) { return card_w() * 9 / 16 + 112; }
static int row_y(int r) { return GRID_Y + r * (card_h() + 36); }

static int nresults(void) { int n; yt_results(&n); return searching ? 0 : n; }

/* ---- keys ---------------------------------------------------------------- */
static void open_video(int i) {
    int n; const yt_item *it = yt_results(&n);
    if (i < 0 || i >= n) return;
    sfx_select();
    yt_open(it[i].id);
    tv_push(&view_ytplay);
}

static void yt_key(tvkey_t k) {
    if (kb.active) {
        int r = osk_key(&kb, k);
        if (r == 1 && kb.text[0]) { topic = -1; search(kb.text); }
        tv_redraw();
        return;
    }
    int n = nresults(), rows = (n + COLS - 1) / COLS;
    int oldr = row, oldc = col;
    switch (k.k) {
    case K_CHAR:                                  /* a keyboard: straight into search */
        osk_begin(&kb, "", "Search YouTube", "Search");
        { char s[2] = { (char)k.ch, 0 }; snprintf(kb.text, sizeof kb.text, "%s", s); kb.last_typed = 1; }
        return;
    case K_UP:
        if (row >= 0) { row--; if (row < 0) col = 0; }
        break;
    case K_DOWN:
        if (row < rows - 1) { row++; if (row == 0) col = 0; if (row * COLS + col >= n) col = (n - 1) % COLS; }
        break;
    case K_LEFT:  if (col > 0) col--; break;
    case K_RIGHT: if (row < 0 ? col < NTOPICS : (col < COLS - 1 && row * COLS + col + 1 < n)) col++; break;
    case K_OK:
        if (row >= 0) { open_video(row * COLS + col); return; }
        sfx_select();
        if (col == 0) osk_begin(&kb, topic < 0 ? query : "", "Search YouTube", "Search");
        else { topic = col - 1; search(TOPICS[topic]); }
        return;
    case K_BACK:
        sfx_back();
        if (row >= 0) { row = -1; col = 0; break; }
        tv_pop();
        return;
    case K_CHUP: if (row > 0) row--; break;
    case K_CHDN: if (row < rows - 1) { row++; if (row * COLS + col >= n) col = (n - 1) % COLS; } break;
    default: return;
    }
    if (row != oldr || col != oldc) sfx_move();
    if (row >= 0) {                                  /* keep the focused row in view */
        int top = row_y(row) - GRID_Y, bottom = row_y(row) + card_h() - (1080 * 100 / UI_DENSITY - 40);
        if (top < scroll_to) scroll_to = (float)top;
        if (bottom > scroll_to) scroll_to = (float)bottom;
    } else scroll_to = 0;
}

static void chip_geom(int i, int *x, int *w) {      /* i 0 = the search field */
    int cx = MX;
    for (int j = 0; j <= i; j++) {
        int ww = j == 0 ? 520 : font_width(FONT_MEDIUM, S(24), TOPICS[j - 1], -1) * 1080 / tv_h * 100 / UI_DENSITY + 56;
        if (j == i) { *x = cx; *w = ww; return; }
        cx += ww + 14;
    }
}

static void yt_pointer(int x, int y, int click, int wheel) {
    if (kb.active) return;
    if (wheel) { scroll_to -= (float)(wheel * 120); if (scroll_to < 0) scroll_to = 0; return; }
    for (int i = 0; i <= NTOPICS; i++) {
        int cx, cw; chip_geom(i, &cx, &cw);
        if (ui_hit(x, y, S(cx), S(TOP - 34), S(cw), S(68))) {
            row = -1; col = i;
            if (click) yt_key((tvkey_t){ K_OK, 0 });
            return;
        }
    }
    int n = nresults();
    for (int i = 0; i < n; i++) {
        int r = i / COLS, c = i % COLS;
        int cx = MX + c * (card_w() + 36), cy = row_y(r) - (int)scroll_y;
        if (ui_hit(x, y, S(cx), S(cy), S(card_w()), S(card_h()))) {
            row = r; col = c;
            if (click) open_video(i);
            return;
        }
    }
}

static void yt_tick_view(uint64_t now) {
    uint64_t dt = now - last_tick;
    last_tick = now;
    if (searching) {
        int r = yt_search_poll();
        if (r) { searching = 0; searched = 1; tv_redraw(); }
        else if (now % 50 < 17) tv_redraw();          /* the spinner */
    }
    thumbs_tick();
    float ns = ui_ease(scroll_y, scroll_to, dt, 220);
    if (ns != scroll_y) { scroll_y = ns; tv_redraw(); }
    if (kb.active && now % 530 < 17) tv_redraw();     /* the caret */
}

/* ---- drawing ------------------------------------------------------------- */
static void draw_card(int i, const yt_item *it, int x, int y, int a) {
    int w = S(card_w()), th = w * 9 / 16, on = row >= 0 && row * COLS + col == i;
    if (on) ui_focus(x - S(12), y - S(12), w + S(24), S(card_h()) + S(16), S(26), a);
    const image_t *im = thumbs[i].im;
    if (im) gfx_image_rr(x, y, w, th, im->px, im->w, im->h, S(16), a);
    else gfx_rrect(x, y, w, th, S(16), 0xFFFFFF, 22 * a / 255);
    if (it->badge[0]) {
        int bw = font_width(FONT_SEMIBOLD, S(18), it->badge, -1) + S(16);
        int live = !strcmp(it->badge, "LIVE");
        gfx_rrect(x + w - bw - S(10), y + th - S(40), bw, S(30), S(7), live ? 0xE62117 : 0x000000, (live ? 230 : 190) * a / 255);
        ui_text(FONT_SEMIBOLD, S(18), x + w - S(10) - bw / 2, y + th - S(25), it->badge, 0xFFFFFF, a, 1);
    }
    uint32_t ink = on ? 0x101318 : 0xFFFFFF, sub = on ? 0x4A5160 : 0xAEB5C2;
    ui_wrap(FONT_MEDIUM, S(23), x + S(4), y + th + S(14), w - S(8), S(30), 2, it->title, ink, a);
    char line[180];
    snprintf(line, sizeof line, "%s%s%s", it->channel, it->channel[0] && it->meta[0] ? "  \xB7  " : "", it->meta);
    font_draw_fit(FONT_REGULAR, S(19), x + S(4), y + th + S(92), line, w - S(8), sub, a);
}

static void yt_draw(uint64_t now) {
    int a = (int)(tv_view_t() * 255);
    ui_scrim_top(S(260), 120 * a / 255);

    /* the grid, under the bar */
    int n;
    const yt_item *it = yt_results(&n);
    if (searching) ui_spinner(tv_w / 2, tv_h / 2, S(30), now, 0xFFFFFF);
    else if (!n && searched)
        ui_text(FONT_MEDIUM, S(28), tv_w / 2, tv_h / 2, yt_search_error(), 0xD5DAE3, a, 1);
    else {
        gfx_clip_push(0, S(GRID_Y - 40), tv_w, tv_h - S(GRID_Y - 40));
        for (int i = 0; i < n; i++) {
            int r = i / COLS, c = i % COLS;
            int x = S(MX + c * (card_w() + 36)), y = S(row_y(r)) - S((int)scroll_y);
            if (y > tv_h || y + S(card_h()) < 0) continue;
            draw_card(i, &it[i], x, y, a);
        }
        gfx_clip_pop();
    }

    /* the bar: a name, the search field, the topics */
    for (int i = 0; i <= NTOPICS; i++) {
        int cx, cw; chip_geom(i, &cx, &cw);
        int x = S(cx), y = S(TOP - 34), w = S(cw), h = S(68);
        int on = row < 0 && col == i, chosen = i > 0 && topic == i - 1;
        if (i == 0) {
            if (on) ui_focus(x, y, w, h, h / 2, a);
            else ui_glass(&gc_bar, x, y, w, h, h / 2, 150, a);
            art_glyph(GL_SEARCH, x + S(40), y + h / 2, S(28), on ? 0x101318 : 0xFFFFFF, a);
            const char *t = topic < 0 ? query : "Search YouTube";
            ui_text(FONT_REGULAR, S(24), x + S(72), y + h / 2, t, on ? 0x101318 : topic < 0 ? 0xFFFFFF : 0xB8BFCC, a, 0);
        } else {
            if (on) ui_focus(x, y, w, h, h / 2, a);
            else gfx_rrect(x, y, w, h, h / 2, 0xFFFFFF, (chosen ? 70 : 26) * a / 255);
            ui_text(FONT_MEDIUM, S(24), x + w / 2, y + h / 2, TOPICS[i - 1], on ? 0x101318 : 0xFFFFFF, a, 1);
        }
    }
    if (kb.active) osk_draw(&kb, now, 255);
}

const view_t view_youtube = {
    .name = "youtube", .backdrop = BG_FILM_DIM,
    .enter = yt_enter, .leave = yt_leave, .key = yt_key, .pointer = yt_pointer,
    .draw = yt_draw, .tick = yt_tick_view,
};

/* ======================================================================== *
 *  The player
 * ======================================================================== */
static uint64_t osd_t0;                 /* the overlay's last wake            */
static int      last_pos_s = -1, last_state, last_buf;

static void osd(void) { osd_t0 = tv_now_ms(); tv_redraw(); }

static void play_enter(void) { osd(); last_pos_s = -1; }
static void play_leave(void) { yt_close(); }

static void play_key(tvkey_t k) {
    switch (k.k) {
    case K_OK:    yt_toggle(); sfx_select(); break;
    case K_LEFT:  yt_seek(-10); break;
    case K_RIGHT: yt_seek(10); break;
    case K_CHDN:  yt_seek(-60); break;
    case K_CHUP:  yt_seek(60); break;
    case K_BACK:  sfx_back(); tv_pop(); return;
    case K_CHAR:  if (k.ch == ' ') yt_toggle(); break;
    default: break;
    }
    osd();
}

static void play_pointer(int x, int y, int click, int wheel) {
    (void)x; (void)y; (void)wheel;
    if (click) yt_toggle();
    osd();
}

static void play_tick(uint64_t now) {
    if (yt_tick(now)) tv_redraw();
    int st = yt_state(), b = yt_buffering(), s = yt_pos_ms() / 1000;
    if (st != last_state || b != last_buf || (s != last_pos_s && now - osd_t0 < 4500)) {
        last_state = st; last_buf = b; last_pos_s = s;
        tv_redraw();
    }
    if (b && now % 40 < 17) tv_redraw();                /* the spinner */
    if (now - osd_t0 >= 4000 && now - osd_t0 < 4600) tv_redraw();   /* the fade */
}

static void fmt_clock(char *o, int cap, int ms) {
    int s = ms / 1000;
    if (s >= 3600) snprintf(o, (size_t)cap, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(o, (size_t)cap, "%d:%02d", s / 60, s % 60);
}

/* The frame, fitted to the screen (nearest pixel: it is going up, not down). */
static void draw_picture(const surface_t *p) {
    int w = tv_w, h = tv_w * p->h / p->w;
    if (h > tv_h) { h = tv_h; w = tv_h * p->w / p->h; }
    int x0 = (tv_w - w) / 2, y0 = (tv_h - h) / 2;
    static int colmap[4096];
    for (int i = 0; i < w && i < 4096; i++) colmap[i] = i * p->w / w;
    surface_t *t = gfx_target(0, 0);
    for (int j = 0; j < h; j++) {
        const uint32_t *s = p->px + (j * p->h / h) * p->stride;
        uint32_t *d = t->px + (int64_t)(y0 + j) * t->stride + x0;
        for (int i = 0; i < w; i++) d[i] = s[colmap[i]];
    }
}

static void play_draw(uint64_t now) {
    gfx_fill(0, 0, tv_w, tv_h, 0x000000);
    const surface_t *p = yt_picture();
    if (p) draw_picture(p);
    int st = yt_state();
    if (st == YT_ERROR) {
        ui_text(FONT_SEMIBOLD, S(34), tv_w / 2, tv_h / 2 - S(24), "This video can't play", 0xFFFFFF, 255, 1);
        ui_wrap(FONT_REGULAR, S(24), tv_w / 2 - S(500), tv_h / 2 + S(14), S(1000), S(34), 3, yt_message(), 0xB8BFCC, 255);
        return;
    }
    if (yt_buffering()) ui_spinner(tv_w / 2, tv_h / 2, S(34), now, 0xFFFFFF);

    /* the overlay: the title at the top, the time along the bottom */
    uint64_t since = now - osd_t0;
    int a = st == YT_PAUSED || st == YT_LOADING ? 255 : since < 4000 ? 255 : since < 4600 ? (int)(255 * (4600 - since) / 600) : 0;
    if (a <= 0) return;
    ui_scrim_top(S(220), 150 * a / 255);
    ui_scrim_bottom(S(240), 170 * a / 255);
    ui_text_shadow(FONT_SEMIBOLD, S(36), S(MX), S(80), yt_title()[0] ? yt_title() : "YouTube", 0xFFFFFF, a, 0);
    ui_text_shadow(FONT_REGULAR, S(22), S(MX), S(124), yt_author(), 0xD5DAE3, a, 0);
    int len = yt_len_ms(), pos = yt_pos_ms();
    int bx = S(MX), bw = tv_w - 2 * S(MX), by = tv_h - S(96);
    gfx_rrect(bx, by, bw, S(8), S(4), 0xFFFFFF, 50 * a / 255);
    if (len > 0) {
        int lw = (int)((int64_t)bw * yt_loaded_ms() / len), pw = (int)((int64_t)bw * pos / len);
        if (lw > bw) lw = bw;
        if (lw > 0) gfx_rrect(bx, by, lw, S(8), S(4), 0xFFFFFF, 70 * a / 255);
        if (pw > 0) gfx_rrect(bx, by, pw, S(8), S(4), 0xE62117, a);
        gfx_circle((float)(bx + pw), (float)(by + S(4)), (float)S(10), 0xFFFFFF, a);
    }
    char t1[24], t2[24], line[64];
    fmt_clock(t1, sizeof t1, pos); fmt_clock(t2, sizeof t2, len);
    snprintf(line, sizeof line, "%s / %s", t1, t2);
    ui_text(FONT_MEDIUM, S(22), bx, by + S(44), line, 0xFFFFFF, a, 0);
    const char *m = yt_message();
    if (m[0]) ui_text(FONT_MEDIUM, S(22), bx + bw, by + S(44), m, 0xFFFFFF, a, 2);
    else if (st == YT_PLAYING && !yt_has_sound())
        ui_text(FONT_REGULAR, S(20), bx + bw, by + S(44), "No sound device", 0xB8BFCC, a, 2);
    if (st == YT_PAUSED) art_glyph(GL_PLAY, tv_w / 2, tv_h / 2, S(96), 0xFFFFFF, 220);
}

const view_t view_ytplay = {
    .name = "ytplay", .backdrop = BG_NONE,
    .enter = play_enter, .leave = play_leave, .key = play_key, .pointer = play_pointer,
    .draw = play_draw, .tick = play_tick,
};
