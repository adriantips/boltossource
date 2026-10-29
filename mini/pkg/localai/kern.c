/* ===========================================================================
 *  BoltOS Local AI  -  kern.c
 *
 *  The arithmetic that takes the time: a row of quantized weights times a
 *  vector, in each format GGUF files use for the models here. The vector
 *  is quantized too, to blocks of 32 signed bytes and a scale (aq_t), so
 *  the inner loops are 8-bit multiplies into 16 and 32 bits -- what a
 *  Cortex-A53's NEON unit does best without the dot-product instructions
 *  later cores have. dot4_* take four vectors at once (the prompt goes in
 *  that way), so each block of weights is unpacked once for four tokens.
 *
 *  The *_ref functions are the plain definitions; the tests check the NEON
 *  ones against them.
 * ===========================================================================*/
#include <stdint.h>
#include <string.h>
#include "kern.h"
#include <arm_neon.h>

static inline float h2f(uint16_t h) { __fp16 f; memcpy(&f, &h, 2); return (float)f; }

typedef struct { uint16_t d; uint8_t qs[16]; } q4_0_t;
typedef struct { uint16_t d, m; uint8_t qs[16]; } q4_1_t;
typedef struct { uint16_t d; int8_t qs[32]; } q8_0_t;
typedef struct { uint8_t ql[128], qh[64]; int8_t sc[16]; uint16_t d; } q6_k_t;
_Static_assert(sizeof(q4_0_t) == 18 && sizeof(q4_1_t) == 20 && sizeof(q8_0_t) == 34 && sizeof(q6_k_t) == 210, "block sizes");

uint64_t row_bytes(int type, int n) {
    switch (type) {
    case GT_F32:  return 4ull * (uint64_t)n;
    case GT_F16:  return 2ull * (uint64_t)n;
    case GT_Q4_0: return (uint64_t)n / 32 * 18;
    case GT_Q4_1: return (uint64_t)n / 32 * 20;
    case GT_Q8_0: return (uint64_t)n / 32 * 34;
    case GT_Q6_K: return (uint64_t)n / 256 * 210;
    }
    return 0;
}

int type_ok(int type, int n) {
    switch (type) {
    case GT_F32: case GT_F16: return 1;
    case GT_Q4_0: case GT_Q4_1: case GT_Q8_0: return n % 32 == 0;
    case GT_Q6_K: return n % 256 == 0;
    }
    return 0;
}

/* ---- the vector side -------------------------------------------------------------- */
void quantize_act(const float *x, int n, aq_t *out) {
    for (int b = 0; b < n / 32; b++) {
        const float *v = x + 32 * b;
        float32x4_t mx = vdupq_n_f32(0);
        for (int i = 0; i < 32; i += 4) mx = vmaxq_f32(mx, vabsq_f32(vld1q_f32(v + i)));
        float amax = vmaxvq_f32(mx);
        float d = amax / 127.0f, id = d ? 1.0f / d : 0.0f;
        int32x4_t sum = vdupq_n_s32(0);
        for (int i = 0; i < 32; i += 16) {
            int32x4_t q0 = vcvtnq_s32_f32(vmulq_n_f32(vld1q_f32(v + i), id));
            int32x4_t q1 = vcvtnq_s32_f32(vmulq_n_f32(vld1q_f32(v + i + 4), id));
            int32x4_t q2 = vcvtnq_s32_f32(vmulq_n_f32(vld1q_f32(v + i + 8), id));
            int32x4_t q3 = vcvtnq_s32_f32(vmulq_n_f32(vld1q_f32(v + i + 12), id));
            sum = vaddq_s32(sum, vaddq_s32(vaddq_s32(q0, q1), vaddq_s32(q2, q3)));
            int16x8_t a = vcombine_s16(vmovn_s32(q0), vmovn_s32(q1)), c = vcombine_s16(vmovn_s32(q2), vmovn_s32(q3));
            vst1q_s8(out[b].q + i, vcombine_s8(vmovn_s16(a), vmovn_s16(c)));
        }
        out[b].d = d;
        out[b].sq = vaddvq_s32(sum);
        out[b].s = d * (float)out[b].sq;
    }
}

/* ---- dequantizing (an embedding row, the tests) --------------------------------------- */
void dequant_row(int type, const uint8_t *row, float *y, int n) {
    switch (type) {
    case GT_F32: memcpy(y, row, 4ull * (uint64_t)n); return;
    case GT_F16: { const uint16_t *h = (const uint16_t *)row; for (int i = 0; i < n; i++) y[i] = h2f(h[i]); return; }
    case GT_Q4_0: {
        const q4_0_t *b = (const q4_0_t *)row;
        for (int k = 0; k < n / 32; k++) {
            float d = h2f(b[k].d);
            for (int j = 0; j < 16; j++) {
                y[32 * k + j] = d * (float)((b[k].qs[j] & 15) - 8);
                y[32 * k + j + 16] = d * (float)((b[k].qs[j] >> 4) - 8);
            }
        }
        return;
    }
    case GT_Q4_1: {
        const q4_1_t *b = (const q4_1_t *)row;
        for (int k = 0; k < n / 32; k++) {
            float d = h2f(b[k].d), m = h2f(b[k].m);
            for (int j = 0; j < 16; j++) {
                y[32 * k + j] = d * (float)(b[k].qs[j] & 15) + m;
                y[32 * k + j + 16] = d * (float)(b[k].qs[j] >> 4) + m;
            }
        }
        return;
    }
    case GT_Q8_0: {
        const q8_0_t *b = (const q8_0_t *)row;
        for (int k = 0; k < n / 32; k++) {
            float d = h2f(b[k].d);
            for (int j = 0; j < 32; j++) y[32 * k + j] = d * (float)b[k].qs[j];
        }
        return;
    }
    case GT_Q6_K: {
        const q6_k_t *b = (const q6_k_t *)row;
        for (int k = 0; k < n / 256; k++) {
            float d = h2f(b[k].d);
            const uint8_t *ql = b[k].ql, *qh = b[k].qh;
            const int8_t *sc = b[k].sc;
            float *o = y + 256 * k;
            for (int h = 0; h < 2; h++) {
                for (int l = 0; l < 32; l++) {
                    int is = l / 16;
                    int q1 = ((ql[l] & 15) | ((qh[l] >> 0 & 3) << 4)) - 32;
                    int q2 = ((ql[l + 32] & 15) | ((qh[l] >> 2 & 3) << 4)) - 32;
                    int q3 = ((ql[l] >> 4) | ((qh[l] >> 4 & 3) << 4)) - 32;
                    int q4 = ((ql[l + 32] >> 4) | ((qh[l] >> 6 & 3) << 4)) - 32;
                    o[l] = d * (float)sc[is] * (float)q1;
                    o[l + 32] = d * (float)sc[is + 2] * (float)q2;
                    o[l + 64] = d * (float)sc[is + 4] * (float)q3;
                    o[l + 96] = d * (float)sc[is + 6] * (float)q4;
                }
                o += 128; ql += 64; qh += 32; sc += 8;
            }
        }
        return;
    }
    }
}

/* ---- the references ------------------------------------------------------------------- */
float dot_ref(int type, const uint8_t *row, const aq_t *x, const float *xf, int n) {
    float w[256], s = 0;
    if (type == GT_F32 || type == GT_F16) {
        for (int i = 0; i < n; i += 256) {
            int k = n - i < 256 ? n - i : 256;
            dequant_row(type, row + row_bytes(type, i), w, k);
            for (int j = 0; j < k; j++) s += w[j] * xf[i + j];
        }
        return s;
    }
    int step = type == GT_Q6_K ? 256 : 32;
    for (int i = 0; i < n; i += step) {
        dequant_row(type, row + row_bytes(type, i), w, step);
        for (int j = 0; j < step; j++) s += w[j] * x[(i + j) / 32].d * (float)x[(i + j) / 32].q[(i + j) % 32];
    }
    return s;
}

/* ---- NEON ------------------------------------------------------------------------------ */
static float dot_q4_0(const uint8_t *row, const aq_t *x, int nb) {
    const q4_0_t *w = (const q4_0_t *)row;
    const uint8x16_t m4 = vdupq_n_u8(15);
    const int8x16_t s8 = vdupq_n_s8(8);
    float32x4_t acc0 = vdupq_n_f32(0), acc1 = vdupq_n_f32(0);
    int i = 0;
    for (; i + 1 < nb; i += 2) {
        uint8x16_t v0 = vld1q_u8(w[i].qs), v1 = vld1q_u8(w[i + 1].qs);
        int8x16_t a0 = vsubq_s8(vreinterpretq_s8_u8(vandq_u8(v0, m4)), s8);
        int8x16_t b0 = vsubq_s8(vreinterpretq_s8_u8(vshrq_n_u8(v0, 4)), s8);
        int8x16_t a1 = vsubq_s8(vreinterpretq_s8_u8(vandq_u8(v1, m4)), s8);
        int8x16_t b1 = vsubq_s8(vreinterpretq_s8_u8(vshrq_n_u8(v1, 4)), s8);
        int8x16_t x0l = vld1q_s8(x[i].q), x0h = vld1q_s8(x[i].q + 16);
        int8x16_t x1l = vld1q_s8(x[i + 1].q), x1h = vld1q_s8(x[i + 1].q + 16);
        int16x8_t p0 = vmull_s8(vget_low_s8(a0), vget_low_s8(x0l));
        p0 = vmlal_high_s8(p0, a0, x0l);
        p0 = vmlal_s8(p0, vget_low_s8(b0), vget_low_s8(x0h));
        p0 = vmlal_high_s8(p0, b0, x0h);
        int16x8_t p1 = vmull_s8(vget_low_s8(a1), vget_low_s8(x1l));
        p1 = vmlal_high_s8(p1, a1, x1l);
        p1 = vmlal_s8(p1, vget_low_s8(b1), vget_low_s8(x1h));
        p1 = vmlal_high_s8(p1, b1, x1h);
        acc0 = vmlaq_n_f32(acc0, vcvtq_f32_s32(vpaddlq_s16(p0)), h2f(w[i].d) * x[i].d);
        acc1 = vmlaq_n_f32(acc1, vcvtq_f32_s32(vpaddlq_s16(p1)), h2f(w[i + 1].d) * x[i + 1].d);
    }
    for (; i < nb; i++) {
        uint8x16_t v0 = vld1q_u8(w[i].qs);
        int8x16_t a0 = vsubq_s8(vreinterpretq_s8_u8(vandq_u8(v0, m4)), s8);
        int8x16_t b0 = vsubq_s8(vreinterpretq_s8_u8(vshrq_n_u8(v0, 4)), s8);
        int8x16_t x0l = vld1q_s8(x[i].q), x0h = vld1q_s8(x[i].q + 16);
        int16x8_t p0 = vmull_s8(vget_low_s8(a0), vget_low_s8(x0l));
        p0 = vmlal_high_s8(p0, a0, x0l);
        p0 = vmlal_s8(p0, vget_low_s8(b0), vget_low_s8(x0h));
        p0 = vmlal_high_s8(p0, b0, x0h);
        acc0 = vmlaq_n_f32(acc0, vcvtq_f32_s32(vpaddlq_s16(p0)), h2f(w[i].d) * x[i].d);
    }
    return vaddvq_f32(vaddq_f32(acc0, acc1));
}

static float dot_q4_1(const uint8_t *row, const aq_t *x, int nb) {
    const q4_1_t *w = (const q4_1_t *)row;
    const uint8x16_t m4 = vdupq_n_u8(15);
    float32x4_t acc = vdupq_n_f32(0);
    float sm = 0;
    for (int i = 0; i < nb; i++) {
        uint8x16_t v = vld1q_u8(w[i].qs);
        int8x16_t a = vreinterpretq_s8_u8(vandq_u8(v, m4)), b = vreinterpretq_s8_u8(vshrq_n_u8(v, 4));
        int8x16_t xl = vld1q_s8(x[i].q), xh = vld1q_s8(x[i].q + 16);
        int16x8_t p = vmull_s8(vget_low_s8(a), vget_low_s8(xl));
        p = vmlal_high_s8(p, a, xl);
        p = vmlal_s8(p, vget_low_s8(b), vget_low_s8(xh));
        p = vmlal_high_s8(p, b, xh);                 /* 4 x 15 x 127: fits */
        acc = vmlaq_n_f32(acc, vcvtq_f32_s32(vpaddlq_s16(p)), h2f(w[i].d) * x[i].d);
        sm += h2f(w[i].m) * x[i].s;
    }
    return vaddvq_f32(acc) + sm;
}

static float dot_q8_0(const uint8_t *row, const aq_t *x, int nb) {
    const q8_0_t *w = (const q8_0_t *)row;
    float32x4_t acc0 = vdupq_n_f32(0), acc1 = vdupq_n_f32(0);
    int i = 0;
    for (; i + 1 < nb; i += 2) {
        int8x16_t a0 = vld1q_s8(w[i].qs), b0 = vld1q_s8(w[i].qs + 16);
        int8x16_t a1 = vld1q_s8(w[i + 1].qs), b1 = vld1q_s8(w[i + 1].qs + 16);
        int8x16_t x0l = vld1q_s8(x[i].q), x0h = vld1q_s8(x[i].q + 16);
        int8x16_t x1l = vld1q_s8(x[i + 1].q), x1h = vld1q_s8(x[i + 1].q + 16);
        int16x8_t p0 = vmull_s8(vget_low_s8(a0), vget_low_s8(x0l)); p0 = vmlal_high_s8(p0, a0, x0l);
        int16x8_t q0 = vmull_s8(vget_low_s8(b0), vget_low_s8(x0h)); q0 = vmlal_high_s8(q0, b0, x0h);
        int16x8_t p1 = vmull_s8(vget_low_s8(a1), vget_low_s8(x1l)); p1 = vmlal_high_s8(p1, a1, x1l);
        int16x8_t q1 = vmull_s8(vget_low_s8(b1), vget_low_s8(x1h)); q1 = vmlal_high_s8(q1, b1, x1h);
        int32x4_t s0 = vpadalq_s16(vpaddlq_s16(p0), q0), s1 = vpadalq_s16(vpaddlq_s16(p1), q1);
        acc0 = vmlaq_n_f32(acc0, vcvtq_f32_s32(s0), h2f(w[i].d) * x[i].d);
        acc1 = vmlaq_n_f32(acc1, vcvtq_f32_s32(s1), h2f(w[i + 1].d) * x[i + 1].d);
    }
    for (; i < nb; i++) {
        int8x16_t a0 = vld1q_s8(w[i].qs), b0 = vld1q_s8(w[i].qs + 16);
        int8x16_t x0l = vld1q_s8(x[i].q), x0h = vld1q_s8(x[i].q + 16);
        int16x8_t p0 = vmull_s8(vget_low_s8(a0), vget_low_s8(x0l)); p0 = vmlal_high_s8(p0, a0, x0l);
        int16x8_t q0 = vmull_s8(vget_low_s8(b0), vget_low_s8(x0h)); q0 = vmlal_high_s8(q0, b0, x0h);
        acc0 = vmlaq_n_f32(acc0, vcvtq_f32_s32(vpadalq_s16(vpaddlq_s16(p0), q0)), h2f(w[i].d) * x[i].d);
    }
    return vaddvq_f32(vaddq_f32(acc0, acc1));
}

/* Q6_K: 256 weights a block, in two halves of 128; each half is four runs of
 * 32 whose low four bits and high two bits sit in different places (see
 * dequant_row), and every 16 weights have a scale of their own. */
static inline void q6_unpack(const uint8_t *ql, const uint8_t *qh, int8x16_t q[8]) {
    const uint8x16_t m4 = vdupq_n_u8(15), m3 = vdupq_n_u8(3);
    const int8x16_t s32 = vdupq_n_s8(32);
    uint8x16_t A0 = vld1q_u8(ql), A1 = vld1q_u8(ql + 16), B0 = vld1q_u8(ql + 32), B1 = vld1q_u8(ql + 48);
    uint8x16_t H0 = vld1q_u8(qh), H1 = vld1q_u8(qh + 16);
#define Q6(lo, hi, sh) vsubq_s8(vreinterpretq_s8_u8(vorrq_u8(lo, vshlq_n_u8(vandq_u8(vshrq_n_u8(hi, sh), m3), 4))), s32)
#define Q6_0(lo, hi) vsubq_s8(vreinterpretq_s8_u8(vorrq_u8(lo, vshlq_n_u8(vandq_u8(hi, m3), 4))), s32)
    q[0] = Q6_0(vandq_u8(A0, m4), H0); q[1] = Q6_0(vandq_u8(A1, m4), H1);
    q[2] = Q6(vandq_u8(B0, m4), H0, 2); q[3] = Q6(vandq_u8(B1, m4), H1, 2);
    q[4] = Q6(vshrq_n_u8(A0, 4), H0, 4); q[5] = Q6(vshrq_n_u8(A1, 4), H1, 4);
    q[6] = Q6(vshrq_n_u8(B0, 4), H0, 6); q[7] = Q6(vshrq_n_u8(B1, 4), H1, 6);
#undef Q6
#undef Q6_0
}

/* 16 weights by 16 bytes of the vector: four lanes of 32-bit sums. */
static inline int32x4_t dot16(int8x16_t a, int8x16_t b) {
    int16x8_t p = vmull_s8(vget_low_s8(a), vget_low_s8(b));
    p = vmlal_high_s8(p, a, b);                       /* 2 x 32 x 127: fits */
    return vpaddlq_s16(p);
}

static float dot_q6_k(const uint8_t *row, const aq_t *x, int nb) {
    const q6_k_t *w = (const q6_k_t *)row;
    float32x4_t acc = vdupq_n_f32(0);
    for (int i = 0; i < nb; i++) {
        float d = h2f(w[i].d);
        const aq_t *xb = x + 8 * i;
        for (int h = 0; h < 2; h++) {
            int8x16_t q[8];
            q6_unpack(w[i].ql + 64 * h, w[i].qh + 32 * h, q);
            const int8_t *sc = w[i].sc + 8 * h;
            for (int c = 0; c < 4; c++) {                   /* runs of 32: q[2c], q[2c+1] */
                const aq_t *xc = xb + 4 * h + c;
                int32x4_t s = vmulq_n_s32(dot16(q[2 * c], vld1q_s8(xc->q)), sc[2 * c]);
                s = vmlaq_n_s32(s, dot16(q[2 * c + 1], vld1q_s8(xc->q + 16)), sc[2 * c + 1]);
                acc = vmlaq_n_f32(acc, vcvtq_f32_s32(s), d * xc->d);
            }
        }
    }
    return vaddvq_f32(acc);
}

static float dot_f(int type, const uint8_t *row, const float *xf, int n) {
    float32x4_t acc = vdupq_n_f32(0);
    int i = 0;
    if (type == GT_F32) {
        const float *w = (const float *)row;
        for (; i + 4 <= n; i += 4) acc = vfmaq_f32(acc, vld1q_f32(w + i), vld1q_f32(xf + i));
        float s = vaddvq_f32(acc);
        for (; i < n; i++) s += w[i] * xf[i];
        return s;
    }
    const __fp16 *w = (const __fp16 *)row;
    for (; i + 4 <= n; i += 4) acc = vfmaq_f32(acc, vcvt_f32_f16(vld1_f16(w + i)), vld1q_f32(xf + i));
    float s = vaddvq_f32(acc);
    for (; i < n; i++) s += (float)w[i] * xf[i];
    return s;
}

float dot_row(int type, const uint8_t *row, const aq_t *x, const float *xf, int n) {
    switch (type) {
    case GT_Q4_0: return dot_q4_0(row, x, n / 32);
    case GT_Q4_1: return dot_q4_1(row, x, n / 32);
    case GT_Q8_0: return dot_q8_0(row, x, n / 32);
    case GT_Q6_K: return dot_q6_k(row, x, n / 256);
    default:      return dot_f(type, row, xf, n);
    }
}

/* ---- four vectors at once -------------------------------------------------------------- *
 *  x0..x3 are four tokens' blocks (each n/32 of them); out[k] gets row . xk. */
static void dot4_q4_0(const uint8_t *row, const aq_t *const xs[4], int nb, float out[4]) {
    const q4_0_t *w = (const q4_0_t *)row;
    const uint8x16_t m4 = vdupq_n_u8(15);
    const int8x16_t s8 = vdupq_n_s8(8);
    float32x4_t acc[4] = { vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0) };
    for (int i = 0; i < nb; i++) {
        uint8x16_t v = vld1q_u8(w[i].qs);
        int8x16_t a = vsubq_s8(vreinterpretq_s8_u8(vandq_u8(v, m4)), s8);
        int8x16_t b = vsubq_s8(vreinterpretq_s8_u8(vshrq_n_u8(v, 4)), s8);
        float d = h2f(w[i].d);
        for (int k = 0; k < 4; k++) {
            const aq_t *xb = xs[k] + i;
            int8x16_t xl = vld1q_s8(xb->q), xh = vld1q_s8(xb->q + 16);
            int16x8_t p = vmull_s8(vget_low_s8(a), vget_low_s8(xl));
            p = vmlal_high_s8(p, a, xl);
            p = vmlal_s8(p, vget_low_s8(b), vget_low_s8(xh));
            p = vmlal_high_s8(p, b, xh);
            acc[k] = vmlaq_n_f32(acc[k], vcvtq_f32_s32(vpaddlq_s16(p)), d * xb->d);
        }
    }
    for (int k = 0; k < 4; k++) out[k] = vaddvq_f32(acc[k]);
}

static void dot4_q4_1(const uint8_t *row, const aq_t *const xs[4], int nb, float out[4]) {
    const q4_1_t *w = (const q4_1_t *)row;
    const uint8x16_t m4 = vdupq_n_u8(15);
    float32x4_t acc[4] = { vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0) };
    float sm[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < nb; i++) {
        uint8x16_t v = vld1q_u8(w[i].qs);
        int8x16_t a = vreinterpretq_s8_u8(vandq_u8(v, m4)), b = vreinterpretq_s8_u8(vshrq_n_u8(v, 4));
        float d = h2f(w[i].d), m = h2f(w[i].m);
        for (int k = 0; k < 4; k++) {
            const aq_t *xb = xs[k] + i;
            int8x16_t xl = vld1q_s8(xb->q), xh = vld1q_s8(xb->q + 16);
            int16x8_t p = vmull_s8(vget_low_s8(a), vget_low_s8(xl));
            p = vmlal_high_s8(p, a, xl);
            p = vmlal_s8(p, vget_low_s8(b), vget_low_s8(xh));
            p = vmlal_high_s8(p, b, xh);
            acc[k] = vmlaq_n_f32(acc[k], vcvtq_f32_s32(vpaddlq_s16(p)), d * xb->d);
            sm[k] += m * xb->s;
        }
    }
    for (int k = 0; k < 4; k++) out[k] = vaddvq_f32(acc[k]) + sm[k];
}

static void dot4_q8_0(const uint8_t *row, const aq_t *const xs[4], int nb, float out[4]) {
    const q8_0_t *w = (const q8_0_t *)row;
    float32x4_t acc[4] = { vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0) };
    for (int i = 0; i < nb; i++) {
        int8x16_t a = vld1q_s8(w[i].qs), b = vld1q_s8(w[i].qs + 16);
        float d = h2f(w[i].d);
        for (int k = 0; k < 4; k++) {
            const aq_t *xb = xs[k] + i;
            int8x16_t xl = vld1q_s8(xb->q), xh = vld1q_s8(xb->q + 16);
            int16x8_t p = vmull_s8(vget_low_s8(a), vget_low_s8(xl)); p = vmlal_high_s8(p, a, xl);
            int16x8_t q = vmull_s8(vget_low_s8(b), vget_low_s8(xh)); q = vmlal_high_s8(q, b, xh);
            acc[k] = vmlaq_n_f32(acc[k], vcvtq_f32_s32(vpadalq_s16(vpaddlq_s16(p), q)), d * xb->d);
        }
    }
    for (int k = 0; k < 4; k++) out[k] = vaddvq_f32(acc[k]);
}

static void dot4_q6_k(const uint8_t *row, const aq_t *const xs[4], int nb, float out[4]) {
    const q6_k_t *w = (const q6_k_t *)row;
    float32x4_t acc[4] = { vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0) };
    for (int i = 0; i < nb; i++) {
        float d = h2f(w[i].d);
        for (int h = 0; h < 2; h++) {
            int8x16_t q[8];
            q6_unpack(w[i].ql + 64 * h, w[i].qh + 32 * h, q);
            const int8_t *sc = w[i].sc + 8 * h;
            for (int c = 0; c < 4; c++)
                for (int k = 0; k < 4; k++) {
                    const aq_t *xc = xs[k] + 8 * i + 4 * h + c;
                    int32x4_t s = vmulq_n_s32(dot16(q[2 * c], vld1q_s8(xc->q)), sc[2 * c]);
                    s = vmlaq_n_s32(s, dot16(q[2 * c + 1], vld1q_s8(xc->q + 16)), sc[2 * c + 1]);
                    acc[k] = vmlaq_n_f32(acc[k], vcvtq_f32_s32(s), d * xc->d);
                }
        }
    }
    for (int k = 0; k < 4; k++) out[k] = vaddvq_f32(acc[k]);
}

void dot_row4(int type, const uint8_t *row, const aq_t *const xs[4], const float *const xfs[4], int n, float out[4]) {
    switch (type) {
    case GT_Q4_0: dot4_q4_0(row, xs, n / 32, out); return;
    case GT_Q4_1: dot4_q4_1(row, xs, n / 32, out); return;
    case GT_Q8_0: dot4_q8_0(row, xs, n / 32, out); return;
    case GT_Q6_K: dot4_q6_k(row, xs, n / 256, out); return;
    default: for (int k = 0; k < 4; k++) out[k] = dot_f(type, row, xfs[k], n); return;
    }
}

/* ---- four rows at a time ----------------------------------------------------------------- *
 *  Each group of four rows is stored block by block, the four rows' blocks
 *  together: their scales first (one load makes a vector of four), then
 *  their weights, each row's 16-byte aligned run after the last. One block
 *  of the vector is then loaded once for four rows, every row has its own
 *  chain of multiplies (an in-order A53 keeps them all moving), and the
 *  four rows' sums are folded into one vector with a single scale multiply.
 *  The 4-bit formats aren't moved to signed before multiplying: a block's
 *  sum of the vector times 8 (Q4_0's zero point) comes off at the end.  */
typedef struct { uint16_t d[4]; uint8_t qs[4][16]; } q4_0x4_t;               /*  72 */
typedef struct { uint16_t d[4], m[4]; uint8_t qs[4][16]; } q4_1x4_t;         /*  80 */
typedef struct { uint16_t d[4]; int8_t qs[4][32]; } q8_0x4_t;                /* 136 */
typedef struct { uint16_t d[4]; int8_t sc[16][4]; uint8_t ql[4][128], qh[4][64]; } q6_kx4_t;  /* 840 */
_Static_assert(sizeof(q4_0x4_t) == 72 && sizeof(q4_1x4_t) == 80 && sizeof(q8_0x4_t) == 136 && sizeof(q6_kx4_t) == 840, "x4 sizes");

int x4_able(int type, int rows) {
    return rows % 4 == 0 && (type == GT_Q4_0 || type == GT_Q4_1 || type == GT_Q8_0 || type == GT_Q6_K);
}

void repack_x4(int type, uint8_t *data, int rows, int n, uint8_t *tmp) {
    uint64_t rb = row_bytes(type, n);
    int nb = type == GT_Q6_K ? n / 256 : n / 32;
    for (int g = 0; g < rows / 4; g++) {
        uint8_t *grp = data + 4 * rb * (uint64_t)g;
        memcpy(tmp, grp, 4 * rb);
        for (int i = 0; i < nb; i++)
            for (int r = 0; r < 4; r++) {
                const uint8_t *src = tmp + rb * (uint64_t)r;
                switch (type) {
                case GT_Q4_0: { const q4_0_t *b = (const q4_0_t *)src + i; q4_0x4_t *o = (q4_0x4_t *)grp + i;
                                o->d[r] = b->d; memcpy(o->qs[r], b->qs, 16); break; }
                case GT_Q4_1: { const q4_1_t *b = (const q4_1_t *)src + i; q4_1x4_t *o = (q4_1x4_t *)grp + i;
                                o->d[r] = b->d; o->m[r] = b->m; memcpy(o->qs[r], b->qs, 16); break; }
                case GT_Q8_0: { const q8_0_t *b = (const q8_0_t *)src + i; q8_0x4_t *o = (q8_0x4_t *)grp + i;
                                o->d[r] = b->d; memcpy(o->qs[r], b->qs, 32); break; }
                case GT_Q6_K: { const q6_k_t *b = (const q6_k_t *)src + i; q6_kx4_t *o = (q6_kx4_t *)grp + i;
                                o->d[r] = b->d; for (int j = 0; j < 16; j++) o->sc[j][r] = b->sc[j];
                                memcpy(o->ql[r], b->ql, 128); memcpy(o->qh[r], b->qh, 64); break; }
                }
            }
    }
}

void dequant_any(int type, const uint8_t *data, int r, int n, float *y) {
    if (!(type & GT_X4)) { dequant_row(type, data + row_bytes(type, n) * (uint64_t)r, y, n); return; }
    int t = type & 0xFF, lane = r % 4;
    uint64_t rb = row_bytes(t, n);
    const uint8_t *grp = data + 4 * rb * (uint64_t)(r / 4);
    int nb = t == GT_Q6_K ? n / 256 : n / 32;
    for (int i = 0; i < nb; i++) {
        switch (t) {
        case GT_Q4_0: { const q4_0x4_t *b = (const q4_0x4_t *)grp + i; q4_0_t one; one.d = b->d[lane]; memcpy(one.qs, b->qs[lane], 16);
                        dequant_row(t, (const uint8_t *)&one, y + 32 * i, 32); break; }
        case GT_Q4_1: { const q4_1x4_t *b = (const q4_1x4_t *)grp + i; q4_1_t one; one.d = b->d[lane]; one.m = b->m[lane];
                        memcpy(one.qs, b->qs[lane], 16); dequant_row(t, (const uint8_t *)&one, y + 32 * i, 32); break; }
        case GT_Q8_0: { const q8_0x4_t *b = (const q8_0x4_t *)grp + i; q8_0_t one; one.d = b->d[lane]; memcpy(one.qs, b->qs[lane], 32);
                        dequant_row(t, (const uint8_t *)&one, y + 32 * i, 32); break; }
        case GT_Q6_K: { const q6_kx4_t *b = (const q6_kx4_t *)grp + i; q6_k_t one; one.d = b->d[lane];
                        for (int j = 0; j < 16; j++) one.sc[j] = b->sc[j][lane];
                        memcpy(one.ql, b->ql[lane], 128); memcpy(one.qh, b->qh[lane], 64);
                        dequant_row(t, (const uint8_t *)&one, y + 256 * i, 256); break; }
        }
    }
}

/* The four rows' int32x4 partial sums, folded into one vector: row r in lane r. */
static inline int32x4_t fold4(int32x4_t a, int32x4_t b, int32x4_t c, int32x4_t d) {
    return vpaddq_s32(vpaddq_s32(a, b), vpaddq_s32(c, d));
}

/* A 16-byte run of weights (0..15 or -32..31, or Q8) times 16 of the vector, two products a lane. */
static inline int16x8_t mul16(int8x16_t w, int8x16_t x) {
    return vmlal_high_s8(vmull_s8(vget_low_s8(w), vget_low_s8(x)), w, x);
}

static void mm_q4_0x4(const q4_0x4_t *w, int nb, const aq_t *const xs[4], int B, float out[4][4]) {
    const uint8x16_t m4 = vdupq_n_u8(15);
    float32x4_t acc[4] = { vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0) };
    for (int i = 0; i < nb; i++) {
        float32x4_t dw = vcvt_f32_f16(vld1_f16((const __fp16 *)w[i].d));
        uint8x16_t v0 = vld1q_u8(w[i].qs[0]), v1 = vld1q_u8(w[i].qs[1]), v2 = vld1q_u8(w[i].qs[2]), v3 = vld1q_u8(w[i].qs[3]);
        int8x16_t l0 = vreinterpretq_s8_u8(vandq_u8(v0, m4)), h0 = vreinterpretq_s8_u8(vshrq_n_u8(v0, 4));
        int8x16_t l1 = vreinterpretq_s8_u8(vandq_u8(v1, m4)), h1 = vreinterpretq_s8_u8(vshrq_n_u8(v1, 4));
        int8x16_t l2 = vreinterpretq_s8_u8(vandq_u8(v2, m4)), h2 = vreinterpretq_s8_u8(vshrq_n_u8(v2, 4));
        int8x16_t l3 = vreinterpretq_s8_u8(vandq_u8(v3, m4)), h3 = vreinterpretq_s8_u8(vshrq_n_u8(v3, 4));
        for (int b = 0; b < B; b++) {
            const aq_t *x = xs[b] + i;
            int8x16_t xl = vld1q_s8(x->q), xh = vld1q_s8(x->q + 16);
            int16x8_t p0 = vaddq_s16(mul16(l0, xl), mul16(h0, xh));     /* 4 x 15 x 127: fits */
            int16x8_t p1 = vaddq_s16(mul16(l1, xl), mul16(h1, xh));
            int16x8_t p2 = vaddq_s16(mul16(l2, xl), mul16(h2, xh));
            int16x8_t p3 = vaddq_s16(mul16(l3, xl), mul16(h3, xh));
            int32x4_t s = fold4(vpaddlq_s16(p0), vpaddlq_s16(p1), vpaddlq_s16(p2), vpaddlq_s16(p3));
            s = vsubq_s32(s, vdupq_n_s32(8 * x->sq));
            acc[b] = vfmaq_f32(acc[b], vcvtq_f32_s32(s), vmulq_n_f32(dw, x->d));
        }
    }
    for (int b = 0; b < B; b++) vst1q_f32(out[b], acc[b]);
}

static void mm_q4_1x4(const q4_1x4_t *w, int nb, const aq_t *const xs[4], int B, float out[4][4]) {
    const uint8x16_t m4 = vdupq_n_u8(15);
    float32x4_t acc[4] = { vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0) };
    for (int i = 0; i < nb; i++) {
        float32x4_t dw = vcvt_f32_f16(vld1_f16((const __fp16 *)w[i].d)), mw = vcvt_f32_f16(vld1_f16((const __fp16 *)w[i].m));
        uint8x16_t v0 = vld1q_u8(w[i].qs[0]), v1 = vld1q_u8(w[i].qs[1]), v2 = vld1q_u8(w[i].qs[2]), v3 = vld1q_u8(w[i].qs[3]);
        int8x16_t l0 = vreinterpretq_s8_u8(vandq_u8(v0, m4)), h0 = vreinterpretq_s8_u8(vshrq_n_u8(v0, 4));
        int8x16_t l1 = vreinterpretq_s8_u8(vandq_u8(v1, m4)), h1 = vreinterpretq_s8_u8(vshrq_n_u8(v1, 4));
        int8x16_t l2 = vreinterpretq_s8_u8(vandq_u8(v2, m4)), h2 = vreinterpretq_s8_u8(vshrq_n_u8(v2, 4));
        int8x16_t l3 = vreinterpretq_s8_u8(vandq_u8(v3, m4)), h3 = vreinterpretq_s8_u8(vshrq_n_u8(v3, 4));
        for (int b = 0; b < B; b++) {
            const aq_t *x = xs[b] + i;
            int8x16_t xl = vld1q_s8(x->q), xh = vld1q_s8(x->q + 16);
            int16x8_t p0 = vaddq_s16(mul16(l0, xl), mul16(h0, xh));
            int16x8_t p1 = vaddq_s16(mul16(l1, xl), mul16(h1, xh));
            int16x8_t p2 = vaddq_s16(mul16(l2, xl), mul16(h2, xh));
            int16x8_t p3 = vaddq_s16(mul16(l3, xl), mul16(h3, xh));
            int32x4_t s = fold4(vpaddlq_s16(p0), vpaddlq_s16(p1), vpaddlq_s16(p2), vpaddlq_s16(p3));
            acc[b] = vfmaq_f32(acc[b], vcvtq_f32_s32(s), vmulq_n_f32(dw, x->d));
            acc[b] = vfmaq_n_f32(acc[b], mw, x->s);
        }
    }
    for (int b = 0; b < B; b++) vst1q_f32(out[b], acc[b]);
}

static void mm_q8_0x4(const q8_0x4_t *w, int nb, const aq_t *const xs[4], int B, float out[4][4]) {
    float32x4_t acc[4] = { vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0) };
    for (int i = 0; i < nb; i++) {
        float32x4_t dw = vcvt_f32_f16(vld1_f16((const __fp16 *)w[i].d));
        int8x16_t a0 = vld1q_s8(w[i].qs[0]), b0 = vld1q_s8(w[i].qs[0] + 16), a1 = vld1q_s8(w[i].qs[1]), b1 = vld1q_s8(w[i].qs[1] + 16);
        int8x16_t a2 = vld1q_s8(w[i].qs[2]), b2 = vld1q_s8(w[i].qs[2] + 16), a3 = vld1q_s8(w[i].qs[3]), b3 = vld1q_s8(w[i].qs[3] + 16);
        for (int b = 0; b < B; b++) {
            const aq_t *x = xs[b] + i;
            int8x16_t xl = vld1q_s8(x->q), xh = vld1q_s8(x->q + 16);
            int32x4_t s = fold4(vpadalq_s16(vpaddlq_s16(mul16(a0, xl)), mul16(b0, xh)),
                                vpadalq_s16(vpaddlq_s16(mul16(a1, xl)), mul16(b1, xh)),
                                vpadalq_s16(vpaddlq_s16(mul16(a2, xl)), mul16(b2, xh)),
                                vpadalq_s16(vpaddlq_s16(mul16(a3, xl)), mul16(b3, xh)));
            acc[b] = vfmaq_f32(acc[b], vcvtq_f32_s32(s), vmulq_n_f32(dw, x->d));
        }
    }
    for (int b = 0; b < B; b++) vst1q_f32(out[b], acc[b]);
}

/* One run of 32 weights (c = 0..3 of a 128-weight half) of one row, as two
 * vectors of -32..31: the low four bits from ql, the high two from qh. */
static inline void q6_run(const uint8_t *ql, const uint8_t *qh, int c, int8x16_t *a, int8x16_t *b) {
    const uint8x16_t m4 = vdupq_n_u8(15), m3 = vdupq_n_u8(3);
    const int8x16_t s32 = vdupq_n_s8(32), sh = vdupq_n_s8((int8_t)(-2 * c));
    uint8x16_t L0 = vld1q_u8(ql + (c & 1) * 32), L1 = vld1q_u8(ql + (c & 1) * 32 + 16);
    if (c >= 2) { L0 = vshrq_n_u8(L0, 4); L1 = vshrq_n_u8(L1, 4); }
    else { L0 = vandq_u8(L0, m4); L1 = vandq_u8(L1, m4); }
    uint8x16_t H0 = vandq_u8(vshlq_u8(vld1q_u8(qh), sh), m3), H1 = vandq_u8(vshlq_u8(vld1q_u8(qh + 16), sh), m3);
    *a = vsubq_s8(vreinterpretq_s8_u8(vorrq_u8(L0, vshlq_n_u8(H0, 4))), s32);
    *b = vsubq_s8(vreinterpretq_s8_u8(vorrq_u8(L1, vshlq_n_u8(H1, 4))), s32);
}

static void mm_q6_kx4(const q6_kx4_t *w, int nb, const aq_t *const xs[4], int B, float out[4][4]) {
    float32x4_t acc[4] = { vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0) };
    for (int i = 0; i < nb; i++) {
        float32x4_t dw = vcvt_f32_f16(vld1_f16((const __fp16 *)w[i].d));
        for (int h = 0; h < 2; h++)
            for (int c = 0; c < 4; c++) {
                int8x16_t a0, b0, a1, b1, a2, b2, a3, b3;
                q6_run(w[i].ql[0] + 64 * h, w[i].qh[0] + 32 * h, c, &a0, &b0);
                q6_run(w[i].ql[1] + 64 * h, w[i].qh[1] + 32 * h, c, &a1, &b1);
                q6_run(w[i].ql[2] + 64 * h, w[i].qh[2] + 32 * h, c, &a2, &b2);
                q6_run(w[i].ql[3] + 64 * h, w[i].qh[3] + 32 * h, c, &a3, &b3);
                int j = 8 * h + 2 * c;                          /* the run's two 16-weight scales */
                int32x4_t sa = vmovl_s16(vget_low_s16(vmovl_s8(vld1_s8(w[i].sc[j]))));
                int32x4_t sb = vmovl_s16(vget_low_s16(vmovl_s8(vld1_s8(w[i].sc[j + 1]))));
                for (int b = 0; b < B; b++) {
                    const aq_t *x = xs[b] + 8 * i + 4 * h + c;
                    int8x16_t xl = vld1q_s8(x->q), xh = vld1q_s8(x->q + 16);
                    int32x4_t ta = fold4(vpaddlq_s16(mul16(a0, xl)), vpaddlq_s16(mul16(a1, xl)),
                                         vpaddlq_s16(mul16(a2, xl)), vpaddlq_s16(mul16(a3, xl)));
                    int32x4_t tb = fold4(vpaddlq_s16(mul16(b0, xh)), vpaddlq_s16(mul16(b1, xh)),
                                         vpaddlq_s16(mul16(b2, xh)), vpaddlq_s16(mul16(b3, xh)));
                    int32x4_t sm = vmlaq_s32(vmulq_s32(ta, sa), tb, sb);
                    acc[b] = vfmaq_f32(acc[b], vcvtq_f32_s32(sm), vmulq_n_f32(dw, x->d));
                }
            }
    }
    for (int b = 0; b < B; b++) vst1q_f32(out[b], acc[b]);
}

void mm_x4(int type, const uint8_t *g, int n, const aq_t *const xs[4], int B, float out[4][4]) {
    switch (type & 0xFF) {
    case GT_Q4_0: mm_q4_0x4((const q4_0x4_t *)g, n / 32, xs, B, out); return;
    case GT_Q4_1: mm_q4_1x4((const q4_1x4_t *)g, n / 32, xs, B, out); return;
    case GT_Q8_0: mm_q8_0x4((const q8_0x4_t *)g, n / 32, xs, B, out); return;
    case GT_Q6_K: mm_q6_kx4((const q6_kx4_t *)g, n / 256, xs, B, out); return;
    }
}
