/* ===========================================================================
 *  BoltOS  -  kernel/app_gameboy.c
 *  The Game Boy emulator's window: pick a cartridge, then run it.
 *
 *  Everything under gb/ is the machine and has never heard of BoltOS. This
 *  file finds ROMs on the filesystem, hands one over, turns the 160x144 frame
 *  of two-bit pixels into something green, feeds the audio ring to the codec,
 *  and writes cartridge RAM back to a .sav next to the ROM so a save game
 *  survives being closed.
 *
 *  Put ROMs in /home/user/roms (or anywhere under the home directory) and
 *  reopen the app. Nothing is shipped with it -- a Game Boy without a
 *  cartridge is exactly as useful as the real thing was.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "pit.h"
#include "keyboard.h"
#include "kheap.h"
#include "fs.h"
#include "audio.h"
#include "stdio.h"
#include "string.h"
#include "../gb/gb.h"

/* Set-1 scancodes, extended keys with 0x200 as kbd_key_down expects. */
#define SC_ESC   0x01
#define SC_ENTER 0x1C
#define SC_BKSP  0x0E
#define SC_Z     0x2C
#define SC_X     0x2D
#define SC_RSH   0x36
#define SC_W     0x11
#define SC_A     0x1E
#define SC_S     0x1F
#define SC_D     0x20
#define SC_UP    0x248
#define SC_DOWN  0x250
#define SC_LEFT  0x24B
#define SC_RIGHT 0x24D

#define MAX_ROMS 32
#define SND_CHUNK 1024

static window_t *g_win;
static gb_t     *g_gb;
static int       running;          /* a cartridge is in the slot              */
static int       failed;
static char      err[64];

static char      roms[MAX_ROMS][64];
static char      rompath[MAX_ROMS][160];
static int       nroms, sel;

static uint32_t *screen;           /* GB_W*GB_H ARGB, what g_blit is handed   */
static int16_t  *sndbuf;
static uint64_t  last_frame;
static int       frames, fps, fps_acc;
static uint64_t  fps_t;
static char      loaded_path[160];
static uint64_t  last_save;

/* Four greens. Not a filter over a grey image -- these are the shades a DMG
 * panel actually shows, and half the charm of the thing is that it is not
 * black and white. */
static const uint32_t dmg[4] = { 0xE0F8D0, 0x88C070, 0x346856, 0x081820 };

static int gameboy_active(void) {
    return g_win && g_win->open && !g_win->minimized && gui_window_focused(g_win);
}

/* ---- finding cartridges -------------------------------------------------- */
static int is_rom_name(const char *n) {
    int len = 0;
    while (n[len]) len++;
    if (len < 4) return 0;
    const char *e = n + len - 3;
    if (e[0] == '.' && (e[1] == 'g' || e[1] == 'G') && (e[2] == 'b' || e[2] == 'B')) return 1;
    if (len >= 5) {
        e = n + len - 4;
        if (e[0] == '.' && (e[1] == 'g' || e[1] == 'G') && (e[2] == 'b' || e[2] == 'B')
            && (e[3] == 'c' || e[3] == 'C')) return 1;
    }
    return 0;
}

static void scan_dir(const char *path, int depth) {
    if (nroms >= MAX_ROMS) return;
    fs_node *d = fs_lookup(path);
    if (!d || !d->is_dir) return;

    for (fs_node *c = d->child; c && nroms < MAX_ROMS; c = c->next) {
        char full[160];
        snprintf(full, sizeof(full), "%s%s%s", path,
                 path[1] ? "/" : "", c->name);
        if (c->is_dir) {
            if (depth > 0) scan_dir(full, depth - 1);
            continue;
        }
        if (!is_rom_name(c->name)) continue;
        strlcpy(roms[nroms], c->name, sizeof(roms[0]));
        strlcpy(rompath[nroms], full, sizeof(rompath[0]));
        nroms++;
    }
}

static void find_roms(void) {
    nroms = 0;
    scan_dir("/home/user/roms", 1);
    scan_dir("/home/user", 0);
    scan_dir("/roms", 1);
}

/* ---- battery saves ------------------------------------------------------- */
static void sav_path(const char *rom, char *out, int cap) {
    snprintf(out, (uint32_t)cap, "%s.sav", rom);
}

static void load_sav(void) {
    if (!g_gb->has_battery || !g_gb->xram || !g_gb->xram_len) return;
    char p[176];
    sav_path(loaded_path, p, sizeof(p));
    fs_node *n = fs_lookup(p);
    if (!n || n->is_dir || !n->data) return;
    uint32_t len = n->size < g_gb->xram_len ? n->size : g_gb->xram_len;
    for (uint32_t i = 0; i < len; i++) g_gb->xram[i] = n->data[i];
}

static void save_sav(void) {
    if (!g_gb || !g_gb->has_battery || !g_gb->xram || !g_gb->xram_len) return;
    if (!g_gb->xram_dirty) return;
    char p[176];
    sav_path(loaded_path, p, sizeof(p));
    fs_node *n = fs_lookup(p);
    if (!n) n = fs_create(p, 0);
    if (!n) return;
    if (fs_write(n, g_gb->xram, g_gb->xram_len) == 0) g_gb->xram_dirty = 0;
}

/* ---- loading ------------------------------------------------------------- */
static void unload(void) {
    if (g_gb) {
        save_sav();
        kfree(g_gb->xram);
        gb_free(g_gb);
        kfree(g_gb);
        g_gb = 0;
    }
    running = 0;
    gb_audio_r = gb_audio_w = 0;
}

static void load_rom(int idx) {
    unload();
    failed = 0;
    err[0] = 0;
    if (idx < 0 || idx >= nroms) return;

    fs_node *n = fs_lookup(rompath[idx]);
    if (!n || n->is_dir || !n->data || !n->size) {
        failed = 1;
        strlcpy(err, "the file went away", sizeof(err));
        return;
    }

    g_gb = kmalloc(sizeof(gb_t));
    if (!g_gb) { failed = 1; strlcpy(err, "out of memory", sizeof(err)); return; }

    /* The ROM is used where it already sits in the VFS rather than copied: a
     * megabyte is a lot to ask the kernel heap for twice, and the node is not
     * going anywhere while its window is open. */
    if (gb_load(g_gb, n->data, n->size) != 0) {
        failed = 1;
        strlcpy(err, g_gb->err ? g_gb->err : "not a cartridge", sizeof(err));
        kfree(g_gb);
        g_gb = 0;
        return;
    }
    /* gb_load leaves err set for a bad header checksum and still returns 0 --
     * plenty of homebrew has one and runs fine. Show it, run it anyway. */
    if (g_gb->err) strlcpy(err, g_gb->err, sizeof(err));

    if (g_gb->xram_len) {
        g_gb->xram = kmalloc(g_gb->xram_len);
        if (!g_gb->xram) { failed = 1; strlcpy(err, "no room for cartridge RAM", sizeof(err));
                           kfree(g_gb); g_gb = 0; return; }
        for (uint32_t i = 0; i < g_gb->xram_len; i++) g_gb->xram[i] = 0;
    }

    strlcpy(loaded_path, rompath[idx], sizeof(loaded_path));
    load_sav();
    running = 1;
    last_frame = pit_ticks();
    last_save = pit_ticks();
    frames = 0; fps = 0; fps_acc = 0; fps_t = pit_ticks();
}

/* ---- input --------------------------------------------------------------- */
static uint8_t read_buttons(void) {
    uint8_t b = 0;
    if (kbd_key_down(SC_RIGHT) || kbd_key_down(SC_D)) b |= GB_RIGHT;
    if (kbd_key_down(SC_LEFT)  || kbd_key_down(SC_A)) b |= GB_LEFT;
    if (kbd_key_down(SC_UP)    || kbd_key_down(SC_W)) b |= GB_UP;
    if (kbd_key_down(SC_DOWN)  || kbd_key_down(SC_S)) b |= GB_DOWN;
    if (kbd_key_down(SC_X))     b |= GB_A;
    if (kbd_key_down(SC_Z))     b |= GB_B;
    if (kbd_key_down(SC_ENTER)) b |= GB_START;
    if (kbd_key_down(SC_RSH) || kbd_key_down(SC_BKSP)) b |= GB_SELECT;
    return b;
}

/* ---- sound --------------------------------------------------------------- */
static void feed_audio(void) {
    if (!sndbuf || !audio_pcm_ok() || audio_busy()) return;

    int avail = gb_audio_w - gb_audio_r;
    if (avail < 0) avail += GB_AUDIO_RING;
    if (avail < SND_CHUNK) return;          /* wait for a full chunk           */

    for (int i = 0; i < SND_CHUNK; i++) {
        int r = gb_audio_r;
        sndbuf[i * 2]     = gb_audio[r * 2];
        sndbuf[i * 2 + 1] = gb_audio[r * 2 + 1];
        gb_audio_r = (r + 1) % GB_AUDIO_RING;
    }
    audio_play_async(sndbuf, SND_CHUNK);
}

/* -------------------------------------------------------------------------- */
void gameboy_pump(void) {
    if (!g_win) return;

    if (!gameboy_active()) {
        if (running) save_sav();
        return;
    }

    if (!running) {
        if (!nroms && !failed) find_roms();
        /* list mode: arrows pick, Enter loads */
        static int prev_up, prev_dn, prev_ret;
        int up = kbd_key_down(SC_UP), dn = kbd_key_down(SC_DOWN), ret = kbd_key_down(SC_ENTER);
        if (up && !prev_up && sel > 0) { sel--; gui_request_redraw(); }
        if (dn && !prev_dn && sel < nroms - 1) { sel++; gui_request_redraw(); }
        if (ret && !prev_ret && nroms) { load_rom(sel); gui_request_redraw(); }
        prev_up = up; prev_dn = dn; prev_ret = ret;
        return;
    }

    static int prev_esc;
    int esc = kbd_key_down(SC_ESC);
    if (esc && !prev_esc) { unload(); find_roms(); gui_request_redraw(); prev_esc = esc; return; }
    prev_esc = esc;

    uint64_t now = pit_ticks();
    /* 60 frames a second is the target and the cap. If the machine cannot keep
     * up the game runs slow rather than dropping frames, which for something
     * whose whole timebase is the frame is the less wrong of the two. */
    if (now - last_frame < 16) return;
    last_frame = now;

    g_gb->buttons = read_buttons();
    gb_run_frame(g_gb, GB_FRAME_T * 2);
    feed_audio();

    /* Convert once, here, rather than in the draw callback: the compositor can
     * repaint for reasons that have nothing to do with a new frame. */
    for (int i = 0; i < GB_W * GB_H; i++)
        screen[i] = 0xFF000000u | dmg[g_gb->fb[i] & 3];

    frames++;
    if (now - fps_t >= 1000) { fps = frames; frames = 0; fps_t = now; }

    /* Commit the cartridge RAM every few seconds. A save game lost to a hard
     * power-off is a bad afternoon, and this costs nothing when nothing has
     * been written. */
    if (now - last_save > 4000) { save_sav(); last_save = now; }

    gui_request_redraw();
}

/* -------------------------------------------------------------------------- */
static void draw_list(int cx, int cy, int cw, int ch) {
    g_fill(cx, cy, cw, ch, 0x14181A);
    g_text(cx + 18, cy + 16, "GAME BOY", 0x88C070, 3);
    g_text(cx + 18, cy + 50, "Put .gb files in /home/user/roms", COL_TEXT_DIM, 1);

    if (failed && err[0]) g_text(cx + 18, cy + 68, err, COL_BAD, 1);

    if (!nroms) {
        g_text(cx + 18, cy + 96, "No cartridges found.", COL_TEXT, 2);
        g_text(cx + 18, cy + 124,
               "Download one onto the machine, then reopen this window.",
               COL_TEXT_DIM, 1);
        return;
    }

    for (int i = 0; i < nroms; i++) {
        int y = cy + 92 + i * 20;
        if (y > cy + ch - 20) break;
        if (i == sel) {
            g_fill(cx + 12, y - 3, cw - 24, 19, 0x24402C);
            g_text(cx + 18, y, ">", 0x88C070, 1);
        }
        g_text(cx + 34, y, roms[i], i == sel ? 0xE0F8D0 : COL_TEXT_DIM, 1);
    }
    g_text(cx + 18, cy + ch - 22, "Up/Down to choose, Enter to run", COL_TEXT_DIM, 1);
}

static void draw_running(int cx, int cy, int cw, int ch) {
    /* Letterbox to whole-pixel multiples where we can: the Game Boy's pixels
     * are square and large, and a non-integer scale turns a two-pixel-wide
     * sprite outline into a shimmering mess. */
    int sx = cw / GB_W, sy = (ch - 20) / GB_H;
    int s = sx < sy ? sx : sy;
    if (s < 1) s = 1;
    int w = GB_W * s, h = GB_H * s;
    int x = cx + (cw - w) / 2, y = cy + (ch - 20 - h) / 2;

    g_fill(cx, cy, cw, ch, 0x0C1410);
    g_blit(x, y, w, h, screen, GB_W, GB_H);
    g_rect(x - 1, y - 1, w + 2, h + 2, 0x2A4030);

    char buf[96];
    snprintf(buf, sizeof(buf), "%s   %d fps   Esc: eject", g_gb->title, fps);
    g_text(cx + 10, cy + ch - 16, buf, 0x88C070, 1);

    const char *keys = "arrows move   Z B   X A   Enter start   RShift select";
    g_text(cx + cw - 10 - g_text_width(keys, 1), cy + ch - 16, keys, COL_TEXT_DIM, 1);
}

static void gameboy_draw(window_t *win, int cx, int cy, int cw, int ch) {
    (void)win;
    if (running && g_gb && screen) draw_running(cx, cy, cw, ch);
    else                          draw_list(cx, cy, cw, ch);
}

static void gameboy_click(window_t *win, int lx, int ly) {
    (void)win; (void)lx;
    if (running) return;
    int i = (ly - 92 + 3) / 20;
    if (i >= 0 && i < nroms) {
        sel = i;
        load_rom(i);
        gui_request_redraw();
    }
}

void gameboy_app_init(void) {
    window_t *win = gui_add_window("Game Boy", GB_W * 3, GB_H * 3 + 52, 0x88C070, ICON_GAMEBOY);
    if (!win) return;

    screen = kmalloc(GB_W * GB_H * 4);
    sndbuf = kmalloc(SND_CHUNK * 2 * sizeof(int16_t));
    if (!screen || !sndbuf) {
        /* Half a megabyte between them; if this fails the machine has bigger
         * problems, but the window should still open and say so. */
        kfree(screen); screen = 0;
        kfree(sndbuf); sndbuf = 0;
        failed = 1;
        strlcpy(err, "out of memory for the screen buffer", sizeof(err));
    }

    win->draw  = gameboy_draw;
    win->click = gameboy_click;
    win->min_w = GB_W * 2;
    win->min_h = GB_H * 2 + 40;
    win->x = 260; win->y = 70;
    g_win = win;

    kbd_raw_enable(1);
}
