/* ===========================================================================
 *  BoltOS Mini  -  mini/numfmt.c
 *
 *  Floating-point numbers to text and back, exactly: what Python's repr()
 *  and C's printf("%f") must print and what "0.1" must read as. libc's
 *  printf on the desktop gets close with doubles; a program printing
 *  0.1 + 0.2 has to see 0.30000000000000004, so this works with the exact
 *  value in big integers instead.
 *
 *  To text: Steele & White's / Burger & Dybvig's digit generation. The
 *  number is r/s with r and s integers (and m+, m- the distances to its
 *  neighbours), scaled by a power of ten into [0.1, 1); each step takes a
 *  digit off r*10/s. "Shortest" stops at the first digits that read back as
 *  the same double; "fixed" takes a count of digits and rounds half-even.
 *
 *  From text: the closest double to the decimal, by making a guess and
 *  then comparing the decimal against the half-way points next to it,
 *  exactly, until it's the right one.
 * ===========================================================================*/
#include <stdint.h>
#include "rt.h"
#include "string.h"

/* ---- big integers: little-endian 32-bit words, fixed size ------------------------------------- */
#define BW 80
typedef struct { int n; uint32_t w[BW]; } big;

static void b_set(big *a, uint64_t v) { a->n = 0; while (v) { a->w[a->n++] = (uint32_t)v; v >>= 32; } }
static void b_mul(big *a, uint32_t m) {
    uint64_t c = 0;
    for (int i = 0; i < a->n; i++) { c += (uint64_t)a->w[i] * m; a->w[i] = (uint32_t)c; c >>= 32; }
    if (c && a->n < BW) a->w[a->n++] = (uint32_t)c;
}
static void b_shl(big *a, int k) {
    int words = k / 32, bits = k % 32;
    if (!a->n) return;
    if (bits) {
        uint32_t c = 0;
        for (int i = 0; i < a->n; i++) { uint32_t v = a->w[i]; a->w[i] = v << bits | c; c = v >> (32 - bits); }
        if (c && a->n < BW) a->w[a->n++] = c;
    }
    if (words) {
        if (a->n + words > BW) words = BW - a->n;
        memmove(a->w + words, a->w, sizeof(uint32_t) * (size_t)a->n);
        memset(a->w, 0, sizeof(uint32_t) * (size_t)words);
        a->n += words;
    }
}
static void b_pow10(big *a, int k) {
    while (k >= 9) { b_mul(a, 1000000000u); k -= 9; }
    static const uint32_t p[9] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000 };
    if (k > 0) b_mul(a, p[k]);
}
static int b_cmp(const big *a, const big *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}
static void b_add(big *a, const big *b) {                    /* a += b */
    uint64_t c = 0;
    int n = a->n > b->n ? a->n : b->n;
    for (int i = 0; i < n; i++) {
        c += (uint64_t)(i < a->n ? a->w[i] : 0) + (i < b->n ? b->w[i] : 0);
        a->w[i] = (uint32_t)c; c >>= 32;
    }
    a->n = n;
    if (c && a->n < BW) a->w[a->n++] = (uint32_t)c;
}
static void b_sub(big *a, const big *b) {                    /* a -= b, a >= b */
    int64_t c = 0;
    for (int i = 0; i < a->n; i++) {
        c += (int64_t)a->w[i] - (i < b->n ? b->w[i] : 0);
        a->w[i] = (uint32_t)c; c >>= 32;
    }
    while (a->n && !a->w[a->n - 1]) a->n--;
}
/* r + m vs s */
static int b_cmp_sum(const big *r, const big *m, const big *s) { big t = *r; b_add(&t, m); return b_cmp(&t, s); }

/* ---- the digits ------------------------------------------------------------------------------- */
typedef union { double d; uint64_t u; } dbits;

/* Digits of x > 0 (finite) into d[], as 0.d1d2... x 10^decpt, rounded half-even:
 * mode 0 the shortest that reads back, 1 `count` digits, 2 the digits down to
 * 10^-count. Returns the number of digits (0: it rounds to zero). */
static int digits(double x, int mode, int count, char *d, int *decpt) {
    dbits b; b.d = x;
    int be = (int)((b.u >> 52) & 0x7FF);
    uint64_t f = b.u & ((1ull << 52) - 1);
    int e;
    if (be) { f |= 1ull << 52; e = be - 1075; } else e = -1074;
    int even = !(f & 1), unequal = be > 1 && f == (1ull << 52);   /* the gap below is half the one above */
    big r, s, mp, mm;
    b_set(&r, f); b_set(&s, 1); b_set(&mp, 1); b_set(&mm, 1);
    if (e >= 0) {
        b_shl(&r, e + 1 + unequal); b_set(&s, unequal ? 4 : 2);
        b_shl(&mp, e + unequal); b_shl(&mm, e);
    } else {
        b_shl(&r, 1 + unequal); b_shl(&s, 1 - e + unequal);
        b_set(&mp, unequal ? 2 : 1);
    }
    int bits = 64 - __builtin_clzll(f);
    int k = (int)__builtin_ceil((double)(e + bits - 1) * 0.30102999566398114 - 1e-10);
    if (k >= 0) b_pow10(&s, k);
    else { b_pow10(&r, -k); b_pow10(&mp, -k); b_pow10(&mm, -k); }
    int hi = b_cmp_sum(&r, &mp, &s);
    if (mode == 0 ? (even ? hi >= 0 : hi > 0) : b_cmp(&r, &s) >= 0) { b_mul(&s, 10); k++; }
    *decpt = k;
    int n = 0;
    if (mode == 2) {
        count += k;                                          /* digits down to 10^-count */
        if (count < 0) { *decpt = 0; return 0; }
        if (count == 0) {
            big t = r; b_shl(&t, 1);
            if (b_cmp(&t, &s) > 0) { d[0] = '1'; *decpt = k + 1; return 1; }
            *decpt = 0; return 0;
        }
    }
    if (mode == 0) {
        for (;;) {
            b_mul(&r, 10); b_mul(&mp, 10); b_mul(&mm, 10);
            int dg = 0;
            while (b_cmp(&r, &s) >= 0) { b_sub(&r, &s); dg++; }
            int c1 = even ? b_cmp(&r, &mm) <= 0 : b_cmp(&r, &mm) < 0;
            int h = b_cmp_sum(&r, &mp, &s);
            int c2 = even ? h >= 0 : h > 0;
            if (!c1 && !c2 && n < 30) { d[n++] = (char)('0' + dg); continue; }
            if (c1 && c2) {                                  /* both read back: the nearer */
                big t = r; b_shl(&t, 1);
                int c = b_cmp(&t, &s);
                if (c > 0 || (c == 0 && (dg & 1))) dg++;
            } else if (c2) dg++;
            d[n++] = (char)('0' + dg);
            break;
        }
        return n;
    }
    for (n = 0; n < count && n < 400; n++) {
        b_mul(&r, 10);
        int dg = 0;
        while (b_cmp(&r, &s) >= 0) { b_sub(&r, &s); dg++; }
        d[n] = (char)('0' + dg);
    }
    /* round half-even on what's left */
    big t = r; b_shl(&t, 1);
    int c = b_cmp(&t, &s);
    if (c > 0 || (c == 0 && n && ((d[n - 1] - '0') & 1))) {
        int i = n - 1;
        while (i >= 0 && d[i] == '9') d[i--] = '0';
        if (i >= 0) d[i]++;
        else { memmove(d + 1, d, (size_t)n - 1); d[0] = '1'; (*decpt)++; }
    }
    return n;
}

static int strlcpy_(char *d, const char *s, int cap) {
    int n = (int)strlen(s);
    if (cap > 0) { int k = n < cap - 1 ? n : cap - 1; memcpy(d, s, (size_t)k); d[k] = 0; }
    return n;
}

static int special(double x, char *out, int cap, int upper, int py) {
    if (x != x) return (int)strlcpy_(out, py ? "nan" : upper ? "NAN" : "nan", cap);
    if (x == __builtin_inf()) return (int)strlcpy_(out, py ? "inf" : upper ? "INF" : "inf", cap);
    if (x == -__builtin_inf()) return (int)strlcpy_(out, py ? "-inf" : upper ? "-INF" : "-inf", cap);
    return -1;
}


typedef struct { char *o; int n, cap; } sb_t;
static void put(sb_t *b, char c) { if (b->n < b->cap - 1) b->o[b->n] = c; b->n++; }
static void puts_(sb_t *b, const char *s) { while (*s) put(b, *s++); }
static int done(sb_t *b) { if (b->cap > 0) b->o[b->n < b->cap - 1 ? b->n : b->cap - 1] = 0; return b->n; }

static void put_exp(sb_t *b, int e, int upper, int mindig) {
    put(b, upper ? 'E' : 'e');
    put(b, e < 0 ? '-' : '+');
    if (e < 0) e = -e;
    char t[8]; int n = 0;
    do { t[n++] = (char)('0' + e % 10); e /= 10; } while (e);
    while (n < mindig) t[n++] = '0';
    while (n) put(b, t[--n]);
}

/* Python's repr(float). */
int num_repr(double x, char *out, int cap) {
    int sp = special(x, out, cap, 0, 1);
    if (sp >= 0) return sp;
    sb_t b = { out, 0, cap };
    if (__builtin_signbit(x)) { put(&b, '-'); x = -x; }
    if (x == 0) { puts_(&b, "0.0"); return done(&b); }
    char d[40]; int dp, n = digits(x, 0, 0, d, &dp);
    if (dp > -4 && dp <= 16) {
        if (dp <= 0) { puts_(&b, "0."); for (int i = 0; i < -dp; i++) put(&b, '0'); for (int i = 0; i < n; i++) put(&b, d[i]); }
        else {
            for (int i = 0; i < dp; i++) put(&b, i < n ? d[i] : '0');
            put(&b, '.');
            if (n > dp) for (int i = dp; i < n; i++) put(&b, d[i]); else put(&b, '0');
        }
    } else {
        put(&b, d[0]);
        if (n > 1) { put(&b, '.'); for (int i = 1; i < n; i++) put(&b, d[i]); }
        put_exp(&b, dp - 1, 0, 2);
    }
    return done(&b);
}

int num_fixed(double x, int prec, char *out, int cap) {
    int sp = special(x, out, cap, 0, 0);
    if (sp >= 0) return sp;
    if (prec < 0) prec = 6;
    if (prec > 100) prec = 100;
    sb_t b = { out, 0, cap };
    if (__builtin_signbit(x)) { put(&b, '-'); x = -x; }
    static char d[420];
    int dp = 0, n = x != 0 ? digits(x, 2, prec, d, &dp) : 0;
    /* d[0..n) x 10^(dp - n) */
    if (dp <= 0) put(&b, '0');
    else for (int i = 0; i < dp; i++) put(&b, i < n ? d[i] : '0');
    if (prec) {
        put(&b, '.');
        for (int i = 0; i < prec; i++) {
            int pos = dp + i;                                /* digit index */
            put(&b, pos >= 0 && pos < n ? d[pos] : '0');
        }
    }
    return done(&b);
}

int num_exp(double x, int prec, char *out, int cap, int upper) {
    int sp = special(x, out, cap, upper, 0);
    if (sp >= 0) return sp;
    if (prec < 0) prec = 6;
    if (prec > 100) prec = 100;
    sb_t b = { out, 0, cap };
    if (__builtin_signbit(x)) { put(&b, '-'); x = -x; }
    char d[110]; int dp = 1, n;
    if (x == 0) { n = prec + 1; memset(d, '0', (size_t)n); }
    else n = digits(x, 1, prec + 1, d, &dp);
    put(&b, d[0]);
    if (prec) { put(&b, '.'); for (int i = 1; i < n; i++) put(&b, d[i]); }
    put_exp(&b, x == 0 ? 0 : dp - 1, upper, 2);
    return done(&b);
}

int num_general(double x, int prec, char *out, int cap, int upper, int alt) {
    int sp = special(x, out, cap, upper, 0);
    if (sp >= 0) return sp;
    if (prec < 0) prec = 6;
    if (prec == 0) prec = 1;
    if (prec > 100) prec = 100;
    int ex = 0;
    if (x != 0) { char d[110]; int dp; digits(x < 0 ? -x : x, 1, prec, d, &dp); ex = dp - 1; }
    int n;
    if (ex < -4 || ex >= prec) n = num_exp(x, prec - 1, out, cap, upper);
    else n = num_fixed(x, prec - 1 - ex, out, cap);
    if (!alt && strchr(out, '.')) {
        /* no trailing zeros, nor a bare point */
        char *e = strchr(out, upper ? 'E' : 'e');
        char tail[16] = "";
        if (e) { strlcpy_(tail, e, sizeof tail); *e = 0; }
        int l = (int)strlen(out);
        while (l > 0 && out[l - 1] == '0') out[--l] = 0;
        if (l > 0 && out[l - 1] == '.') out[--l] = 0;
        n = strlcpy_(out + l, tail, cap - l) + l;
    }
    return n;
}

/* ---- from text ----------------------------------------------------------------------------------- */
static double next_up(double x) { dbits b; b.d = x; if (x >= 0) b.u++; else b.u--; return b.d; }
static double next_down(double x) { dbits b; b.d = x; if (x > 0) b.u--; else b.u++; return b.d; }

/* M x 10^E against the midpoint (2f+1) x 2^(e-1) above z = f x 2^e. */
static int cmp_mid(const big *M, int E, double z) {
    dbits bz; bz.d = z;
    int be = (int)((bz.u >> 52) & 0x7FF);
    uint64_t f = bz.u & ((1ull << 52) - 1);
    int e;
    if (be) { f |= 1ull << 52; e = be - 1075; } else e = -1074;
    big l = *M, r;
    b_set(&r, 2 * f + 1);
    if (E >= 0) b_pow10(&l, E); else b_pow10(&r, -E);
    if (e - 1 >= 0) b_shl(&r, e - 1); else b_shl(&l, 1 - e);
    return b_cmp(&l, &r);
}

double num_parse(const char *s, const char **end) {
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if ((p[0] | 32) == 'i' && (p[1] | 32) == 'n' && (p[2] | 32) == 'f') {
        p += 3;
        if (!strncasecmp(p, "inity", 5)) p += 5;
        if (end) *end = p;
        return neg ? -__builtin_inf() : __builtin_inf();
    }
    if ((p[0] | 32) == 'n' && (p[1] | 32) == 'a' && (p[2] | 32) == 'n') { if (end) *end = p + 3; return __builtin_nan(""); }
    char dg[48]; int nd = 0, dexp = 0, any = 0, sticky = 0;
    while (*p >= '0' && *p <= '9') {
        any = 1;
        if (nd || *p != '0') { if (nd < 40) dg[nd++] = *p; else { dexp++; if (*p != '0') sticky = 1; } }
        p++;
    }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            any = 1;
            if (nd || *p != '0') { if (nd < 40) { dg[nd++] = *p; dexp--; } else if (*p != '0') sticky = 1; }
            else dexp--;
            p++;
        }
    }
    if (!any) { if (end) *end = s; return 0; }
    if ((*p | 32) == 'e') {
        const char *q = p + 1;
        int en = 0, ev = 0;
        if (*q == '+' || *q == '-') en = *q++ == '-';
        if (*q >= '0' && *q <= '9') {
            while (*q >= '0' && *q <= '9') { if (ev < 100000) ev = ev * 10 + (*q - '0'); q++; }
            dexp += en ? -ev : ev;
            p = q;
        }
    }
    if (end) *end = p;
    if (!nd) return neg ? -0.0 : 0.0;
    if (nd + dexp > 310) return neg ? -__builtin_inf() : __builtin_inf();
    if (nd + dexp < -330) return neg ? -0.0 : 0.0;
    /* exact for small cases */
    static const double p10[23] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14,
                                    1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
    uint64_t m = 0;
    int take = nd < 19 ? nd : 19;
    for (int i = 0; i < take; i++) m = m * 10 + (uint64_t)(dg[i] - '0');
    if (nd <= 15 && dexp >= -22 && dexp <= 22) {
        double z = (double)m;
        z = dexp >= 0 ? z * p10[dexp] : z / p10[-dexp];
        return neg ? -z : z;
    }
    /* a guess, then exact comparisons */
    double z = (double)m;
    int e = dexp + (nd - take);
    while (e > 22) { z *= 1e22; e -= 22; }
    while (e < -22) { z /= 1e22; e += 22; }
    z = e >= 0 ? z * p10[e] : z / p10[-e];
    if (z == 0) z = 4.9406564584124654e-324;
    if (z > 1.7976931348623157e308) z = 1.7976931348623157e308;
    big M; b_set(&M, 0);
    for (int i = 0; i < nd; i++) { b_mul(&M, 10); big d1; b_set(&d1, (uint64_t)(dg[i] - '0')); b_add(&M, &d1); }
    for (int it = 0; it < 64; it++) {
        int up = cmp_mid(&M, dexp, z);
        if (up > 0 || (up == 0 && (sticky || ((dbits){ .d = z }.u & 1)))) {
            if (z == 1.7976931348623157e308) { z = __builtin_inf(); break; }
            z = next_up(z); continue;
        }
        double lo = next_down(z);
        if (lo > 0) {
            int dn = cmp_mid(&M, dexp, lo);
            if (dn < 0 || (dn == 0 && !sticky && !((dbits){ .d = lo }.u & 1))) { z = lo; continue; }
        } else if (lo == 0 && z > 0) {
            int dn = cmp_mid(&M, dexp, 0.0);
            if (dn < 0) { z = 0; break; }
        }
        break;
    }
    return neg ? -z : z;
}
