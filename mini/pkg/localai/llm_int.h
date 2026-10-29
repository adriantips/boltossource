/* BoltOS Local AI - llm_int.h: what llm.c and tok.c share inside a model. */
#pragma once
#include "llm.h"
#include "kern.h"

enum { PRE_QWEN2, PRE_SMOLLM, PRE_GPT2 };

typedef struct {
    const gtensor_t *attn_norm, *q, *k, *v, *o, *q_bias, *k_bias, *v_bias, *q_norm, *k_norm;
    const gtensor_t *ffn_norm, *gate, *up, *down;
} layer_t;

typedef struct {
    int n;                                /* tokens                               */
    const uint8_t **str; uint16_t *len;   /* each one's text, in the file         */
    uint8_t *type;                        /* GGUF token types: 3 control, 4 user  */
    uint32_t *vh; uint32_t vmask;         /* text -> id (open addressing), id+1   */
    uint64_t *mk; uint32_t *mv; uint32_t mmask;   /* (left<<32|right) -> rank<<20... */
    uint32_t *mres;                       /* merge rank -> resulting token          */
    int byte_tok[256];                    /* each byte's own token                  */
    uint8_t cp2byte[324];                 /* byte-level code point -> byte, 256+ = none */
    int pre;
} vocab_t;

struct llm {
    llm_info_t info;
    gtensor_t *t; int nt;
    const gtensor_t *tok_embd, *out_norm, *output;
    layer_t *L;
    vocab_t voc;
    int rope_neox;
    /* state */
    int8_t *kc, *vc;                      /* [layer][ctx][n_kv*head_dim], 8 bits   */
    __fp16 *kh;                           /* the keys in half precision instead (k16) */
    int k16;
    float  *ks;                           /* [layer][ctx][n_kv*head_dim/32]: keys' scales, per 32 */
    float  *vs;                           /* [layer][ctx][n_kv]: values' scale, per head */
    int16_t *w16;                         /* [n_head][ctx]: attention weights       */
    int pos;
    float *x, *xb, *xb2, *q, *k, *v, *att, *hb, *hb2, *logits;   /* B tokens each */
    aq_t *aq;                             /* the quantized vector(s)               */
    float *rope_cos, *rope_sin;           /* [ctx][head_dim/2]                     */
    int B;                                /* tokens a batch: 4                     */
    par_run par; int nparts;
    llm_stats_t st;
};

int  vocab_load(llm_t *m, const uint8_t *file, uint64_t len, arena_t *a, char *err, int cap);
const gtensor_t *find_tensor(const llm_t *m, const char *name);
