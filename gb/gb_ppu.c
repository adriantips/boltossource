/* ===========================================================================
 *  The picture processing unit, one scanline at a time.
 *
 *  Not a pixel FIFO. The real PPU builds each line three pixels at a time in
 *  a shifter whose timing can be perturbed by mid-line register writes, and a
 *  handful of demos depend on that; games do not. What games do depend on is
 *  the mode timing being right -- 80 T-states of OAM search, 172 of transfer,
 *  204 of blanking, 456 to the line -- because that is what their raster
 *  effects and their STAT interrupts are counting.
 *
 *  So: the line is drawn in one go at the start of mode 0, with whatever the
 *  registers said at that moment, and the mode flags and interrupts move on
 *  the real boundaries.
 * ===========================================================================*/
#include <stdint.h>
#include "gb.h"

#define LCDC_BG_ON    0x01
#define LCDC_OBJ_ON   0x02
#define LCDC_OBJ_16   0x04
#define LCDC_BG_MAP   0x08
#define LCDC_TILE_LO  0x10
#define LCDC_WIN_ON   0x20
#define LCDC_WIN_MAP  0x40
#define LCDC_ON       0x80

/* Which of the four shades a two-bit colour maps to, through a palette
 * register. Three registers, same three lines of arithmetic. */
static uint8_t pal(uint8_t reg, int color) { return (uint8_t)((reg >> (color * 2)) & 3); }

static uint8_t vram(gb_t *g, uint16_t addr) { return g->vram[addr & 0x1FFF]; }

static void draw_line(gb_t *gb) {
    int ly = gb->io[R_LY];
    if (ly >= GB_H) return;

    uint8_t lcdc = gb->io[R_LCDC];
    uint8_t *out = &gb->fb[ly * GB_W];

    /* The raw (pre-palette) background colour of each pixel, kept because
     * sprite priority is decided against colour 0, not against the shade the
     * palette happened to map it to. */
    uint8_t bgraw[GB_W];

    if (!(lcdc & LCDC_BG_ON)) {
        for (int x = 0; x < GB_W; x++) { out[x] = 0; bgraw[x] = 0; }
    } else {
        uint16_t map = (lcdc & LCDC_BG_MAP) ? 0x9C00 : 0x9800;
        int signed_tiles = !(lcdc & LCDC_TILE_LO);
        int y = (ly + gb->io[R_SCY]) & 0xFF;
        uint16_t rowbase = (uint16_t)(map + (y / 8) * 32);
        int ty = (y & 7) * 2;

        for (int x = 0; x < GB_W; x++) {
            int xx = (x + gb->io[R_SCX]) & 0xFF;
            uint8_t idx = vram(gb, (uint16_t)(rowbase + xx / 8));
            uint16_t tile = signed_tiles ? (uint16_t)(0x9000 + (int8_t)idx * 16)
                                         : (uint16_t)(0x8000 + idx * 16);
            uint8_t lo = vram(gb, (uint16_t)(tile + ty));
            uint8_t hi = vram(gb, (uint16_t)(tile + ty + 1));
            int b = 7 - (xx & 7);
            int c = (((hi >> b) & 1) << 1) | ((lo >> b) & 1);
            bgraw[x] = (uint8_t)c;
            out[x] = pal(gb->io[R_BGP], c);
        }
    }

    /* The window. It has its own line counter, which only advances on lines
     * where it was actually visible -- a window switched off for ten lines and
     * back on resumes where it left off, and getting this wrong tears every
     * status bar in the library. */
    if ((lcdc & LCDC_WIN_ON) && (lcdc & LCDC_BG_ON) &&
        ly >= gb->io[R_WY] && gb->io[R_WX] <= 166) {
        int wx = (int)gb->io[R_WX] - 7;
        if (wx < GB_W) {
            uint16_t map = (lcdc & LCDC_WIN_MAP) ? 0x9C00 : 0x9800;
            int signed_tiles = !(lcdc & LCDC_TILE_LO);
            int wy = gb->win_line;
            uint16_t rowbase = (uint16_t)(map + (wy / 8) * 32);
            int ty = (wy & 7) * 2;

            for (int x = wx < 0 ? 0 : wx; x < GB_W; x++) {
                int xx = x - wx;
                uint8_t idx = vram(gb, (uint16_t)(rowbase + xx / 8));
                uint16_t tile = signed_tiles ? (uint16_t)(0x9000 + (int8_t)idx * 16)
                                             : (uint16_t)(0x8000 + idx * 16);
                uint8_t lo = vram(gb, (uint16_t)(tile + ty));
                uint8_t hi = vram(gb, (uint16_t)(tile + ty + 1));
                int b = 7 - (xx & 7);
                int c = (((hi >> b) & 1) << 1) | ((lo >> b) & 1);
                bgraw[x] = (uint8_t)c;
                out[x] = pal(gb->io[R_BGP], c);
            }
            gb->win_line++;
        }
    }

    if (!(lcdc & LCDC_OBJ_ON)) return;

    /* Ten sprites per line, taken in OAM order -- the eleventh is dropped even
     * if it would have been on top. Priority among those ten goes to the
     * smallest X, and to the lowest OAM index when two share an X. */
    int h = (lcdc & LCDC_OBJ_16) ? 16 : 8;
    int cand[10], n = 0;
    for (int i = 0; i < 40 && n < 10; i++) {
        int sy = gb->oam[i * 4] - 16;
        if (ly < sy || ly >= sy + h) continue;
        cand[n++] = i;
    }
    /* Insertion sort into priority order, then draw worst-first so the best
     * ends up on top without a per-pixel priority test. */
    for (int i = 1; i < n; i++) {
        int k = cand[i], j = i - 1;
        while (j >= 0 && (gb->oam[cand[j] * 4 + 1] > gb->oam[k * 4 + 1] ||
                         (gb->oam[cand[j] * 4 + 1] == gb->oam[k * 4 + 1] && cand[j] > k))) {
            cand[j + 1] = cand[j];
            j--;
        }
        cand[j + 1] = k;
    }

    for (int s = n - 1; s >= 0; s--) {
        int i = cand[s];
        int sy = gb->oam[i * 4] - 16;
        int sx = gb->oam[i * 4 + 1] - 8;
        uint8_t tile = gb->oam[i * 4 + 2];
        uint8_t at = gb->oam[i * 4 + 3];
        if (h == 16) tile &= 0xFE;               /* an 8x16 sprite ignores bit 0 */

        int row = ly - sy;
        if (at & 0x40) row = h - 1 - row;         /* vertical flip               */
        uint16_t addr = (uint16_t)(0x8000 + tile * 16 + row * 2);
        uint8_t lo = vram(gb, addr), hi = vram(gb, (uint16_t)(addr + 1));
        uint8_t reg = (at & 0x10) ? gb->io[R_OBP1] : gb->io[R_OBP0];

        for (int px = 0; px < 8; px++) {
            int x = sx + px;
            if (x < 0 || x >= GB_W) continue;
            int b = (at & 0x20) ? px : 7 - px;    /* horizontal flip             */
            int c = (((hi >> b) & 1) << 1) | ((lo >> b) & 1);
            if (c == 0) continue;                 /* colour 0 is transparent     */
            /* Attribute bit 7 puts the sprite behind every background colour
             * except 0 -- not behind the background as a whole. */
            if ((at & 0x80) && bgraw[x] != 0) continue;
            out[x] = pal(reg, c);
        }
    }
}

void gb_ppu_step(gb_t *gb, int t) {
    uint8_t lcdc = gb->io[R_LCDC];

    if (!(lcdc & LCDC_ON)) {
        /* The LCD is off: LY reads 0 and the mode reads 0. Keep counting
         * anyway and hand the caller a frame at the usual rate, or a ROM that
         * leaves the screen off while it decompresses looks like a hang. */
        gb->io[R_LY] = 0;
        gb->io[R_STAT] = (uint8_t)(gb->io[R_STAT] & ~0x03);
        gb->win_line = 0;
        gb->line_t += t;
        if (gb->line_t >= GB_FRAME_T) { gb->line_t = 0; gb->frame_ready = 1; }
        return;
    }

    gb->line_t += t;

    int ly = gb->io[R_LY];
    int old_mode = gb->io[R_STAT] & 3;
    int mode;
    if (ly >= GB_H)            mode = 1;          /* vblank                     */
    else if (gb->line_t < 80)  mode = 2;          /* searching OAM              */
    else if (gb->line_t < 252) mode = 3;          /* handing pixels to the LCD  */
    else                       mode = 0;          /* blanking                   */

    if (mode != old_mode) {
        gb->io[R_STAT] = (uint8_t)((gb->io[R_STAT] & ~0x03) | mode);
        /* The line is drawn on the 3->0 edge: everything that was going to
         * affect it has been written by then. */
        if (mode == 0) draw_line(gb);
        if ((mode == 0 && (gb->io[R_STAT] & 0x08)) ||
            (mode == 2 && (gb->io[R_STAT] & 0x20)) ||
            (mode == 1 && (gb->io[R_STAT] & 0x10)))
            gb_irq(gb, 1);
    }

    while (gb->line_t >= GB_LINE_T) {
        gb->line_t -= GB_LINE_T;
        ly = (ly + 1) % 154;
        gb->io[R_LY] = (uint8_t)ly;

        if (ly == GB_H) {
            gb_irq(gb, 0);
            gb->frame_ready = 1;
            if (gb->io[R_STAT] & 0x10) gb_irq(gb, 1);
        }
        if (ly == 0) gb->win_line = 0;

        /* LY == LYC is latched into STAT bit 2 and can raise its own
         * interrupt. Games use it as a scanline trigger far more than they
         * use mode 0. */
        int coin = (ly == gb->io[R_LYC]);
        gb->io[R_STAT] = (uint8_t)((gb->io[R_STAT] & ~0x04) | (coin ? 0x04 : 0));
        if (coin && (gb->io[R_STAT] & 0x40)) gb_irq(gb, 1);
    }
}
