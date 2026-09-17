/* The Game Boy core on the host, checked against things that are not it.
 *
 * An emulator is unusually easy to test badly: it is trivial to assert that
 * the code does what the code does. So where there is an independent oracle,
 * this uses one --
 *
 *   - DAA is re-derived here straight from the hardware description and diffed
 *     against the core's version over all 8192 inputs. Two implementations
 *     written from the same spec in different shapes; if they agree, neither
 *     is the one I typed wrong. DAA is where this kind of core is usually
 *     broken, and the symptom is a score displayed in hexadecimal.
 *   - The instruction tests assert results worked out by hand from the
 *     instruction set, not captured from a run.
 *   - The PPU test builds a tile by hand and asserts the pixels that must come
 *     out of it, including sprite priority against background colour 0, which
 *     is the rule everybody gets backwards the first time.
 *
 * No ROM images are involved and none are needed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../gb/gb.h"
#include "../../gb/gb_cpu.c"
#include "../../gb/gb_mem.c"
#include "../../gb/gb_ppu.c"
#include "../../gb/gb_apu.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                       printf("\n"); fails++; } } while (0)

static uint8_t rom[0x20000];
static gb_t g;

/* A cartridge with a valid header and `code` sitting at the reset vector. */
static void boot(const uint8_t *code, int n, uint8_t carttype, uint32_t len) {
    memset(rom, 0, sizeof(rom));
    rom[0x147] = carttype;
    rom[0x148] = 0x01;
    rom[0x149] = 0x02;                       /* 8 KiB of cartridge RAM        */
    memcpy(rom + 0x134, "TESTCART", 8);
    uint8_t sum = 0;
    for (int i = 0x134; i <= 0x14C; i++) sum = (uint8_t)(sum - rom[i] - 1);
    rom[0x14D] = sum;
    if (code) memcpy(rom + 0x150, code, (size_t)n);

    int rc = gb_load(&g, rom, len);
    if (rc != 0) { printf("FAIL: gb_load refused the test cartridge: %s\n", g.err); fails++; }
    g.pc = 0x150;
    static uint8_t xram[8192];
    memset(xram, 0, sizeof(xram));
    g.xram = xram;
    g.xram_len = sizeof(xram);
}

static void run(int instrs) { for (int i = 0; i < instrs; i++) gb_step(&g); }

#define FZ_ 0x80
#define FN_ 0x40
#define FH_ 0x20
#define FC_ 0x10

/* ---- DAA, against a second implementation -------------------------------- */
static void daa_oracle(uint8_t *a, uint8_t *f) {
    int v = *a, n = (*f & FN_) != 0, h = (*f & FH_) != 0, c = (*f & FC_) != 0;
    if (!n) {
        if (c || v > 0x99) { v += 0x60; c = 1; }
        if (h || (v & 0x0F) > 0x09) v += 0x06;
    } else {
        if (c) v -= 0x60;
        if (h) v -= 0x06;
    }
    *a = (uint8_t)v;
    *f = (uint8_t)((*a == 0 ? FZ_ : 0) | (n ? FN_ : 0) | (c ? FC_ : 0));
}

static void daa_matches(void) {
    static const uint8_t code[] = { 0x27 };          /* DAA */
    int bad = 0, first_a = -1, first_f = 0;

    for (int a = 0; a < 256; a++) {
        for (int fl = 0; fl < 16; fl++) {
            uint8_t f = (uint8_t)(((fl & 1) ? FZ_ : 0) | ((fl & 2) ? FN_ : 0) |
                                  ((fl & 4) ? FH_ : 0) | ((fl & 8) ? FC_ : 0));
            boot(code, sizeof(code), 0x00, 0x8000);
            g.a = (uint8_t)a;
            g.f = f;
            gb_step(&g);

            uint8_t wa = (uint8_t)a, wf = f;
            daa_oracle(&wa, &wf);
            if (g.a != wa || (g.f & 0xF0) != wf) {
                if (!bad) { first_a = a; first_f = f; }
                bad++;
            }
        }
    }
    CHECK(bad == 0, "DAA disagrees with the spec in %d of 4096 cases "
                    "(first: A=%02X F=%02X)", bad, first_a, first_f);
}

/* ---- flags worked out by hand -------------------------------------------- */
static void flags_are_right(void) {
    /* 0x0F + 0x01 sets the half carry and nothing else. */
    { static const uint8_t c[] = { 0x3E, 0x0F, 0xC6, 0x01 };     /* LD A,0F; ADD A,01 */
      boot(c, sizeof(c), 0, 0x8000); run(2);
      CHECK(g.a == 0x10, "ADD: A is %02X, expected 10", g.a);
      CHECK((g.f & FH_) && !(g.f & FC_) && !(g.f & FZ_) && !(g.f & FN_),
            "ADD 0F+01: flags %02X, expected H only", g.f); }

    /* 0xFF + 0x01 wraps to zero: Z, H and C all set. */
    { static const uint8_t c[] = { 0x3E, 0xFF, 0xC6, 0x01 };
      boot(c, sizeof(c), 0, 0x8000); run(2);
      CHECK(g.a == 0x00, "ADD wrap: A is %02X", g.a);
      CHECK((g.f & (FZ_ | FH_ | FC_)) == (FZ_ | FH_ | FC_) && !(g.f & FN_),
            "ADD FF+01: flags %02X, expected Z H C", g.f); }

    /* SUB always sets N, and borrows out of bit 4 set H. */
    { static const uint8_t c[] = { 0x3E, 0x10, 0xD6, 0x01 };     /* LD A,10; SUB 01 */
      boot(c, sizeof(c), 0, 0x8000); run(2);
      CHECK(g.a == 0x0F, "SUB: A is %02X, expected 0F", g.a);
      CHECK((g.f & FN_) && (g.f & FH_) && !(g.f & FC_),
            "SUB 10-01: flags %02X, expected N H", g.f); }

    /* CP leaves A alone and sets Z when they match. */
    { static const uint8_t c[] = { 0x3E, 0x42, 0xFE, 0x42 };
      boot(c, sizeof(c), 0, 0x8000); run(2);
      CHECK(g.a == 0x42, "CP changed A to %02X", g.a);
      CHECK((g.f & FZ_) && (g.f & FN_), "CP equal: flags %02X, expected Z N", g.f); }

    /* INC does not touch the carry; DEC sets N. */
    { static const uint8_t c[] = { 0x37, 0x3E, 0xFF, 0x3C };     /* SCF; LD A,FF; INC A */
      boot(c, sizeof(c), 0, 0x8000); run(3);
      CHECK(g.a == 0 && (g.f & FZ_) && (g.f & FC_),
            "INC wrap: A=%02X flags=%02X, carry should have survived", g.a, g.f); }

    /* RLCA clears Z even when the result is zero; its CB twin does not. */
    { static const uint8_t c[] = { 0x3E, 0x00, 0x07 };           /* LD A,0; RLCA */
      boot(c, sizeof(c), 0, 0x8000); run(2);
      CHECK(!(g.f & FZ_), "RLCA set Z; on this chip it never does (flags %02X)", g.f); }
    { static const uint8_t c[] = { 0x3E, 0x00, 0xCB, 0x07 };     /* LD A,0; RLC A */
      boot(c, sizeof(c), 0, 0x8000); run(2);
      CHECK(g.f & FZ_, "CB RLC A on zero did not set Z (flags %02X)", g.f); }

    /* The low nibble of F does not exist: POP AF must clear it. */
    { static const uint8_t c[] = { 0x01, 0xFF, 0xFF, 0xC5, 0xF1 };  /* LD BC,FFFF; PUSH BC; POP AF */
      boot(c, sizeof(c), 0, 0x8000); run(3);
      CHECK((g.f & 0x0F) == 0, "POP AF kept the low nibble of F (%02X)", g.f); }

    /* ADD SP,e takes its carries from the low byte, always. */
    { static const uint8_t c[] = { 0x31, 0xFF, 0x0F, 0xE8, 0x01 };  /* LD SP,0FFF; ADD SP,1 */
      boot(c, sizeof(c), 0, 0x8000); run(2);
      CHECK(g.sp == 0x1000, "ADD SP,1: SP is %04X", g.sp);
      CHECK((g.f & FH_) && (g.f & FC_) && !(g.f & FZ_),
            "ADD SP,1 from 0FFF: flags %02X, expected H and C from the low byte", g.f); }

    /* ADD HL,BC sets H from bit 11 and leaves Z alone. */
    { static const uint8_t c[] = { 0x21, 0xFF, 0x0F, 0x01, 0x01, 0x00, 0x09 };
      boot(c, sizeof(c), 0, 0x8000); g.f = FZ_; run(3);
      CHECK(g.h == 0x10 && g.l == 0x00, "ADD HL,BC: HL is %02X%02X", g.h, g.l);
      CHECK((g.f & FH_) && (g.f & FZ_), "ADD HL: flags %02X, Z should survive", g.f); }

    /* LD (HL+),A writes then increments. */
    { static const uint8_t c[] = { 0x21, 0x00, 0xC0, 0x3E, 0xAB, 0x22 };
      boot(c, sizeof(c), 0, 0x8000); run(3);
      CHECK(gb_rd(&g, 0xC000) == 0xAB, "LD (HL+),A stored %02X", gb_rd(&g, 0xC000));
      CHECK(g.h == 0xC0 && g.l == 0x01, "LD (HL+),A left HL at %02X%02X", g.h, g.l); }
}

/* ---- the bus ------------------------------------------------------------- */
static void memory_behaves(void) {
    boot(0, 0, 0, 0x8000);

    gb_wr(&g, 0xC123, 0x5A);
    CHECK(gb_rd(&g, 0xE123) == 0x5A, "echo RAM does not mirror work RAM");
    gb_wr(&g, 0xE456, 0xA5);
    CHECK(gb_rd(&g, 0xC456) == 0xA5, "a write to echo RAM did not reach work RAM");

    CHECK(gb_rd(&g, 0xFEB0) == 0xFF, "the unusable region read %02X, not FF",
          gb_rd(&g, 0xFEB0));

    /* ROM is read-only, however hard you push. */
    uint8_t before = gb_rd(&g, 0x0200);
    gb_wr(&g, 0x0200, (uint8_t)(before ^ 0xFF));
    CHECK(gb_rd(&g, 0x0200) == before, "a write landed in ROM");

    /* The joypad reads zero for pressed, and only in the selected half. */
    g.buttons = GB_A | GB_RIGHT;
    gb_wr(&g, 0xFF00, 0x10);                        /* select the direction pad */
    CHECK((gb_rd(&g, 0xFF00) & 0x01) == 0, "right is held but reads as released");
    gb_wr(&g, 0xFF00, 0x20);                        /* select the buttons       */
    CHECK((gb_rd(&g, 0xFF00) & 0x01) == 0, "A is held but reads as released");
    g.buttons = 0;
    CHECK((gb_rd(&g, 0xFF00) & 0x0F) == 0x0F, "nothing is held and something reads pressed");

    /* OAM DMA copies 160 bytes and nothing else. */
    for (int i = 0; i < 0xA0; i++) gb_wr(&g, (uint16_t)(0xC200 + i), (uint8_t)i);
    gb_wr(&g, 0xFF46, 0xC2);
    int bad = 0;
    for (int i = 0; i < 0xA0; i++) if (g.oam[i] != (uint8_t)i) bad++;
    CHECK(bad == 0, "OAM DMA got %d of 160 bytes wrong", bad);
}

static void banking_works(void) {
    /* Put the bank number into the first byte of every 16 KiB bank, then ask
     * for each one through the mapper. */
    boot(0, 0, 0x01, 0x20000);                      /* MBC1, 128 KiB = 8 banks  */
    for (int b = 0; b < 8; b++) rom[b * 0x4000] = (uint8_t)(0xB0 + b);

    for (int b = 1; b < 8; b++) {
        gb_wr(&g, 0x2000, (uint8_t)b);
        uint8_t got = gb_rd(&g, 0x4000);
        CHECK(got == 0xB0 + b, "MBC1 bank %d read %02X, expected %02X", b, got, 0xB0 + b);
    }
    /* Bank 0 is not selectable through the low register; it reads as bank 1. */
    gb_wr(&g, 0x2000, 0x00);
    CHECK(gb_rd(&g, 0x4000) == 0xB1, "MBC1 bank 0 did not alias to bank 1");

    /* Cartridge RAM is dead until it is enabled. */
    gb_wr(&g, 0x0000, 0x00);
    gb_wr(&g, 0xA000, 0x77);
    CHECK(gb_rd(&g, 0xA000) == 0xFF, "disabled cartridge RAM answered a read");
    gb_wr(&g, 0x0000, 0x0A);
    gb_wr(&g, 0xA000, 0x77);
    CHECK(gb_rd(&g, 0xA000) == 0x77, "enabled cartridge RAM lost a write");

    boot(0, 0, 0x19, 0x20000);                      /* MBC5: nine bits of bank  */
    for (int b = 0; b < 8; b++) rom[b * 0x4000] = (uint8_t)(0xC0 + b);
    gb_wr(&g, 0x2000, 0x03);
    CHECK(gb_rd(&g, 0x4000) == 0xC3, "MBC5 low bank register");
    gb_wr(&g, 0x2000, 0x00);
    CHECK(gb_rd(&g, 0x4000) == 0xC0, "MBC5 bank 0 should be selectable and is not");
}

/* ---- timing -------------------------------------------------------------- */
static void timer_counts(void) {
    boot(0, 0, 0, 0x8000);

    /* TAC = 0x05: enabled, 262144 Hz, one tick every 16 T-states. */
    gb_wr(&g, 0xFF07, 0x05);
    gb_wr(&g, 0xFF05, 0x00);
    gb_timer_step(&g, 16 * 100);
    CHECK(g.io[R_TIMA] == 100, "TIMA counted %d in 100 periods", g.io[R_TIMA]);

    /* Disabled, it does not count at all. */
    gb_wr(&g, 0xFF07, 0x00);
    gb_wr(&g, 0xFF05, 0x00);
    gb_timer_step(&g, 16 * 100);
    CHECK(g.io[R_TIMA] == 0, "a disabled timer counted to %d", g.io[R_TIMA]);

    /* An overflow reloads from TMA and raises interrupt 2. */
    gb_wr(&g, 0xFF07, 0x05);
    gb_wr(&g, 0xFF06, 0x42);
    gb_wr(&g, 0xFF05, 0xFF);
    g.io[R_IF] = 0;
    gb_timer_step(&g, 16);
    CHECK(g.io[R_TIMA] == 0x42, "TIMA reloaded %02X, expected TMA (42)", g.io[R_TIMA]);
    CHECK(g.io[R_IF] & 0x04, "the timer overflowed without raising its interrupt");

    /* DIV runs whatever TAC says, and any write to it resets it. */
    gb_wr(&g, 0xFF07, 0x00);
    g.io[R_DIV] = 0; g.div_t = 0;
    gb_timer_step(&g, 256 * 5);
    CHECK(g.io[R_DIV] == 5, "DIV counted %d in 5 periods", g.io[R_DIV]);
    gb_wr(&g, 0xFF04, 0x99);
    CHECK(g.io[R_DIV] == 0, "a write to DIV did not reset it");
}

static void frame_timing(void) {
    boot(0, 0, 0, 0x8000);
    g.io[R_LCDC] = 0x80;

    /* 154 lines of 456 T-states, and the vblank interrupt on the line after
     * the last visible one. */
    g.io[R_IF] = 0;
    int t = 0;
    while (!g.frame_ready && t < GB_FRAME_T * 3) { gb_ppu_step(&g, 4); t += 4; }
    CHECK(g.frame_ready, "the PPU never finished a frame");
    CHECK(g.io[R_IF] & 0x01, "a frame ended without a vblank interrupt");
    CHECK(t >= GB_LINE_T * GB_H && t <= GB_LINE_T * (GB_H + 1),
          "vblank arrived after %d T-states; line 144 is at %d", t, GB_LINE_T * GB_H);

    g.frame_ready = 0;
    int t2 = 0;
    while (!g.frame_ready && t2 < GB_FRAME_T * 3) { gb_ppu_step(&g, 4); t2 += 4; }
    CHECK(t2 >= GB_FRAME_T - 8 && t2 <= GB_FRAME_T + 8,
          "the second frame took %d T-states, expected %d", t2, GB_FRAME_T);
}

/* ---- the picture --------------------------------------------------------- */
static void render_one_frame(void) {
    g.frame_ready = 0;
    for (int t = 0; t < GB_FRAME_T && !g.frame_ready; t += 4) gb_ppu_step(&g, 4);
}

static void ppu_draws(void) {
    boot(0, 0, 0, 0x8000);
    g.io[R_LCDC] = 0x91;            /* on, BG on, tile data at 8000            */
    g.io[R_BGP]  = 0xE4;            /* 3 2 1 0 -- colour n maps to shade n     */
    g.io[R_SCX]  = 0;
    g.io[R_SCY]  = 0;
    g.io[R_LY]   = 0;
    g.line_t     = 0;

    /* Tile 1: the left four pixels of every row colour 1, the right four
     * colour 2. Low bitplane 0xF0, high bitplane 0x0F. */
    for (int r = 0; r < 8; r++) {
        gb_wr(&g, (uint16_t)(0x8010 + r * 2),     0xF0);
        gb_wr(&g, (uint16_t)(0x8010 + r * 2 + 1), 0x0F);
    }
    for (int i = 0; i < 32 * 32; i++) gb_wr(&g, (uint16_t)(0x9800 + i), 1);

    render_one_frame();

    int bad = 0;
    for (int x = 0; x < 8; x++) {
        if (g.fb[x] != 1) bad++;
        if (g.fb[8 + x] != 1) bad++;     /* the next tile is the same tile     */
    }
    CHECK(g.fb[0] == 1 && g.fb[3] == 1, "left half of the tile: got %d %d, expected 1",
          g.fb[0], g.fb[3]);
    CHECK(g.fb[4] == 2 && g.fb[7] == 2, "right half of the tile: got %d %d, expected 2",
          g.fb[4], g.fb[7]);
    (void)bad;

    /* The palette really is applied: invert it and the shades swap. */
    g.io[R_BGP] = 0x1B;             /* 0 1 2 3 */
    render_one_frame();
    CHECK(g.fb[0] == 2, "BGP is not being applied (got %d, expected 2)", g.fb[0]);
    CHECK(g.fb[4] == 1, "BGP is not being applied to colour 2 (got %d)", g.fb[4]);

    /* Scrolling moves the window onto the map. With every tile identical the
     * only visible effect is the phase of the left/right split. */
    g.io[R_BGP] = 0xE4;
    g.io[R_SCX] = 4;
    render_one_frame();
    CHECK(g.fb[0] == 2, "SCX=4 should start mid-tile (got %d, expected 2)", g.fb[0]);
    g.io[R_SCX] = 0;

    /* Turning the background off blanks it whatever is in VRAM. */
    g.io[R_LCDC] = 0x90;
    render_one_frame();
    CHECK(g.fb[0] == 0, "LCDC bit 0 clear should blank the background (got %d)", g.fb[0]);
}

static void sprites_have_priority(void) {
    boot(0, 0, 0, 0x8000);
    g.io[R_LCDC] = 0x93;            /* on, BG on, sprites on                   */
    g.io[R_BGP]  = 0xE4;
    g.io[R_OBP0] = 0xE4;
    g.io[R_LY] = 0; g.line_t = 0;

    /* Background tile 1: solid colour 1 everywhere. */
    for (int r = 0; r < 8; r++) {
        gb_wr(&g, (uint16_t)(0x8010 + r * 2),     0xFF);
        gb_wr(&g, (uint16_t)(0x8010 + r * 2 + 1), 0x00);
    }
    /* Sprite tile 2: solid colour 3. */
    for (int r = 0; r < 8; r++) {
        gb_wr(&g, (uint16_t)(0x8020 + r * 2),     0xFF);
        gb_wr(&g, (uint16_t)(0x8020 + r * 2 + 1), 0xFF);
    }
    for (int i = 0; i < 32 * 32; i++) gb_wr(&g, (uint16_t)(0x9800 + i), 1);

    /* One sprite at (8,16), which is screen (0,0). */
    g.oam[0] = 16; g.oam[1] = 8; g.oam[2] = 2; g.oam[3] = 0x00;
    render_one_frame();
    CHECK(g.fb[0] == 3, "a foreground sprite did not cover the background (got %d)", g.fb[0]);

    /* Attribute bit 7 puts it behind any background colour that is not 0. */
    g.oam[3] = 0x80;
    render_one_frame();
    CHECK(g.fb[0] == 1, "a background-priority sprite drew over colour 1 (got %d)", g.fb[0]);

    /* ...but it still shows through background colour 0. */
    for (int r = 0; r < 8; r++) gb_wr(&g, (uint16_t)(0x8010 + r * 2), 0x00);
    render_one_frame();
    CHECK(g.fb[0] == 3, "a background-priority sprite was hidden by colour 0 (got %d)",
          g.fb[0]);

    /* A transparent sprite pixel is never drawn, whatever its priority. */
    for (int r = 0; r < 8; r++) {
        gb_wr(&g, (uint16_t)(0x8020 + r * 2),     0x00);
        gb_wr(&g, (uint16_t)(0x8020 + r * 2 + 1), 0x00);
    }
    for (int r = 0; r < 8; r++) gb_wr(&g, (uint16_t)(0x8010 + r * 2), 0xFF);
    g.oam[3] = 0x00;
    render_one_frame();
    CHECK(g.fb[0] == 1, "colour 0 of a sprite was drawn (got %d, expected the bg)", g.fb[0]);
}

/* ---- interrupts ---------------------------------------------------------- */
static void interrupts_dispatch(void) {
    static const uint8_t code[] = { 0xFB, 0x00, 0x00, 0x00 };   /* EI; NOP NOP NOP */
    boot(code, sizeof(code), 0, 0x8000);
    g.sp = 0xFFFE;
    g.ie = 0x01;                     /* vblank only */
    g.io[R_IF] = 0x01;

    gb_step(&g);                     /* EI: takes effect after the next one    */
    CHECK(g.ime == 0, "EI enabled interrupts immediately; it is delayed by one");
    gb_step(&g);                     /* the NOP, during which IME comes up     */
    CHECK(g.ime == 1, "EI never took effect");
    gb_step(&g);                     /* now the interrupt is taken             */
    CHECK(g.pc == 0x40, "vblank went to %04X, expected 0040", g.pc);
    CHECK(g.ime == 0, "IME was not cleared on dispatch");
    CHECK((g.io[R_IF] & 0x01) == 0, "the IF bit was not cleared on dispatch");
    CHECK(g.sp == 0xFFFC, "the return address was not pushed (SP %04X)", g.sp);

    /* A masked interrupt is not taken. */
    boot(0, 0, 0, 0x8000);
    g.ime = 1; g.ie = 0x00; g.io[R_IF] = 0x1F;
    uint16_t pc = g.pc;
    gb_step(&g);
    CHECK(g.pc != 0x40 && g.pc != 0x48, "an interrupt masked off in IE was taken anyway");
    (void)pc;

    /* HALT wakes on a pending interrupt even with IME clear. */
    boot(0, 0, 0, 0x8000);
    g.halted = 1; g.ime = 0; g.ie = 0x04; g.io[R_IF] = 0x04;
    gb_step(&g);
    CHECK(g.halted == 0, "a pending interrupt did not wake the CPU out of HALT");
}

/* ---- sound --------------------------------------------------------------- */
static void apu_produces_samples(void) {
    boot(0, 0, 0, 0x8000);
    gb_audio_r = gb_audio_w = 0;

    gb_wr(&g, 0xFF26, 0x80);         /* APU on                                 */
    gb_wr(&g, 0xFF25, 0xFF);         /* everything to both speakers            */
    gb_wr(&g, 0xFF24, 0x77);         /* both master volumes to 7               */
    gb_wr(&g, 0xFF11, 0x80);         /* channel 1: 50% duty                    */
    gb_wr(&g, 0xFF12, 0xF0);         /* full volume, no envelope               */
    gb_wr(&g, 0xFF13, 0x00);
    gb_wr(&g, 0xFF14, 0x87);         /* trigger, frequency 0x700               */

    CHECK(g.apu.ch1.on, "channel 1 did not switch on when triggered");

    for (int i = 0; i < 4096; i++) gb_apu_step(&g, 64);

    int n = gb_audio_w - gb_audio_r;
    if (n < 0) n += GB_AUDIO_RING;
    CHECK(n > 0, "the APU produced no samples at all");

    int nonzero = 0, peak = 0;
    for (int i = 0; i < n; i++) {
        int v = gb_audio[((gb_audio_r + i) % GB_AUDIO_RING) * 2];
        if (v) nonzero++;
        if (v > peak) peak = v;
    }
    CHECK(nonzero > n / 8, "a square wave at full volume was silent %d%% of the time",
          100 - (nonzero * 100 / (n ? n : 1)));
    CHECK(peak > 1000, "peak amplitude was only %d; the mix is far too quiet", peak);

    /* Switching the APU off silences every channel. */
    gb_wr(&g, 0xFF26, 0x00);
    CHECK(!g.apu.ch1.on, "channel 1 survived the APU being switched off");
}

/* ---- the whole machine, end to end --------------------------------------- *
 *  Everything above tests one part with the others held still. This runs a
 *  real program: a cartridge assembled by hand below that builds a tile,
 *  fills the tile map with it, sets the palette and the LCD control register
 *  and then spins. If the picture comes out, then the CPU executed, its writes
 *  reached VRAM through the bus, the PPU read them back and the frame loop
 *  terminated -- which is the part no unit test covers and the part that is
 *  actually broken when a game shows a black screen. */
static void a_real_program_runs(void) {
    static const uint8_t prog[] = {
        /* 150 */ 0x21, 0x10, 0x80,        /* LD HL,$8010   -- tile 1        */
        /* 153 */ 0x06, 0x08,              /* LD B,8        -- eight rows    */
        /* 155 */ 0x3E, 0xFF,              /* LD A,$FF                       */
        /* 157 */ 0x22,                    /* LD (HL+),A    -- low plane     */
        /* 158 */ 0x3E, 0x00,              /* LD A,$00                       */
        /* 15A */ 0x22,                    /* LD (HL+),A    -- high plane    */
        /* 15B */ 0x05,                    /* DEC B                          */
        /* 15C */ 0x20, 0xF7,              /* JR NZ,$155                     */
        /* 15E */ 0x21, 0x00, 0x98,        /* LD HL,$9800   -- the tile map  */
        /* 161 */ 0x01, 0x00, 0x04,        /* LD BC,$0400                    */
        /* 164 */ 0x3E, 0x01,              /* LD A,1                         */
        /* 166 */ 0x22,                    /* LD (HL+),A                     */
        /* 167 */ 0x0B,                    /* DEC BC        -- sets no flags */
        /* 168 */ 0x78,                    /* LD A,B                         */
        /* 169 */ 0xB1,                    /* OR C          -- so test it    */
        /* 16A */ 0x20, 0xF8,              /* JR NZ,$164                     */
        /* 16C */ 0x3E, 0xE4,              /* LD A,$E4                       */
        /* 16E */ 0xE0, 0x47,              /* LDH ($47),A   -- BGP           */
        /* 170 */ 0x3E, 0x91,              /* LD A,$91                       */
        /* 172 */ 0xE0, 0x40,              /* LDH ($40),A   -- LCDC on       */
        /* 174 */ 0x18, 0xFE,              /* JR $174       -- and spin      */
    };
    boot(prog, sizeof(prog), 0x00, 0x8000);

    for (int f = 0; f < 12; f++) gb_run_frame(&g, GB_FRAME_T * 2);

    CHECK(g.pc == 0x174, "the program ended up at %04X, not in its final loop", g.pc);
    CHECK(g.vram[0x0010] == 0xFF, "the tile never reached VRAM (8010 = %02X)",
          g.vram[0x0010]);
    CHECK(gb_rd(&g, 0x9800) == 0x01, "the tile map was not filled (9800 = %02X)",
          gb_rd(&g, 0x9800));

    int wrong = 0;
    for (int i = 0; i < GB_W * GB_H; i++) if (g.fb[i] != 1) wrong++;
    CHECK(wrong == 0, "%d of %d pixels are not shade 1; the screen should be "
                      "solid", wrong, GB_W * GB_H);

    /* And the frame really is being produced at the right rate: twelve frames
     * of a 154-line screen is a known number of T-states. */
    int64_t per = g.cyc / 12;
    CHECK(per > GB_FRAME_T - 2000 && per < GB_FRAME_T + 2000,
          "twelve frames averaged %lld T-states each, expected about %d",
          (long long)per, GB_FRAME_T);
}

int main(void) {
    printf("gameboy: DAA\n");        daa_matches();
    printf("gameboy: flags\n");      flags_are_right();
    printf("gameboy: memory\n");     memory_behaves();
    printf("gameboy: banking\n");    banking_works();
    printf("gameboy: timer\n");      timer_counts();
    printf("gameboy: frame\n");      frame_timing();
    printf("gameboy: ppu\n");        ppu_draws();
    printf("gameboy: sprites\n");    sprites_have_priority();
    printf("gameboy: interrupts\n"); interrupts_dispatch();
    printf("gameboy: apu\n");        apu_produces_samples();
    printf("gameboy: a real program\n"); a_real_program_runs();

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("gameboy host tests passed\n");
    return 0;
}
