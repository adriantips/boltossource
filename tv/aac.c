/* ===========================================================================
 *  BoltOS TV  -  tv/aac.c
 *
 *  AAC-LC, from the standard (ISO/IEC 14496-3, subpart 4): the sound of a
 *  YouTube stream. See tv/aac.h for what it does and doesn't do.
 *
 *  An access unit is a run of syntax elements -- a single channel (SCE), a
 *  channel pair (CPE), fill and data elements to skip -- each holding one
 *  or two individual channel streams. A channel stream says how its 1024
 *  spectral lines are cut into scalefactor bands and which Huffman codebook
 *  each band uses, then gives a scalefactor per band and the quantised
 *  lines. Back to sound: |q|^(4/3) times 2^((sf - 100) / 4), the stereo
 *  tools (mid/side, intensity), noise where a band says "noise like this",
 *  TNS's filter along the spectrum, then the inverse MDCT -- one long
 *  transform or eight short ones -- windowed and overlapped with the frame
 *  before.
 *
 *  Floating point throughout (SSE in the kernel, so no x87). The inverse
 *  MDCT is the usual one through an N/4-point complex FFT.
 * ===========================================================================*/
#include <stdint.h>
#include "aac.h"
#include "kheap.h"
#include "string.h"
#include "stdio.h"
#include "math.h"

#define PI 3.14159265358979323846

enum { ONLY_LONG = 0, LONG_START, EIGHT_SHORT, LONG_STOP };
enum { ZERO_HCB = 0, ESC_HCB = 11, NOISE_HCB = 13, INTENSITY_HCB2 = 14, INTENSITY_HCB = 15 };
enum { ID_SCE = 0, ID_CPE, ID_CCE, ID_LFE, ID_DSE, ID_PCE, ID_FIL, ID_END };

/* ---- bits ---------------------------------------------------------------- */
typedef struct { const uint8_t *p; int bits, pos, over; } bits_t;

static uint32_t getb(bits_t *b, int n) {
    uint32_t v = 0;
    while (n > 0) {
        if (b->pos >= b->bits) { b->over = 1; return v << n; }
        int byte = b->pos >> 3, sh = 7 - (b->pos & 7);
        int take = sh + 1 < n ? sh + 1 : n;
        uint32_t chunk = (uint32_t)(b->p[byte] >> (sh + 1 - take)) & ((1u << take) - 1);
        v = v << take | chunk;
        b->pos += take; n -= take;
    }
    return v;
}
static inline int get1(bits_t *b) { return (int)getb(b, 1); }

/* ---- Huffman: a binary tree per codebook, walked a bit at a time --------- */
typedef struct { int16_t (*kid)[2]; int n; } tree_t;
static tree_t sf_tree, cb_tree[11];
static int tables_ready;

static int tree_build(tree_t *t, int count, const uint32_t *code32, const uint16_t *code16, const uint8_t *len) {
    t->kid = kmalloc((uint64_t)count * 2 * sizeof *t->kid);
    if (!t->kid) return -1;
    t->n = 1;
    t->kid[0][0] = t->kid[0][1] = 0;
    for (int s = 0; s < count; s++) {
        uint32_t c = code32 ? code32[s] : code16[s];
        int node = 0;
        for (int i = len[s] - 1; i >= 0; i--) {
            int bit = (int)(c >> i) & 1;
            if (i == 0) { t->kid[node][bit] = (int16_t)(-(s + 1)); break; }
            if (t->kid[node][bit] <= 0) {
                if (t->n >= count * 2) return -1;
                t->kid[node][bit] = (int16_t)t->n;
                t->kid[t->n][0] = t->kid[t->n][1] = 0;
                t->n++;
            }
            node = t->kid[node][bit];
        }
    }
    return 0;
}

static int huff(bits_t *b, const tree_t *t) {
    int node = 0;
    for (int guard = 0; guard < 20; guard++) {
        int k = t->kid[node][get1(b)];
        if (k < 0) return -k - 1;
        if (k == 0) break;                         /* not a code */
        node = k;
    }
    b->over = 1;
    return 0;
}

/* ---- the transform -------------------------------------------------------- */
typedef struct { float re, im; } cpx;
typedef struct {
    int n;                          /* output length: 2048 or 256 */
    float *tcos, *tsin;             /* n/4 */
    cpx *tw;                        /* FFT twiddles, n/8 */
    uint16_t *rev;                  /* n/4 */
    cpx *z;
} mdct_t;

static float pow43[8192];
static float sfgain[256 + 100];     /* 2^((i - 100 - 100) / 4): sf - 100 from -100 */
static float win_sine_l[1024], win_sine_s[128], win_kbd_l[1024], win_kbd_s[128];
static mdct_t m_long, m_short;

static int mdct_init(mdct_t *m, int n) {
    int n4 = n / 4;
    m->n = n;
    m->tcos = kmalloc((uint64_t)n4 * sizeof(float));
    m->tsin = kmalloc((uint64_t)n4 * sizeof(float));
    m->tw   = kmalloc((uint64_t)n4 * sizeof(cpx));
    m->rev  = kmalloc((uint64_t)n4 * sizeof(uint16_t));
    m->z    = kmalloc((uint64_t)n4 * sizeof(cpx));
    if (!m->tcos || !m->tsin || !m->tw || !m->rev || !m->z) return -1;
    /* 2/N is the standard's scale: the output is 16-bit PCM's. */
    double scale = 2.0 / n;
    for (int k = 0; k < n4; k++) {
        double a = 2 * PI * (k + 0.125) / n;
        m->tcos[k] = (float)(cos(a) * sqrt(scale));
        m->tsin[k] = (float)(sin(a) * sqrt(scale));
    }
    for (int k = 0; k < n4; k++) {
        double a = 2 * PI * k / n4;
        m->tw[k].re = (float)cos(a); m->tw[k].im = (float)sin(a);
    }
    int bits = 0;
    while ((1 << bits) < n4) bits++;
    for (int k = 0; k < n4; k++) {
        int r = 0;
        for (int i = 0; i < bits; i++) if (k >> i & 1) r |= 1 << (bits - 1 - i);
        m->rev[k] = (uint16_t)r;
    }
    return 0;
}

/* In place, input already in bit-reversed order. */
static void fft(const mdct_t *m, cpx *z, int n) {
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1, step = n / len;
        for (int i = 0; i < n; i += len)
            for (int j = 0; j < half; j++) {
                cpx w = m->tw[j * step], *a = &z[i + j], *b = &z[i + j + half];
                float tr = b->re * w.re - b->im * w.im, ti = b->re * w.im + b->im * w.re;
                b->re = a->re - tr; b->im = a->im - ti;
                a->re += tr; a->im += ti;
            }
    }
}

/* out[n] = 2/N * sum_k in[k] cos(2pi/N (n + n0)(k + 1/2)), n0 = (N/2 + 1)/2;
 * in has N/2 lines, out N samples. */
static void imdct(const mdct_t *m, const float *in, float *out) {
    int n = m->n, n2 = n / 2, n4 = n / 4, n8 = n / 8;
    cpx *z = m->z;
    for (int k = 0; k < n4; k++) {
        float a = in[n2 - 1 - 2 * k], b = in[2 * k];
        int j = m->rev[k];
        z[j].re = a * m->tcos[k] - b * m->tsin[k];
        z[j].im = a * m->tsin[k] + b * m->tcos[k];
    }
    fft(m, z, n4);
    for (int k = 0; k < n8; k++) {
        int p = n8 - k - 1, q = n8 + k;
        float r0 = z[p].im * m->tsin[p] - z[p].re * m->tcos[p];
        float i1 = z[p].im * m->tcos[p] + z[p].re * m->tsin[p];
        float r1 = z[q].im * m->tsin[q] - z[q].re * m->tcos[q];
        float i0 = z[q].im * m->tcos[q] + z[q].re * m->tsin[q];
        z[p].re = r0; z[p].im = i0;
        z[q].re = r1; z[q].im = i1;
    }
    float *half = out + n4;
    for (int k = 0; k < n4; k++) { half[2 * k] = -z[k].re; half[2 * k + 1] = -z[k].im; }
    for (int k = 0; k < n4; k++) { out[k] = -out[n2 - k - 1]; out[n - k - 1] = out[n2 + k]; }
}

/* The Kaiser-Bessel-derived window's rising half, `half` points. */
static double bessel_i0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 60; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-12 * s) break; }
    return s;
}
static void kbd(float *w, int half, double alpha) {
    double sum = 0, acc = 0;
    double ker[1025];
    for (int i = 0; i <= half; i++) {
        double r = 2.0 * i / half - 1.0;
        ker[i] = bessel_i0(PI * alpha * sqrt(1.0 - r * r));
        sum += ker[i];
    }
    for (int i = 0; i < half; i++) { acc += ker[i]; w[i] = (float)sqrt(acc / sum); }
}

static int tables_init(void) {
    if (tables_ready) return tables_ready > 0 ? 0 : -1;
    tables_ready = -1;
    if (tree_build(&sf_tree, 121, aac_sf_code, 0, aac_sf_bits)) return -1;
    for (int b = 0; b < 11; b++)
        if (tree_build(&cb_tree[b], aac_cb_size[b], 0, aac_cb_code[b], aac_cb_bits[b])) return -1;
    for (int i = 0; i < 8192; i++) pow43[i] = (float)pow((double)i, 4.0 / 3.0);
    for (int i = 0; i < 356; i++) sfgain[i] = (float)pow(2.0, 0.25 * (i - 200));
    for (int i = 0; i < 1024; i++) win_sine_l[i] = (float)sin(PI / 2048 * (i + 0.5));
    for (int i = 0; i < 128; i++) win_sine_s[i] = (float)sin(PI / 256 * (i + 0.5));
    kbd(win_kbd_l, 1024, 4.0);
    kbd(win_kbd_s, 128, 6.0);
    if (mdct_init(&m_long, 2048) || mdct_init(&m_short, 256)) return -1;
    tables_ready = 1;
    return 0;
}

/* ---- the decoder ---------------------------------------------------------- */
typedef struct {
    int   win_seq, win_shape, max_sfb, num_windows, num_groups, group_len[8];
    int   num_swb; const uint16_t *swb;
    uint8_t cb[8][64];
    int   sf[8][64];
    int   tns_present;
    int   tns_nf[8], tns_len[8][4], tns_order[8][4], tns_dir[8][4];
    float tns_coef[8][4][32];
} ics_t;

typedef struct {
    float overlap[1024];
    int   prev_shape;
} chan_t;

struct aac {
    int   object, sr_index, rate, channels;
    chan_t ch[2];
    ics_t ics[2];
    float coef[2][1024];
    int   q[1024];
    float buf[2048], tmp[2048];
    uint32_t rnd;
    char  err[80];
};

static int fail(aac *d, const char *why) {
    if (!d->err[0]) snprintf(d->err, sizeof d->err, "%s", why);
    return -1;
}
const char *aac_error(const aac *d) { return d->err; }
int aac_rate(const aac *d) { return d->rate; }
int aac_channels(const aac *d) { return d->channels; }

static const int RATES[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };

aac *aac_open(const uint8_t *asc, int asc_len) {
    if (tables_init() || asc_len < 2) return 0;
    bits_t b = { asc, asc_len * 8, 0, 0 };
    int obj = (int)getb(&b, 5);
    if (obj == 31) obj = 32 + (int)getb(&b, 6);
    int sri = (int)getb(&b, 4);
    if (sri == 15) return 0;                       /* an explicit rate: not one YouTube uses */
    int chc = (int)getb(&b, 4);
    if (obj == 5 || obj == 29) {                   /* HE-AAC: the LC core is what's decoded */
        int ext = (int)getb(&b, 4);
        (void)ext;
        obj = (int)getb(&b, 5);
    }
    if (obj != 2 || sri > 12 || chc < 1 || chc > 2) return 0;
    aac *d = kmalloc(sizeof *d);
    if (!d) return 0;
    memset(d, 0, sizeof *d);
    d->object = obj; d->sr_index = sri; d->rate = RATES[sri]; d->channels = chc;
    d->rnd = 0x1f2e3d4c;
    return d;
}

void aac_close(aac *d) { if (d) kfree(d); }
void aac_reset(aac *d) { if (d) { memset(d->ch, 0, sizeof d->ch); d->err[0] = 0; } }

static int ics_info(aac *d, bits_t *b, ics_t *s) {
    if (get1(b)) return fail(d, "reserved bit set");
    s->win_seq = (int)getb(b, 2);
    s->win_shape = get1(b);
    if (s->win_seq == EIGHT_SHORT) {
        s->max_sfb = (int)getb(b, 4);
        int grouping = (int)getb(b, 7);
        s->num_windows = 8;
        s->num_groups = 1; s->group_len[0] = 1;
        for (int i = 6; i >= 0; i--) {
            if (grouping >> i & 1) s->group_len[s->num_groups - 1]++;
            else s->group_len[s->num_groups++] = 1;
        }
        s->num_swb = aac_num_swb_128[d->sr_index];
        s->swb = aac_swb_offset_128[d->sr_index];
    } else {
        s->max_sfb = (int)getb(b, 6);
        if (get1(b)) return fail(d, "prediction (AAC Main)");
        s->num_windows = 1; s->num_groups = 1; s->group_len[0] = 1;
        s->num_swb = aac_num_swb_1024[d->sr_index];
        s->swb = aac_swb_offset_1024[d->sr_index];
    }
    if (s->max_sfb > s->num_swb) return fail(d, "too many bands");
    return 0;
}

static int section_data(aac *d, bits_t *b, ics_t *s) {
    int bits = s->win_seq == EIGHT_SHORT ? 3 : 5, esc = (1 << bits) - 1;
    for (int g = 0; g < s->num_groups; g++) {
        int k = 0;
        while (k < s->max_sfb) {
            int cb = (int)getb(b, 4), len = 0, inc;
            if (cb == 12) return fail(d, "reserved codebook");
            do { inc = (int)getb(b, bits); len += inc; } while (inc == esc && !b->over);
            if (b->over || k + len > s->max_sfb) return fail(d, "bad sections");
            for (int i = 0; i < len; i++) s->cb[g][k++] = (uint8_t)cb;
        }
        for (; k < 64; k++) s->cb[g][k] = 0;
    }
    return 0;
}

static int scale_factors(aac *d, bits_t *b, ics_t *s, int global_gain) {
    int sf = global_gain, is = 0, noise = global_gain - 90, first_noise = 1;
    for (int g = 0; g < s->num_groups; g++)
        for (int k = 0; k < s->max_sfb; k++) {
            int cb = s->cb[g][k];
            if (cb == ZERO_HCB) s->sf[g][k] = 0;
            else if (cb == INTENSITY_HCB || cb == INTENSITY_HCB2) { is += huff(b, &sf_tree) - 60; s->sf[g][k] = is; }
            else if (cb == NOISE_HCB) {
                if (first_noise) { noise += (int)getb(b, 9) - 256; first_noise = 0; }
                else noise += huff(b, &sf_tree) - 60;
                s->sf[g][k] = noise;
            } else {
                sf += huff(b, &sf_tree) - 60;
                if (sf < 0 || sf > 255) return fail(d, "scalefactor out of range");
                s->sf[g][k] = sf;
            }
        }
    return b->over ? fail(d, "short scalefactors") : 0;
}

static int tns_data(aac *d, bits_t *b, ics_t *s) {
    int sh = s->win_seq == EIGHT_SHORT;
    for (int w = 0; w < s->num_windows; w++) {
        s->tns_nf[w] = (int)getb(b, sh ? 1 : 2);
        int res = s->tns_nf[w] ? get1(b) : 0;
        for (int f = 0; f < s->tns_nf[w]; f++) {
            s->tns_len[w][f] = (int)getb(b, sh ? 4 : 6);
            int order = (int)getb(b, sh ? 3 : 5);
            if (order > (sh ? 7 : 12)) return fail(d, "TNS order too high");
            s->tns_order[w][f] = order;
            if (order) {
                s->tns_dir[w][f] = get1(b);
                int compress = get1(b);
                int len = res + 3 - compress;
                const float *map = aac_tns_map[2 * compress + res];
                /* FFmpeg keeps these negated (its LPC step negates them back): the standard has +sin */
                for (int i = 0; i < order; i++) s->tns_coef[w][f][i] = -map[getb(b, len)];
            }
        }
    }
    return 0;
}

/* The quantised lines, into d->q in window order (short windows 128 apart). */
static int spectral_data(aac *d, bits_t *b, ics_t *s) {
    memset(d->q, 0, sizeof d->q);
    int win0 = 0;
    for (int g = 0; g < s->num_groups; g++) {
        for (int k = 0; k < s->max_sfb; k++) {
            int cb = s->cb[g][k];
            if (cb == ZERO_HCB || cb >= NOISE_HCB) continue;
            const tree_t *t = &cb_tree[cb - 1];
            int dim4 = cb <= 4, unsig = !(cb == 1 || cb == 2 || cb == 5 || cb == 6);
            int mod = cb <= 4 ? 3 : cb <= 6 ? 9 : cb <= 8 ? 8 : cb <= 10 ? 13 : 17;
            int off = unsig ? 0 : (mod - 1) / 2;
            for (int w = 0; w < s->group_len[g]; w++) {
                int *q = d->q + (win0 + w) * 128;
                for (int i = s->swb[k]; i < s->swb[k + 1]; i += dim4 ? 4 : 2) {
                    int idx = huff(b, t), v[4];
                    if (dim4) {
                        v[0] = idx / 27 - off; v[1] = idx / 9 % 3 - off; v[2] = idx / 3 % 3 - off; v[3] = idx % 3 - off;
                    } else { v[0] = idx / mod - off; v[1] = idx % mod - off; }
                    int nv = dim4 ? 4 : 2;
                    if (unsig) for (int j = 0; j < nv; j++) if (v[j] && get1(b)) v[j] = -v[j];
                    if (cb == ESC_HCB)
                        for (int j = 0; j < 2; j++) {
                            int a = v[j] < 0 ? -v[j] : v[j];
                            if (a != 16) continue;
                            int n = 0;
                            while (get1(b) && n < 9 && !b->over) n++;
                            if (n >= 9) return fail(d, "bad escape");
                            a = (1 << (n + 4)) + (int)getb(b, n + 4);
                            v[j] = v[j] < 0 ? -a : a;
                        }
                    for (int j = 0; j < nv; j++) q[i + j] = v[j];
                }
            }
        }
        win0 += s->group_len[g];
    }
    return b->over ? fail(d, "short spectral data") : 0;
}

static uint32_t rnd_next(aac *d) { d->rnd = d->rnd * 1664525u + 1013904223u; return d->rnd; }

/* Quantised lines and scalefactors to spectrum; noise where it says. */
static void dequant(aac *d, const ics_t *s, float *coef) {
    memset(coef, 0, 1024 * sizeof(float));
    int win0 = 0;
    for (int g = 0; g < s->num_groups; g++) {
        for (int k = 0; k < s->max_sfb; k++) {
            int cb = s->cb[g][k], lo = s->swb[k], hi = s->swb[k + 1];
            if (cb == ZERO_HCB || cb == INTENSITY_HCB || cb == INTENSITY_HCB2) continue;
            for (int w = 0; w < s->group_len[g]; w++) {
                float *c = coef + (win0 + w) * 128;
                if (cb == NOISE_HCB) {
                    float e = 0;
                    for (int i = lo; i < hi; i++) { c[i] = (float)(int32_t)rnd_next(d); e += c[i] * c[i]; }
                    float sc = (float)pow(2.0, 0.25 * s->sf[g][k]) / (e > 0 ? sqrtf(e) : 1.0f);
                    for (int i = lo; i < hi; i++) c[i] *= sc;
                    continue;
                }
                float gain = sfgain[s->sf[g][k] + 100];
                const int *q = d->q + (win0 + w) * 128;
                for (int i = lo; i < hi; i++) {
                    int v = q[i], a = v < 0 ? -v : v;
                    if (a > 8191) a = 8191;
                    float x = pow43[a] * gain;
                    c[i] = v < 0 ? -x : x;
                }
            }
        }
        win0 += s->group_len[g];
    }
}

static void apply_tns(aac *d, const ics_t *s, float *coef) {
    if (!s->tns_present) return;
    int sh = s->win_seq == EIGHT_SHORT;
    int tmax = sh ? aac_tns_max_bands_128[d->sr_index] : aac_tns_max_bands_1024[d->sr_index];
    for (int w = 0; w < s->num_windows; w++) {
        float *c = coef + w * 128;
        int bottom = s->num_swb;
        for (int f = 0; f < s->tns_nf[w]; f++) {
            int top = bottom;
            bottom = top - s->tns_len[w][f];
            if (bottom < 0) bottom = 0;
            int order = s->tns_order[w][f];
            if (!order) continue;
            float lpc[33], tmp[33];
            lpc[0] = 1;
            for (int m = 1; m <= order; m++) {
                float k = s->tns_coef[w][f][m - 1];
                for (int i = 1; i < m; i++) tmp[i] = lpc[i] + k * lpc[m - i];
                for (int i = 1; i < m; i++) lpc[i] = tmp[i];
                lpc[m] = k;
            }
            int lim = tmax < s->max_sfb ? tmax : s->max_sfb;
            int a = bottom < lim ? bottom : lim, z = top < lim ? top : lim;
            int start = s->swb[a], end = s->swb[z], size = end - start;
            if (size <= 0) continue;
            int inc = 1;
            if (s->tns_dir[w][f]) { inc = -1; start = end - 1; }
            for (int m = 0; m < size; m++) {
                int at = start + m * inc;
                float y = c[at];
                for (int i = 1; i <= order && i <= m; i++) y -= lpc[i] * c[at - i * inc];
                c[at] = y;
            }
        }
    }
}

/* One channel's spectrum to 1024 samples, through the window and overlap. */
static void synth(aac *d, const ics_t *s, chan_t *ch, const float *coef, float *out) {
    const float *lw_prev = ch->prev_shape ? win_kbd_l : win_sine_l, *lw = s->win_shape ? win_kbd_l : win_sine_l;
    const float *sw_prev = ch->prev_shape ? win_kbd_s : win_sine_s, *sw = s->win_shape ? win_kbd_s : win_sine_s;
    float *b = d->buf;
    if (s->win_seq == EIGHT_SHORT) {
        float *t = d->tmp;
        memset(b, 0, 2048 * sizeof(float));
        for (int w = 0; w < 8; w++) {
            imdct(&m_short, coef + w * 128, t);
            const float *rise = w ? sw : sw_prev;
            float *o = b + 448 + 128 * w;
            for (int i = 0; i < 128; i++) { o[i] += t[i] * rise[i]; o[128 + i] += t[128 + i] * sw[127 - i]; }
        }
    } else {
        imdct(&m_long, coef, b);
        if (s->win_seq == LONG_STOP) {
            for (int i = 0; i < 448; i++) b[i] = 0;
            for (int i = 0; i < 128; i++) b[448 + i] *= sw_prev[i];
        } else for (int i = 0; i < 1024; i++) b[i] *= lw_prev[i];
        if (s->win_seq == LONG_START) {
            for (int i = 0; i < 128; i++) b[1472 + i] *= sw[127 - i];
            for (int i = 1600; i < 2048; i++) b[i] = 0;
        } else for (int i = 0; i < 1024; i++) b[1024 + i] *= lw[1023 - i];
    }
    for (int i = 0; i < 1024; i++) { out[i] = ch->overlap[i] + b[i]; ch->overlap[i] = b[1024 + i]; }
    ch->prev_shape = s->win_shape;
}

static int channel_stream(aac *d, bits_t *b, ics_t *s, int common) {
    int gg = (int)getb(b, 8);
    if (!common && ics_info(d, b, s)) return -1;
    if (section_data(d, b, s) || scale_factors(d, b, s, gg)) return -1;
    int pulse = get1(b), np = 0, pstart = 0, poff[4], pamp[4];
    if (pulse) {
        if (s->win_seq == EIGHT_SHORT) return fail(d, "pulses in a short block");
        np = (int)getb(b, 2) + 1; pstart = (int)getb(b, 6);
        if (pstart >= s->num_swb) return fail(d, "bad pulses");
        for (int i = 0; i < np; i++) { poff[i] = (int)getb(b, 5); pamp[i] = (int)getb(b, 4); }
    }
    s->tns_present = get1(b);
    if (s->tns_present && tns_data(d, b, s)) return -1;
    if (get1(b)) return fail(d, "gain control (AAC SSR)");
    if (spectral_data(d, b, s)) return -1;
    if (pulse) {
        int k = s->swb[pstart];
        for (int i = 0; i < np; i++) {
            k += poff[i];
            if (k >= 1024) return fail(d, "bad pulses");
            d->q[k] += d->q[k] > 0 ? pamp[i] : -pamp[i];
        }
    }
    return 0;
}

static float clipf(float v) { return v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v; }

static void write_pcm(int16_t *out, const float *l, const float *r) {
    for (int i = 0; i < 1024; i++) {
        float a = clipf(l[i]), c = clipf(r[i]);
        out[2 * i]     = (int16_t)(a < 0 ? a - 0.5f : a + 0.5f);
        out[2 * i + 1] = (int16_t)(c < 0 ? c - 0.5f : c + 0.5f);
    }
}

int aac_decode(aac *d, const uint8_t *au, int len, int16_t *out) {
    bits_t b = { au, len * 8, 0, 0 };
    float pcm[2][1024];
    int got = 0;                                   /* channels written into pcm */
    d->err[0] = 0;
    for (int guard = 0; guard < 64; guard++) {
        int id = (int)getb(&b, 3);
        if (b.over) break;
        if (id == ID_END) break;
        if (id != ID_FIL) getb(&b, 4);             /* element_instance_tag: a fill element has none */
        if (id == ID_SCE || id == ID_LFE) {
            if (got >= 2) { fail(d, "more channels than two"); goto bad; }
            ics_t *s = &d->ics[0];
            if (channel_stream(d, &b, s, 0)) goto bad;
            if (id == ID_LFE) continue;            /* no subwoofer here */
            dequant(d, s, d->coef[0]);
            apply_tns(d, s, d->coef[0]);
            synth(d, s, &d->ch[got], d->coef[0], pcm[got]);
            got++;
        } else if (id == ID_CPE) {
            if (got) { fail(d, "more channels than two"); goto bad; }
            ics_t *l = &d->ics[0], *r = &d->ics[1];
            int common = get1(&b), ms = 0;
            uint8_t ms_used[8][64];
            memset(ms_used, 0, sizeof ms_used);
            if (common) {
                if (ics_info(d, &b, l)) goto bad;
                *r = *l;
                ms = (int)getb(&b, 2);
                if (ms == 3) { fail(d, "reserved M/S mode"); goto bad; }
                for (int g = 0; g < l->num_groups; g++)
                    for (int k = 0; k < l->max_sfb; k++) ms_used[g][k] = (uint8_t)(ms == 2 ? 1 : ms == 1 ? get1(&b) : 0);
            }
            if (channel_stream(d, &b, l, common)) goto bad;
            dequant(d, l, d->coef[0]);
            if (channel_stream(d, &b, r, common)) goto bad;
            dequant(d, r, d->coef[1]);
            if (common) {
                int win0 = 0;
                for (int g = 0; g < l->num_groups; g++) {
                    for (int k = 0; k < l->max_sfb; k++) {
                        int lo = l->swb[k], hi = l->swb[k + 1], rcb = r->cb[g][k];
                        for (int w = 0; w < l->group_len[g]; w++) {
                            float *a = d->coef[0] + (win0 + w) * 128, *c = d->coef[1] + (win0 + w) * 128;
                            if (rcb == INTENSITY_HCB || rcb == INTENSITY_HCB2) {
                                float sc = (float)pow(0.5, 0.25 * r->sf[g][k]);
                                if (rcb == INTENSITY_HCB2) sc = -sc;
                                if (ms == 1 && ms_used[g][k]) sc = -sc;
                                for (int i = lo; i < hi; i++) c[i] = a[i] * sc;
                            } else if (ms_used[g][k] && l->cb[g][k] < NOISE_HCB && rcb < NOISE_HCB) {
                                for (int i = lo; i < hi; i++) { float m = a[i], s = c[i]; a[i] = m + s; c[i] = m - s; }
                            }
                        }
                    }
                    win0 += l->group_len[g];
                }
            }
            apply_tns(d, l, d->coef[0]);
            apply_tns(d, r, d->coef[1]);
            synth(d, l, &d->ch[0], d->coef[0], pcm[0]);
            synth(d, r, &d->ch[1], d->coef[1], pcm[1]);
            got = 2;
        } else if (id == ID_FIL) {
            int n = (int)getb(&b, 4);
            if (n == 15) n += (int)getb(&b, 8) - 1;
            b.pos += n * 8;
        } else if (id == ID_DSE) {
            int align = get1(&b), n = (int)getb(&b, 8);
            if (n == 255) n += (int)getb(&b, 8);
            if (align) b.pos = (b.pos + 7) & ~7;
            b.pos += n * 8;
        } else { fail(d, id == ID_CCE ? "coupling channels" : "a program config element"); goto bad; }
        if (b.pos > b.bits) { fail(d, "the unit ran short"); goto bad; }
    }
    if (!got) { fail(d, "no channels"); goto bad; }
    write_pcm(out, pcm[0], pcm[got > 1 ? 1 : 0]);
    return 1024;
bad:
    memset(out, 0, 2048 * sizeof(int16_t));
    return -1;
}

#ifdef AAC_SELFTEST
/* The fast transform against the formula it stands for. */
int aac_selftest_imdct(void) {
    if (tables_init()) return 1;
    const mdct_t *ms[2] = { &m_long, &m_short };
    int bad = 0;
    for (int t = 0; t < 2; t++) {
        int n = ms[t]->n;
        static float in[1024], out[2048];
        for (int k = 0; k < n / 2; k++) in[k] = (float)((k * 7919 % 1000) - 500) / 100.0f;
        imdct(ms[t], in, out);
        double n0 = (n / 2 + 1) / 2.0, err = 0, peak = 0;
        for (int i = 0; i < n; i++) {
            double s = 0;
            for (int k = 0; k < n / 2; k++) s += in[k] * cos(2 * PI / n * (i + n0) * (k + 0.5));
            s *= 2.0 / n;
            if (i < 4) printf("  n=%d out[%d] = %f, formula %f\n", n, i, out[i], s);
            double e = fabs(s - out[i]);
            if (e > err) err = e;
            if (fabs(s) > peak) peak = fabs(s);
        }
        printf("imdct %d: worst error %g of peak %g\n", n, err, peak);
        if (err > peak * 1e-4) bad = 1;
    }
    return bad;
}
#endif
