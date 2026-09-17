/* ===========================================================================
 *  The SM83 core.
 *
 *  Sharp's chip, not a Z80 and not an 8080: no IX/IY, no shadow registers, no
 *  parity flag, and a handful of instructions (LDH, LD (HL+), ADD SP,n) that
 *  belong to neither. The half-carry flag is the part that bites -- it is used
 *  by exactly one instruction, DAA, and getting it wrong anywhere else shows
 *  up only as a score displayed in hexadecimal three levels into a game.
 *
 *  The 0x40-0xBF block is decoded arithmetically rather than as 128 switch
 *  cases, because it is completely regular: three bits of destination, three
 *  of source, and 0x76 punched out of the middle for HALT.
 * ===========================================================================*/
#include <stdint.h>
#include "gb.h"

#define FZ 0x80
#define FN 0x40
#define FH 0x20
#define FC 0x10

/* Not-taken timings; a taken branch adds its own extra below. */
static const uint8_t cyc[256] = {
     4,12, 8, 8, 4, 4, 8, 4,20, 8, 8, 8, 4, 4, 8, 4,
     4,12, 8, 8, 4, 4, 8, 4,12, 8, 8, 8, 4, 4, 8, 4,
     8,12, 8, 8, 4, 4, 8, 4, 8, 8, 8, 8, 4, 4, 8, 4,
     8,12, 8, 8,12,12,12, 4, 8, 8, 8, 8, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     8, 8, 8, 8, 8, 8, 4, 8, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     8,12,12,16,12,16, 8,16, 8,16,12, 4,12,24, 8,16,
     8,12,12, 0,12,16, 8,16, 8,16,12, 0,12, 0, 8,16,
    12,12, 8, 0, 0,16, 8,16,16, 4,16, 0, 0, 0, 8,16,
    12,12, 8, 4, 0,16, 8,16,12, 8,16, 4, 0, 0, 8,16
};

/* ---- register plumbing --------------------------------------------------- */
static uint16_t bc(gb_t *g) { return (uint16_t)((g->b << 8) | g->c); }
static uint16_t de(gb_t *g) { return (uint16_t)((g->d << 8) | g->e); }
static uint16_t hl(gb_t *g) { return (uint16_t)((g->h << 8) | g->l); }
static void set_bc(gb_t *g, uint16_t v) { g->b = (uint8_t)(v >> 8); g->c = (uint8_t)v; }
static void set_de(gb_t *g, uint16_t v) { g->d = (uint8_t)(v >> 8); g->e = (uint8_t)v; }
static void set_hl(gb_t *g, uint16_t v) { g->h = (uint8_t)(v >> 8); g->l = (uint8_t)v; }

static uint8_t fetch(gb_t *g)  { return gb_rd(g, g->pc++); }
static uint16_t fetch16(gb_t *g) {
    uint8_t lo = fetch(g), hi = fetch(g);
    return (uint16_t)(lo | (hi << 8));
}
static void push(gb_t *g, uint16_t v) {
    gb_wr(g, --g->sp, (uint8_t)(v >> 8));
    gb_wr(g, --g->sp, (uint8_t)v);
}
static uint16_t pop(gb_t *g) {
    uint8_t lo = gb_rd(g, g->sp++), hi = gb_rd(g, g->sp++);
    return (uint16_t)(lo | (hi << 8));
}

/* r index: 0=B 1=C 2=D 3=E 4=H 5=L 6=(HL) 7=A. Slot 6 costs a bus cycle, which
 * the timing table has already accounted for. */
static uint8_t getr(gb_t *g, int i) {
    switch (i) {
    case 0: return g->b; case 1: return g->c; case 2: return g->d; case 3: return g->e;
    case 4: return g->h; case 5: return g->l; case 6: return gb_rd(g, hl(g));
    default: return g->a;
    }
}
static void setr(gb_t *g, int i, uint8_t v) {
    switch (i) {
    case 0: g->b = v; break; case 1: g->c = v; break;
    case 2: g->d = v; break; case 3: g->e = v; break;
    case 4: g->h = v; break; case 5: g->l = v; break;
    case 6: gb_wr(g, hl(g), v); break;
    default: g->a = v; break;
    }
}

static void flags(gb_t *g, int z, int n, int h, int c) {
    g->f = (uint8_t)((z ? FZ : 0) | (n ? FN : 0) | (h ? FH : 0) | (c ? FC : 0));
}

/* ---- ALU ----------------------------------------------------------------- */
static void alu(gb_t *g, int op, uint8_t v) {
    int a = g->a, r, cf = (g->f & FC) ? 1 : 0;
    switch (op) {
    case 0: r = a + v;           flags(g, !(r & 0xFF), 0, ((a & 15) + (v & 15)) > 15, r > 0xFF); g->a = (uint8_t)r; break;
    case 1: r = a + v + cf;      flags(g, !(r & 0xFF), 0, ((a & 15) + (v & 15) + cf) > 15, r > 0xFF); g->a = (uint8_t)r; break;
    case 2: r = a - v;           flags(g, !(r & 0xFF), 1, (a & 15) < (v & 15), r < 0); g->a = (uint8_t)r; break;
    case 3: r = a - v - cf;      flags(g, !(r & 0xFF), 1, (a & 15) < ((v & 15) + cf), r < 0); g->a = (uint8_t)r; break;
    case 4: g->a = (uint8_t)(a & v); flags(g, !g->a, 0, 1, 0); break;
    case 5: g->a = (uint8_t)(a ^ v); flags(g, !g->a, 0, 0, 0); break;
    case 6: g->a = (uint8_t)(a | v); flags(g, !g->a, 0, 0, 0); break;
    default: r = a - v;          flags(g, !(r & 0xFF), 1, (a & 15) < (v & 15), r < 0); break;  /* CP */
    }
}

static uint8_t inc8(gb_t *g, uint8_t v) {
    uint8_t r = (uint8_t)(v + 1);
    g->f = (uint8_t)((g->f & FC) | (r ? 0 : FZ) | (((v & 15) + 1) > 15 ? FH : 0));
    return r;
}
static uint8_t dec8(gb_t *g, uint8_t v) {
    uint8_t r = (uint8_t)(v - 1);
    g->f = (uint8_t)((g->f & FC) | FN | (r ? 0 : FZ) | ((v & 15) == 0 ? FH : 0));
    return r;
}
static void add16(gb_t *g, uint16_t v) {
    uint32_t r = (uint32_t)hl(g) + v;
    g->f = (uint8_t)((g->f & FZ)
         | (((hl(g) & 0x0FFF) + (v & 0x0FFF)) > 0x0FFF ? FH : 0)
         | (r > 0xFFFF ? FC : 0));
    set_hl(g, (uint16_t)r);
}
/* ADD SP,n and LD HL,SP+n set the carry flags from the LOW BYTE, always, and
 * Z and N are cleared even though the result is sixteen bits wide. */
static uint16_t addsp(gb_t *g, int8_t n) {
    uint16_t sp = g->sp;
    uint16_t r = (uint16_t)(sp + n);
    g->f = (uint8_t)((((sp & 0x0F) + (n & 0x0F)) > 0x0F ? FH : 0)
                   | (((sp & 0xFF) + (n & 0xFF)) > 0xFF ? FC : 0));
    return r;
}

/* ---- CB block ------------------------------------------------------------ */
static uint8_t cb_shift(gb_t *g, int op, uint8_t v) {
    int c = (g->f & FC) ? 1 : 0, out;
    uint8_t r;
    switch (op) {
    case 0: out = v >> 7; r = (uint8_t)((v << 1) | out); break;                 /* RLC  */
    case 1: out = v & 1;  r = (uint8_t)((v >> 1) | (out << 7)); break;          /* RRC  */
    case 2: out = v >> 7; r = (uint8_t)((v << 1) | c); break;                   /* RL   */
    case 3: out = v & 1;  r = (uint8_t)((v >> 1) | (c << 7)); break;            /* RR   */
    case 4: out = v >> 7; r = (uint8_t)(v << 1); break;                         /* SLA  */
    case 5: out = v & 1;  r = (uint8_t)((v >> 1) | (v & 0x80)); break;          /* SRA  */
    case 6: out = 0;      r = (uint8_t)((v << 4) | (v >> 4)); break;            /* SWAP */
    default: out = v & 1; r = (uint8_t)(v >> 1); break;                         /* SRL  */
    }
    flags(g, !r, 0, 0, out);
    return r;
}

static int do_cb(gb_t *g) {
    uint8_t op = fetch(g);
    int idx = op & 7, grp = op >> 6, bit = (op >> 3) & 7;
    uint8_t v = getr(g, idx);
    int t = (idx == 6) ? 16 : 8;

    if (grp == 0) { setr(g, idx, cb_shift(g, bit, v)); return t; }
    if (grp == 1) {                                        /* BIT: no write-back */
        g->f = (uint8_t)((g->f & FC) | FH | ((v & (1 << bit)) ? 0 : FZ));
        return idx == 6 ? 12 : 8;
    }
    if (grp == 2) setr(g, idx, (uint8_t)(v & ~(1 << bit)));   /* RES */
    else          setr(g, idx, (uint8_t)(v | (1 << bit)));    /* SET */
    return t;
}

/* ---- interrupts ---------------------------------------------------------- */
static int service_irq(gb_t *g) {
    uint8_t pending = (uint8_t)(g->ie & g->io[R_IF] & 0x1F);
    if (!pending) return 0;

    /* Any pending interrupt wakes the CPU whether or not IME is set; that is
     * the whole point of HALT as a power-saving instruction. */
    g->halted = 0;
    if (!g->ime) return 0;

    for (int i = 0; i < 5; i++) {
        if (!(pending & (1 << i))) continue;
        g->ime = 0;
        g->io[R_IF] &= (uint8_t)~(1 << i);
        push(g, g->pc);
        g->pc = (uint16_t)(0x40 + i * 8);
        return 20;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
int gb_step(gb_t *gb) {
    /* EI takes effect after the instruction that follows it, which is what
     * makes "EI; HALT" safe and "DI; EI" a no-op. */
    int enable_now = gb->ime_pending;
    if (enable_now) gb->ime_pending = 0;

    int t = service_irq(gb);
    if (t) { gb->cyc += t; return t; }
    if (enable_now) gb->ime = 1;

    if (gb->halted) { gb->cyc += 4; return 4; }

    uint8_t op = fetch(gb);
    t = cyc[op];

    /* The regular block: LD r,r' and ALU A,r, with HALT punched out of it. */
    if (op >= 0x40 && op <= 0xBF) {
        if (op == 0x76) {
            /* HALT with interrupts disabled and something already pending does
             * not halt -- it fails to advance PC once, so the next byte runs
             * twice. Games do hit this; it is cheaper to be right than to
             * explain the symptom later. */
            if (!gb->ime && (gb->ie & gb->io[R_IF] & 0x1F)) gb->pc--;
            else gb->halted = 1;
        } else if (op < 0x80) {
            setr(gb, (op >> 3) & 7, getr(gb, op & 7));
        } else {
            alu(gb, (op >> 3) & 7, getr(gb, op & 7));
        }
        gb->cyc += t;
        return t;
    }

    switch (op) {
    case 0x00: break;                                        /* NOP            */
    case 0x10: gb->pc++; gb->stopped = 1; break;             /* STOP (eats a byte) */
    case 0x76: break;                                        /* unreachable    */
    case 0xCB: t = do_cb(gb); break;

    /* --- 16-bit loads --- */
    case 0x01: set_bc(gb, fetch16(gb)); break;
    case 0x11: set_de(gb, fetch16(gb)); break;
    case 0x21: set_hl(gb, fetch16(gb)); break;
    case 0x31: gb->sp = fetch16(gb); break;
    case 0x08: { uint16_t a = fetch16(gb);
                 gb_wr(gb, a, (uint8_t)gb->sp);
                 gb_wr(gb, (uint16_t)(a + 1), (uint8_t)(gb->sp >> 8)); break; }
    case 0xF9: gb->sp = hl(gb); break;
    case 0xF8: set_hl(gb, addsp(gb, (int8_t)fetch(gb))); break;
    case 0xE8: gb->sp = addsp(gb, (int8_t)fetch(gb)); break;

    /* --- 8-bit loads --- */
    case 0x02: gb_wr(gb, bc(gb), gb->a); break;
    case 0x12: gb_wr(gb, de(gb), gb->a); break;
    case 0x0A: gb->a = gb_rd(gb, bc(gb)); break;
    case 0x1A: gb->a = gb_rd(gb, de(gb)); break;
    case 0x22: gb_wr(gb, hl(gb), gb->a); set_hl(gb, (uint16_t)(hl(gb) + 1)); break;
    case 0x32: gb_wr(gb, hl(gb), gb->a); set_hl(gb, (uint16_t)(hl(gb) - 1)); break;
    case 0x2A: gb->a = gb_rd(gb, hl(gb)); set_hl(gb, (uint16_t)(hl(gb) + 1)); break;
    case 0x3A: gb->a = gb_rd(gb, hl(gb)); set_hl(gb, (uint16_t)(hl(gb) - 1)); break;
    case 0x06: gb->b = fetch(gb); break;
    case 0x0E: gb->c = fetch(gb); break;
    case 0x16: gb->d = fetch(gb); break;
    case 0x1E: gb->e = fetch(gb); break;
    case 0x26: gb->h = fetch(gb); break;
    case 0x2E: gb->l = fetch(gb); break;
    case 0x36: gb_wr(gb, hl(gb), fetch(gb)); break;
    case 0x3E: gb->a = fetch(gb); break;
    case 0xE0: gb_wr(gb, (uint16_t)(0xFF00 + fetch(gb)), gb->a); break;
    case 0xF0: gb->a = gb_rd(gb, (uint16_t)(0xFF00 + fetch(gb))); break;
    case 0xE2: gb_wr(gb, (uint16_t)(0xFF00 + gb->c), gb->a); break;
    case 0xF2: gb->a = gb_rd(gb, (uint16_t)(0xFF00 + gb->c)); break;
    case 0xEA: gb_wr(gb, fetch16(gb), gb->a); break;
    case 0xFA: gb->a = gb_rd(gb, fetch16(gb)); break;

    /* --- inc/dec --- */
    case 0x04: gb->b = inc8(gb, gb->b); break;
    case 0x0C: gb->c = inc8(gb, gb->c); break;
    case 0x14: gb->d = inc8(gb, gb->d); break;
    case 0x1C: gb->e = inc8(gb, gb->e); break;
    case 0x24: gb->h = inc8(gb, gb->h); break;
    case 0x2C: gb->l = inc8(gb, gb->l); break;
    case 0x34: gb_wr(gb, hl(gb), inc8(gb, gb_rd(gb, hl(gb)))); break;
    case 0x3C: gb->a = inc8(gb, gb->a); break;
    case 0x05: gb->b = dec8(gb, gb->b); break;
    case 0x0D: gb->c = dec8(gb, gb->c); break;
    case 0x15: gb->d = dec8(gb, gb->d); break;
    case 0x1D: gb->e = dec8(gb, gb->e); break;
    case 0x25: gb->h = dec8(gb, gb->h); break;
    case 0x2D: gb->l = dec8(gb, gb->l); break;
    case 0x35: gb_wr(gb, hl(gb), dec8(gb, gb_rd(gb, hl(gb)))); break;
    case 0x3D: gb->a = dec8(gb, gb->a); break;

    case 0x03: set_bc(gb, (uint16_t)(bc(gb) + 1)); break;
    case 0x13: set_de(gb, (uint16_t)(de(gb) + 1)); break;
    case 0x23: set_hl(gb, (uint16_t)(hl(gb) + 1)); break;
    case 0x33: gb->sp++; break;
    case 0x0B: set_bc(gb, (uint16_t)(bc(gb) - 1)); break;
    case 0x1B: set_de(gb, (uint16_t)(de(gb) - 1)); break;
    case 0x2B: set_hl(gb, (uint16_t)(hl(gb) - 1)); break;
    case 0x3B: gb->sp--; break;

    case 0x09: add16(gb, bc(gb)); break;
    case 0x19: add16(gb, de(gb)); break;
    case 0x29: add16(gb, hl(gb)); break;
    case 0x39: add16(gb, gb->sp); break;

    /* --- rotates on A: these always clear Z, unlike their CB twins --- */
    case 0x07: { int c = gb->a >> 7; gb->a = (uint8_t)((gb->a << 1) | c); flags(gb, 0, 0, 0, c); break; }
    case 0x0F: { int c = gb->a & 1;  gb->a = (uint8_t)((gb->a >> 1) | (c << 7)); flags(gb, 0, 0, 0, c); break; }
    case 0x17: { int c = gb->a >> 7, o = (gb->f & FC) ? 1 : 0;
                 gb->a = (uint8_t)((gb->a << 1) | o); flags(gb, 0, 0, 0, c); break; }
    case 0x1F: { int c = gb->a & 1, o = (gb->f & FC) ? 1 : 0;
                 gb->a = (uint8_t)((gb->a >> 1) | (o << 7)); flags(gb, 0, 0, 0, c); break; }

    /* --- misc --- */
    case 0x27: {                                          /* DAA               */
        int a = gb->a, adj = 0, c = 0;
        if ((gb->f & FH) || (!(gb->f & FN) && (a & 15) > 9)) adj |= 0x06;
        if ((gb->f & FC) || (!(gb->f & FN) && a > 0x99))     { adj |= 0x60; c = 1; }
        a += (gb->f & FN) ? -adj : adj;
        gb->a = (uint8_t)a;
        gb->f = (uint8_t)((gb->f & FN) | (gb->a ? 0 : FZ) | (c ? FC : 0));
        break;
    }
    case 0x2F: gb->a = (uint8_t)~gb->a; gb->f |= FN | FH; break;
    case 0x37: gb->f = (uint8_t)((gb->f & FZ) | FC); break;
    case 0x3F: gb->f = (uint8_t)((gb->f & FZ) | ((gb->f & FC) ? 0 : FC)); break;
    case 0xF3: gb->ime = 0; gb->ime_pending = 0; break;
    case 0xFB: gb->ime_pending = 1; break;

    /* --- jumps --- */
    case 0xC3: gb->pc = fetch16(gb); break;
    case 0xE9: gb->pc = hl(gb); break;
    case 0x18: { int8_t r = (int8_t)fetch(gb); gb->pc = (uint16_t)(gb->pc + r); break; }
    case 0x20: case 0x28: case 0x30: case 0x38: {
        int8_t r = (int8_t)fetch(gb);
        int cond = (op == 0x20) ? !(gb->f & FZ) : (op == 0x28) ? !!(gb->f & FZ)
                 : (op == 0x30) ? !(gb->f & FC) : !!(gb->f & FC);
        if (cond) { gb->pc = (uint16_t)(gb->pc + r); t = 12; }
        break;
    }
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: {
        uint16_t a = fetch16(gb);
        int cond = (op == 0xC2) ? !(gb->f & FZ) : (op == 0xCA) ? !!(gb->f & FZ)
                 : (op == 0xD2) ? !(gb->f & FC) : !!(gb->f & FC);
        if (cond) { gb->pc = a; t = 16; }
        break;
    }
    case 0xCD: { uint16_t a = fetch16(gb); push(gb, gb->pc); gb->pc = a; break; }
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: {
        uint16_t a = fetch16(gb);
        int cond = (op == 0xC4) ? !(gb->f & FZ) : (op == 0xCC) ? !!(gb->f & FZ)
                 : (op == 0xD4) ? !(gb->f & FC) : !!(gb->f & FC);
        if (cond) { push(gb, gb->pc); gb->pc = a; t = 24; }
        break;
    }
    case 0xC9: gb->pc = pop(gb); break;
    case 0xD9: gb->pc = pop(gb); gb->ime = 1; break;        /* RETI            */
    case 0xC0: case 0xC8: case 0xD0: case 0xD8: {
        int cond = (op == 0xC0) ? !(gb->f & FZ) : (op == 0xC8) ? !!(gb->f & FZ)
                 : (op == 0xD0) ? !(gb->f & FC) : !!(gb->f & FC);
        if (cond) { gb->pc = pop(gb); t = 20; }
        break;
    }
    case 0xC7: case 0xCF: case 0xD7: case 0xDF:
    case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        push(gb, gb->pc);
        gb->pc = (uint16_t)(op & 0x38);
        break;

    /* --- stack --- */
    case 0xC1: set_bc(gb, pop(gb)); break;
    case 0xD1: set_de(gb, pop(gb)); break;
    case 0xE1: set_hl(gb, pop(gb)); break;
    case 0xF1: { uint16_t v = pop(gb);
                 gb->a = (uint8_t)(v >> 8);
                 gb->f = (uint8_t)(v & 0xF0); break; }   /* low nibble of F is wired to 0 */
    case 0xC5: push(gb, bc(gb)); break;
    case 0xD5: push(gb, de(gb)); break;
    case 0xE5: push(gb, hl(gb)); break;
    case 0xF5: push(gb, (uint16_t)((gb->a << 8) | (gb->f & 0xF0))); break;

    /* --- ALU with an immediate --- */
    case 0xC6: alu(gb, 0, fetch(gb)); break;
    case 0xCE: alu(gb, 1, fetch(gb)); break;
    case 0xD6: alu(gb, 2, fetch(gb)); break;
    case 0xDE: alu(gb, 3, fetch(gb)); break;
    case 0xE6: alu(gb, 4, fetch(gb)); break;
    case 0xEE: alu(gb, 5, fetch(gb)); break;
    case 0xF6: alu(gb, 6, fetch(gb)); break;
    case 0xFE: alu(gb, 7, fetch(gb)); break;

    default:
        /* D3, DB, DD, E3, E4, EB, EC, ED, F4, FC, FD are not instructions. On
         * real hardware they lock the CPU until a reset. Treating them as a
         * NOP means a ROM that jumps into its own data limps on instead of
         * freezing, which is more useful and no less accurate than pretending
         * to know what the chip does. */
        t = 4;
        break;
    }

    gb->cyc += t;
    return t;
}
