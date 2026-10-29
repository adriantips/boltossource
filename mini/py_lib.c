/* ===========================================================================
 *  BoltOS Mini  -  mini/py_lib.c
 *
 *  What a Python program finds without importing anything -- print, len,
 *  range, sorted, the types and the exceptions -- the methods of str,
 *  list, dict, set and files, and the modules: math, random, time, sys,
 *  os, string and json here in C; collections, itertools, functools and
 *  typing written in Python below, and read the first time they're
 *  imported.
 * ===========================================================================*/
#include <stdint.h>
#include "py.h"
#include "string.h"
#include "stdio.h"
#include "math.h"
#define MATHX_REPLACE
#include "mathx.h"
#include "stdlib.h"

#define ARGS V *a, int n, V *kwn, V *kwv, int nk
#define UNUSED_KW (void)kwn; (void)kwv; (void)nk

V native(const char *name, native_fn fn) {
    Native *x = rt_alloc(sizeof(Native));
    x->type = O_NATIVE; x->name = name; x->fn = fn;
    return vobj(x);
}

static int kwarg(V *kwn, V *kwv, int nk, const char *name, V *out) {
    for (int i = 0; i < nk; i++) if (is_obj(kwn[i], O_STR) && !strcmp(as_str(kwn[i])->s, name)) { *out = kwv[i]; return 1; }
    return 0;
}

static void need(int n, int lo, int hi, const char *fn) {
    if (n < lo || n > hi) {
        if (lo == hi) raise_new(P->e_type, "%s() takes exactly %d argument%s (%d given)", fn, lo, lo == 1 ? "" : "s", n);
        raise_new(P->e_type, "%s() takes %d to %d arguments (%d given)", fn, lo, hi, n);
    }
}

/* A subclass of dict or list hands its own one over. */
static V SELF(V v) {
    if (is_obj(v, O_INST) && ((Inst *)v.u.o)->native.t == V_OBJ && ((Inst *)v.u.o)->cls->builtin) return ((Inst *)v.u.o)->native;
    return v;
}

static int64_t to_i64(V v, const char *what) {
    int64_t i;
    if (fits_i64(v, &i)) return i;
    if (is_obj(v, O_BIG)) raise_new(P->e_overflow, "%s is too large", what);
    raise_new(P->e_type, "'%s' object cannot be interpreted as an integer", type_name(v));
    return 0;
}

static double to_f(V v) {
    if (v.t == V_FLOAT) return v.u.f;
    if (is_int(v)) return int_to_double(v);
    V m;
    if (is_obj(v, O_INST) && lookup_special(v, P->s_float, &m)) return to_f(call0(m));
    raise_new(P->e_type, "must be real number, not %s", type_name(v));
    return 0;
}

static Str *arg_str(V v, const char *fn) {
    if (!is_obj(v, O_STR)) raise_new(P->e_type, "%s: expected str, got %s", fn, type_name(v));
    return as_str(v);
}

V seq_from(V it) {
    if (is_obj(it, O_LIST)) {
        List *s = as_list(it), *l = list_new(s->len);
        memcpy(l->v, s->v, sizeof(V) * s->len); l->len = s->len;
        return vobj(l);
    }
    List *l = list_new(8);
    V i = iter_of(it), x;
    while (iter_next(i, &x)) list_push(l, x);
    return vobj(l);
}

static int64_t len_of(V v) {
    if (v.t == V_OBJ) {
        switch (v.u.o->type) {
        case O_STR: return as_str(v)->len;
        case O_LIST: return as_list(v)->len;
        case O_TUPLE: return ((Tuple *)v.u.o)->len;
        case O_DICT: case O_SET: return as_dict(v)->n;
        case O_RANGE: {
            Range *r = (Range *)v.u.o;
            return r->step > 0 ? (r->stop > r->start ? (r->stop - r->start + r->step - 1) / r->step : 0)
                               : (r->start > r->stop ? (r->start - r->stop - r->step - 1) / -r->step : 0);
        }
        case O_INST: {
            V m;
            if (lookup_special(v, P->s_len, &m)) return to_i64(call0(m), "len()");
            if (((Inst *)v.u.o)->cls->builtin && ((Inst *)v.u.o)->native.t == V_OBJ) return len_of(((Inst *)v.u.o)->native);
            break;
        }
        }
    }
    raise_new(P->e_type, "object of type '%s' has no len()", type_name(v));
    return 0;
}

/* ---- sorting: a stable merge sort -------------------------------------------------------------------- */
static int less(V x, V y, int rev) { return rev ? compare(y, x, C_LT) : compare(x, y, C_LT); }

static void msort(V *v, V *k, V *tv, V *tk, uint32_t n, int rev) {
    if (n < 2) return;
    if (n <= 8) {
        for (uint32_t i = 1; i < n; i++)
            for (uint32_t j = i; j > 0 && less(k[j], k[j - 1], rev); j--) {
                V t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
                t = k[j]; k[j] = k[j - 1]; k[j - 1] = t;
            }
        return;
    }
    rt_check();
    uint32_t m = n / 2;
    msort(v, k, tv, tk, m, rev);
    msort(v + m, k + m, tv, tk, n - m, rev);
    uint32_t i = 0, j = m, o = 0;
    while (i < m && j < n) {
        if (less(k[j], k[i], rev)) { tv[o] = v[j]; tk[o++] = k[j++]; }
        else { tv[o] = v[i]; tk[o++] = k[i++]; }
    }
    while (i < m) { tv[o] = v[i]; tk[o++] = k[i++]; }
    while (j < n) { tv[o] = v[j]; tk[o++] = k[j++]; }
    memcpy(v, tv, sizeof(V) * n); memcpy(k, tk, sizeof(V) * n);
}

V list_sort(List *l, V key, int reverse) {
    uint32_t n = l->len;
    V *k = rt_alloc(sizeof(V) * (n ? n : 1)), *tv = rt_alloc(sizeof(V) * (n ? n : 1)), *tk = rt_alloc(sizeof(V) * (n ? n : 1));
    for (uint32_t i = 0; i < n; i++) k[i] = key.t == V_NONE ? l->v[i] : call1(key, l->v[i]);
    msort(l->v, k, tv, tk, n, reverse);
    return vnone();
}

static void sort_kw(V *kwn, V *kwv, int nk, V *key, int *rev) {
    V r;
    *key = vnone(); *rev = 0;
    kwarg(kwn, kwv, nk, "key", key);
    if (kwarg(kwn, kwv, nk, "reverse", &r)) *rev = truthy(r);
}

/* ---- builtins ------------------------------------------------------------------------------------------ */
static V file_write_str(V file, Str *s);

static V b_print(ARGS) {
    V sep = vnone(), end = vnone(), file = vnone();
    kwarg(kwn, kwv, nk, "sep", &sep);
    kwarg(kwn, kwv, nk, "end", &end);
    kwarg(kwn, kwv, nk, "file", &file);
    SB sb; sb_new(&sb, 64);
    for (int i = 0; i < n; i++) {
        if (i) { if (sep.t == V_NONE) sb_addc(&sb, ' '); else sb_addstr(&sb, arg_str(sep, "print")); }
        if (is_obj(a[i], O_STR)) sb_addstr(&sb, as_str(a[i]));
        else repr_into(&sb, a[i], 0);
    }
    if (end.t == V_NONE) sb_addc(&sb, '\n'); else sb_addstr(&sb, arg_str(end, "print"));
    if (is_obj(file, O_FILE)) { file_write_str(file, sb_str(&sb)); return vnone(); }
    rt_write(sb.b, sb.n);
    return vnone();
}

static V b_input(ARGS) {
    UNUSED_KW;
    char buf[4096];
    Str *prompt = n ? to_str(a[0]) : 0;
    if (rt_readline(prompt ? prompt->s : "", buf, sizeof buf) < 0) raise_new(P->e_eof, "EOF when reading a line");
    rt_write(buf, strlen(buf)); rt_write("\n", 1);           /* what was typed stays in the output */
    return vstr(buf);
}

static V b_len(ARGS) { UNUSED_KW; need(n, 1, 1, "len"); return vint(len_of(a[0])); }

static V b_range(ARGS) {
    UNUSED_KW; need(n, 1, 3, "range");
    Range *r = rt_alloc(sizeof(Range));
    r->type = O_RANGE; r->step = 1;
    if (n == 1) { r->start = 0; r->stop = to_i64(a[0], "range()"); }
    else { r->start = to_i64(a[0], "range()"); r->stop = to_i64(a[1], "range()"); if (n == 3) r->step = to_i64(a[2], "range()"); }
    if (!r->step) raise_new(P->e_value, "range() arg 3 must not be zero");
    return vobj(r);
}

static V b_abs(ARGS) {
    UNUSED_KW; need(n, 1, 1, "abs");
    V x = a[0];
    if (x.t == V_FLOAT) return vfloat(fabs(x.u.f));
    if (is_int(x)) return int_cmp(x, vint(0)) < 0 ? int_neg(x.t == V_BOOL ? vint(x.u.i) : x) : (x.t == V_BOOL ? vint(x.u.i) : x);
    V m;
    if (lookup_special(x, P->s_abs, &m)) return call0(m);
    raise_new(P->e_type, "bad operand type for abs(): '%s'", type_name(x));
    return vnone();
}

static V minmax(ARGS, int want_max) {
    V key = vnone(), dflt; int has_d = kwarg(kwn, kwv, nk, "default", &dflt);
    kwarg(kwn, kwv, nk, "key", &key);
    const char *nm = want_max ? "max" : "min";
    if (!n) raise_new(P->e_type, "%s expected at least 1 argument, got 0", nm);
    V src = n == 1 ? a[0] : vnone();
    V best = vnone(), bk = vnone(); int have = 0;
    V it = n == 1 ? iter_of(src) : vnone(), x;
    for (int i = 0; ; i++) {
        if (n == 1) { if (!iter_next(it, &x)) break; }
        else { if (i >= n) break; x = a[i]; }
        V kx = key.t == V_NONE ? x : call1(key, x);
        if (!have || (want_max ? compare(kx, bk, C_GT) : compare(kx, bk, C_LT))) { best = x; bk = kx; have = 1; }
    }
    if (!have) { if (has_d) return dflt; raise_new(P->e_value, "%s() arg is an empty sequence", nm); }
    return best;
}
static V b_min(ARGS) { return minmax(a, n, kwn, kwv, nk, 0); }
static V b_max(ARGS) { return minmax(a, n, kwn, kwv, nk, 1); }

static V b_sum(ARGS) {
    V start = vint(0);
    if (n > 1) start = a[1];
    kwarg(kwn, kwv, nk, "start", &start);
    need(n, 1, 2, "sum");
    if (is_obj(start, O_STR)) raise_new(P->e_type, "sum() can't sum strings [use ''.join(seq) instead]");
    V acc = start, it = iter_of(a[0]), x;
    double fs = 0; int fl = 0;
    while (iter_next(it, &x)) {
        if (x.t == V_FLOAT && (acc.t == V_FLOAT || is_int(acc))) {
            if (!fl) { fs = acc.t == V_FLOAT ? acc.u.f : int_to_double(acc); fl = 1; }
            fs += x.u.f; acc = vfloat(fs); continue;
        }
        if (fl && is_int(x)) { fs += int_to_double(x); acc = vfloat(fs); continue; }
        acc = binop(B_ADD, acc, x);
        fl = 0;
    }
    return acc;
}

static V b_sorted(ARGS) {
    need(n, 1, 1, "sorted");
    V key; int rev;
    sort_kw(kwn, kwv, nk, &key, &rev);
    V l = seq_from(a[0]);
    list_sort(as_list(l), key, rev);
    return l;
}

static Iter *mkiter(int kind, V src) {
    Iter *it = rt_alloc(sizeof(Iter));
    it->type = O_ITER; it->kind = (uint8_t)kind; it->src = src;
    return it;
}

static V b_reversed(ARGS) {
    UNUSED_KW; need(n, 1, 1, "reversed");
    V s = SELF(a[0]);
    V m;
    if (is_obj(a[0], O_INST) && lookup_special(a[0], intern_c("__reversed__"), &m)) return call0(m);
    if (is_obj(s, O_DICT)) s = seq_from(s);
    if (!is_obj(s, O_LIST) && !is_obj(s, O_TUPLE) && !is_obj(s, O_STR) && !is_obj(s, O_RANGE)) s = seq_from(s);
    Iter *it = mkiter(IT_REV, s);
    it->i = len_of(s) - 1;
    return vobj(it);
}

static V b_enumerate(ARGS) {
    need(n, 1, 2, "enumerate");
    V st = n > 1 ? a[1] : vint(0);
    kwarg(kwn, kwv, nk, "start", &st);
    Iter *it = mkiter(IT_ENUM, iter_of(a[0]));
    it->i = to_i64(st, "start");
    return vobj(it);
}

static V b_zip(ARGS) {
    UNUSED_KW;
    Iter *it = mkiter(IT_ZIP, vnone());
    it->more = rt_alloc(sizeof(V) * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++) it->more[i] = iter_of(a[i]);
    it->nmore = n;
    if (!n) it->kind = IT_FILTER, it->src = iter_of(vobj(tuple_new(0)));
    return vobj(it);
}

static V b_map(ARGS) {
    UNUSED_KW;
    if (n < 2) raise_new(P->e_type, "map() must have at least two arguments.");
    Iter *it = mkiter(IT_MAP, vnone());
    it->fn = a[0];
    it->nmore = n - 1 > 8 ? 8 : n - 1;
    it->more = rt_alloc(sizeof(V) * (size_t)it->nmore);
    for (int i = 0; i < it->nmore; i++) it->more[i] = iter_of(a[i + 1]);
    return vobj(it);
}

static V b_filter(ARGS) {
    UNUSED_KW; need(n, 2, 2, "filter");
    Iter *it = mkiter(IT_FILTER, iter_of(a[1]));
    it->fn = a[0];
    return vobj(it);
}

static V b_any(ARGS) { UNUSED_KW; need(n, 1, 1, "any"); V it = iter_of(a[0]), x; while (iter_next(it, &x)) if (truthy(x)) return vbool(1); return vbool(0); }
static V b_all(ARGS) { UNUSED_KW; need(n, 1, 1, "all"); V it = iter_of(a[0]), x; while (iter_next(it, &x)) if (!truthy(x)) return vbool(0); return vbool(1); }

static V b_round(ARGS) {
    V nd = n > 1 ? a[1] : vnone();
    kwarg(kwn, kwv, nk, "ndigits", &nd);
    need(n, 1, 2, "round");
    V x = a[0];
    if (is_int(x)) {
        if (nd.t == V_NONE) return x.t == V_BOOL ? vint(x.u.i) : x;
        int64_t d = to_i64(nd, "ndigits");
        if (d >= 0) return x;
        V p = int_pow(vint(10), vint(-d)), q, r;
        q = int_floordiv(x, p); r = int_mod(x, p);
        int c = int_cmp(int_mul(r, vint(2)), p);
        if (c > 0 || (c == 0 && int_cmp(int_mod(q, vint(2)), vint(0)) != 0)) q = int_add(q, vint(1));
        return int_mul(q, p);
    }
    double f = to_f(x);
    if (nd.t == V_NONE) {
        if (f != f || f == __builtin_inf() || f == -__builtin_inf()) raise_new(P->e_value, "cannot convert float to integer");
        double r = rint(f);
        return int_from_double(r);
    }
    int64_t d = to_i64(nd, "ndigits");
    if (f != f || f == __builtin_inf() || f == -__builtin_inf()) return vfloat(f);
    char b[600];
    if (d > 300) return vfloat(f);
    if (d < -300) return vfloat(0.0 * f);
    if (d >= 0) { num_fixed(f, (int)d, b, sizeof b); return vfloat(num_parse(b, 0)); }
    double p = pow(10, (double)-d);
    return vfloat(rint(f / p) * p);
}

static V b_divmod(ARGS) {
    UNUSED_KW; need(n, 2, 2, "divmod");
    Tuple *t = tuple_new(2);
    t->v[0] = binop(B_FLOORDIV, a[0], a[1]);
    t->v[1] = binop(B_MOD, a[0], a[1]);
    return vobj(t);
}

static V b_pow(ARGS) {
    UNUSED_KW; need(n, 2, 3, "pow");
    if (n == 3) {
        if (!is_int(a[0]) || !is_int(a[1]) || !is_int(a[2])) raise_new(P->e_type, "pow() 3rd argument not allowed unless all arguments are integers");
        V base = int_mod(a[0], a[2]), e = a[1], m = a[2], r = vint(1);
        if (int_cmp(e, vint(0)) < 0) raise_new(P->e_value, "base is not invertible for the given modulus");
        while (int_cmp(e, vint(0)) > 0) {
            rt_check();
            if (int_cmp(int_mod(e, vint(2)), vint(1)) == 0) r = int_mod(int_mul(r, base), m);
            e = int_floordiv(e, vint(2));
            base = int_mod(int_mul(base, base), m);
        }
        return r;
    }
    return binop(B_POW, a[0], a[1]);
}

static V b_ord(ARGS) {
    UNUSED_KW; need(n, 1, 1, "ord");
    Str *s = arg_str(a[0], "ord");
    if (s->len != 1) raise_new(P->e_type, "ord() expected a character, but string of length %u found", s->len);
    return vint((uint8_t)s->s[0]);
}
static V b_chr(ARGS) {
    UNUSED_KW; need(n, 1, 1, "chr");
    int64_t c = to_i64(a[0], "chr()");
    if (c < 0 || c > 0x10FFFF) raise_new(P->e_value, "chr() arg not in range(0x110000)");
    char ch = c < 256 ? (char)c : '?';
    return vstrn(&ch, 1);
}
static V radix(V x, int base, const char *pfx) {
    int neg = int_cmp(x, vint(0)) < 0;
    Str *d = int_to_str(neg ? int_neg(x) : x, base);
    SB sb; sb_new(&sb, d->len + 4);
    if (neg) sb_addc(&sb, '-');
    sb_adds(&sb, pfx); sb_addstr(&sb, d);
    return vobj(sb_str(&sb));
}
static V b_hex(ARGS) { UNUSED_KW; need(n, 1, 1, "hex"); if (!is_int(a[0])) raise_new(P->e_type, "'%s' object cannot be interpreted as an integer", type_name(a[0])); return radix(a[0], 16, "0x"); }
static V b_oct(ARGS) { UNUSED_KW; need(n, 1, 1, "oct"); if (!is_int(a[0])) raise_new(P->e_type, "'%s' object cannot be interpreted as an integer", type_name(a[0])); return radix(a[0], 8, "0o"); }
static V b_bin(ARGS) { UNUSED_KW; need(n, 1, 1, "bin"); if (!is_int(a[0])) raise_new(P->e_type, "'%s' object cannot be interpreted as an integer", type_name(a[0])); return radix(a[0], 2, "0b"); }

static int class_match(V v, V cls, int sub) {
    if (is_obj(cls, O_TUPLE)) {
        Tuple *t = (Tuple *)cls.u.o;
        for (uint32_t i = 0; i < t->len; i++) if (class_match(v, t->v[i], sub)) return 1;
        return 0;
    }
    if (!is_obj(cls, O_CLASS)) raise_new(P->e_type, "%s() arg 2 must be a type or tuple of types", sub ? "issubclass" : "isinstance");
    if (sub) { if (!is_obj(v, O_CLASS)) raise_new(P->e_type, "issubclass() arg 1 must be a class"); return is_subclass((Class *)v.u.o, (Class *)cls.u.o); }
    return is_instance(v, (Class *)cls.u.o);
}
static V b_isinstance(ARGS) { UNUSED_KW; need(n, 2, 2, "isinstance"); return vbool(class_match(a[0], a[1], 0)); }
static V b_issubclass(ARGS) { UNUSED_KW; need(n, 2, 2, "issubclass"); return vbool(class_match(a[0], a[1], 1)); }
static V b_type(ARGS) { UNUSED_KW; need(n, 1, 1, "type"); return vobj(type_of(a[0])); }
static V b_repr(ARGS) { UNUSED_KW; need(n, 1, 1, "repr"); return vobj(to_repr(a[0])); }
static V b_hash(ARGS) { UNUSED_KW; need(n, 1, 1, "hash"); return vint((int64_t)(hash_of(a[0]) >> 1)); }
static V b_id(ARGS) { UNUSED_KW; need(n, 1, 1, "id"); return vint(a[0].t == V_OBJ ? (int64_t)(uintptr_t)a[0].u.o : (int64_t)hash_of(a[0]) >> 1); }
static V b_iter(ARGS) { UNUSED_KW; need(n, 1, 1, "iter"); return iter_of(a[0]); }
static V b_next(ARGS) {
    UNUSED_KW; need(n, 1, 2, "next");
    V it = a[0], x;
    if (is_obj(it, O_INST)) {
        V m;
        if (!lookup_special(it, P->s_next, &m)) raise_new(P->e_type, "'%s' object is not an iterator", type_name(it));
        if (n == 1) return call0(m);
        Handler h; h.prev = py_handler; py_handler = &h;
        if (rt_setjmp(&h.jb) == 0) { V r = call0(m); py_handler = h.prev; return r; }
        py_handler = h.prev;
        if (is_instance(py_exc, P->e_stop)) return a[1];
        raise(py_exc);
    }
    if (!is_obj(it, O_ITER)) raise_new(P->e_type, "'%s' object is not an iterator", type_name(it));
    if (iter_next(it, &x)) return x;
    if (n == 2) return a[1];
    raise(make_exc(P->e_stop, (V){ .t = V_UNBOUND }));
    return vnone();
}
static V b_callable(ARGS) {
    UNUSED_KW; need(n, 1, 1, "callable");
    V v = a[0], m;
    if (v.t != V_OBJ) return vbool(0);
    int t = v.u.o->type;
    return vbool(t == O_FUNC || t == O_NATIVE || t == O_BOUND || t == O_CLASS || (t == O_INST && lookup_special(v, P->s_call, &m)));
}
static V b_getattr(ARGS) {
    UNUSED_KW; need(n, 2, 3, "getattr");
    Str *nm = arg_str(a[1], "getattr");
    V v;
    if (getattr_opt(a[0], intern(nm->s, nm->len), &v)) return v;
    if (n == 3) return a[2];
    return getattr(a[0], intern(nm->s, nm->len));
}
static V b_setattr(ARGS) { UNUSED_KW; need(n, 3, 3, "setattr"); Str *nm = arg_str(a[1], "setattr"); setattr(a[0], intern(nm->s, nm->len), a[2]); return vnone(); }
static V b_hasattr(ARGS) { UNUSED_KW; need(n, 2, 2, "hasattr"); Str *nm = arg_str(a[1], "hasattr"); V v; return vbool(getattr_opt(a[0], intern(nm->s, nm->len), &v)); }
static V b_delattr(ARGS) {
    UNUSED_KW; need(n, 2, 2, "delattr");
    Str *nm = arg_str(a[1], "delattr");
    if (is_obj(a[0], O_INST) && dict_del(((Inst *)a[0].u.o)->dict, vobj(intern(nm->s, nm->len)))) return vnone();
    raise_new(P->e_attr, "'%s' object has no attribute '%s'", type_name(a[0]), nm->s);
    return vnone();
}
static V b_format(ARGS) { UNUSED_KW; need(n, 1, 2, "format"); return vobj(format_value(a[0], n > 1 ? arg_str(a[1], "format") : str_c(""))); }
V py_super0(void);
static V b_super(ARGS) {
    UNUSED_KW;
    if (!n) return py_super0();
    need(n, 2, 2, "super");
    if (!is_obj(a[0], O_CLASS)) raise_new(P->e_type, "super() argument 1 must be a type");
    Super *s = rt_alloc(sizeof(Super));
    s->type = O_SUPER; s->cls = (Class *)a[0].u.o; s->self = a[1];
    return vobj(s);
}
static V wrap(int type, V fn) { Wrap *w = rt_alloc(sizeof(Wrap)); w->type = (uint8_t)type; w->fn = fn; return vobj(w); }
static V b_staticmethod(ARGS) { UNUSED_KW; need(n, 1, 1, "staticmethod"); return wrap(O_STATICM, a[0]); }
static V b_classmethod(ARGS) { UNUSED_KW; need(n, 1, 1, "classmethod"); return wrap(O_CLASSM, a[0]); }

static V prop_setter(ARGS) {
    UNUSED_KW; need(n, 2, 2, "setter");
    Prop *p = (Prop *)a[0].u.o, *q = rt_alloc(sizeof(Prop));
    *q = *p; q->set = a[1];
    return vobj(q);
}
static V b_property(ARGS) {
    V g = n > 0 ? a[0] : vnone(), s = n > 1 ? a[1] : vnone();
    kwarg(kwn, kwv, nk, "fget", &g); kwarg(kwn, kwv, nk, "fset", &s);
    Prop *p = rt_alloc(sizeof(Prop));
    p->type = O_PROP; p->get = g; p->set = s;
    return vobj(p);
}
V py_globals(void);
V py_locals(void);
static V b_globals(ARGS) { UNUSED_KW; (void)a; (void)n; return py_globals(); }
static V b_locals(ARGS) { UNUSED_KW; (void)a; (void)n; return py_locals(); }
static V b_abstractmethod(ARGS) { UNUSED_KW; return n ? a[0] : vnone(); }
static V b_reclimit(ARGS) { UNUSED_KW; (void)a; (void)n; return vint(1000); }
static V b_exit(ARGS) { UNUSED_KW; raise(make_exc(P->e_exit, n ? a[0] : (V){ .t = V_UNBOUND })); }

static V b_vars(ARGS) {
    UNUSED_KW; need(n, 1, 1, "vars");
    if (is_obj(a[0], O_INST)) return vobj(((Inst *)a[0].u.o)->dict);
    if (is_obj(a[0], O_CLASS)) return vobj(((Class *)a[0].u.o)->dict);
    if (is_obj(a[0], O_MODULE)) return vobj(((Module *)a[0].u.o)->dict);
    raise_new(P->e_type, "vars() argument must have __dict__ attribute");
    return vnone();
}
static V b_dir(ARGS) {
    UNUSED_KW; need(n, 1, 1, "dir");
    List *l = list_new(16);
    Dict *seen = set_new();
    V v = a[0];
    Dict *ds[64]; int nd = 0;
    if (is_obj(v, O_INST)) ds[nd++] = ((Inst *)v.u.o)->dict;
    if (is_obj(v, O_MODULE)) ds[nd++] = ((Module *)v.u.o)->dict;
    Class *c = is_obj(v, O_CLASS) ? (Class *)v.u.o : is_obj(v, O_MODULE) ? 0 : type_of(v);
    for (int i = 0; c && i < c->nmro && nd < 64; i++) ds[nd++] = c->mro[i]->dict;
    for (int i = 0; i < nd; i++) { uint32_t pos = 0; V k; while (dict_next(ds[i], &pos, &k, 0)) if (!dict_get(seen, k, 0) && is_obj(k, O_STR) && strcmp(as_str(k)->s, "__ctor__")) { dict_set(seen, k, vnone()); list_push(l, k); } }
    list_sort(l, vnone(), 0);
    return vobj(l);
}

/* ---- files ------------------------------------------------------------------------------------------- */
static void path_for(Str *p, char *out, int cap) {
    const char *dir = rt_cwd();
    if (p->s[0] == '/') snprintf(out, (size_t)cap, "%s", p->s);
    else snprintf(out, (size_t)cap, "%s/%s", strcmp(dir, "/") ? dir : "", p->s);
}

static V b_open(ARGS) {
    V mode = n > 1 ? a[1] : vstr("r");
    kwarg(kwn, kwv, nk, "mode", &mode);
    need(n, 1, 3, "open");
    Str *p = arg_str(a[0], "open"), *m = arg_str(mode, "open");
    char path[300];
    path_for(p, path, sizeof path);
    File *f = rt_alloc(sizeof(File));
    f->type = O_FILE; f->path = str_c(path);
    char mc = m->s[0];
    if (mc == 'r') {
        char *data; uint32_t len; const char *why;
        if (rt_file_read(path, &data, &len, &why)) {
            V e = make_exc(strstr(why, "no file") || strstr(why, "There's no") ? P->e_notfound : P->e_os, vobj(str_c(why)));
            char b[400]; snprintf(b, sizeof b, "[Errno 2] No such file or directory: '%s'", p->s);
            ((Tuple *)((Inst *)e.u.o)->native.u.o)->v[0] = vstr(b);
            raise(e);
        }
        f->mode = 'r'; f->buf = data; f->len = len;
        if (strchr(m->s, '+')) f->mode = '+';
    } else if (mc == 'w' || mc == 'a' || mc == 'x') {
        f->mode = (uint8_t)mc; f->cap = 1024; f->buf = rt_alloc_leaf(f->cap);
        if (mc == 'x') {
            char *data; uint32_t len; const char *why;
            if (!rt_file_read(path, &data, &len, &why)) raise_new(P->e_os, "[Errno 17] File exists: '%s'", p->s);
            f->mode = 'w';
        }
        list_push(P->open_files, vobj(f));
    } else raise_new(P->e_value, "invalid mode: '%s'", m->s);
    return vobj(f);
}

static V file_write_str(V file, Str *s) {
    File *f = (File *)file.u.o;
    if (f->closed) raise_new(P->e_value, "I/O operation on closed file.");
    if (f->mode == 'r') raise_new(P->e_os, "not writable");
    if (f->len + s->len + 1 > f->cap) {
        uint32_t nc = f->cap * 2;
        while (nc < f->len + s->len + 1) nc *= 2;
        f->buf = rt_realloc(f->buf, f->len, nc); f->cap = nc;
    }
    memcpy(f->buf + f->len, s->s, s->len);
    f->len += s->len;
    return vint(s->len);
}

static void file_flush(File *f) {
    if (f->closed || f->mode == 'r') return;
    const char *why;
    if (rt_file_write(f->path->s, f->buf, f->len, f->mode == 'a', &why)) raise_new(P->e_os, "can't write %s: %s", f->path->s, why);
    if (f->mode == 'a') f->len = 0;
}

void py_close_files(void);
void py_close_files(void) {
    for (uint32_t i = 0; P->open_files && i < P->open_files->len; i++) {
        File *f = (File *)P->open_files->v[i].u.o;
        if (!f->closed) { file_flush(f); f->closed = 1; }
    }
}

static File *as_file(V v) { File *f = (File *)v.u.o; if (f->closed) raise_new(P->e_value, "I/O operation on closed file."); return f; }

static V f_read(ARGS) {
    UNUSED_KW;
    File *f = as_file(a[0]);
    if (f->mode != 'r' && f->mode != '+') raise_new(P->e_os, "not readable");
    int64_t k = n > 1 && a[1].t != V_NONE ? to_i64(a[1], "size") : -1;
    uint32_t left = f->len - f->pos, take = k < 0 || (uint64_t)k > left ? left : (uint32_t)k;
    V r = vstrn(f->buf + f->pos, take);
    f->pos += take;
    return r;
}
static V f_readline(ARGS) {
    UNUSED_KW; (void)n;
    File *f = as_file(a[0]);
    uint32_t s = f->pos;
    while (f->pos < f->len && f->buf[f->pos] != '\n') f->pos++;
    if (f->pos < f->len) f->pos++;
    return vstrn(f->buf + s, f->pos - s);
}
static V f_readlines(ARGS) {
    UNUSED_KW; (void)n;
    List *l = list_new(16);
    File *f = as_file(a[0]);
    while (f->pos < f->len) list_push(l, f_readline(a, 1, 0, 0, 0));
    return vobj(l);
}
static V f_write(ARGS) { UNUSED_KW; need(n, 2, 2, "write"); return file_write_str(a[0], arg_str(a[1], "write")); }
static V f_writelines(ARGS) { UNUSED_KW; need(n, 2, 2, "writelines"); V it = iter_of(a[1]), x; while (iter_next(it, &x)) file_write_str(a[0], arg_str(x, "writelines")); return vnone(); }
static V f_close(ARGS) { UNUSED_KW; (void)n; File *f = (File *)a[0].u.o; if (!f->closed) { file_flush(f); f->closed = 1; } return vnone(); }
static V f_flush(ARGS) { UNUSED_KW; (void)a; (void)n; return vnone(); }
static V f_enter(ARGS) { UNUSED_KW; (void)n; return a[0]; }
static V f_exit(ARGS) { (void)n; return f_close(a, 1, kwn, kwv, nk); }

/* ---- the types' constructors ----------------------------------------------------------------------------- */
static V c_int(ARGS) {
    V base = n > 1 ? a[1] : vnone();
    kwarg(kwn, kwv, nk, "base", &base);
    if (!n) return vint(0);
    V x = a[0];
    if (is_obj(x, O_STR)) {
        int ok;
        int b = base.t == V_NONE ? 10 : (int)to_i64(base, "base");
        V r = int_parse(as_str(x)->s, b, &ok);
        if (!ok) raise_new(P->e_value, "invalid literal for int() with base %d: %s", b, to_repr(x)->s);
        return r;
    }
    if (base.t != V_NONE) raise_new(P->e_type, "int() can't convert non-string with explicit base");
    if (x.t == V_FLOAT) return int_from_double(x.u.f);
    if (x.t == V_BOOL) return vint(x.u.i);
    if (is_int(x)) return x;
    V m;
    if (lookup_special(x, P->s_int, &m)) return call0(m);
    raise_new(P->e_type, "int() argument must be a string, a bytes-like object or a real number, not '%s'", type_name(x));
    return vnone();
}
static V c_float(ARGS) {
    UNUSED_KW;
    if (!n) return vfloat(0);
    V x = a[0];
    if (is_obj(x, O_STR)) {
        const char *e;
        Str *s = as_str(x);
        double d = num_parse(s->s, &e);
        while (*e == ' ' || *e == '\n' || *e == '\t') e++;
        if (e == s->s || *e) raise_new(P->e_value, "could not convert string to float: %s", to_repr(x)->s);
        return vfloat(d);
    }
    return vfloat(to_f(x));
}
static V c_str(ARGS) { UNUSED_KW; return n ? vobj(to_str(a[0])) : vstr(""); }
static V c_bool(ARGS) { UNUSED_KW; return vbool(n && truthy(a[0])); }
static V c_list(ARGS) { UNUSED_KW; return n ? seq_from(a[0]) : vobj(list_new(4)); }
static V c_tuple(ARGS) {
    UNUSED_KW;
    if (!n) return vobj(tuple_new(0));
    if (is_obj(a[0], O_TUPLE)) return a[0];
    List *l = as_list(seq_from(a[0]));
    Tuple *t = tuple_new(l->len);
    memcpy(t->v, l->v, sizeof(V) * l->len);
    return vobj(t);
}
static void dict_update(Dict *d, V src) {
    src = SELF(src);
    if (is_obj(src, O_DICT)) { uint32_t pos = 0; V k, v; while (dict_next(as_dict(src), &pos, &k, &v)) dict_set(d, k, v); return; }
    V m;
    if (is_obj(src, O_INST) && getattr_opt(src, intern_c("keys"), &m)) {
        V ks = call0(m), it = iter_of(ks), k;
        while (iter_next(it, &k)) dict_set(d, k, get_item(src, k));
        return;
    }
    V it = iter_of(src), x;
    while (iter_next(it, &x)) {
        V pr = seq_from(x);
        if (as_list(pr)->len != 2) raise_new(P->e_value, "dictionary update sequence element has length %u; 2 is required", as_list(pr)->len);
        dict_set(d, as_list(pr)->v[0], as_list(pr)->v[1]);
    }
}
static V c_dict(ARGS) {
    Dict *d = dict_new();
    if (n) dict_update(d, a[0]);
    for (int i = 0; i < nk; i++) dict_set(d, kwn[i], kwv[i]);
    return vobj(d);
}
static V c_set(ARGS) {
    UNUSED_KW;
    Dict *s = set_new();
    if (n) { V it = iter_of(a[0]), x; while (iter_next(it, &x)) dict_set(s, x, vnone()); }
    return vobj(s);
}
static V c_frozenset(ARGS) { V s = c_set(a, n, kwn, kwv, nk); as_dict(s)->frozen = 1; return s; }
static V c_object(ARGS) {
    UNUSED_KW; (void)a; (void)n;
    Inst *in = rt_alloc(sizeof(Inst));
    in->type = O_INST; in->cls = P->t_object; in->dict = dict_new(); in->native = vnone();
    return vobj(in);
}
static V c_slice(ARGS) {
    UNUSED_KW; need(n, 1, 3, "slice");
    Slice *s = rt_alloc(sizeof(Slice));
    s->type = O_SLICE;
    s->lo = n > 1 ? a[0] : vnone(); s->hi = n > 1 ? a[1] : a[0]; s->step = n > 2 ? a[2] : vnone();
    return vobj(s);
}

/* ---- str methods --------------------------------------------------------------------------------------------- */
#define S0 Str *s = as_str(SELF(a[0]))
static int is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

static V map_chars(V *a, int n, int how) {
    (void)n; S0;
    Str *r = str_new(s->s, s->len);
    int start = 1;
    for (uint32_t i = 0; i < r->len; i++) {
        char c = r->s[i];
        int lo = c >= 'a' && c <= 'z', up = c >= 'A' && c <= 'Z';
        switch (how) {
        case 0: if (lo) r->s[i] = (char)(c - 32); break;
        case 1: if (up) r->s[i] = (char)(c + 32); break;
        case 2: if (up) r->s[i] = (char)(c + 32); else if (lo) r->s[i] = (char)(c - 32); break;
        case 3: if (start && lo) r->s[i] = (char)(c - 32); else if (!start && up) r->s[i] = (char)(c + 32); start = !(lo || up); break;
        case 4: if (i == 0 && lo) r->s[i] = (char)(c - 32); else if (i && up) r->s[i] = (char)(c + 32); break;
        }
    }
    return vobj(r);
}
static V s_upper(ARGS) { UNUSED_KW; return map_chars(a, n, 0); }
static V s_lower(ARGS) { UNUSED_KW; return map_chars(a, n, 1); }
static V s_swapcase(ARGS) { UNUSED_KW; return map_chars(a, n, 2); }
static V s_title(ARGS) { UNUSED_KW; return map_chars(a, n, 3); }
static V s_capitalize(ARGS) { UNUSED_KW; return map_chars(a, n, 4); }

static V strip_how(V *a, int n, int left, int right) {
    S0;
    Str *chars = n > 1 && a[1].t != V_NONE ? arg_str(a[1], "strip") : 0;
    uint32_t i = 0, j = s->len;
    #define STRIPPABLE(c) (chars ? memchr(chars->s, (c), chars->len) != 0 : is_space((unsigned char)(c)))
    if (left) while (i < j && STRIPPABLE(s->s[i])) i++;
    if (right) while (j > i && STRIPPABLE(s->s[j - 1])) j--;
    #undef STRIPPABLE
    return vstrn(s->s + i, j - i);
}
static V s_strip(ARGS) { UNUSED_KW; return strip_how(a, n, 1, 1); }
static V s_lstrip(ARGS) { UNUSED_KW; return strip_how(a, n, 1, 0); }
static V s_rstrip(ARGS) { UNUSED_KW; return strip_how(a, n, 0, 1); }

static V do_split(V *a, int n, V *kwn, V *kwv, int nk, int from_right) {
    S0;
    V sepv = n > 1 ? a[1] : vnone(), maxv = n > 2 ? a[2] : vint(-1);
    kwarg(kwn, kwv, nk, "sep", &sepv); kwarg(kwn, kwv, nk, "maxsplit", &maxv);
    int64_t maxs = to_i64(maxv, "maxsplit");
    List *l = list_new(8);
    if (sepv.t == V_NONE) {
        /* runs of whitespace; nothing empty */
        if (!from_right) {
            uint32_t i = 0;
            while (i < s->len) {
                while (i < s->len && is_space((unsigned char)s->s[i])) i++;
                if (i >= s->len) break;
                if (maxs >= 0 && (int64_t)l->len >= maxs) {
                    uint32_t e = s->len; while (e > i && is_space((unsigned char)s->s[e - 1])) e--;
                    list_push(l, vstrn(s->s + i, e - i)); break;
                }
                uint32_t st = i;
                while (i < s->len && !is_space((unsigned char)s->s[i])) i++;
                list_push(l, vstrn(s->s + st, i - st));
            }
            return vobj(l);
        }
        int64_t i = (int64_t)s->len - 1;
        while (i >= 0) {
            while (i >= 0 && is_space((unsigned char)s->s[i])) i--;
            if (i < 0) break;
            if (maxs >= 0 && (int64_t)l->len >= maxs) { int64_t b = 0; while (b <= i && is_space((unsigned char)s->s[b])) b++; list_push(l, vstrn(s->s + b, (uint32_t)(i - b + 1))); break; }
            int64_t e = i;
            while (i >= 0 && !is_space((unsigned char)s->s[i])) i--;
            list_push(l, vstrn(s->s + i + 1, (uint32_t)(e - i)));
        }
        for (uint32_t k = 0; k < l->len / 2; k++) { V t = l->v[k]; l->v[k] = l->v[l->len - 1 - k]; l->v[l->len - 1 - k] = t; }
        return vobj(l);
    }
    Str *sep = arg_str(sepv, "split");
    if (!sep->len) raise_new(P->e_value, "empty separator");
    if (!from_right) {
        uint32_t st = 0, i = 0;
        while (i + sep->len <= s->len) {
            if ((maxs < 0 || (int64_t)l->len < maxs) && !memcmp(s->s + i, sep->s, sep->len)) { list_push(l, vstrn(s->s + st, i - st)); i += sep->len; st = i; }
            else i++;
        }
        list_push(l, vstrn(s->s + st, s->len - st));
        return vobj(l);
    }
    int64_t end = s->len, i = (int64_t)s->len - sep->len;
    while (i >= 0) {
        if ((maxs < 0 || (int64_t)l->len < maxs) && !memcmp(s->s + i, sep->s, sep->len)) { list_push(l, vstrn(s->s + i + sep->len, (uint32_t)(end - i - sep->len))); end = i; i -= sep->len; }
        else i--;
    }
    list_push(l, vstrn(s->s, (uint32_t)end));
    for (uint32_t k = 0; k < l->len / 2; k++) { V t = l->v[k]; l->v[k] = l->v[l->len - 1 - k]; l->v[l->len - 1 - k] = t; }
    return vobj(l);
}
static V s_split(ARGS) { return do_split(a, n, kwn, kwv, nk, 0); }
static V s_rsplit(ARGS) { return do_split(a, n, kwn, kwv, nk, 1); }
static V s_splitlines(ARGS) {
    UNUSED_KW; S0;
    int keep = n > 1 && truthy(a[1]);
    List *l = list_new(8);
    uint32_t st = 0;
    for (uint32_t i = 0; i < s->len; i++) {
        if (s->s[i] == '\n' || s->s[i] == '\r') {
            uint32_t e = i;
            if (s->s[i] == '\r' && i + 1 < s->len && s->s[i + 1] == '\n') i++;
            list_push(l, vstrn(s->s + st, (keep ? i + 1 : e) - st));
            st = i + 1;
        }
    }
    if (st < s->len) list_push(l, vstrn(s->s + st, s->len - st));
    return vobj(l);
}
static V s_join(ARGS) {
    UNUSED_KW; need(n, 2, 2, "join"); S0;
    SB sb; sb_new(&sb, 64);
    V it = iter_of(a[1]), x; int i = 0;
    while (iter_next(it, &x)) {
        if (!is_obj(x, O_STR)) raise_new(P->e_type, "sequence item %d: expected str instance, %s found", i, type_name(x));
        if (i++) sb_addstr(&sb, s);
        sb_addstr(&sb, as_str(x));
    }
    return vobj(sb_str(&sb));
}
static V s_replace(ARGS) {
    UNUSED_KW; need(n, 3, 4, "replace"); S0;
    Str *o = arg_str(a[1], "replace"), *w = arg_str(a[2], "replace");
    int64_t cnt = n > 3 ? to_i64(a[3], "count") : -1;
    SB sb; sb_new(&sb, s->len + 16);
    uint32_t i = 0; int64_t done = 0;
    if (!o->len) {
        for (; i <= s->len; i++) {
            if (cnt < 0 || done < cnt) { sb_addstr(&sb, w); done++; }
            if (i < s->len) sb_addc(&sb, s->s[i]);
        }
        return vobj(sb_str(&sb));
    }
    while (i < s->len) {
        if ((cnt < 0 || done < cnt) && i + o->len <= s->len && !memcmp(s->s + i, o->s, o->len)) { sb_addstr(&sb, w); i += o->len; done++; }
        else sb_addc(&sb, s->s[i++]);
    }
    return vobj(sb_str(&sb));
}
static int64_t find_in(Str *s, Str *sub, int64_t lo, int64_t hi, int rev) {
    if (lo < 0) lo += s->len; if (lo < 0) lo = 0;
    if (hi < 0) hi += s->len; if (hi > (int64_t)s->len) hi = s->len;
    if (!rev) { for (int64_t i = lo; i + (int64_t)sub->len <= hi; i++) if (!memcmp(s->s + i, sub->s, sub->len)) return i; }
    else { for (int64_t i = hi - sub->len; i >= lo; i--) if (!memcmp(s->s + i, sub->s, sub->len)) return i; }
    return -1;
}
static V find_how(V *a, int n, int rev, int must) {
    need(n, 2, 4, rev ? "rfind" : "find"); S0;
    Str *sub = arg_str(a[1], "find");
    int64_t lo = n > 2 && a[2].t != V_NONE ? to_i64(a[2], "start") : 0, hi = n > 3 && a[3].t != V_NONE ? to_i64(a[3], "end") : (int64_t)s->len;
    int64_t r = find_in(s, sub, lo, hi, rev);
    if (r < 0 && must) raise_new(P->e_value, "substring not found");
    return vint(r);
}
static V s_find(ARGS) { UNUSED_KW; return find_how(a, n, 0, 0); }
static V s_rfind(ARGS) { UNUSED_KW; return find_how(a, n, 1, 0); }
static V s_index(ARGS) { UNUSED_KW; return find_how(a, n, 0, 1); }
static V s_rindex(ARGS) { UNUSED_KW; return find_how(a, n, 1, 1); }
static V s_count(ARGS) {
    UNUSED_KW; need(n, 2, 4, "count"); S0;
    Str *sub = arg_str(a[1], "count");
    int64_t c = 0;
    if (!sub->len) return vint(s->len + 1);
    for (uint32_t i = 0; i + sub->len <= s->len; ) { if (!memcmp(s->s + i, sub->s, sub->len)) { c++; i += sub->len; } else i++; }
    return vint(c);
}
static int affix(Str *s, V pre, int end) {
    if (is_obj(pre, O_TUPLE)) { Tuple *t = (Tuple *)pre.u.o; for (uint32_t i = 0; i < t->len; i++) if (affix(s, t->v[i], end)) return 1; return 0; }
    Str *p = arg_str(pre, end ? "endswith" : "startswith");
    if (p->len > s->len) return 0;
    return !memcmp(s->s + (end ? s->len - p->len : 0), p->s, p->len);
}
static V s_startswith(ARGS) { UNUSED_KW; need(n, 2, 2, "startswith"); S0; return vbool(affix(s, a[1], 0)); }
static V s_endswith(ARGS) { UNUSED_KW; need(n, 2, 2, "endswith"); S0; return vbool(affix(s, a[1], 1)); }
static V s_removeprefix(ARGS) { UNUSED_KW; need(n, 2, 2, "removeprefix"); S0; Str *p = arg_str(a[1], "removeprefix"); return affix(s, a[1], 0) ? vstrn(s->s + p->len, s->len - p->len) : vobj(s); }
static V s_removesuffix(ARGS) { UNUSED_KW; need(n, 2, 2, "removesuffix"); S0; Str *p = arg_str(a[1], "removesuffix"); return affix(s, a[1], 1) && p->len ? vstrn(s->s, s->len - p->len) : vobj(s); }
static V char_class(V *a, int how) {
    S0;
    if (!s->len) return vbool(0);
    int any_cased = 0;
    for (uint32_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->s[i];
        int dg = c >= '0' && c <= '9', al = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0xC0, lo = c >= 'a' && c <= 'z', up = c >= 'A' && c <= 'Z';
        switch (how) {
        case 0: if (!dg) return vbool(0); break;
        case 1: if (!al) return vbool(0); break;
        case 2: if (!dg && !al) return vbool(0); break;
        case 3: if (!is_space(c)) return vbool(0); break;
        case 4: if (lo) return vbool(0); if (up) any_cased = 1; break;
        case 5: if (up) return vbool(0); if (lo) any_cased = 1; break;
        case 6: if (!(al || c == '_' || (i && dg))) return vbool(0); break;
        }
    }
    return vbool(how == 4 || how == 5 ? any_cased : 1);
}
static V s_isdigit(ARGS) { UNUSED_KW; (void)n; return char_class(a, 0); }
static V s_isalpha(ARGS) { UNUSED_KW; (void)n; return char_class(a, 1); }
static V s_isalnum(ARGS) { UNUSED_KW; (void)n; return char_class(a, 2); }
static V s_isspace(ARGS) { UNUSED_KW; (void)n; return char_class(a, 3); }
static V s_isupper(ARGS) { UNUSED_KW; (void)n; return char_class(a, 4); }
static V s_islower(ARGS) { UNUSED_KW; (void)n; return char_class(a, 5); }
static V s_isidentifier(ARGS) { UNUSED_KW; (void)n; return char_class(a, 6); }
static V justify(V *a, int n, int how) {
    need(n, 2, 3, "ljust"); S0;
    int64_t w = to_i64(a[1], "width");
    char fill = n > 2 ? arg_str(a[2], "fill")->s[0] : ' ';
    if (w <= (int64_t)s->len) return vobj(s);
    uint32_t padn = (uint32_t)(w - s->len), left = how == 0 ? 0 : how == 1 ? padn : padn / 2 + (padn & s->len & 1);
    Str *r = str_new(0, (uint32_t)w);
    memset(r->s, fill, (size_t)w);
    memcpy(r->s + left, s->s, s->len);
    return vobj(r);
}
static V s_ljust(ARGS) { UNUSED_KW; return justify(a, n, 0); }
static V s_rjust(ARGS) { UNUSED_KW; return justify(a, n, 1); }
static V s_center(ARGS) { UNUSED_KW; return justify(a, n, 2); }
static V s_zfill(ARGS) {
    UNUSED_KW; need(n, 2, 2, "zfill"); S0;
    int64_t w = to_i64(a[1], "width");
    if (w <= (int64_t)s->len) return vobj(s);
    Str *r = str_new(0, (uint32_t)w);
    uint32_t padn = (uint32_t)(w - s->len), sign = s->len && (s->s[0] == '-' || s->s[0] == '+');
    if (sign) r->s[0] = s->s[0];
    memset(r->s + sign, '0', padn);
    memcpy(r->s + sign + padn, s->s + sign, s->len - sign);
    return vobj(r);
}
static V part(V *a, int n, int rev) {
    need(n, 2, 2, "partition"); S0;
    Str *sep = arg_str(a[1], "partition");
    int64_t i = find_in(s, sep, 0, s->len, rev);
    Tuple *t = tuple_new(3);
    if (i < 0) { t->v[rev ? 2 : 0] = vobj(s); t->v[1] = vstr(""); t->v[rev ? 0 : 2] = vstr(""); return vobj(t); }
    t->v[0] = vstrn(s->s, (uint32_t)i); t->v[1] = vobj(sep); t->v[2] = vstrn(s->s + i + sep->len, (uint32_t)(s->len - i - sep->len));
    return vobj(t);
}
static V s_partition(ARGS) { UNUSED_KW; return part(a, n, 0); }
static V s_rpartition(ARGS) { UNUSED_KW; return part(a, n, 1); }
static V s_encode(ARGS) { UNUSED_KW; (void)n; return SELF(a[0]); }
static V s_expandtabs(ARGS) {
    UNUSED_KW; S0;
    int64_t ts = n > 1 ? to_i64(a[1], "tabsize") : 8;
    SB sb; sb_new(&sb, s->len + 8);
    uint32_t col = 0;
    for (uint32_t i = 0; i < s->len; i++) {
        if (s->s[i] == '\t') { do { sb_addc(&sb, ' '); col++; } while (ts > 0 && col % ts); }
        else { sb_addc(&sb, s->s[i]); col = s->s[i] == '\n' ? 0 : col + 1; }
    }
    return vobj(sb_str(&sb));
}

/* "{} {name} {0:>5}".format(...) */
static V s_format(ARGS) {
    S0;
    SB sb; sb_new(&sb, s->len + 32);
    int auto_i = 1;
    for (uint32_t i = 0; i < s->len; i++) {
        char c = s->s[i];
        if (c == '{' && i + 1 < s->len && s->s[i + 1] == '{') { sb_addc(&sb, '{'); i++; continue; }
        if (c == '}' && i + 1 < s->len && s->s[i + 1] == '}') { sb_addc(&sb, '}'); i++; continue; }
        if (c != '{') { sb_addc(&sb, c); continue; }
        uint32_t j = i + 1, depth = 0;
        while (j < s->len && (s->s[j] != '}' || depth)) { if (s->s[j] == '{') depth++; else if (s->s[j] == '}') depth--; j++; }
        if (j >= s->len) raise_new(P->e_value, "Single '{' encountered in format string");
        /* field[.attr|[key]][!conv][:spec] */
        uint32_t k = i + 1;
        while (k < j && s->s[k] != '!' && s->s[k] != ':' && s->s[k] != '.' && s->s[k] != '[') k++;
        V v;
        if (k == i + 1) {
            if (auto_i >= n) raise_new(P->e_index, "Replacement index %d out of range for positional args tuple", auto_i - 1);
            v = a[auto_i++];
        } else if (s->s[i + 1] >= '0' && s->s[i + 1] <= '9') {
            int idx = 0; for (uint32_t q = i + 1; q < k; q++) idx = idx * 10 + (s->s[q] - '0');
            if (idx + 1 >= n) raise_new(P->e_index, "Replacement index %d out of range for positional args tuple", idx);
            v = a[idx + 1];
        } else {
            int hit = 0;
            for (int q = 0; q < nk; q++) if (as_str(kwn[q])->len == k - i - 1 && !memcmp(as_str(kwn[q])->s, s->s + i + 1, k - i - 1)) { v = kwv[q]; hit = 1; }
            if (!hit) raise_new(P->e_key, "'%.*s'", (int)(k - i - 1), s->s + i + 1);
        }
        while (k < j && (s->s[k] == '.' || s->s[k] == '[')) {
            if (s->s[k] == '.') {
                uint32_t q = k + 1; while (q < j && s->s[q] != '.' && s->s[q] != '[' && s->s[q] != '!' && s->s[q] != ':') q++;
                v = getattr(v, intern(s->s + k + 1, q - k - 1)); k = q;
            } else {
                uint32_t q = k + 1; while (q < j && s->s[q] != ']') q++;
                int num = 1; for (uint32_t z = k + 1; z < q; z++) if (s->s[z] < '0' || s->s[z] > '9') num = 0;
                V key = num ? vint(strtol(s->s + k + 1, 0, 10)) : vstrn(s->s + k + 1, q - k - 1);
                v = get_item(v, key); k = q + 1;
            }
        }
        if (k < j && s->s[k] == '!') { char cv = s->s[k + 1]; v = cv == 'r' || cv == 'a' ? vobj(to_repr(v)) : vobj(to_str(v)); k += 2; }
        Str *spec = 0;
        if (k < j && s->s[k] == ':') {
            Str *raw = str_new(s->s + k + 1, j - k - 1);
            if (memchr(raw->s, '{', raw->len)) { V sa[n + 1]; sa[0] = vobj(raw); for (int q = 1; q < n; q++) sa[q] = a[q]; raw = as_str(s_format(sa, n, kwn, kwv, nk)); }
            spec = raw;
        }
        sb_addstr(&sb, spec ? format_value(v, spec) : to_str(v));
        i = j;
    }
    return vobj(sb_str(&sb));
}

/* ---- list methods ------------------------------------------------------------------------------------------- */
#define L0 List *l = as_list(SELF(a[0]))
static V l_append(ARGS) { UNUSED_KW; need(n, 2, 2, "append"); L0; list_push(l, a[1]); return vnone(); }
static V l_extend(ARGS) { UNUSED_KW; need(n, 2, 2, "extend"); L0; V src = seq_from(a[1]); List *s = as_list(src); for (uint32_t i = 0; i < s->len; i++) list_push(l, s->v[i]); return vnone(); }
static V l_insert(ARGS) {
    UNUSED_KW; need(n, 3, 3, "insert"); L0;
    int64_t i = to_i64(a[1], "index");
    if (i < 0) { i += l->len; if (i < 0) i = 0; }
    if (i > (int64_t)l->len) i = l->len;
    list_push(l, vnone());
    memmove(l->v + i + 1, l->v + i, sizeof(V) * (l->len - 1 - (uint32_t)i));
    l->v[i] = a[2];
    return vnone();
}
static V l_pop(ARGS) {
    UNUSED_KW; need(n, 1, 2, "pop"); L0;
    if (!l->len) raise_new(P->e_index, "pop from empty list");
    int64_t i = n > 1 ? to_i64(a[1], "index") : -1;
    if (i < 0) i += l->len;
    if (i < 0 || i >= (int64_t)l->len) raise_new(P->e_index, "pop index out of range");
    V v = l->v[i];
    memmove(l->v + i, l->v + i + 1, sizeof(V) * (l->len - 1 - (uint32_t)i));
    l->len--;
    return v;
}
static V l_remove(ARGS) {
    UNUSED_KW; need(n, 2, 2, "remove"); L0;
    for (uint32_t i = 0; i < l->len; i++) if (eq(l->v[i], a[1])) { memmove(l->v + i, l->v + i + 1, sizeof(V) * (l->len - 1 - i)); l->len--; return vnone(); }
    raise_new(P->e_value, "list.remove(x): x not in list");
    return vnone();
}
static V seq_index(V *v, uint32_t len, V *a, int n, const char *what) {
    int64_t lo = n > 2 ? to_i64(a[2], "start") : 0, hi = n > 3 ? to_i64(a[3], "end") : len;
    if (lo < 0) lo += len; if (lo < 0) lo = 0;
    if (hi < 0) hi += len; if (hi > (int64_t)len) hi = len;
    for (int64_t i = lo; i < hi; i++) if (eq(v[i], a[1])) return vint(i);
    raise_new(P->e_value, "%s", what);
    return vnone();
}
static V l_index(ARGS) { UNUSED_KW; need(n, 2, 4, "index"); L0; return seq_index(l->v, l->len, a, n, "value is not in list"); }
static V l_count(ARGS) { UNUSED_KW; need(n, 2, 2, "count"); L0; int64_t c = 0; for (uint32_t i = 0; i < l->len; i++) c += eq(l->v[i], a[1]); return vint(c); }
static V l_sort(ARGS) { need(n, 1, 1, "sort"); L0; V key; int rev; sort_kw(kwn, kwv, nk, &key, &rev); return list_sort(l, key, rev); }
static V l_reverse(ARGS) { UNUSED_KW; (void)n; L0; for (uint32_t i = 0; i < l->len / 2; i++) { V t = l->v[i]; l->v[i] = l->v[l->len - 1 - i]; l->v[l->len - 1 - i] = t; } return vnone(); }
static V l_clear(ARGS) { UNUSED_KW; (void)n; L0; l->len = 0; return vnone(); }
static V l_copy(ARGS) { UNUSED_KW; (void)n; return seq_from(SELF(a[0])); }
static V t_index(ARGS) { UNUSED_KW; need(n, 2, 4, "index"); Tuple *t = (Tuple *)SELF(a[0]).u.o; return seq_index(t->v, t->len, a, n, "tuple.index(x): x not in tuple"); }
static V t_count(ARGS) { UNUSED_KW; need(n, 2, 2, "count"); Tuple *t = (Tuple *)SELF(a[0]).u.o; int64_t c = 0; for (uint32_t i = 0; i < t->len; i++) c += eq(t->v[i], a[1]); return vint(c); }

/* ---- dict methods ------------------------------------------------------------------------------------------- */
#define D0 Dict *d = as_dict(SELF(a[0]))
static V d_get(ARGS) { UNUSED_KW; need(n, 2, 3, "get"); D0; V v; if (dict_get(d, a[1], &v)) return v; return n > 2 ? a[2] : vnone(); }
static V dict_part(V *a, int part) {
    D0;
    List *l = list_new(d->n);
    uint32_t pos = 0; V k, v;
    while (dict_next(d, &pos, &k, &v)) {
        if (part == 0) list_push(l, k);
        else if (part == 1) list_push(l, v);
        else { Tuple *t = tuple_new(2); t->v[0] = k; t->v[1] = v; list_push(l, vobj(t)); }
    }
    return vobj(l);
}
static V d_keys(ARGS) { UNUSED_KW; (void)n; return dict_part(a, 0); }
static V d_values(ARGS) { UNUSED_KW; (void)n; return dict_part(a, 1); }
static V d_items(ARGS) { UNUSED_KW; (void)n; return dict_part(a, 2); }
static V d_pop(ARGS) {
    UNUSED_KW; need(n, 2, 3, "pop"); D0;
    V v;
    if (dict_get(d, a[1], &v)) { dict_del(d, a[1]); return v; }
    if (n > 2) return a[2];
    raise(make_exc(P->e_key, a[1]));
}
static V d_popitem(ARGS) {
    UNUSED_KW; (void)n; D0;
    for (int64_t i = (int64_t)d->used - 1; i >= 0; i--) {
        if (d->e[i].key.t == V_UNBOUND) continue;
        Tuple *t = tuple_new(2); t->v[0] = d->e[i].key; t->v[1] = d->e[i].val;
        dict_del(d, t->v[0]);
        return vobj(t);
    }
    raise_new(P->e_key, "popitem(): dictionary is empty");
    return vnone();
}
static V d_setdefault(ARGS) { UNUSED_KW; need(n, 2, 3, "setdefault"); D0; V v; if (dict_get(d, a[1], &v)) return v; v = n > 2 ? a[2] : vnone(); dict_set(d, a[1], v); return v; }
static V d_update(ARGS) { D0; if (n > 1) dict_update(d, a[1]); for (int i = 0; i < nk; i++) dict_set(d, kwn[i], kwv[i]); return vnone(); }
static V d_clear(ARGS) { UNUSED_KW; (void)n; D0; d->n = d->used = 0; d->icap = 0; d->cap = 0; d->e = 0; d->idx = 0; return vnone(); }
static V d_copy(ARGS) { UNUSED_KW; (void)n; D0; Dict *r = dict_new(); dict_update(r, vobj(d)); return vobj(r); }
static V d_fromkeys(ARGS) {
    UNUSED_KW;
    /* dict.fromkeys(keys, value): called on the class, so no self */
    int o = n > 0 && is_obj(a[0], O_DICT) ? 1 : 0;
    Dict *r = dict_new();
    if (n <= o) raise_new(P->e_type, "fromkeys expected at least 1 argument");
    V it = iter_of(a[o]), k;
    while (iter_next(it, &k)) dict_set(r, k, n > o + 1 ? a[o + 1] : vnone());
    return vobj(r);
}

/* ---- set methods ---------------------------------------------------------------------------------------------- */
static V st_add(ARGS) { UNUSED_KW; need(n, 2, 2, "add"); dict_set(as_dict(a[0]), a[1], vnone()); return vnone(); }
static V st_remove(ARGS) { UNUSED_KW; need(n, 2, 2, "remove"); if (!dict_del(as_dict(a[0]), a[1])) raise(make_exc(P->e_key, a[1])); return vnone(); }
static V st_discard(ARGS) { UNUSED_KW; need(n, 2, 2, "discard"); dict_del(as_dict(a[0]), a[1]); return vnone(); }
static V st_pop(ARGS) {
    UNUSED_KW; (void)n;
    Dict *d = as_dict(a[0]); uint32_t pos = 0; V k;
    if (!dict_next(d, &pos, &k, 0)) raise_new(P->e_key, "pop from an empty set");
    dict_del(d, k);
    return k;
}
static V st_copy(ARGS) { UNUSED_KW; (void)n; Dict *r = set_new(); uint32_t pos = 0; V k; while (dict_next(as_dict(a[0]), &pos, &k, 0)) dict_set(r, k, vnone()); return vobj(r); }
static V set_of(V v) { if (is_obj(v, O_SET)) return v; V *a = &v; return c_set(a, 1, 0, 0, 0); }
static V st_op(V *a, int n, int op) {
    V r = st_copy(a, 1, 0, 0, 0);
    for (int i = 1; i < n; i++) r = binop(op, r, set_of(a[i]));
    return r;
}
static V st_union(ARGS) { UNUSED_KW; return st_op(a, n, B_OR); }
static V st_intersection(ARGS) { UNUSED_KW; return st_op(a, n, B_AND); }
static V st_difference(ARGS) { UNUSED_KW; return st_op(a, n, B_SUB); }
static V st_symdiff(ARGS) { UNUSED_KW; return st_op(a, n, B_XOR); }
static V st_update(ARGS) {
    UNUSED_KW;
    for (int i = 1; i < n; i++) { V it = iter_of(a[i]), x; while (iter_next(it, &x)) dict_set(as_dict(a[0]), x, vnone()); }
    return vnone();
}
static V st_inplace(V *a, int n, int op) {
    V r = st_op(a, n, op);
    Dict *d = as_dict(a[0]), *s = as_dict(r);
    *d = *s; d->type = O_SET;
    return vnone();
}
static V st_intersection_update(ARGS) { UNUSED_KW; return st_inplace(a, n, B_AND); }
static V st_difference_update(ARGS) { UNUSED_KW; return st_inplace(a, n, B_SUB); }
static V st_issubset(ARGS) { UNUSED_KW; need(n, 2, 2, "issubset"); return vbool(compare(a[0], set_of(a[1]), C_LE)); }
static V st_issuperset(ARGS) { UNUSED_KW; need(n, 2, 2, "issuperset"); return vbool(compare(a[0], set_of(a[1]), C_GE)); }
static V st_isdisjoint(ARGS) { UNUSED_KW; need(n, 2, 2, "isdisjoint"); V r = binop(B_AND, a[0], set_of(a[1])); return vbool(!as_dict(r)->n); }

/* ---- numbers' methods ---------------------------------------------------------------------------------------- */
static V i_bit_length(ARGS) {
    UNUSED_KW; (void)n;
    V x = a[0];
    if (int_cmp(x, vint(0)) < 0) x = int_neg(x);
    int64_t bits = 0;
    while (int_cmp(x, vint(0)) > 0) { x = int_shr(x, 1); bits++; }
    return vint(bits);
}
static V f_is_integer(ARGS) { UNUSED_KW; (void)n; return vbool(a[0].t == V_FLOAT ? a[0].u.f == floor(a[0].u.f) : 1); }

/* ---- the method tables ----------------------------------------------------------------------------------------- */
typedef struct { const char *name; native_fn fn; } meth_t;
static const meth_t STR_M[] = {
    { "upper", s_upper }, { "lower", s_lower }, { "casefold", s_lower }, { "swapcase", s_swapcase }, { "title", s_title },
    { "capitalize", s_capitalize }, { "strip", s_strip }, { "lstrip", s_lstrip }, { "rstrip", s_rstrip }, { "split", s_split },
    { "rsplit", s_rsplit }, { "splitlines", s_splitlines }, { "join", s_join }, { "replace", s_replace }, { "find", s_find },
    { "rfind", s_rfind }, { "index", s_index }, { "rindex", s_rindex }, { "count", s_count }, { "startswith", s_startswith },
    { "endswith", s_endswith }, { "isdigit", s_isdigit }, { "isnumeric", s_isdigit }, { "isdecimal", s_isdigit },
    { "isalpha", s_isalpha }, { "isalnum", s_isalnum }, { "isspace", s_isspace }, { "isupper", s_isupper },
    { "islower", s_islower }, { "isidentifier", s_isidentifier }, { "ljust", s_ljust }, { "rjust", s_rjust },
    { "center", s_center }, { "zfill", s_zfill }, { "format", s_format }, { "partition", s_partition },
    { "rpartition", s_rpartition }, { "encode", s_encode }, { "decode", s_encode }, { "expandtabs", s_expandtabs },
    { "removeprefix", s_removeprefix }, { "removesuffix", s_removesuffix }, { 0, 0 } };
static const meth_t LIST_M[] = {
    { "append", l_append }, { "extend", l_extend }, { "insert", l_insert }, { "pop", l_pop }, { "remove", l_remove },
    { "index", l_index }, { "count", l_count }, { "sort", l_sort }, { "reverse", l_reverse }, { "clear", l_clear },
    { "copy", l_copy }, { 0, 0 } };
static const meth_t TUPLE_M[] = { { "index", t_index }, { "count", t_count }, { 0, 0 } };
static const meth_t DICT_M[] = {
    { "get", d_get }, { "keys", d_keys }, { "values", d_values }, { "items", d_items }, { "pop", d_pop },
    { "popitem", d_popitem }, { "setdefault", d_setdefault }, { "update", d_update }, { "clear", d_clear },
    { "copy", d_copy }, { "fromkeys", d_fromkeys }, { 0, 0 } };
static const meth_t SET_M[] = {
    { "add", st_add }, { "remove", st_remove }, { "discard", st_discard }, { "pop", st_pop }, { "clear", d_clear },
    { "copy", st_copy }, { "union", st_union }, { "intersection", st_intersection }, { "difference", st_difference },
    { "symmetric_difference", st_symdiff }, { "update", st_update }, { "intersection_update", st_intersection_update },
    { "difference_update", st_difference_update }, { "issubset", st_issubset }, { "issuperset", st_issuperset },
    { "isdisjoint", st_isdisjoint }, { 0, 0 } };
static const meth_t FILE_M[] = {
    { "read", f_read }, { "readline", f_readline }, { "readlines", f_readlines }, { "write", f_write },
    { "writelines", f_writelines }, { "close", f_close }, { "flush", f_flush }, { "__enter__", f_enter },
    { "__exit__", f_exit }, { 0, 0 } };
static const meth_t INT_M[] = { { "bit_length", i_bit_length }, { 0, 0 } };
static const meth_t FLOAT_M[] = { { "is_integer", f_is_integer }, { 0, 0 } };

static void methods(Class *c, const meth_t *m) {
    for (; m->name; m++) dict_set_str(c->dict, intern_c(m->name), native(m->name, m->fn));
}

V method_of(V self, Str *name, int *found) {
    Class *c = type_of(self);
    V m;
    *found = 0;
    if (is_obj(self, O_CLASS)) {
        /* str.upper, dict.fromkeys: the unbound native */
        Class *k = (Class *)self.u.o;
        for (int i = 0; i < k->nmro; i++) if (dict_get_str(k->mro[i]->dict, name, &m) && is_obj(m, O_NATIVE)) { *found = 1; return m; }
        return vnone();
    }
    for (int i = 0; i < c->nmro; i++) {
        if (!dict_get_str(c->mro[i]->dict, name, &m)) continue;
        if (!is_obj(m, O_NATIVE)) continue;
        Native *nt = (Native *)m.u.o;
        if (nt->fn == d_fromkeys) { *found = 1; return m; }
        Native *b = rt_alloc(sizeof(Native));
        *b = *nt; b->self = self; b->has_self = 1;
        *found = 1;
        return vobj(b);
    }
    return vnone();
}

/* ---- modules in C ------------------------------------------------------------------------------------------------ */
static Module *mod_new(const char *name) {
    Module *m = rt_alloc(sizeof(Module));
    m->type = O_MODULE; m->name = intern_c(name); m->dict = dict_new();
    dict_set_str(m->dict, P->s_name, vobj(m->name));
    return m;
}
static void mod_fn(Module *m, const char *name, native_fn fn) { dict_set_str(m->dict, intern_c(name), native(name, fn)); }
static void mod_val(Module *m, const char *name, V v) { dict_set_str(m->dict, intern_c(name), v); }

#define MATH1(nm, expr) static V m_##nm(ARGS) { UNUSED_KW; need(n, 1, 1, #nm); double x = to_f(a[0]); (void)x; return vfloat(expr); }
static void dom(int bad, const char *fn) { if (bad) raise_new(P->e_value, "math domain error (%s)", fn); }
static V m_sqrt(ARGS) { UNUSED_KW; need(n, 1, 1, "sqrt"); double x = to_f(a[0]); dom(x < 0, "sqrt"); return vfloat(sqrt(x)); }
MATH1(sin, sin(x)) MATH1(cos, cos(x)) MATH1(tan, tan(x)) MATH1(atan, atan(x)) MATH1(sinh, sinh(x)) MATH1(cosh, cosh(x))
MATH1(tanh, tanh(x)) MATH1(exp, exp(x)) MATH1(fabs, fabs(x)) MATH1(degrees, x * 57.29577951308232) MATH1(radians, x / 57.29577951308232)
MATH1(expm1, exp(x) - 1) MATH1(cbrt, cbrt(x))
static V m_asin(ARGS) { UNUSED_KW; need(n, 1, 1, "asin"); double x = to_f(a[0]); dom(x < -1 || x > 1, "asin"); return vfloat(asin(x)); }
static V m_acos(ARGS) { UNUSED_KW; need(n, 1, 1, "acos"); double x = to_f(a[0]); dom(x < -1 || x > 1, "acos"); return vfloat(acos(x)); }
static V m_atan2(ARGS) { UNUSED_KW; need(n, 2, 2, "atan2"); return vfloat(atan2(to_f(a[0]), to_f(a[1]))); }
static V m_log(ARGS) {
    UNUSED_KW; need(n, 1, 2, "log");
    double x = to_f(a[0]);
    dom(x <= 0, "log");
    if (n == 2) { double b = to_f(a[1]); dom(b <= 0 || b == 1, "log"); return vfloat(log(x) / log(b)); }
    return vfloat(log(x));
}
static V m_log2(ARGS) { UNUSED_KW; need(n, 1, 1, "log2"); double x = to_f(a[0]); dom(x <= 0, "log2"); return vfloat(log2(x)); }
static V m_log10(ARGS) { UNUSED_KW; need(n, 1, 1, "log10"); double x = to_f(a[0]); dom(x <= 0, "log10"); return vfloat(log10(x)); }
static V m_pow(ARGS) { UNUSED_KW; need(n, 2, 2, "pow"); return vfloat(pow(to_f(a[0]), to_f(a[1]))); }
static V m_floor(ARGS) { UNUSED_KW; need(n, 1, 1, "floor"); if (is_int(a[0])) return a[0]; return int_from_double(floor(to_f(a[0]))); }
static V m_ceil(ARGS) { UNUSED_KW; need(n, 1, 1, "ceil"); if (is_int(a[0])) return a[0]; return int_from_double(ceil(to_f(a[0]))); }
static V m_trunc(ARGS) { UNUSED_KW; need(n, 1, 1, "trunc"); if (is_int(a[0])) return a[0]; return int_from_double(to_f(a[0])); }
static V m_fmod(ARGS) { UNUSED_KW; need(n, 2, 2, "fmod"); double y = to_f(a[1]); dom(y == 0, "fmod"); return vfloat(fmod(to_f(a[0]), y)); }
static V m_hypot(ARGS) { UNUSED_KW; double s = 0; for (int i = 0; i < n; i++) { double x = to_f(a[i]); s += x * x; } return vfloat(sqrt(s)); }
static V m_isqrt(ARGS) {
    UNUSED_KW; need(n, 1, 1, "isqrt");
    V x = a[0];
    if (int_cmp(x, vint(0)) < 0) raise_new(P->e_value, "isqrt() argument must be nonnegative");
    if (int_cmp(x, vint(0)) == 0) return vint(0);
    V r = int_from_double(sqrt(int_to_double(x)));
    for (int i = 0; i < 200; i++) {                         /* Newton, then settle */
        V nr = int_floordiv(int_add(r, int_floordiv(x, r)), vint(2));
        if (int_cmp(nr, r) >= 0) break;
        r = nr;
    }
    while (int_cmp(int_mul(r, r), x) > 0) r = int_sub(r, vint(1));
    while (int_cmp(int_mul(int_add(r, vint(1)), int_add(r, vint(1))), x) <= 0) r = int_add(r, vint(1));
    return r;
}
static V m_factorial(ARGS) {
    UNUSED_KW; need(n, 1, 1, "factorial");
    int64_t k = to_i64(a[0], "factorial()");
    if (k < 0) raise_new(P->e_value, "factorial() not defined for negative values");
    V r = vint(1);
    for (int64_t i = 2; i <= k; i++) r = int_mul(r, vint(i));
    return r;
}
static V m_gcd(ARGS) {
    UNUSED_KW;
    V g = vint(0);
    for (int i = 0; i < n; i++) {
        V b = a[i];
        if (int_cmp(b, vint(0)) < 0) b = int_neg(b);
        V x = g;
        while (int_cmp(b, vint(0)) != 0) { V t = int_mod(x, b); x = b; b = t; }
        g = x;
    }
    return g;
}
static V m_lcm(ARGS) {
    UNUSED_KW;
    V l = vint(1);
    for (int i = 0; i < n; i++) {
        V b = a[i];
        if (int_cmp(b, vint(0)) == 0) return vint(0);
        V pair[2] = { l, b };
        V g = m_gcd(pair, 2, 0, 0, 0);
        l = int_floordiv(int_mul(l, b), g);
        if (int_cmp(l, vint(0)) < 0) l = int_neg(l);
    }
    return l;
}
static V m_comb(ARGS) {
    UNUSED_KW; need(n, 2, 2, "comb");
    int64_t N = to_i64(a[0], "n"), k = to_i64(a[1], "k");
    if (N < 0 || k < 0) raise_new(P->e_value, "n and k must be non-negative integers");
    if (k > N) return vint(0);
    if (k > N - k) k = N - k;
    V r = vint(1);
    for (int64_t i = 1; i <= k; i++) r = int_floordiv(int_mul(r, vint(N - k + i)), vint(i));
    return r;
}
static V m_perm(ARGS) {
    UNUSED_KW; need(n, 1, 2, "perm");
    int64_t N = to_i64(a[0], "n"), k = n > 1 && a[1].t != V_NONE ? to_i64(a[1], "k") : N;
    if (k > N) return vint(0);
    V r = vint(1);
    for (int64_t i = 0; i < k; i++) r = int_mul(r, vint(N - i));
    return r;
}
static V m_isfinite(ARGS) { UNUSED_KW; need(n, 1, 1, "isfinite"); double x = to_f(a[0]); return vbool(x == x && x != __builtin_inf() && x != -__builtin_inf()); }
static V m_isinf(ARGS) { UNUSED_KW; need(n, 1, 1, "isinf"); double x = to_f(a[0]); return vbool(x == __builtin_inf() || x == -__builtin_inf()); }
static V m_isnan(ARGS) { UNUSED_KW; need(n, 1, 1, "isnan"); double x = to_f(a[0]); return vbool(x != x); }
static V m_isclose(ARGS) {
    need(n, 2, 2, "isclose");
    double x = to_f(a[0]), y = to_f(a[1]); V rt = vfloat(1e-9), at = vfloat(0);
    kwarg(kwn, kwv, nk, "rel_tol", &rt); kwarg(kwn, kwv, nk, "abs_tol", &at);
    double d = fabs(x - y), m = fabs(x) > fabs(y) ? fabs(x) : fabs(y);
    return vbool(x == y || d <= to_f(rt) * m || d <= to_f(at));
}
static V m_copysign(ARGS) { UNUSED_KW; need(n, 2, 2, "copysign"); return vfloat(copysign(to_f(a[0]), to_f(a[1]))); }
static V m_prod(ARGS) { UNUSED_KW; need(n, 1, 1, "prod"); V acc = vint(1), it = iter_of(a[0]), x; while (iter_next(it, &x)) acc = binop(B_MUL, acc, x); return acc; }
static V m_fsum(ARGS) {
    UNUSED_KW; need(n, 1, 1, "fsum");
    double s = 0, c = 0; V it = iter_of(a[0]), x;
    while (iter_next(it, &x)) { double y = to_f(x) - c, t = s + y; c = (t - s) - y; s = t; }   /* Kahan */
    return vfloat(s);
}
static V m_dist(ARGS) {
    UNUSED_KW; need(n, 2, 2, "dist");
    List *p = as_list(seq_from(a[0])), *q = as_list(seq_from(a[1]));
    if (p->len != q->len) raise_new(P->e_value, "both points must have the same number of dimensions");
    double s = 0;
    for (uint32_t i = 0; i < p->len; i++) { double d = to_f(p->v[i]) - to_f(q->v[i]); s += d * d; }
    return vfloat(sqrt(s));
}

static double frand(void) { return (double)(((uint64_t)rt_rand() << 21) ^ (rt_rand() >> 11)) / 9007199254740992.0; }
static V r_random(ARGS) { UNUSED_KW; (void)a; (void)n; return vfloat(frand()); }
static int64_t rand_below(int64_t n) { if (n <= 0) raise_new(P->e_value, "empty range for randrange()"); return (int64_t)((((uint64_t)rt_rand() << 32) | rt_rand()) % (uint64_t)n); }
static V r_randint(ARGS) { UNUSED_KW; need(n, 2, 2, "randint"); int64_t lo = to_i64(a[0], "a"), hi = to_i64(a[1], "b"); if (hi < lo) raise_new(P->e_value, "empty range for randint(%lld, %lld)", (long long)lo, (long long)hi); return vint(lo + rand_below(hi - lo + 1)); }
static V r_randrange(ARGS) {
    UNUSED_KW; need(n, 1, 3, "randrange");
    int64_t lo = 0, hi, st = 1;
    if (n == 1) hi = to_i64(a[0], "stop"); else { lo = to_i64(a[0], "start"); hi = to_i64(a[1], "stop"); if (n > 2) st = to_i64(a[2], "step"); }
    if (!st) raise_new(P->e_value, "zero step for randrange()");
    int64_t cnt = st > 0 ? (hi - lo + st - 1) / st : (lo - hi - st - 1) / -st;
    return vint(lo + st * rand_below(cnt));
}
static V r_uniform(ARGS) { UNUSED_KW; need(n, 2, 2, "uniform"); double lo = to_f(a[0]), hi = to_f(a[1]); return vfloat(lo + (hi - lo) * frand()); }
static V r_choice(ARGS) {
    UNUSED_KW; need(n, 1, 1, "choice");
    int64_t len = len_of(a[0]);
    if (!len) raise_new(P->e_index, "Cannot choose from an empty sequence");
    return get_item(a[0], vint(rand_below(len)));
}
static V r_shuffle(ARGS) {
    UNUSED_KW; need(n, 1, 1, "shuffle");
    List *l = as_list(SELF(a[0]));
    for (uint32_t i = l->len; i > 1; i--) { uint32_t j = (uint32_t)rand_below(i); V t = l->v[i - 1]; l->v[i - 1] = l->v[j]; l->v[j] = t; }
    return vnone();
}
static V r_sample(ARGS) {
    UNUSED_KW; need(n, 2, 2, "sample");
    List *src = as_list(seq_from(a[0]));
    int64_t k = to_i64(a[1], "k");
    if (k < 0 || k > (int64_t)src->len) raise_new(P->e_value, "Sample larger than population or is negative");
    for (int64_t i = 0; i < k; i++) { uint32_t j = (uint32_t)(i + rand_below((int64_t)src->len - i)); V t = src->v[i]; src->v[i] = src->v[j]; src->v[j] = t; }
    src->len = (uint32_t)k;
    return vobj(src);
}
static V r_choices(ARGS) {
    need(n, 1, 2, "choices");
    V kv = vint(1), wv = n > 1 ? a[1] : vnone();
    kwarg(kwn, kwv, nk, "k", &kv); kwarg(kwn, kwv, nk, "weights", &wv);
    List *src = as_list(seq_from(a[0]));
    if (!src->len) raise_new(P->e_index, "Cannot choose from an empty population");
    int64_t k = to_i64(kv, "k");
    List *out = list_new((uint32_t)k);
    double *cum = 0, total = 0;
    if (wv.t != V_NONE) {
        List *w = as_list(seq_from(wv));
        if (w->len != src->len) raise_new(P->e_value, "The number of weights does not match the population");
        cum = rt_alloc_leaf(sizeof(double) * src->len);
        for (uint32_t i = 0; i < w->len; i++) { total += to_f(w->v[i]); cum[i] = total; }
    }
    for (int64_t i = 0; i < k; i++) {
        if (!cum) { list_push(out, src->v[rand_below(src->len)]); continue; }
        double r = frand() * total; uint32_t j = 0;
        while (j + 1 < src->len && cum[j] <= r) j++;
        list_push(out, src->v[j]);
    }
    return vobj(out);
}
static V r_seed(ARGS) { UNUSED_KW; rt_seed(n && a[0].t != V_NONE ? hash_of(a[0]) : rt_ms() * 2654435761u); return vnone(); }
static V r_gauss(ARGS) {
    UNUSED_KW;
    double mu = n > 0 ? to_f(a[0]) : 0, sg = n > 1 ? to_f(a[1]) : 1, u1 = frand(), u2 = frand();
    if (u1 < 1e-300) u1 = 1e-300;
    return vfloat(mu + sg * sqrt(-2 * log(u1)) * cos(6.283185307179586 * u2));
}

static V t_time(ARGS) {
    UNUSED_KW; (void)a; (void)n;
    extern int clock_known(void); extern uint64_t clock_unix(void);
    double ms = (double)(rt_ms() % 1000) / 1000.0;
    return vfloat(clock_known() ? (double)clock_unix() + ms : (double)rt_ms() / 1000.0);
}
static V t_mono(ARGS) { UNUSED_KW; (void)a; (void)n; return vfloat((double)rt_ms() / 1000.0); }
static V t_mono_ns(ARGS) { UNUSED_KW; (void)a; (void)n; return vint((int64_t)rt_ms() * 1000000); }
static V t_sleep(ARGS) {
    UNUSED_KW; need(n, 1, 1, "sleep");
    double s = to_f(a[0]);
    if (s < 0) raise_new(P->e_value, "sleep length must be non-negative");
    rt_sleep_ms((uint64_t)(s * 1000));
    return vnone();
}
static V t_strftime(ARGS) {
    UNUSED_KW; need(n, 1, 2, "strftime");
    extern void clock_local(int *y, int *mo, int *d, int *h, int *mi, int *s, int *wday);
    int y, mo, d, h, mi, s, wd;
    clock_local(&y, &mo, &d, &h, &mi, &s, &wd);
    Str *f = arg_str(a[0], "strftime");
    static const char *const DN[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const MN[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    SB sb; sb_new(&sb, 32);
    for (uint32_t i = 0; i < f->len; i++) {
        char b[32];
        if (f->s[i] != '%' || i + 1 >= f->len) { sb_addc(&sb, f->s[i]); continue; }
        switch (f->s[++i]) {
        case 'Y': snprintf(b, sizeof b, "%04d", y); break;
        case 'm': snprintf(b, sizeof b, "%02d", mo); break;
        case 'd': snprintf(b, sizeof b, "%02d", d); break;
        case 'H': snprintf(b, sizeof b, "%02d", h); break;
        case 'M': snprintf(b, sizeof b, "%02d", mi); break;
        case 'S': snprintf(b, sizeof b, "%02d", s); break;
        case 'A': snprintf(b, sizeof b, "%s", DN[wd]); break;
        case 'a': snprintf(b, sizeof b, "%.3s", DN[wd]); break;
        case 'B': snprintf(b, sizeof b, "%s", MN[mo - 1]); break;
        case 'b': snprintf(b, sizeof b, "%.3s", MN[mo - 1]); break;
        case 'y': snprintf(b, sizeof b, "%02d", y % 100); break;
        case '%': snprintf(b, sizeof b, "%%"); break;
        default: snprintf(b, sizeof b, "%%%c", f->s[i]);
        }
        sb_adds(&sb, b);
    }
    return vobj(sb_str(&sb));
}

static V sys_write(ARGS) { UNUSED_KW; need(n, 1, 1, "write"); Str *s = to_str(a[0]); rt_write(s->s, s->len); return vint(s->len); }
static V sys_flush(ARGS) { UNUSED_KW; (void)a; (void)n; return vnone(); }
static V sys_readline(ARGS) { UNUSED_KW; (void)a; (void)n; char buf[4096]; if (rt_readline("", buf, sizeof buf) < 0) return vstr(""); size_t l = strlen(buf); buf[l] = '\n'; rt_write(buf, l + 1); return vstrn(buf, (uint32_t)l + 1); }

static V os_getcwd(ARGS) { UNUSED_KW; (void)a; (void)n; return vstr(rt_cwd()); }
static V os_join(ARGS) {
    UNUSED_KW;
    SB sb; sb_new(&sb, 64);
    for (int i = 0; i < n; i++) {
        Str *p = arg_str(a[i], "join");
        if (p->len && p->s[0] == '/') sb.n = 0;
        else if (sb.n && sb.b[sb.n - 1] != '/') sb_addc(&sb, '/');
        sb_addstr(&sb, p);
    }
    return vobj(sb_str(&sb));
}
static V os_basename(ARGS) { UNUSED_KW; need(n, 1, 1, "basename"); Str *p = arg_str(a[0], "basename"); const char *s = strrchr(p->s, '/'); return vstr(s ? s + 1 : p->s); }
static V os_dirname(ARGS) { UNUSED_KW; need(n, 1, 1, "dirname"); Str *p = arg_str(a[0], "dirname"); const char *s = strrchr(p->s, '/'); return s ? vstrn(p->s, (uint32_t)(s - p->s ? s - p->s : 1)) : vstr(""); }
static V os_splitext(ARGS) {
    UNUSED_KW; need(n, 1, 1, "splitext");
    Str *p = arg_str(a[0], "splitext");
    const char *dot = strrchr(p->s, '.'), *sl = strrchr(p->s, '/');
    Tuple *t = tuple_new(2);
    if (!dot || (sl && dot < sl) || dot == p->s || (sl && dot == sl + 1)) { t->v[0] = vobj(p); t->v[1] = vstr(""); }
    else { t->v[0] = vstrn(p->s, (uint32_t)(dot - p->s)); t->v[1] = vstr(dot); }
    return vobj(t);
}
static V os_exists(ARGS) {
    UNUSED_KW; need(n, 1, 1, "exists");
    char path[300]; path_for(arg_str(a[0], "exists"), path, sizeof path);
    char *d; uint32_t l; const char *why;
    return vbool(rt_file_read(path, &d, &l, &why) == 0);
}

/* ---- json ------------------------------------------------------------------------------------------------- */
static void json_out(SB *sb, V v, int indent, int depth) {
    if (depth > 100) raise_new(P->e_value, "Circular reference detected");
    char b[64];
    switch (v.t) {
    case V_NONE: sb_adds(sb, "null"); return;
    case V_BOOL: sb_adds(sb, v.u.i ? "true" : "false"); return;
    case V_INT: snprintf(b, sizeof b, "%lld", (long long)v.u.i); sb_adds(sb, b); return;
    case V_FLOAT: if (v.u.f != v.u.f) sb_adds(sb, "NaN"); else { num_repr(v.u.f, b, sizeof b); sb_adds(sb, b); } return;
    }
    if (is_obj(v, O_BIG)) { sb_addstr(sb, int_to_str(v, 10)); return; }
    if (is_obj(v, O_STR)) {
        Str *s = as_str(v);
        sb_addc(sb, '"');
        for (uint32_t i = 0; i < s->len; i++) {
            unsigned char c = (unsigned char)s->s[i];
            if (c == '"' || c == '\\') { sb_addc(sb, '\\'); sb_addc(sb, (char)c); }
            else if (c == '\n') sb_adds(sb, "\\n");
            else if (c == '\t') sb_adds(sb, "\\t");
            else if (c == '\r') sb_adds(sb, "\\r");
            else if (c < 32 || c >= 127) { snprintf(b, sizeof b, "\\u%04x", c); sb_adds(sb, b); }
            else sb_addc(sb, (char)c);
        }
        sb_addc(sb, '"');
        return;
    }
    int lst = is_obj(v, O_LIST) || is_obj(v, O_TUPLE);
    if (lst || is_obj(v, O_DICT)) {
        sb_addc(sb, lst ? '[' : '{');
        uint32_t n = lst ? (is_obj(v, O_LIST) ? as_list(v)->len : ((Tuple *)v.u.o)->len) : as_dict(v)->n, i = 0, pos = 0;
        V k, x;
        for (;;) {
            if (lst) { if (i >= n) break; x = is_obj(v, O_LIST) ? as_list(v)->v[i] : ((Tuple *)v.u.o)->v[i]; }
            else if (!dict_next(as_dict(v), &pos, &k, &x)) break;
            if (i) sb_addc(sb, ',');
            if (indent >= 0) { sb_addc(sb, '\n'); for (int z = 0; z < indent * (depth + 1); z++) sb_addc(sb, ' '); }
            else if (i) sb_addc(sb, ' ');
            if (!lst) {
                if (!is_obj(k, O_STR)) k = vobj(to_str(k));
                json_out(sb, k, indent, depth + 1); sb_adds(sb, ": ");
            }
            json_out(sb, x, indent, depth + 1);
            i++;
        }
        if (indent >= 0 && i) { sb_addc(sb, '\n'); for (int z = 0; z < indent * depth; z++) sb_addc(sb, ' '); }
        sb_addc(sb, lst ? ']' : '}');
        return;
    }
    raise_new(P->e_type, "Object of type %s is not JSON serializable", type_name(v));
}
static V j_dumps(ARGS) {
    need(n, 1, 1, "dumps");
    V ind = vnone();
    kwarg(kwn, kwv, nk, "indent", &ind);
    SB sb; sb_new(&sb, 128);
    json_out(&sb, a[0], ind.t == V_NONE ? -1 : (int)to_i64(ind, "indent"), 0);
    return vobj(sb_str(&sb));
}
typedef struct { const char *s; uint32_t i, n; } JP;
static void jws(JP *p) { while (p->i < p->n && is_space((unsigned char)p->s[p->i])) p->i++; }
static V jerr(JP *p) { raise_new(P->e_value, "Expecting value: char %u", p->i); return vnone(); }
static V jval(JP *p, int depth) {
    jws(p);
    if (p->i >= p->n || depth > 200) return jerr(p);
    char c = p->s[p->i];
    if (c == '{' || c == '[') {
        int obj = c == '{';
        p->i++;
        V out = obj ? vobj(dict_new()) : vobj(list_new(4));
        jws(p);
        if (p->i < p->n && p->s[p->i] == (obj ? '}' : ']')) { p->i++; return out; }
        for (;;) {
            if (obj) {
                V k = jval(p, depth + 1);
                if (!is_obj(k, O_STR)) return jerr(p);
                jws(p);
                if (p->i >= p->n || p->s[p->i] != ':') return jerr(p);
                p->i++;
                dict_set(as_dict(out), k, jval(p, depth + 1));
            } else list_push(as_list(out), jval(p, depth + 1));
            jws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == (obj ? '}' : ']')) { p->i++; return out; }
            return jerr(p);
        }
    }
    if (c == '"') {
        SB sb; sb_new(&sb, 32);
        p->i++;
        while (p->i < p->n && p->s[p->i] != '"') {
            char d = p->s[p->i++];
            if (d != '\\') { sb_addc(&sb, d); continue; }
            if (p->i >= p->n) return jerr(p);
            char e = p->s[p->i++];
            switch (e) {
            case 'n': sb_addc(&sb, '\n'); break;
            case 't': sb_addc(&sb, '\t'); break;
            case 'r': sb_addc(&sb, '\r'); break;
            case 'b': sb_addc(&sb, 8); break;
            case 'f': sb_addc(&sb, 12); break;
            case 'u': {
                unsigned v = 0;
                for (int k = 0; k < 4 && p->i < p->n; k++) { char h = p->s[p->i++]; v = v * 16 + (unsigned)(h <= '9' ? h - '0' : (h | 32) - 'a' + 10); }
                sb_addc(&sb, v < 256 ? (char)v : '?'); break;
            }
            default: sb_addc(&sb, e);
            }
        }
        p->i++;
        return vobj(sb_str(&sb));
    }
    if (!strncmp(p->s + p->i, "true", 4)) { p->i += 4; return vbool(1); }
    if (!strncmp(p->s + p->i, "false", 5)) { p->i += 5; return vbool(0); }
    if (!strncmp(p->s + p->i, "null", 4)) { p->i += 4; return vnone(); }
    const char *e;
    uint32_t st = p->i;
    int isf = 0;
    while (p->i < p->n && strchr("+-0123456789.eE", p->s[p->i])) { if (strchr(".eE", p->s[p->i])) isf = 1; p->i++; }
    if (p->i == st) return jerr(p);
    Str *num = str_new(p->s + st, p->i - st);
    if (isf) return vfloat(num_parse(num->s, &e));
    int ok; V r = int_parse(num->s, 10, &ok);
    if (!ok) return jerr(p);
    return r;
}
static V j_loads(ARGS) {
    UNUSED_KW; need(n, 1, 1, "loads");
    Str *s = arg_str(a[0], "loads");
    JP p = { s->s, 0, s->len };
    V v = jval(&p, 0);
    jws(&p);
    if (p.i != p.n) raise_new(P->e_value, "Extra data: char %u", p.i);
    return v;
}
static V j_dump(ARGS) { V s = j_dumps(a, 1, kwn, kwv, nk); need(n, 2, 2, "dump"); file_write_str(a[1], as_str(s)); return vnone(); }
static V j_load(ARGS) { UNUSED_KW; need(n, 1, 1, "load"); V s = f_read(a, 1, 0, 0, 0); return j_loads(&s, 1, 0, 0, 0); }

/* ---- modules written in Python ------------------------------------------------------------------------------- */
static const char PY_COLLECTIONS[] =
"class Counter:\n"
"    def __init__(self, it=None, **kw):\n"
"        self._d = {}\n"
"        if it is not None: self.update(it)\n"
"        for k in kw: self._d[k] = kw[k]\n"
"    def update(self, it):\n"
"        if isinstance(it, dict) or isinstance(it, Counter):\n"
"            for k in it.keys(): self._d[k] = self._d.get(k, 0) + it[k]\n"
"        else:\n"
"            for x in it: self._d[x] = self._d.get(x, 0) + 1\n"
"    def subtract(self, it):\n"
"        for x in it: self._d[x] = self._d.get(x, 0) - 1\n"
"    def __getitem__(self, k): return self._d.get(k, 0)\n"
"    def __setitem__(self, k, v): self._d[k] = v\n"
"    def __delitem__(self, k): del self._d[k]\n"
"    def __contains__(self, k): return k in self._d\n"
"    def __len__(self): return len(self._d)\n"
"    def __iter__(self): return iter(list(self._d))\n"
"    def __eq__(self, o): return self._d == (o._d if isinstance(o, Counter) else o)\n"
"    def keys(self): return self._d.keys()\n"
"    def values(self): return self._d.values()\n"
"    def items(self): return self._d.items()\n"
"    def get(self, k, d=None): return self._d.get(k, d)\n"
"    def total(self): return sum(self._d.values())\n"
"    def elements(self):\n"
"        out = []\n"
"        for k, v in self._d.items(): out.extend([k] * v)\n"
"        return out\n"
"    def most_common(self, n=None):\n"
"        s = sorted(self._d.items(), key=lambda kv: kv[1], reverse=True)\n"
"        return s if n is None else s[:n]\n"
"    def __add__(self, o):\n"
"        r = Counter(self._d)\n"
"        r.update(o)\n"
"        return r\n"
"    def __repr__(self):\n"
"        return 'Counter({' + ', '.join(repr(k) + ': ' + repr(v) for k, v in self.most_common()) + '})'\n"
"class defaultdict:\n"
"    def __init__(self, factory=None, *args):\n"
"        self.default_factory = factory\n"
"        self._d = dict(*args)\n"
"    def __getitem__(self, k):\n"
"        if k in self._d: return self._d[k]\n"
"        if self.default_factory is None: raise KeyError(k)\n"
"        v = self.default_factory()\n"
"        self._d[k] = v\n"
"        return v\n"
"    def __setitem__(self, k, v): self._d[k] = v\n"
"    def __delitem__(self, k): del self._d[k]\n"
"    def __contains__(self, k): return k in self._d\n"
"    def __len__(self): return len(self._d)\n"
"    def __iter__(self): return iter(list(self._d))\n"
"    def __eq__(self, o): return self._d == (o._d if isinstance(o, defaultdict) else o)\n"
"    def keys(self): return self._d.keys()\n"
"    def values(self): return self._d.values()\n"
"    def items(self): return self._d.items()\n"
"    def get(self, k, d=None): return self._d.get(k, d)\n"
"    def pop(self, k, *d): return self._d.pop(k, *d)\n"
"    def setdefault(self, k, d=None): return self._d.setdefault(k, d)\n"
"    def update(self, *a, **kw): self._d.update(*a, **kw)\n"
"    def __repr__(self): return 'defaultdict(' + repr(self.default_factory) + ', ' + repr(self._d) + ')'\n"
"class deque:\n"
"    def __init__(self, it=(), maxlen=None):\n"
"        self.maxlen = maxlen\n"
"        self._l = []\n"
"        for x in it: self.append(x)\n"
"    def append(self, x):\n"
"        self._l.append(x)\n"
"        if self.maxlen is not None and len(self._l) > self.maxlen: self._l.pop(0)\n"
"    def appendleft(self, x):\n"
"        self._l.insert(0, x)\n"
"        if self.maxlen is not None and len(self._l) > self.maxlen: self._l.pop()\n"
"    def pop(self):\n"
"        if not self._l: raise IndexError('pop from an empty deque')\n"
"        return self._l.pop()\n"
"    def popleft(self):\n"
"        if not self._l: raise IndexError('pop from an empty deque')\n"
"        return self._l.pop(0)\n"
"    def extend(self, it):\n"
"        for x in it: self.append(x)\n"
"    def extendleft(self, it):\n"
"        for x in it: self.appendleft(x)\n"
"    def rotate(self, n=1):\n"
"        if self._l:\n"
"            n = n % len(self._l)\n"
"            self._l = self._l[-n:] + self._l[:-n]\n"
"    def clear(self): self._l = []\n"
"    def __len__(self): return len(self._l)\n"
"    def __iter__(self): return iter(self._l)\n"
"    def __getitem__(self, i): return self._l[i]\n"
"    def __setitem__(self, i, v): self._l[i] = v\n"
"    def __contains__(self, x): return x in self._l\n"
"    def __bool__(self): return len(self._l) > 0\n"
"    def __repr__(self): return 'deque(' + repr(self._l) + ')'\n"
"OrderedDict = dict\n"
"def namedtuple(name, fields):\n"
"    if isinstance(fields, str): fields = fields.replace(',', ' ').split()\n"
"    fields = list(fields)\n"
"    class T:\n"
"        _fields = tuple(fields)\n"
"        def __init__(self, *a, **kw):\n"
"            vals = list(a)\n"
"            for f in fields[len(a):]:\n"
"                if f not in kw: raise TypeError(name + '() missing argument: ' + repr(f))\n"
"                vals.append(kw[f])\n"
"            if len(vals) != len(fields): raise TypeError(name + '() takes ' + str(len(fields)) + ' arguments')\n"
"            for f, v in zip(fields, vals): setattr(self, f, v)\n"
"        def __getitem__(self, i): return getattr(self, fields[i])\n"
"        def __iter__(self): return iter([getattr(self, f) for f in fields])\n"
"        def __len__(self): return len(fields)\n"
"        def __eq__(self, o): return list(self) == list(o)\n"
"        def __lt__(self, o): return list(self) < list(o)\n"
"        def __hash__(self): return hash(tuple(self))\n"
"        def _asdict(self): return {f: getattr(self, f) for f in fields}\n"
"        def _replace(self, **kw):\n"
"            d = self._asdict()\n"
"            d.update(kw)\n"
"            return T(**d)\n"
"        def __repr__(self): return name + '(' + ', '.join(f + '=' + repr(getattr(self, f)) for f in fields) + ')'\n"
"    T.__name__ = name\n"
"    return T\n";

static const char PY_ITERTOOLS[] =
"class count:\n"
"    def __init__(self, start=0, step=1): self.n = start; self.step = step\n"
"    def __iter__(self): return self\n"
"    def __next__(self):\n"
"        v = self.n\n"
"        self.n += self.step\n"
"        return v\n"
"class cycle:\n"
"    def __init__(self, it): self.items = list(it); self.i = 0\n"
"    def __iter__(self): return self\n"
"    def __next__(self):\n"
"        if not self.items: raise StopIteration\n"
"        v = self.items[self.i % len(self.items)]\n"
"        self.i += 1\n"
"        return v\n"
"class repeat:\n"
"    def __init__(self, x, times=None): self.x = x; self.left = times\n"
"    def __iter__(self): return self\n"
"    def __next__(self):\n"
"        if self.left is not None:\n"
"            if self.left <= 0: raise StopIteration\n"
"            self.left -= 1\n"
"        return self.x\n"
"def chain(*its):\n"
"    out = []\n"
"    for it in its: out.extend(it)\n"
"    return out\n"
"def islice(it, *a):\n"
"    s = slice(*a)\n"
"    start = s.start or 0\n"
"    stop = s.stop\n"
"    step = s.step or 1\n"
"    out = []\n"
"    i = 0\n"
"    for x in it:\n"
"        if stop is not None and i >= stop: break\n"
"        if i >= start and (i - start) % step == 0: out.append(x)\n"
"        i += 1\n"
"    return out\n"
"def accumulate(it, func=None, initial=None):\n"
"    out = []\n"
"    acc = initial\n"
"    if initial is not None: out.append(initial)\n"
"    for x in it:\n"
"        if acc is None and not out: acc = x\n"
"        else: acc = func(acc, x) if func else acc + x\n"
"        out.append(acc)\n"
"    return out\n"
"def product(*its, repeat=1):\n"
"    pools = [list(p) for p in its] * repeat\n"
"    res = [()]\n"
"    for p in pools: res = [r + (x,) for r in res for x in p]\n"
"    return res\n"
"def permutations(it, r=None):\n"
"    pool = list(it)\n"
"    n = len(pool)\n"
"    r = n if r is None else r\n"
"    out = []\n"
"    def go(cur, used):\n"
"        if len(cur) == r:\n"
"            out.append(tuple(cur))\n"
"            return\n"
"        for i in range(n):\n"
"            if not used[i]:\n"
"                used[i] = True\n"
"                cur.append(pool[i])\n"
"                go(cur, used)\n"
"                cur.pop()\n"
"                used[i] = False\n"
"    if r <= n: go([], [False] * n)\n"
"    return out\n"
"def combinations(it, r):\n"
"    pool = list(it)\n"
"    out = []\n"
"    def go(start, cur):\n"
"        if len(cur) == r:\n"
"            out.append(tuple(cur))\n"
"            return\n"
"        for i in range(start, len(pool)):\n"
"            cur.append(pool[i])\n"
"            go(i + 1, cur)\n"
"            cur.pop()\n"
"    go(0, [])\n"
"    return out\n"
"def combinations_with_replacement(it, r):\n"
"    pool = list(it)\n"
"    out = []\n"
"    def go(start, cur):\n"
"        if len(cur) == r:\n"
"            out.append(tuple(cur))\n"
"            return\n"
"        for i in range(start, len(pool)):\n"
"            cur.append(pool[i])\n"
"            go(i, cur)\n"
"            cur.pop()\n"
"    go(0, [])\n"
"    return out\n"
"def groupby(it, key=None):\n"
"    out = []\n"
"    for x in it:\n"
"        k = key(x) if key else x\n"
"        if out and out[-1][0] == k: out[-1][1].append(x)\n"
"        else: out.append((k, [x]))\n"
"    return out\n"
"def zip_longest(*its, fillvalue=None):\n"
"    ls = [list(i) for i in its]\n"
"    n = max(len(l) for l in ls) if ls else 0\n"
"    return [tuple(l[i] if i < len(l) else fillvalue for l in ls) for i in range(n)]\n"
"def pairwise(it):\n"
"    l = list(it)\n"
"    return [(l[i], l[i + 1]) for i in range(len(l) - 1)]\n";

static const char PY_FUNCTOOLS[] =
"def reduce(f, it, *init):\n"
"    it = list(it)\n"
"    if init: acc = init[0]\n"
"    elif not it: raise TypeError('reduce() of empty iterable with no initial value')\n"
"    else:\n"
"        acc = it[0]\n"
"        it = it[1:]\n"
"    for x in it: acc = f(acc, x)\n"
"    return acc\n"
"class partial:\n"
"    def __init__(self, f, *a, **kw): self.func = f; self.args = a; self.keywords = kw\n"
"    def __call__(self, *a, **kw):\n"
"        k = dict(self.keywords)\n"
"        k.update(kw)\n"
"        return self.func(*(self.args + a), **k)\n"
"def lru_cache(maxsize=128, typed=False):\n"
"    def deco(f):\n"
"        memo = {}\n"
"        def wrapper(*a):\n"
"            if a in memo: return memo[a]\n"
"            v = f(*a)\n"
"            memo[a] = v\n"
"            return v\n"
"        wrapper.__name__ = f.__name__\n"
"        return wrapper\n"
"    if callable(maxsize): return deco(maxsize)\n"
"    return deco\n"
"cache = lru_cache(None)\n"
"def wraps(f):\n"
"    def deco(g): return g\n"
"    return deco\n"
"def cmp_to_key(cmp):\n"
"    class K:\n"
"        def __init__(self, o): self.o = o\n"
"        def __lt__(self, other): return cmp(self.o, other.o) < 0\n"
"        def __gt__(self, other): return cmp(self.o, other.o) > 0\n"
"        def __eq__(self, other): return cmp(self.o, other.o) == 0\n"
"    return K\n";

static const char PY_TYPING[] =
"class _T:\n"
"    def __getitem__(self, k): return self\n"
"    def __call__(self, *a): return a[0] if a else None\n"
"Any = Optional = Union = List = Dict = Tuple = Set = Callable = Iterable = Iterator = Sequence = _T()\n"
"FrozenSet = Type = Generator = Mapping = _T()\n"
"def TypeVar(name, *a, **k): return _T()\n"
"def NamedTuple(name, fields): raise TypeError('NamedTuple: use collections.namedtuple')\n";

static const char PY_DATACLASSES[] =
"class _Field:\n"
"    def __init__(self, default, default_factory):\n"
"        self.default = default\n"
"        self.default_factory = default_factory\n"
"_MISSING = object()\n"
"def field(default=_MISSING, default_factory=None, **kw):\n"
"    return _Field(default, default_factory)\n"
"def dataclass(cls=None, eq=True, order=False, frozen=False, **kw):\n"
"    def wrap(cls):\n"
"        names = list(cls.__dict__.get('__annotations__', {}).keys())\n"
"        defaults = {}\n"
"        for n in names:\n"
"            if n in cls.__dict__: defaults[n] = cls.__dict__[n]\n"
"        def __init__(self, *args, **kwargs):\n"
"            if len(args) > len(names):\n"
"                raise TypeError('__init__() takes ' + str(len(names) + 1) + ' positional arguments but ' + str(len(args) + 1) + ' were given')\n"
"            for i, n in enumerate(names):\n"
"                if i < len(args): v = args[i]\n"
"                elif n in kwargs: v = kwargs[n]\n"
"                elif n in defaults:\n"
"                    d = defaults[n]\n"
"                    if isinstance(d, _Field):\n"
"                        if d.default_factory is not None: v = d.default_factory()\n"
"                        elif d.default is not _MISSING: v = d.default\n"
"                        else: raise TypeError(\"__init__() missing 1 required positional argument: '\" + n + \"'\")\n"
"                    else: v = d\n"
"                else: raise TypeError(\"__init__() missing 1 required positional argument: '\" + n + \"'\")\n"
"                setattr(self, n, v)\n"
"            if hasattr(self, '__post_init__'): self.__post_init__()\n"
"        def __repr__(self):\n"
"            return type(self).__name__ + '(' + ', '.join(n + '=' + repr(getattr(self, n)) for n in names) + ')'\n"
"        def __eq__(self, other):\n"
"            if type(other) is not type(self): return False\n"
"            return all(getattr(self, n) == getattr(other, n) for n in names)\n"
"        def key(self): return tuple(getattr(self, n) for n in names)\n"
"        cls.__init__ = __init__\n"
"        if '__repr__' not in cls.__dict__: cls.__repr__ = __repr__\n"
"        if eq and '__eq__' not in cls.__dict__: cls.__eq__ = __eq__\n"
"        if order:\n"
"            cls.__lt__ = lambda a, b: key(a) < key(b)\n"
"            cls.__le__ = lambda a, b: key(a) <= key(b)\n"
"            cls.__gt__ = lambda a, b: key(a) > key(b)\n"
"            cls.__ge__ = lambda a, b: key(a) >= key(b)\n"
"        return cls\n"
"    if cls is None: return wrap\n"
"    return wrap(cls)\n"
"def asdict(o): return {n: getattr(o, n) for n in type(o).__dict__.get('__annotations__', {})}\n";

static Module *py_module(const char *name, const char *src) {
    Module *m = mod_new(name);
    dict_set_str(P->modules, m->name, vobj(m));
    /* run it as the module's own globals */
    extern Module *import_source(Module *m, const char *src);
    import_source(m, src);
    return m;
}

Module *builtin_module(Str *name) {
    const char *s = name->s;
    if (!strcmp(s, "math")) {
        Module *m = mod_new("math");
        mod_val(m, "pi", vfloat(3.141592653589793)); mod_val(m, "e", vfloat(2.718281828459045));
        mod_val(m, "tau", vfloat(6.283185307179586)); mod_val(m, "inf", vfloat(__builtin_inf())); mod_val(m, "nan", vfloat(__builtin_nan("")));
        mod_fn(m, "sqrt", m_sqrt); mod_fn(m, "sin", m_sin); mod_fn(m, "cos", m_cos); mod_fn(m, "tan", m_tan);
        mod_fn(m, "asin", m_asin); mod_fn(m, "acos", m_acos); mod_fn(m, "atan", m_atan); mod_fn(m, "atan2", m_atan2);
        mod_fn(m, "sinh", m_sinh); mod_fn(m, "cosh", m_cosh); mod_fn(m, "tanh", m_tanh); mod_fn(m, "exp", m_exp);
        mod_fn(m, "expm1", m_expm1); mod_fn(m, "log", m_log); mod_fn(m, "log2", m_log2); mod_fn(m, "log10", m_log10);
        mod_fn(m, "pow", m_pow); mod_fn(m, "floor", m_floor); mod_fn(m, "ceil", m_ceil); mod_fn(m, "trunc", m_trunc);
        mod_fn(m, "fabs", m_fabs); mod_fn(m, "fmod", m_fmod); mod_fn(m, "hypot", m_hypot); mod_fn(m, "degrees", m_degrees);
        mod_fn(m, "radians", m_radians); mod_fn(m, "factorial", m_factorial); mod_fn(m, "gcd", m_gcd); mod_fn(m, "lcm", m_lcm);
        mod_fn(m, "isqrt", m_isqrt); mod_fn(m, "comb", m_comb); mod_fn(m, "perm", m_perm); mod_fn(m, "isfinite", m_isfinite);
        mod_fn(m, "isinf", m_isinf); mod_fn(m, "isnan", m_isnan); mod_fn(m, "isclose", m_isclose); mod_fn(m, "copysign", m_copysign);
        mod_fn(m, "prod", m_prod); mod_fn(m, "fsum", m_fsum); mod_fn(m, "dist", m_dist); mod_fn(m, "cbrt", m_cbrt);
        return m;
    }
    if (!strcmp(s, "random")) {
        Module *m = mod_new("random");
        mod_fn(m, "random", r_random); mod_fn(m, "randint", r_randint); mod_fn(m, "randrange", r_randrange);
        mod_fn(m, "uniform", r_uniform); mod_fn(m, "choice", r_choice); mod_fn(m, "choices", r_choices);
        mod_fn(m, "shuffle", r_shuffle); mod_fn(m, "sample", r_sample); mod_fn(m, "seed", r_seed); mod_fn(m, "gauss", r_gauss);
        mod_fn(m, "normalvariate", r_gauss);
        return m;
    }
    if (!strcmp(s, "time")) {
        Module *m = mod_new("time");
        mod_fn(m, "time", t_time); mod_fn(m, "sleep", t_sleep); mod_fn(m, "monotonic", t_mono); mod_fn(m, "perf_counter", t_mono);
        mod_fn(m, "process_time", t_mono); mod_fn(m, "monotonic_ns", t_mono_ns); mod_fn(m, "perf_counter_ns", t_mono_ns);
        mod_fn(m, "time_ns", t_mono_ns); mod_fn(m, "strftime", t_strftime);
        return m;
    }
    if (!strcmp(s, "sys")) {
        Module *m = mod_new("sys");
        List *argv = list_new(1); list_push(argv, vobj(P->main_file));
        mod_val(m, "argv", vobj(argv));
        mod_val(m, "version", vstr("3.12 (BoltOS Mini)"));
        mod_val(m, "platform", vstr("boltos"));
        mod_val(m, "maxsize", vint(INT64_MAX));
        mod_val(m, "path", vobj(list_new(1)));
        mod_val(m, "modules", vobj(P->modules));
        mod_fn(m, "exit", b_exit);
        mod_fn(m, "getrecursionlimit", b_reclimit); mod_fn(m, "setrecursionlimit", b_abstractmethod);
        Module *out = mod_new("stdout");
        mod_fn(out, "write", sys_write); mod_fn(out, "flush", sys_flush);
        mod_val(m, "stdout", vobj(out)); mod_val(m, "stderr", vobj(out));
        Module *in = mod_new("stdin");
        mod_fn(in, "readline", sys_readline);
        mod_val(m, "stdin", vobj(in));
        Tuple *vi = tuple_new(3); vi->v[0] = vint(3); vi->v[1] = vint(12); vi->v[2] = vint(0);
        mod_val(m, "version_info", vobj(vi));
        return m;
    }
    if (!strcmp(s, "os") || !strcmp(s, "os.path")) {
        Module *path = mod_new("os.path");
        mod_fn(path, "join", os_join); mod_fn(path, "basename", os_basename); mod_fn(path, "dirname", os_dirname);
        mod_fn(path, "splitext", os_splitext); mod_fn(path, "exists", os_exists); mod_fn(path, "isfile", os_exists);
        if (s[2] == '.') return path;
        Module *m = mod_new("os");
        mod_fn(m, "getcwd", os_getcwd);
        mod_val(m, "path", vobj(path)); mod_val(m, "sep", vstr("/")); mod_val(m, "linesep", vstr("\n")); mod_val(m, "name", vstr("posix"));
        return m;
    }
    if (!strcmp(s, "string")) {
        Module *m = mod_new("string");
        mod_val(m, "ascii_lowercase", vstr("abcdefghijklmnopqrstuvwxyz"));
        mod_val(m, "ascii_uppercase", vstr("ABCDEFGHIJKLMNOPQRSTUVWXYZ"));
        mod_val(m, "ascii_letters", vstr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"));
        mod_val(m, "digits", vstr("0123456789")); mod_val(m, "hexdigits", vstr("0123456789abcdefABCDEF"));
        mod_val(m, "octdigits", vstr("01234567")); mod_val(m, "punctuation", vstr("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"));
        mod_val(m, "whitespace", vstr(" \t\n\r\x0b\x0c"));
        mod_val(m, "printable", vstr("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ \t\n\r\x0b\x0c"));
        return m;
    }
    if (!strcmp(s, "json")) {
        Module *m = mod_new("json");
        mod_fn(m, "dumps", j_dumps); mod_fn(m, "loads", j_loads); mod_fn(m, "dump", j_dump); mod_fn(m, "load", j_load);
        mod_val(m, "JSONDecodeError", vobj(P->e_value));
        return m;
    }
    if (!strcmp(s, "collections")) return py_module("collections", PY_COLLECTIONS);
    if (!strcmp(s, "itertools")) return py_module("itertools", PY_ITERTOOLS);
    if (!strcmp(s, "functools")) return py_module("functools", PY_FUNCTOOLS);
    if (!strcmp(s, "typing")) return py_module("typing", PY_TYPING);
    if (!strcmp(s, "dataclasses")) return py_module("dataclasses", PY_DATACLASSES);
    if (!strcmp(s, "__future__") || !strcmp(s, "abc")) {
        Module *m = mod_new(s);
        if (!strcmp(s, "abc")) { mod_val(m, "ABC", vobj(P->t_object)); mod_fn(m, "abstractmethod", b_abstractmethod); }
        return m;
    }
    return 0;
}

/* ---- the state: types, exceptions, builtins ------------------------------------------------------------------ */
static Class *exc(const char *name, Class *base) {
    Class *c = class_new(name, base, 0);
    c->is_exc = 1;
    dict_set_str(P->builtins, c->name, vobj(c));
    return c;
}

static Class *btype(const char *name, Class *base, int kind, native_fn ctor, const meth_t *m) {
    Class *c = class_new(name, base, kind);
    if (ctor) dict_set_str(c->dict, P->s_ctor, native(name, ctor));
    if (m) methods(c, m);
    dict_set_str(P->builtins, c->name, vobj(c));
    return c;
}


void py_init_state(void) {
    P->interned = dict_new();
    P->builtins = dict_new();
    P->modules = dict_new();
    P->open_files = list_new(4);
#define NM(field, text) P->field = intern_c(text)
    NM(s_init, "__init__"); NM(s_str, "__str__"); NM(s_repr, "__repr__"); NM(s_len, "__len__"); NM(s_iter, "__iter__");
    NM(s_next, "__next__"); NM(s_getitem, "__getitem__"); NM(s_setitem, "__setitem__"); NM(s_delitem, "__delitem__");
    NM(s_contains, "__contains__"); NM(s_call, "__call__"); NM(s_eq, "__eq__"); NM(s_ne, "__ne__"); NM(s_lt, "__lt__");
    NM(s_le, "__le__"); NM(s_gt, "__gt__"); NM(s_ge, "__ge__"); NM(s_hash, "__hash__"); NM(s_bool, "__bool__");
    NM(s_enter, "__enter__"); NM(s_exit, "__exit__"); NM(s_name, "__name__"); NM(s_main, "__main__"); NM(s_args, "args");
    NM(s_missing, "__missing__"); NM(s_new, "__new__"); NM(s_add, "__add__"); NM(s_class, "__class__"); NM(s_dict, "__dict__");
    NM(s_doc, "__doc__"); NM(s_getattr, "__getattr__"); NM(s_setattr, "__setattr__"); NM(s_neg, "__neg__"); NM(s_abs, "__abs__");
    NM(s_int, "__int__"); NM(s_float, "__float__"); NM(s_format, "__format__"); NM(s_module, "__module__"); NM(s_self, "self");
    NM(s_ctor, "__ctor__"); NM(s_tb, "__tb__");
#undef NM
    Class *o = P->t_object = btype("object", 0, 0, c_object, 0);
    P->t_type = btype("type", o, 0, b_type, 0);
    P->t_int = btype("int", o, O_BIG, c_int, INT_M);
    P->t_bool = btype("bool", P->t_int, O_BIG, c_bool, 0);
    P->t_float = btype("float", o, V_FLOAT + 100, c_float, FLOAT_M);
    P->t_str = btype("str", o, O_STR, c_str, STR_M);
    P->t_list = btype("list", o, O_LIST, c_list, LIST_M);
    P->t_tuple = btype("tuple", o, O_TUPLE, c_tuple, TUPLE_M);
    P->t_dict = btype("dict", o, O_DICT, c_dict, DICT_M);
    P->t_set = btype("set", o, O_SET, c_set, SET_M);
    P->t_frozenset = btype("frozenset", o, O_SET, c_frozenset, 0);
    P->t_range = btype("range", o, O_RANGE, b_range, 0);
    P->t_slice = btype("slice", o, O_SLICE, c_slice, 0);
    P->t_none = class_new("NoneType", o, 0);
    P->t_func = class_new("function", o, 0);
    P->t_module = class_new("module", o, 0);
    P->t_file = class_new("TextIOWrapper", o, O_FILE); methods(P->t_file, FILE_M);
    P->t_iter = class_new("iterator", o, 0);
    /* the frozenset type has the set's methods that don't change it */
    methods(P->t_frozenset, SET_M);

    Class *b = P->e_base = exc("BaseException", o);
    P->e_exit = exc("SystemExit", b);
    P->e_kbd = exc("KeyboardInterrupt", b);
    Class *e = P->e_exc = exc("Exception", b);
    P->e_arith = exc("ArithmeticError", e);
    P->e_zero = exc("ZeroDivisionError", P->e_arith);
    P->e_overflow = exc("OverflowError", P->e_arith);
    P->e_assert = exc("AssertionError", e);
    P->e_attr = exc("AttributeError", e);
    P->e_eof = exc("EOFError", e);
    P->e_import = exc("ImportError", e);
    P->e_notfound = exc("ModuleNotFoundError", P->e_import);
    P->e_lookup = exc("LookupError", e);
    P->e_index = exc("IndexError", P->e_lookup);
    P->e_key = exc("KeyError", P->e_lookup);
    P->e_memory = exc("MemoryError", e);
    P->e_name = exc("NameError", e);
    P->e_unbound = exc("UnboundLocalError", P->e_name);
    P->e_os = exc("OSError", e);
    dict_set_str(P->builtins, intern_c("IOError"), vobj(P->e_os));
    Class *fnf = exc("FileNotFoundError", P->e_os);
    exc("PermissionError", P->e_os);
    exc("FileExistsError", P->e_os);
    P->e_runtime = exc("RuntimeError", e);
    P->e_notimpl = exc("NotImplementedError", P->e_runtime);
    P->e_recursion = exc("RecursionError", P->e_runtime);
    P->e_stop = exc("StopIteration", e);
    P->e_syntax = exc("SyntaxError", e);
    P->e_indent = exc("IndentationError", P->e_syntax);
    P->e_type = exc("TypeError", e);
    P->e_value = exc("ValueError", e);
    exc("UnicodeError", P->e_value);
    exc("Warning", e);
    (void)fnf;

    static const struct { const char *name; native_fn fn; } B[] = {
        { "print", b_print }, { "input", b_input }, { "len", b_len }, { "abs", b_abs }, { "min", b_min }, { "max", b_max },
        { "sum", b_sum }, { "sorted", b_sorted }, { "reversed", b_reversed }, { "enumerate", b_enumerate }, { "zip", b_zip },
        { "map", b_map }, { "filter", b_filter }, { "any", b_any }, { "all", b_all }, { "round", b_round },
        { "divmod", b_divmod }, { "pow", b_pow }, { "ord", b_ord }, { "chr", b_chr }, { "hex", b_hex }, { "oct", b_oct },
        { "bin", b_bin }, { "isinstance", b_isinstance }, { "issubclass", b_issubclass }, { "repr", b_repr },
        { "ascii", b_repr }, { "hash", b_hash }, { "id", b_id }, { "iter", b_iter }, { "next", b_next },
        { "callable", b_callable }, { "getattr", b_getattr }, { "setattr", b_setattr }, { "hasattr", b_hasattr },
        { "delattr", b_delattr }, { "format", b_format }, { "super", b_super }, { "staticmethod", b_staticmethod },
        { "classmethod", b_classmethod }, { "property", b_property }, { "exit", b_exit }, { "quit", b_exit },
        { "open", b_open }, { "vars", b_vars }, { "dir", b_dir }, { "globals", b_globals }, { "locals", b_locals }, { 0, 0 } };
    for (int i = 0; B[i].name; i++) dict_set_str(P->builtins, intern_c(B[i].name), native(B[i].name, B[i].fn));
    dict_set_str(P->builtins, intern_c("NotImplemented"), vnone());
    dict_set_str(P->builtins, intern_c("Ellipsis"), vnone());
    dict_set_str(P->builtins, intern_c("__name__"), vobj(P->s_main));
    /* property's .setter, and abc.abstractmethod */
    Class *pc = class_new("property", o, 0);
    dict_set_str(pc->dict, intern_c("setter"), native("setter", prop_setter));
    dict_set_str(P->builtins, intern_c("__propclass__"), vobj(pc));
}

/* p.setter: the property's own method */
V prop_attr(V prop, Str *name, int *found);
V prop_attr(V prop, Str *name, int *found) {
    *found = 0;
    if (!strcmp(name->s, "setter")) {
        Native *b = rt_alloc(sizeof(Native));
        b->type = O_NATIVE; b->name = "setter"; b->fn = prop_setter; b->self = prop; b->has_self = 1;
        *found = 1;
        return vobj(b);
    }
    if (!strcmp(name->s, "getter") || !strcmp(name->s, "fget")) { *found = 1; return ((Prop *)prop.u.o)->get; }
    return vnone();
}

void lib_init(void) { }
