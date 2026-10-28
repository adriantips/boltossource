/* ===========================================================================
 *  BoltOS Mini  -  mini/h264mb.c
 *
 *  Slice data: every macroblock's syntax (7.3.5), from CAVLC (9.2) or CABAC
 *  (9.3), turned into what h264pred.c needs -- intra modes, motion vectors
 *  and reference indices (8.4.1), and dequantised residuals (8.5) -- and
 *  then reconstructed.
 * ===========================================================================*/
#include <stdint.h>
#include "h264int.h"
#include "string.h"

/* ---- CAVLC's code tables (Table 9-5, 9-7, 9-8, 9-9, 9-10) ------------------------------ */
static const uint8_t ct_len[4][4 * 17] = {
 { 1,0,0,0, 6,2,0,0, 8,6,3,0, 9,8,7,5, 10,9,8,6, 11,10,9,7, 13,11,10,8, 13,13,11,9, 13,13,13,10,
   14,14,13,11, 14,14,14,13, 15,15,14,14, 15,15,15,14, 16,15,15,15, 16,16,16,15, 16,16,16,16, 16,16,16,16 },
 { 2,0,0,0, 6,2,0,0, 6,5,3,0, 7,6,6,4, 8,6,6,4, 8,7,7,5, 9,8,8,6, 11,9,9,6, 11,11,11,7,
   12,11,11,9, 12,12,12,11, 12,12,12,11, 13,13,13,12, 13,13,13,13, 13,14,13,13, 14,14,14,13, 14,14,14,14 },
 { 4,0,0,0, 6,4,0,0, 6,5,4,0, 6,5,5,4, 7,5,5,4, 7,5,5,4, 7,6,6,4, 7,6,6,4, 8,7,7,5,
   8,8,7,6, 9,8,8,7, 9,9,8,8, 9,9,9,8, 10,9,9,9, 10,10,10,10, 10,10,10,10, 10,10,10,10 },
 { 6,0,0,0, 6,6,0,0, 6,6,6,0, 6,6,6,6, 6,6,6,6, 6,6,6,6, 6,6,6,6, 6,6,6,6, 6,6,6,6,
   6,6,6,6, 6,6,6,6, 6,6,6,6, 6,6,6,6, 6,6,6,6, 6,6,6,6, 6,6,6,6, 6,6,6,6 },
};
static const uint8_t ct_bits[4][4 * 17] = {
 { 1,0,0,0, 5,1,0,0, 7,4,1,0, 7,6,5,3, 7,6,5,3, 7,6,5,4, 15,6,5,4, 11,14,5,4, 8,10,13,4,
   15,14,9,4, 11,10,13,12, 15,14,9,12, 11,10,13,8, 15,1,9,12, 11,14,13,8, 7,10,9,12, 4,6,5,8 },
 { 3,0,0,0, 11,2,0,0, 7,7,3,0, 7,10,9,5, 7,6,5,4, 4,6,5,6, 7,6,5,8, 15,6,5,4, 11,14,13,4,
   15,10,9,4, 11,14,13,12, 8,10,9,8, 15,14,13,12, 11,10,9,12, 7,11,6,8, 9,8,10,1, 7,6,5,4 },
 { 15,0,0,0, 15,14,0,0, 11,15,13,0, 8,12,14,12, 15,10,11,11, 11,8,9,10, 9,14,13,9, 8,10,9,8, 15,14,13,13,
   11,14,10,12, 15,10,13,12, 11,14,9,12, 8,10,13,8, 13,7,9,12, 9,12,11,10, 5,8,7,6, 1,4,3,2 },
 { 3,0,0,0, 0,1,0,0, 4,5,6,0, 8,9,10,11, 12,13,14,15, 16,17,18,19, 20,21,22,23, 24,25,26,27, 28,29,30,31,
   32,33,34,35, 36,37,38,39, 40,41,42,43, 44,45,46,47, 48,49,50,51, 52,53,54,55, 56,57,58,59, 60,61,62,63 },
};
static const uint8_t cdc_len[4 * 5] = { 2,0,0,0, 6,1,0,0, 6,6,3,0, 6,7,7,6, 6,8,8,7 };
static const uint8_t cdc_bits[4 * 5] = { 1,0,0,0, 7,1,0,0, 4,6,1,0, 3,3,2,5, 2,3,2,0 };
static const uint8_t tz_len[15][16] = {
    {1,3,3,4,4,5,5,6,6,7,7,8,8,9,9,9}, {3,3,3,3,3,4,4,4,4,5,5,6,6,6,6}, {4,3,3,3,4,4,3,3,4,5,5,6,5,6},
    {5,3,4,4,3,3,3,4,3,4,5,5,5}, {4,4,4,3,3,3,3,3,4,5,4,5}, {6,5,3,3,3,3,3,3,4,3,6}, {6,5,3,3,3,2,3,4,3,6},
    {6,4,5,3,2,2,3,3,6}, {6,6,4,2,2,3,2,5}, {5,5,3,2,2,2,4}, {4,4,3,3,1,3}, {4,4,2,1,3}, {3,3,1,2}, {2,2,1}, {1,1},
};
static const uint8_t tz_bits[15][16] = {
    {1,3,2,3,2,3,2,3,2,3,2,3,2,3,2,1}, {7,6,5,4,3,5,4,3,2,3,2,3,2,1,0}, {5,7,6,5,4,3,4,3,2,3,2,1,1,0},
    {3,7,5,4,6,5,4,3,3,2,2,1,0}, {5,4,3,7,6,5,4,3,2,1,1,0}, {1,1,7,6,5,4,3,2,1,1,0}, {1,1,5,4,3,3,2,1,1,0},
    {1,1,1,3,3,2,2,1,0}, {1,0,1,3,2,1,1,1}, {1,0,1,3,2,1,1}, {0,1,1,2,1,3}, {0,1,1,1,1}, {0,1,1,1}, {0,1,1}, {0,1},
};
static const uint8_t ctz_len[3][4] = { {1,2,3,3}, {1,2,2,0}, {1,1,0,0} };
static const uint8_t ctz_bits[3][4] = { {1,1,1,0}, {1,1,0,0}, {1,0,0,0} };
static const uint8_t run_len[7][16] = { {1,1}, {1,2,2}, {2,2,2,2}, {2,2,2,3,3}, {2,2,3,3,3,3}, {2,3,3,3,3,3,3},
                                        {3,3,3,3,3,3,3,4,5,6,7,8,9,10,11} };
static const uint8_t run_bits[7][16] = { {1,0}, {1,1,0}, {3,2,1,0}, {3,2,1,1,0}, {3,2,3,2,1,0}, {3,0,1,3,2,5,4},
                                         {7,6,5,4,3,2,1,1,1,1,1,1,1,1,1} };
static const uint8_t cbp_intra[48] = { 47,31,15,0,23,27,29,30,7,11,13,14,39,43,45,46,16,3,5,10,12,19,21,26,28,35,37,42,44,1,2,4,
                                       8,17,18,20,24,6,9,22,25,32,33,34,36,40,38,41 };
static const uint8_t cbp_inter[48] = { 0,16,1,2,4,8,32,3,5,10,12,15,47,7,11,13,14,6,9,31,35,37,42,44,33,34,36,40,39,43,45,46,
                                       17,18,20,24,19,21,26,28,23,27,29,30,22,25,38,41 };
/* zigzag: scan position -> raster index in the 4x4 */
static const uint8_t zz[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const uint8_t dq[6][3] = { {10,13,16}, {11,14,18}, {13,16,20}, {14,18,23}, {16,20,25}, {18,23,29} };
static const uint8_t qpc_tab[52] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,
                                     29,30,31,32,32,33,34,34,35,35,36,36,37,37,37,38,38,38,39,39,39,39 };

/* A lookup table per code: indexed by the next `bits` bits, (length << 8) | symbol. */
typedef struct { uint16_t *t; int bits; } vlc_t;
static vlc_t v_ct[4], v_cdc, v_tz[15], v_ctz[3], v_run[7];
static uint16_t vlc_mem[3 * 65536 + 1024 + 15 * 512 + 3 * 8 + 7 * 2048 + 256];
static int vlc_used, vlc_ready;

static void vlc_make(vlc_t *v, const uint8_t *len, const uint8_t *bits, int n) {
    int max = 0;
    for (int i = 0; i < n; i++) if (len[i] > max) max = len[i];
    v->bits = max;
    v->t = vlc_mem + vlc_used;
    vlc_used += 1 << max;
    for (int i = 0; i < (1 << max); i++) v->t[i] = 0;
    for (int i = 0; i < n; i++) {
        if (!len[i]) continue;
        int sh = max - len[i];
        uint32_t base = (uint32_t)bits[i] << sh;
        for (uint32_t k = 0; k < (1u << sh); k++) v->t[base + k] = (uint16_t)(len[i] << 8 | i);
    }
}

static void vlc_init(void) {
    if (vlc_ready) return;
    vlc_ready = 1;
    for (int i = 0; i < 4; i++) vlc_make(&v_ct[i], ct_len[i], ct_bits[i], 4 * 17);
    vlc_make(&v_cdc, cdc_len, cdc_bits, 4 * 5);
    for (int i = 0; i < 15; i++) vlc_make(&v_tz[i], tz_len[i], tz_bits[i], 16 - i);
    for (int i = 0; i < 3; i++) vlc_make(&v_ctz[i], ctz_len[i], ctz_bits[i], 4 - i);
    for (int i = 0; i < 7; i++) vlc_make(&v_run[i], run_len[i], run_bits[i], i == 6 ? 15 : i + 2);
}

static inline int vlc(bs_t *b, const vlc_t *v) {
    uint16_t e = v->t[bs_peek32(b) >> (32 - v->bits)];
    b->pos += e >> 8;
    return e & 255;
}

/* ---- CABAC's engine (9.3.3.2) --------------------------------------------------------------- */
static inline int cab_bit(h264 *d) { return bs_u1(&d->bs); }

static void cab_start(h264 *d) {
    d->bs.pos = (d->bs.pos + 7) & ~7;                          /* cabac_alignment_one_bit */
    d->cab.range = 510;
    d->cab.offset = bs_u(&d->bs, 9);
}

static inline int cab_decide(h264 *d, int ci) {
    uint8_t *c = &d->ctx[ci];
    int state = *c >> 1, mps = *c & 1, bin;
    uint32_t lps = h264_lps_range[state][(d->cab.range >> 6) & 3];
    d->cab.range -= lps;
    if (d->cab.offset >= d->cab.range) {
        bin = !mps;
        d->cab.offset -= d->cab.range;
        d->cab.range = lps;
        if (state == 0) mps = !mps;
        state = h264_lps_next[state];
    } else {
        bin = mps;
        if (state < 62) state++;
    }
    *c = (uint8_t)(state << 1 | mps);
    while (d->cab.range < 256) { d->cab.range <<= 1; d->cab.offset = d->cab.offset << 1 | (uint32_t)cab_bit(d); }
    return bin;
}

static inline int cab_bypass(h264 *d) {
    d->cab.offset = d->cab.offset << 1 | (uint32_t)cab_bit(d);
    if (d->cab.offset >= d->cab.range) { d->cab.offset -= d->cab.range; return 1; }
    return 0;
}

static int cab_terminate(h264 *d) {
    d->cab.range -= 2;
    if (d->cab.offset >= d->cab.range) return 1;
    while (d->cab.range < 256) { d->cab.range <<= 1; d->cab.offset = d->cab.offset << 1 | (uint32_t)cab_bit(d); }
    return 0;
}

void cabac_init_ctx(h264 *d) {
    const int8_t (*t)[2] = d->sl.type == 2 ? h264_ctx_init_i : d->sl.cabac_init_idc == 0 ? h264_ctx_init_pb0 :
                           d->sl.cabac_init_idc == 1 ? h264_ctx_init_pb1 : h264_ctx_init_pb2;
    int qp = clip3(0, 51, d->sl.qp);
    for (int i = 0; i < 460; i++) {
        int pre = clip3(1, 126, ((t[i][0] * qp) >> 4) + t[i][1]);
        d->ctx[i] = pre <= 63 ? (uint8_t)((63 - pre) << 1) : (uint8_t)((pre - 64) << 1 | 1);
    }
}

/* ---- neighbours ------------------------------------------------------------------------------- */
int h264_avail(h264 *d, int mx, int my) {
    if (mx < 0 || my < 0 || mx >= d->mb_w || my >= d->mb_h) return 0;
    mbinfo *m = &d->cur->mb[my * d->mb_w + mx];
    return m->slice == d->slice_num && (my * d->mb_w + mx) < d->mba;
}
static mbinfo *mb_at(h264 *d, int mx, int my) { return h264_avail(d, mx, my) ? &d->cur->mb[my * d->mb_w + mx] : 0; }

/* The MB and 4x4 block holding luma 4x4 position (x, y), relative to the current MB
 * (x, y in -1..4). For inside the current MB, `cur` is returned regardless. */
static mbinfo *nb4(h264 *d, mbinfo *cur, int x, int y, int *blk) {
    int mx = d->mbx, my = d->mby;
    if (x < 0) { mx--; x += 4; } else if (x > 3) { mx++; x -= 4; }
    if (y < 0) { my--; y += 4; }
    *blk = blk_at[y][x];
    if (mx == d->mbx && my == d->mby) return cur;
    return mb_at(d, mx, my);
}

/* ---- motion vectors (8.4.1) --------------------------------------------------------------------- */
/* Neighbour N's refIdx and mv in list l, for a partition whose first 4x4 is `first`.
 * Returns 0 if N is not available (outside, other slice, not yet decoded). */
static int nb_mv(h264 *d, mbinfo *cur, int l, int x, int y, int first, int *ref, int16_t mv[2]) {
    int blk;
    if (x > 3 && y >= 0) return 0;
    mbinfo *n = nb4(d, cur, x, y, &blk);
    if (!n) return 0;
    if (n == cur && blk >= first) return 0;
    int r = n->intra ? -1 : n->ref[l][blk >> 2];
    *ref = r;
    if (r >= 0) { mv[0] = n->mv[l][blk][0]; mv[1] = n->mv[l][blk][1]; }
    else mv[0] = mv[1] = 0;
    return 1;
}

/* mvpLX for a partition at 4x4 (x, y), w x h 4x4s; shape 1 = 16x8, 2 = 8x16 (part = which half). */
static void mvp(h264 *d, mbinfo *cur, int l, int x, int y, int w, int h, int ref, int shape, int part, int16_t out[2]) {
    int first = blk_at[y][x];
    int ra = -1, rb = -1, rc = -1;
    int16_t a[2] = { 0, 0 }, b[2] = { 0, 0 }, c[2] = { 0, 0 };
    int aa = nb_mv(d, cur, l, x - 1, y, first, &ra, a);
    int ab = nb_mv(d, cur, l, x, y - 1, first, &rb, b);
    int ac = nb_mv(d, cur, l, x + w, y - 1, first, &rc, c);
    if (!ac) ac = nb_mv(d, cur, l, x - 1, y - 1, first, &rc, c);
    (void)h;
    if (shape == 1) {
        if (part == 0 && rb == ref) { out[0] = b[0]; out[1] = b[1]; return; }
        if (part == 1 && ra == ref) { out[0] = a[0]; out[1] = a[1]; return; }
    } else if (shape == 2) {
        if (part == 0 && ra == ref) { out[0] = a[0]; out[1] = a[1]; return; }
        if (part == 1 && rc == ref) { out[0] = c[0]; out[1] = c[1]; return; }
    }
    if (!ab && !ac && aa) { out[0] = a[0]; out[1] = a[1]; return; }
    int n = (ra == ref) + (rb == ref) + (rc == ref);
    if (n == 1) {
        if (ra == ref) { out[0] = a[0]; out[1] = a[1]; }
        else if (rb == ref) { out[0] = b[0]; out[1] = b[1]; }
        else { out[0] = c[0]; out[1] = c[1]; }
        return;
    }
    out[0] = (int16_t)median3(a[0], b[0], c[0]);
    out[1] = (int16_t)median3(a[1], b[1], c[1]);
}

static void set_part(mbinfo *m, int l, int x, int y, int w, int h, int ref, const int16_t mv[2]) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) {
            int b = blk_at[j][i];
            m->mv[l][b][0] = mv[0]; m->mv[l][b][1] = mv[1];
            m->ref[l][b >> 2] = (int8_t)ref;
        }
}

static void pskip_mv(h264 *d, mbinfo *m) {
    int ra = -1, rb = -1;
    int16_t a[2] = { 0, 0 }, b[2] = { 0, 0 }, mv[2] = { 0, 0 };
    int aa = nb_mv(d, m, 0, -1, 0, 0, &ra, a), ab = nb_mv(d, m, 0, 0, -1, 0, &rb, b);
    if (aa && ab && !(ra == 0 && !a[0] && !a[1]) && !(rb == 0 && !b[0] && !b[1])) mvp(d, m, 0, 0, 0, 4, 4, 0, 0, 0, mv);
    set_part(m, 0, 0, 0, 4, 4, 0, mv);
    for (int i = 0; i < 4; i++) m->ref[1][i] = -1;
}

static int min_pos(int a, int b) { return (a >= 0 && b >= 0) ? imin(a, b) : imax(a, b); }

/* Direct prediction for the 8x8s in `mask` (8.4.1.2). */
static void direct_mvs(h264 *d, mbinfo *m, int mask) {
    slice_t *sl = &d->sl;
    picture *col = sl->list[1][0];
    mbinfo *cm = &col->mb[d->mba];
    static const int corner[4] = { 0, 5, 10, 15 };
    if (sl->direct_spatial) {
        int ref[2]; int16_t mvl[2][2] = { { 0, 0 }, { 0, 0 } };
        for (int l = 0; l < 2; l++) {
            int ra = -1, rb = -1, rc = -1;
            int16_t t[2];
            nb_mv(d, m, l, -1, 0, 0, &ra, t);
            nb_mv(d, m, l, 0, -1, 0, &rb, t);
            if (!nb_mv(d, m, l, 4, -1, 0, &rc, t)) nb_mv(d, m, l, -1, -1, 0, &rc, t);
            ref[l] = min_pos(ra, min_pos(rb, rc));
        }
        int zero = ref[0] < 0 && ref[1] < 0;
        if (zero) ref[0] = ref[1] = 0;
        else for (int l = 0; l < 2; l++) if (ref[l] >= 0) mvp(d, m, l, 0, 0, 4, 4, ref[l], 0, 0, mvl[l]);
        for (int k = 0; k < 4; k++) {
            if (!(mask >> k & 1)) continue;
            for (int s = 0; s < 4; s++) {
                int b = k * 4 + s;
                int cb = d->s->direct_8x8 ? corner[k] : b;
                int colzero = 0;
                if (!zero && !cm->intra && col->ref == 1) {
                    int r = cm->ref[0][cb >> 2] >= 0 ? cm->ref[0][cb >> 2] : cm->ref[1][cb >> 2];
                    const int16_t *v = cm->ref[0][cb >> 2] >= 0 ? cm->mv[0][cb] : cm->mv[1][cb];
                    colzero = r == 0 && iabs(v[0]) <= 1 && iabs(v[1]) <= 1;
                }
                for (int l = 0; l < 2; l++) {
                    int16_t v[2] = { mvl[l][0], mvl[l][1] };
                    if (zero || ref[l] < 0 || (ref[l] == 0 && colzero)) { v[0] = v[1] = 0; }
                    m->mv[l][b][0] = v[0]; m->mv[l][b][1] = v[1];
                    m->ref[l][k] = (int8_t)ref[l];
                }
            }
        }
        return;
    }
    /* temporal */
    for (int k = 0; k < 4; k++) {
        if (!(mask >> k & 1)) continue;
        for (int s = 0; s < 4; s++) {
            int b = k * 4 + s;
            int cb = d->s->direct_8x8 ? corner[k] : b;
            int16_t mvc[2] = { 0, 0 };
            int r0 = 0;
            if (!cm->intra) {
                int lc = cm->ref[0][cb >> 2] >= 0 ? 0 : 1;
                mvc[0] = cm->mv[lc][cb][0]; mvc[1] = cm->mv[lc][cb][1];
                int slot = cm->refpic[lc][cb >> 2];
                for (int i = 0; i < sl->num_ref_idx[0]; i++) if (sl->slot[0][i] == slot) { r0 = i; break; }
            }
            picture *p0 = sl->list[0][r0], *p1 = sl->list[1][0];
            int16_t v0[2], v1[2];
            int tb = clip3(-128, 127, d->cur->poc - p0->poc), td = clip3(-128, 127, p1->poc - p0->poc);
            if (p0->ref == 2 || !td) { v0[0] = mvc[0]; v0[1] = mvc[1]; v1[0] = v1[1] = 0; }
            else {
                int tx = (16384 + iabs(td / 2)) / td;
                int dsf = clip3(-1024, 1023, (tb * tx + 32) >> 6);
                for (int c = 0; c < 2; c++) {
                    v0[c] = (int16_t)((dsf * mvc[c] + 128) >> 8);
                    v1[c] = (int16_t)(v0[c] - mvc[c]);
                }
            }
            m->mv[0][b][0] = v0[0]; m->mv[0][b][1] = v0[1];
            m->mv[1][b][0] = v1[0]; m->mv[1][b][1] = v1[1];
            m->ref[0][k] = (int8_t)r0; m->ref[1][k] = 0;
        }
    }
}

/* ---- the macroblock -------------------------------------------------------------------------------- */
typedef struct {
    int mbt;                       /* raw mb_type within its slice type          */
    int intra, i16, pcm, b_direct16;
    int part;                      /* 0 16x16, 1 16x8, 2 8x16, 3 8x8               */
    int pred[2];                   /* partitions' lists: 1 L0, 2 L1, 3 both        */
    int sub[4];
    int ref8x8_zero;               /* P_8x8ref0                                     */
    int i16mode, cbp, chroma;
    int8_t prev[16], rem[16];
    int8_t ref[2][4];
    int16_t mvd[2][16][2];
} mbs_t;

/* B mb_types 1..21: shape and each partition's lists (Table 7-14) */
static const uint8_t b_part[23][3] = {
    {0,0,0}, {0,1,0}, {0,2,0}, {0,3,0},
    {1,1,1}, {2,1,1}, {1,2,2}, {2,2,2}, {1,1,2}, {2,1,2}, {1,2,1}, {2,2,1},
    {1,1,3}, {2,1,3}, {1,2,3}, {2,2,3}, {1,3,1}, {2,3,1}, {1,3,2}, {2,3,2}, {1,3,3}, {2,3,3}, {3,0,0},
};
/* sub_mb_type: (B) lists, and the shape: 0 8x8, 1 8x4, 2 4x8, 3 4x4 */
static const uint8_t b_sub[13][2] = { {0,0}, {1,0}, {2,0}, {3,0}, {1,1}, {1,2}, {2,1}, {2,2}, {3,1}, {3,2}, {1,3}, {2,3}, {3,3} };

/* nC (9.2.1) for luma 4x4 block b (or chroma AC block: c 0/1, b 0..3). */
static int nc_luma(h264 *d, mbinfo *m, int b) {
    int x = blk_x[b], y = blk_y[b], ba, bb, n = 0, k = 0;
    mbinfo *a = nb4(d, m, x - 1, y, &ba), *t = nb4(d, m, x, y - 1, &bb);
    if (a) { n += a->nz[ba]; k++; }
    if (t) { n += t->nz[bb]; k++; }
    return k == 2 ? (n + 1) >> 1 : n;
}
static int nc_chroma(h264 *d, mbinfo *m, int c, int b) {
    int x = b & 1, y = b >> 1, n = 0, k = 0;
    mbinfo *a = x ? m : mb_at(d, d->mbx - 1, d->mby);
    mbinfo *t = y ? m : mb_at(d, d->mbx, d->mby - 1);
    if (a) { n += a->nz[16 + c * 4 + (y * 2 + (x ^ 1))]; k++; }
    if (t) { n += t->nz[16 + c * 4 + ((y ^ 1) * 2 + x)]; k++; }
    return k == 2 ? (n + 1) >> 1 : n;
}

/* One CAVLC block: levels into out[scan[start + i]]. Returns TotalCoeff. */
static int cavlc_block(h264 *d, int16_t *out, int nc, int start, int max, const uint8_t *scan) {
    bs_t *b = &d->bs;
    int sym = nc < 0 ? vlc(b, &v_cdc) : vlc(b, &v_ct[nc < 2 ? 0 : nc < 4 ? 1 : nc < 8 ? 2 : 3]);
    int total = sym >> 2, t1 = sym & 3;
    if (!total) return 0;
    if (total > max) { h264_fail(d, "CAVLC: too many coefficients"); return -1; }
    int level[16], run[16];
    int suffix = (total > 10 && t1 < 3) ? 1 : 0;
    for (int i = 0; i < total; i++) {
        if (i < t1) { level[i] = bs_u1(b) ? -1 : 1; continue; }
        int prefix = 0;
        while (!bs_u1(b)) { if (++prefix > 31) { h264_fail(d, "CAVLC: bad level"); return -1; } }
        int code = imin(15, prefix) << suffix;
        if (suffix > 0 || prefix >= 14) {
            int size = (prefix == 14 && suffix == 0) ? 4 : prefix >= 15 ? prefix - 3 : suffix;
            if (size > 0) code += (int)bs_u(b, size);
        }
        if (prefix >= 15 && suffix == 0) code += 15;
        if (prefix >= 16) code += (1 << (prefix - 3)) - 4096;
        if (i == t1 && t1 < 3) code += 2;
        level[i] = (code & 1) ? (-code - 1) >> 1 : (code + 2) >> 1;
        if (suffix == 0) suffix = 1;
        if (iabs(level[i]) > (3 << (suffix - 1)) && suffix < 6) suffix++;
    }
    int zeros = 0;
    if (total < max) zeros = max == 4 ? vlc(b, &v_ctz[total - 1]) : vlc(b, &v_tz[total - 1]);
    for (int i = 0; i < total - 1; i++) {
        run[i] = zeros > 0 ? vlc(b, &v_run[imin(zeros, 7) - 1]) : 0;
        zeros -= run[i];
    }
    run[total - 1] = zeros;
    if (zeros < 0) { h264_fail(d, "CAVLC: bad runs"); return -1; }
    int pos = -1;
    for (int i = total - 1; i >= 0; i--) {
        pos += run[i] + 1;
        if (start + pos >= 16) { h264_fail(d, "CAVLC: coefficient past the block"); return -1; }
        out[scan[start + pos]] = (int16_t)level[i];
    }
    return total;
}

/* ---- CABAC syntax elements --------------------------------------------------------------------------- */
static int cab_mb_skip(h264 *d) {
    mbinfo *a = mb_at(d, d->mbx - 1, d->mby), *b = mb_at(d, d->mbx, d->mby - 1);
    int inc = (a && !a->skip) + (b && !b->skip);
    return cab_decide(d, (d->sl.type == 1 ? 24 : 11) + inc);
}

static int cab_mb_type_i(h264 *d, int off, int in_i_slice) {
    int b0;
    if (in_i_slice) {
        mbinfo *a = mb_at(d, d->mbx - 1, d->mby), *b = mb_at(d, d->mbx, d->mby - 1);
        int inc = (a && a->type != MBT_I4) + (b && b->type != MBT_I4);
        b0 = cab_decide(d, off + inc);
    } else b0 = cab_decide(d, off);
    if (!b0) return 0;                                          /* I_NxN */
    if (cab_terminate(d)) return 25;                            /* I_PCM */
    int s = in_i_slice ? 3 : 1;                                 /* ctx of the bins after */
    int t = 1;
    t += 12 * cab_decide(d, off + s);
    if (cab_decide(d, off + s + 1)) t += 4 + 4 * cab_decide(d, off + s + (in_i_slice ? 2 : 1));
    t += 2 * cab_decide(d, off + (in_i_slice ? 6 : 3));       /* the prediction mode's two bins */
    t += cab_decide(d, off + (in_i_slice ? 7 : 3));
    return t;
}

static int cab_ref(h264 *d, mbinfo *m, int l, int x, int y) {
    int inc = 0;
    for (int k = 0; k < 2; k++) {
        int blk;
        mbinfo *n = nb4(d, m, k ? x : x - 1, k ? y - 1 : y, &blk);
        if (!n || n->skip || n->intra) continue;
        if (n->direct16 || (n->sub_direct >> (blk >> 2) & 1)) continue;
        if (n->ref[l][blk >> 2] > 0) inc += k ? 2 : 1;
    }
    int ref = 0;
    if (cab_decide(d, 54 + inc)) {
        ref = 1;
        int ctx = 54 + 4;
        while (cab_decide(d, ctx)) { ref++; ctx = 54 + 5; if (ref > 32) break; }
    }
    return ref;
}

static int cab_mvd(h264 *d, mbinfo *m, int l, int comp, int x, int y) {
    int sum = 0;
    for (int k = 0; k < 2; k++) {
        int blk;
        mbinfo *n = nb4(d, m, k ? x : x - 1, k ? y - 1 : y, &blk);
        if (n) sum += n->mvd[l][blk][comp];
    }
    int base = comp ? 47 : 40;
    int inc = sum < 3 ? 0 : sum > 32 ? 2 : 1;
    if (!cab_decide(d, base + inc)) return 0;
    int v = 1, ctx = base + 3;
    while (v < 9 && cab_decide(d, ctx)) { v++; if (ctx < base + 6) ctx++; }
    if (v >= 9) {
        int k = 3;
        while (cab_bypass(d)) { v += 1 << k; k++; if (k > 24) break; }
        while (k--) v += cab_bypass(d) << k;
    }
    return cab_bypass(d) ? -v : v;
}

static int cab_cbp(h264 *d, mbinfo *m) {
    int cbp = 0;
    mbinfo *ma = mb_at(d, d->mbx - 1, d->mby), *mb = mb_at(d, d->mbx, d->mby - 1);
    for (int k = 0; k < 4; k++) {
        int x = (k & 1) * 2, y = (k >> 1) * 2, ca, cb;
        /* left */
        if (x) ca = !(cbp >> (k - 1) & 1);
        else ca = ma ? (ma->pcm ? 0 : !(ma->cbp >> (k + 1) & 1)) : 0;
        if (y) cb = !(cbp >> (k - 2) & 1);
        else cb = mb ? (mb->pcm ? 0 : !(mb->cbp >> (k + 2) & 1)) : 0;
        if (cab_decide(d, 73 + ca + 2 * cb)) cbp |= 1 << k;
    }
    (void)m;
    int ca = ma && (ma->pcm || (ma->cbp >> 4) != 0), cb = mb && (mb->pcm || (mb->cbp >> 4) != 0);
    if (cab_decide(d, 77 + ca + 2 * cb)) {
        ca = ma && (ma->pcm || (ma->cbp >> 4) == 2); cb = mb && (mb->pcm || (mb->cbp >> 4) == 2);
        cbp |= (1 + cab_decide(d, 77 + 4 + ca + 2 * cb)) << 4;
    }
    return cbp;
}

static int cab_qp_delta(h264 *d) {
    if (!cab_decide(d, 60 + d->last_qp_delta_nz)) return 0;
    int k = 1, ctx = 62;
    while (cab_decide(d, ctx)) { k++; ctx = 63; if (k > 104) break; }
    return (k & 1) ? (k + 1) >> 1 : -(k >> 1);
}

/* coded_block_flag's neighbour term for a block of category cat (0 Y DC, 1/2 Y 4x4, 3 C DC, 4 C AC). */
static int cbf_term(h264 *d, mbinfo *cur, mbinfo *n, int bit, int cur_intra) {
    (void)d;
    if (!n) return cur_intra;
    if (n->pcm) return 1;
    if (n == cur) return (cur->cbf >> bit) & 1;
    if (n->skip) return 0;
    return (n->cbf >> bit) & 1;
}

static const uint8_t cbf_off[5] = { 0, 4, 8, 12, 16 }, sig_off[5] = { 0, 15, 29, 44, 47 }, abs_off[5] = { 0, 10, 20, 30, 39 };

/* One CABAC block (9.3.2.3 and 7.3.5.3.3): levels into out[scan[start + i]]. cbf_inc
 * is the coded_block_flag's context increment. Returns the coefficients' count. */
static int cabac_block(h264 *d, int16_t *out, int cat, int cbf_inc, int start, int max, const uint8_t *scan) {
    if (!cab_decide(d, 85 + cbf_off[cat] + cbf_inc)) return 0;
    int sig[16], n = 0;
    int i;
    for (i = 0; i < max - 1; i++) {
        int inc = cat == 3 ? imin(i, 2) : i;
        if (cab_decide(d, 105 + sig_off[cat] + inc)) {
            sig[n++] = i;
            if (cab_decide(d, 166 + sig_off[cat] + inc)) break;
        }
    }
    if (i == max - 1) sig[n++] = max - 1;
    int eq1 = 0, gt1 = 0;
    for (int k = n - 1; k >= 0; k--) {
        int inc = gt1 ? 0 : imin(4, 1 + eq1);
        int abs;
        if (!cab_decide(d, 227 + abs_off[cat] + inc)) abs = 1;
        else {
            int ctx = 227 + abs_off[cat] + 5 + imin(4 - (cat == 3), gt1);
            int p = 1;
            while (p < 14 && cab_decide(d, ctx)) p++;
            abs = p + 1;
            if (p >= 14) {
                int kk = 0, suf = 0;
                while (cab_bypass(d)) { suf += 1 << kk; kk++; if (kk > 24) break; }
                while (kk--) suf += cab_bypass(d) << kk;
                abs += suf;
            }
        }
        if (abs == 1) eq1++; else gt1++;
        out[scan[start + sig[k]]] = (int16_t)(cab_bypass(d) ? -abs : abs);
    }
    return n;
}

static const uint8_t ident4[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

/* ---- residual, both entropy codes; dequantised as it lands (8.5) ------------------------------------- */
static void dequant4(int16_t *c, int qp, int from) {
    const uint8_t *v = dq[qp % 6];
    int sh = qp / 6;
    for (int i = from; i < 16; i++)
        if (c[i]) c[i] = (int16_t)((c[i] * v[((i & 1) + ((i >> 2) & 1))]) << sh);
}

static int residual(h264 *d, mbinfo *m, int cbp, int i16, int intra) {
    int qp = m->qp, cabac = d->pp->cabac;
    int16_t *Y = d->coef, *C = d->coef + 256;
    memset(d->coef, 0, sizeof d->coef);
    memset(m->nz, 0, sizeof m->nz);
    m->cbf = 0;
    int16_t dc[16];
    /* Luma */
    if (i16) {
        memset(dc, 0, sizeof dc);
        int n;
        if (cabac) {
            mbinfo *a = mb_at(d, d->mbx - 1, d->mby), *b = mb_at(d, d->mbx, d->mby - 1);
            int inc = cbf_term(d, m, a, 24, 1) + 2 * cbf_term(d, m, b, 24, 1);
            n = cabac_block(d, dc, 0, inc, 0, 16, zz);
        } else n = cavlc_block(d, dc, nc_luma(d, m, 0), 0, 16, zz);
        if (n < 0) return -1;
        if (n) m->cbf |= 1u << 24;
        /* inverse Hadamard, then the scale (8.5.10) */
        /* rows of the Hadamard matrix: ++++, ++--, +--+, +-+- */
        int t[16];
        for (int i = 0; i < 4; i++) {
            int a0 = dc[i * 4] + dc[i * 4 + 1], a1 = dc[i * 4] - dc[i * 4 + 1], a2 = dc[i * 4 + 2] + dc[i * 4 + 3], a3 = dc[i * 4 + 2] - dc[i * 4 + 3];
            t[i * 4] = a0 + a2; t[i * 4 + 1] = a0 - a2; t[i * 4 + 2] = a1 - a3; t[i * 4 + 3] = a1 + a3;
        }
        int v0 = dq[qp % 6][0], q6 = qp / 6;
        for (int j = 0; j < 4; j++) {
            int a0 = t[j] + t[4 + j], a1 = t[j] - t[4 + j], a2 = t[8 + j] + t[12 + j], a3 = t[8 + j] - t[12 + j];
            int f[4] = { a0 + a2, a0 - a2, a1 - a3, a1 + a3 };
            for (int i = 0; i < 4; i++) {
                int v = f[i] * v0;
                v = q6 >= 2 ? v << (q6 - 2) : (v + (1 << (1 - q6))) >> (2 - q6);
                d->dc_y[i * 4 + j] = (int16_t)v;                   /* row i, column j */
            }
        }
    }
    for (int b8 = 0; b8 < 4; b8++) {
        if (!(cbp >> b8 & 1)) continue;
        for (int s = 0; s < 4; s++) {
            int b = b8 * 4 + s, n;
            int16_t *blk = Y + b * 16;
            if (cabac) {
                int x = blk_x[b], y = blk_y[b], ba, bb;
                mbinfo *na = nb4(d, m, x - 1, y, &ba), *nb = nb4(d, m, x, y - 1, &bb);
                int inc = cbf_term(d, m, na, ba, intra) + 2 * cbf_term(d, m, nb, bb, intra);
                n = cabac_block(d, blk, i16 ? 1 : 2, inc, i16 ? 1 : 0, i16 ? 15 : 16, zz);
            } else n = cavlc_block(d, blk, nc_luma(d, m, b), i16 ? 1 : 0, i16 ? 15 : 16, zz);
            if (n < 0) return -1;
            m->nz[b] = (uint8_t)n;
            if (n) m->cbf |= 1u << b;
            dequant4(blk, qp, i16 ? 1 : 0);
        }
    }
    if (i16) for (int b = 0; b < 16; b++) Y[b * 16] = d->dc_y[blk_y[b] * 4 + blk_x[b]];
    /* Chroma */
    int cc = cbp >> 4;
    if (cc) {
        for (int c = 0; c < 2; c++) {
            int16_t cdc[4] = { 0, 0, 0, 0 };
            int n;
            if (cabac) {
                mbinfo *a = mb_at(d, d->mbx - 1, d->mby), *b = mb_at(d, d->mbx, d->mby - 1);
                int inc = cbf_term(d, m, a, 25 + c, intra) + 2 * cbf_term(d, m, b, 25 + c, intra);
                n = cabac_block(d, cdc, 3, inc, 0, 4, ident4);
            } else n = cavlc_block(d, cdc, -1, 0, 4, ident4);
            if (n < 0) return -1;
            if (n) m->cbf |= 1u << (25 + c);
            int qpc = m->qpc[c];
            int f0 = cdc[0] + cdc[1] + cdc[2] + cdc[3], f1 = cdc[0] - cdc[1] + cdc[2] - cdc[3];
            int f2 = cdc[0] + cdc[1] - cdc[2] - cdc[3], f3 = cdc[0] - cdc[1] - cdc[2] + cdc[3];
            int v0 = dq[qpc % 6][0], sh = qpc / 6;
            d->dc_c[c][0] = (int16_t)(((f0 * v0) << sh) >> 1);
            d->dc_c[c][1] = (int16_t)(((f1 * v0) << sh) >> 1);
            d->dc_c[c][2] = (int16_t)(((f2 * v0) << sh) >> 1);
            d->dc_c[c][3] = (int16_t)(((f3 * v0) << sh) >> 1);
        }
        for (int c = 0; c < 2; c++) {
            for (int b = 0; b < 4; b++) {
                int16_t *blk = C + (c * 4 + b) * 16;
                if (cc & 2) {
                    int n;
                    if (cabac) {
                        int x = b & 1, y = b >> 1;
                        mbinfo *na = x ? m : mb_at(d, d->mbx - 1, d->mby), *nb = y ? m : mb_at(d, d->mbx, d->mby - 1);
                        int bita = 16 + c * 4 + (y * 2 + (x ^ 1)), bitb = 16 + c * 4 + ((y ^ 1) * 2 + x);
                        int ta = (na && na != m && (na->cbp >> 4) != 2 && !na->pcm) ? 0 : cbf_term(d, m, na, bita, intra);
                        int tb = (nb && nb != m && (nb->cbp >> 4) != 2 && !nb->pcm) ? 0 : cbf_term(d, m, nb, bitb, intra);
                        n = cabac_block(d, blk, 4, ta + 2 * tb, 1, 15, zz);
                    } else n = cavlc_block(d, blk, nc_chroma(d, m, c, b), 1, 15, zz);
                    if (n < 0) return -1;
                    m->nz[16 + c * 4 + b] = (uint8_t)n;
                    if (n) m->cbf |= 1u << (16 + c * 4 + b);
                    dequant4(blk, m->qpc[c], 1);
                }
                blk[0] = d->dc_c[c][b];
            }
        }
    }
    return 0;
}

/* ---- reconstruction -------------------------------------------------------------------------------- */
static void recon_luma(h264 *d, mbinfo *m) {
    picture *p = d->cur;
    int ys = p->ystride;
    uint8_t *Y = p->y + d->mby * 16 * ys + d->mbx * 16;
    for (int b = 0; b < 16; b++) {
        int16_t *c = d->coef + b * 16;
        uint8_t *dst = Y + blk_y[b] * 4 * ys + blk_x[b] * 4;
        if (m->nz[b]) h264_idct4_add(dst, ys, c);
        else if (m->i16 && c[0]) h264_dc_add(dst, ys, c[0]);
    }
}

static void recon_chroma(h264 *d, mbinfo *m) {
    picture *p = d->cur;
    int cs = p->cstride;
    for (int c = 0; c < 2; c++) {
        uint8_t *base = (c ? p->v : p->u) + d->mby * 8 * cs + d->mbx * 8;
        for (int b = 0; b < 4; b++) {
            int16_t *co = d->coef + 256 + (c * 4 + b) * 16;
            uint8_t *dst = base + (b >> 1) * 4 * cs + (b & 1) * 4;
            if (m->nz[16 + c * 4 + b]) h264_idct4_add(dst, cs, co);
            else if (co[0]) h264_dc_add(dst, cs, co[0]);
        }
    }
}

static void set_qp(h264 *d, mbinfo *m, int qp) {
    d->qp = qp;
    m->qp = (int8_t)qp;
    m->qpc[0] = (int8_t)qpc_tab[clip3(0, 51, qp + d->pp->chroma_qp_offset)];
    m->qpc[1] = (int8_t)qpc_tab[clip3(0, 51, qp + d->pp->second_chroma_qp_offset)];
}

static int pred_intra4_mode(h264 *d, mbinfo *m, int b) {
    int x = blk_x[b], y = blk_y[b], ba, bb;
    mbinfo *a = nb4(d, m, x - 1, y, &ba), *t = nb4(d, m, x, y - 1, &bb);
    int ci = d->pp->constrained_intra;
    if (!a || !t || (ci && a != m && !a->intra) || (ci && t != m && !t->intra)) return 2;
    int ma = a->type == MBT_I4 ? a->i4[ba] : 2, mt = t->type == MBT_I4 ? t->i4[bb] : 2;
    return imin(ma, mt);
}

/* Read one macroblock's syntax and decode it. */
static int macroblock(h264 *d, int skipped) {
    slice_t *sl = &d->sl;
    mbinfo *m = &d->cur->mb[d->mba];
    int cabac = d->pp->cabac;
    memset(m, 0, sizeof *m);
    m->slice = (int16_t)d->slice_num;
    m->dbk = (int8_t)sl->disable_deblock; m->aoff = (int8_t)sl->alpha_off; m->boff = (int8_t)sl->beta_off;
    for (int l = 0; l < 2; l++) for (int k = 0; k < 4; k++) { m->ref[l][k] = -1; m->refpic[l][k] = -1; }
    for (int k = 0; k < 16; k++) m->i4[k] = -1;
    if (skipped) {
        m->skip = 1;
        m->type = sl->type == 1 ? MBT_BSKIP : MBT_PSKIP;
        set_qp(d, m, d->qp);
        if (sl->type == 1) { m->direct16 = 1; direct_mvs(d, m, 15); }
        else pskip_mv(d, m);
        for (int l = 0; l < 2; l++) for (int k = 0; k < 4; k++) if (m->ref[l][k] >= 0) m->refpic[l][k] = sl->slot[l][m->ref[l][k]];
        h264_inter(d, m);
        d->last_qp_delta_nz = 0;
        return 0;
    }
    mbs_t s;
    memset(&s, 0, sizeof s);
    /* mb_type */
    int t;
    if (cabac) {
        if (sl->type == 2) t = cab_mb_type_i(d, 3, 1);
        else if (sl->type == 0) {
            if (cab_decide(d, 14)) t = 5 + cab_mb_type_i(d, 17, 0);
            else if (!cab_decide(d, 15)) t = 3 * cab_decide(d, 16);
            else t = 2 - cab_decide(d, 17);
        } else {
            mbinfo *a = mb_at(d, d->mbx - 1, d->mby), *b = mb_at(d, d->mbx, d->mby - 1);
            int inc = (a && !a->skip && !a->direct16) + (b && !b->skip && !b->direct16);
            if (!cab_decide(d, 27 + inc)) t = 0;
            else if (!cab_decide(d, 27 + 3)) t = 1 + cab_decide(d, 27 + 5);
            else {
                int bits = cab_decide(d, 27 + 4) << 3;
                bits |= cab_decide(d, 27 + 5) << 2;
                bits |= cab_decide(d, 27 + 5) << 1;
                bits |= cab_decide(d, 27 + 5);
                if (bits < 8) t = bits + 3;
                else if (bits == 13) t = 23 + cab_mb_type_i(d, 32, 0);
                else if (bits == 14) t = 11;
                else if (bits == 15) t = 22;
                else t = ((bits << 1) | cab_decide(d, 27 + 5)) - 4;
            }
        }
    } else t = (int)bs_ue(&d->bs);
    /* to an intra type, or the partition layout */
    int it = -1;
    if (sl->type == 2) it = t;
    else if (sl->type == 0 && t >= 5) it = t - 5;
    else if (sl->type == 1 && t >= 23) it = t - 23;
    if (it > 25 || (sl->type == 0 && t > 30) || (sl->type == 1 && t > 48)) { h264_fail(d, "bad mb_type"); return -1; }
    if (it >= 0) {
        s.intra = 1;
        if (it == 25) s.pcm = 1;
        else if (it > 0) { s.i16 = 1; s.i16mode = (it - 1) % 4; s.cbp = ((it - 1) / 4 % 3) << 4 | (it >= 13 ? 15 : 0); }
    } else if (sl->type == 0) {
        s.part = t == 0 ? 0 : t == 1 ? 1 : t == 2 ? 2 : 3;
        s.pred[0] = s.pred[1] = 1;
        s.ref8x8_zero = t == 4;
    } else {
        if (t == 0) s.b_direct16 = 1;
        else { s.part = b_part[t][0]; s.pred[0] = b_part[t][1]; s.pred[1] = b_part[t][2]; }
    }
    m->intra = (uint8_t)s.intra; m->pcm = (uint8_t)s.pcm; m->i16 = (uint8_t)s.i16;
    m->type = s.intra ? (s.pcm ? MBT_PCM : s.i16 ? MBT_I16 : MBT_I4) : (sl->type == 1 ? MBT_B : MBT_P);
    m->direct16 = (uint8_t)s.b_direct16;

    if (s.pcm) {
        picture *p = d->cur;
        int pos = cabac ? d->bs.pos : d->bs.pos;
        pos = (pos + 7) & ~7;
        const uint8_t *q = d->bs.p + (pos >> 3);
        for (int y = 0; y < 16; y++) memcpy(p->y + (d->mby * 16 + y) * p->ystride + d->mbx * 16, q + y * 16, 16);
        q += 256;
        for (int c = 0; c < 2; c++)
            for (int y = 0; y < 8; y++) memcpy((c ? p->v : p->u) + (d->mby * 8 + y) * p->cstride + d->mbx * 8, q + c * 64 + y * 8, 8);
        d->bs.pos = pos + 384 * 8;
        if (cabac) { d->cab.range = 510; d->cab.offset = bs_u(&d->bs, 9); }
        set_qp(d, m, d->qp);
        m->qp = 0; m->qpc[0] = m->qpc[1] = 0;
        for (int i = 0; i < 24; i++) m->nz[i] = 16;
        m->cbf = 0x7FFFFFF; m->cbp = 0x2F;
        d->last_qp_delta_nz = 0;
        return 0;
    }

    /* prediction syntax */
    if (s.intra) {
        if (!s.i16) {
            for (int b = 0; b < 16; b++) {
                if (cabac) {
                    s.prev[b] = (int8_t)cab_decide(d, 68);
                    if (!s.prev[b]) s.rem[b] = (int8_t)(cab_decide(d, 69) | cab_decide(d, 69) << 1 | cab_decide(d, 69) << 2);
                } else {
                    s.prev[b] = (int8_t)bs_u1(&d->bs);
                    if (!s.prev[b]) s.rem[b] = (int8_t)bs_u(&d->bs, 3);
                }
            }
        }
        if (cabac) {
            mbinfo *a = mb_at(d, d->mbx - 1, d->mby), *b = mb_at(d, d->mbx, d->mby - 1);
            int inc = (a && a->intra && !a->pcm && a->chroma_mode) + (b && b->intra && !b->pcm && b->chroma_mode);
            int c = 0;
            if (cab_decide(d, 64 + inc)) { c = 1; if (cab_decide(d, 67)) { c = 2; if (cab_decide(d, 67)) c = 3; } }
            s.chroma = c;
        } else s.chroma = (int)bs_ue(&d->bs);
        if (s.chroma > 3) { h264_fail(d, "bad chroma mode"); return -1; }
        m->chroma_mode = (uint8_t)s.chroma;
    } else if (s.part == 3) {
        for (int k = 0; k < 4; k++) {
            int st;
            if (cabac) {
                if (sl->type == 0) st = cab_decide(d, 21) ? 0 : !cab_decide(d, 22) ? 1 : cab_decide(d, 23) ? 2 : 3;
                else {
                    if (!cab_decide(d, 36)) st = 0;
                    else if (!cab_decide(d, 37)) st = 1 + cab_decide(d, 39);
                    else {
                        st = 3;
                        if (cab_decide(d, 38)) { if (cab_decide(d, 39)) st = 11 + cab_decide(d, 39); else st += 4; }
                        if (st < 11) { st += 2 * cab_decide(d, 39); st += cab_decide(d, 39); }
                    }
                }
            } else st = (int)bs_ue(&d->bs);
            if ((sl->type == 0 && st > 3) || st > 12) { h264_fail(d, "bad sub_mb_type"); return -1; }
            s.sub[k] = st;
            if (sl->type == 1 && st == 0) m->sub_direct |= (uint8_t)(1 << k);
        }
        for (int l = 0; l < 2; l++)
            for (int k = 0; k < 4; k++) {
                int uses = sl->type == 0 ? (l == 0) : (s.sub[k] && (b_sub[s.sub[k]][0] >> l & 1));
                s.ref[l][k] = uses ? 0 : -1;
                if (!uses || sl->num_ref_idx[l] <= 1 || s.ref8x8_zero) continue;
                if (cabac) s.ref[l][k] = (int8_t)cab_ref(d, m, l, (k & 1) * 2, (k >> 1) * 2);
                else s.ref[l][k] = (int8_t)(sl->num_ref_idx[l] == 2 ? !bs_u1(&d->bs) : bs_ue(&d->bs));
                if (s.ref[l][k] >= sl->num_ref_idx[l]) { h264_fail(d, "bad ref_idx"); return -1; }
                m->ref[l][k] = s.ref[l][k];                     /* CABAC's next ref_idx context reads it */
            }
        for (int k = 0; k < 4; k++) for (int l = 0; l < 2; l++) m->ref[l][k] = -1;
        for (int l = 0; l < 2; l++)
            for (int k = 0; k < 4; k++) {
                if (s.ref[l][k] < 0) continue;
                int shape = sl->type == 0 ? s.sub[k] : b_sub[s.sub[k]][1];
                int w = shape == 0 || shape == 1 ? 2 : 1, h = shape == 0 || shape == 2 ? 2 : 1;
                for (int j = 0; j < 2; j += h)
                    for (int i = 0; i < 2; i += w) {
                        int x = (k & 1) * 2 + i, y = (k >> 1) * 2 + j;
                        int mx, my;
                        if (cabac) { mx = cab_mvd(d, m, l, 0, x, y); my = cab_mvd(d, m, l, 1, x, y); }
                        else { mx = bs_se(&d->bs); my = bs_se(&d->bs); }
                        for (int yy = y; yy < y + h; yy++)
                            for (int xx = x; xx < x + w; xx++) {
                                int b = blk_at[yy][xx];
                                s.mvd[l][b][0] = (int16_t)mx; s.mvd[l][b][1] = (int16_t)my;
                                m->mvd[l][b][0] = (uint8_t)imin(iabs(mx), 255); m->mvd[l][b][1] = (uint8_t)imin(iabs(my), 255);
                            }
                    }
            }
    } else if (!s.b_direct16) {
        int np = s.part == 0 ? 1 : 2;
        for (int l = 0; l < 2; l++)
            for (int p = 0; p < np; p++) {
                s.ref[l][p] = (s.pred[p] >> l & 1) ? 0 : -1;
                if (s.ref[l][p] < 0 || sl->num_ref_idx[l] <= 1) continue;
                int x = s.part == 2 ? p * 2 : 0, y = s.part == 1 ? p * 2 : 0;
                if (cabac) s.ref[l][p] = (int8_t)cab_ref(d, m, l, x, y);
                else s.ref[l][p] = (int8_t)(sl->num_ref_idx[l] == 2 ? !bs_u1(&d->bs) : bs_ue(&d->bs));
                if (s.ref[l][p] >= sl->num_ref_idx[l]) { h264_fail(d, "bad ref_idx"); return -1; }
                /* CABAC reads the earlier partition's ref_idx for the next one's context */
                int w = s.part == 2 ? 2 : 4, h = s.part == 1 ? 2 : 4;
                for (int yy = y; yy < y + h; yy += 2) for (int xx = x; xx < x + w; xx += 2) m->ref[l][blk_at[yy][xx] >> 2] = s.ref[l][p];
            }
        for (int k = 0; k < 4; k++) for (int l = 0; l < 2; l++) m->ref[l][k] = -1;
        for (int l = 0; l < 2; l++)
            for (int p = 0; p < np; p++) {
                if (s.ref[l][p] < 0) continue;
                int x = s.part == 2 ? p * 2 : 0, y = s.part == 1 ? p * 2 : 0;
                int w = s.part == 2 ? 2 : 4, h = s.part == 1 ? 2 : 4;
                int mx, my;
                if (cabac) { mx = cab_mvd(d, m, l, 0, x, y); my = cab_mvd(d, m, l, 1, x, y); }
                else { mx = bs_se(&d->bs); my = bs_se(&d->bs); }
                for (int yy = y; yy < y + h; yy++)
                    for (int xx = x; xx < x + w; xx++) {
                        int b = blk_at[yy][xx];
                        s.mvd[l][b][0] = (int16_t)mx; s.mvd[l][b][1] = (int16_t)my;
                        m->mvd[l][b][0] = (uint8_t)imin(iabs(mx), 255); m->mvd[l][b][1] = (uint8_t)imin(iabs(my), 255);
                    }
            }
    }

    /* coded_block_pattern, mb_qp_delta */
    if (!s.i16) {
        if (cabac) s.cbp = cab_cbp(d, m);
        else {
            int k = (int)bs_ue(&d->bs);
            if (k > 47) { h264_fail(d, "bad coded_block_pattern"); return -1; }
            s.cbp = s.intra ? cbp_intra[k] : cbp_inter[k];
            s.cbp = (s.cbp & 15) | (s.cbp >> 4) << 4;
        }
    }
    m->cbp = (uint8_t)s.cbp;
    int qp = d->qp;
    if ((s.cbp & 15) || (s.cbp >> 4) || s.i16) {
        int dqp = cabac ? cab_qp_delta(d) : bs_se(&d->bs);   /* its context reads the last MB's */
        if (dqp < -26 || dqp > 25) { h264_fail(d, "bad mb_qp_delta"); return -1; }
        qp = (qp + dqp + 52) % 52;
        m->qp_delta_nz = dqp != 0;
        d->last_qp_delta_nz = dqp != 0;
    } else d->last_qp_delta_nz = 0;
    set_qp(d, m, qp);

    /* motion vectors, now that every mvd is known */
    if (!s.intra) {
        if (s.b_direct16) direct_mvs(d, m, 15);
        else if (s.part == 3) {
            if (sl->type == 1 && m->sub_direct) direct_mvs(d, m, m->sub_direct);
            for (int k = 0; k < 4; k++) {
                if (sl->type == 1 && !s.sub[k]) continue;
                int shape = sl->type == 0 ? s.sub[k] : b_sub[s.sub[k]][1];
                int w = shape == 0 || shape == 1 ? 2 : 1, h = shape == 0 || shape == 2 ? 2 : 1;
                for (int l = 0; l < 2; l++) {
                    if (s.ref[l][k] < 0) { if (sl->type == 1) m->ref[l][k] = -1; continue; }
                    for (int j = 0; j < 2; j += h)
                        for (int i = 0; i < 2; i += w) {
                            int x = (k & 1) * 2 + i, y = (k >> 1) * 2 + j;
                            int16_t p[2];
                            m->ref[l][k] = s.ref[l][k];
                            mvp(d, m, l, x, y, w, h, s.ref[l][k], 0, 0, p);
                            int b0 = blk_at[y][x];
                            int16_t mv[2] = { (int16_t)(p[0] + s.mvd[l][b0][0]), (int16_t)(p[1] + s.mvd[l][b0][1]) };
                            set_part(m, l, x, y, w, h, s.ref[l][k], mv);
                        }
                }
            }
        } else {
            int np = s.part == 0 ? 1 : 2;
            for (int p = 0; p < np; p++) {
                int x = s.part == 2 ? p * 2 : 0, y = s.part == 1 ? p * 2 : 0;
                int w = s.part == 2 ? 2 : 4, h = s.part == 1 ? 2 : 4;
                for (int l = 0; l < 2; l++) {
                    if (s.ref[l][p] < 0) continue;
                    int16_t pr[2];
                    mvp(d, m, l, x, y, w, h, s.ref[l][p], s.part, p, pr);
                    int b0 = blk_at[y][x];
                    int16_t mv[2] = { (int16_t)(pr[0] + s.mvd[l][b0][0]), (int16_t)(pr[1] + s.mvd[l][b0][1]) };
                    set_part(m, l, x, y, w, h, s.ref[l][p], mv);
                }
            }
        }
        for (int l = 0; l < 2; l++) for (int k = 0; k < 4; k++) if (m->ref[l][k] >= 0) m->refpic[l][k] = sl->slot[l][m->ref[l][k]];
    }

    /* the residual */
    if (residual(d, m, s.cbp, s.i16, s.intra) < 0) return -1;

    /* prediction, and the residual on top of it */
    picture *pc = d->cur;
    int ys = pc->ystride;
    uint8_t *Y = pc->y + d->mby * 16 * ys + d->mbx * 16;
    if (s.intra) {
        if (s.i16) {
            h264_intra16(d, m, s.i16mode, Y, ys);
            recon_luma(d, m);
        } else {
            for (int b = 0; b < 16; b++) {
                int pred = pred_intra4_mode(d, m, b);
                int mode = s.prev[b] ? pred : (s.rem[b] < pred ? s.rem[b] : s.rem[b] + 1);
                m->i4[b] = (int8_t)mode;
                uint8_t *dst = Y + blk_y[b] * 4 * ys + blk_x[b] * 4;
                h264_intra4x4(d, m, b, mode, dst, ys);
                if (m->nz[b]) h264_idct4_add(dst, ys, d->coef + b * 16);
            }
        }
        int cs = pc->cstride;
        h264_intra_chroma(d, m, s.chroma, pc->u + d->mby * 8 * cs + d->mbx * 8, pc->v + d->mby * 8 * cs + d->mbx * 8, cs);
        recon_chroma(d, m);
    } else {
        h264_inter(d, m);
        recon_luma(d, m);
        recon_chroma(d, m);
    }
    return 0;
}

/* ---- the slice's macroblocks ------------------------------------------------------------------------ */
int h264_debug;                         /* tests: a line per macroblock of the first pictures */
void h264_trace(const char *fmt, ...);
int h264_slice_data(h264 *d) {
    slice_t *sl = &d->sl;
    vlc_init();
    d->qp = sl->qp;
    d->last_qp_delta_nz = 0;
    int cabac = d->pp->cabac;
    if (cabac) { cabac_init_ctx(d); cab_start(d); }
    d->mba = sl->first_mb;
    int total = d->mb_w * d->mb_h;
#define AT_MB() (d->mbx = d->mba % d->mb_w, d->mby = d->mba / d->mb_w, d->cur->mb[d->mba].slice = (int16_t)d->slice_num)
    if (cabac) {
        while (d->mba < total) {
            AT_MB();
            int skipped = sl->type != 2 && cab_mb_skip(d);
            if (macroblock(d, skipped) < 0) return -1;
            if (h264_debug && d->frames < h264_debug) {
                mbinfo *m = &d->cur->mb[d->mba];
                h264_trace("f%d mb %d (%d,%d) type %d skip %d qp %d cbp %02x i4 %d %d %d %d chroma %d bits %d\n", d->frames, d->mba, d->mbx, d->mby,
                           m->type, m->skip, m->qp, m->cbp, m->i4[0], m->i4[1], m->i4[2], m->i4[3], m->chroma_mode, d->bs.pos);
            }
            d->mba++;
            if (cab_terminate(d)) break;                       /* end_of_slice_flag */
        }
    } else {
        while (d->mba < total) {
            if (sl->type != 2) {                               /* mb_skip_run, then a coded MB if any */
                int run = (int)bs_ue(&d->bs);
                while (run-- > 0 && d->mba < total) {
                    AT_MB();
                    if (macroblock(d, 1) < 0) return -1;
                    d->mba++;
                }
                if (d->mba >= total || bs_left(&d->bs) <= 0) break;
            }
            AT_MB();
            if (macroblock(d, 0) < 0) return -1;
            d->mba++;
            if (bs_left(&d->bs) <= 0) break;
        }
    }
#undef AT_MB
    d->slice_num++;
    return 0;
}
