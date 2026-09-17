/* ===========================================================================
 *  BoltOS  -  kernel/app_solitaire.c
 *  Klondike. The one that shipped with every copy of Windows for thirty years
 *  and taught an entire generation to use a mouse.
 *
 *  Two decisions worth writing down.
 *
 *  Click to pick up, click to put down -- not drag and drop. The window
 *  manager hands an app `click` and `drag`, and nothing at all when the button
 *  comes back up (see window_t in gui.h). A drop needs a release, so a
 *  drag-and-drop version would have to guess at one from the tick, and a
 *  solitaire that sometimes drops a card in the wrong pile is worse than one
 *  that never tries. Double-click still does the obvious thing and flies a
 *  card to its foundation.
 *
 *  The whole game state is one struct of bytes and the draw is a pure function
 *  of it. That is not tidiness for its own sake: undo is then a memcpy, and
 *  undo is the difference between this and a cruel game, because Klondike
 *  deals you unwinnable hands and turning a card you should not have turned is
 *  how most of them are lost.
 * ===========================================================================*/
#include <stdint.h>
#include <stdio.h>
#include "gui.h"
#include "hw.h"           /* rtc_now, for the shuffle seed */
#include "pit.h"
#include "keyboard.h"
#include "notify.h"
#include "string.h"

/* A card is suit*13 + rank, rank 0 = ace. Suits are ordered clubs, diamonds,
 * hearts, spades, so (suit & 1) is not the colour -- red is 1 and 2. */
enum { CLUB = 0, DIAMOND, HEART, SPADE };
#define SUIT(c)  ((c) / 13)
#define RANK(c)  ((c) % 13)
#define RED(c)   (SUIT(c) == DIAMOND || SUIT(c) == HEART)
#define NOCARD   0xFF

#define TABLEAUS 7
#define MAXPILE  24

typedef struct {
    uint8_t stock[52],  nstock;
    uint8_t waste[52],  nwaste;
    uint8_t found[4][13], nfound[4];
    uint8_t tab[TABLEAUS][MAXPILE], ntab[TABLEAUS], up[TABLEAUS];  /* up = face-up count */
    uint16_t moves;
    uint8_t  won;
} game_t;

/* Where a click landed and, when something is held, where it came from. */
enum { SRC_NONE = 0, SRC_WASTE, SRC_TAB, SRC_FOUND };

static struct {
    game_t   g;
    game_t   undo;
    int      have_undo;
    int      held_src, held_pile, held_idx;   /* held_idx: first card of the run */
    int      draw3;
    uint64_t start_tick, end_tick;
    /* double-click detection: the WM only reports single clicks */
    uint64_t last_click;
    int      last_src, last_pile, last_idx;
    /* layout from the last draw, for hit-testing */
    int      cw, ch, cardw, cardh, ox, oy, gap, fanv;
    char     status[64];
} S;

static window_t *win;

/* ------------------------------------------------------------------ rng --- */
/* xorshift32. The seed is the clock plus the tick count, because the RTC has
 * one-second resolution and two games started in the same second should not be
 * the same game. */
static uint32_t rng_state = 2463534242u;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static void seed_rng(void) {
    struct rtc_time t;
    rtc_now(&t);
    uint32_t s = (uint32_t)pit_ticks();
    s ^= ((uint32_t)t.hour << 16) ^ ((uint32_t)t.min << 8) ^ t.sec ^ ((uint32_t)t.day << 24);
    rng_state = s ? s : 1;
    for (int i = 0; i < 8; i++) rnd();       /* xorshift starts poorly */
}

/* --------------------------------------------------------------- rules ---- */
static int can_stack_tab(uint8_t card, uint8_t onto) {
    if (onto == NOCARD) return RANK(card) == 12;             /* only a king */
    return RED(card) != RED(onto) && RANK(card) + 1 == RANK(onto);
}
/* Foundation f belongs to suit f. It does not have to -- any ace could claim
 * any empty one and the game plays identically -- but the empty ones are drawn
 * with a ghost pip, and a hint that tells you where the hearts go had better be
 * telling the truth. The first version let the first ace take slot 0 and the
 * pips underneath were decoration. */
static int can_stack_found(uint8_t card, int f, const game_t *g) {
    if (SUIT(card) != f) return 0;
    if (g->nfound[f] == 0) return RANK(card) == 0;
    return RANK(card) == RANK(g->found[f][g->nfound[f] - 1]) + 1;
}

static void snapshot(void) { S.undo = S.g; S.have_undo = 1; }

static void check_win(void) {
    for (int f = 0; f < 4; f++) if (S.g.nfound[f] != 13) return;
    S.g.won = 1;
    S.end_tick = pit_ticks();
    gui_notify(NOTE_SUCCESS, "Solitaire", "You win.");
}

static void deal(void) {
    memset(&S.g, 0, sizeof S.g);
    S.held_src = SRC_NONE;
    S.have_undo = 0;

    uint8_t deck[52];
    for (int i = 0; i < 52; i++) deck[i] = (uint8_t)i;
    for (int i = 51; i > 0; i--) {                    /* Fisher-Yates */
        uint32_t j = rnd() % (uint32_t)(i + 1);
        uint8_t t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }

    int k = 0;
    for (int c = 0; c < TABLEAUS; c++) {
        for (int r = 0; r <= c; r++) S.g.tab[c][r] = deck[k++];
        S.g.ntab[c] = (uint8_t)(c + 1);
        S.g.up[c]   = 1;
    }
    while (k < 52) S.g.stock[S.g.nstock++] = deck[k++];

    S.start_tick = pit_ticks();
    S.end_tick = 0;
    strlcpy(S.status, "Ace to the top, king to an empty column.", sizeof S.status);
}

/* Turn the stock over, honouring draw-1 / draw-3. An empty stock recycles the
 * waste -- unlimited passes, which is the version everybody actually plays. */
static void draw_stock(void) {
    snapshot();
    if (S.g.nstock == 0) {
        if (S.g.nwaste == 0) return;
        while (S.g.nwaste) S.g.stock[S.g.nstock++] = S.g.waste[--S.g.nwaste];
        S.g.moves++;
        return;
    }
    int n = S.draw3 ? 3 : 1;
    while (n-- && S.g.nstock) S.g.waste[S.g.nwaste++] = S.g.stock[--S.g.nstock];
    S.g.moves++;
}

/* Take `count` cards off the top of a source. The caller has already checked
 * the move is legal; this only moves bytes and turns the card underneath. */
static void lift(int src, int pile, int idx, uint8_t *out, int *count) {
    if (src == SRC_WASTE) { out[0] = S.g.waste[--S.g.nwaste]; *count = 1; return; }
    if (src == SRC_FOUND) { out[0] = S.g.found[pile][--S.g.nfound[pile]]; *count = 1; return; }
    int n = S.g.ntab[pile] - idx;
    for (int i = 0; i < n; i++) out[i] = S.g.tab[pile][idx + i];
    S.g.ntab[pile] = (uint8_t)idx;
    int left = (int)S.g.up[pile] - n;
    S.g.up[pile] = (uint8_t)(left < 0 ? 0 : left);
    /* Taking the last face-up card turns the one underneath. This is the move
     * the whole game turns on and it is one line, which is why it was wrong
     * twice: the count is of face-up cards, not of cards. */
    if (S.g.ntab[pile] && !S.g.up[pile]) S.g.up[pile] = 1;
    *count = n;
}

static int try_to_foundation(int src, int pile, int idx) {
    uint8_t card;
    if (src == SRC_WASTE) { if (!S.g.nwaste) return 0; card = S.g.waste[S.g.nwaste - 1]; }
    else if (src == SRC_TAB) {
        if (!S.g.ntab[pile] || idx != S.g.ntab[pile] - 1) return 0;   /* only the top card */
        card = S.g.tab[pile][idx];
    } else return 0;

    for (int f = 0; f < 4; f++) {
        if (!can_stack_found(card, f, &S.g)) continue;
        snapshot();
        uint8_t buf[MAXPILE]; int n;
        lift(src, pile, idx, buf, &n);
        S.g.found[f][S.g.nfound[f]++] = buf[0];
        S.g.moves++;
        check_win();
        return 1;
    }
    return 0;
}

/* Every face-up card is on a foundation-ready run: finish the game. Windows
 * pops a dialog; this just does it, one card per tick so you can watch. */
static int auto_finishable(void) {
    if (S.g.nstock || S.g.nwaste) return 0;
    for (int c = 0; c < TABLEAUS; c++) if (S.g.up[c] != S.g.ntab[c]) return 0;
    return 1;
}
static void auto_step(void) {
    for (int c = 0; c < TABLEAUS; c++) {
        if (!S.g.ntab[c]) continue;
        if (try_to_foundation(SRC_TAB, c, S.g.ntab[c] - 1)) return;
    }
}

/* ------------------------------------------------------------- drawing ---- */
/* Suit pips. Drawn rather than typed: the font has no card suits, and an
 * 'S'/'H'/'D'/'C' next to the rank reads as a two-letter word, not a card. */
static void tri(int x, int y, int w, int h, int down, uint32_t col) {
    for (int r = 0; r < h; r++) {
        int half = down ? (w * (h - r)) / (2 * h) : (w * r) / (2 * h);
        if (half < 1) continue;
        g_fill(x + w / 2 - half, y + r, half * 2, 1, col);
    }
}
static void pip(int x, int y, int s, int suit, uint32_t col) {
    int h = s, w = s;
    switch (suit) {
    case DIAMOND:
        tri(x, y, w, h / 2, 0, col);
        tri(x, y + h / 2, w, h - h / 2, 1, col);
        break;
    case HEART: {
        int r = w / 4;
        g_round(x, y + h / 6, 2 * r, 2 * r, r, col, 255);
        g_round(x + w - 2 * r, y + h / 6, 2 * r, 2 * r, r, col, 255);
        g_fill(x, y + h / 6 + r, w, 1, col);
        tri(x, y + h / 6 + r, w, h - h / 6 - r, 1, col);
        break;
    }
    case SPADE:
        tri(x, y, w, (h * 2) / 3, 0, col);
        { int r = w / 4;
          g_round(x, y + h / 2, 2 * r, 2 * r, r, col, 255);
          g_round(x + w - 2 * r, y + h / 2, 2 * r, 2 * r, r, col, 255); }
        g_fill(x + w / 2 - 1, y + h - h / 4, 3, h / 4, col);
        break;
    default: {                                        /* club */
        int r = w / 4;
        g_round(x + w / 2 - r, y, 2 * r, 2 * r, r, col, 255);
        g_round(x, y + h / 3, 2 * r, 2 * r, r, col, 255);
        g_round(x + w - 2 * r, y + h / 3, 2 * r, 2 * r, r, col, 255);
        g_fill(x + w / 2 - 1, y + h / 2, 3, h / 2, col);
        break;
    }
    }
}

/* The bottom bar. Every one of these has a keyboard shortcut too, but a game
 * whose only controls are undocumented letters is a game nobody plays twice. */
#define BAR_H 24
enum { BAR_NEW = 0, BAR_UNDO, BAR_DRAW, BAR_AUTO, NBAR };
static const int   BAR_W[NBAR]     = { 48, 52, 56, 52 };
static const char *BAR_LABEL[NBAR] = { "New", "Undo", "", "Auto" };
static int bar_x(int i) {
    int x = 8;
    for (int k = 0; k < i && k < NBAR; k++) x += BAR_W[k] + 5;
    return x;
}

static const char *rank_name(int r) {
    static const char *N[13] = { "A","2","3","4","5","6","7","8","9","10","J","Q","K" };
    return N[r];
}

static void draw_back(int x, int y, int w, int h) {
    g_round(x, y, w, h, 5, 0x1E3A6E, 255);
    g_round(x + 3, y + 3, w - 6, h - 6, 4, 0x2E5AA8, 255);
    /* a lattice, the way the blue Windows back looked */
    for (int r = 6; r < h - 6; r += 6)
        for (int c = 6; c < w - 6; c += 6)
            g_fill(x + c, y + r, 2, 2, 0x8FB6F0);
}

static void draw_card(int x, int y, int w, int h, uint8_t card, int selected) {
    uint32_t face = selected ? 0xFFF2C0 : 0xFAFAF6;
    g_round(x, y, w, h, 5, 0x101018, 255);           /* the edge, as a shadow */
    g_round(x + 1, y + 1, w - 2, h - 2, 5, face, 255);
    uint32_t col = RED(card) ? 0xC62828 : 0x1A1A1A;

    const char *r = rank_name(RANK(card));
    g_text(x + 4, y + 4, r, col, 1);
    pip(x + 4, y + 14, 8, SUIT(card), col);

    /* The big centre pip only fits once the card is reasonably tall, and a
     * squashed one looks like dirt. */
    if (h >= 46 && w >= 34) {
        int s = w / 2;
        pip(x + (w - s) / 2, y + (h - s) / 2, s, SUIT(card), col);
    }
    if (h >= 56) {                                   /* mirrored corner */
        int rw = g_text_width(r, 1);
        g_text(x + w - 4 - rw, y + h - 12, r, col, 1);
    }
    if (selected) g_rect(x, y, w, h, COL_ACCENT);
}

static void draw_slot(int x, int y, int w, int h, int suit_hint) {
    g_round(x, y, w, h, 5, 0x0E4A2A, 255);
    g_rect(x, y, w, h, 0x2A7A50);
    if (suit_hint >= 0) {
        int s = w / 2;
        pip(x + (w - s) / 2, y + (h - s) / 2, s, suit_hint, 0x1E6440);
    }
}

/* Layout is derived from the client rect every frame; the hit test reads the
 * same fields, so resizing can never desynchronise the two. */
static void layout(int cw, int ch) {
    int gap = cw / 60;
    if (gap < 6) gap = 6;
    int cardw = (cw - gap * (TABLEAUS + 1)) / TABLEAUS;
    if (cardw < 28) cardw = 28;
    int cardh = (cardw * 7) / 5;                       /* close enough to a real card */
    int maxh  = (ch - 24) / 3;
    if (cardh > maxh && maxh > 30) cardh = maxh;

    S.gap = gap; S.cardw = cardw; S.cardh = cardh;
    S.ox = (cw - (cardw * TABLEAUS + gap * (TABLEAUS - 1))) / 2;
    S.oy = 10;
    /* how far down each face-up card in a column is offset */
    S.fanv = cardh / 4;
    int need = 20 * S.fanv + cardh;
    int avail = ch - (S.oy + cardh + gap) - BAR_H - 6;
    if (need > avail && avail > cardh) {
        S.fanv = (avail - cardh) / 19;
        if (S.fanv < 5) S.fanv = 5;
    }
}

static void sol_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    S.cw = cw; S.ch = ch;
    layout(cw, ch);

    g_fill(cx, cy, cw, ch, 0x0B6E3C);                 /* the green baize */

    int cardw = S.cardw, cardh = S.cardh, gap = S.gap;
    int top = cy + S.oy, left = cx + S.ox;

    /* --- stock and waste, top left ------------------------------------- */
    if (S.g.nstock) draw_back(left, top, cardw, cardh);
    else {
        draw_slot(left, top, cardw, cardh, -1);
        if (S.g.nwaste) {                              /* the recycle arrow */
            int mx = left + cardw / 2, my = top + cardh / 2;
            g_rect(mx - 8, my - 8, 16, 16, 0x2A7A50);
            g_fill(mx - 8, my - 8, 10, 2, 0x8FD6B0);
            g_fill(mx + 4, my - 10, 2, 8, 0x8FD6B0);
        }
    }
    /* Draw-3 fans what it just dealt; draw-1 shows one card, because fanning
     * three there would put two unplayable cards where a playable one goes. */
    int wx = left + cardw + gap;
    int fan = cardw / 4;
    int cap = S.draw3 ? 3 : 1;
    int shown = S.g.nwaste < cap ? S.g.nwaste : cap;
    for (int i = 0; i < shown; i++) {
        int idx = S.g.nwaste - shown + i;
        int held = (S.held_src == SRC_WASTE && i == shown - 1);
        draw_card(wx + i * fan, top, cardw, cardh, S.g.waste[idx], held);
    }
    if (!S.g.nwaste) draw_slot(wx, top, cardw, cardh, -1);

    /* --- foundations, top right ----------------------------------------- */
    for (int f = 0; f < 4; f++) {
        int fx = left + (3 + f) * (cardw + gap);
        if (S.g.nfound[f]) draw_card(fx, top, cardw, cardh, S.g.found[f][S.g.nfound[f] - 1], 0);
        else               draw_slot(fx, top, cardw, cardh, f);
    }

    /* --- the seven columns ----------------------------------------------- */
    int ty = top + cardh + gap + 6;
    for (int c = 0; c < TABLEAUS; c++) {
        int x = left + c * (cardw + gap);
        if (!S.g.ntab[c]) { draw_slot(x, ty, cardw, cardh, -1); continue; }
        int down = S.g.ntab[c] - S.g.up[c];
        int y = ty;
        /* face-down cards stack tighter: they carry no information */
        for (int i = 0; i < down; i++) { draw_back(x, y, cardw, cardh); y += S.fanv / 2 + 1; }
        for (int i = down; i < S.g.ntab[c]; i++) {
            int held = (S.held_src == SRC_TAB && S.held_pile == c && i >= S.held_idx);
            draw_card(x, y, cardw, cardh, S.g.tab[c][i], held);
            y += S.fanv;
        }
    }

    /* --- status strip ----------------------------------------------------- */
    g_blend(cx, cy + ch - BAR_H, cw, BAR_H, 0x000000, 110);

    for (int i = 0; i < NBAR; i++) {
        char lab[16];
        if (i == BAR_DRAW) snprintf(lab, sizeof lab, "Draw %d", S.draw3 ? 3 : 1);
        else               strlcpy(lab, BAR_LABEL[i], sizeof lab);
        int live = (i != BAR_UNDO) || S.have_undo;
        int bx = cx + bar_x(i), by = cy + ch - BAR_H + 3;
        g_round(bx, by, BAR_W[i], BAR_H - 6, 5, live ? 0x1B5E20 : 0x134A18, 255);
        g_text(bx + (BAR_W[i] - g_text_width(lab, 1)) / 2, by + (BAR_H - 6 - 8) / 2,
               lab, live ? 0xD7FFD9 : 0x6B8C6E, 1);
    }

    uint64_t end = S.end_tick ? S.end_tick : pit_ticks();
    uint32_t sec = (uint32_t)((end - S.start_tick) / 1000);
    char line[80];
    snprintf(line, sizeof line, "Moves %u   %u:%02u", S.g.moves, sec / 60, sec % 60);
    int lx0 = cx + bar_x(NBAR) + 10;
    g_text(lx0, cy + ch - BAR_H + 9, line, 0xE8F5E9, 1);

    const char *msg = S.g.won        ? "You win. New deal?"
                    : auto_finishable() ? "Press A to finish it."
                    : S.status;
    int mw = g_text_width(msg, 1);
    if (msg[0] && lx0 + g_text_width(line, 1) + 16 + mw < cx + cw - 10)
        g_text(cx + cw - 10 - mw, cy + ch - BAR_H + 9, msg, 0xB9F6CA, 1);
}

/* --------------------------------------------------------------- input ---- */
/* Which pile is at (lx, ly), and which card index within it. Returns SRC_NONE
 * when the click landed on baize. */
static int hit(int lx, int ly, int *pile, int *idx) {
    int cardw = S.cardw, cardh = S.cardh, gap = S.gap;
    int top = S.oy, left = S.ox;

    if (ly >= top && ly < top + cardh) {
        if (lx >= left && lx < left + cardw) { *pile = 0; *idx = 0; return -1; }   /* stock */
        int wx = left + cardw + gap;
        int cap = S.draw3 ? 3 : 1;
        int shown = S.g.nwaste < cap ? S.g.nwaste : cap;
        int fan = cardw / 4;
        int wend = wx + (shown ? (shown - 1) * fan + cardw : cardw);
        if (lx >= wx && lx < wend && S.g.nwaste) { *pile = 0; *idx = S.g.nwaste - 1; return SRC_WASTE; }
        for (int f = 0; f < 4; f++) {
            int fx = left + (3 + f) * (cardw + gap);
            if (lx >= fx && lx < fx + cardw) { *pile = f; *idx = 0; return SRC_FOUND; }
        }
        return SRC_NONE;
    }

    int ty = top + cardh + gap + 6;
    if (ly < ty) return SRC_NONE;
    for (int c = 0; c < TABLEAUS; c++) {
        int x = left + c * (cardw + gap);
        if (lx < x || lx >= x + cardw) continue;
        *pile = c;
        if (!S.g.ntab[c]) { *idx = 0; return SRC_TAB; }
        int down = S.g.ntab[c] - S.g.up[c];
        int y = ty + down * (S.fanv / 2 + 1);
        if (ly < y) { *idx = -1; return SRC_TAB; }        /* on a face-down card */
        /* walk the face-up fan from the bottom so the topmost card wins */
        for (int i = S.g.ntab[c] - 1; i >= down; i--) {
            int cy2 = y + (i - down) * S.fanv;
            if (ly >= cy2 && ly < cy2 + cardh) { *idx = i; return SRC_TAB; }
        }
        *idx = -1;
        return SRC_TAB;
    }
    return SRC_NONE;
}

/* Put what is held onto (src, pile). Returns 1 if the move happened. */
static int drop_on(int src, int pile) {
    uint8_t buf[MAXPILE]; int n = 0;

    if (src == SRC_FOUND) {
        uint8_t card;
        if (S.held_src == SRC_WASTE)    { if (!S.g.nwaste) return 0; card = S.g.waste[S.g.nwaste - 1]; }
        else if (S.held_src == SRC_TAB) { if (S.held_idx != S.g.ntab[S.held_pile] - 1) return 0;
                                          card = S.g.tab[S.held_pile][S.held_idx]; }
        else return 0;
        if (!can_stack_found(card, pile, &S.g)) return 0;
        snapshot();
        lift(S.held_src, S.held_pile, S.held_idx, buf, &n);
        S.g.found[pile][S.g.nfound[pile]++] = buf[0];
        S.g.moves++;
        check_win();
        return 1;
    }

    if (src != SRC_TAB) return 0;
    uint8_t first;
    if (S.held_src == SRC_WASTE)      { if (!S.g.nwaste) return 0; first = S.g.waste[S.g.nwaste - 1]; }
    else if (S.held_src == SRC_FOUND) { if (!S.g.nfound[S.held_pile]) return 0;
                                        first = S.g.found[S.held_pile][S.g.nfound[S.held_pile] - 1]; }
    else                              { if (S.held_idx >= S.g.ntab[S.held_pile]) return 0;
                                        first = S.g.tab[S.held_pile][S.held_idx]; }

    uint8_t onto = S.g.ntab[pile] ? S.g.tab[pile][S.g.ntab[pile] - 1] : NOCARD;
    if (!can_stack_tab(first, onto)) return 0;

    snapshot();
    lift(S.held_src, S.held_pile, S.held_idx, buf, &n);
    for (int i = 0; i < n && S.g.ntab[pile] < MAXPILE; i++)
        S.g.tab[pile][S.g.ntab[pile]++] = buf[i];
    S.g.up[pile] = (uint8_t)(S.g.up[pile] + n);
    if (S.g.up[pile] > S.g.ntab[pile]) S.g.up[pile] = S.g.ntab[pile];
    S.g.moves++;
    return 1;
}

static void do_new(void)  { seed_rng(); deal(); }
static void do_undo(void) {
    if (!S.have_undo) return;
    S.g = S.undo; S.have_undo = 0; S.held_src = SRC_NONE;
    strlcpy(S.status, "Undone.", sizeof S.status);
}
static void do_auto(void) {
    if (!auto_finishable()) { strlcpy(S.status, "Turn everything face up first.", sizeof S.status); return; }
    for (int i = 0; i < 52 && !S.g.won; i++) auto_step();
}

static void sol_click(window_t *w, int lx, int ly) {
    (void)w;
    if (!S.cardw) return;

    if (ly >= S.ch - BAR_H) {
        for (int i = 0; i < NBAR; i++) {
            int bx = bar_x(i);
            if (lx < bx || lx >= bx + BAR_W[i]) continue;
            S.held_src = SRC_NONE;
            switch (i) {
            case BAR_NEW:  do_new(); break;
            case BAR_UNDO: do_undo(); break;
            case BAR_DRAW: S.draw3 = !S.draw3; break;
            case BAR_AUTO: do_auto(); break;
            }
            gui_request_redraw();
            return;
        }
        return;
    }

    int pile = 0, idx = 0;
    int src = hit(lx, ly, &pile, &idx);

    /* Double-click: the WM does not report one, so time two clicks on the same
     * card. 500ms, which is what Windows has defaulted to for thirty years --
     * 400 was mine and it was noticeably mean on a slow compositor, where the
     * two clicks are a frame apart rather than a millisecond. */
    uint64_t now = pit_ticks();
    int dbl = (now - S.last_click < 500 && src == S.last_src &&
               pile == S.last_pile && idx == S.last_idx && src > 0);
    S.last_click = now; S.last_src = src; S.last_pile = pile; S.last_idx = idx;

    if (src == -1) {                                   /* the stock */
        S.held_src = SRC_NONE;
        draw_stock();
        gui_request_redraw();
        return;
    }
    if (src == SRC_NONE) { S.held_src = SRC_NONE; gui_request_redraw(); return; }

    if (dbl && (src == SRC_WASTE || src == SRC_TAB)) {
        S.held_src = SRC_NONE;
        if (!try_to_foundation(src, pile, idx)) strlcpy(S.status, "Nowhere for that one yet.", sizeof S.status);
        gui_request_redraw();
        return;
    }

    if (S.held_src == SRC_NONE) {
        /* pick up */
        if (src == SRC_TAB) {
            if (idx < 0 || !S.g.ntab[pile]) return;
            int down = S.g.ntab[pile] - S.g.up[pile];
            if (idx < down) {                          /* a face-down card: turn it? */
                return;
            }
            S.held_src = SRC_TAB; S.held_pile = pile; S.held_idx = idx;
        } else if (src == SRC_WASTE) {
            if (!S.g.nwaste) return;
            S.held_src = SRC_WASTE; S.held_pile = 0; S.held_idx = S.g.nwaste - 1;
        } else if (src == SRC_FOUND) {
            if (!S.g.nfound[pile]) return;
            S.held_src = SRC_FOUND; S.held_pile = pile; S.held_idx = 0;
        }
        S.status[0] = 0;
        gui_request_redraw();
        return;
    }

    /* put down. Clicking the pile it came from just cancels. */
    if (src == S.held_src && pile == S.held_pile) { S.held_src = SRC_NONE; gui_request_redraw(); return; }
    if (!drop_on(src, pile)) strlcpy(S.status, "That card cannot go there.", sizeof S.status);
    else S.status[0] = 0;
    S.held_src = SRC_NONE;
    gui_request_redraw();
}

/* Right-click is the shortcut everyone reaches for: send it home. */
static void sol_rclick(window_t *w, int lx, int ly) {
    (void)w;
    int pile = 0, idx = 0;
    int src = hit(lx, ly, &pile, &idx);
    S.held_src = SRC_NONE;
    if (src == SRC_TAB && idx >= 0) try_to_foundation(SRC_TAB, pile, idx);
    else if (src == SRC_WASTE)      try_to_foundation(SRC_WASTE, 0, 0);
    gui_request_redraw();
}

static void sol_key(window_t *w, char c) {
    (void)w;
    switch (c) {
    case 'n': case 'N': do_new(); break;
    case 'u': case 'U': do_undo(); break;
    case 'd': case 'D': S.draw3 = !S.draw3; break;
    case 'a': case 'A': do_auto(); break;
    case ' ': S.held_src = SRC_NONE; draw_stock(); break;
    case 27:  S.held_src = SRC_NONE; break;
    default: return;
    }
    gui_request_redraw();
}

/* The clock in the status strip has to move on its own. */
static void sol_tick(window_t *w) { (void)w; if (!S.g.won) gui_request_redraw(); }

void solitaire_app_init(void) {
    memset(&S, 0, sizeof S);
    seed_rng();
    deal();

    win = gui_add_window("Solitaire", 720, 520, 0x2E7D32, ICON_CARDS);
    if (!win) return;
    win->draw   = sol_draw;
    win->click  = sol_click;
    win->rclick = sol_rclick;
    win->key    = sol_key;
    win->tick   = sol_tick;
    win->min_w  = 480; win->min_h = 380;
    win->x = 120; win->y = 60;
}
