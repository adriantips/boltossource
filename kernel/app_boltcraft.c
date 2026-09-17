/* ===========================================================================
 *  BoltOS  -  kernel/app_boltcraft.c
 *  The desktop's way into boltcraft, which is not a kernel app at all.
 *
 *  Everything else with an icon on that desktop is compiled into the kernel and
 *  draws into a window. boltcraft is a ring-3 ELF at /bin/boltcraft: it asks for
 *  the panel with SYS_FBGRAB, paints all 786,432 pixels itself, and gives the
 *  screen back when you press escape. There is no window for it to live in and
 *  there should not be -- the whole point of the thing is that the kernel has
 *  never heard of it.
 *
 *  So this is a launcher and nothing else: a card saying what the program is
 *  and which keys it wants, and a button that calls proc_exec on a path. It
 *  would work just as well for any other program in /bin, which is the honest
 *  description of how much it knows about voxels.
 *
 *  Why bother, when `run boltcraft` at a prompt does the same thing? Because a
 *  game that can only be started by someone who already knows it is installed,
 *  knows what it is called, and knows there is a shell, is a game nobody finds.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "fs.h"
#include "proc.h"
#include "notify.h"
#include "string.h"
#include "stdio.h"

#define CRAFT_PATH "/bin/boltcraft"

static window_t *win;
static char      status[96];
static int       launching;      /* the click is handled on the next tick    */
static int       last_cw, last_ch;   /* client size the last draw worked in  */

/* The button. One function so the draw and the hit test cannot disagree, and
 * the hit test passes the origin as 0,0 because a click arrives client-local. */
static void play_rect(int cx, int cy, int cw, int ch, int *x, int *y, int *w, int *h) {
    *w = 190; *h = 42;
    *x = cx + cw / 2 - *w / 2;
    *y = cy + ch - 68;
}

static void craft_draw(window_t *w_, int cx, int cy, int cw, int ch) {
    (void)w_;
    last_cw = cw; last_ch = ch;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    int pad = 16, x = cx + pad, w = cw - 2 * pad, y = cy + pad;

    g_round(x, y, w, 72, 12, COL_PANEL_2, 255);
    gui_icon(ICON_CRAFT, x + 18, y + 18, 2, COL_ACCENT);
    g_text(x + 66, y + 14, "boltcraft", COL_TEXT, 3);
    g_text(x + 66, y + 46, "a voxel sandbox  -  dig, build, get lost in a cave",
           COL_TEXT_DIM, 1);
    y += 86;

    static const char *const lines[] = {
        "256 x 256 x 64 blocks, no chunks, about 90 KiB.",
        "It runs in ring 3 and talks to the kernel only",
        "through the syscalls in SDK.md.",
        "",
        "W A S D  step        arrows  look",
        "E  break     Q  place        F  fly",
        "1-9 0  pick a block   K / L  save, load",
        "esc  quit, and give the screen back",
    };
    for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++)
        g_text(x + 4, y + (int)i * 20, lines[i],
               i < 3 ? COL_TEXT_DIM : COL_TEXT, 1);

    int bx, by, bw, bh;
    play_rect(cx, cy, cw, ch, &bx, &by, &bw, &bh);
    int ready = fs_lookup(CRAFT_PATH) != 0;
    g_round(bx, by, bw, bh, 10, ready ? COL_ACCENT : COL_PANEL_2, 255);
    const char *label = launching ? "Starting..." : (ready ? "Play" : "Not installed");
    g_text(bx + bw / 2 - g_text_width(label, 2) / 2, by + bh / 2 - g_font_height(2) / 2,
           label, ready ? 0x101014 : COL_TEXT_DIM, 2);

    if (status[0])
        g_text(cx + cw / 2 - g_text_width(status, 1) / 2, by + bh + 10, status,
               COL_TEXT_DIM, 1);
    else if (ready)
        g_text(cx + cw / 2 - g_text_width("The desktop comes back when you quit.", 1) / 2,
               by + bh + 10, "The desktop comes back when you quit.", COL_TEXT_DIM, 1);
}

static void craft_click(window_t *w_, int lx, int ly) {
    (void)w_;
    if (!last_cw) return;                        /* not drawn yet            */
    int bx, by, bw, bh;
    play_rect(0, 0, last_cw, last_ch, &bx, &by, &bw, &bh);
    if (lx < bx || lx >= bx + bw || ly < by || ly >= by + bh) return;
    if (!fs_lookup(CRAFT_PATH)) {
        snprintf(status, sizeof status, "%s is missing", CRAFT_PATH);
        gui_request_redraw();
        return;
    }
    /* Not started here. proc_exec builds an address space and schedules a ring-3
     * thread that will take the panel, and doing that from inside a click
     * handler means the last thing on screen is a half-drawn button. Say
     * "Starting...", let the frame out, and launch on the tick. */
    launching = 1;
    status[0] = 0;
    gui_request_redraw();
}

static void craft_tick(window_t *w_) {
    (void)w_;
    if (!launching) return;
    launching = 0;
    if (proc_exec(CRAFT_PATH) < 0) {
        snprintf(status, sizeof status, "%s would not start", CRAFT_PATH);
        gui_notify(NOTE_ERROR, "boltcraft", status);
    } else {
        status[0] = 0;
    }
    gui_request_redraw();
}

void boltcraft_app_init(void) {
    win = gui_add_window("boltcraft", 470, 400, 0x6FA84F, ICON_CRAFT);
    if (!win) return;
    win->draw  = craft_draw;
    win->click = craft_click;
    win->tick  = craft_tick;
    win->min_w = 380;
    win->min_h = 330;
    win->x = 150; win->y = 90;
}
