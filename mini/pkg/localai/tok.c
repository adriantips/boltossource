/* ===========================================================================
 *  BoltOS Local AI  -  tok.c
 *
 *  The tokenizer these models share: GPT-2's byte-level BPE. Text is cut
 *  into words by a pattern (each model family has its own -- Qwen2 keeps
 *  every digit apart, SmolLM splits digits off first too), every byte of a
 *  word becomes its one-character token, and the pair whose merge comes
 *  earliest in the file's list is joined, again and again, until no pair
 *  can be. The vocabulary and the merges stay in the file; two hash tables
 *  over them are all that's built.
 *
 *  The pattern's \p{L} and \p{N} need Unicode classes; what's here knows
 *  ASCII exactly and the rest by ranges, which is right for the letters,
 *  digits and punctuation of most languages.
 * ===========================================================================*/
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "llm_int.h"

/* ---- GPT-2's byte <-> character mapping --------------------------------------------- */
static int printable(int b) { return (b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255); }
static int byte_cp(int b) {
    if (printable(b)) return b;
    int n = 0;
    for (int k = 0; k < b; k++) if (!printable(k)) n++;
    return 256 + n;
}

static int u8enc(uint32_t c, uint8_t *o) {
    if (c < 0x80) { o[0] = (uint8_t)c; return 1; }
    if (c < 0x800) { o[0] = (uint8_t)(0xC0 | c >> 6); o[1] = (uint8_t)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (uint8_t)(0xE0 | c >> 12); o[1] = (uint8_t)(0x80 | (c >> 6 & 63)); o[2] = (uint8_t)(0x80 | (c & 63)); return 3; }
    o[0] = (uint8_t)(0xF0 | c >> 18); o[1] = (uint8_t)(0x80 | (c >> 12 & 63)); o[2] = (uint8_t)(0x80 | (c >> 6 & 63)); o[3] = (uint8_t)(0x80 | (c & 63));
    return 4;
}

static uint32_t u8dec(const uint8_t *s, int n, int *used) {
    uint8_t c = s[0];
    int k = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (k > n) k = 1;
    uint32_t v = k == 1 ? c : k == 2 ? (c & 31u) : k == 3 ? (c & 15u) : (c & 7u);
    for (int i = 1; i < k; i++) {
        if ((s[i] & 0xC0) != 0x80) { *used = 1; return c; }
        v = v << 6 | (s[i] & 63u);
    }
    *used = k;
    return v;
}

/* ---- hashing ---------------------------------------------------------------------------- */
static uint32_t hash_bytes(const uint8_t *s, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ s[i]) * 16777619u;
    return h;
}
static uint32_t hash64(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; return (uint32_t)k; }

static int vocab_find(const vocab_t *v, const uint8_t *s, int n) {
    for (uint32_t h = hash_bytes(s, n) & v->vmask;; h = (h + 1) & v->vmask) {
        uint32_t e = v->vh[h];
        if (!e) return -1;
        int id = (int)e - 1;
        if (v->len[id] == n && !memcmp(v->str[id], s, (size_t)n)) return id;
    }
}

/* rank of merging a then b, and what it makes; -1 if they don't merge */
static int merge_find(const vocab_t *v, int a, int b, int *res) {
    uint64_t key = (uint64_t)(uint32_t)a << 32 | (uint32_t)b;
    for (uint32_t h = hash64(key) & v->mmask;; h = (h + 1) & v->mmask) {
        if (v->mv[h] == 0) return -1;
        if (v->mk[h] == key) { int r = (int)v->mv[h] - 1; *res = (int)v->mres[r]; return r; }
    }
}

/* ---- the GGUF arrays ---------------------------------------------------------------------- */
const uint8_t *gguf_array(const uint8_t *file, uint64_t len, const char *key, uint32_t *etype, uint64_t *n);
const uint8_t *gguf_value(const uint8_t *file, uint64_t len, const char *key, uint32_t *type);

int vocab_load(llm_t *m, const uint8_t *file, uint64_t len, arena_t *a, char *err, int cap) {
    vocab_t *v = &m->voc;
    uint32_t et, t;
    uint64_t n, nm, nt;
    const uint8_t *toks = gguf_array(file, len, "tokenizer.ggml.tokens", &et, &n);
    const uint8_t *mer = gguf_array(file, len, "tokenizer.ggml.merges", &et, &nm);
    const uint8_t *typ = gguf_array(file, len, "tokenizer.ggml.token_type", &t, &nt);
    const uint8_t *model = gguf_value(file, len, "tokenizer.ggml.model", &t);
    const uint8_t *pre = gguf_value(file, len, "tokenizer.ggml.pre", &t);
    if (!toks || !mer || !model) { snprintf(err, (size_t)cap, "The file has no BPE tokenizer"); return -1; }
    uint64_t ml; memcpy(&ml, model, 8);
    if (ml != 4 || memcmp(model + 8, "gpt2", 4)) { snprintf(err, (size_t)cap, "Its tokenizer isn't GPT-2 style BPE"); return -1; }
    v->pre = PRE_GPT2;
    if (pre) {
        uint64_t pl; memcpy(&pl, pre, 8);
        if (pl == 5 && !memcmp(pre + 8, "qwen2", 5)) v->pre = PRE_QWEN2;
        else if (pl == 6 && !memcmp(pre + 8, "smollm", 6)) v->pre = PRE_SMOLLM;
    }
    v->n = (int)n;
    v->str = arena_alloc(a, 8 * n);
    v->len = arena_alloc(a, 2 * n);
    v->type = arena_alloc(a, n);
    uint32_t cap2 = 1; while (cap2 < 2 * n) cap2 <<= 1;
    v->vmask = cap2 - 1;
    v->vh = arena_alloc(a, 4ull * cap2);
    uint32_t mcap = 1; while (mcap < 2 * nm) mcap <<= 1;
    v->mmask = mcap - 1;
    v->mk = arena_alloc(a, 8ull * mcap);
    v->mv = arena_alloc(a, 4ull * mcap);
    v->mres = arena_alloc(a, 4 * nm + 4);
    if (!v->str || !v->len || !v->type || !v->vh || !v->mk || !v->mv || !v->mres) { snprintf(err, (size_t)cap, "Not enough memory for the tokenizer"); return -1; }
    memset(v->vh, 0, 4ull * cap2);
    memset(v->mv, 0, 4ull * mcap);
    const uint8_t *p = toks;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t l; memcpy(&l, p, 8);
        if (l > 65535 || p + 8 + l > file + len) { snprintf(err, (size_t)cap, "The tokenizer is damaged"); return -1; }
        v->str[i] = p + 8; v->len[i] = (uint16_t)l;
        p += 8 + l;
        v->type[i] = 1;
        if (typ && nt == n) { int32_t ty; memcpy(&ty, typ + 4 * i, 4); v->type[i] = (uint8_t)ty; }
        uint32_t h = hash_bytes(v->str[i], (int)l) & v->vmask;
        while (v->vh[h]) h = (h + 1) & v->vmask;
        v->vh[h] = (uint32_t)i + 1;
    }
    memset(v->cp2byte, 0, sizeof v->cp2byte);
    for (int b = 0; b < 256; b++) {
        uint8_t u[4];
        int cp = byte_cp(b), k = u8enc((uint32_t)cp, u);
        v->byte_tok[b] = vocab_find(v, u, k);
        if (cp < (int)sizeof v->cp2byte) v->cp2byte[cp] = (uint8_t)b;
    }
    p = mer;
    int nmerge = 0;
    for (uint64_t i = 0; i < nm; i++) {
        uint64_t l; memcpy(&l, p, 8);
        const uint8_t *s = p + 8;
        p += 8 + l;
        int sp = -1;
        for (int k = 0; k < (int)l; k++) if (s[k] == ' ') { sp = k; break; }
        if (sp <= 0) continue;
        int left = vocab_find(v, s, sp), right = vocab_find(v, s + sp + 1, (int)l - sp - 1);
        uint8_t both[512];
        if (l - 1 > sizeof both) continue;
        memcpy(both, s, (size_t)sp); memcpy(both + sp, s + sp + 1, (size_t)(l - sp - 1));
        int res = vocab_find(v, both, (int)l - 1);
        if (left < 0 || right < 0 || res < 0) continue;
        uint64_t key = (uint64_t)(uint32_t)left << 32 | (uint32_t)right;
        uint32_t h = hash64(key) & v->mmask;
        while (v->mv[h]) { if (v->mk[h] == key) break; h = (h + 1) & v->mmask; }
        if (v->mv[h]) continue;                                 /* the earlier rank wins */
        v->mk[h] = key; v->mv[h] = (uint32_t)i + 1; v->mres[i] = (uint32_t)res;
        nmerge++;
    }
    (void)nmerge;
    return 0;
}

/* ---- classes ------------------------------------------------------------------------------- */
static int is_space(uint32_t c) {
    return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}
static int is_num(uint32_t c) {
    if (c < 0x80) return c >= '0' && c <= '9';
    return c == 0xB2 || c == 0xB3 || c == 0xB9 || (c >= 0xBC && c <= 0xBE) || (c >= 0x660 && c <= 0x669) ||
           (c >= 0x6F0 && c <= 0x6F9) || (c >= 0x966 && c <= 0x96F) || (c >= 0x2070 && c <= 0x2079) ||
           (c >= 0x2080 && c <= 0x2089) || (c >= 0x2150 && c <= 0x2189) || (c >= 0x2460 && c <= 0x249B) ||
           (c >= 0x2776 && c <= 0x2793) || c == 0x3007 || (c >= 0x3021 && c <= 0x3029) || (c >= 0xFF10 && c <= 0xFF19);
}
static int is_letter(uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (c < 0xC0) return c == 0xAA || c == 0xB5 || c == 0xBA;
    if (c == 0xD7 || c == 0xF7) return 0;
    if (c < 0x2B0) return 1;                                     /* Latin */
    if (c >= 0x300 && c < 0x370) return 0;                       /* combining marks */
    if (is_space(c) || is_num(c)) return 0;
    if (c >= 0x2000 && c < 0x2C00) return (c >= 0x2090 && c <= 0x209C) || c == 0x2102 || c == 0x2107 ||
                                          (c >= 0x210A && c <= 0x2113) || c == 0x2115 || (c >= 0x2119 && c <= 0x211D) ||
                                          c == 0x2124 || c == 0x2126 || c == 0x2128 || (c >= 0x212A && c <= 0x212D) ||
                                          (c >= 0x212F && c <= 0x2139);
    if (c >= 0x3000 && c < 0x3040) return c == 0x3005 || c == 0x3006;
    if (c >= 0xFE00 && c < 0xFE10) return 0;                     /* variation selectors */
    if (c >= 0xFF00 && c < 0xFF66) return (c >= 0xFF21 && c <= 0xFF3A) || (c >= 0xFF41 && c <= 0xFF5A);
    if (c >= 0x1F000 && c < 0x1FB00) return 0;                   /* emoji, symbols */
    if (c >= 0xE000 && c < 0xF900) return 0;                     /* private use */
    if ((c >= 0x5B0 && c <= 0x5C7) || (c >= 0x610 && c <= 0x61A) || (c >= 0x64B && c <= 0x65F) ||
        (c >= 0x900 && c <= 0x903) || (c >= 0x93A && c <= 0x94F) || (c >= 0xE31 && c <= 0xE3A) ||
        (c >= 0xE47 && c <= 0xE4E) || c == 0x200D) return 0;     /* marks in common scripts */
    if (c == 0x60C || c == 0x61B || c == 0x61F || c == 0x6D4 || c == 0x964 || c == 0x965 || c == 0x5BE ||
        c == 0x5C0 || c == 0x5C3 || c == 0x5F3 || c == 0x5F4 || c == 0x37E || c == 0x387 || c == 0x55A ||
        (c >= 0x55B && c <= 0x55F) || c == 0x589) return 0;     /* punctuation in common scripts */
    return 1;
}

/* ---- cutting text into words ---------------------------------------------------------------- */
typedef struct { const uint8_t *s; int n; } span_t;
static uint32_t cp_at(const uint8_t *s, int n, int i, int *k) { if (i >= n) { *k = 0; return 0; } return u8dec(s + i, n - i, k); }

static int contraction(const uint8_t *s, int n, int i) {     /* (?i:'s|'t|'re|'ve|'m|'ll|'d) */
    if (i >= n || s[i] != '\'') return 0;
    int c1 = i + 1 < n ? (s[i + 1] | 32) : 0, c2 = i + 2 < n ? (s[i + 2] | 32) : 0;
    if (c1 == 's' || c1 == 't' || c1 == 'm' || c1 == 'd') return 2;
    if ((c1 == 'r' && c2 == 'e') || (c1 == 'v' && c2 == 'e') || (c1 == 'l' && c2 == 'l')) return 3;
    return 0;
}

/* The length of the run of characters from i that pass `f`. */
static int run(const uint8_t *s, int n, int i, int (*f)(uint32_t)) {
    int j = i, k;
    while (j < n) { uint32_t c = cp_at(s, n, j, &k); if (!f(c)) break; j += k; }
    return j - i;
}
static int not_slnr(uint32_t c) { return !is_space(c) && !is_letter(c) && !is_num(c); }

/* Whitespace: \s+(?!\S) then \s+ -- all of it at the end or before more
 * space, else all but the last character, which goes with the next word. */
static int ws_run(const uint8_t *s, int n, int i) {
    int len = run(s, n, i, is_space);
    if (!len) return 0;
    if (i + len >= n) return len;
    int last = i, k;
    for (int j = i; j < i + len; j += k) { cp_at(s, n, j, &k); last = j; }
    return last > i ? last - i : len;
}

/* Qwen2's pattern, one match from i. */
static int match_qwen2(const uint8_t *s, int n, int i) {
    int k, c0len, m = contraction(s, n, i);
    if (m) return m;
    uint32_t c0 = cp_at(s, n, i, &c0len);
    /* [^\r\n\p{L}\p{N}]?\p{L}+ */
    if (is_letter(c0)) return run(s, n, i, is_letter);
    if (c0 != '\r' && c0 != '\n' && !is_num(c0)) {
        int l = run(s, n, i + c0len, is_letter);
        if (l) return c0len + l;
    }
    if (is_num(c0)) return c0len;                                /* \p{N} */
    /* ` ?[^\s\p{L}\p{N}]+[\r\n]*` */
    int j = i;
    if (c0 == ' ') j += 1;
    int p = run(s, n, j, not_slnr);
    if (p) {
        j += p;
        while (j < n && (s[j] == '\r' || s[j] == '\n')) j++;
        return j - i;
    }
    /* \s*[\r\n]+ : the longest run of space that ends in a line break */
    int sp = run(s, n, i, is_space), end = 0;
    for (int q = i; q < i + sp; q += k) { cp_at(s, n, q, &k); if (s[q] == '\r' || s[q] == '\n') end = q + 1 - i; }
    if (end) return end;
    m = ws_run(s, n, i);
    return m ? m : c0len;
}

/* GPT-2's: 's|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+
 * (SmolLM's and the others' -- SmolLM with digits apart, done before). */
static int match_gpt2(const uint8_t *s, int n, int i, int digits_alone) {
    int k, c0len, m = contraction(s, n, i);
    if (m) return m;
    uint32_t c0 = cp_at(s, n, i, &c0len);
    int j = i + (c0 == ' ' ? 1 : 0);
    uint32_t c1 = cp_at(s, n, j, &k);
    if (j < n && is_letter(c1)) return j - i + run(s, n, j, is_letter);
    if (j < n && is_num(c1)) return j - i + (digits_alone ? k : run(s, n, j, is_num));
    if (j < n && not_slnr(c1)) return j - i + run(s, n, j, not_slnr);
    m = ws_run(s, n, i);
    return m ? m : c0len;
}

/* ---- one word through BPE ----------------------------------------------------------------------- */
static int bpe_word(const vocab_t *v, const uint8_t *w, int n, int *out, int cap) {
    enum { MAXW = 512 };
    int sym[MAXW], cnt = 0;
    for (int i = 0; i < n && cnt < MAXW; i++) sym[cnt++] = v->byte_tok[w[i]];   /* llm_encode keeps n <= MAXW */
    for (;;) {
        int best = -1, best_rank = 0x7fffffff, best_res = 0;
        for (int i = 0; i + 1 < cnt; i++) {
            int res, r = merge_find(v, sym[i], sym[i + 1], &res);
            if (r >= 0 && r < best_rank) { best_rank = r; best = i; best_res = res; }
        }
        if (best < 0) break;
        sym[best] = best_res;
        for (int i = best + 1; i + 1 < cnt; i++) sym[i] = sym[i + 1];
        cnt--;
    }
    int o = 0;
    for (int i = 0; i < cnt && o < cap; i++) if (sym[i] >= 0) out[o++] = sym[i];
    return o;
}

int llm_encode(llm_t *m, const char *text, int len, int *out, int cap) {
    const vocab_t *v = &m->voc;
    const uint8_t *s = (const uint8_t *)text;
    int n = len < 0 ? (int)strlen(text) : len, o = 0;
    for (int i = 0; i < n && o < cap;) {
        int l;
        if (v->pre == PRE_QWEN2) l = match_qwen2(s, n, i);
        else l = match_gpt2(s, n, i, v->pre == PRE_SMOLLM);
        if (l <= 0) l = 1;
        /* words longer than BPE's buffer go in pieces */
        for (int a = 0; a < l && o < cap; a += 512) {
            int k = l - a < 512 ? l - a : 512;
            o += bpe_word(v, s + i + a, k, out + o, cap - o);
        }
        i += l;
    }
    return o;
}

int llm_piece(llm_t *m, int tok, char *out, int cap) {
    const vocab_t *v = &m->voc;
    if (tok < 0 || tok >= v->n || cap < 1) return 0;
    const uint8_t *s = v->str[tok];
    int n = v->len[tok], o = 0;
    if (v->type[tok] == 3) return 0;                             /* control: nothing to show */
    if (v->type[tok] == 4) {                                     /* user-defined: as written */
        if (n > cap) n = cap;
        memcpy(out, s, (size_t)n);
        return n;
    }
    for (int i = 0; i < n && o < cap;) {
        int k;
        uint32_t c = u8dec(s + i, n - i, &k);
        i += k;
        if (c < sizeof v->cp2byte && (printable((int)c) || c >= 256)) out[o++] = (char)v->cp2byte[c];
        else out[o++] = '?';
    }
    return o;
}

int llm_special(llm_t *m, const char *text) {
    int id = vocab_find(&m->voc, (const uint8_t *)text, (int)strlen(text));
    return id;
}

int llm_is_control(llm_t *m, int tok) { return tok >= 0 && tok < m->voc.n && m->voc.type[tok] == 3; }
