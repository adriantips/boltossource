#pragma once
#include <stdint.h>
#include "h264.h"

/* BoltOS Mini's H.264 decoder: what its four files share (h264.c the stream,
 * parameter sets, slices and references; h264mb.c macroblocks, CAVLC and
 * CABAC; h264pred.c prediction and the transforms; h264dbk.c the loop
 * filter). Section numbers are ITU-T H.264's. */

/* ---- bits -------------------------------------------------------------------- */
typedef struct { const uint8_t *p; int bits; int pos; } bs_t;

static inline uint32_t bs_peek32(const bs_t *b) {
    const uint8_t *q = b->p + (b->pos >> 3);
    uint64_t v = ((uint64_t)q[0] << 32) | ((uint64_t)q[1] << 24) | ((uint64_t)q[2] << 16) | ((uint64_t)q[3] << 8) | q[4];
    return (uint32_t)(v >> (8 - (b->pos & 7)));
}
static inline uint32_t bs_u(bs_t *b, int n) {
    if (!n) return 0;
    uint32_t v = bs_peek32(b) >> (32 - n);
    b->pos += n;
    return v;
}
static inline int bs_u1(bs_t *b) { int v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1; b->pos++; return v; }
static inline uint32_t bs_ue(bs_t *b) {
    uint32_t v = bs_peek32(b);
    int lz = v ? __builtin_clz(v) : 32;
    if (lz > 15) {                                  /* long codes: in two reads   */
        if (lz > 31) { b->pos = b->bits; return 0; }  /* not a code: the stream is bad */
        b->pos += lz;
        return bs_u(b, lz + 1) - 1;
    }
    b->pos += 2 * lz + 1;
    return (v >> (31 - 2 * lz)) - 1;
}
static inline int32_t bs_se(bs_t *b) {
    uint32_t k = bs_ue(b);
    return (k & 1) ? (int32_t)((k + 1) >> 1) : -(int32_t)(k >> 1);
}
static inline int bs_left(const bs_t *b) { return b->bits - b->pos; }

/* ---- parameter sets ------------------------------------------------------------- */
typedef struct {
    int valid, profile, level, id;
    int chroma_format;                  /* 1 = 4:2:0                               */
    int log2_max_frame_num, poc_type, log2_max_poc_lsb;
    int delta_pic_order_always_zero, offset_for_non_ref_pic, offset_for_top_to_bottom;
    int num_ref_frames_in_poc_cycle, offset_for_ref_frame[256];
    int max_num_ref_frames, gaps_allowed;
    int mb_w, mb_h, frame_mbs_only, direct_8x8;
    int crop_l, crop_r, crop_t, crop_b;
    int reorder;                        /* max_num_reorder_frames, or a guess       */
} sps_t;

typedef struct {
    int valid, id, sps_id, cabac, bottom_field_poc;
    int num_ref_idx[2], weighted_pred, weighted_bipred;
    int init_qp, chroma_qp_offset, deblock_ctrl, constrained_intra, redundant_pic_cnt;
    int transform_8x8, second_chroma_qp_offset;
} pps_t;

/* ---- pictures -------------------------------------------------------------------- */
#define MAX_REFS 16
#define PAD 32                          /* luma border; chroma has half            */

/* What a macroblock leaves behind, for its neighbours, the loop filter and
 * (as the co-located block) the B pictures after it. */
typedef struct {
    uint8_t  type;                      /* MBT_*                                   */
    uint8_t  intra, skip, pcm, i16, direct16;
    uint8_t  cbp;                       /* luma bits 0-3, chroma << 4               */
    uint8_t  chroma_mode;
    int8_t   qp, qpc[2];
    int16_t  slice;                     /* which slice of the picture              */
    uint8_t  qp_delta_nz;               /* CABAC: mb_qp_delta was not 0            */
    uint8_t  sub_direct;                /* bit k: 8x8 k was B_Direct_8x8           */
    int8_t   dbk, aoff, boff;           /* its slice's loop-filter settings          */
    uint32_t cbf;                       /* coded_block_flag: 0-15 luma, 16-23 chroma AC, 24 Y DC, 25/26 Cb/Cr DC */
    uint8_t  nz[24];                    /* coefficients in each 4x4 (CAVLC: count) */
    int8_t   i4[16];                    /* Intra4x4PredMode, -1 if not I4x4        */
    int8_t   ref[2][4];                 /* refIdx per 8x8, -1 unused                */
    int8_t   refpic[2][4];              /* the DPB slot that refIdx named          */
    int16_t  mv[2][16][2];
    uint8_t  mvd[2][16][2];             /* |mvd|, for CABAC's contexts (saturated)  */
} mbinfo;

enum { MBT_I4, MBT_I16, MBT_PCM, MBT_P, MBT_B, MBT_PSKIP, MBT_BSKIP };

typedef struct {
    uint8_t *mem;
    uint8_t *y, *u, *v;                 /* (0,0) of the picture, inside the border */
    int      ystride, cstride;
    mbinfo  *mb;
    int      poc, frame_num, frame_num_wrap, pic_num, long_term, long_idx;
    int      ref;                       /* 0 not, 1 short-term, 2 long-term        */
    int      wait_output, idr, mmco5;
    int64_t  pts;
    int      in_use;
} picture;

/* ---- the slice ---------------------------------------------------------------------- */
typedef struct {
    int type;                           /* 0 P, 1 B, 2 I                           */
    int first_mb, frame_num, idr, idr_id, nal_ref_idc;
    int poc_lsb, delta_poc_bottom, delta_poc[2];
    int direct_spatial, num_ref_idx[2];
    int qp, disable_deblock, alpha_off, beta_off;
    int cabac_init_idc;
    int luma_log2_wd, chroma_log2_wd;
    int16_t lw[2][MAX_REFS], lo[2][MAX_REFS], cw[2][MAX_REFS][2], co[2][MAX_REFS][2];
    int8_t  lwf[2][MAX_REFS], cwf[2][MAX_REFS];
    int16_t implicit_w[MAX_REFS][MAX_REFS];    /* B, weighted_bipred 2: w1 for (r0, r1) */
    picture *list[2][MAX_REFS];
    int8_t   slot[2][MAX_REFS];         /* each one's DPB slot                     */
    int mmco_count, mmco[66][3], adaptive, long_term_ref, no_output_prior;
} slice_t;

/* ---- CABAC ---------------------------------------------------------------------------- */
typedef struct { uint32_t range, offset; const uint8_t *p, *end; int bits; } cabac_t;

/* ---- the decoder --------------------------------------------------------------------------- */
#define DPB_N 18

struct h264 {
    int nal_len;
    sps_t sps[4]; pps_t pps[16];
    const sps_t *s; const pps_t *pp;
    int mb_w, mb_h, w, h;               /* in macroblocks; in luma samples          */
    picture dpb[DPB_N];
    picture *cur;
    int cur_slot;
    slice_t sl;
    int slice_num;
    /* POC */
    int prev_poc_msb, prev_poc_lsb, prev_frame_num, prev_frame_num_offset, frame_num_offset;
    int prev_ref_has_mmco5, prev_ref_poc_bottom, prev_poc_msb_cur;
    int max_long_idx;
    /* the macroblock being decoded */
    int mbx, mby, mba;
    int qp, last_qp_delta_nz, prev_mb_skipped;
    int16_t coef[16 * 16 + 2 * 16 * 4 + 32];    /* residual: 16 luma blocks of 16, DCs, chroma */
    int16_t dc_y[16], dc_c[2][4];
    uint8_t lowres_buf[8];
    /* entropy */
    bs_t bs;
    cabac_t cab;
    uint8_t ctx[460];                    /* CABAC: state << 1 | mps                  */
    /* scratch */
    uint8_t *nal;  int nal_cap;
    char err[96];
    int64_t pts;
    int out_ready;
    int frames;
};

/* h264.c */
void  h264_fail(h264 *d, const char *what);

/* h264mb.c */
int   h264_slice_data(h264 *d);
void  cabac_init_ctx(h264 *d);

/* h264pred.c */
void  h264_intra4x4(h264 *d, mbinfo *m, int blk, int mode, uint8_t *dst, int stride);
void  h264_intra16(h264 *d, mbinfo *m, int mode, uint8_t *dst, int stride);
void  h264_intra_chroma(h264 *d, mbinfo *m, int mode, uint8_t *u, uint8_t *v, int stride);
void  h264_idct4_add(uint8_t *dst, int stride, int16_t *c);
void  h264_dc_add(uint8_t *dst, int stride, int dc);
void  h264_inter(h264 *d, mbinfo *m);
int   h264_avail(h264 *d, int mbx, int mby);   /* that MB is decoded and in this slice */
void  h264_mc_init(void);

/* h264dbk.c */
void  h264_deblock(h264 *d);

extern const uint8_t h264_lps_range[64][4];
extern const uint8_t h264_lps_next[64];
extern const int8_t  h264_ctx_init_i[460][2], h264_ctx_init_pb0[460][2], h264_ctx_init_pb1[460][2], h264_ctx_init_pb2[460][2];

/* Raster position of 4x4 block k (0..15, the standard's order) in the MB. */
static const uint8_t blk_x[16] = { 0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3 };
static const uint8_t blk_y[16] = { 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3 };
/* and back: block index at raster (x, y) */
static const uint8_t blk_at[4][4] = { { 0, 1, 4, 5 }, { 2, 3, 6, 7 }, { 8, 9, 12, 13 }, { 10, 11, 14, 15 } };

static inline int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint8_t clip1(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iabs(int a) { return a < 0 ? -a : a; }
static inline int median3(int a, int b, int c) { return imax(imin(a, b), imin(imax(a, b), c)); }
