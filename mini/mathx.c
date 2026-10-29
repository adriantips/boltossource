/* ===========================================================================
 *  BoltOS Mini  -  mini/mathx.c
 *
 *  Elementary functions to within an ulp or so, for the programs the Code
 *  app runs. The shared libc's are quick approximations made for drawing
 *  (a curve doesn't mind the 15th digit); a program printing 25 ** 0.5
 *  has to get 5.0. These follow the classic fdlibm methods: reduce the
 *  argument exactly, with the constant split in a high and a low part,
 *  then a minimax polynomial on what's left.
 *
 *    exp:   x = k ln2 + r, |r| <= ln2/2;   exp(r) from a rational form
 *    log:   x = 2^k (1+f);  log(1+f) from s = f/(2+f)
 *    pow:   log2(x) in two doubles, times y in two, then 2^that
 *    sin, cos, tan: x = n pi/2 + r with pi/2 in three parts; kernels on r
 *    atan:  four breakpoints, then a polynomial; atan2, asin, acos from it
 * ===========================================================================*/
#include <stdint.h>
#include "mathx.h"

typedef union { double d; uint64_t u; } db;
static inline uint32_t hi32(double x) { db b; b.d = x; return (uint32_t)(b.u >> 32); }
static inline uint32_t lo32(double x) { db b; b.d = x; return (uint32_t)b.u; }
static inline double mk(uint32_t h, uint32_t l) { db b; b.u = (uint64_t)h << 32 | l; return b.d; }
static inline double with_lo0(double x) { db b; b.d = x; b.u &= 0xFFFFFFFF00000000ull; return b.d; }
static inline double fabs_(double x) { return __builtin_fabs(x); }
static double scalb(double x, int n) {
    /* x * 2^n without overflow on the way */
    while (n > 1000) { x *= 0x1p1000; n -= 1000; }
    while (n < -1000) { x *= 0x1p-1000; n += 1000; }
    db b; b.u = (uint64_t)(0x3FF + n) << 52;
    return x * b.d;
}

static const double
    ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10,
    P1 = 1.66666666666666019037e-01, P2 = -2.77777777770155933842e-03, P3 = 6.61375632143793436117e-05,
    P4 = -1.65339022054652515390e-06, P5 = 4.13813679705723846039e-08,
    Lg1 = 6.666666666666735130e-01, Lg2 = 3.999999999940941908e-01, Lg3 = 2.857142874366239149e-01,
    Lg4 = 2.222219843214978396e-01, Lg5 = 1.818357216161805012e-01, Lg6 = 1.531383769920937332e-01,
    Lg7 = 1.479819860511658591e-01;

/* ---- two doubles as one number, for exp and log rounded right ---------------------------------- */
typedef struct { double hi, lo; } dd;
static dd two_sum(double a, double b) { double s = a + b, bb = s - a; dd r = { s, (a - (s - bb)) + (b - bb) }; return r; }
static dd fast_sum(double a, double b) { double s = a + b; dd r = { s, b - (s - a) }; return r; }
static inline double fma_(double a, double b, double c) { double r; __asm__("fmadd %d0, %d1, %d2, %d3" : "=w"(r) : "w"(a), "w"(b), "w"(c)); return r; }
static dd two_prod(double a, double b) { double p = a * b; dd r = { p, fma_(a, b, -p) }; return r; }
static dd dd_add(dd a, dd b) { dd s = two_sum(a.hi, b.hi); s.lo += a.lo + b.lo; return fast_sum(s.hi, s.lo); }
static dd dd_mul(dd a, dd b) { dd p = two_prod(a.hi, b.hi); p.lo += a.hi * b.lo + a.lo * b.hi; return fast_sum(p.hi, p.lo); }
static dd dd_div_d(dd a, double b) {
    double q1 = a.hi / b;
    dd p = two_prod(q1, b);
    double q2 = ((a.hi - p.hi) - p.lo + a.lo) / b;
    return fast_sum(q1, q2);
}
static const dd LN2 = { 6.93147180559945286227e-01, 2.319046813846299558e-17 };
static const dd INV_LN2 = { 1.4426950408889634, 2.0355273740931033e-17 };
static const dd INV_LN10 = { 0.4342944819032518, 1.0983196502167651e-17 };

/* exp(x) as 2^k * (hi + lo): the reduced part by its Taylor series in two doubles */
static dd exp_dd(dd x, int *k) {
    double kd = __builtin_rint(x.hi * 1.4426950408889634);
    *k = (int)kd;
    dd t = two_prod(kd, LN2.hi);
    t.lo += kd * LN2.lo;
    dd r = dd_add(x, (dd){ -t.hi, -t.lo });
    dd sum = { 1.0, 0.0 }, term = { 1.0, 0.0 };
    for (int n = 1; n < 27; n++) {
        term = dd_div_d(dd_mul(term, r), (double)n);
        sum = dd_add(sum, term);
        if (__builtin_fabs(term.hi) < 1e-36) break;
    }
    return sum;
}

double x_exp(double x) {
    if (x != x) return x;
    if (x > 709.782712893383973096) return __builtin_inf();
    if (x < -745.13321910194110842) return 0.0;
    if (__builtin_fabs(x) < 0x1p-54) return 1.0 + x;
    int k;
    dd e = exp_dd((dd){ x, 0 }, &k);
    if (k > -1000) return scalb(e.hi + e.lo, k);
    /* deep in the subnormals: scaled in two steps, rounding once */
    return scalb(scalb(e.hi + e.lo, k + 60), -60);
}


static double fd_log(double x);
/* log(x) in two doubles: fdlibm's, then one Newton step against exp in two doubles */
static dd log_dd(double x) {
    int k = 0;
    uint32_t hx = hi32(x);
    if (hx < 0x00100000) { x *= 0x1p54; k -= 54; hx = hi32(x); }
    k += (int)(hx >> 20) - 1023;
    double m = mk((hx & 0x000FFFFF) | 0x3FF00000, lo32(x));
    if (m > 1.4142135623730951) { m *= 0.5; k++; }
    double y0 = fd_log(m);
    int ek;
    dd e = exp_dd((dd){ -y0, 0 }, &ek);                 /* exp(-y0) = 2^ek * e */
    e.hi = scalb(e.hi, ek); e.lo = scalb(e.lo, ek);
    dd me = two_prod(m, e.hi);
    me.lo += m * e.lo;
    dd corr = dd_add(me, (dd){ -1.0, 0 });             /* m e^-y0 - 1, tiny */
    double c = corr.hi + corr.lo;
    dd y = two_sum(y0, c - c * c * 0.5);
    dd kl = two_prod((double)k, LN2.hi);
    kl.lo += (double)k * LN2.lo;
    return dd_add(kl, y);
}

double x_log(double x) {
    if (x != x) return x;
    if (x < 0) return __builtin_nan("");
    if (x == 0) return -__builtin_inf();
    if (x == __builtin_inf()) return x;
    if (x == 1.0) return 0.0;
    dd r = log_dd(x);
    return r.hi + r.lo;
}

static double fd_log(double x) {
    if (x != x) return x;
    if (x < 0) return __builtin_nan("");
    if (x == 0) return -__builtin_inf();
    if (x == __builtin_inf()) return x;
    int k = 0;
    uint32_t hx = hi32(x);
    if (hx < 0x00100000) { x *= 0x1p54; k -= 54; hx = hi32(x); }
    k += (int)(hx >> 20) - 1023;
    hx &= 0x000FFFFF;
    /* m in [sqrt(2)/2, sqrt(2)) */
    uint32_t i = (hx + 0x95F64) & 0x100000;
    double m = mk(hx | (i ^ 0x3FF00000), lo32(x));
    k += (int)(i >> 20);
    double f = m - 1.0;
    double s = f / (2.0 + f), z = s * s, w = z * z;
    double t1 = w * (Lg2 + w * (Lg4 + w * Lg6)), t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    double R = t2 + t1, hfsq = 0.5 * f * f;
    double dk = k;
    return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);
}

double x_log2(double x) {
    if (x > 0 && x != __builtin_inf()) {
        db b; b.d = x;
        if (!(b.u & 0x000FFFFFFFFFFFFFull) && (b.u >> 52)) return (double)((int)(b.u >> 52) - 1023);
    }
    if (!(x > 0) || x == __builtin_inf()) return x_log(x);
    dd r = dd_mul(log_dd(x), INV_LN2);
    return r.hi + r.lo;
}

double x_log10(double x) {
    if (!(x > 0) || x == __builtin_inf()) return x_log(x);
    dd r = dd_mul(log_dd(x), INV_LN10);
    double v = r.hi + r.lo;
    /* powers of ten land exactly on their exponent */
    double rv = __builtin_rint(v);
    if (v != rv && __builtin_fabs(v - rv) < 1e-15 * (__builtin_fabs(rv) + 1)) {
        double p = x_pow(10.0, rv);
        if (p == x) return rv;
    }
    return v;
}

double x_expm1(double x) {
    if (fabs_(x) < 1e-5) return x + x * x * 0.5 + x * x * x / 6.0;
    if (fabs_(x) < 700) {
        int k;
        dd e = exp_dd((dd){ x, 0 }, &k);
        e.hi = scalb(e.hi, k); e.lo = scalb(e.lo, k);
        dd r = dd_add(e, (dd){ -1.0, 0 });
        return r.hi + r.lo;
    }
    double u = x_exp(x);
    if (u == 1.0) return x;
    double um1 = u - 1.0;
    if (um1 == -1.0) return -1.0;
    if (u == __builtin_inf()) return u;
    return um1 * x / x_log(u);
}

double x_log1p(double x) {
    if (x == -1.0) return -__builtin_inf();
    if (x < -1.0 || x != x) return __builtin_nan("");
    double u = 1.0 + x;
    if (u == 1.0) return x;
    /* log(u) and the part of x that u lost */
    dd l = log_dd(u);
    double lost = (x - (u - 1.0)) / u;
    return l.hi + (l.lo + lost);
}

/* ---- pow ------------------------------------------------------------------------------------- */
static const double
    bp[2] = { 1.0, 1.5 }, dp_h[2] = { 0.0, 5.84962487220764160156e-01 }, dp_l[2] = { 0.0, 1.35003920212974897128e-08 },
    two53 = 9007199254740992.0,
    L1 = 5.99999999999994648725e-01, L2 = 4.28571428578550184252e-01, L3 = 3.33333329818377432918e-01,
    L4 = 2.72728123808534006489e-01, L5 = 2.30660745775561754067e-01, L6 = 2.06975017800338417784e-01,
    lg2 = 6.93147180559945286227e-01, lg2_h = 6.93147182464599609375e-01, lg2_l = -1.90465429995776804525e-09,
    ovt = 8.0085662595372944372e-17, cp = 9.61796693925975554329e-01, cp_h = 9.61796700954437255859e-01,
    cp_l = -7.02846165095275826516e-09, ivln2 = 1.44269504088896338700e+00, ivln2_h = 1.44269502162933349609e+00,
    ivln2_l = 1.92596299112661746887e-08;

double x_pow(double x, double y) {
    int32_t hx = (int32_t)hi32(x), hy = (int32_t)hi32(y);
    uint32_t lx = lo32(x), ly = lo32(y);
    int32_t ix = hx & 0x7FFFFFFF, iy = hy & 0x7FFFFFFF;
    if ((iy | ly) == 0) return 1.0;
    if (x == 1.0) return 1.0;
    if (ix > 0x7FF00000 || (ix == 0x7FF00000 && lx) || iy > 0x7FF00000 || (iy == 0x7FF00000 && ly)) return x + y;
    /* is y an integer, odd or even? */
    int yisint = 0;
    if (hx < 0) {
        if (iy >= 0x43400000) yisint = 2;
        else if (iy >= 0x3FF00000) {
            int k = (iy >> 20) - 0x3FF;
            if (k > 20) { uint32_t j = ly >> (52 - k); if ((j << (52 - k)) == ly) yisint = 2 - (int)(j & 1); }
            else if (ly == 0) { int32_t j = iy >> (20 - k); if ((j << (20 - k)) == iy) yisint = 2 - (j & 1); }
        }
    }
    if (ly == 0) {
        if (iy == 0x7FF00000) {                       /* y is +-inf */
            if (((ix - 0x3FF00000) | lx) == 0) return 1.0;
            if (ix >= 0x3FF00000) return hy >= 0 ? y : 0.0;
            return hy < 0 ? -y : 0.0;
        }
        if (iy == 0x3FF00000) return hy < 0 ? 1.0 / x : x;
        if (hy == 0x40000000) return x * x;
        if (hy == 0x3FE00000 && hx >= 0) return __builtin_sqrt(x);
    }
    double ax = fabs_(x);
    if (lx == 0 && (ix == 0x7FF00000 || ix == 0 || ix == 0x3FF00000)) {
        double z = ax;
        if (hy < 0) z = 1.0 / z;
        if (hx < 0) {
            if (((ix - 0x3FF00000) | yisint) == 0) z = (z - z) / (z - z);
            else if (yisint == 1) z = -z;
        }
        return z;
    }
    double sn = 1.0;
    if (hx < 0) {
        if (yisint == 0) return (x - x) / (x - x);
        if (yisint == 1) sn = -1.0;
    }
    double t1, t2;
    if (iy > 0x41E00000) {                            /* |y| huge */
        if (iy > 0x43F00000) { if (ix <= 0x3FEFFFFF) return hy < 0 ? __builtin_inf() : 0.0; return hy > 0 ? __builtin_inf() : 0.0; }
        if (ix < 0x3FEFFFFF) return hy < 0 ? sn * __builtin_inf() : sn * 0.0;
        if (ix > 0x3FF00000) return hy > 0 ? sn * __builtin_inf() : sn * 0.0;
        double t = ax - 1.0;
        double w = (t * t) * (0.5 - t * (0.3333333333333333333333 - t * 0.25));
        double u = ivln2_h * t, v = t * ivln2_l - w * ivln2;
        t1 = with_lo0(u + v);
        t2 = v - (t1 - u);
    } else {
        int n = 0;
        if (ix < 0x00100000) { ax *= two53; n -= 53; ix = (int32_t)hi32(ax); }
        n += ((ix) >> 20) - 0x3FF;
        int32_t j = ix & 0x000FFFFF;
        ix = j | 0x3FF00000;
        int k;
        if (j <= 0x3988E) k = 0;
        else if (j < 0xBB67A) k = 1;
        else { k = 0; n += 1; ix -= 0x00100000; }
        ax = mk((uint32_t)ix, lo32(ax));
        double u = ax - bp[k], v = 1.0 / (ax + bp[k]);
        double ss = u * v, s_h = with_lo0(ss);
        double t_h = mk((uint32_t)(((ix >> 1) | 0x20000000) + 0x00080000 + (k << 18)), 0);
        double t_l = ax - (t_h - bp[k]);
        double s_l = v * ((u - s_h * t_h) - s_h * t_l);
        double s2 = ss * ss;
        double r = s2 * s2 * (L1 + s2 * (L2 + s2 * (L3 + s2 * (L4 + s2 * (L5 + s2 * L6)))));
        r += s_l * (s_h + ss);
        s2 = s_h * s_h;
        t_h = with_lo0(3.0 + s2 + r);
        t_l = r - ((t_h - 3.0) - s2);
        u = s_h * t_h; v = s_l * t_h + t_l * ss;
        double p_h = with_lo0(u + v), p_l = v - (p_h - u);
        double z_h = cp_h * p_h, z_l = cp_l * p_h + p_l * cp + dp_l[k];
        double t = (double)n;
        t1 = with_lo0(((z_h + z_l) + dp_h[k]) + t);
        t2 = z_l - (((t1 - t) - dp_h[k]) - z_h);
    }
    /* y * log2(x), in two parts */
    double y1 = with_lo0(y);
    double p_l = (y - y1) * t1 + y * t2, p_h = y1 * t1, z = p_l + p_h;
    int32_t j = (int32_t)hi32(z), i = (int32_t)lo32(z);
    if (j >= 0x40900000) {
        if (((j - 0x40900000) | i) != 0 || p_l + ovt > z - p_h) return sn * __builtin_inf();
    } else if ((j & 0x7FFFFFFF) >= 0x4090CC00) {
        if (((j - (int32_t)0xC090CC00) | i) != 0 || p_l <= z - p_h) return sn * 0.0;
    }
    i = j & 0x7FFFFFFF;
    int k = (i >> 20) - 0x3FF, n = 0;
    if (i > 0x3FE00000) {
        n = j + (0x00100000 >> (k + 1));
        k = ((n & 0x7FFFFFFF) >> 20) - 0x3FF;
        double t = mk((uint32_t)(n & ~(0x000FFFFF >> k)), 0);
        n = ((n & 0x000FFFFF) | 0x00100000) >> (20 - k);
        if (j < 0) n = -n;
        p_h -= t;
    }
    double t = with_lo0(p_l + p_h);
    double u = t * lg2_h, v = (p_l - (t - p_h)) * lg2 + t * lg2_l;
    z = u + v;
    double w = v - (z - u);
    t = z * z;
    t1 = z - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    double r = (z * t1) / (t1 - 2.0) - (w + z * w);
    z = 1.0 - (r - z);
    j = (int32_t)hi32(z);
    j += n << 20;
    if ((j >> 20) <= 0) z = scalb(z, n);
    else z = mk((uint32_t)j, lo32(z));
    return sn * z;
}

/* ---- sin, cos, tan --------------------------------------------------------------------------- */
static const double
    S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03, S3 = -1.98412698298579493134e-04,
    S4 = 2.75573137070700676789e-06, S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10,
    C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03, C3 = 2.48015872894767294178e-05,
    C4 = -2.75573143513906633035e-07, C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11,
    invpio2 = 6.36619772367581382433e-01,
    pio2_1 = 1.57079632673412561417e+00, pio2_1t = 6.07710050650619224932e-11,
    pio2_2 = 6.07710050630396597660e-11, pio2_2t = 2.02226624879595063154e-21,
    pio2_3 = 2.02226624871116645580e-21, pio2_3t = 8.47842766036889956997e-32;

static double ksin(double x, double y, int iy) {
    double z = x * x, v = z * x;
    double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    if (!iy) return x + v * (S1 + z * r);
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

static double kcos(double x, double y) {
    double z = x * x;
    double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    double hz = 0.5 * z, w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + (z * r - x * y));
}

/* x = n pi/2 + (y0 + y1) */
static int rem_pio2(double x, double *y0, double *y1) {
    double ax = fabs_(x);
    if (ax <= 7.85398163397448278999e-01) { *y0 = x; *y1 = 0; return 0; }
    double fn = __builtin_rint(x * invpio2);
    int n = (int)fn;
    double r = x - fn * pio2_1, w = fn * pio2_1t;
    double y = r - w;
    /* more bits of pi/2 when they cancel */
    uint32_t j = hi32(x) >> 20 & 0x7FF;
    if ((int)j - (int)(hi32(y) >> 20 & 0x7FF) > 16) {
        double t = r;
        w = fn * pio2_2; r = t - w; w = fn * pio2_2t - ((t - r) - w); y = r - w;
        if ((int)j - (int)(hi32(y) >> 20 & 0x7FF) > 49) {
            t = r; w = fn * pio2_3; r = t - w; w = fn * pio2_3t - ((t - r) - w); y = r - w;
        }
    }
    *y0 = y; *y1 = (r - y) - w;
    return n;
}

double x_sin(double x) {
    if (x != x || fabs_(x) == __builtin_inf()) return __builtin_nan("");
    if (fabs_(x) < 0x1p-27) return x;
    double a, b;
    int n = rem_pio2(x, &a, &b);
    switch (n & 3) {
    case 0: return ksin(a, b, 1);
    case 1: return kcos(a, b);
    case 2: return -ksin(a, b, 1);
    default: return -kcos(a, b);
    }
}

double x_cos(double x) {
    if (x != x || fabs_(x) == __builtin_inf()) return __builtin_nan("");
    if (fabs_(x) < 0x1p-27) return 1.0;
    double a, b;
    int n = rem_pio2(x, &a, &b);
    switch (n & 3) {
    case 0: return kcos(a, b);
    case 1: return -ksin(a, b, 1);
    case 2: return -kcos(a, b);
    default: return ksin(a, b, 1);
    }
}

double x_tan(double x) {
    if (x != x || fabs_(x) == __builtin_inf()) return __builtin_nan("");
    if (fabs_(x) < 0x1p-27) return x;
    double a, b;
    int n = rem_pio2(x, &a, &b);
    double s = ksin(a, b, 1), c = kcos(a, b);
    return (n & 1) ? -c / s : s / c;
}

/* ---- atan and friends ------------------------------------------------------------------------- */
static const double atanhi[4] = { 4.63647609000806093515e-01, 7.85398163397448278999e-01, 9.82793723247329054082e-01, 1.57079632679489655800e+00 };
static const double atanlo[4] = { 2.26987774529616870924e-17, 3.06161699786838301793e-17, 1.39033110312309984516e-17, 6.12323399573676603587e-17 };
static const double aT[11] = { 3.33333333333329318027e-01, -1.99999999998764832476e-01, 1.42857142725034663711e-01,
    -1.11111104054623557880e-01, 9.09088713343650656196e-02, -7.69187620504482999495e-02, 6.66107313738753120669e-02,
    -5.83357013379057348645e-02, 4.97687799461593236017e-02, -3.65315727442169155270e-02, 1.62858201153657823623e-02 };

double x_atan(double x) {
    if (x != x) return x;
    double ax = fabs_(x);
    int id;
    if (ax >= 0x1p66) return x > 0 ? atanhi[3] + atanlo[3] : -atanhi[3] - atanlo[3];
    if (ax < 0.4375) { if (ax < 0x1p-29) return x; id = -1; }
    else {
        if (ax < 1.1875) {
            if (ax < 0.6875) { id = 0; ax = (2.0 * ax - 1.0) / (2.0 + ax); }
            else { id = 1; ax = (ax - 1.0) / (ax + 1.0); }
        } else {
            if (ax < 2.4375) { id = 2; ax = (ax - 1.5) / (1.0 + 1.5 * ax); }
            else { id = 3; ax = -1.0 / ax; }
        }
    }
    double z = ax * ax, w = z * z;
    double s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    double s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if (id < 0) return (x < 0 ? -1 : 1) * (ax - ax * (s1 + s2));
    z = atanhi[id] - ((ax * (s1 + s2) - atanlo[id]) - ax);
    return x < 0 ? -z : z;
}

double x_atan2(double y, double x) {
    static const double pi = 3.1415926535897931160E+00, pi_lo = 1.2246467991473531772E-16;
    if (x != x || y != y) return x + y;
    if (x == 1.0) return x_atan(y);
    int sy = __builtin_signbit(y), sx = __builtin_signbit(x);
    if (y == 0) return sx ? (sy ? -pi : pi) : y;
    if (x == 0) return sy ? -pi / 2 : pi / 2;
    if (fabs_(x) == __builtin_inf()) {
        if (fabs_(y) == __builtin_inf()) return (sy ? -1 : 1) * (sx ? 3 * pi / 4 : pi / 4);
        return sx ? (sy ? -pi : pi) : (sy ? -0.0 : 0.0);
    }
    if (fabs_(y) == __builtin_inf()) return sy ? -pi / 2 : pi / 2;
    double z = x_atan(fabs_(y / x));
    if (!sx) return sy ? -z : z;
    z = pi - (z - pi_lo);
    return sy ? -z : z;
}

double x_asin(double x) {
    double ax = fabs_(x);
    if (ax > 1) return __builtin_nan("");
    if (ax == 1) return x * 1.57079632679489661923;
    return x_atan2(x, __builtin_sqrt((1.0 - x) * (1.0 + x)));
}

double x_acos(double x) {
    double ax = fabs_(x);
    if (ax > 1) return __builtin_nan("");
    return x_atan2(__builtin_sqrt((1.0 - x) * (1.0 + x)), x);
}

double x_sinh(double x) {
    double ax = fabs_(x);
    if (ax < 1e-8) return x;
    if (ax < 1) { double e = x_expm1(ax); double r = (e + e / (e + 1)) * 0.5; return x < 0 ? -r : r; }
    if (ax > 710) return x < 0 ? -__builtin_inf() : __builtin_inf();
    double e = x_exp(ax * 0.5), r = 0.5 * e * e - 0.5 / (e * e);
    return x < 0 ? -r : r;
}

double x_cosh(double x) {
    double ax = fabs_(x);
    if (ax > 710) return __builtin_inf();
    if (ax < 1) { double e = x_expm1(ax); return 1.0 + e * e / (2 * (1 + e)); }
    double e = x_exp(ax * 0.5);
    return 0.5 * e * e + 0.5 / (e * e);
}

double x_tanh(double x) {
    double ax = fabs_(x);
    if (ax > 22) return x < 0 ? -1.0 : 1.0;
    if (ax < 1e-8) return x;
    double e = x_expm1(2 * ax), r = e / (e + 2);
    return x < 0 ? -r : r;
}

double x_cbrt(double x) {
    if (x == 0 || x != x || fabs_(x) == __builtin_inf()) return x;
    double ax = fabs_(x);
    double r = x_exp(x_log(ax) / 3.0);
    r = r - (r * r * r - ax) / (3.0 * r * r);               /* one Newton step */
    return x < 0 ? -r : r;
}

double x_hypot(double a, double b) {
    a = fabs_(a); b = fabs_(b);
    if (a == __builtin_inf() || b == __builtin_inf()) return __builtin_inf();
    if (a < b) { double t = a; a = b; b = t; }
    if (a == 0) return 0;
    double r = b / a;
    return a * __builtin_sqrt(1 + r * r);
}

double x_fmod(double x, double y) {
    if (y == 0 || x != x || y != y || fabs_(x) == __builtin_inf()) return __builtin_nan("");
    if (fabs_(y) == __builtin_inf()) return x;
    /* exact: take y * 2^k off while it fits */
    double ax = fabs_(x), ay = fabs_(y);
    if (ax < ay) return x;
    while (ax >= ay) {
        double t = ay;
        int e1, e2;
        __builtin_frexp(ax, &e1); __builtin_frexp(t, &e2);
        t = __builtin_ldexp(t, e1 - e2);
        if (t > ax) t *= 0.5;
        ax -= t;
    }
    return x < 0 ? -ax : ax;
}
