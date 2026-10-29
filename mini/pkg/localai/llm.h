/* ===========================================================================
 *  BoltOS Local AI  -  llm.h
 *
 *  A language model, run on the CPU: a GGUF file (as Hugging Face hosts
 *  them) already in memory, its tokenizer, and the transformer of the
 *  Llama family -- Qwen2/2.5, Qwen3 and Llama/SmolLM2 -- with weights in
 *  F32, F16, Q8_0, Q4_0, Q4_1 or Q6_K.
 *
 *  Nothing here knows about BoltOS: memory comes from an arena the caller
 *  hands over, and work is split across cores through a callback, so the
 *  same files build into the package and into a test program on a Mac
 *  (test/llmtest.c), where they are checked against a reference.
 * ===========================================================================*/
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ---- memory: a bump allocator over one block ------------------------------ */
typedef struct { uint8_t *base; uint64_t size, used; } arena_t;
void *arena_alloc(arena_t *a, uint64_t n);          /* 64-byte aligned; NULL if full */

/* ---- running on several cores --------------------------------------------- *
 *  par(fn, arg, n) calls fn(arg, i, n) for i in 0..n-1, at the same time on
 *  as many cores as there are, and returns when all have. n is what the
 *  caller set with llm_set_par.                                             */
typedef void (*par_fn)(void *arg, int part, int nparts);
typedef void (*par_run)(par_fn fn, void *arg);

/* ---- the file ---------------------------------------------------------------- */
enum { GT_F32 = 0, GT_F16 = 1, GT_Q4_0 = 2, GT_Q4_1 = 3, GT_Q8_0 = 8, GT_Q6_K = 14 };
typedef struct {
    const char *name; int name_len;
    int      type, nd;
    uint64_t ne[4];
    const uint8_t *data;
    uint64_t bytes;
} gtensor_t;

enum { ARCH_LLAMA, ARCH_QWEN2, ARCH_QWEN3 };

typedef struct llm llm_t;

typedef struct {
    int   arch;
    char  name[64];                   /* general.name                        */
    int   n_layer, dim, n_head, n_kv, head_dim, ffn, vocab, ctx_train;
    int   ctx;                        /* what the KV cache holds             */
    float rope_base, eps;
    int   bos, eos, eot;              /* -1 when the model has none          */
    int   im_start, im_end, think, think_end, tool_call, tool_call_end;
    uint64_t weight_bytes;            /* read for every token                 */
    uint64_t state_bytes;             /* the cache and scratch, from the arena */
} llm_info_t;

/* The file at `file` (it stays where it is: the weights are used in place).
 * ctx: tokens to keep (capped at the model's own). Returns NULL with err. */
llm_t *llm_open(const uint8_t *file, uint64_t len, arena_t *a, int ctx, char *err, int cap);
/* Keys kept in half precision rather than 8 bits: twice the memory for
 * them, slower attention, closer to the model as trained. Set before
 * llm_open; 0 by default. */
extern int llm_keys_f16;
/* The same for a file too big for one window of RAM: [0, lo_len) at lo
 * (the header in it), the rest at hi, where hi[0] is the file's byte
 * x & ~511 -- x from llm_split_point. */
llm_t *llm_open_split(const uint8_t *lo, uint64_t lo_len, const uint8_t *hi, uint64_t x, uint64_t len,
                      arena_t *a, int ctx, char *err, int cap);
uint64_t llm_split_point(const uint8_t *head, uint64_t have, uint64_t total, uint64_t max_hi);
const llm_info_t *llm_info(const llm_t *m);
/* What llm_open would take from the arena for `ctx` tokens, without opening. */
uint64_t llm_state_bytes(const uint8_t *file, uint64_t len, int ctx);
void llm_set_par(llm_t *m, par_run run, int nparts);
/* Rearrange the weights for the four-rows-at-a-time kernels (kern.c), in
 * place, on the cores llm_set_par gave it. Once, after llm_open; the file
 * in memory isn't a GGUF file any more after this. */
int  llm_prepare(llm_t *m, arena_t *a);

/* ---- the tokenizer ------------------------------------------------------------ */
int  llm_encode(llm_t *m, const char *utf8, int len, int *out, int cap);   /* no special tokens */
int  llm_piece(llm_t *m, int tok, char *out, int cap);    /* its bytes (UTF-8, maybe partial) */
int  llm_special(llm_t *m, const char *text);             /* "<|im_start|>" -> id, -1 */
int  llm_is_control(llm_t *m, int tok);                   /* a control token: never shown */

/* ---- running it ----------------------------------------------------------------- */
void   llm_reset(llm_t *m);                                /* forget everything        */
int    llm_pos(const llm_t *m);                            /* tokens in the cache      */
void   llm_rewind(llm_t *m, int pos);                      /* keep only the first pos  */
/* Feed tokens; the logits after the last one are then llm_logits(). Long
 * runs go in batches. `stop` is checked between batches: nonzero stops it.
 * Returns how many went in. */
int    llm_feed(llm_t *m, const int *toks, int n, volatile int *stop);
float *llm_logits(llm_t *m);

typedef struct {
    float temp, top_p, rep_pen;
    int   top_k, rep_last;
    uint64_t seed;
} sampler_t;
int  llm_sample(llm_t *m, sampler_t *s, const int *recent, int nrecent);

/* For the tests: the hidden state after the last token, and timing. */
typedef struct { uint64_t matmul_ns, attn_ns, other_ns, tokens, quant_ns, calls, sample_ns, attn_par_ns; } llm_stats_t;
llm_stats_t *llm_stats(llm_t *m);
uint64_t llm_now_ns(void);                                 /* provided by the host     */
