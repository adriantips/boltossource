/* Overlay drawing. All of it runs at panel resolution after the 3D has been
 * upscaled, which is the only reason the text is readable at scale 4. */
#include "craft.h"

extern const unsigned char font8x8_basic[128][8];

void hud_rect(uint32_t *fb, int fbw, int fbh, int x, int y, int w, int h, uint32_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fbw) w = fbw - x;
    if (y + h > fbh) h = fbh - y;
    for (int r = 0; r < h; r++) {
        uint32_t *d = fb + (size_t)(y + r) * fbw + x;
        for (int i = 0; i < w; i++) d[i] = c;
    }
}

static void glyph(uint32_t *fb, int fbw, int fbh, int x, int y, char ch,
                  uint32_t c, int s) {
    unsigned u = (unsigned char)ch;
    if (u > 127) u = '?';

    /* Scale 1 is most of the text on screen and used to go through hud_rect
     * once per lit pixel -- a clipped 1x1 fill, call and all. Four lines of
     * help is a few thousand of those every frame. */
    if (s == 1) {
        if (x < 0 || y < 0 || x + 8 > fbw || y + 8 > fbh) return;
        for (int row = 0; row < 8; row++) {
            unsigned bits = font8x8_basic[u][row];
            if (!bits) continue;
            uint32_t *d = fb + (size_t)(y + row) * fbw + x;
            for (int col = 0; col < 8; col++)
                if (bits & (1u << col)) d[col] = c;
        }
        return;
    }
    for (int row = 0; row < 8; row++) {
        unsigned bits = font8x8_basic[u][row];
        if (!bits) continue;
        for (int col = 0; col < 8; col++)
            if (bits & (1u << col))
                hud_rect(fb, fbw, fbh, x + col * s, y + row * s, s, s, c);
    }
}

void hud_text(uint32_t *fb, int fbw, int fbh, int x, int y, const char *s,
              uint32_t c, int scale) {
    for (; *s; s++) {
        if (*s != ' ') glyph(fb, fbw, fbh, x, y, *s, c, scale);
        x += 8 * scale;
    }
}

/* Same text twice, offset by one. Without it the white lines vanish against
 * snow and the dark ones vanish against a cave wall. */
void hud_text_shadow(uint32_t *fb, int fbw, int fbh, int x, int y, const char *s,
                     uint32_t c, int scale) {
    hud_text(fb, fbw, fbh, x + scale, y + scale, s, 0x00000000u, scale);
    hud_text(fb, fbw, fbh, x, y, s, c, scale);
}

/* The hotbar tile: the block's side texture, nearest-sampled up to `side`
 * pixels, composited over whatever is already there so glass and leaves keep
 * their holes. */
void hud_block_icon(uint32_t *fb, int fbw, int fbh, int x, int y, int side, uint8_t b) {
    const uint32_t *t = g_tex[b][F_SIDE];
    int seethru = blk_seethru[b];

    /* One divide per column instead of one per pixel: ten tiles a frame is
     * fifteen thousand of them otherwise, and division is not free anywhere,
     * least of all inside an emulator. */
    unsigned char map[256];
    int n = side > 256 ? 256 : side;
    for (int i = 0; i < n; i++) map[i] = (unsigned char)(i * TEXW / side);

    for (int r = 0; r < n; r++) {
        int ty = map[r];
        int py = y + r;
        if (py < 0 || py >= fbh) continue;
        uint32_t *d = fb + (size_t)py * fbw;
        for (int i = 0; i < n; i++) {
            int px = x + i;
            if (px < 0 || px >= fbw) continue;
            uint32_t texel = t[ty * TEXW + map[i]];
            int a = seethru ? (int)(texel >> 24) : 255;
            if (!a) continue;
            uint32_t c = texel & 0x00FFFFFFu;
            /* a touch of the face shading so the tiles match the world */
            c = scale32(c, 232);
            d[px] = a == 255 ? c : mix32(d[px], c, a);
        }
    }
}
