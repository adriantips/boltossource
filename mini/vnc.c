/* ===========================================================================
 *  BoltOS Mini  -  mini/vnc.c
 *
 *  The Pi's screen, on the Mac: an RFB (VNC) server that macOS's own Screen
 *  Sharing app connects to. RFB 3.8 with the classic VNC password -- Screen
 *  Sharing will not talk to a server that offers no authentication -- and
 *  from then on the Mac's keyboard and trackpad drive Mini as well as the
 *  keyboard on the Pi's own USB port.
 *
 *  Updates come from the screen's tile stamps (screen.c): every 16x16 tile
 *  carries the g_front_gen it last changed in, and a viewer is owed each
 *  tile stamped after the update it was last sent. An update is encoded
 *  whole into a buffer when it starts and streamed out as the TCP send ring
 *  takes it, across as many loops as that needs; the UI never waits.
 *
 *  Encodings, in the order the viewer lists them: ZRLE (64x64 tiles as one
 *  colour, a packed palette, runs, or plain pixels), zlib (pixels through a
 *  small deflate made for screens), Hextile (16x16 tiles, one colour or
 *  raw), Raw. macOS Screen Sharing lists zlib before ZRLE and doesn't ask
 *  for Hextile at all -- before zlib it was sent every changed pixel raw. A flat screen is mostly tiles of
 *  one colour, so a whole new screen is tens of kilobytes, not 2.4 MB.
 *  ZRLE's zlib stream is written as stored blocks: its tiles are small
 *  already, and the 29 MB/s cable is cheaper than compressing them again.
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
    int      bpp, depth, big, rmax, gmax, bmax, rsh, gsh, bsh, native;
    int      cpix;                      /* bytes in a ZRLE CPIXEL            */
    int      enc;                       /* 16 ZRLE, 6 zlib, 5 Hextile, 0 Raw: the viewer's first */
    int      desktop_size, cursor, cursor_sent, size_sent;
    int      want;
    uint32_t gen;                       /* g_front_gen it has been sent      */
    int      full;
    int      z_started, zl_started;     /* ZRLE's, zlib's stream header is out */
    /* the update being streamed */
    uint8_t *out;
    int      out_cap, out_len, out_pos, out_rects;
    uint64_t t_conn;
    uint32_t updates;
    uint32_t p_updates; uint64_t p_bytes;       /* since the last /perf  */
    /* the clipboard: a viewer's arriving in pieces, ours going out */
    char    *cut; uint32_t cut_len, cut_left;
    uint32_t cut_sent;
} vclient_t;

void clip_from_viewer(const char *s, int n);
uint32_t clip_gen(void);
const char *clip_get(int *n);

static vclient_t vc[VNC_N];
static uint16_t vnc_port;

/* Every viewer is still being sent an update: a new frame would wait. */
int vnc_streaming(void) {
    int n = 0;
    for (int i = 0; i < VNC_N; i++) {
        if (!vc[i].c || vc[i].st != V_RUN) continue;
        if (vc[i].out_pos >= vc[i].out_len) return 0;
        n++;
    }
    return n > 0;
}

/* For /perf: each viewer's updates since the last call. */
int vnc_perf(char *out, int cap, uint64_t window_us) {
    int n = 0;
    for (int i = 0; i < VNC_N && n < cap - 160; i++) {
        vclient_t *v = &vc[i];
        if (!v->c || v->st != V_RUN) continue;
        uint16_t port; char ip[16]; net_fmt_ip(tcp_peer(v->c, &port), ip);
        uint64_t w = window_us ? window_us : 1;
        n += snprintf(out + n, (size_t)(cap - n), "viewer %s:%u RFB 3.%d, %s, %d-byte pixels: %u updates (%lu.%lu/s), %lu KiB\n",
                      ip, port, v->minor, v->enc == 16 ? "ZRLE" : v->enc == 6 ? "zlib" : v->enc == 5 ? "Hextile" : "Raw",
                      v->enc == 16 ? v->cpix : v->bpp / 8, v->p_updates,
                      (unsigned long)(v->p_updates * 10000000ull / w / 10), (unsigned long)(v->p_updates * 10000000ull / w % 10),
                      (unsigned long)(v->p_bytes >> 10));
        v->p_updates = 0; v->p_bytes = 0;
    }
    return n;
}

int vnc_clients(void) {
    int n = 0;
    for (int i = 0; i < VNC_N; i++) if (vc[i].c && vc[i].st == V_RUN) n++;
    return n;
}

static void vc_drop(vclient_t *v, const char *why) {
    if (v->c) { uint16_t p; uint32_t ip = tcp_peer(v->c, &p); char s[16]; net_fmt_ip(ip, s);
                kprintf("[vnc] %s left (%s)\n", s, why); tcp_abort(v->c); }
    if (v->st == V_RUN) shell_toast("Screen sharing ended", 0);
    if (v->out) kfree(v->out);
    kfree(v->cut);
    memset(v, 0, sizeof *v);
    shell_request_frame();
}

static int vtcp_write(tcb_t *c, const void *b, int n) { g_perf.vnc_bytes += (uint64_t)n; return tcp_write(c, b, n); }
#define tcp_write vtcp_write

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }

/* A ZRLE CPIXEL drops the unused byte of a 32-bit pixel whose colour fits
 * in its low or high three bytes. RFC 6143 also asks for depth <= 24;
 * RealVNC/TigerVNC and libvncserver go by the colour bits alone, and so do
 * we. (Screen Sharing, which asks for depth 32, gets zlib instead: see
 * enc_zlib.) */
static void set_cpix(vclient_t *v) {
    int lo = (v->rmax << v->rsh) < (1 << 24) && (v->gmax << v->gsh) < (1 << 24) && (v->bmax << v->bsh) < (1 << 24);
    int hi = v->rsh >= 8 && v->gsh >= 8 && v->bsh >= 8;
    v->cpix = v->bpp == 32 && (lo || hi) ? 3 : v->bpp / 8;
}

static void set_native_format(vclient_t *v) {
    v->bpp = 32; v->depth = 24; v->big = 0; v->rmax = v->gmax = v->bmax = 255;
    v->rsh = 16; v->gsh = 8; v->bsh = 0; v->native = 1;
    set_cpix(v);
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
    int code = input_keysym_to_evdev(sym);                 /* a Linux program's, with the key-ups */
    if (code) input_raw(1, code, down);
    int bit = 0;
    if (sym == 0xFFE1 || sym == 0xFFE2) bit = MOD_SHIFT;
    else if (sym == 0xFFE3 || sym == 0xFFE4) bit = MOD_CTRL;
    else if (sym == 0xFFE9 || sym == 0xFFEA) bit = MOD_ALT;
    else if (sym == 0xFFE7 || sym == 0xFFE8 || sym == 0xFFEB || sym == 0xFFEC) bit = MOD_SUPER;
    /* Super alone, pressed and let go, is Home as on a USB keyboard -- here
     * only while an app has the whole screen (a game), where Esc is its own. */
    static int super_alone;
    if (bit == MOD_SUPER && down) super_alone = 1;
    else if (down) super_alone = 0;
    if (bit == MOD_SUPER && !down && super_alone) {
        super_alone = 0;
        int shell_is_fullscreen(void);
        if (shell_is_fullscreen()) input_key(KEY_SUPER, 0, 0, SRC_VNC);
    }
    if (bit) { if (down) vmods |= bit; else vmods &= ~bit; return; }
    if (!down) return;
    int ch, k = input_from_keysym(sym, &ch);
    input_key(k, ch, vmods, SRC_VNC);
}

static void pointer_event(int mask, int x, int y) {
    static int last, lx = -1, ly = -1;
    int b = (mask & 1) | ((mask & 4) ? 2 : 0) | ((mask & 2) ? 4 : 0);
    int wheel = 0;
    if ((mask & 8) && !(last & 8)) wheel = 1;
    if ((mask & 16) && !(last & 16)) wheel = -1;
    last = mask;
    /* A viewer says where, not how far: the difference, which stops at the window's edge. */
    if (lx >= 0) input_raw_mouse(x - lx, y - ly, wheel, b);
    lx = x; ly = y;
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
        v->bpp = m[4]; v->depth = m[5]; v->big = m[6] != 0;
        v->rmax = (m[8] << 8) | m[9]; v->gmax = (m[10] << 8) | m[11]; v->bmax = (m[12] << 8) | m[13];
        v->rsh = m[14]; v->gsh = m[15]; v->bsh = m[16];
        if (!m[7] || (v->bpp != 8 && v->bpp != 16 && v->bpp != 32)) { set_native_format(v); }
        v->native = v->bpp == 32 && !v->big && v->rmax == 255 && v->gmax == 255 && v->bmax == 255 &&
                    v->rsh == 16 && v->gsh == 8 && v->bsh == 0;
        set_cpix(v);
        v->full = 1;
        kprintf("[vnc] pixel format: %d bpp, depth %d, %s, max %d/%d/%d, shift %d/%d/%d\n", v->bpp, m[5],
                v->big ? "big" : "little", v->rmax, v->gmax, v->bmax, v->rsh, v->gsh, v->bsh);
        return 20;
    case 2: {                                              /* SetEncodings    */
        if (n < 4) return 0;
        int cnt = (m[2] << 8) | m[3];
        if (n < 4 + 4 * cnt) return 0;
        v->enc = 0; v->desktop_size = v->cursor = 0;
        int picked = 0;
        for (int i = 0; i < cnt; i++) {
            int32_t e = (int32_t)(((uint32_t)m[4 + 4 * i] << 24) | ((uint32_t)m[5 + 4 * i] << 16) |
                                  ((uint32_t)m[6 + 4 * i] << 8) | m[7 + 4 * i]);
            if (v->updates == 0 && i < 24) kprintf("%s%d%s", i ? " " : "[vnc] encodings: ", (int)e, i + 1 == cnt || i == 23 ? "\n" : "");
            if (!picked && (e == 16 || e == 6 || e == 5 || e == 0)) { v->enc = e; picked = 1; }
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
    case 6: {                                              /* ClientCutText: the Mac's clipboard */
        if (n < 8) return 0;
        uint32_t len = ((uint32_t)m[4] << 24) | ((uint32_t)m[5] << 16) | ((uint32_t)m[6] << 8) | m[7];
        if (len > 4u << 20) return -1;
        if ((uint32_t)n >= 8 + len) { clip_from_viewer((const char *)m + 8, (int)len); v->cut_sent = clip_gen(); return (int)(8 + len); }
        /* bigger than what's here: the rest is gathered as it comes (vnc_poll) */
        kfree(v->cut);
        v->cut = kmalloc(len + 1);
        if (!v->cut) return -1;
        memcpy(v->cut, m + 8, (size_t)(n - 8));
        v->cut_len = (uint32_t)(n - 8); v->cut_left = len - v->cut_len;
        return n;
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

static uint8_t *cursor_rect(vclient_t *v, uint8_t *o) {
    int w = 12, h = 19;
    put16(o, 0); put16(o + 2, 0); put16(o + 4, (uint16_t)w); put16(o + 6, (uint16_t)h);
    put32(o + 8, (uint32_t)-239);
    o += 12;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) o = put_px(v, o, arrow[y][x] == '.' ? 0xFFFFFF : 0x000000);
    for (int y = 0; y < h; y++) {
        uint8_t m0 = 0, m1 = 0;
        for (int x = 0; x < w; x++) if (arrow[y][x] != ' ') { if (x < 8) m0 |= (uint8_t)(0x80 >> x); else m1 |= (uint8_t)(0x80 >> (x - 8)); }
        *o++ = m0; *o++ = m1;
    }
    v->cursor_sent = 1;
    return o;
}

static uint8_t *rect_header(uint8_t *o, int x, int y, int w, int h, int32_t enc) {
    put16(o, (uint16_t)x); put16(o + 2, (uint16_t)y); put16(o + 4, (uint16_t)w); put16(o + 6, (uint16_t)h);
    put32(o + 8, (uint32_t)enc);
    return o + 12;
}

/* ---- Raw and Hextile --------------------------------------------------------------------- */
static uint8_t *enc_raw(vclient_t *v, uint8_t *o, int rx, int ry, int rw, int rh) {
    for (int y = 0; y < rh; y++) {
        const uint32_t *s = g_front + (size_t)(ry + y) * scr_w + rx;
        if (v->native) { memcpy(o, s, (size_t)rw * 4); o += rw * 4; }
        else for (int x = 0; x < rw; x++) o = put_px(v, o, s[x]);
    }
    return o;
}

static uint8_t *enc_hextile(vclient_t *v, uint8_t *o, int rx, int ry, int rw, int rh) {
    for (int ty = ry; ty < ry + rh; ty += TILE)
        for (int tx = rx; tx < rx + rw; tx += TILE) {
            int tw = rx + rw - tx < TILE ? rx + rw - tx : TILE, th = ry + rh - ty < TILE ? ry + rh - ty : TILE;
            const uint32_t *s = g_front + (size_t)ty * scr_w + tx;
            uint32_t first = s[0];
            int solid = 1;
            for (int y = 0; y < th && solid; y++)
                for (int x = 0; x < tw; x++) if (s[(size_t)y * scr_w + x] != first) { solid = 0; break; }
            if (solid) { *o++ = 2; o = put_px(v, o, first); continue; }
            *o++ = 1;
            for (int y = 0; y < th; y++) {
                if (v->native) { memcpy(o, s + (size_t)y * scr_w, (size_t)tw * 4); o += tw * 4; }
                else for (int x = 0; x < tw; x++) o = put_px(v, o, s[(size_t)y * scr_w + x]);
            }
        }
    return o;
}

/* ---- ZRLE --------------------------------------------------------------------------------- */
static inline uint8_t *put_cpx(vclient_t *v, uint8_t *o, uint32_t c) {
    if (v->native) {
        o[0] = (uint8_t)c; o[1] = (uint8_t)(c >> 8); o[2] = (uint8_t)(c >> 16);
        if (v->cpix == 4) { o[3] = 0; return o + 4; }
        return o + 3;
    }
    if (v->cpix != 3) return put_px(v, o, c);
    uint32_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    uint32_t p = ((r * (uint32_t)v->rmax / 255) << v->rsh) | ((g * (uint32_t)v->gmax / 255) << v->gsh) |
                 ((b * (uint32_t)v->bmax / 255) << v->bsh);
    if (!((v->rmax << v->rsh) < (1 << 24) && (v->gmax << v->gsh) < (1 << 24) && (v->bmax << v->bsh) < (1 << 24))) p >>= 8;
    if (v->big) { o[0] = (uint8_t)(p >> 16); o[1] = (uint8_t)(p >> 8); o[2] = (uint8_t)p; }
    else        { o[0] = (uint8_t)p; o[1] = (uint8_t)(p >> 8); o[2] = (uint8_t)(p >> 16); }
    return o + 3;
}

static inline int rle_len_bytes(int n) { return (n - 1) / 255 + 1; }
static inline uint8_t *put_rle_len(uint8_t *o, int n) {
    n--;
    while (n >= 255) { *o++ = 255; n -= 255; }
    *o++ = (uint8_t)n;
    return o;
}

/* A tile's palette: up to 127 colours in a small hash, stamped per tile so
 * it never needs clearing. */
static uint32_t pal_key[256], pal_stamp[256], stamp;
static uint8_t  pal_idx[256];
static uint32_t pal[128];
static int      npal;

static inline int pal_find(uint32_t c) {
    uint32_t h = (c * 0x9E3779B1u) >> 24;
    while (pal_stamp[h] == stamp) {
        if (pal_key[h] == c) return pal_idx[h];
        h = (h + 1) & 255;
    }
    return -1;
}
static inline int pal_add(uint32_t c) {           /* -1 once it is full    */
    uint32_t h = (c * 0x9E3779B1u) >> 24;
    while (pal_stamp[h] == stamp) {
        if (pal_key[h] == c) return pal_idx[h];
        h = (h + 1) & 255;
    }
    if (npal >= 127) return -1;
    pal_stamp[h] = stamp; pal_key[h] = c; pal_idx[h] = (uint8_t)npal;
    pal[npal] = c;
    return npal++;
}

static uint8_t *zrle_tile(vclient_t *v, uint8_t *o, const uint32_t *s, int stride, int tw, int th) {
    int cp = v->cpix;
    /* One pass: the runs, and the palette while it lasts. */
    stamp++;
    npal = 0;
    int many = 0;
    long plain = 0, palrle = 0;
    uint32_t prev = s[0] & 0xFFFFFFu;
    int run = 0;
    pal_add(prev);
    for (int y = 0; y < th; y++) {
        const uint32_t *row = s + (size_t)y * stride;
        for (int x = 0; x < tw; x++) {
            uint32_t c = row[x] & 0xFFFFFFu;
            if (c == prev) { run++; continue; }
            plain += cp + rle_len_bytes(run);
            palrle += run == 1 ? 1 : 1 + rle_len_bytes(run);
            prev = c; run = 1;
            if (!many && pal_add(c) < 0) many = 1;
        }
    }
    plain += cp + rle_len_bytes(run);
    palrle += run == 1 ? 1 : 1 + rle_len_bytes(run);

    if (!many && npal == 1) { *o++ = 1; return put_cpx(v, o, pal[0]); }
    long best = (long)tw * th * cp;                 /* raw                   */
    int mode = 0;
    if (plain < best) { best = plain; mode = 128; }
    int bits = 0;
    if (!many) {
        long pr = (long)npal * cp + palrle;
        if (pr < best) { best = pr; mode = 130; }
        if (npal <= 16) {
            bits = npal == 2 ? 1 : npal <= 4 ? 2 : 4;
            long pk = (long)npal * cp + (long)th * ((tw * bits + 7) / 8);
            if (pk <= best) { best = pk; mode = 2; }
        }
    }

    if (mode == 0) {                                  /* raw CPIXELs           */
        *o++ = 0;
        for (int y = 0; y < th; y++) {
            const uint32_t *row = s + (size_t)y * stride;
            if (v->native && cp == 4) { memcpy(o, row, (size_t)tw * 4); o += tw * 4; }
            else if (v->native) {
                for (int x = 0; x < tw; x++) {
                    uint32_t c = row[x];
                    o[0] = (uint8_t)c; o[1] = (uint8_t)(c >> 8); o[2] = (uint8_t)(c >> 16); o += 3;
                }
            } else for (int x = 0; x < tw; x++) o = put_cpx(v, o, row[x]);
        }
        return o;
    }
    if (mode == 2) {                                  /* packed palette        */
        *o++ = (uint8_t)npal;
        for (int i = 0; i < npal; i++) o = put_cpx(v, o, pal[i]);
        uint32_t lc = pal[0]; int li = 0;
        for (int y = 0; y < th; y++) {
            const uint32_t *row = s + (size_t)y * stride;
            uint32_t acc = 0; int nb = 0;
            for (int x = 0; x < tw; x++) {
                uint32_t c = row[x] & 0xFFFFFFu;
                if (c != lc) { lc = c; li = pal_find(c); }
                acc = (acc << bits) | (uint32_t)li; nb += bits;
                if (nb == 8) { *o++ = (uint8_t)acc; acc = 0; nb = 0; }
            }
            if (nb) *o++ = (uint8_t)(acc << (8 - nb));
        }
        return o;
    }
    /* runs: plain (128) or through the palette (128 + n) */
    *o++ = (uint8_t)(mode == 128 ? 128 : 128 + npal);
    if (mode != 128) for (int i = 0; i < npal; i++) o = put_cpx(v, o, pal[i]);
    prev = s[0] & 0xFFFFFFu; run = 0;
    for (int y = 0; y < th; y++) {
        const uint32_t *row = s + (size_t)y * stride;
        for (int x = 0; x < tw; x++) {
            uint32_t c = row[x] & 0xFFFFFFu;
            if (c == prev) { run++; continue; }
            if (mode == 128) { o = put_cpx(v, o, prev); o = put_rle_len(o, run); }
            else if (run == 1) *o++ = (uint8_t)pal_find(prev);
            else { *o++ = (uint8_t)(128 | pal_find(prev)); o = put_rle_len(o, run); }
            prev = c; run = 1;
        }
    }
    if (mode == 128) { o = put_cpx(v, o, prev); o = put_rle_len(o, run); }
    else if (run == 1) *o++ = (uint8_t)pal_find(prev);
    else { *o++ = (uint8_t)(128 | pal_find(prev)); o = put_rle_len(o, run); }
    return o;
}

static uint8_t *zscratch;
static int zscratch_cap;
static uint32_t *zband;                           /* one band, tile after tile */

/* The rect's ZRLE data, then wrapped as zlib stored blocks behind its length.
 * Each 64-line band is first copied out row by row, tile after tile: read
 * in place, a tile's rows are 4 KB apart and every one is a cache miss. */
static uint8_t *enc_zrle(vclient_t *v, uint8_t *o, int rx, int ry, int rw, int rh) {
    uint8_t *z = zscratch;
    for (int ty = ry; ty < ry + rh; ty += 64) {
        int th = ry + rh - ty < 64 ? ry + rh - ty : 64;
        for (int r = 0; r < th; r++) {
            const uint32_t *row = g_front + (size_t)(ty + r) * scr_w + rx;
            for (int tx = 0; tx < rw; tx += 64) {
                int tw = rw - tx < 64 ? rw - tx : 64;
                memcpy(zband + (size_t)tx * th + (size_t)r * tw, row + tx, (size_t)tw * 4);
            }
        }
        for (int tx = 0; tx < rw; tx += 64) {
            int tw = rw - tx < 64 ? rw - tx : 64;
            z = zrle_tile(v, z, zband + (size_t)tx * th, tw, tw, th);
        }
    }
    int n = (int)(z - zscratch);
    uint8_t *len_at = o;
    o += 4;
    uint8_t *start = o;
    if (!v->z_started) { *o++ = 0x78; *o++ = 0x01; v->z_started = 1; }
    for (int at = 0; at < n; ) {
        int k = n - at > 65535 ? 65535 : n - at;
        *o++ = 0;                                     /* BFINAL 0, stored      */
        o[0] = (uint8_t)k; o[1] = (uint8_t)(k >> 8); o[2] = (uint8_t)~k; o[3] = (uint8_t)(~k >> 8);
        o += 4;
        memcpy(o, zscratch + at, (size_t)k);
        o += k; at += k;
    }
    put32(len_at, (uint32_t)(o - start));
    return o;
}

/* ---- zlib ---------------------------------------------------------------------------------
 *  Encoding 6: the rect's pixels, as the viewer asked for them, through one
 *  deflate stream per connection. It is what Screen Sharing asks for first.
 *
 *  The compressor is made for screens, not files, and for a 1 GHz core: for
 *  each pixel it looks at two places only -- the pixel before it (a run of
 *  one colour) and the one above it (a row like the last) -- takes the
 *  longer match as a deflate back-reference, and writes everything with
 *  deflate's fixed Huffman codes. A flat screen comes to a few kilobytes. A
 *  rect that doesn't shrink (a photo, a video) goes as stored blocks.
 *  Every rect ends in a sync flush, so it ends on a byte. */
static uint16_t fl_code[288];                   /* fixed literal/length codes, bit-reversed */
static uint8_t  fl_len[288];
static uint16_t len_sym[259];                   /* match length -> symbol (257..285) */
static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                    67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t  lext[29]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t  dext[30]  = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static uint32_t rev(uint32_t c, int n) { uint32_t r = 0; for (int i = 0; i < n; i++) r |= ((c >> i) & 1) << (n - 1 - i); return r; }

static void zl_tables(void) {
    for (int v = 0; v < 288; v++) {
        uint32_t c; int n;
        if (v < 144)      { c = 0x30 + (uint32_t)v;          n = 8; }
        else if (v < 256) { c = 0x190 + (uint32_t)(v - 144); n = 9; }
        else if (v < 280) { c = (uint32_t)(v - 256);         n = 7; }
        else              { c = 0xC0 + (uint32_t)(v - 280);  n = 8; }
        fl_code[v] = (uint16_t)rev(c, n); fl_len[v] = (uint8_t)n;
    }
    for (int l = 3; l <= 258; l++) {
        int i = 28;
        while (lbase[i] > l) i--;
        len_sym[l] = (uint16_t)i;
    }
}

typedef struct { uint8_t *o; uint64_t acc; int n; } bits_t;
typedef uint32_t u32u __attribute__((aligned(1), may_alias));
static inline void bput(bits_t *b, uint32_t v, int n) {
    b->acc |= (uint64_t)v << b->n;
    b->n += n;
    if (b->n >= 32) { *(u32u *)b->o = (uint32_t)b->acc; b->o += 4; b->acc >>= 32; b->n -= 32; }
}
static inline void blit_(bits_t *b, int sym) { bput(b, fl_code[sym], fl_len[sym]); }
static void balign(bits_t *b) {
    while (b->n > 0) { *b->o++ = (uint8_t)b->acc; b->acc >>= 8; b->n -= 8; }
    b->n = 0; b->acc = 0;
}

/* A back-reference of len bytes (any length; split at 258) from dist bytes back. */
static void bmatch(bits_t *b, int len, int dsym, uint32_t dx) {
    while (len > 0) {
        int c = len > 258 ? 258 : len;
        if (len - c > 0 && len - c < 3) c = len - 3;
        int i = len_sym[c];
        blit_(b, 257 + i);
        if (lext[i]) bput(b, (uint32_t)(c - lbase[i]), lext[i]);
        bput(b, rev((uint32_t)dsym, 5), 5);
        if (dext[dsym]) bput(b, dx, dext[dsym]);
        len -= c;
    }
}

static int dist_sym(int d, uint32_t *extra) {
    int j = 29;
    while (dbase[j] > d) j--;
    *extra = (uint32_t)(d - dbase[j]);
    return j;
}

/* The rect deflated into o, as long as it stays under `limit`; NULL if not. */
static uint8_t *zl_deflate(uint8_t *o, uint8_t *limit, int rx, int ry, int rw, int rh) {
    bits_t b = { o, 0, 0 };
    bput(&b, 2, 3);                                   /* BFINAL 0, fixed Huffman */
    uint32_t x1, xu;
    int d1 = dist_sym(4, &x1);
    int du = rw * 4 <= 32768 ? dist_sym(rw * 4, &xu) : -1;
    const uint32_t M = 0xFFFFFF;
    for (int y = 0; y < rh; y++) {
        const uint32_t *row = g_front + (size_t)(ry + y) * scr_w + rx;
        const uint32_t *up = y && du >= 0 ? row - scr_w : 0;
        for (int x = 0; x < rw; ) {
            int n1 = 0, n2 = 0;
            if (x) { uint32_t p = row[x - 1] & M; while (x + n1 < rw && (row[x + n1] & M) == p) n1++; }
            if (up) while (x + n2 < rw && ((row[x + n2] ^ up[x + n2]) & M) == 0) n2++;
            if (!n1 && !n2) {
                uint32_t p = row[x];
                blit_(&b, (int)(p & 255)); blit_(&b, (int)((p >> 8) & 255)); blit_(&b, (int)((p >> 16) & 255)); blit_(&b, 0);
                x++;
                continue;
            }
            if (n1 >= n2) { bmatch(&b, n1 * 4, d1, x1); x += n1; }
            else          { bmatch(&b, n2 * 4, du, xu); x += n2; }
        }
        /* A photo or a video doesn't shrink: tell after a few rows, not at the end. */
        long made = (long)(b.o - o), seen = (long)(y + 1) * rw * 4;
        if (b.o > limit || (y >= 3 && made * 10 > seen * 9)) return 0;
    }
    blit_(&b, 256);                                   /* end of block           */
    bput(&b, 0, 3);                                   /* sync flush: an empty stored block */
    balign(&b);
    b.o[0] = 0; b.o[1] = 0; b.o[2] = 0xFF; b.o[3] = 0xFF;
    return b.o + 4;
}

/* The rect's pixels as stored blocks, row after row. */
static uint8_t *zl_stored(vclient_t *v, uint8_t *o, int rx, int ry, int rw, int rh) {
    int bpp = px_bytes(v), rowb = rw * bpp, total = rowb * rh, y = 0, in_row = 0;
    while (total > 0) {
        int k = total > 65535 ? 65535 : total;
        *o++ = 0;
        o[0] = (uint8_t)k; o[1] = (uint8_t)(k >> 8); o[2] = (uint8_t)~k; o[3] = (uint8_t)(~k >> 8);
        o += 4;
        total -= k;
        while (k > 0) {                               /* fill the block from the rows */
            const uint32_t *s = g_front + (size_t)(ry + y) * scr_w + rx;
            int take = rowb - in_row < k ? rowb - in_row : k;
            if (v->native) memcpy(o, (const uint8_t *)s + in_row, (size_t)take);
            else {
                uint8_t px[4];
                for (int i = 0; i < take; i++) { put_px(v, px, s[(in_row + i) / bpp]); o[i] = px[(in_row + i) % bpp]; }
            }
            o += take; k -= take; in_row += take;
            if (in_row == rowb) { in_row = 0; y++; }
        }
    }
    return o;
}

static uint8_t *enc_zlib(vclient_t *v, uint8_t *o, int rx, int ry, int rw, int rh) {
    uint8_t *len_at = o;
    o += 4;
    uint8_t *start = o;
    if (!v->zl_started) { *o++ = 0x78; *o++ = 0x01; v->zl_started = 1; }
    uint8_t *e = v->native ? zl_deflate(o, o + rw * rh * 4 - 64, rx, ry, rw, rh) : 0;
    o = e ? e : zl_stored(v, o, rx, ry, rw, rh);
    put32(len_at, (uint32_t)(o - start));
    return o;
}

/* ---- what changed ------------------------------------------------------------------------- */
typedef struct { uint16_t x, y, w, h; } vrect_t;
static vrect_t rects[2048];

static inline int tile_owed(vclient_t *v, int tx, int ty) {
    return v->full || (int32_t)(g_tile_gen[ty * scr_tw + tx] - v->gen) > 0;
}

/* ZRLE: per 64-line band, runs of 16-pixel columns with anything owed, each
 * cut down to the tile rows that have it. Hextile and Raw: runs of tiles,
 * a tile row at a time. */
static int collect(vclient_t *v) {
    int n = 0, max = (int)(sizeof rects / sizeof rects[0]);
    int band = v->enc == 16 || v->enc == 6 ? 4 : 1;
    for (int b0 = 0; b0 < scr_th; b0 += band) {
        int b1 = b0 + band < scr_th ? b0 + band : scr_th;
        int run = -1, rlo = 0, rhi = 0;
        for (int tx = 0; tx <= scr_tw; tx++) {
            int lo = -1, hi = -1;
            if (tx < scr_tw)
                for (int ty = b0; ty < b1; ty++) if (tile_owed(v, tx, ty)) { if (lo < 0) lo = ty; hi = ty; }
            if (lo >= 0) {
                if (run < 0) { run = tx; rlo = lo; rhi = hi; }
                else { if (lo < rlo) rlo = lo; if (hi > rhi) rhi = hi; }
                continue;
            }
            if (run < 0) continue;
            if (n < max) {
                int x = run * SCR_TILE, y = rlo * SCR_TILE;
                int x1 = tx * SCR_TILE < scr_w ? tx * SCR_TILE : scr_w;
                int y1 = (rhi + 1) * SCR_TILE < scr_h ? (rhi + 1) * SCR_TILE : scr_h;
                rects[n].x = (uint16_t)x; rects[n].y = (uint16_t)y;
                rects[n].w = (uint16_t)(x1 - x); rects[n].h = (uint16_t)(y1 - y);
                n++;
            }
            run = -1;
        }
    }
    return n;
}

/* Decide what changed and encode all of it into v->out. */
static void vc_begin_update(vclient_t *v) {
    int resize = 0;
    if (v->size_sent && (v->size_sent != ((scr_w << 16) | scr_h))) resize = v->desktop_size;
    if (!v->full && v->gen == g_front_gen && v->cursor_sent == v->cursor && !resize) return;
    uint32_t gen = g_front_gen;
    int n = collect(v);
    int extra = (v->cursor && !v->cursor_sent) + resize;
    v->gen = gen;
    v->full = 0;
    if (!n && !extra) return;

    /* At worst every pixel raw, plus headers, stored-block framing and the cursor. */
    int need = scr_w * scr_h * 4 + (scr_w * scr_h * 4 / 65535 + 1) * 5 + n * 32 + 8192;
    if (v->out_cap < need) {
        if (v->out) kfree(v->out);
        v->out = kmalloc((uint64_t)need);
        v->out_cap = v->out ? need : 0;
        if (!v->out) { v->full = 1; return; }
    }
    if (v->enc == 16 && zscratch_cap < scr_w * scr_h * 4 + 4096) {
        if (zscratch) kfree(zscratch);
        if (zband) kfree(zband);
        zscratch_cap = scr_w * scr_h * 4 + 4096;
        zscratch = kmalloc((uint64_t)zscratch_cap);
        zband = kmalloc((uint64_t)scr_w * 64 * 4);
        if (!zscratch || !zband) { zscratch_cap = 0; v->enc = 0; }
    }
    uint8_t *o = v->out;
    o[0] = 0; o[1] = 0; put16(o + 2, (uint16_t)(n + extra));
    o += 4;
    if (resize) o = rect_header(o, 0, 0, scr_w, scr_h, -223);
    if (v->cursor && !v->cursor_sent) o = cursor_rect(v, o);
    v->size_sent = (scr_w << 16) | scr_h;
    for (int i = 0; i < n; i++) {
        int rx = rects[i].x, ry = rects[i].y, rw = rects[i].w, rh = rects[i].h;
        if (v->enc == 16)     { o = rect_header(o, rx, ry, rw, rh, 16); o = enc_zrle(v, o, rx, ry, rw, rh); }
        else if (v->enc == 6) { o = rect_header(o, rx, ry, rw, rh, 6);  o = enc_zlib(v, o, rx, ry, rw, rh); }
        else if (v->enc == 5) { o = rect_header(o, rx, ry, rw, rh, 5);  o = enc_hextile(v, o, rx, ry, rw, rh); }
        else                 { o = rect_header(o, rx, ry, rw, rh, 0);  o = enc_raw(v, o, rx, ry, rw, rh); }
    }
    v->out_len = (int)(o - v->out);
    v->p_updates++; v->p_bytes += (uint64_t)v->out_len;
    v->out_pos = 0;
    v->out_rects = n;
    v->want = 0;
    v->updates++;
}

/* Stream as much of the current update as the send ring will take. */
static void vc_pump(vclient_t *v) {
    while (v->out_pos < v->out_len) {
        int room = tcp_space(v->c);
        if (room <= 0) return;
        int k = v->out_len - v->out_pos < room ? v->out_len - v->out_pos : room;
        int w = tcp_write(v->c, v->out + v->out_pos, k);
        if (w <= 0) return;
        v->out_pos += w;
    }
}

int vnc_init(uint16_t port) {
    vnc_port = port;
    if (!vnc_des_selftest()) { kprintf("[vnc] DES self-test failed: no VNC\n"); return -1; }
    zl_tables();
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
        if (v->cut_left && v->inlen) {                    /* the rest of a long clipboard */
            uint32_t k = v->cut_left < (uint32_t)v->inlen ? v->cut_left : (uint32_t)v->inlen;
            memcpy(v->cut + v->cut_len, v->in, k);
            v->cut_len += k; v->cut_left -= k;
            memmove(v->in, v->in + k, (size_t)v->inlen - k); v->inlen -= (int)k;
            if (!v->cut_left) { clip_from_viewer(v->cut, (int)v->cut_len); v->cut_sent = clip_gen(); kfree(v->cut); v->cut = 0; }
        }
        for (;;) {
            if (v->cut_left) break;
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
        /* something copied on the Pi: to the viewer, between updates */
        uint32_t cg = clip_gen();
        int cn; const char *ct = clip_get(&cn);
        if (cn > 60000) cn = 60000;
        if (cg && cg != v->cut_sent && v->out_pos >= v->out_len && tcp_space(v->c) >= 8 + cn) {
            uint8_t hdr[8] = { 3, 0, 0, 0, (uint8_t)(cn >> 24), (uint8_t)(cn >> 16), (uint8_t)(cn >> 8), (uint8_t)cn };
            tcp_write(v->c, hdr, 8);
            if (cn) tcp_write(v->c, ct, cn);
            v->cut_sent = cg;
        }
        uint64_t t0 = now_us();
        if (v->out_pos < v->out_len) vc_pump(v);
        else if (v->want) {
            uint32_t u = v->updates;
            vc_begin_update(v);
            uint64_t t1 = now_us();
            if (v->updates != u) {
                g_perf.vnc_updates++; g_perf.vnc_rects += (uint32_t)v->out_rects;
                g_perf.vnc_scan_us += t1 - t0;
                if (t1 - t0 > g_perf.vnc_scan_max) g_perf.vnc_scan_max = t1 - t0;
            }
            t0 = t1;
            vc_pump(v);
        }
        g_perf.vnc_pump_us += now_us() - t0;
    }
}
