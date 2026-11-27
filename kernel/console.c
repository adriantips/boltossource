#include <stdint.h>
#include "console.h"
#include "klog.h"
#include "kprintf.h"
#include "serial.h"
#include "framebuffer.h"
#include "pit.h"
#include "io.h"
#include "boot.h"
#include "boltmark.h"

extern const unsigned char font8x8_basic[128][8];
extern const unsigned char font8x8_latin1[128][8];  /* mkfont.py, 0x80..0xFF */

#define SCALE   2
#define CELL    (8 * SCALE)        /* 16x16 px per character cell */
#define MARGIN  16
#define MAXCOLS 200
#define MAXROWS 80

static uint32_t W, H, bottom;      /* screen size; text clips above the taskbar */
static uint32_t cols, rows;        /* text grid size, in cells */
static uint32_t ccol, crow;        /* cursor position, in cells */
static uint32_t fg = 0xF0F0F5;     /* BoltUI --text-primary */
static int on_fb = 0;

/* When the GUI takes over the screen it detaches the text console (so stray
 * kprintf / console_clear never scribble on the compositor's framebuffer) and
 * installs a sink so command output can be captured into a terminal window. */
static void (*sink)(char) = 0;
void console_set_sink(void (*fn)(char)) { sink = fn; }
void (*console_get_sink(void))(char) { return sink; }
void console_detach(void) { on_fb = 0; }

static char grid[MAXROWS][MAXCOLS];

/* cursor blink */
static int      cur_on = 0;
static uint64_t cur_last = 0;

static uint32_t lerp_color(uint32_t a, uint32_t b, int n, int d) {
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = ar + (br - ar) * n / d, g = ag + (bg - ag) * n / d, c = ab + (bb - ab) * n / d;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)c;
}

/* A normal boot keeps quiet: the screen is black with the BoltOS mark unlit
 * in the middle, and the boot log goes to the serial port and `dmesg` only,
 * until gui_login.c lights the mark up. Safe and recovery boots show the log
 * as text, and so does a boot that goes wrong (console_reveal). */
static int quiet;

/* The mark, straight onto the panel, over a flat colour. Before the heap
 * exists, so one row at a time. */
static void fb_mark(int x, int y, int size, uint32_t col, int a, uint32_t bg) {
    static uint8_t row[2048];
    if (size <= 0 || size > (int)sizeof row) return;
    bm_pose_t p = BM_STILL;
    p.stroke = bm_stroke_for(size);
    for (int j = 0; j < size; j++) {
        boltmark_raster(&p, (float)size, 0, j, size, 1, row, 0, 0, size);
        for (int i = 0; i < size; i++)
            if (row[i] && x + i >= 0 && y + j >= 0)
                fb_pixel((uint32_t)(x + i), (uint32_t)(y + j), lerp_color(bg, col, row[i] * a, 255 * 255));
    }
}

void console_boot_mark(int W, int H, int *x, int *y, int *size) {
    *size = H * 22 / 100;
    boltmark_center(W / 2, H * 46 / 100, *size, x, y);
}

/* Background tint for a cell, sampled at its vertical center (one color per
 * cell keeps scroll-repaint cheap; the gradient is subtle enough not to band). */
static uint32_t bg_at(uint32_t py) {
    return lerp_color(0x0F0C29, 0x24243E, (int)(py + CELL / 2), (int)H);
}

/* Repaint one cell: restore its background, then stamp its glyph (if any). */
static void draw_cell(uint32_t col, uint32_t row) {
    if (quiet) return;
    uint32_t px = MARGIN + col * CELL, py = MARGIN + row * CELL;
    fb_rect(px, py, CELL, CELL, bg_at(py));
    unsigned char ch = (unsigned char)grid[row][col];
    if (ch >= 32 && ch < 128) {
        /* Used to be `ch & 0x7F`, which drew 'e' for an e-acute rather than
         * nothing -- a mask, not a decision. Latin-1 has a glyph now. */
        const unsigned char *g = (unsigned char)ch >= 0x80
                               ? font8x8_latin1[(unsigned char)ch - 0x80]
                               : font8x8_basic[(unsigned char)ch];
        for (int ry = 0; ry < 8; ry++) {
            unsigned char bits = g[ry];
            for (int rx = 0; rx < 8; rx++)
                if (bits & (1u << rx))
                    fb_rect(px + rx * SCALE, py + ry * SCALE, SCALE, SCALE, fg);
        }
    }
}

static void cursor_erase(void) {
    if (cur_on) { cur_on = 0; if (on_fb) draw_cell(ccol, crow); }
}
static void cursor_draw(void) {
    if (!on_fb || quiet) return;
    uint32_t px = MARGIN + ccol * CELL, py = MARGIN + crow * CELL;
    fb_rect(px, py + CELL - 3, CELL - 2, 2, fg);     /* underline */
    cur_on = 1;
}

void console_cursor_tick(void) {
    if (!on_fb) return;
    uint64_t t = pit_ticks();                        /* PIT @ 1000 Hz -> ms */
    if (t - cur_last >= 500) {
        cur_last = t;
        if (cur_on) cursor_erase(); else cursor_draw();
    }
}

static void repaint(void) {
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < cols; c++)
            draw_cell(c, r);
}

static void scroll(void) {
    for (uint32_t r = 1; r < rows; r++)
        for (uint32_t c = 0; c < cols; c++)
            grid[r - 1][c] = grid[r][c];
    for (uint32_t c = 0; c < cols; c++) grid[rows - 1][c] = 0;
    if (on_fb) repaint();
    crow = rows - 1;
}

static void newline(void) {
    ccol = 0;
    if (++crow >= rows) scroll();
}

void console_clear(void) {
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < cols; c++)
            grid[r][c] = 0;
    ccol = crow = 0;
    if (on_fb) repaint();
    cur_on = 0;
    cur_last = pit_ticks();
}

/* Ping the Bochs VBE DISPI enable register to force VirtualBox (under NEM) to
 * re-examine the framebuffer content. Also on plain QEMU this is a harmless
 * no-op that re-asserts the same state. */
static void vbe_ping(void) {
    outw(0x01CE, 4);               /* DI_ENABLE */
    outw(0x01CF, 0);               /* disable   */
    outw(0x01CE, 4);
    /* enable + LFB + NOCLEARMEM: without 0x80 the adapter wipes video memory
     * as it comes back on, and with it whatever was just drawn (QEMU does) */
    outw(0x01CF, 0xC1);
}

/* The text console's backdrop: the gradient, and a taskbar strip with the mark. */
static void paint_text_backdrop(void) {
    for (uint32_t y = 0; y < H; y++) {
        uint32_t c = lerp_color(0x0F0C29, 0x24243E, (int)y, (int)H);
        fb_rect(0, y, W, 1, c);
    }
    fb_rect(0, H - 52, W, 52, 0x0D0D0F);     /* taskbar */
    fb_rect(0, H - 52, W, 1,  0x2E2E38);
    fb_mark(22, (int)H - 44, 36, 0xFFFFFF, 230, 0x0D0D0F);
}

void console_init(void) {
    if (!fb_present()) { on_fb = 0; return; }
    on_fb = 1;
    W = fb_width();
    H = fb_height();
    quiet = !boot_is_safe();

    if (quiet) {
        int mx, my, ms;
        fb_fill(BOOT_BG);
        console_boot_mark((int)W, (int)H, &mx, &my, &ms);
        fb_mark(mx, my, ms, 0xFFFFFF, BOOT_GHOST, BOOT_BG);
    } else {
        paint_text_backdrop();
    }
    vbe_ping();                                /* flush to VirtualBox display */

    bottom = H - 60;
    cols = (W - 2 * MARGIN) / CELL; if (cols > MAXCOLS) cols = MAXCOLS;
    rows = (bottom - MARGIN) / CELL; if (rows > MAXROWS) rows = MAXROWS;
    ccol = crow = 0;
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < cols; c++)
            grid[r][c] = 0;
    cur_on = 0;
    cur_last = 0;
}

/* Something went wrong during a quiet boot, or there is no desktop to come:
 * the boot log, as text, from here on. */
void console_reveal(void) {
    if (!on_fb || !quiet) return;
    quiet = 0;
    paint_text_backdrop();
    repaint();
    vbe_ping();
}

/* Minimal ANSI CSI handling.
 *
 * The console is otherwise a dumb character grid, but a full-screen program
 * (`edit`) has to clear the screen and place the cursor. Doing that with escape
 * sequences means one piece of code works here, on a serial terminal and over
 * SSH -- where the far end is a real terminal and inventing a private protocol
 * it has never heard of would be the wrong answer. Sequences that are not
 * implemented are swallowed rather than printed as stray text. */
static int      esc_state;        /* 0 = normal, 1 = saw ESC, 2 = inside a CSI */
static char     esc_buf[16];
static uint32_t esc_len;

static void csi_apply(char final) {
    uint32_t a = 0;               /* only the first numeric parameter is used */
    for (uint32_t i = 0; i < esc_len; i++) {
        if (esc_buf[i] == ';') break;
        if (esc_buf[i] >= '0' && esc_buf[i] <= '9') a = a * 10 + (uint32_t)(esc_buf[i] - '0');
    }
    switch (final) {
    case 'D': {                                     /* move input caret left */
        uint32_t at = crow * cols + ccol;
        uint32_t n = a ? a : 1;
        cursor_erase();
        at = at > n ? at - n : 0;
        crow = at / cols; ccol = at % cols;
        break;
    }
    case 'J':                                        /* erase display        */
        if (a == 2) console_clear();
        break;
    case 'H':                                        /* cursor home          */
        ccol = crow = 0;
        break;
    case 'K':                                        /* erase to end of line */
        for (uint32_t c = ccol; c < cols; c++) { grid[crow][c] = 0; draw_cell(c, crow); }
        break;
    default:                                         /* colours, and the rest */
        break;
    }
}

static void putc_fb(char c) {
    if (esc_state == 1) { esc_state = (c == '[') ? 2 : 0; esc_len = 0; return; }
    if (esc_state == 2) {
        if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E) { csi_apply(c); esc_state = 0; }
        else if (esc_len < sizeof esc_buf) esc_buf[esc_len++] = c;
        return;
    }
    if (c == 0x1B) { esc_state = 1; return; }
    cursor_erase();                          /* never render over the cursor */
    if (c == '\r') { ccol = 0; return; }
    if (c == '\n') { newline(); return; }
    if (c == '\b') {
        if (ccol > 0)      ccol--;
        else if (crow > 0) { crow--; ccol = cols - 1; }
        grid[crow][ccol] = 0;
        draw_cell(ccol, crow);
        return;
    }
    if (c == '\t') {
        uint32_t n = 4 - (ccol & 3);
        while (n--) {
            grid[crow][ccol] = ' ';
            draw_cell(ccol, crow);
            if (++ccol >= cols) newline();
        }
        return;
    }
    if ((unsigned char)c < 32) return;       /* drop remaining control chars */
    grid[crow][ccol] = c;
    draw_cell(ccol, crow);
    if (++ccol >= cols) newline();
}

/* kprintf's sink: serial always; then a redirect sink if one is installed
 * (GUI terminal), otherwise the framebuffer text console when present. */
void kputc(char c) {
    if (c == '\n') serial_putc('\r');
    serial_putc(c);
    klog_putc(c);            /* every kernel message is also kept for `dmesg` */
    if (sink)       sink(c);
    else if (on_fb) putc_fb(c);
}
