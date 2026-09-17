/* ===========================================================================
 *  Game Boy: the bus, the cartridge, the timer and the joypad.
 *
 *  Everything the CPU reads or writes comes through gb_rd/gb_wr, including the
 *  registers -- there is no fast path that bypasses them, because on this
 *  machine almost every address has a side effect and the ones that do not are
 *  not worth a special case.
 * ===========================================================================*/
#include <stdint.h>
#include "gb.h"
#include "string.h"

/* ---- cartridge ----------------------------------------------------------- */
static const struct { uint8_t id; int mbc; int battery; } carts[] = {
    { 0x00, MBC_NONE, 0 }, { 0x08, MBC_NONE, 0 }, { 0x09, MBC_NONE, 1 },
    { 0x01, MBC1, 0 }, { 0x02, MBC1, 0 }, { 0x03, MBC1, 1 },
    { 0x05, MBC2, 0 }, { 0x06, MBC2, 1 },
    { 0x0F, MBC3, 1 }, { 0x10, MBC3, 1 }, { 0x11, MBC3, 0 },
    { 0x12, MBC3, 0 }, { 0x13, MBC3, 1 },
    { 0x19, MBC5, 0 }, { 0x1A, MBC5, 0 }, { 0x1B, MBC5, 1 },
    { 0x1C, MBC5, 0 }, { 0x1D, MBC5, 0 }, { 0x1E, MBC5, 1 },
};

/* Header byte 0x149. MBC2 is the odd one: its RAM is on the mapper, 512
 * nibbles, and the header says none. */
static const int ram_sizes[6] = { 0, 2048, 8192, 32768, 131072, 65536 };

int gb_load(gb_t *gb, uint8_t *rom, uint32_t len) {
    memset(gb, 0, sizeof(*gb));
    if (len < 0x150) { gb->err = "too small to be a cartridge"; return -1; }

    gb->rom = rom;
    gb->rom_len = len;

    for (int i = 0; i < 16; i++) {
        uint8_t ch = rom[0x134 + i];
        gb->title[i] = (ch >= 32 && ch < 127) ? (char)ch : ' ';
    }
    gb->title[16] = 0;
    for (int i = 15; i >= 0 && gb->title[i] == ' '; i--) gb->title[i] = 0;

    uint8_t type = rom[0x147];
    gb->mbc = -1;
    for (unsigned i = 0; i < sizeof(carts) / sizeof(carts[0]); i++)
        if (carts[i].id == type) { gb->mbc = carts[i].mbc; gb->has_battery = carts[i].battery; }
    if (gb->mbc < 0) { gb->err = "unsupported cartridge type"; return -1; }

    gb->rom_banks = (int)(len / 16384);
    if (gb->rom_banks < 2) gb->rom_banks = 2;

    int want = (gb->mbc == MBC2) ? 512 : ram_sizes[rom[0x149] < 6 ? rom[0x149] : 0];
    gb->xram_len = (uint32_t)want;
    gb->xram_banks = want > 8192 ? want / 8192 : 1;

    /* The header checksum covers 0x134..0x14C and the boot ROM refuses the
     * cartridge if it does not match. Worth reporting rather than enforcing:
     * plenty of homebrew has it wrong and runs perfectly. */
    uint8_t sum = 0;
    for (int i = 0x134; i <= 0x14C; i++) sum = (uint8_t)(sum - rom[i] - 1);
    if (sum != rom[0x14D]) gb->err = "header checksum is wrong (running it anyway)";

    gb_reset(gb);
    return 0;
}

void gb_reset(gb_t *gb) {
    /* Post-boot-ROM state. There is no boot ROM here, so these are the values
     * it would have left behind -- including the ones nothing documents as
     * mattering and several games read anyway. */
    gb->a = 0x01; gb->f = 0xB0;
    gb->b = 0x00; gb->c = 0x13;
    gb->d = 0x00; gb->e = 0xD8;
    gb->h = 0x01; gb->l = 0x4D;
    gb->sp = 0xFFFE;
    gb->pc = 0x0100;
    gb->ime = 0; gb->ime_pending = 0; gb->halted = 0; gb->stopped = 0;
    gb->cyc = 0;
    gb->rom_bank = 1; gb->ram_bank = 0; gb->mbc1_mode = 0; gb->xram_on = 0;
    gb->line_t = 0; gb->win_line = 0; gb->frame_ready = 0;
    gb->div_t = 0; gb->tima_t = 0;

    memset(gb->vram, 0, sizeof(gb->vram));
    memset(gb->wram, 0, sizeof(gb->wram));
    memset(gb->oam, 0, sizeof(gb->oam));
    memset(gb->hram, 0, sizeof(gb->hram));
    memset(gb->io, 0, sizeof(gb->io));
    memset(gb->fb, 0, sizeof(gb->fb));

    gb->io[R_P1]   = 0xCF;
    gb->io[R_TIMA] = 0x00;
    gb->io[R_TMA]  = 0x00;
    gb->io[R_TAC]  = 0xF8;
    gb->io[R_IF]   = 0xE1;
    gb->io[R_LCDC] = 0x91;
    gb->io[R_STAT] = 0x85;
    gb->io[R_BGP]  = 0xFC;
    gb->io[R_OBP0] = 0xFF;
    gb->io[R_OBP1] = 0xFF;
    gb->ie = 0;

    gb->apu.on = 1;
    gb->apu.ch1.on = gb->apu.ch2.on = gb->apu.ch3.on = gb->apu.ch4.on = 0;
    gb->apu.ch4.lfsr = 0x7FFF;
}

void gb_free(gb_t *gb) { (void)gb; }   /* the app owns rom and xram */

void gb_irq(gb_t *gb, int bit) { gb->io[R_IF] |= (uint8_t)(1 << bit); }

/* ---- the mapper ---------------------------------------------------------- */
static uint32_t rom_off(gb_t *gb, uint16_t addr) {
    int bank;
    if (addr < 0x4000) {
        /* MBC1 in "advanced" mode banks the low region too, which is how the
         * 1 MiB multi-carts reach their upper halves. */
        bank = (gb->mbc == MBC1 && gb->mbc1_mode) ? (gb->ram_bank << 5) : 0;
    } else {
        bank = gb->rom_bank;
        if (gb->mbc == MBC1) bank = (bank & 0x1F) | (gb->mbc1_mode ? 0 : (gb->ram_bank << 5));
        /* MBC5 is the one that can really put bank 0 in the high window; on
         * the others the register's zero means one, which is why bank 0 is
         * unreachable there and why a game with 0x20/0x40/0x60 in it is
         * relying on exactly this. */
        if (bank == 0 && gb->mbc != MBC5) bank = 1;
    }
    if (gb->rom_banks > 0) bank %= gb->rom_banks;
    uint32_t off = (uint32_t)bank * 0x4000 + (addr & 0x3FFF);
    return off < gb->rom_len ? off : 0;
}

static void mbc_write(gb_t *gb, uint16_t addr, uint8_t v) {
    switch (gb->mbc) {
    case MBC_NONE:
        break;
    case MBC1:
        if (addr < 0x2000)      gb->xram_on = (v & 0x0F) == 0x0A;
        else if (addr < 0x4000) gb->rom_bank = (v & 0x1F) ? (v & 0x1F) : 1;
        else if (addr < 0x6000) gb->ram_bank = v & 3;
        else                    gb->mbc1_mode = v & 1;
        break;
    case MBC2:
        /* The mapper's two jobs are selected by address bit 8, not by range. */
        if (addr < 0x4000) {
            if (addr & 0x0100) gb->rom_bank = (v & 0x0F) ? (v & 0x0F) : 1;
            else               gb->xram_on = (v & 0x0F) == 0x0A;
        }
        break;
    case MBC3:
        if (addr < 0x2000)      gb->xram_on = (v & 0x0F) == 0x0A;
        else if (addr < 0x4000) gb->rom_bank = (v & 0x7F) ? (v & 0x7F) : 1;
        else if (addr < 0x6000) gb->ram_bank = v;        /* >=8 selects the RTC */
        /* The clock is not emulated: latching it does nothing and reading it
         * gives zeroes. Games that show a time show midnight; games that gate
         * on elapsed time never advance. Better than refusing to run them. */
        break;
    case MBC5:
        if (addr < 0x2000)      gb->xram_on = (v & 0x0F) == 0x0A;
        else if (addr < 0x3000) gb->rom_bank = (gb->rom_bank & 0x100) | v;
        else if (addr < 0x4000) gb->rom_bank = (gb->rom_bank & 0xFF) | ((v & 1) << 8);
        else if (addr < 0x6000) gb->ram_bank = v & 0x0F;
        break;
    }
}

static uint32_t xram_off(gb_t *gb, uint16_t addr) {
    if (gb->mbc == MBC2) return (uint32_t)(addr & 0x01FF);
    uint32_t bank = (uint32_t)(gb->ram_bank & 0x0F);
    if (gb->mbc == MBC1 && !gb->mbc1_mode) bank = 0;
    return bank * 0x2000 + (addr & 0x1FFF);
}

/* ---- the bus ------------------------------------------------------------- */
uint8_t gb_rd(gb_t *gb, uint16_t addr) {
    if (addr < 0x8000) return gb->rom ? gb->rom[rom_off(gb, addr)] : 0xFF;
    if (addr < 0xA000) return gb->vram[addr - 0x8000];
    if (addr < 0xC000) {
        if (!gb->xram_on || !gb->xram || !gb->xram_len) return 0xFF;
        uint32_t o = xram_off(gb, addr);
        if (o >= gb->xram_len) return 0xFF;
        /* MBC2's RAM is four bits wide and reads back with the top nibble set. */
        return gb->mbc == MBC2 ? (uint8_t)(gb->xram[o] | 0xF0) : gb->xram[o];
    }
    if (addr < 0xE000) return gb->wram[addr - 0xC000];
    if (addr < 0xFE00) return gb->wram[addr - 0xE000];      /* echo            */
    if (addr < 0xFEA0) return gb->oam[addr - 0xFE00];
    if (addr < 0xFF00) return 0xFF;                          /* unusable        */
    if (addr < 0xFF80) {
        uint8_t r = (uint8_t)(addr & 0x7F);
        if (r == R_P1) {
            /* Zero means pressed, and the two halves are selected by bits 4/5.
             * Both selected means both halves are wired together, which is how
             * the STOP-key check works. */
            uint8_t sel = gb->io[R_P1] & 0x30;
            uint8_t out = 0x0F;
            if (!(sel & 0x10)) out &= (uint8_t)~(gb->buttons & 0x0F);
            if (!(sel & 0x20)) out &= (uint8_t)~((gb->buttons >> 4) & 0x0F);
            return (uint8_t)(0xC0 | sel | out);
        }
        if (r >= 0x10 && r <= 0x3F) return gb_apu_read(gb, r);
        return gb->io[r];
    }
    if (addr < 0xFFFF) return gb->hram[addr - 0xFF80];
    return gb->ie;
}

void gb_wr(gb_t *gb, uint16_t addr, uint8_t v) {
    if (addr < 0x8000) { mbc_write(gb, addr, v); return; }
    if (addr < 0xA000) { gb->vram[addr - 0x8000] = v; return; }
    if (addr < 0xC000) {
        if (!gb->xram_on || !gb->xram || !gb->xram_len) return;
        uint32_t o = xram_off(gb, addr);
        if (o >= gb->xram_len) return;
        gb->xram[o] = gb->mbc == MBC2 ? (uint8_t)(v & 0x0F) : v;
        gb->xram_dirty = 1;
        return;
    }
    if (addr < 0xE000) { gb->wram[addr - 0xC000] = v; return; }
    if (addr < 0xFE00) { gb->wram[addr - 0xE000] = v; return; }
    if (addr < 0xFEA0) { gb->oam[addr - 0xFE00] = v; return; }
    if (addr < 0xFF00) return;
    if (addr < 0xFF80) {
        uint8_t r = (uint8_t)(addr & 0x7F);
        switch (r) {
        case R_P1:  gb->io[r] = (uint8_t)(v & 0x30); return;
        case R_DIV: gb->io[R_DIV] = 0; gb->div_t = 0; return;   /* any write resets */
        case R_LY:  return;                                      /* read-only       */
        case R_STAT: gb->io[r] = (uint8_t)((v & 0x78) | (gb->io[r] & 0x07)); return;
        case R_DMA: {
            /* 160 bytes from XX00 into OAM. The real thing takes 640 T-states
             * and locks most of the bus; doing it instantly is wrong in a way
             * no game has ever been able to see, because they all spin in HRAM
             * for exactly that long afterwards. */
            uint16_t src = (uint16_t)v << 8;
            for (int i = 0; i < 0xA0; i++) gb->oam[i] = gb_rd(gb, (uint16_t)(src + i));
            gb->io[r] = v;
            return;
        }
        default:
            if (r >= 0x10 && r <= 0x3F) { gb_apu_write(gb, r, v); return; }
            gb->io[r] = v;
            return;
        }
    }
    if (addr < 0xFFFF) { gb->hram[addr - 0xFF80] = v; return; }
    gb->ie = v;
}

/* ---- divider and timer --------------------------------------------------- */
void gb_timer_step(gb_t *gb, int t) {
    /* DIV is the top eight bits of a 16-bit counter running at the master
     * clock, so it increments every 256 T-states and nothing can stop it. */
    gb->div_t += t;
    while (gb->div_t >= 256) { gb->div_t -= 256; gb->io[R_DIV]++; }

    if (!(gb->io[R_TAC] & 4)) return;
    static const int period[4] = { 1024, 16, 64, 256 };
    int p = period[gb->io[R_TAC] & 3];

    gb->tima_t += t;
    while (gb->tima_t >= p) {
        gb->tima_t -= p;
        if (++gb->io[R_TIMA] == 0) {
            /* An overflow reloads from TMA and raises the interrupt. The real
             * hardware leaves TIMA at zero for four T-states first; nothing
             * here can observe the difference. */
            gb->io[R_TIMA] = gb->io[R_TMA];
            gb_irq(gb, 2);
        }
    }
}

/* -------------------------------------------------------------------------- */
void gb_run_frame(gb_t *gb, int max_t) {
    gb->frame_ready = 0;
    int spent = 0;
    while (!gb->frame_ready && spent < max_t) {
        int t = gb_step(gb);
        spent += t;
        gb_timer_step(gb, t);
        gb_ppu_step(gb, t);
        gb_apu_step(gb, t);
    }
}
