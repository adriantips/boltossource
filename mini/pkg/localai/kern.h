/* BoltOS Local AI - kern.h: quantized rows times vectors (kern.c). */
#pragma once
#include <stdint.h>
#include "llm.h"

/* A block of 32 numbers of the vector: q[i] * d; the sum of q, and d times it. */
typedef struct { int8_t q[32]; float d, s; int32_t sq, pad_; } aq_t;

/* Weights rearranged at load (repack_x4): four rows' blocks side by side,
 * the same bytes in another order. Their types, for gtensor_t.type: */
enum { GT_X4 = 0x100 };                         /* GT_X4 | the GGUF type     */
int   x4_able(int type, int rows);              /* can this be rearranged      */
void  repack_x4(int type, uint8_t *data, int rows, int n, uint8_t *tmp);   /* tmp: 4 rows' bytes */
/* Four rows (one x4 group) times B vectors (B <= 4): out[b][r]. */
void  mm_x4(int type, const uint8_t *group, int n, const aq_t *const xs[4], int B, float out[4][4]);
/* Row r of any tensor, rearranged or not. */
void  dequant_any(int type, const uint8_t *data, int r, int n, float *y);

uint64_t row_bytes(int type, int n);
int   type_ok(int type, int n);                 /* this format, a row this long */
void  quantize_act(const float *x, int n, aq_t *out);          /* n % 32 == 0 */
void  dequant_row(int type, const uint8_t *row, float *y, int n);
/* row . x: the quantized form for the quantized types, xf for F32/F16. */
float dot_row(int type, const uint8_t *row, const aq_t *x, const float *xf, int n);
void  dot_row4(int type, const uint8_t *row, const aq_t *const xs[4], const float *const xfs[4], int n, float out[4]);
float dot_ref(int type, const uint8_t *row, const aq_t *x, const float *xf, int n);
