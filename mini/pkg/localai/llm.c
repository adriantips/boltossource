/* ===========================================================================
 *  BoltOS Local AI  -  llm.c
 *
 *  The model: its GGUF file read in place, and the transformer run over it.
 *  Every layer is the same few steps --
 *
 *      x += Wo . attention(rope(Wq . norm(x)), rope(Wk . norm(x)), Wv . norm(x))
 *      x += Wdown . (silu(Wgate . norm(x)) * (Wup . norm(x)))
 *
 *  -- with Qwen2's biases on q, k and v, and Qwen3's norms on each head of q
 *  and k. Keys and values are kept for every position, in half precision.
 *
 *  A prompt goes through four tokens at a time, so each row of weights,
 *  once unpacked, serves four; a reply comes one token at a time. Every
 *  product of a weight matrix and the vector(s) is split by rows over the
 *  cores (par), and so is attention, by heads.
 * ===========================================================================*/
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <arm_neon.h>
#include "llm_int.h"

#define NB 4                                  /* tokens a batch */

void *arena_alloc(arena_t *a, uint64_t n) {
    uint64_t at = (a->used + 63) & ~63ull;
    if (at + n > a->size) return 0;
    a->used = at + n;
    return a->base + at;
}

/* ---- GGUF ---------------------------------------------------------------------------- */
enum { GV_U8, GV_I8, GV_U16, GV_I16, GV_U32, GV_I32, GV_F32, GV_BOOL, GV_STR, GV_ARR, GV_U64, GV_I64, GV_F64 };
static const int gv_size[13] = { 1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8 };

static struct {
    const uint8_t *file; uint64_t len;
    int n;
    struct { const uint8_t *k; int kl; uint32_t type; const uint8_t *v; } e[160];
    const uint8_t *tensors; uint64_t nt;
    uint32_t align;
    int ok;
} G;

static int gguf_scan(const uint8_t *f, uint64_t len) {
    if (G.file == f && G.len == len) return G.ok;
    memset(&G, 0, sizeof G);
    G.file = f; G.len = len;
    const uint8_t *p = f, *end = f + len;
#define NEED(n) do { if ((uint64_t)(end - p) < (uint64_t)(n)) return 0; } while (0)
    NEED(24);
    if (memcmp(p, "GGUF", 4)) return 0;
    uint32_t ver; memcpy(&ver, p + 4, 4);
    if (ver < 2) return 0;
    uint64_t nt, nkv; memcpy(&nt, p + 8, 8); memcpy(&nkv, p + 16, 8);
    p += 24;
    G.align = 32;
    for (uint64_t i = 0; i < nkv; i++) {
        NEED(8); uint64_t kl; memcpy(&kl, p, 8); p += 8;
        NEED(kl + 4);
        const uint8_t *k = p; p += kl;
        uint32_t t; memcpy(&t, p, 4); p += 4;
        if (G.n < 160) { G.e[G.n].k = k; G.e[G.n].kl = (int)kl; G.e[G.n].type = t; G.e[G.n].v = p; G.n++; }
        if (t == GV_STR) { NEED(8); uint64_t l; memcpy(&l, p, 8); NEED(8 + l); p += 8 + l; }
        else if (t == GV_ARR) {
            NEED(12); uint32_t et; uint64_t n; memcpy(&et, p, 4); memcpy(&n, p + 4, 8); p += 12;
            if (et == GV_STR) for (uint64_t j = 0; j < n; j++) { NEED(8); uint64_t l; memcpy(&l, p, 8); NEED(8 + l); p += 8 + l; }
            else if (et < 13 && gv_size[et]) { NEED(n * (uint64_t)gv_size[et]); p += n * (uint64_t)gv_size[et]; }
            else return 0;
        } else if (t < 13 && gv_size[t]) { NEED(gv_size[t]); p += gv_size[t]; }
        else return 0;
        if (kl == 17 && !memcmp(k, "general.alignment", 17) && t == GV_U32) memcpy(&G.align, G.e[G.n - 1].v, 4);
    }
    G.tensors = p; G.nt = nt;
    G.ok = 1;
#undef NEED
    return 1;
}

const uint8_t *gguf_value(const uint8_t *f, uint64_t len, const char *key, uint32_t *type) {
    if (!gguf_scan(f, len)) return 0;
    int kl = (int)strlen(key);
    for (int i = 0; i < G.n; i++)
        if (G.e[i].kl == kl && !memcmp(G.e[i].k, key, (size_t)kl)) { *type = G.e[i].type; return G.e[i].v; }
    return 0;
}

const uint8_t *gguf_array(const uint8_t *f, uint64_t len, const char *key, uint32_t *etype, uint64_t *n) {
    uint32_t t;
    const uint8_t *v = gguf_value(f, len, key, &t);
    if (!v || t != GV_ARR) return 0;
    memcpy(etype, v, 4); memcpy(n, v + 4, 8);
    return v + 12;
}

static int64_t kv_int(const uint8_t *f, uint64_t len, const char *key, int64_t dflt) {
    uint32_t t;
    const uint8_t *v = gguf_value(f, len, key, &t);
    if (!v) return dflt;
    switch (t) {
    case GV_U8: return v[0];
    case GV_I8: return (int8_t)v[0];
    case GV_U16: { uint16_t x; memcpy(&x, v, 2); return x; }
    case GV_I16: { int16_t x; memcpy(&x, v, 2); return x; }
    case GV_U32: { uint32_t x; memcpy(&x, v, 4); return x; }
    case GV_I32: { int32_t x; memcpy(&x, v, 4); return x; }
    case GV_U64: case GV_I64: { int64_t x; memcpy(&x, v, 8); return x; }
    case GV_BOOL: return v[0];
    }
    return dflt;
}

static float kv_float(const uint8_t *f, uint64_t len, const char *key, float dflt) {
    uint32_t t;
    const uint8_t *v = gguf_value(f, len, key, &t);
    if (!v) return dflt;
    if (t == GV_F32) { float x; memcpy(&x, v, 4); return x; }
    if (t == GV_F64) { double x; memcpy(&x, v, 8); return (float)x; }
    return (float)kv_int(f, len, key, (int64_t)dflt);
}

static void kv_str(const uint8_t *f, uint64_t len, const char *key, char *out, int cap) {
    uint32_t t;
    const uint8_t *v = gguf_value(f, len, key, &t);
    out[0] = 0;
    if (!v || t != GV_STR) return;
    uint64_t l; memcpy(&l, v, 8);
    if (l > (uint64_t)cap - 1) l = (uint64_t)cap - 1;
    memcpy(out, v + 8, l); out[l] = 0;
}

/* Where the file is: all of it at lo, or -- when it's bigger than either
 * window of RAM -- [0, lo_len) at lo and the rest at hi, hi holding the
 * file from x0 on. A tensor that starts before x is in lo. */
typedef struct { const uint8_t *lo; uint64_t lo_len; const uint8_t *hi; uint64_t x, x0, len; } fmap_t;
static fmap_t FM;

static int read_tensors(llm_t *m, arena_t *a, const uint8_t *f, uint64_t len, char *err, int cap) {
    (void)len;
    m->nt = (int)G.nt;
    m->t = arena_alloc(a, sizeof(gtensor_t) * (uint64_t)m->nt);
    if (!m->t) { snprintf(err, (size_t)cap, "Not enough memory"); return -1; }
    const uint8_t *p = G.tensors, *end = f + len;
    for (int i = 0; i < m->nt; i++) {
        gtensor_t *t = &m->t[i];
        if (end - p < 8) goto bad;
        uint64_t nl; memcpy(&nl, p, 8); p += 8;
        if ((uint64_t)(end - p) < nl + 4) goto bad;
        t->name = (const char *)p; t->name_len = (int)nl; p += nl;
        uint32_t nd; memcpy(&nd, p, 4); p += 4;
        if (nd > 4 || (uint64_t)(end - p) < 8ull * nd + 12) goto bad;
        t->nd = (int)nd;
        for (int d = 0; d < 4; d++) t->ne[d] = 1;
        for (uint32_t d = 0; d < nd; d++) { memcpy(&t->ne[d], p, 8); p += 8; }
        uint32_t ty; memcpy(&ty, p, 4); p += 4;
        uint64_t off; memcpy(&off, p, 8); p += 8;
        t->type = (int)ty;
        t->data = (const uint8_t *)(uintptr_t)off;               /* fixed up below */
    }
    uint64_t data0 = (uint64_t)(p - f);
    data0 = (data0 + G.align - 1) / G.align * G.align;
    for (int i = 0; i < m->nt; i++) {
        gtensor_t *t = &m->t[i];
        uint64_t off = (uint64_t)(uintptr_t)t->data;
        if (!type_ok(t->type, (int)t->ne[0])) {
            snprintf(err, (size_t)cap, "%.*s is in a format this can't run (type %d)", t->name_len, t->name, t->type);
            return -1;
        }
        t->bytes = row_bytes(t->type, (int)t->ne[0]) * t->ne[1] * t->ne[2] * t->ne[3];
        uint64_t at = data0 + off;
        if (at < FM.x) { if (at + t->bytes > FM.lo_len) goto bad; t->data = FM.lo + at; }
        else { if (at + t->bytes > FM.len) goto bad; t->data = FM.hi + (at - FM.x0); }
    }
    return 0;
bad:
    snprintf(err, (size_t)cap, "The file is damaged or cut short");
    return -1;
}

const gtensor_t *find_tensor(const llm_t *m, const char *name) {
    int n = (int)strlen(name);
    for (int i = 0; i < m->nt; i++) if (m->t[i].name_len == n && !memcmp(m->t[i].name, name, (size_t)n)) return &m->t[i];
    return 0;
}

/* ---- setting up ------------------------------------------------------------------------ */
static int hparams(const uint8_t *f, uint64_t len, llm_info_t *I, char *err, int cap) {
    char arch[32], k[96];
    kv_str(f, len, "general.architecture", arch, sizeof arch);
    if (!strcmp(arch, "llama")) I->arch = ARCH_LLAMA;
    else if (!strcmp(arch, "qwen2")) I->arch = ARCH_QWEN2;
    else if (!strcmp(arch, "qwen3")) I->arch = ARCH_QWEN3;
    else { snprintf(err, (size_t)cap, "It's a \"%s\" model; this runs Llama, Qwen2 and Qwen3 ones", arch); return -1; }
#define KI(name, d) (snprintf(k, sizeof k, "%s." name, arch), kv_int(f, len, k, d))
#define KF(name, d) (snprintf(k, sizeof k, "%s." name, arch), kv_float(f, len, k, d))
    I->n_layer = (int)KI("block_count", 0);
    I->dim = (int)KI("embedding_length", 0);
    I->ffn = (int)KI("feed_forward_length", 0);
    I->n_head = (int)KI("attention.head_count", 0);
    I->n_kv = (int)KI("attention.head_count_kv", I->n_head);
    I->head_dim = I->n_head ? (int)KI("attention.key_length", I->dim / I->n_head) : 0;
    I->ctx_train = (int)KI("context_length", 2048);
    I->rope_base = KF("rope.freq_base", 10000.0f);
    I->eps = KF("attention.layer_norm_rms_epsilon", 1e-6f);
#undef KI
#undef KF
    kv_str(f, len, "general.name", I->name, sizeof I->name);
    I->bos = (int)kv_int(f, len, "tokenizer.ggml.bos_token_id", -1);
    I->eos = (int)kv_int(f, len, "tokenizer.ggml.eos_token_id", -1);
    if (!I->n_layer || !I->dim || !I->n_head || !I->ffn || !I->head_dim || I->n_head % I->n_kv ||
        I->n_layer > 64 || I->head_dim > 256 || I->head_dim % 64) {
        snprintf(err, (size_t)cap, "The model's shape is missing or odd"); return -1;
    }
    return 0;
}

static uint64_t state_bytes(const llm_info_t *I, int ctx) {
    uint64_t kvd = (uint64_t)I->n_kv * I->head_dim, qd = (uint64_t)I->n_head * I->head_dim;
    uint64_t big = (uint64_t)I->dim > qd ? (uint64_t)I->dim : qd;
    uint64_t s = (uint64_t)I->n_layer * (uint64_t)ctx * (2 * kvd + kvd / 8 + 4ull * I->n_kv);   /* the cache */
    s += 2ull * I->n_head * (uint64_t)ctx;
    s += 4ull * NB * (3 * big + qd + 2 * kvd + 2 * (uint64_t)I->ffn);
    s += 4ull * I->n_head * (uint64_t)ctx;                        /* scores */
    s += 4ull * 151936 + 4ull * 262144;                           /* logits, generously */
    s += (sizeof(aq_t)) * NB * ((uint64_t)I->ffn > big ? (uint64_t)I->ffn : big) / 32;
    s += 8ull * (uint64_t)ctx * (uint64_t)(I->head_dim / 2);      /* rope */
    s += 64ull << 20;                                             /* the tokenizer's tables, tensors */
    return s;
}

uint64_t llm_state_bytes(const uint8_t *file, uint64_t len, int ctx) {
    llm_info_t I; char err[8];
    memset(&I, 0, sizeof I);
    if (!gguf_scan(file, len) || hparams(file, len, &I, err, sizeof err)) return 0;
    if (ctx > I.ctx_train) ctx = I.ctx_train;
    return state_bytes(&I, ctx);
}

static void serial(par_fn fn, void *arg) { fn(arg, 0, 1); }
int llm_keys_f16;

/* Where to cut a file of `total` bytes so that what goes high fits in
 * max_hi: the start of the first tensor that leaves little enough after it.
 * `head` holds the file's first bytes, its header among them. 0 if none. */
uint64_t llm_split_point(const uint8_t *head, uint64_t have, uint64_t total, uint64_t max_hi) {
    if (!gguf_scan(head, have)) return 0;
    const uint8_t *p = G.tensors, *end = head + have;
    uint64_t best = 0;
    for (uint64_t i = 0; i < G.nt; i++) {
        if (end - p < 8) return 0;
        uint64_t nl; memcpy(&nl, p, 8); p += 8 + nl;
        if (end - p < 4) return 0;
        uint32_t nd; memcpy(&nd, p, 4); p += 4 + 8ull * nd + 4;
        if (end - p < 8) return 0;
        uint64_t off; memcpy(&off, p, 8); p += 8;
        (void)off;
    }
    uint64_t data0 = ((uint64_t)(p - head) + G.align - 1) / G.align * G.align;
    p = G.tensors;
    for (uint64_t i = 0; i < G.nt; i++) {
        uint64_t nl; memcpy(&nl, p, 8); p += 8 + nl;
        uint32_t nd; memcpy(&nd, p, 4); p += 4 + 8ull * nd + 4;
        uint64_t off; memcpy(&off, p, 8); p += 8;
        uint64_t at = data0 + off;
        if (total - (at & ~511ull) <= max_hi && (!best || at < best)) best = at;
    }
    return best;
}

llm_t *llm_open_split(const uint8_t *lo, uint64_t lo_len, const uint8_t *hi, uint64_t x, uint64_t len,
                      arena_t *a, int ctx, char *err, int cap) {
    FM.lo = lo; FM.lo_len = lo_len; FM.hi = hi; FM.x = x; FM.x0 = x & ~511ull; FM.len = len;
    return llm_open(lo, lo_len, a, ctx, err, cap);
}

llm_t *llm_open(const uint8_t *f, uint64_t len, arena_t *a, int ctx, char *err, int cap) {
    if (FM.lo != f || FM.lo_len != len) { FM.lo = f; FM.lo_len = len; FM.hi = 0; FM.x = FM.x0 = FM.len = len; }
    if (!gguf_scan(f, len)) { snprintf(err, (size_t)cap, "It isn't a GGUF file, or it's cut short"); return 0; }
    llm_t *m = arena_alloc(a, sizeof *m);
    if (!m) { snprintf(err, (size_t)cap, "Not enough memory"); return 0; }
    memset(m, 0, sizeof *m);
    llm_info_t *I = &m->info;
    if (hparams(f, len, I, err, cap) || read_tensors(m, a, f, len, err, cap)) return 0;
    if (ctx > I->ctx_train) ctx = I->ctx_train;
    I->ctx = ctx;
    m->rope_neox = I->arch != ARCH_LLAMA;
    m->tok_embd = find_tensor(m, "token_embd.weight");
    m->out_norm = find_tensor(m, "output_norm.weight");
    m->output = find_tensor(m, "output.weight");
    if (!m->output) m->output = m->tok_embd;                      /* tied */
    if (!m->tok_embd || !m->out_norm) { snprintf(err, (size_t)cap, "The file is missing its embeddings"); return 0; }
    m->L = arena_alloc(a, sizeof(layer_t) * (uint64_t)I->n_layer);
    if (!m->L) { snprintf(err, (size_t)cap, "Not enough memory"); return 0; }
    int qd = I->n_head * I->head_dim, kvd = I->n_kv * I->head_dim;
    I->weight_bytes = m->output->bytes;
    for (int l = 0; l < I->n_layer; l++) {
        layer_t *L = &m->L[l];
        char n[64];
#define T(field, name) (snprintf(n, sizeof n, "blk.%d." name, l), L->field = find_tensor(m, n))
        T(attn_norm, "attn_norm.weight"); T(q, "attn_q.weight"); T(k, "attn_k.weight"); T(v, "attn_v.weight");
        T(o, "attn_output.weight"); T(q_bias, "attn_q.bias"); T(k_bias, "attn_k.bias"); T(v_bias, "attn_v.bias");
        T(q_norm, "attn_q_norm.weight"); T(k_norm, "attn_k_norm.weight");
        T(ffn_norm, "ffn_norm.weight"); T(gate, "ffn_gate.weight"); T(up, "ffn_up.weight"); T(down, "ffn_down.weight");
#undef T
        if (!L->attn_norm || !L->q || !L->k || !L->v || !L->o || !L->ffn_norm || !L->gate || !L->up || !L->down ||
            L->attn_norm->type != GT_F32 || L->ffn_norm->type != GT_F32 ||
            (int)L->q->ne[0] != I->dim || (int)L->q->ne[1] != qd || (int)L->k->ne[1] != kvd || (int)L->v->ne[1] != kvd ||
            (int)L->o->ne[0] != qd || (int)L->gate->ne[1] != I->ffn || (int)L->down->ne[0] != I->ffn) {
            snprintf(err, (size_t)cap, "Layer %d isn't laid out as expected", l); return 0;
        }
        I->weight_bytes += L->q->bytes + L->k->bytes + L->v->bytes + L->o->bytes + L->gate->bytes + L->up->bytes + L->down->bytes;
    }
    if (vocab_load(m, f, len, a, err, cap)) return 0;
    I->vocab = m->voc.n;
    if ((int)m->output->ne[1] != I->vocab || (int)m->tok_embd->ne[1] != I->vocab) { snprintf(err, (size_t)cap, "The vocabulary doesn't match the model"); return 0; }
    I->im_start = llm_special(m, "<|im_start|>");
    I->im_end = llm_special(m, "<|im_end|>");
    I->eot = llm_special(m, "<|endoftext|>");
    I->think = llm_special(m, "<think>");
    I->think_end = llm_special(m, "</think>");
    I->tool_call = llm_special(m, "<tool_call>");
    I->tool_call_end = llm_special(m, "</tool_call>");

    uint64_t used0 = a->used;
    int ffn = I->ffn, big = I->dim > qd ? I->dim : qd;
    m->k16 = llm_keys_f16;
    if (m->k16) m->kh = arena_alloc(a, 2ull * I->n_layer * (uint64_t)ctx * (uint64_t)kvd);
    m->kc = arena_alloc(a, m->k16 ? 64 : (uint64_t)I->n_layer * (uint64_t)ctx * (uint64_t)kvd);
    m->vc = arena_alloc(a, (uint64_t)I->n_layer * (uint64_t)ctx * (uint64_t)kvd);
    m->ks = arena_alloc(a, 4ull * I->n_layer * (uint64_t)ctx * (uint64_t)(kvd / 32));
    m->vs = arena_alloc(a, 4ull * I->n_layer * (uint64_t)ctx * (uint64_t)I->n_kv);
    m->w16 = arena_alloc(a, 2ull * I->n_head * (uint64_t)ctx);
    m->x = arena_alloc(a, 4ull * NB * (uint64_t)big);
    m->xb = arena_alloc(a, 4ull * NB * (uint64_t)big);
    m->xb2 = arena_alloc(a, 4ull * NB * (uint64_t)big);
    m->q = arena_alloc(a, 4ull * NB * (uint64_t)qd);
    m->k = arena_alloc(a, 4ull * NB * (uint64_t)kvd);
    m->v = arena_alloc(a, 4ull * NB * (uint64_t)kvd);
    m->hb = arena_alloc(a, 4ull * NB * (uint64_t)ffn);
    m->hb2 = arena_alloc(a, 4ull * NB * (uint64_t)ffn);
    m->att = arena_alloc(a, 4ull * I->n_head * (uint64_t)ctx);
    m->logits = arena_alloc(a, 4ull * (uint64_t)I->vocab);
    m->aq = arena_alloc(a, sizeof(aq_t) * NB * (uint64_t)(ffn > big ? ffn : big) / 32);
    m->rope_cos = arena_alloc(a, 4ull * (uint64_t)ctx * (uint64_t)(I->head_dim / 2));
    m->rope_sin = arena_alloc(a, 4ull * (uint64_t)ctx * (uint64_t)(I->head_dim / 2));
    if (!m->kc || (m->k16 && !m->kh) || !m->vc || !m->ks || !m->vs || !m->w16 || !m->x || !m->xb || !m->xb2 || !m->q || !m->k || !m->v || !m->hb || !m->hb2 || !m->att ||
        !m->logits || !m->aq || !m->rope_cos || !m->rope_sin) {
        snprintf(err, (size_t)cap, "Not enough memory for %d tokens of context", ctx); return 0;
    }
    I->state_bytes = a->used - used0;
    int half = I->head_dim / 2;
    for (int p = 0; p < ctx; p++)
        for (int i = 0; i < half; i++) {
            double th = (double)p * pow((double)I->rope_base, -2.0 * i / I->head_dim);
            m->rope_cos[p * half + i] = (float)cos(th);
            m->rope_sin[p * half + i] = (float)sin(th);
        }
    m->B = NB;
    m->par = serial; m->nparts = 1;
    return m;
}

/* ---- rearranging the weights, once, across the cores ----------------------------------------- */
typedef struct { llm_t *m; uint8_t *tmp[4]; uint8_t *pick; } rp_t;

static void repack_part(void *arg, int part, int nparts) {
    rp_t *r = arg;
    llm_t *m = r->m;
    for (int i = 0; i < m->nt; i++) {
        if (!r->pick[i]) continue;
        gtensor_t *t = &m->t[i];
        int n = (int)t->ne[0], groups = (int)t->ne[1] / 4;
        int g0 = (int)((int64_t)groups * part / nparts), g1 = (int)((int64_t)groups * (part + 1) / nparts);
        uint64_t gb = 4 * row_bytes(t->type, n);
        repack_x4(t->type, (uint8_t *)t->data + gb * (uint64_t)g0, (g1 - g0) * 4, n, r->tmp[part & 3]);
    }
}

/* Q6_K's six-bit weights cost an A53 more to unpack than to multiply: the
 * output matrix (the biggest one, read for every token) goes to Q8_0
 * instead, which is finer than Q6_K, so nothing is lost -- if there's room
 * for it, a third more bytes, in the arena. */
typedef struct { const gtensor_t *src; uint8_t *dst; } rq_t;
static void requant_part(void *arg, int part, int nparts) {
    rq_t *r = arg;
    int n = (int)r->src->ne[0], rows = (int)r->src->ne[1];
    int r0 = (int)((int64_t)rows * part / nparts), r1 = (int)((int64_t)rows * (part + 1) / nparts);
    uint64_t sb = row_bytes(r->src->type, n), db = row_bytes(GT_Q8_0, n);
    float row[8192];
    for (int i = r0; i < r1; i++) {
        dequant_row(r->src->type, r->src->data + sb * (uint64_t)i, row, n);
        uint8_t *o = r->dst + db * (uint64_t)i;
        for (int b = 0; b < n / 32; b++) {
            const float *v = row + 32 * b;
            float amax = 0;
            for (int k = 0; k < 32; k++) { float x = v[k] < 0 ? -v[k] : v[k]; if (x > amax) amax = x; }
            float d = amax / 127.0f, id = d ? 1.0f / d : 0;
            __fp16 h = (__fp16)d;
            memcpy(o + 34 * b, &h, 2);
            for (int k = 0; k < 32; k++) { float q = v[k] * id; o[34 * b + 2 + k] = (uint8_t)(int8_t)(q < 0 ? q - 0.5f : q + 0.5f); }
        }
    }
}

int llm_prepare(llm_t *m, arena_t *a) {
    gtensor_t *out = (gtensor_t *)m->output;
    if (out->type == GT_Q6_K && out->ne[0] <= 8192) {
        uint64_t bytes = row_bytes(GT_Q8_0, (int)out->ne[0]) * out->ne[1];
        uint8_t *dst = arena_alloc(a, bytes);
        if (dst) {
            rq_t q = { out, dst };
            m->par(requant_part, &q);
            out->data = dst; out->type = GT_Q8_0; out->bytes = bytes;
            m->info.weight_bytes += bytes - row_bytes(GT_Q6_K, (int)out->ne[0]) * out->ne[1];
        }
    }
    rp_t r = { m, { 0 }, 0 };
    r.pick = arena_alloc(a, (uint64_t)m->nt);
    uint64_t most = 0;
    if (!r.pick) return -1;
    for (int i = 0; i < m->nt; i++) {
        gtensor_t *t = &m->t[i];
        int weight = t->nd == 2 && (t != m->tok_embd || m->output == m->tok_embd);
        r.pick[i] = weight && x4_able(t->type, (int)t->ne[1]);
        if (r.pick[i] && row_bytes(t->type, (int)t->ne[0]) > most) most = row_bytes(t->type, (int)t->ne[0]);
    }
    for (int k = 0; k < 4; k++) if (!(r.tmp[k] = arena_alloc(a, 4 * most))) return -1;
    m->par(repack_part, &r);
    for (int i = 0; i < m->nt; i++) if (r.pick[i]) m->t[i].type |= GT_X4;
    return 0;
}

const llm_info_t *llm_info(const llm_t *m) { return &m->info; }
void llm_set_par(llm_t *m, par_run run, int nparts) { m->par = run ? run : serial; m->nparts = run ? nparts : 1; }
llm_stats_t *llm_stats(llm_t *m) { return &m->st; }
void llm_reset(llm_t *m) { m->pos = 0; }
int  llm_pos(const llm_t *m) { return m->pos; }
void llm_rewind(llm_t *m, int pos) { if (pos >= 0 && pos < m->pos) m->pos = pos; }
float *llm_logits(llm_t *m) { return m->logits; }

/* ---- the pieces ----------------------------------------------------------------------- */
/* exp(x) for four lanes: 2^(x log2 e), the fraction by a polynomial. */
static inline float32x4_t vexp(float32x4_t x) {
    x = vminq_f32(vmaxq_f32(x, vdupq_n_f32(-87.0f)), vdupq_n_f32(88.0f));
    float32x4_t t = vmulq_n_f32(x, 1.44269504f);
    float32x4_t fl = vrndmq_f32(t);
    float32x4_t f = vsubq_f32(t, fl);                              /* 0..1 */
    float32x4_t p = vdupq_n_f32(1.535336188e-4f);
    p = vfmaq_f32(vdupq_n_f32(1.339887440e-3f), p, f);
    p = vfmaq_f32(vdupq_n_f32(9.618437357e-3f), p, f);
    p = vfmaq_f32(vdupq_n_f32(5.550332471e-2f), p, f);
    p = vfmaq_f32(vdupq_n_f32(2.402264791e-1f), p, f);
    p = vfmaq_f32(vdupq_n_f32(6.931472028e-1f), p, f);
    p = vfmaq_f32(vdupq_n_f32(1.0f), p, f);
    int32x4_t e = vshlq_n_s32(vaddq_s32(vcvtq_s32_f32(fl), vdupq_n_s32(127)), 23);
    return vmulq_f32(p, vreinterpretq_f32_s32(e));
}

static void rmsnorm(float *o, const float *x, const float *w, int n, float eps) {
    float32x4_t s4 = vdupq_n_f32(0);
    for (int i = 0; i < n; i += 4) { float32x4_t v = vld1q_f32(x + i); s4 = vfmaq_f32(s4, v, v); }
    float s = 1.0f / sqrtf(vaddvq_f32(s4) / (float)n + eps);
    for (int i = 0; i < n; i += 4) vst1q_f32(o + i, vmulq_f32(vmulq_n_f32(vld1q_f32(x + i), s), vld1q_f32(w + i)));
}

static void softmax(float *x, int n) {
    float mx = x[0];
    for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    float32x4_t s4 = vdupq_n_f32(0);
    int i = 0;
    for (; i + 4 <= n; i += 4) { float32x4_t e = vexp(vsubq_f32(vld1q_f32(x + i), vdupq_n_f32(mx))); vst1q_f32(x + i, e); s4 = vaddq_f32(s4, e); }
    float s = vaddvq_f32(s4);
    for (; i < n; i++) { x[i] = expf(x[i] - mx); s += x[i]; }
    float inv = 1.0f / s;
    for (i = 0; i < n; i++) x[i] *= inv;
}

/* ---- products, over the cores ------------------------------------------------------------ */
/* Up to three matrices with the same input (q, k and v; gate and up): one
 * quantized input, one trip to the cores. Each matrix's rows are split
 * over the cores on their own. */
typedef struct {
    int nw;
    const gtensor_t *w[3];
    float *y[3]; int y_ld[3];
    int B, n;
    const aq_t *xq; const float *xf; int xf_ld;
} mm_t;

static void mm_one(const mm_t *j, int k, int part, int nparts) {
    const gtensor_t *w = j->w[k];
    int nb = j->n / 32, rows = (int)w->ne[1];
    float *y = j->y[k]; int y_ld = j->y_ld[k];
    if (w->type & GT_X4) {                                       /* four rows at a time */
        int groups = rows / 4;
        int g0 = (int)((int64_t)groups * part / nparts), g1 = (int)((int64_t)groups * (part + 1) / nparts);
        uint64_t gb = 4 * row_bytes(w->type & 0xFF, j->n);
        const uint8_t *g = w->data + gb * (uint64_t)g0;
        for (int gi = g0; gi < g1; gi++, g += gb)
            for (int b = 0; b < j->B; b += 4) {
                int kk = j->B - b < 4 ? j->B - b : 4;
                const aq_t *xs[4] = { j->xq + nb * b, j->xq + nb * (b + 1), j->xq + nb * (b + 2), j->xq + nb * (b + 3) };
                float o[4][4];
                mm_x4(w->type, g, j->n, xs, kk, o);
                for (int q = 0; q < kk; q++) memcpy(y + (int64_t)y_ld * (b + q) + 4 * gi, o[q], 16);
            }
        return;
    }
    int r0 = (int)((int64_t)rows * part / nparts), r1 = (int)((int64_t)rows * (part + 1) / nparts);
    uint64_t rb = row_bytes(w->type, j->n);
    const uint8_t *row = w->data + rb * (uint64_t)r0;
    for (int r = r0; r < r1; r++, row += rb) {
        int b = 0;
        for (; b + 4 <= j->B; b += 4) {
            const aq_t *xs[4] = { j->xq + nb * b, j->xq + nb * (b + 1), j->xq + nb * (b + 2), j->xq + nb * (b + 3) };
            const float *xfs[4] = { j->xf + j->xf_ld * b, j->xf + j->xf_ld * (b + 1), j->xf + j->xf_ld * (b + 2), j->xf + j->xf_ld * (b + 3) };
            float o[4];
            dot_row4(w->type, row, xs, xfs, j->n, o);
            for (int q = 0; q < 4; q++) y[(int64_t)y_ld * (b + q) + r] = o[q];
        }
        for (; b < j->B; b++) y[(int64_t)y_ld * b + r] = dot_row(w->type, row, j->xq + nb * b, j->xf + j->xf_ld * b, j->n);
    }
}

static void mm_part(void *arg, int part, int nparts) {
    const mm_t *j = arg;
    for (int k = 0; k < j->nw; k++) mm_one(j, k, part, nparts);
}

/* y_k[b] = W_k . x[b] for b < B, k < nw; x rows are x_ld apart. */
static void matmul_n(llm_t *m, int nw, const gtensor_t *const *w, float *const *y, const int *y_ld, int B, const float *x, int x_ld) {
    uint64_t t0 = llm_now_ns();
    int n = (int)w[0]->ne[0];
    if ((w[0]->type & 0xFF) != GT_F32 && (w[0]->type & 0xFF) != GT_F16)
        for (int b = 0; b < B; b++) quantize_act(x + (int64_t)x_ld * b, n, m->aq + (n / 32) * b);
    mm_t j = { nw, { 0 }, { 0 }, { 0 }, B, n, m->aq, x, x_ld };
    for (int k = 0; k < nw; k++) { j.w[k] = w[k]; j.y[k] = y[k]; j.y_ld[k] = y_ld[k]; }
    uint64_t t1 = llm_now_ns();
    m->par(mm_part, &j);
    m->st.matmul_ns += llm_now_ns() - t0;
    m->st.quant_ns += t1 - t0;
    m->st.calls++;
}

static void matmul(llm_t *m, const gtensor_t *w, int B, const float *x, int x_ld, float *y, int y_ld) {
    matmul_n(m, 1, &w, &y, &y_ld, B, x, x_ld);
}

/* Matrices that can go together: the same input size, and the same kind
 * of input (quantized, or not). */
static int same_input(const gtensor_t *a, const gtensor_t *b) {
    int fa = (a->type & 0xFF) == GT_F32 || (a->type & 0xFF) == GT_F16, fb = (b->type & 0xFF) == GT_F32 || (b->type & 0xFF) == GT_F16;
    return a->ne[0] == b->ne[0] && fa == fb;
}

static void add_bias(float *y, const gtensor_t *b, int n) {
    if (!b) return;
    const float *bb = (const float *)b->data;
    for (int i = 0; i < n; i++) y[i] += bb[i];
}

static void rope(const llm_t *m, float *v, int heads, int pos) {
    int hd = m->info.head_dim, half = hd / 2;
    const float *c = m->rope_cos + pos * half, *s = m->rope_sin + pos * half;
    for (int h = 0; h < heads; h++) {
        float *x = v + h * hd;
        if (m->rope_neox)
            for (int i = 0; i < half; i++) {
                float a = x[i], b = x[i + half];
                x[i] = a * c[i] - b * s[i]; x[i + half] = a * s[i] + b * c[i];
            }
        else
            for (int i = 0; i < half; i++) {
                float a = x[2 * i], b = x[2 * i + 1];
                x[2 * i] = a * c[i] - b * s[i]; x[2 * i + 1] = a * s[i] + b * c[i];
            }
    }
}

/* A vector as 8-bit numbers and a scale: the cache's keys and values, and
 * a head of q before it meets the keys. */
static float q8_row(const float *x, int n, int8_t *q) {
    float32x4_t mx = vdupq_n_f32(0);
    for (int i = 0; i < n; i += 4) mx = vmaxq_f32(mx, vabsq_f32(vld1q_f32(x + i)));
    float d = vmaxvq_f32(mx) / 127.0f, id = d ? 1.0f / d : 0.0f;
    for (int i = 0; i < n; i += 16) {
        int32x4_t a = vcvtnq_s32_f32(vmulq_n_f32(vld1q_f32(x + i), id)), b = vcvtnq_s32_f32(vmulq_n_f32(vld1q_f32(x + i + 4), id));
        int32x4_t c = vcvtnq_s32_f32(vmulq_n_f32(vld1q_f32(x + i + 8), id)), e = vcvtnq_s32_f32(vmulq_n_f32(vld1q_f32(x + i + 12), id));
        vst1q_s8(q + i, vcombine_s8(vmovn_s16(vcombine_s16(vmovn_s32(a), vmovn_s32(b))), vmovn_s16(vcombine_s16(vmovn_s32(c), vmovn_s32(e)))));
    }
    return d;
}

static inline int8x16_t ld8(const int8_t *p) { return vld1q_s8(p); }
static inline int16x8_t mul8(int8x16_t a, int8x16_t b) { return vmlal_high_s8(vmull_s8(vget_low_s8(a), vget_low_s8(b)), a, b); }

/* Attention for B tokens at positions pos0.., heads split over the cores.
 * The cache is 8-bit, a scale for each head of each position, so both
 * halves run on the integer multiplies: q . k as 8 x 8 bits, and the
 * values weighted by the softmax as 16-bit fractions of its total. */
typedef struct { llm_t *m; int l, B, pos0; } at_t;

static void attn_part(void *arg, int part, int nparts) {
    const at_t *j = arg;
    llm_t *m = j->m;
    const llm_info_t *I = &m->info;
    int hd = I->head_dim, nkv = I->n_kv, kvd = nkv * hd, qd = I->n_head * hd, grp = I->n_head / nkv;
    int h0 = I->n_head * part / nparts, h1 = I->n_head * (part + 1) / nparts;
    const int8_t *kc = m->kc + (uint64_t)j->l * I->ctx * kvd, *vc = m->vc + (uint64_t)j->l * I->ctx * kvd;
    const float *ks = m->ks + (uint64_t)j->l * I->ctx * (kvd / 32), *vs = m->vs + (uint64_t)j->l * I->ctx * nkv;
    float scale = 1.0f / sqrtf((float)hd);
    for (int b = 0; b < j->B; b++) {
        int n = j->pos0 + b + 1;
        for (int h = h0; h < h1; h++) {
            const float *q = m->q + (int64_t)qd * b + h * hd;
            float *att = m->att + (int64_t)h * I->ctx;
            int16_t *w16 = m->w16 + (int64_t)h * I->ctx;
            int kh = h / grp;
            /* q, like the keys, in blocks of 32 with a scale each: the blocks'
             * integer sums come out side by side in one vector, which meets
             * the vector of their scales */
            int8_t qq[256] __attribute__((aligned(16)));
            float qsc[8] __attribute__((aligned(16)));
            int nb = hd / 32;
            for (int c = 0; c < nb; c++) qsc[c] = q8_row(q + 32 * c, 32, qq + 32 * c) * scale;
            const int8_t *k = kc + kh * hd;
            const float *ksh = ks + kh * nb;
            if (m->k16) {
                const __fp16 *kf = m->kh + (uint64_t)j->l * I->ctx * kvd + kh * hd;
                for (int t = 0; t < n; t++, kf += kvd) {
                    float32x4_t s0 = vdupq_n_f32(0), s1 = s0, s2 = s0, s3 = s0;
                    for (int i = 0; i < hd; i += 16) {
                        s0 = vfmaq_f32(s0, vld1q_f32(q + i), vcvt_f32_f16(vld1_f16(kf + i)));
                        s1 = vfmaq_f32(s1, vld1q_f32(q + i + 4), vcvt_f32_f16(vld1_f16(kf + i + 4)));
                        s2 = vfmaq_f32(s2, vld1q_f32(q + i + 8), vcvt_f32_f16(vld1_f16(kf + i + 8)));
                        s3 = vfmaq_f32(s3, vld1q_f32(q + i + 12), vcvt_f32_f16(vld1_f16(kf + i + 12)));
                    }
                    att[t] = vaddvq_f32(vaddq_f32(vaddq_f32(s0, s1), vaddq_f32(s2, s3))) * scale;
                }
            } else if (hd == 128) {
                int8x16_t q0 = ld8(qq), q1 = ld8(qq + 16), q2 = ld8(qq + 32), q3 = ld8(qq + 48);
                int8x16_t q4 = ld8(qq + 64), q5 = ld8(qq + 80), q6 = ld8(qq + 96), q7 = ld8(qq + 112);
                float32x4_t qv = vld1q_f32(qsc);
                for (int t = 0; t < n; t++, k += kvd) {
                    int32x4_t b0 = vpadalq_s16(vpaddlq_s16(mul8(q0, ld8(k))), mul8(q1, ld8(k + 16)));
                    int32x4_t b1 = vpadalq_s16(vpaddlq_s16(mul8(q2, ld8(k + 32))), mul8(q3, ld8(k + 48)));
                    int32x4_t b2 = vpadalq_s16(vpaddlq_s16(mul8(q4, ld8(k + 64))), mul8(q5, ld8(k + 80)));
                    int32x4_t b3 = vpadalq_s16(vpaddlq_s16(mul8(q6, ld8(k + 96))), mul8(q7, ld8(k + 112)));
                    int32x4_t s4 = vpaddq_s32(vpaddq_s32(b0, b1), vpaddq_s32(b2, b3));
                    float32x4_t f = vmulq_f32(vmulq_f32(vcvtq_f32_s32(s4), qv), vld1q_f32(ksh + (uint64_t)t * (kvd / 32)));
                    att[t] = vaddvq_f32(f);
                }
            } else {
                for (int t = 0; t < n; t++, k += kvd) {
                    float sum = 0;
                    for (int c = 0; c < nb; c++) {
                        int32x4_t a0 = vpadalq_s16(vpaddlq_s16(mul8(ld8(qq + 32 * c), ld8(k + 32 * c))), mul8(ld8(qq + 32 * c + 16), ld8(k + 32 * c + 16)));
                        sum += (float)vaddvq_s32(a0) * qsc[c] * ksh[(uint64_t)t * (kvd / 32) + c];
                    }
                    att[t] = sum;
                }
            }
            softmax(att, n);
            /* each position's weight, with its value's scale, as a share of
             * 32767: the sum stays inside 32 bits whatever the length */
            float tot = 0;
            for (int t = 0; t < n; t++) { att[t] *= vs[(uint64_t)t * nkv + kh]; tot += att[t]; }
            float inv = tot > 0 ? 32767.0f / tot : 0;
            for (int t = 0; t < n; t++) w16[t] = (int16_t)(att[t] * inv + 0.5f);
            float back = tot / 32767.0f;
            float *o = m->xb + (int64_t)qd * b + h * hd;
            for (int c = 0; c < hd; c += 64) {
                int32x4_t a[16];
                for (int i = 0; i < 16; i++) a[i] = vdupq_n_s32(0);
                const int8_t *v = vc + kh * hd + c;
                for (int t = 0; t < n; t++, v += kvd) {
                    int16_t w = w16[t];
                    if (!w) continue;
                    for (int r = 0; r < 4; r++) {
                        int8x16_t x = ld8(v + 16 * r);
                        int16x8_t lo = vmovl_s8(vget_low_s8(x)), hi = vmovl_high_s8(x);
                        a[4 * r]     = vmlal_n_s16(a[4 * r], vget_low_s16(lo), w);
                        a[4 * r + 1] = vmlal_high_n_s16(a[4 * r + 1], lo, w);
                        a[4 * r + 2] = vmlal_n_s16(a[4 * r + 2], vget_low_s16(hi), w);
                        a[4 * r + 3] = vmlal_high_n_s16(a[4 * r + 3], hi, w);
                    }
                }
                for (int i = 0; i < 16; i++) vst1q_f32(o + c + 4 * i, vmulq_n_f32(vcvtq_f32_s32(a[i]), back));
            }
        }
    }
}

/* B tokens at pos .. pos+B-1; with `last`, the logits for the last one. */
static void forward(llm_t *m, const int *tok, int B, int last) {
    const llm_info_t *I = &m->info;
    int dim = I->dim, hd = I->head_dim, qd = I->n_head * hd, kvd = I->n_kv * hd, ffn = I->ffn;
    int big = dim > qd ? dim : qd;
    uint64_t t0 = llm_now_ns();
    for (int b = 0; b < B; b++) {
        const gtensor_t *e = m->tok_embd;
        dequant_any(e->type, e->data, tok[b], dim, m->x + (int64_t)big * b);
    }
    for (int l = 0; l < I->n_layer; l++) {
        const layer_t *L = &m->L[l];
        for (int b = 0; b < B; b++) rmsnorm(m->xb + big * b, m->x + big * b, (const float *)L->attn_norm->data, dim, I->eps);
        if (same_input(L->q, L->k) && same_input(L->q, L->v)) {
            const gtensor_t *w[3] = { L->q, L->k, L->v };
            float *y[3] = { m->q, m->k, m->v };
            int ld[3] = { qd, kvd, kvd };
            matmul_n(m, 3, w, y, ld, B, m->xb, big);
        } else {
            matmul(m, L->q, B, m->xb, big, m->q, qd);
            matmul(m, L->k, B, m->xb, big, m->k, kvd);
            matmul(m, L->v, B, m->xb, big, m->v, kvd);
        }
        uint64_t t1 = llm_now_ns();
        for (int b = 0; b < B; b++) {
            float *q = m->q + qd * b, *k = m->k + kvd * b, *v = m->v + kvd * b;
            add_bias(q, L->q_bias, qd); add_bias(k, L->k_bias, kvd); add_bias(v, L->v_bias, kvd);
            if (L->q_norm) for (int h = 0; h < I->n_head; h++) rmsnorm(q + h * hd, q + h * hd, (const float *)L->q_norm->data, hd, I->eps);
            if (L->k_norm) for (int h = 0; h < I->n_kv; h++) rmsnorm(k + h * hd, k + h * hd, (const float *)L->k_norm->data, hd, I->eps);
            rope(m, q, I->n_head, m->pos + b);
            rope(m, k, I->n_kv, m->pos + b);
            uint64_t at = (uint64_t)l * I->ctx + (uint64_t)(m->pos + b);
            if (m->k16) for (int i = 0; i < kvd; i += 4) vst1_f16(m->kh + at * kvd + i, vcvt_f16_f32(vld1q_f32(k + i)));
            else for (int c = 0; c < kvd / 32; c++) m->ks[at * (kvd / 32) + c] = q8_row(k + 32 * c, 32, m->kc + at * kvd + 32 * c);
            for (int h = 0; h < I->n_kv; h++) m->vs[at * I->n_kv + h] = q8_row(v + h * hd, hd, m->vc + at * kvd + h * hd);
        }
        at_t aj = { m, l, B, m->pos };
        uint64_t t2 = llm_now_ns();
        m->par(attn_part, &aj);
        m->st.attn_par_ns += llm_now_ns() - t2;
        m->st.attn_ns += llm_now_ns() - t1;
        matmul(m, L->o, B, m->xb, qd, m->xb2, big);
        for (int b = 0; b < B; b++) for (int i = 0; i < dim; i++) m->x[big * b + i] += m->xb2[big * b + i];
        for (int b = 0; b < B; b++) rmsnorm(m->xb + big * b, m->x + big * b, (const float *)L->ffn_norm->data, dim, I->eps);
        if (same_input(L->gate, L->up)) {
            const gtensor_t *w[2] = { L->gate, L->up };
            float *y[2] = { m->hb, m->hb2 };
            int ld[2] = { ffn, ffn };
            matmul_n(m, 2, w, y, ld, B, m->xb, big);
        } else {
            matmul(m, L->gate, B, m->xb, big, m->hb, ffn);
            matmul(m, L->up, B, m->xb, big, m->hb2, ffn);
        }
        for (int b = 0; b < B; b++) {
            float *g = m->hb + ffn * b, *u = m->hb2 + ffn * b;
            for (int i = 0; i < ffn; i += 4) {
                float32x4_t x = vld1q_f32(g + i);
                float32x4_t sg = vdivq_f32(x, vaddq_f32(vdupq_n_f32(1.0f), vexp(vnegq_f32(x))));
                vst1q_f32(g + i, vmulq_f32(sg, vld1q_f32(u + i)));
            }
        }
        matmul(m, L->down, B, m->hb, ffn, m->xb2, big);
        for (int b = 0; b < B; b++) for (int i = 0; i < dim; i++) m->x[big * b + i] += m->xb2[big * b + i];
    }
    if (last) {
        float *xl = m->x + big * (B - 1);
        rmsnorm(xl, xl, (const float *)m->out_norm->data, dim, I->eps);
        matmul(m, m->output, 1, xl, big, m->logits, I->vocab);
    }
    m->pos += B;
    m->st.tokens += (uint64_t)B;
    m->st.other_ns += llm_now_ns() - t0;
}

int llm_feed(llm_t *m, const int *toks, int n, volatile int *stop) {
    int done = 0;
    while (done < n) {
        if (stop && *stop) break;
        if (m->pos >= m->info.ctx) break;
        int b = n - done < NB ? n - done : NB;
        if (m->pos + b > m->info.ctx) b = m->info.ctx - m->pos;
        forward(m, toks + done, b, done + b == n);
        done += b;
    }
    return done;
}

/* ---- choosing the next token --------------------------------------------------------------- */
static uint32_t rnd(sampler_t *s) {
    s->seed ^= s->seed >> 12; s->seed ^= s->seed << 25; s->seed ^= s->seed >> 27;
    return (uint32_t)((s->seed * 0x2545F4914F6CDD1Dull) >> 32);
}

int llm_sample(llm_t *m, sampler_t *s, const int *recent, int nrecent) {
    float *lg = m->logits;
    int V = m->info.vocab;
    int from = nrecent > s->rep_last ? nrecent - s->rep_last : 0;
    for (int i = from; i < nrecent; i++) {                           /* once per distinct token */
        int t = recent[i], seen = 0;
        for (int j = from; j < i; j++) if (recent[j] == t) { seen = 1; break; }
        if (seen || t < 0 || t >= V) continue;
        lg[t] = lg[t] > 0 ? lg[t] / s->rep_pen : lg[t] * s->rep_pen;
    }
    enum { K = 64 };
    int k = s->top_k > 0 && s->top_k < K ? s->top_k : K;
    int ids[K]; float vals[K]; int n = 0;
    for (int i = 0; i < V; i++) {
        float v = lg[i];
        if (n == k && v <= vals[n - 1]) continue;
        int j = n < k ? n++ : k - 1;
        while (j > 0 && vals[j - 1] < v) { vals[j] = vals[j - 1]; ids[j] = ids[j - 1]; j--; }
        vals[j] = v; ids[j] = i;
    }
    if (s->temp <= 0.0f || n == 1) return ids[0];
    float p[K], sum = 0;
    for (int i = 0; i < n; i++) { p[i] = expf((vals[i] - vals[0]) / s->temp); sum += p[i]; }
    float cut = s->top_p * sum, acc = 0;
    int keep = n;
    for (int i = 0; i < n; i++) { acc += p[i]; if (acc >= cut) { keep = i + 1; break; } }
    float tot = 0;
    for (int i = 0; i < keep; i++) tot += p[i];
    float r = (float)(rnd(s) >> 8) / 16777216.0f * tot;
    for (int i = 0; i < keep; i++) { r -= p[i]; if (r <= 0) return ids[i]; }
    return ids[keep - 1];
}
