#pragma once
#include <stdint.h>

/* ===========================================================================
 *  A Game Boy, in about fifteen hundred lines.
 *
 *  DMG only -- no colour, no Super Game Boy border, no link cable. The parts
 *  are the ones the hardware has: an SM83 core, a scanline PPU, four sound
 *  channels, the divider/timer pair, and whichever memory bank controller the
 *  cartridge header claims. Everything is cycle-counted in T-states because
 *  every other clock in the machine is derived from that one, and a PPU that
 *  runs on anything else drifts away from the CPU within a second.
 *
 *  The emulator knows nothing about BoltOS: it is handed a ROM image and a
 *  button bitmap and produces a 160x144 framebuffer and audio samples.
 *  kernel/app_gameboy.c is the only file that has heard of a window.
 * ===========================================================================*/

#define GB_W 160
#define GB_H 144

/* 4.194304 MHz. One frame is 154 scanlines of 456 T-states. */
#define GB_HZ       4194304
#define GB_LINE_T   456
#define GB_FRAME_T  (GB_LINE_T * 154)

/* Buttons, in the order the joypad register wants them. */
enum {
    GB_RIGHT = 0x01, GB_LEFT = 0x02, GB_UP = 0x04, GB_DOWN = 0x08,
    GB_A = 0x10, GB_B = 0x20, GB_SELECT = 0x40, GB_START = 0x80
};

/* Which mapper the header asked for. Anything else is refused by name rather
 * than run as if it were an MBC1, which produces a black screen and no clue. */
enum { MBC_NONE = 0, MBC1, MBC2, MBC3, MBC5 };

typedef struct {
    /* ---- cpu ---- */
    uint8_t  a, f, b, c, d, e, h, l;
    uint16_t sp, pc;
    int      ime, ime_pending, halted, stopped;
    int64_t  cyc;                    /* T-states since power on               */

    /* ---- memory ---- */
    uint8_t *rom;
    uint32_t rom_len;
    int      rom_banks;
    uint8_t *xram;                   /* cartridge RAM, battery-backed         */
    uint32_t xram_len;
    int      xram_banks;
    uint8_t  vram[0x2000];
    uint8_t  wram[0x2000];
    uint8_t  oam[0xA0];
    uint8_t  hram[0x7F];
    uint8_t  io[0x80];
    uint8_t  ie;

    /* ---- mapper ---- */
    int      mbc, has_battery, xram_on, xram_dirty;
    int      rom_bank, ram_bank, mbc1_mode;

    /* ---- ppu ---- */
    int      line_t;                 /* T-states into the current scanline    */
    uint8_t  fb[GB_W * GB_H];        /* 0..3, before any palette is applied   */
    int      frame_ready;
    int      win_line;               /* the window has its own line counter   */

    /* ---- timer ---- */
    int      div_t, tima_t;

    /* ---- input ---- */
    uint8_t  buttons;                /* GB_* bitmap, 1 = pressed              */

    /* ---- apu ---- */
    struct {
        int      on;
        int      seq, seq_t;         /* the 512 Hz frame sequencer            */
        struct {
            int on, dac, freq, timer, duty, dpos;
            int len, len_on;
            int vol, env_dir, env_per, env_t;
            int swp_per, swp_dir, swp_sh, swp_t, swp_on, swp_shadow;
        } ch1, ch2;
        struct { int on, dac, freq, timer, pos, len, len_on, level; uint8_t ram[16]; } ch3;
        struct { int on, dac, timer, div, sh, width, lfsr, len, len_on,
                     vol, env_dir, env_per, env_t; } ch4;
        int      samp_t;             /* T-states towards the next output frame*/
    } apu;

    char     title[17];
    const char *err;
} gb_t;

/* ---- lifecycle ----------------------------------------------------------- */
/* Takes ownership of neither pointer; `rom` must outlive the gb_t. Returns 0,
 * or -1 with gb->err naming what was wrong with the cartridge. */
int  gb_load(gb_t *gb, uint8_t *rom, uint32_t len);
void gb_reset(gb_t *gb);
void gb_free(gb_t *gb);

/* Run until the PPU finishes a frame (or `max_t` T-states have gone by, so a
 * ROM that never enables the LCD cannot wedge the caller). */
void gb_run_frame(gb_t *gb, int max_t);

/* ---- the parts, for the emulator's own use ------------------------------- */
uint8_t gb_rd(gb_t *gb, uint16_t addr);
void    gb_wr(gb_t *gb, uint16_t addr, uint8_t v);
int     gb_step(gb_t *gb);              /* one instruction; returns T-states  */
void    gb_ppu_step(gb_t *gb, int t);
void    gb_timer_step(gb_t *gb, int t);
void    gb_apu_step(gb_t *gb, int t);
void    gb_apu_write(gb_t *gb, uint16_t addr, uint8_t v);
uint8_t gb_apu_read(gb_t *gb, uint16_t addr);
void    gb_irq(gb_t *gb, int bit);      /* 0 vblank, 1 stat, 2 timer, 4 joypad*/

/* Audio: the emulator fills a ring at 48 kHz; the app drains it. */
#define GB_AUDIO_RING 8192
extern int16_t gb_audio[GB_AUDIO_RING * 2];
extern volatile int gb_audio_w, gb_audio_r;

/* IO register names, used in more than one file. */
#define R_P1   0x00
#define R_DIV  0x04
#define R_TIMA 0x05
#define R_TMA  0x06
#define R_TAC  0x07
#define R_IF   0x0F
#define R_LCDC 0x40
#define R_STAT 0x41
#define R_SCY  0x42
#define R_SCX  0x43
#define R_LY   0x44
#define R_LYC  0x45
#define R_DMA  0x46
#define R_BGP  0x47
#define R_OBP0 0x48
#define R_OBP1 0x49
#define R_WY   0x4A
#define R_WX   0x4B
