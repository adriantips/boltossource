/* ===========================================================================
 *  BoltOS Mini  -  mini/vnc.c
 *
 *  The Pi's screen, on the Mac: an RFB (VNC) server that macOS's own Screen
 *  Sharing app connects to. RFB 3.8 with the classic VNC password -- Screen
 *  Sharing will not talk to a server that offers no authentication -- and
 *  from then on the Mac's keyboard and trackpad drive Mini as well as the
 *  keyboard on the Pi's own USB port.
 *
 *  Updates are computed, not tracked: each viewer has a shadow of what it
 *  was last sent, and when the front buffer has moved on, the two are
 *  compared 16x16 tile by tile. The changed tiles go out as Hextile -- a
 *  tile of one colour costs five bytes, any other tile its raw pixels --
 *  or Raw for a viewer that does not know Hextile. A big update is streamed
 *  across as many loops as the TCP send ring needs; the UI never waits.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

char g_vnc_password[9] = "boltos";

/* ---- DES, for the VNC password challenge -----------------------------------------
 *  The textbook algorithm, a bit at a time: it runs once per login.          */
static const uint8_t IPt[64] = {58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6,
    64,56,48,40,32,24,16,8,57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,61,53,45,37,29,21,13,5,
    63,55,47,39,31,23,15,7};
static const uint8_t FPt[64] = {40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,38,6,46,14,54,22,62,30,
    37,5,45,13,53,21,61,29,36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,34,2,42,10,50,18,58,26,
    33,1,41,9,49,17,57,25};
static const uint8_t Et[48] = {32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,12,13,14,15,16,17,16,17,18,19,
    20,21,20,21,22,23,24,25,24,25,26,27,28,29,28,29,30,31,32,1};
static const uint8_t Pt[32] = {16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,2,8,24,14,32,27,3,9,19,13,
    30,6,22,11,4,25};
static const uint8_t PC1[56] = {57,49,41,33,25,17,9,1,58,50,42,34,26,18,10,2,59,51,43,35,27,19,11,3,
    60,52,44,36,63,55,47,39,31,23,15,7,62,54,46,38,30,22,14,6,61,53,45,37,29,21,13,5,28,20,12,4};
static const uint8_t PC2[48] = {14,17,11,24,1,5,3,28,15,6,21,10,23,19,12,4,26,8,16,7,27,20,13,2,
    41,52,31,37,47,55,30,40,51,45,33,48,44,49,39,56,34,53,46,42,50,36,29,32};
static const uint8_t SHIFTS[16] = {1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};
static const uint8_t SBOX[8][64] = {
 {14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
  4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
 {15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,
  0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
 {10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
  13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
 {7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
  10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14},
 {2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
  4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
 {12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
  9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
 {4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
  1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
 {13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
  7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11}};

static uint64_t perm(uint64_t in, const uint8_t *t, int n, int inbits) {
    uint64_t out = 0;
    for (int i = 0; i < n; i++) out = (out << 1) | ((in >> (inbits - t[i])) & 1);
    return out;
}

static uint64_t des_encrypt(uint64_t key, uint64_t block) {
    uint64_t k56 = perm(key, PC1, 56, 64);
    uint32_t C = (uint32_t)(k56 >> 28) & 0x0FFFFFFF, D = (uint32_t)k56 & 0x0FFFFFFF;
    uint64_t sub[16];
    for (int r = 0; r < 16; r++) {
        for (int s = 0; s < SHIFTS[r]; s++) {
            C = ((C << 1) | (C >> 27)) & 0x0FFFFFFF;
            D = ((D << 1) | (D >> 27)) & 0x0FFFFFFF;
        }
        sub[r] = perm(((uint64_t)C << 28) | D, PC2, 48, 56);
    }
    uint64_t ip = perm(block, IPt, 64, 64);
    uint32_t L = (uint32_t)(ip >> 32), R = (uint32_t)ip;
    for (int r = 0; r < 16; r++) {
        uint64_t e = perm(R, Et, 48, 32) ^ sub[r];
        uint32_t s = 0;
        for (int b = 0; b < 8; b++) {
            int six = (int)(e >> (42 - 6 * b)) & 0x3F;
            int row = ((six >> 4) & 2) | (six & 1), col = (six >> 1) & 0xF;
            s = (s << 4) | SBOX[b][row * 16 + col];
        }
        uint32_t f = (uint32_t)perm(s, Pt, 32, 32);
        uint32_t t = R; R = L ^ f; L = t;
    }
    return perm(((uint64_t)R << 32) | L, FPt, 64, 64);
}

/* VNC's quirk: the key is the password with each byte's bits reversed. */
static void vnc_auth_response(const uint8_t chal[16], uint8_t out[16]) {
    uint64_t key = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t b = (uint8_t)g_vnc_password[i], r = 0;
        if (i >= (int)strlen(g_vnc_password)) b = 0;
        for (int k = 0; k < 8; k++) if (b & (1 << k)) r |= (uint8_t)(0x80 >> k);
        key = (key << 8) | r;
    }
    for (int h = 0; h < 2; h++) {
        uint64_t blk = 0;
        for (int i = 0; i < 8; i++) blk = (blk << 8) | chal[8 * h + i];
        uint64_t c = des_encrypt(key, blk);
        for (int i = 0; i < 8; i++) out[8 * h + i] = (uint8_t)(c >> (56 - 8 * i));
    }
}

int vnc_des_selftest(void) {   /* FIPS 81 / the usual textbook vector */
    return des_encrypt(0x133457799BBCDFF1ull, 0x0123456789ABCDEFull) == 0x85E813540F0AB405ull;
}

/* ---- the server --------------------------------------------------------------------- */
#define VNC_N  3
#define TILE   16
enum { V_VERSION, V_SECTYPE, V_AUTH, V_INIT, V_RUN };

typedef struct {
    tcb_t   *c;
    int      st, minor;
    uint8_t  in[1024]; int inlen;
    uint8_t  chal[16];
    /* the viewer's pixel format */
    int      bpp, big, rmax, gmax, bmax, rsh, gsh, bsh, native;
    int      hextile, desktop_size, cursor, cursor_sent, size_sent;
    int      want, incremental;
    uint32_t *shadow;
    int      sw, sh;                    /* shadow's size                     */
    uint32_t gen;
    int      full;
    /* the update being streamed */
    struct { uint16_t x, y, w, h; } *rects;
    int      nrects, rect_cap, cur, cur_tile;
    uint64_t t_conn;
    uint32_t updates;
} vclient_t;

static vclient_t vc[VNC_N];
static uint16_t vnc_port;

int vnc_clients(void) {
    int n = 0;
    for (int i = 0; i < VNC_N; i++) if (vc[i].c && vc[i].st == V_RUN) n++;
    return n;
}

static void vc_drop(vclient_t *v, const char *why) {
    if (v->c) { uint16_t p; uint32_t ip = tcp_peer(v->c, &p); char s[16]; net_fmt_ip(ip, s);
                kprintf("[vnc] %s left (%s)\n", s, why); tcp_abort(v->c); }
    if (v->st == V_RUN) shell_toast("Screen sharing ended", 0);
    if (v->shadow) kfree(v->shadow);
    if (v->rects) kfree(v->rects);
    memset(v, 0, sizeof *v);
    shell_request_frame();
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }

static void set_native_format(vclient_t *v) {
    v->bpp = 32; v->big = 0; v->rmax = v->gmax = v->bmax = 255;
    v->rsh = 16; v->gsh = 8; v->bsh = 0; v->native = 1;
}

static int px_bytes(vclient_t *v) { return v->bpp / 8; }

static uint8_t *put_px(vclient_t *v, uint8_t *o, uint32_t c) {
    if (v->native) { memcpy(o, &c, 4); return o + 4; }
    uint32_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    uint32_t p = ((r * (uint32_t)v->rmax / 255) << v->rsh) | ((g * (uint32_t)v->gmax / 255) << v->gsh) |
                 ((b * (uint32_t)v->bmax / 255) << v->bsh);
    int n = px_bytes(v);
    for (int i = 0; i < n; i++) o[i] = (uint8_t)(v->big ? p >> (8 * (n - 1 - i)) : p >> (8 * i));
    return o + n;
}

static void send_server_init(vclient_t *v) {
    uint8_t b[64];
    put16(b, (uint16_t)scr_w); put16(b + 2, (uint16_t)scr_h);
    b[4] = 32; b[5] = 24; b[6] = 0; b[7] = 1;
    put16(b + 8, 255); put16(b + 10, 255); put16(b + 12, 255);
    b[14] = 16; b[15] = 8; b[16] = 0; b[17] = b[18] = b[19] = 0;
    const char *name = "BoltOS Mini";
    put32(b + 20, (uint32_t)strlen(name));
    memcpy(b + 24, name, strlen(name));
    tcp_write(v->c, b, 24 + (int)strlen(name));
    set_native_format(v);
}

/* ---- client -> server ------------------------------------------------------------------ */
static int vmods;

static void key_event(int down, uint32_t sym) {
    int bit = 0;
    if (sym == 0xFFE1 || sym == 0xFFE2) bit = MOD_SHIFT;
    else if (sym == 0xFFE3 || sym == 0xFFE4) bit = MOD_CTRL;
    else if (sym == 0xFFE9 || sym == 0xFFEA) bit = MOD_ALT;
    else if (sym == 0xFFE7 || sym == 0xFFE8 || sym == 0xFFEB || sym == 0xFFEC) bit = MOD_SUPER;
    if (bit) { if (down) vmods |= bit; else vmods &= ~bit; return; }
    if (!down) return;
    int ch, k = input_from_keysym(sym, &ch);
    input_key(k, ch, vmods, SRC_VNC);
}

static void pointer_event(int mask, int x, int y) {
    static int last;
    int b = (mask & 1) | ((mask & 4) ? 2 : 0) | ((mask & 2) ? 4 : 0);
    int wheel = 0;
    if ((mask & 8) && !(last & 8)) wheel = 1;
    if ((mask & 16) && !(last & 16)) wheel = -1;
    last = mask;
    input_pointer(x, y, b, wheel, SRC_VNC);
}

/* Consume one complete message from v->in; returns bytes used, 0 if the
 * message is not all here yet, -1 on something we cannot parse. */
static int vc_message(vclient_t *v) {
    uint8_t *m = v->in;
    int n = v->inlen;
    switch (v->st) {
    case V_VERSION:
        if (n < 12) return 0;
        if (memcmp(m, "RFB 003.", 8) != 0) return -1;
        v->minor = (m[8] - '0') * 100 + (m[9] - '0') * 10 + (m[10] - '0');
        if (v->minor >= 7) {
            uint8_t t[2] = { 1, 2 };                       /* one type: VNC auth */
            tcp_write(v->c, t, 2);
            v->st = V_SECTYPE;
        } else {
            uint8_t t[4] = { 0, 0, 0, 2 };
            tcp_write(v->c, t, 4);
            for (int i = 0; i < 16; i++) v->chal[i] = (uint8_t)mini_rand();
            tcp_write(v->c, v->chal, 16);
            v->st = V_AUTH;
        }
        return 12;
    case V_SECTYPE:
        if (n < 1) return 0;
        if (m[0] != 2) return -1;
        for (int i = 0; i < 16; i++) v->chal[i] = (uint8_t)mini_rand();
        tcp_write(v->c, v->chal, 16);
        v->st = V_AUTH;
        return 1;
    case V_AUTH: {
        if (n < 16) return 0;
        uint8_t want[16];
        vnc_auth_response(v->chal, want);
        if (memcmp(want, m, 16) != 0) {
            uint8_t r[4] = { 0, 0, 0, 1 };
            tcp_write(v->c, r, 4);
            if (v->minor >= 8) {
                const char *why = "Wrong password (it is shown in Settings on the Pi)";
                uint8_t l[4]; put32(l, (uint32_t)strlen(why));
                tcp_write(v->c, l, 4); tcp_write(v->c, why, (int)strlen(why));
            }
            kprintf("[vnc] wrong password\n");
            tcp_close(v->c);
            v->c = 0;
            return -1;
        }
        uint8_t ok[4] = { 0, 0, 0, 0 };
        tcp_write(v->c, ok, 4);
        v->st = V_INIT;
        return 16;
    }
    case V_INIT:
        if (n < 1) return 0;
        send_server_init(v);
        v->st = V_RUN;
        v->full = 1;
        { uint16_t p; char s[16]; net_fmt_ip(tcp_peer(v->c, &p), s);
          kprintf("[vnc] %s is watching (RFB 3.%d)\n", s, v->minor); }
        shell_toast("Screen shared", "A Mac is viewing this screen");
        return 1;
    }
    /* V_RUN: the normal messages */
    if (n < 1) return 0;
    switch (m[0]) {
    case 0:                                                /* SetPixelFormat  */
        if (n < 20) return 0;
        v->bpp = m[4]; v->big = m[6] != 0;
        v->rmax = (m[8] << 8) | m[9]; v->gmax = (m[10] << 8) | m[11]; v->bmax = (m[12] << 8) | m[13];
        v->rsh = m[14]; v->gsh = m[15]; v->bsh = m[16];
        if (!m[7] || (v->bpp != 8 && v->bpp != 16 && v->bpp != 32)) { set_native_format(v); }
        v->native = v->bpp == 32 && !v->big && v->rmax == 255 && v->gmax == 255 && v->bmax == 255 &&
                    v->rsh == 16 && v->gsh == 8 && v->bsh == 0;
        v->full = 1;
        return 20;
    case 2: {                                              /* SetEncodings    */
        if (n < 4) return 0;
        int cnt = (m[2] << 8) | m[3];
        if (n < 4 + 4 * cnt) return 0;
        v->hextile = v->desktop_size = v->cursor = 0;
        for (int i = 0; i < cnt; i++) {
            int32_t e = (int32_t)(((uint32_t)m[4 + 4 * i] << 24) | ((uint32_t)m[5 + 4 * i] << 16) |
                                  ((uint32_t)m[6 + 4 * i] << 8) | m[7 + 4 * i]);
            if (e == 5) v->hextile = 1;
            if (e == -223) v->desktop_size = 1;
            if (e == -239) v->cursor = 1;
        }
        v->cursor_sent = 0;
        return 4 + 4 * cnt;
    }
    case 3:                                                /* UpdateRequest   */
        if (n < 10) return 0;
        v->want = 1;
        if (!m[1]) v->full = 1;
        return 10;
    case 4:                                                /* KeyEvent        */
        if (n < 8) return 0;
        key_event(m[1], ((uint32_t)m[4] << 24) | ((uint32_t)m[5] << 16) | ((uint32_t)m[6] << 8) | m[7]);
        return 8;
    case 5:                                                /* PointerEvent    */
        if (n < 6) return 0;
        pointer_event(m[1], (m[2] << 8) | m[3], (m[4] << 8) | m[5]);
        return 6;
    case 6: {                                              /* ClientCutText   */
        if (n < 8) return 0;
        uint32_t len = ((uint32_t)m[4] << 24) | ((uint32_t)m[5] << 16) | ((uint32_t)m[6] << 8) | m[7];
        if (len > 100000) return -1;
        if ((uint32_t)n < 8 + len) {
            if (8 + len > sizeof v->in) {                  /* bigger than us: skip it */
                v->inlen = 0; return 0;
            }
            return 0;
        }
        return (int)(8 + len);
    }
    default:
        kprintf("[vnc] unknown message %u\n", m[0]);
        return -1;
    }
}

/* ---- server -> client: updates --------------------------------------------------------- */
static const char *arrow[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
    "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.........X ", "X......XXXXX",
    "X...X..X    ", "X..XX..X    ", "X.X  X..X   ", "XX   X..X   ", "X     X..X  ", "      X..X  ", "       XX   " };

static void send_cursor_rect(vclient_t *v) {
    int w = 12, h = 19, bpp = px_bytes(v);
    uint8_t buf[12 + 12 * 19 * 4 + 2 * 19];
    put16(buf, 0); put16(buf + 2, 0); put16(buf + 4, (uint16_t)w); put16(buf + 6, (uint16_t)h);
    put32(buf + 8, (uint32_t)-239);
    uint8_t *o = buf + 12;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) o = put_px(v, o, arrow[y][x] == '.' ? 0xFFFFFF : 0x000000);
    for (int y = 0; y < h; y++) {
        uint8_t m0 = 0, m1 = 0;
        for (int x = 0; x < w; x++) if (arrow[y][x] != ' ') { if (x < 8) m0 |= (uint8_t)(0x80 >> x); else m1 |= (uint8_t)(0x80 >> (x - 8)); }
        *o++ = m0; *o++ = m1;
    }
    (void)bpp;
    tcp_write(v->c, buf, (int)(o - buf));
    v->cursor_sent = 1;
}

static int tile_dirty(vclient_t *v, int tx, int ty) {
    int x0 = tx * TILE, y0 = ty * TILE;
    int w = scr_w - x0 < TILE ? scr_w - x0 : TILE, h = scr_h - y0 < TILE ? scr_h - y0 : TILE;
    for (int y = 0; y < h; y++)
        if (memcmp(g_front + (size_t)(y0 + y) * scr_w + x0, v->shadow + (size_t)(y0 + y) * scr_w + x0, (size_t)w * 4))
            return 1;
    return 0;
}

static void add_rect(vclient_t *v, int x, int y, int w, int h) {
    if (v->nrects == v->rect_cap) {
        int cap = v->rect_cap ? v->rect_cap * 2 : 256;
        void *n = kmalloc((uint64_t)cap * sizeof *v->rects);
        if (!n) return;
        if (v->rects) { memcpy(n, v->rects, (size_t)v->nrects * sizeof *v->rects); kfree(v->rects); }
        v->rects = n; v->rect_cap = cap;
    }
    v->rects[v->nrects].x = (uint16_t)x; v->rects[v->nrects].y = (uint16_t)y;
    v->rects[v->nrects].w = (uint16_t)w; v->rects[v->nrects].h = (uint16_t)h;
    v->nrects++;
}

/* Decide what changed and start sending it. */
static void vc_begin_update(vclient_t *v) {
    if (v->sw != scr_w || v->sh != scr_h || !v->shadow) {
        if (v->shadow) kfree(v->shadow);
        v->shadow = kmalloc((uint64_t)scr_w * scr_h * 4);
        if (!v->shadow) return;
        v->sw = scr_w; v->sh = scr_h;
        v->full = 1;
    }
    int resize = 0;
    if (v->size_sent && (v->size_sent != ((scr_w << 16) | scr_h))) resize = v->desktop_size;
    v->nrects = 0;
    int tw = (scr_w + TILE - 1) / TILE, th = (scr_h + TILE - 1) / TILE;
    if (!v->full && v->gen == g_front_gen && v->cursor_sent == v->cursor && !resize) return;
    uint32_t gen = g_front_gen;
    for (int ty = 0; ty < th; ty++) {
        int run = -1;
        for (int tx = 0; tx <= tw; tx++) {
            int d = tx < tw && (v->full || tile_dirty(v, tx, ty));
            if (d && run < 0) run = tx;
            if (!d && run >= 0) {
                int x = run * TILE, y = ty * TILE;
                int w = (tx * TILE > scr_w ? scr_w : tx * TILE) - x;
                int h = scr_h - y < TILE ? scr_h - y : TILE;
                add_rect(v, x, y, w, h);
                run = -1;
            }
        }
    }
    v->gen = gen;
    v->full = 0;
    int extra = (v->cursor && !v->cursor_sent) + resize;
    if (!v->nrects && !extra) return;
    /* What the viewer will have once this is through. */
    for (int i = 0; i < v->nrects; i++)
        for (int y = 0; y < v->rects[i].h; y++) {
            size_t o = (size_t)(v->rects[i].y + y) * scr_w + v->rects[i].x;
            memcpy(v->shadow + o, g_front + o, (size_t)v->rects[i].w * 4);
        }
    uint8_t h[4] = { 0, 0 };
    put16(h + 2, (uint16_t)(v->nrects + extra));
    tcp_write(v->c, h, 4);
    if (resize) {
        uint8_t r[12]; put16(r, 0); put16(r + 2, 0); put16(r + 4, (uint16_t)scr_w); put16(r + 6, (uint16_t)scr_h);
        put32(r + 8, (uint32_t)-223); tcp_write(v->c, r, 12);
    }
    if (v->cursor && !v->cursor_sent) send_cursor_rect(v);
    v->size_sent = (scr_w << 16) | scr_h;
    v->cur = 0; v->cur_tile = -1;
    v->want = 0;
    v->updates++;
}

/* Stream as much of the current update as the send ring will take. */
static void vc_pump(vclient_t *v) {
    static uint8_t buf[4 + 64 * (1 + TILE * TILE * 4) + 64];
    int bpp = px_bytes(v);
    while (v->cur < v->nrects) {
        int rx = v->rects[v->cur].x, ry = v->rects[v->cur].y, rw = v->rects[v->cur].w, rh = v->rects[v->cur].h;
        if (v->cur_tile < 0) {                             /* the rect's header */
            if (tcp_space(v->c) < 12) return;
            uint8_t r[12];
            put16(r, (uint16_t)rx); put16(r + 2, (uint16_t)ry); put16(r + 4, (uint16_t)rw); put16(r + 6, (uint16_t)rh);
            put32(r + 8, v->hextile ? 5u : 0u);
            tcp_write(v->c, r, 12);
            v->cur_tile = 0;
        }
        if (!v->hextile) {                                 /* Raw: whole rect   */
            int need = rw * rh * bpp;
            if (tcp_space(v->c) < need) return;
            uint8_t *o = buf;
            for (int y = 0; y < rh; y++) {
                const uint32_t *s = v->shadow + (size_t)(ry + y) * scr_w + rx;
                if (v->native) { memcpy(o, s, (size_t)rw * 4); o += rw * 4; }
                else for (int x = 0; x < rw; x++) o = put_px(v, o, s[x]);
            }
            tcp_write(v->c, buf, (int)(o - buf));
        } else {                                           /* Hextile, a tile at a time */
            int ntiles = (rw + TILE - 1) / TILE;
            while (v->cur_tile < ntiles) {
                if (tcp_space(v->c) < 1 + TILE * TILE * 4) return;
                int tx = rx + v->cur_tile * TILE;
                int tw = rx + rw - tx < TILE ? rx + rw - tx : TILE;
                const uint32_t *s = v->shadow + (size_t)ry * scr_w + tx;
                uint32_t first = s[0];
                int solid = 1;
                for (int y = 0; y < rh && solid; y++)
                    for (int x = 0; x < tw; x++) if (s[(size_t)y * scr_w + x] != first) { solid = 0; break; }
                uint8_t *o = buf;
                if (solid) { *o++ = 2; o = put_px(v, o, first); }
                else {
                    *o++ = 1;
                    for (int y = 0; y < rh; y++) {
                        if (v->native) { memcpy(o, s + (size_t)y * scr_w, (size_t)tw * 4); o += tw * 4; }
                        else for (int x = 0; x < tw; x++) o = put_px(v, o, s[(size_t)y * scr_w + x]);
                    }
                }
                tcp_write(v->c, buf, (int)(o - buf));
                v->cur_tile++;
            }
        }
        v->cur++;
        v->cur_tile = -1;
    }
}

int vnc_init(uint16_t port) {
    vnc_port = port;
    if (!vnc_des_selftest()) { kprintf("[vnc] DES self-test failed: no VNC\n"); return -1; }
    tcp_listen(port);
    kprintf("[vnc] screen sharing on port %u (password \"%s\")\n", port, g_vnc_password);
    return 0;
}

void vnc_poll(void) {
    tcb_t *nc;
    while ((nc = tcp_accept(vnc_port))) {
        vclient_t *v = 0;
        for (int i = 0; i < VNC_N; i++) if (!vc[i].c) { v = &vc[i]; break; }
        if (!v) { tcp_abort(nc); continue; }
        memset(v, 0, sizeof *v);
        v->c = nc;
        v->t_conn = now_ms();
        tcp_write(nc, "RFB 003.008\n", 12);
    }
    for (int i = 0; i < VNC_N; i++) {
        vclient_t *v = &vc[i];
        if (!v->c) continue;
        if (!tcp_open(v->c) || tcp_peer_closed(v->c)) { vc_drop(v, "closed"); continue; }
        int n = tcp_read(v->c, v->in + v->inlen, (int)sizeof v->in - v->inlen);
        if (n > 0) v->inlen += n;
        for (;;) {
            int used = vc_message(v);
            if (used < 0) { if (v->c) vc_drop(v, "protocol"); else memset(v, 0, sizeof *v); break; }
            if (used == 0) break;
            memmove(v->in, v->in + used, (size_t)(v->inlen - used));
            v->inlen -= used;
        }
        if (!v->c) continue;
        if (v->st != V_RUN) {
            if (now_ms() - v->t_conn > 60000) vc_drop(v, "never logged in");
            continue;
        }
        if (v->cur < v->nrects) vc_pump(v);
        else if (v->want && tcp_unsent(v->c) < 128 * 1024) { vc_begin_update(v); vc_pump(v); }
    }
}
