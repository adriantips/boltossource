/* ===========================================================================
 *  Four channels: two squares, a wavetable and a noise generator.
 *
 *  All of it runs off the same T-state count as everything else, which is the
 *  only way the music stays in time with the game. The frame sequencer ticks
 *  at 512 Hz and drives length, sweep and envelope on the schedule the
 *  hardware uses (length on the even steps, sweep on 2 and 6, envelope on 7).
 *
 *  Output is resampled to 48 kHz by counting T-states rather than by any
 *  filtering, so this is nearest-neighbour and it aliases. On square waves and
 *  an LFSR that is almost inaudible, and a proper band-limited resampler is a
 *  lot of arithmetic to add to an emulator running inside a compositor loop.
 * ===========================================================================*/
#include <stdint.h>
#include "gb.h"

int16_t gb_audio[GB_AUDIO_RING * 2];
volatile int gb_audio_w, gb_audio_r;

#define NR(x) (gb->io[x])

static const uint8_t duty_tab[4] = { 0x01, 0x81, 0x87, 0x7E };
/* Noise divisor codes 0..7; code 0 is half a step, hence the doubling. */
static const int noise_div[8] = { 8, 16, 32, 48, 64, 80, 96, 112 };

/* ---- register access ----------------------------------------------------- */
uint8_t gb_apu_read(gb_t *gb, uint16_t r) {
    if (r >= 0x30) return gb->apu.ch3.ram[r - 0x30];
    if (r == 0x26) {
        uint8_t v = (uint8_t)((gb->apu.on ? 0x80 : 0) | 0x70);
        if (gb->apu.ch1.on) v |= 1;
        if (gb->apu.ch2.on) v |= 2;
        if (gb->apu.ch3.on) v |= 4;
        if (gb->apu.ch4.on) v |= 8;
        return v;
    }
    /* Most of these registers read back with the write-only bits set. Some
     * games checksum them; all of them are happier if the unused bits are 1. */
    static const uint8_t ormask[0x20] = {
        0x80,0x3F,0x00,0xFF,0xBF, 0xFF,0x3F,0x00,0xFF,0xBF,
        0x7F,0xFF,0x9F,0xFF,0xBF, 0xFF,0xFF,0x00,0x00,0xBF,
        0x00,0x00,0x70,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF
    };
    return (uint8_t)(gb->io[r] | ormask[r - 0x10]);
}

static void trigger_square(gb_t *gb, int ch2) {
    /* One routine for both squares; ch1 is the one with a sweep unit. */
    __typeof__(gb->apu.ch1) *c = ch2 ? &gb->apu.ch2 : &gb->apu.ch1;
    int base = ch2 ? 0x16 : 0x11;

    c->on = c->dac;
    if (c->len == 0) c->len = 64;
    c->timer = (2048 - c->freq) * 4;
    c->vol = NR(base + 1) >> 4;
    c->env_dir = (NR(base + 1) >> 3) & 1;
    c->env_per = NR(base + 1) & 7;
    c->env_t = c->env_per;

    if (!ch2) {
        c->swp_shadow = c->freq;
        c->swp_per = (NR(0x10) >> 4) & 7;
        c->swp_dir = (NR(0x10) >> 3) & 1;
        c->swp_sh = NR(0x10) & 7;
        c->swp_t = c->swp_per ? c->swp_per : 8;
        c->swp_on = (c->swp_per || c->swp_sh);
        /* An overflow on the trigger's own first calculation kills the channel
         * immediately, before a single sample comes out of it. */
        if (c->swp_sh) {
            int n = c->swp_shadow + (c->swp_dir ? -1 : 1) * (c->swp_shadow >> c->swp_sh);
            if (n > 2047) c->on = 0;
        }
    }
}

void gb_apu_write(gb_t *gb, uint16_t r, uint8_t v) {
    if (r >= 0x30) { gb->apu.ch3.ram[r - 0x30] = v; gb->io[r] = v; return; }

    /* With the APU off, everything but NR52 ignores writes and every register
     * reads zero. */
    if (!gb->apu.on && r != 0x26) return;
    gb->io[r] = v;

    switch (r) {
    case 0x11: gb->apu.ch1.duty = v >> 6; gb->apu.ch1.len = 64 - (v & 63); break;
    case 0x12:
        gb->apu.ch1.dac = (v & 0xF8) != 0;
        if (!gb->apu.ch1.dac) gb->apu.ch1.on = 0;
        break;
    case 0x13: gb->apu.ch1.freq = (gb->apu.ch1.freq & 0x700) | v; break;
    case 0x14:
        gb->apu.ch1.freq = (gb->apu.ch1.freq & 0xFF) | ((v & 7) << 8);
        gb->apu.ch1.len_on = (v >> 6) & 1;
        if (v & 0x80) trigger_square(gb, 0);
        break;

    case 0x16: gb->apu.ch2.duty = v >> 6; gb->apu.ch2.len = 64 - (v & 63); break;
    case 0x17:
        gb->apu.ch2.dac = (v & 0xF8) != 0;
        if (!gb->apu.ch2.dac) gb->apu.ch2.on = 0;
        break;
    case 0x18: gb->apu.ch2.freq = (gb->apu.ch2.freq & 0x700) | v; break;
    case 0x19:
        gb->apu.ch2.freq = (gb->apu.ch2.freq & 0xFF) | ((v & 7) << 8);
        gb->apu.ch2.len_on = (v >> 6) & 1;
        if (v & 0x80) trigger_square(gb, 1);
        break;

    case 0x1A:
        gb->apu.ch3.dac = (v & 0x80) != 0;
        if (!gb->apu.ch3.dac) gb->apu.ch3.on = 0;
        break;
    case 0x1B: gb->apu.ch3.len = 256 - v; break;     /* wave length is 8 bits  */
    case 0x1C: gb->apu.ch3.level = (v >> 5) & 3; break;
    case 0x1D: gb->apu.ch3.freq = (gb->apu.ch3.freq & 0x700) | v; break;
    case 0x1E:
        gb->apu.ch3.freq = (gb->apu.ch3.freq & 0xFF) | ((v & 7) << 8);
        gb->apu.ch3.len_on = (v >> 6) & 1;
        if (v & 0x80) {
            gb->apu.ch3.on = gb->apu.ch3.dac;
            if (gb->apu.ch3.len == 0) gb->apu.ch3.len = 256;
            gb->apu.ch3.timer = (2048 - gb->apu.ch3.freq) * 2;
            gb->apu.ch3.pos = 0;
        }
        break;

    case 0x20: gb->apu.ch4.len = 64 - (v & 63); break;
    case 0x21:
        gb->apu.ch4.dac = (v & 0xF8) != 0;
        if (!gb->apu.ch4.dac) gb->apu.ch4.on = 0;
        break;
    case 0x22:
        gb->apu.ch4.sh = v >> 4;
        gb->apu.ch4.width = (v >> 3) & 1;
        gb->apu.ch4.div = v & 7;
        break;
    case 0x23:
        gb->apu.ch4.len_on = (v >> 6) & 1;
        if (v & 0x80) {
            gb->apu.ch4.on = gb->apu.ch4.dac;
            if (gb->apu.ch4.len == 0) gb->apu.ch4.len = 64;
            gb->apu.ch4.lfsr = 0x7FFF;
            gb->apu.ch4.timer = noise_div[gb->apu.ch4.div] << gb->apu.ch4.sh;
            gb->apu.ch4.vol = NR(0x21) >> 4;
            gb->apu.ch4.env_dir = (NR(0x21) >> 3) & 1;
            gb->apu.ch4.env_per = NR(0x21) & 7;
            gb->apu.ch4.env_t = gb->apu.ch4.env_per;
        }
        break;

    case 0x26:
        gb->apu.on = (v & 0x80) != 0;
        if (!gb->apu.on) {
            /* Switching the APU off clears every register except the wave RAM,
             * and nothing will answer a write until it is switched back on. */
            for (int i = 0x10; i <= 0x25; i++) gb->io[i] = 0;
            gb->apu.ch1.on = gb->apu.ch2.on = gb->apu.ch3.on = gb->apu.ch4.on = 0;
        }
        break;
    }
}

/* ---- the units ----------------------------------------------------------- */
static void sweep_calc(gb_t *gb) {
    __typeof__(gb->apu.ch1) *c = &gb->apu.ch1;
    if (!c->swp_on || !c->swp_per) return;
    int n = c->swp_shadow + (c->swp_dir ? -1 : 1) * (c->swp_shadow >> c->swp_sh);
    if (n > 2047) { c->on = 0; return; }
    if (c->swp_sh == 0) return;
    c->swp_shadow = n;
    c->freq = n;
    gb->io[0x13] = (uint8_t)(n & 0xFF);
    gb->io[0x14] = (uint8_t)((gb->io[0x14] & 0xF8) | ((n >> 8) & 7));
}

static void env_tick(int *vol, int dir, int per, int *t) {
    if (!per) return;
    if (--*t > 0) return;
    *t = per;
    if (dir && *vol < 15) (*vol)++;
    else if (!dir && *vol > 0) (*vol)--;
}

static void frame_seq(gb_t *gb) {
    int s = gb->apu.seq;

    if ((s & 1) == 0) {                          /* length, on the even steps  */
        if (gb->apu.ch1.len_on && gb->apu.ch1.len && --gb->apu.ch1.len == 0) gb->apu.ch1.on = 0;
        if (gb->apu.ch2.len_on && gb->apu.ch2.len && --gb->apu.ch2.len == 0) gb->apu.ch2.on = 0;
        if (gb->apu.ch3.len_on && gb->apu.ch3.len && --gb->apu.ch3.len == 0) gb->apu.ch3.on = 0;
        if (gb->apu.ch4.len_on && gb->apu.ch4.len && --gb->apu.ch4.len == 0) gb->apu.ch4.on = 0;
    }
    if (s == 2 || s == 6) {
        if (gb->apu.ch1.swp_t && --gb->apu.ch1.swp_t == 0) {
            gb->apu.ch1.swp_t = gb->apu.ch1.swp_per ? gb->apu.ch1.swp_per : 8;
            sweep_calc(gb);
        }
    }
    if (s == 7) {
        env_tick(&gb->apu.ch1.vol, gb->apu.ch1.env_dir, gb->apu.ch1.env_per, &gb->apu.ch1.env_t);
        env_tick(&gb->apu.ch2.vol, gb->apu.ch2.env_dir, gb->apu.ch2.env_per, &gb->apu.ch2.env_t);
        env_tick(&gb->apu.ch4.vol, gb->apu.ch4.env_dir, gb->apu.ch4.env_per, &gb->apu.ch4.env_t);
    }
    gb->apu.seq = (s + 1) & 7;
}

/* Each channel's current output, 0..15. */
static int out1(gb_t *gb) {
    if (!gb->apu.ch1.on || !gb->apu.ch1.dac) return 0;
    return ((duty_tab[gb->apu.ch1.duty] >> gb->apu.ch1.dpos) & 1) ? gb->apu.ch1.vol : 0;
}
static int out2(gb_t *gb) {
    if (!gb->apu.ch2.on || !gb->apu.ch2.dac) return 0;
    return ((duty_tab[gb->apu.ch2.duty] >> gb->apu.ch2.dpos) & 1) ? gb->apu.ch2.vol : 0;
}
static int out3(gb_t *gb) {
    if (!gb->apu.ch3.on || !gb->apu.ch3.dac || gb->apu.ch3.level == 0) return 0;
    uint8_t byte = gb->apu.ch3.ram[(gb->apu.ch3.pos >> 1) & 15];
    int s = (gb->apu.ch3.pos & 1) ? (byte & 15) : (byte >> 4);
    return s >> (gb->apu.ch3.level - 1);
}
static int out4(gb_t *gb) {
    if (!gb->apu.ch4.on || !gb->apu.ch4.dac) return 0;
    return (gb->apu.ch4.lfsr & 1) ? 0 : gb->apu.ch4.vol;
}

static void emit(gb_t *gb) {
    int l = 0, r = 0;
    uint8_t pan = gb->io[0x25];
    int o[4] = { out1(gb), out2(gb), out3(gb), out4(gb) };
    for (int i = 0; i < 4; i++) {
        if (pan & (1 << i))       r += o[i];
        if (pan & (0x10 << i))    l += o[i];
    }
    int lv = ((gb->io[0x24] >> 4) & 7) + 1;
    int rv = (gb->io[0x24] & 7) + 1;
    /* 4 channels * 15 * 8 volume steps, scaled to leave headroom. */
    l = l * lv * 44;
    r = r * rv * 44;
    if (l >  32000) l =  32000;
    if (r >  32000) r =  32000;

    int w = gb_audio_w;
    int next = (w + 1) % GB_AUDIO_RING;
    if (next == gb_audio_r) return;            /* the ring is full; drop it    */
    gb_audio[w * 2]     = (int16_t)l;
    gb_audio[w * 2 + 1] = (int16_t)r;
    gb_audio_w = next;
}

void gb_apu_step(gb_t *gb, int t) {
    if (!gb->apu.on) {
        /* Still advance the resampler so silence takes the right amount of
         * time; a paused ring drains and the app hears a click. */
        gb->apu.samp_t += t * 48000;
        while (gb->apu.samp_t >= GB_HZ) { gb->apu.samp_t -= GB_HZ; emit(gb); }
        return;
    }

    gb->apu.seq_t += t;
    while (gb->apu.seq_t >= 8192) { gb->apu.seq_t -= 8192; frame_seq(gb); }

    gb->apu.ch1.timer -= t;
    while (gb->apu.ch1.timer <= 0) {
        gb->apu.ch1.timer += (2048 - gb->apu.ch1.freq) * 4;
        gb->apu.ch1.dpos = (gb->apu.ch1.dpos + 1) & 7;
    }
    gb->apu.ch2.timer -= t;
    while (gb->apu.ch2.timer <= 0) {
        gb->apu.ch2.timer += (2048 - gb->apu.ch2.freq) * 4;
        gb->apu.ch2.dpos = (gb->apu.ch2.dpos + 1) & 7;
    }
    gb->apu.ch3.timer -= t;
    while (gb->apu.ch3.timer <= 0) {
        gb->apu.ch3.timer += (2048 - gb->apu.ch3.freq) * 2;
        gb->apu.ch3.pos = (gb->apu.ch3.pos + 1) & 31;
    }
    gb->apu.ch4.timer -= t;
    while (gb->apu.ch4.timer <= 0) {
        int period = noise_div[gb->apu.ch4.div] << gb->apu.ch4.sh;
        gb->apu.ch4.timer += period > 0 ? period : 8;
        /* The LFSR feeds bit 0 xor bit 1 back into bit 14, and in narrow mode
         * into bit 6 as well -- which is what makes short noise sound pitched
         * rather than like static. */
        int x = (gb->apu.ch4.lfsr ^ (gb->apu.ch4.lfsr >> 1)) & 1;
        gb->apu.ch4.lfsr = (gb->apu.ch4.lfsr >> 1) | (x << 14);
        if (gb->apu.ch4.width)
            gb->apu.ch4.lfsr = (gb->apu.ch4.lfsr & ~0x40) | (x << 6);
    }

    gb->apu.samp_t += t * 48000;
    while (gb->apu.samp_t >= GB_HZ) { gb->apu.samp_t -= GB_HZ; emit(gb); }
}
