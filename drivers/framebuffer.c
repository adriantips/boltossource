#include <stdint.h>
#include "framebuffer.h"
#include "io.h"
#include "mm.h"

/* The VESA linear framebuffer the bootloader selected. It lives below 4 GiB
 * and is already identity-mapped, so we write straight to it. 32 bpp / xRGB. */
static volatile uint32_t *fb;
static uint64_t fb_pa;                  /* physical base of the LFB */
static uint32_t fw, fh, pitch_px;

/* Set while a ring-3 program (the userland browser) owns the screen: the GUI
 * compositor checks it and stops touching VRAM so the two never fight over the
 * panel. Cleared by SYS_FBEND or when that process exits. */
volatile int g_fb_exclusive;

void fb_init(struct bootinfo *bi) {
    if (!bi->fb_addr || !bi->fb_width || bi->fb_bpp != 32) { fb = 0; return; }
    fb_pa    = (uint64_t)bi->fb_addr;
    fb       = (volatile uint32_t *)P2V(fb_pa);
    fw       = bi->fb_width;
    fh       = bi->fb_height;
    pitch_px = bi->fb_pitch ? bi->fb_pitch / 4 : bi->fb_width;
}

uint64_t fb_phys(void)     { return fb_pa; }
uint32_t fb_pitch_px(void) { return pitch_px; }

/* ---- Bochs/QEMU DISPI runtime mode switch ------------------------------- *
 * QEMU's default "std" (Bochs) VGA exposes the DISPI register interface on I/O
 * ports 0x01CE (index) / 0x01CF (data). Writing XRES/YRES/BPP and re-enabling
 * with the LFB bit gives a real native-resolution linear framebuffer at the
 * same BAR (so the base pointer never moves). No BIOS, no real-mode trip, no
 * upscaling and no letterbox bars -- the panel itself becomes the new size. */
#define DISPI_INDEX   0x01CE
#define DISPI_DATA    0x01CF
#define DI_XRES       1
#define DI_YRES       2
#define DI_BPP        3
#define DI_ENABLE     4
#define DI_VIRT_W     6
#define DI_BANK       5
#define DI_X_OFF      8
#define DI_Y_OFF      9
#define DI_EN_ENABLED 0x01
#define DI_EN_LFB     0x40

static void dispi_w(uint16_t idx, uint16_t val) { outw(DISPI_INDEX, idx); outw(DISPI_DATA, val); }
static uint16_t dispi_r(uint16_t idx) { outw(DISPI_INDEX, idx); return inw(DISPI_DATA); }

/* Reprogram the panel to w*h, 32bpp, linear. Returns 0 on success. The LFB BAR
 * is unchanged so `fb` stays valid; we only update the cached geometry. */
int fb_set_mode(uint32_t w, uint32_t h) {
    if (!fb || !w || !h) return -1;
    dispi_w(DI_ENABLE, 0);                 /* disable while reprogramming      */
    dispi_w(DI_XRES,   (uint16_t)w);
    dispi_w(DI_YRES,   (uint16_t)h);
    dispi_w(DI_BPP,    32);
    dispi_w(DI_VIRT_W, (uint16_t)w);       /* pitch = width (no padding)       */
    dispi_w(DI_BANK,   0);
    dispi_w(DI_X_OFF,  0);
    dispi_w(DI_Y_OFF,  0);
    dispi_w(DI_ENABLE, DI_EN_ENABLED | DI_EN_LFB);
    /* verify the card accepted it; on a non-DISPI adapter the readback differs */
    if (dispi_r(DI_XRES) != (uint16_t)w || dispi_r(DI_YRES) != (uint16_t)h) return -1;
    fw = w; fh = h; pitch_px = w;
    return 0;
}

int      fb_present(void) { return fb != 0; }
uint32_t fb_width(void)   { return fw; }
uint32_t fb_height(void)  { return fh; }

void fb_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x < fw && y < fh) fb[(uint64_t)y * pitch_px + x] = color;
}

void fb_fill(uint32_t color) {
    for (uint32_t y = 0; y < fh; y++)
        for (uint32_t x = 0; x < fw; x++)
            fb[(uint64_t)y * pitch_px + x] = color;
}

void fb_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t j = 0; j < h; j++)
        for (uint32_t i = 0; i < w; i++)
            fb_pixel(x + i, y + j, color);
}

/* Blit a tightly-packed fw*fh xRGB image (the GUI compositor's backbuffer)
 * straight onto the visible framebuffer, one scanline at a time so a pitch
 * wider than the width is handled. 32-bit stores keep it fast in a freestanding
 * build with no SSE memcpy. */
/* ---- software dimming ---------------------------------------------------- *
 * Three table lookups a pixel, and a branch that skips all of it at 100 --
 * which is where it sits unless somebody asks for something else. A multiply
 * per channel per pixel would be the same arithmetic done 786,000 times a
 * frame instead of 256 times per level change. */
static uint8_t dim_lut[256];
static int     dim_pct = 100;

void fb_set_dim(int pct) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    dim_pct = pct;
    for (int i = 0; i < 256; i++) dim_lut[i] = (uint8_t)((i * pct) / 100);
}

int fb_dim(void) { return dim_pct; }

static inline uint32_t dim_px(uint32_t c) {
    return ((uint32_t)dim_lut[(c >> 16) & 0xFF] << 16) |
           ((uint32_t)dim_lut[(c >> 8)  & 0xFF] << 8)  |
            (uint32_t)dim_lut[c & 0xFF];
}

/* A dword loop through a volatile pointer is the slowest possible way to
 * move a frame under QEMU's TCG, which has a fast path for rep movs and none
 * for that. VRAM is ordinary write-combined memory as far as this copy is
 * concerned; the volatile was only ever there to stop the compiler being
 * clever, and rep movs cannot be. */
static inline void vram_copy(volatile uint32_t *d, const uint32_t *s, uint32_t n) {
    uint64_t c = n;
    uint32_t *dd = (uint32_t *)d;
    __asm__ volatile("rep movsl" : "+D"(dd), "+S"(s), "+c"(c) : : "memory");
}

void fb_blit(const uint32_t *src) {
    if (!fb) return;
    for (uint32_t y = 0; y < fh; y++) {
        volatile uint32_t *d = fb  + (uint64_t)y * pitch_px;
        const uint32_t    *s = src + (uint64_t)y * fw;
        if (dim_pct >= 100) vram_copy(d, s, fw);
        else                { for (uint32_t x = 0; x < fw; x++) d[x] = dim_px(s[x]); }
    }
}

/* Copy one rectangle of the logical desktop out to the panel -- the
 * compositor's damage, or the few pixels under the pointer. `src` addresses
 * logical (x, y); the desktop is lw*lh and lands on the panel at (ox, oy,
 * ow, oh), which is a plain copy at 100% and a nearest-neighbour stretch
 * when display scaling is on. The column mapping is fb_blit_scaled's, so a
 * rectangle drawn here meets one drawn there without a seam. */
void fb_present_rect(const uint32_t *src, int stride, int x, int y, int w, int h,
                     int lw, int lh, int ox, int oy, int ow, int oh) {
    if (!fb || !src || w <= 0 || h <= 0 || lw <= 0 || lh <= 0 || ow <= 0 || oh <= 0) return;
    if (ow == lw && oh == lh) {
        int px0 = ox + x, px1 = ox + x + w;
        if (px0 < 0) px0 = 0;
        if (px1 > (int)fw) px1 = (int)fw;
        if (px1 <= px0) return;
        for (int j = 0; j < h; j++) {
            int py = oy + y + j;
            if (py < 0 || py >= (int)fh) continue;
            volatile uint32_t *d = fb + (uint64_t)py * pitch_px + px0;
            const uint32_t *s = src + (int64_t)j * stride + (px0 - (ox + x));
            uint32_t n = (uint32_t)(px1 - px0);
            if (dim_pct >= 100) vram_copy(d, s, n);
            else for (uint32_t i = 0; i < n; i++) d[i] = dim_px(s[i]);
        }
        return;
    }
    int px0 = ox + (int)((int64_t)x * ow / lw);
    int px1 = ox + (int)(((int64_t)(x + w) * ow + lw - 1) / lw);
    int py0 = oy + (int)((int64_t)y * oh / lh);
    int py1 = oy + (int)(((int64_t)(y + h) * oh + lh - 1) / lh);
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > (int)fw) px1 = (int)fw;
    if (py1 > (int)fh) py1 = (int)fh;
    for (int py = py0; py < py1; py++) {
        int sy = (int)((int64_t)(py - oy) * lh / oh);
        if (sy < y) sy = y;
        if (sy >= y + h) sy = y + h - 1;
        const uint32_t *srow = src + (int64_t)(sy - y) * stride;
        volatile uint32_t *d = fb + (uint64_t)py * pitch_px;
        for (int px = px0; px < px1; px++) {
            int sx = (int)((int64_t)(px - ox) * lw / ow);
            if (sx < x) sx = x;
            if (sx >= x + w) sx = x + w - 1;
            uint32_t c = srow[sx - x];
            d[px] = dim_pct >= 100 ? c : dim_px(c);
        }
    }
}

/* The other direction: read the panel back into a packed fw*fh buffer.
 *
 * A screenshot taken from the compositor's backbuffer would be a picture of
 * what the compositor drew, which is not the same thing as what is on screen
 * -- DOOM and the ring-3 browser own the panel outright, the text console
 * never goes through the compositor at all, and the letterbox bars belong to
 * the panel. Reading VRAM is slower than reading the backbuffer, and it is the
 * only version of this that is telling the truth.
 *
 * Alpha is forced opaque: the panel is xRGB and a PNG writer would otherwise
 * see whatever the hardware leaves in the top byte. */
/* Undoing the software dim is approximate -- at 10% each channel carries four
 * bits of what it started with and the banding shows -- but a screenshot that
 * is merely a bit posterised beats one that is black. */
static inline uint32_t undim_ch(uint32_t v) {
    uint32_t r = v * 100u / (uint32_t)dim_pct;
    return r > 255 ? 255 : r;
}

int fb_capture(uint32_t *dst) {
    if (!fb || !dst) return -1;
    for (uint32_t y = 0; y < fh; y++) {
        const volatile uint32_t *s = fb + (uint64_t)y * pitch_px;
        uint32_t *d = dst + (uint64_t)y * fw;
        if (dim_pct >= 100) {
            for (uint32_t x = 0; x < fw; x++) d[x] = s[x] | 0xFF000000u;
        } else {
            for (uint32_t x = 0; x < fw; x++) {
                uint32_t c = s[x];
                d[x] = 0xFF000000u | (undim_ch((c >> 16) & 0xFF) << 16)
                                   | (undim_ch((c >> 8) & 0xFF) << 8)
                                   |  undim_ch(c & 0xFF);
            }
        }
    }
    return 0;
}

/* Scale a logical sw*sh desktop into the (dx,dy,dw,dh) rect of the panel and
 * paint black bars around it. A per-column source-index map is built once so
 * the inner loop is array lookups, not a divide per pixel. */
void fb_blit_scaled(const uint32_t *src, uint32_t sw, uint32_t sh,
                    uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh) {
    if (!fb) return;
    /* Letterbox bars, when there are any. Display scaling fills the panel edge
     * to edge, and clearing 8 million pixels a frame to then overwrite every
     * one of them was most of the cost of running scaled at 4K. */
    if (dx || dy || dx + dw < fw || dy + dh < fh) {
        for (uint32_t y = 0; y < fh; y++) {
            volatile uint32_t *d = fb + (uint64_t)y * pitch_px;
            for (uint32_t x = 0; x < fw; x++) d[x] = 0;
        }
    }
    if (!sw || !sh || !dw || !dh || dx >= fw || dy >= fh) return;

    static uint16_t colmap[4096];
    uint32_t w = dw;
    if (dx + w > fw) w = fw - dx;
    if (w > 4096)    w = 4096;
    for (uint32_t i = 0; i < w; i++) colmap[i] = (uint16_t)((uint64_t)i * sw / dw);

    for (uint32_t j = 0; j < dh; j++) {
        uint32_t py = dy + j;
        if (py >= fh) break;
        uint32_t sy = (uint32_t)((uint64_t)j * sh / dh);
        const uint32_t    *s = src + (uint64_t)sy * sw;
        volatile uint32_t *d = fb + (uint64_t)py * pitch_px + dx;
        if (dim_pct >= 100) for (uint32_t i = 0; i < w; i++) d[i] = s[colmap[i]];
        else                for (uint32_t i = 0; i < w; i++) d[i] = dim_px(s[colmap[i]]);
    }
}
