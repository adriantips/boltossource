/* BoltOS Local AI - test/llmtest.c: the engine on a Mac (or any AArch64
 * machine with a C library), against the same GGUF files the Pi gets.
 *
 *   cc -O2 -o llmtest test/llmtest.c llm.c tok.c kern.c -lpthread
 *   ./llmtest MODEL.gguf kern                 kernels vs. the plain C ones
 *   ./llmtest MODEL.gguf tok "some text"      token ids, one per line
 *   ./llmtest MODEL.gguf logits "text"        the top 10 logits after the text
 *   ./llmtest MODEL.gguf chat "question" [N]  a reply, greedy, N tokens at most
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <math.h>
#include "../llm_int.h"

uint64_t llm_now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ---- a few threads, the way the Pi's cores are used: spin on a counter ---- */
#define NT 4
static pthread_t th[NT];
static par_fn job_fn; static void *job_arg;
static volatile unsigned job_seq, job_done[NT];
static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    unsigned seen = 0;
    for (;;) {
        while (__atomic_load_n(&job_seq, __ATOMIC_ACQUIRE) == seen) { }
        seen = job_seq;
        job_fn(job_arg, id, NT);
        __atomic_store_n(&job_done[id], seen, __ATOMIC_RELEASE);
    }
    return 0;
}
static void par(par_fn fn, void *arg) {
    job_fn = fn; job_arg = arg;
    unsigned s = __atomic_add_fetch(&job_seq, 1, __ATOMIC_RELEASE);
    fn(arg, 0, NT);
    for (int i = 1; i < NT; i++) while (__atomic_load_n(&job_done[i], __ATOMIC_ACQUIRE) != s) { }
}

static int chat_prompt(llm_t *m, const char *q, int *t) {
    const llm_info_t *I = llm_info(m);
    int n = 0;
    t[n++] = I->im_start;
    n += llm_encode(m, "system\nYou are a helpful assistant. Answer briefly.", -1, t + n, 4096 - n);
    t[n++] = I->im_end;
    n += llm_encode(m, "\n", -1, t + n, 4096 - n);
    t[n++] = I->im_start;
    n += llm_encode(m, "user\n", -1, t + n, 4096 - n);
    n += llm_encode(m, q, -1, t + n, 4096 - n);
    t[n++] = I->im_end;
    n += llm_encode(m, "\n", -1, t + n, 4096 - n);
    t[n++] = I->im_start;
    n += llm_encode(m, "assistant\n", -1, t + n, 4096 - n);
    if (I->arch == ARCH_QWEN3) {                                  /* thinking off */
        t[n++] = I->think;
        n += llm_encode(m, "\n\n", -1, t + n, 4096 - n);
        t[n++] = I->think_end;
        n += llm_encode(m, "\n\n", -1, t + n, 4096 - n);
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: llmtest MODEL.gguf kern|tok|logits|chat ...\n"); return 2; }
    int fd = open(argv[1], O_RDONLY);
    struct stat st; fstat(fd, &st);
    const uint8_t *f = mmap(0, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    arena_t a = { malloc(1ull << 30), 1ull << 30, 0 };
    char err[200];
    uint64_t t0 = llm_now_ns();
    llm_t *m;
    llm_keys_f16 = getenv("LLM_K16") != 0;
    if (getenv("LLM_SPLIT")) {                     /* as the Pi lays out a big file: two places */
        uint64_t len = (uint64_t)st.st_size, hi_max = 512ull << 20;
        uint64_t x = llm_split_point(f, 16 << 20, len, hi_max), x0 = x & ~511ull, lo_len = (x + 511) & ~511ull;
        uint8_t *lo = malloc(lo_len), *hi = malloc(len - x0);
        memcpy(lo, f, lo_len); memcpy(hi, f + x0, len - x0);
        fprintf(stderr, "  split at %llu: %llu MB low, %llu MB high\n", (unsigned long long)x,
                (unsigned long long)(lo_len >> 20), (unsigned long long)((len - x0) >> 20));
        m = llm_open_split(lo, lo_len, hi, x, len, &a, 2048, err, sizeof err);
    } else m = llm_open(f, (uint64_t)st.st_size, &a, 2048, err, sizeof err);
    if (!m) { fprintf(stderr, "open: %s\n", err); return 1; }
    const llm_info_t *I = llm_info(m);
    fprintf(stderr, "%s: arch %d, %d layers, dim %d, heads %d/%d x %d, ffn %d, vocab %d, ctx %d, rope %.0f, eps %g\n"
            "  %.1f MB read a token, %.1f MB of state, opened in %.0f ms (pre %d)\n",
            I->name, I->arch, I->n_layer, I->dim, I->n_head, I->n_kv, I->head_dim, I->ffn, I->vocab, I->ctx,
            I->rope_base, I->eps, I->weight_bytes / 1e6, I->state_bytes / 1e6, (llm_now_ns() - t0) / 1e6, m->voc.pre);

    if (!strcmp(argv[2], "kern")) {
        float xf[8192]; aq_t xq[256];
        srand(1);
        for (int i = 0; i < 8192; i++) xf[i] = (float)rand() / (float)RAND_MAX * 2 - 1;
        int seen[32] = { 0 };
        for (int i = 0; i < m->nt; i++) {
            const gtensor_t *t = &m->t[i];
            if (t->nd < 2 || seen[t->type & 31]++) continue;
            int n = (int)t->ne[0];
            quantize_act(xf, n, xq);
            double worst = 0;
            for (int r = 0; r < 64; r++) {
                const uint8_t *row = t->data + row_bytes(t->type, n) * (uint64_t)r;
                float a1 = dot_row(t->type, row, xq, xf, n), b1 = dot_ref(t->type, row, xq, xf, n);
                const aq_t *xs[4] = { xq, xq, xq, xq }; const float *xfs[4] = { xf, xf, xf, xf };
                float o4[4]; dot_row4(t->type, row, xs, xfs, n, o4);
                double e = fabs(a1 - b1) / (fabs(b1) + 1e-3), e4 = fabs(o4[2] - b1) / (fabs(b1) + 1e-3);
                if (e > worst) worst = e;
                if (e4 > worst) worst = e4;
            }
            /* the same rows, rearranged, four at a time, for one to four vectors */
            if (x4_able(t->type, 64)) {
                uint64_t rb = row_bytes(t->type, n);
                uint8_t *cp = malloc(64 * rb), *tmp = malloc(4 * rb);
                memcpy(cp, t->data, 64 * rb);
                repack_x4(t->type, cp, 64, n, tmp);
                aq_t *x2 = malloc(sizeof(aq_t) * (size_t)(n / 32) * 4);
                for (int k = 0; k < 4; k++) quantize_act(xf + k * 7, n, x2 + k * (n / 32));
                const aq_t *xs[4] = { x2, x2 + n / 32, x2 + 2 * (n / 32), x2 + 3 * (n / 32) };
                for (int B = 1; B <= 4; B++)
                    for (int g = 0; g < 16; g++) {
                        float o[4][4];
                        mm_x4(t->type | GT_X4, cp + 4 * rb * (uint64_t)g, n, xs, B, o);
                        for (int b = 0; b < B; b++) for (int r = 0; r < 4; r++) {
                            float ref = dot_ref(t->type, t->data + rb * (uint64_t)(4 * g + r), xs[b], 0, n);
                            double e = fabs(o[b][r] - ref) / (fabs(ref) + 1e-3);
                            if (e > worst) worst = e;
                        }
                    }
                float d1[8192], d2[8192];
                for (int r = 0; r < 64; r += 7) {
                    dequant_row(t->type, t->data + rb * (uint64_t)r, d1, n);
                    dequant_any(t->type | GT_X4, cp, r, n, d2);
                    if (memcmp(d1, d2, sizeof(float) * (size_t)n)) { printf("  dequant_any differs at row %d\n", r); break; }
                }
                free(cp); free(tmp); free(x2);
            }
            printf("type %2d (%.*s, %d cols): worst relative error %.2e\n", t->type, t->name_len, t->name, n, worst);
        }
        return 0;
    }
    if (!strcmp(argv[2], "tok")) {
        int t[8192];
        int n = llm_encode(m, argv[3], -1, t, 8192);
        for (int i = 0; i < n; i++) printf("%d\n", t[i]);
        return 0;
    }
    for (int i = 1; i < NT; i++) pthread_create(&th[i], 0, worker, (void *)(intptr_t)i);
    llm_set_par(m, par, NT);
    uint64_t tp = llm_now_ns();
    if (llm_prepare(m, &a)) { fprintf(stderr, "prepare failed\n"); return 1; }
    fprintf(stderr, "  rearranged the weights in %.0f ms\n", (llm_now_ns() - tp) / 1e6);
    if (!strcmp(argv[2], "logits")) {
        int t[8192];
        int n = 0;
        if (I->bos >= 0 && I->arch == ARCH_LLAMA) t[n++] = I->bos;
        n += llm_encode(m, argv[3], -1, t + n, 8192 - n);
        llm_feed(m, t, n, 0);
        float *lg = llm_logits(m);
        for (int k = 0; k < 10; k++) {
            int best = 0;
            for (int i = 1; i < I->vocab; i++) if (lg[i] > lg[best]) best = i;
            char p[64]; int pl = llm_piece(m, best, p, 63); p[pl] = 0;
            printf("%d %.4f %s\n", best, lg[best], p);
            lg[best] = -1e30f;
        }
        return 0;
    }
    if (!strcmp(argv[2], "raw")) {                                /* special tokens written as text */
        static const char *const sp[] = { "<|im_start|>", "<|im_end|>", "<think>", "</think>", "<tool_call>",
                                          "</tool_call>", "<tool_response>", "</tool_response>" };
        int t[8192], n = 0;
        const char *p = argv[3];
        while (*p) {
            const char *best = 0; int bi = -1;
            for (int i = 0; i < 8; i++) { const char *q = strstr(p, sp[i]); if (q && (!best || q < best)) { best = q; bi = i; } }
            const char *e = best ? best : p + strlen(p);
            n += llm_encode(m, p, (int)(e - p), t + n, 8192 - n);
            if (!best) break;
            int id = llm_special(m, sp[bi]);
            if (id >= 0) t[n++] = id; else n += llm_encode(m, sp[bi], -1, t + n, 8192 - n);
            p = best + strlen(sp[bi]);
        }
        llm_feed(m, t, n, 0);
        sampler_t s = { 0, 1, 1.0f, 1, 64, 1 };
        int maxn = argc > 4 ? atoi(argv[4]) : 120, recent[4096], nr = 0;
        for (int out = 0; out < maxn; out++) {
            int tok = llm_sample(m, &s, recent, nr);
            recent[nr++] = tok;
            char pc[64]; int pl = llm_piece(m, tok, pc, 63); pc[pl] = 0;
            if (tok == I->tool_call) printf("[TOOL_CALL]"); else if (tok == I->tool_call_end) { printf("[/TOOL_CALL]"); break; }
            else fwrite(pc, 1, (size_t)pl, stdout);
            fflush(stdout);
            if (tok == I->eos || tok == I->im_end || tok == I->eot) break;
            llm_feed(m, &tok, 1, 0);
        }
        printf("\n");
        return 0;
    }
    if (!strcmp(argv[2], "nll")) {                             /* how well it predicts a text: lower is better */
        static int t[8192];
        FILE *tf = fopen(argv[3], "rb"); static char txt[65536]; size_t tl = fread(txt, 1, sizeof txt - 1, tf); txt[tl] = 0; fclose(tf);
        int n = llm_encode(m, txt, -1, t, 8192);
        if (n > 1000) n = 1000;
        double nll = 0; int cnt = 0;
        for (int i = 0; i + 1 < n; i++) {
            llm_feed(m, &t[i], 1, 0);
            float *lg = llm_logits(m), mx = lg[0];
            for (int k = 1; k < I->vocab; k++) if (lg[k] > mx) mx = lg[k];
            double z = 0; for (int k = 0; k < I->vocab; k++) z += exp(lg[k] - mx);
            nll += -(lg[t[i + 1]] - mx - log(z)); cnt++;
        }
        printf("%d tokens: mean NLL %.4f (perplexity %.2f)\n", cnt, nll / cnt, exp(nll / cnt));
        return 0;
    }
    if (!strcmp(argv[2], "chat")) {
        int t[4096];
        int n = chat_prompt(m, argv[3], t), maxn = argc > 4 ? atoi(argv[4]) : 200;
        uint64_t a0 = llm_now_ns();
        llm_feed(m, t, n, 0);
        uint64_t a1 = llm_now_ns();
        sampler_t s = { 0, 1, 1.0f, 1, 64, 1 };
        int out = 0, recent[4096], nr = 0;
        for (; out < maxn; out++) {
            int tok = llm_sample(m, &s, recent, nr);
            recent[nr++] = tok;
            if (tok == I->eos || tok == I->im_end || tok == I->eot) break;
            char p[64]; int pl = llm_piece(m, tok, p, 63);
            fwrite(p, 1, (size_t)pl, stdout); fflush(stdout);
            llm_feed(m, &tok, 1, 0);
        }
        uint64_t a2 = llm_now_ns();
        llm_stats_t *S = llm_stats(m);
        fprintf(stderr, "\n[prompt %d tokens in %.0f ms = %.1f tok/s; reply %d tokens in %.0f ms = %.1f tok/s; matmul %.0f%%]\n",
                n, (a1 - a0) / 1e6, n / ((a1 - a0) / 1e9), out, (a2 - a1) / 1e6, out / ((a2 - a1) / 1e9),
                100.0 * S->matmul_ns / (S->other_ns + 1));
        return 0;
    }
    return 2;
}
