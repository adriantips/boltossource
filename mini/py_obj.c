/* ===========================================================================
 *  BoltOS Mini  -  mini/py_obj.c
 *
 *  Python's objects (py.h): strings, lists, tuples, dicts and sets, integers
 *  of any size, equality, ordering and hashing, repr() and str(), format
 *  specs and the % operator, and making and raising exceptions.
 *
 *  Dicts keep their order as Python's do: the entries in an array in the
 *  order they came, and a separate table of indices into it, probed with
 *  the same perturbation CPython uses. A set is a dict without values.
 *
 *  Big integers are a sign and 32-bit digits; any result that fits 64 bits
 *  goes back to being a plain value, so programs that never leave 64 bits
 *  never meet one. Division is Knuth's algorithm D.
 * ===========================================================================*/
#include <stdint.h>
#include <stdarg.h>
#include "py.h"
#include "string.h"
#include "stdio.h"
#include "math.h"

PyState *P;
Handler *py_handler;
V py_exc;

V vnone(void)          { V v; v.t = V_NONE; v.u.i = 0; return v; }
V vbool(int b)         { V v; v.t = V_BOOL; v.u.i = b != 0; return v; }
V vint(int64_t i)      { V v; v.t = V_INT; v.u.i = i; return v; }
V vfloat(double f)     { V v; v.t = V_FLOAT; v.u.f = f; return v; }
V vobj(void *o)        { V v; v.t = V_OBJ; v.u.o = (Obj *)o; return v; }

/* ---- strings ------------------------------------------------------------------------------- */
Str *str_new(const char *s, uint32_t n) {
    Str *x = rt_alloc_leaf(sizeof(Str) + n + 1);
    x->type = O_STR; x->len = n;
    if (s) memcpy(x->s, s, n);
    x->s[n] = 0;
    return x;
}
Str *str_c(const char *s) { return str_new(s, (uint32_t)strlen(s)); }
V vstr(const char *s) { return vobj(str_c(s)); }
V vstrn(const char *s, uint32_t n) { return vobj(str_new(s, n)); }

uint64_t str_hash(Str *s) {
    if (s->hash) return s->hash;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < s->len; i++) { h ^= (uint8_t)s->s[i]; h *= 16777619u; }
    if (!h) h = 1;
    s->hash = h;
    return h;
}

Str *intern(const char *s, uint32_t n) {
    Str *t = str_new(s, n);
    V got;
    if (dict_get(P->interned, vobj(t), &got)) return as_str(got);
    t->interned = 1;
    dict_set(P->interned, vobj(t), vobj(t));
    return t;
}
Str *intern_c(const char *s) { return intern(s, (uint32_t)strlen(s)); }

/* ---- text builder ------------------------------------------------------------------------------ */
void sb_new(SB *s, uint32_t cap) { s->cap = cap < 16 ? 16 : cap; s->n = 0; s->b = rt_alloc_leaf(s->cap); }
static void sb_room(SB *s, uint32_t more) {
    if (s->n + more + 1 <= s->cap) return;
    uint32_t nc = s->cap * 2;
    while (nc < s->n + more + 1) nc *= 2;
    s->b = rt_realloc(s->b, s->n, nc);
    s->cap = nc;
}
void sb_add(SB *s, const char *p, uint32_t n) { sb_room(s, n); memcpy(s->b + s->n, p, n); s->n += n; }
void sb_addc(SB *s, char c) { sb_room(s, 1); s->b[s->n++] = c; }
void sb_adds(SB *s, const char *p) { sb_add(s, p, (uint32_t)strlen(p)); }
void sb_addstr(SB *s, Str *x) { sb_add(s, x->s, x->len); }
Str *sb_str(SB *s) { return str_new(s->b, s->n); }

/* ---- lists and tuples ----------------------------------------------------------------------------- */
List *list_new(uint32_t cap) {
    List *l = rt_alloc(sizeof(List));
    l->type = O_LIST;
    l->cap = cap < 4 ? 4 : cap;
    l->v = rt_alloc(sizeof(V) * l->cap);
    return l;
}
void list_push(List *l, V x) {
    if (l->len == l->cap) {
        uint32_t nc = l->cap * 2;
        l->v = rt_realloc(l->v, sizeof(V) * l->cap, sizeof(V) * nc);
        l->cap = nc;
    }
    l->v[l->len++] = x;
}
Tuple *tuple_new(uint32_t n) {
    Tuple *t = rt_alloc(sizeof(Tuple) + sizeof(V) * n);
    t->type = O_TUPLE; t->len = n;
    return t;
}

/* ---- dicts and sets ---------------------------------------------------------------------------------- */
Dict *dict_new(void) { Dict *d = rt_alloc(sizeof(Dict)); d->type = O_DICT; return d; }
Dict *set_new(void)  { Dict *d = rt_alloc(sizeof(Dict)); d->type = O_SET; return d; }

static void dict_rebuild(Dict *d, uint32_t want) {
    uint32_t cap = 8;
    while (cap < want) cap *= 2;
    uint32_t icap = cap * 2;
    DEnt *e = rt_alloc(sizeof(DEnt) * cap);
    int32_t *idx = rt_alloc_leaf(sizeof(int32_t) * icap);
    memset(idx, 0xFF, sizeof(int32_t) * icap);
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->used; i++) {
        if (d->e[i].key.t == V_UNBOUND) continue;
        e[n] = d->e[i];
        uint64_t h = e[n].hash, mask = icap - 1, j = h & mask, pert = h;
        while (idx[j] >= 0) { pert >>= 5; j = (j * 5 + 1 + pert) & mask; }
        idx[j] = (int32_t)n;
        n++;
    }
    d->e = e; d->idx = idx; d->cap = cap; d->icap = icap; d->used = n; d->n = n;
}

/* The index slot for key: its entry (>= 0), or -1 with *slot the empty one to use. */
static int64_t dict_find(Dict *d, V key, uint64_t h, uint64_t *slot) {
    if (!d->icap) { *slot = 0; return -1; }
    uint64_t mask = d->icap - 1, j = h & mask, pert = h;
    for (;;) {
        int32_t ix = d->idx[j];
        if (ix < 0) { *slot = j; return -1; }
        DEnt *en = &d->e[ix];
        if (en->hash == h && en->key.t != V_UNBOUND) {
            if (en->key.t == V_OBJ && key.t == V_OBJ && en->key.u.o == key.u.o) return ix;
            if (eq(en->key, key)) return ix;
        }
        pert >>= 5; j = (j * 5 + 1 + pert) & mask;
    }
}

int dict_get(Dict *d, V key, V *out) {
    uint64_t slot, h = hash_of(key);
    int64_t ix = dict_find(d, key, h, &slot);
    if (ix < 0) return 0;
    if (out) *out = d->e[ix].val;
    return 1;
}

void dict_set(Dict *d, V key, V val) {
    uint64_t slot, h = hash_of(key);
    int64_t ix = dict_find(d, key, h, &slot);
    if (ix >= 0) { d->e[ix].val = val; return; }
    if (d->used >= d->cap) {
        dict_rebuild(d, d->n * 2 + 1);
        dict_find(d, key, h, &slot);
    }
    DEnt *en = &d->e[d->used];
    en->key = key; en->val = val; en->hash = h;
    d->idx[slot] = (int32_t)d->used;
    d->used++; d->n++;
}

int dict_del(Dict *d, V key) {
    uint64_t slot, h = hash_of(key);
    int64_t ix = dict_find(d, key, h, &slot);
    if (ix < 0) return 0;
    d->e[ix].key.t = V_UNBOUND; d->e[ix].val = vnone();
    d->n--;
    return 1;
}

int dict_get_str(Dict *d, Str *key, V *out) { return dict_get(d, vobj(key), out); }
void dict_set_str(Dict *d, Str *key, V val) { dict_set(d, vobj(key), val); }

int dict_next(Dict *d, uint32_t *pos, V *key, V *val) {
    while (*pos < d->used) {
        DEnt *en = &d->e[(*pos)++];
        if (en->key.t == V_UNBOUND) continue;
        if (key) *key = en->key;
        if (val) *val = en->val;
        return 1;
    }
    return 0;
}

/* ---- types --------------------------------------------------------------------------------------- */
Class *type_of(V v) {
    switch (v.t) {
    case V_NONE: return P->t_none;
    case V_BOOL: return P->t_bool;
    case V_INT: return P->t_int;
    case V_FLOAT: return P->t_float;
    case V_OBJ: break;
    default: return P->t_object;
    }
    switch (v.u.o->type) {
    case O_STR: return P->t_str;
    case O_LIST: return P->t_list;
    case O_TUPLE: return P->t_tuple;
    case O_DICT: return P->t_dict;
    case O_SET: return ((Dict *)v.u.o)->frozen ? P->t_frozenset : P->t_set;
    case O_BIG: return P->t_int;
    case O_FUNC: case O_NATIVE: case O_BOUND: return P->t_func;
    case O_CLASS: return P->t_type;
    case O_INST: return ((Inst *)v.u.o)->cls;
    case O_MODULE: return P->t_module;
    case O_RANGE: return P->t_range;
    case O_FILE: return P->t_file;
    case O_SLICE: return P->t_slice;
    default: return P->t_iter;
    }
}
const char *type_name(V v) { return type_of(v)->name->s; }

int is_subclass(Class *a, Class *b) {
    for (int i = 0; i < a->nmro; i++) if (a->mro[i] == b) return 1;
    return 0;
}
int is_instance(V v, Class *c) { return is_subclass(type_of(v), c); }

/* C3: the order Python looks for a class's attributes in. */
static int c3(Class *c) {
    Class **seqs[17]; int lens[17], pos[17], ns = 0;
    for (int i = 0; i < c->nbases && ns < 16; i++) { seqs[ns] = c->bases[i]->mro; lens[ns] = c->bases[i]->nmro; pos[ns++] = 0; }
    if (c->nbases > 1) { seqs[ns] = c->bases; lens[ns] = c->nbases; pos[ns++] = 0; }
    Class *out[64]; int n = 0;
    out[n++] = c;
    for (;;) {
        Class *pick = 0;
        for (int i = 0; i < ns && !pick; i++) {
            if (pos[i] >= lens[i]) continue;
            Class *cand = seqs[i][pos[i]];
            int bad = 0;
            for (int j = 0; j < ns && !bad; j++)
                for (int k = pos[j] + 1; k < lens[j]; k++) if (seqs[j][k] == cand) { bad = 1; break; }
            if (!bad) pick = cand;
        }
        if (!pick) {
            for (int i = 0; i < ns; i++) if (pos[i] < lens[i]) return -1;
            break;
        }
        if (n < 64) out[n++] = pick;
        for (int i = 0; i < ns; i++) if (pos[i] < lens[i] && seqs[i][pos[i]] == pick) pos[i]++;
    }
    c->mro = rt_alloc(sizeof(Class *) * (size_t)n);
    memcpy(c->mro, out, sizeof(Class *) * (size_t)n);
    c->nmro = n;
    return 0;
}

int class_set_bases(Class *c, Class **bases, int n) {
    c->bases = rt_alloc(sizeof(Class *) * (size_t)(n ? n : 1));
    memcpy(c->bases, bases, sizeof(Class *) * (size_t)n);
    c->nbases = n;
    for (int i = 0; i < n; i++) { if (bases[i]->is_exc) c->is_exc = 1; if (bases[i]->builtin && !c->builtin) c->builtin = bases[i]->builtin; }
    return c3(c);
}

Class *class_new(const char *name, Class *base, int builtin) {
    Class *c = rt_alloc(sizeof(Class));
    c->type = O_CLASS;
    c->name = intern_c(name);
    c->dict = dict_new();
    c->builtin = builtin;
    if (base) class_set_bases(c, &base, 1);
    else { c->mro = rt_alloc(sizeof(Class *)); c->mro[0] = c; c->nmro = 1; }
    return c;
}

/* ---- exceptions ------------------------------------------------------------------------------------ */
V make_exc(Class *c, V msg) {
    Inst *e = rt_alloc(sizeof(Inst));
    e->type = O_INST; e->cls = c; e->dict = dict_new();
    Tuple *t = tuple_new(msg.t == V_UNBOUND ? 0 : 1);
    if (t->len) t->v[0] = msg;
    e->native = vobj(t);
    return vobj(e);
}

void raise_new(Class *c, const char *fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    raise(make_exc(c, vstr(b)));
}

Str  *traceback_now(void);
void  py_uncaught(V exc) __attribute__((noreturn));

void raise(V exc) {
    if (exc.t == V_OBJ && exc.u.o->type == O_CLASS) exc = call0(exc);
    if (!is_obj(exc, O_INST) || !((Inst *)exc.u.o)->cls->is_exc) exc = make_exc(P->e_type, vstr("exceptions must derive from BaseException"));
    Inst *e = (Inst *)exc.u.o;
    V tb;
    if (!dict_get_str(e->dict, P->s_tb, &tb)) dict_set_str(e->dict, P->s_tb, vobj(traceback_now()));
    py_exc = exc;
    if (!py_handler) py_uncaught(exc);
    rt_longjmp(&py_handler->jb, 1);
}

/* ---- big integers -------------------------------------------------------------------------------- */
static Big *big_alloc(uint32_t n) { Big *b = rt_alloc_leaf(sizeof(Big) + 4 * (n ? n : 1)); b->type = O_BIG; b->n = n; return b; }

int is_int(V v) { return v.t == V_INT || v.t == V_BOOL || is_obj(v, O_BIG); }

V big_norm(Big *b) {
    while (b->n && !b->d[b->n - 1]) b->n--;
    if (!b->n) return vint(0);
    if (b->n <= 2) {
        uint64_t m = b->d[0] | (b->n > 1 ? (uint64_t)b->d[1] << 32 : 0);
        if (b->sign > 0 && m <= (uint64_t)INT64_MAX) return vint((int64_t)m);
        if (b->sign < 0 && m <= (uint64_t)INT64_MAX + 1) return vint((int64_t)(0 - m));
    }
    return vobj(b);
}

static Big *to_big(V v) {
    if (is_obj(v, O_BIG)) return (Big *)v.u.o;
    int64_t i = v.u.i;
    Big *b = big_alloc(2);
    uint64_t m = i < 0 ? 0 - (uint64_t)i : (uint64_t)i;
    b->d[0] = (uint32_t)m; b->d[1] = (uint32_t)(m >> 32);
    b->n = 2; b->sign = i < 0 ? -1 : i > 0 ? 1 : 0;
    while (b->n && !b->d[b->n - 1]) b->n--;
    return b;
}
V big_from_i64(int64_t i) { return vobj(to_big(vint(i))); }

static int mag_cmp(const Big *a, const Big *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = (int)a->n - 1; i >= 0; i--) if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}
static Big *mag_add(const Big *a, const Big *b) {
    uint32_t n = (a->n > b->n ? a->n : b->n) + 1;
    Big *r = big_alloc(n);
    uint64_t c = 0;
    for (uint32_t i = 0; i < n; i++) {
        c += (uint64_t)(i < a->n ? a->d[i] : 0) + (i < b->n ? b->d[i] : 0);
        r->d[i] = (uint32_t)c; c >>= 32;
    }
    r->sign = 1;
    return r;
}
static Big *mag_sub(const Big *a, const Big *b) {        /* |a| >= |b| */
    Big *r = big_alloc(a->n);
    int64_t c = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        c += (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0);
        r->d[i] = (uint32_t)c; c >>= 32;
    }
    r->sign = 1;
    return r;
}
static V signed_add(Big *a, Big *b, int bsign) {
    if (!a->sign) { Big *r = big_alloc(b->n); memcpy(r->d, b->d, 4 * b->n); r->sign = (int8_t)bsign; return big_norm(r); }
    if (!bsign) { Big *r = big_alloc(a->n); memcpy(r->d, a->d, 4 * a->n); r->sign = a->sign; return big_norm(r); }
    Big *r;
    if (a->sign == bsign) { r = mag_add(a, b); r->sign = a->sign; }
    else if (mag_cmp(a, b) >= 0) { r = mag_sub(a, b); r->sign = a->sign; }
    else { r = mag_sub(b, a); r->sign = (int8_t)bsign; }
    return big_norm(r);
}

V int_add(V a, V b) {
    int64_t r;
    if (a.t != V_OBJ && b.t != V_OBJ && !__builtin_add_overflow(a.u.i, b.u.i, &r)) return vint(r);
    Big *x = to_big(a), *y = to_big(b);
    return signed_add(x, y, y->sign);
}
V int_sub(V a, V b) {
    int64_t r;
    if (a.t != V_OBJ && b.t != V_OBJ && !__builtin_sub_overflow(a.u.i, b.u.i, &r)) return vint(r);
    Big *x = to_big(a), *y = to_big(b);
    return signed_add(x, y, -y->sign);
}
V int_mul(V a, V b) {
    int64_t r;
    if (a.t != V_OBJ && b.t != V_OBJ && !__builtin_mul_overflow(a.u.i, b.u.i, &r)) return vint(r);
    Big *x = to_big(a), *y = to_big(b);
    if (!x->sign || !y->sign) return vint(0);
    Big *p = big_alloc(x->n + y->n);
    memset(p->d, 0, 4 * (x->n + y->n));
    for (uint32_t i = 0; i < x->n; i++) {
        uint64_t c = 0;
        if ((i & 63) == 63) rt_check();
        for (uint32_t j = 0; j < y->n; j++) {
            c += (uint64_t)x->d[i] * y->d[j] + p->d[i + j];
            p->d[i + j] = (uint32_t)c; c >>= 32;
        }
        p->d[i + y->n] = (uint32_t)c;
    }
    p->sign = (int8_t)(x->sign * y->sign);
    return big_norm(p);
}
V int_neg(V a) {
    if (a.t != V_OBJ && a.u.i != INT64_MIN) return vint(-a.u.i);
    Big *x = to_big(a), *r = big_alloc(x->n);
    memcpy(r->d, x->d, 4 * x->n); r->sign = (int8_t)-x->sign;
    return big_norm(r);
}

/* |u| / |v| -> q, r (Knuth D, as in Hacker's Delight). */
static void mag_divmod(const Big *u, const Big *v, Big **qo, Big **ro) {
    uint32_t m = u->n, n = v->n;
    if (mag_cmp(u, v) < 0) {
        Big *q = big_alloc(1); q->n = 0; q->sign = 0;
        Big *r = big_alloc(u->n); memcpy(r->d, u->d, 4 * u->n); r->sign = 1;
        *qo = q; *ro = r; return;
    }
    Big *q = big_alloc(m - n + 1), *r = big_alloc(n);
    q->sign = r->sign = 1;
    if (n == 1) {
        uint64_t k = 0;
        for (int j = (int)m - 1; j >= 0; j--) {
            uint64_t cur = k << 32 | u->d[j];
            q->d[j] = (uint32_t)(cur / v->d[0]); k = cur % v->d[0];
        }
        q->n = m; r->d[0] = (uint32_t)k; r->n = 1;
        *qo = q; *ro = r; return;
    }
    int s = __builtin_clz(v->d[n - 1]);
    uint32_t *vn = rt_alloc_leaf(4 * n), *un = rt_alloc_leaf(4 * (m + 1));
    for (int i = (int)n - 1; i > 0; i--) vn[i] = (v->d[i] << s) | (s ? (uint32_t)((uint64_t)v->d[i - 1] >> (32 - s)) : 0);
    vn[0] = v->d[0] << s;
    un[m] = s ? (uint32_t)((uint64_t)u->d[m - 1] >> (32 - s)) : 0;
    for (int i = (int)m - 1; i > 0; i--) un[i] = (u->d[i] << s) | (s ? (uint32_t)((uint64_t)u->d[i - 1] >> (32 - s)) : 0);
    un[0] = u->d[0] << s;
    for (int j = (int)(m - n); j >= 0; j--) {
        uint64_t num = (uint64_t)un[j + n] << 32 | un[j + n - 1];
        uint64_t qhat = num / vn[n - 1], rhat = num % vn[n - 1];
        while (qhat >= (1ull << 32) || qhat * vn[n - 2] > (rhat << 32 | un[j + n - 2])) {
            qhat--; rhat += vn[n - 1];
            if (rhat >= (1ull << 32)) break;
        }
        int64_t borrow = 0, t;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t p = qhat * vn[i];
            t = (int64_t)un[i + j] - borrow - (int64_t)(p & 0xFFFFFFFFu);
            un[i + j] = (uint32_t)t;
            borrow = (int64_t)(p >> 32) - (t >> 32);
        }
        t = (int64_t)un[j + n] - borrow;
        un[j + n] = (uint32_t)t;
        q->d[j] = (uint32_t)qhat;
        if (t < 0) {
            q->d[j]--;
            uint64_t c = 0;
            for (uint32_t i = 0; i < n; i++) {
                c += (uint64_t)un[i + j] + vn[i];
                un[i + j] = (uint32_t)c; c >>= 32;
            }
            un[j + n] += (uint32_t)c;
        }
    }
    for (uint32_t i = 0; i < n; i++) r->d[i] = (un[i] >> s) | (s ? (uint32_t)((uint64_t)un[i + 1] << (32 - s)) : 0);
    q->n = m - n + 1; r->n = n;
    *qo = q; *ro = r;
}

/* floor division and modulo, Python's way (the remainder takes the divisor's sign) */
static void int_divmod(V a, V b, V *q, V *r) {
    if (a.t != V_OBJ && b.t != V_OBJ) {
        int64_t x = a.u.i, y = b.u.i;
        if (!y) raise_new(P->e_zero, "integer division or modulo by zero");
        if (!(x == INT64_MIN && y == -1)) {
            int64_t qq = x / y, rr = x % y;
            if (rr && ((rr < 0) != (y < 0))) { qq--; rr += y; }
            if (q) *q = vint(qq);
            if (r) *r = vint(rr);
            return;
        }
    }
    Big *x = to_big(a), *y = to_big(b);
    if (!y->sign) raise_new(P->e_zero, "integer division or modulo by zero");
    Big *qm, *rm;
    mag_divmod(x, y, &qm, &rm);
    qm->sign = (int8_t)(x->sign * y->sign); rm->sign = x->sign;
    V qv = big_norm(qm), rv = big_norm(rm);
    int rnz = !(rv.t == V_INT && rv.u.i == 0);
    if (rnz && x->sign != y->sign) { qv = int_sub(qv, vint(1)); rv = int_add(rv, b); }
    if (q) *q = qv;
    if (r) *r = rv;
}
V int_floordiv(V a, V b) { V q; int_divmod(a, b, &q, 0); return q; }
V int_mod(V a, V b) { V r; int_divmod(a, b, 0, &r); return r; }

int int_cmp(V a, V b) {
    if (a.t != V_OBJ && b.t != V_OBJ) return a.u.i < b.u.i ? -1 : a.u.i > b.u.i;
    Big *x = to_big(a), *y = to_big(b);
    if (x->sign != y->sign) return x->sign < y->sign ? -1 : 1;
    int c = mag_cmp(x, y);
    return x->sign < 0 ? -c : c;
}

V int_pow(V a, V b) {
    int64_t e;
    if (!fits_i64(b, &e)) raise_new(P->e_overflow, "exponent too large");
    V r = vint(1), base = a;
    while (e > 0) {
        rt_check();
        if (e & 1) r = int_mul(r, base);
        e >>= 1;
        if (e) base = int_mul(base, base);
    }
    return r;
}

V int_shl(V a, int64_t k) {
    if (k < 0) raise_new(P->e_value, "negative shift count");
    if (a.t != V_OBJ && k < 63) {
        int64_t x = a.u.i;
        if (x == 0) return vint(0);
        if ((x > 0 ? x : -x) < (INT64_C(1) << (62 - k)) && x != INT64_MIN) return vint(x * (INT64_C(1) << k));
    }
    if (k > 1000000) raise_new(P->e_overflow, "shift too large");
    Big *x = to_big(a);
    uint32_t w = (uint32_t)(k / 32), bits = (uint32_t)(k % 32);
    Big *r = big_alloc(x->n + w + 1);
    memset(r->d, 0, 4 * r->n);
    for (uint32_t i = 0; i < x->n; i++) {
        uint64_t v = (uint64_t)x->d[i] << bits;
        r->d[i + w] |= (uint32_t)v;
        r->d[i + w + 1] |= (uint32_t)(v >> 32);
    }
    r->sign = x->sign;
    return big_norm(r);
}
V int_shr(V a, int64_t k) {
    if (k < 0) raise_new(P->e_value, "negative shift count");
    if (a.t != V_OBJ) return vint(k >= 64 ? (a.u.i < 0 ? -1 : 0) : a.u.i >> k);
    Big *x = to_big(a);
    if (x->sign < 0) return int_sub(vint(-1), int_shr(int_sub(vint(-1), a), k));   /* floor, as Python's */
    uint32_t w = (uint32_t)(k / 32), bits = (uint32_t)(k % 32);
    if (w >= x->n) return vint(0);
    Big *r = big_alloc(x->n - w);
    for (uint32_t i = 0; i < r->n; i++) {
        uint64_t v = x->d[i + w] | (i + w + 1 < x->n ? (uint64_t)x->d[i + w + 1] << 32 : 0);
        r->d[i] = (uint32_t)(v >> bits);
    }
    r->sign = 1;
    return big_norm(r);
}

/* & | ^ on two's complement, wide enough for both */
V int_bitop(V a, V b, int op) {
    if (a.t != V_OBJ && b.t != V_OBJ) {
        int64_t x = a.u.i, y = b.u.i;
        return vint(op == B_AND ? x & y : op == B_OR ? x | y : x ^ y);
    }
    Big *x = to_big(a), *y = to_big(b);
    uint32_t n = (x->n > y->n ? x->n : y->n) + 1;
    uint32_t *p = rt_alloc_leaf(4 * n), *q = rt_alloc_leaf(4 * n), *r = rt_alloc_leaf(4 * n);
    for (int which = 0; which < 2; which++) {
        Big *s = which ? y : x; uint32_t *o = which ? q : p;
        for (uint32_t i = 0; i < n; i++) o[i] = i < s->n ? s->d[i] : 0;
        if (s->sign < 0) { uint64_t c = 1; for (uint32_t i = 0; i < n; i++) { c += (uint32_t)~o[i]; o[i] = (uint32_t)c; c >>= 32; } }
    }
    for (uint32_t i = 0; i < n; i++) r[i] = op == B_AND ? p[i] & q[i] : op == B_OR ? p[i] | q[i] : p[i] ^ q[i];
    int neg = (r[n - 1] >> 31) & 1;
    if (neg) { uint64_t c = 1; for (uint32_t i = 0; i < n; i++) { c += (uint32_t)~r[i]; r[i] = (uint32_t)c; c >>= 32; } }
    Big *o = big_alloc(n);
    memcpy(o->d, r, 4 * n); o->sign = neg ? -1 : 1;
    return big_norm(o);
}

double int_to_double(V a) {
    if (a.t != V_OBJ) return (double)a.u.i;
    Big *x = to_big(a);
    double d = 0;
    for (int i = (int)x->n - 1; i >= 0; i--) d = d * 4294967296.0 + x->d[i];
    return x->sign < 0 ? -d : d;
}

V int_from_double(double d) {
    if (d != d) raise_new(P->e_value, "cannot convert float NaN to integer");
    if (d == __builtin_inf() || d == -__builtin_inf()) raise_new(P->e_overflow, "cannot convert float infinity to integer");
    d = d < 0 ? __builtin_ceil(d) : __builtin_floor(d);
    if (d > -9.2e18 && d < 9.2e18) return vint((int64_t)d);
    int neg = d < 0; if (neg) d = -d;
    int e; double m = frexp(d, &e);                         /* d = m * 2^e, m in [0.5, 1) */
    uint64_t mant = (uint64_t)ldexp(m, 53);
    V r = int_shl(vint((int64_t)mant), e - 53);
    return neg ? int_neg(r) : r;
}

int fits_i64(V v, int64_t *out) {
    if (v.t == V_INT || v.t == V_BOOL) { *out = v.u.i; return 1; }
    return 0;
}

V int_parse(const char *s, int base, int *ok) {
    *ok = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    int neg = 0;
    if (*s == '+' || *s == '-') neg = *s++ == '-';
    if (base == 0) {
        base = 10;
        if (s[0] == '0' && (s[1] | 32) == 'x') { base = 16; s += 2; }
        else if (s[0] == '0' && (s[1] | 32) == 'o') { base = 8; s += 2; }
        else if (s[0] == '0' && (s[1] | 32) == 'b') { base = 2; s += 2; }
    } else if (base == 16 && s[0] == '0' && (s[1] | 32) == 'x') s += 2;
    else if (base == 8 && s[0] == '0' && (s[1] | 32) == 'o') s += 2;
    else if (base == 2 && s[0] == '0' && (s[1] | 32) == 'b') s += 2;
    V r = vint(0);
    int any = 0;
    for (; *s; s++) {
        if (*s == '_' && any) continue;
        int c = *s, dg = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'z' ? (c | 32) - 'a' + 10 : 99;
        if (dg >= base) break;
        r = int_add(int_mul(r, vint(base)), vint(dg));
        any = 1;
    }
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (!any || *s) return vint(0);
    *ok = 1;
    return neg ? int_neg(r) : r;
}

Str *int_to_str(V a, int base) {
    char tmp[80];
    if (a.t != V_OBJ) {
        int64_t x = a.u.i;
        uint64_t m = x < 0 ? 0 - (uint64_t)x : (uint64_t)x;
        int n = 0;
        do { int dg = (int)(m % (uint64_t)base); tmp[n++] = (char)(dg < 10 ? '0' + dg : 'a' + dg - 10); m /= (uint64_t)base; } while (m);
        if (x < 0) tmp[n++] = '-';
        Str *s = str_new(0, (uint32_t)n);
        for (int i = 0; i < n; i++) s->s[i] = tmp[n - 1 - i];
        return s;
    }
    Big *x = to_big(a);
    uint32_t *w = rt_alloc_leaf(4 * x->n);
    memcpy(w, x->d, 4 * x->n);
    uint32_t n = x->n;
    SB sb; sb_new(&sb, x->n * 10 + 4);
    uint32_t chunk = 1, per = 0;
    while ((uint64_t)chunk * (uint32_t)base < 1000000000u) { chunk *= (uint32_t)base; per++; }
    while (n) {
        uint64_t k = 0;
        for (int j = (int)n - 1; j >= 0; j--) { uint64_t cur = k << 32 | w[j]; w[j] = (uint32_t)(cur / chunk); k = cur % chunk; }
        while (n && !w[n - 1]) n--;
        for (uint32_t i = 0; i < per; i++) {
            int dg = (int)(k % (uint32_t)base); k /= (uint32_t)base;
            sb_addc(&sb, (char)(dg < 10 ? '0' + dg : 'a' + dg - 10));
            if (!n && !k) break;
        }
    }
    if (x->sign < 0) sb_addc(&sb, '-');
    for (uint32_t i = 0; i < sb.n / 2; i++) { char t = sb.b[i]; sb.b[i] = sb.b[sb.n - 1 - i]; sb.b[sb.n - 1 - i] = t; }
    return sb_str(&sb);
}

uint64_t big_hash(Big *b) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < b->n; i++) { h ^= b->d[i]; h *= 1099511628211ull; }
    return b->sign < 0 ? ~h : h;
}

/* ---- equality, order, hashing --------------------------------------------------------------------------- */
static int is_num(V v) { return v.t == V_INT || v.t == V_BOOL || v.t == V_FLOAT || is_obj(v, O_BIG); }

static int num_cmp(V a, V b) {                     /* -1 0 1, or 2 when unordered (NaN) */
    if (a.t == V_FLOAT || b.t == V_FLOAT) {
        if (a.t == V_FLOAT && b.t == V_FLOAT) return a.u.f < b.u.f ? -1 : a.u.f > b.u.f ? 1 : a.u.f == b.u.f ? 0 : 2;
        double x = a.t == V_FLOAT ? a.u.f : int_to_double(a), y = b.t == V_FLOAT ? b.u.f : int_to_double(b);
        if (x != x || y != y) return 2;
        if (x < y) return -1;
        if (x > y) return 1;
        /* equal as doubles: an int that is exactly this float? */
        V other = a.t == V_FLOAT ? b : a; double f = a.t == V_FLOAT ? a.u.f : b.u.f;
        if (other.t == V_OBJ) { V fi = int_from_double(f); int c = int_cmp(a.t == V_FLOAT ? fi : a, a.t == V_FLOAT ? b : fi); return c; }
        return 0;
    }
    return int_cmp(a, b);
}

int eq(V a, V b) {
    if (is_num(a) && is_num(b)) return num_cmp(a, b) == 0;
    if (a.t != V_OBJ || b.t != V_OBJ) return a.t == b.t && (a.t == V_NONE || a.u.i == b.u.i);
    if (a.u.o == b.u.o && a.u.o->type != O_INST) return 1;
    int ta = a.u.o->type, tb = b.u.o->type;
    if (ta == O_INST || tb == O_INST) {
        V m;
        if (ta == O_INST && lookup_special(a, P->s_eq, &m)) return truthy(call1(m, b));
        if (tb == O_INST && lookup_special(b, P->s_eq, &m)) return truthy(call1(m, a));
        return a.u.o == b.u.o;
    }
    if (ta != tb) return 0;
    switch (ta) {
    case O_STR: { Str *x = as_str(a), *y = as_str(b); return x->len == y->len && !memcmp(x->s, y->s, x->len); }
    case O_LIST: case O_TUPLE: {
        uint32_t n1 = ta == O_LIST ? as_list(a)->len : ((Tuple *)a.u.o)->len, n2 = ta == O_LIST ? as_list(b)->len : ((Tuple *)b.u.o)->len;
        V *p = ta == O_LIST ? as_list(a)->v : ((Tuple *)a.u.o)->v, *q = ta == O_LIST ? as_list(b)->v : ((Tuple *)b.u.o)->v;
        if (n1 != n2) return 0;
        for (uint32_t i = 0; i < n1; i++) if (!eq(p[i], q[i])) return 0;
        return 1;
    }
    case O_DICT: {
        Dict *x = as_dict(a), *y = as_dict(b);
        if (x->n != y->n) return 0;
        uint32_t pos = 0; V k, v, w;
        while (dict_next(x, &pos, &k, &v)) if (!dict_get(y, k, &w) || !eq(v, w)) return 0;
        return 1;
    }
    case O_SET: {
        Dict *x = as_dict(a), *y = as_dict(b);
        if (x->n != y->n) return 0;
        uint32_t pos = 0; V k;
        while (dict_next(x, &pos, &k, 0)) if (!dict_get(y, k, 0)) return 0;
        return 1;
    }
    case O_RANGE: { Range *x = (Range *)a.u.o, *y = (Range *)b.u.o; return x->start == y->start && x->stop == y->stop && x->step == y->step; }
    case O_BOUND: { Bound *x = (Bound *)a.u.o, *y = (Bound *)b.u.o; return eq(x->self, y->self) && eq(x->fn, y->fn); }
    }
    return 0;
}

static int seq_cmp(V *p, uint32_t n1, V *q, uint32_t n2) {
    for (uint32_t i = 0; i < n1 && i < n2; i++) {
        if (eq(p[i], q[i])) continue;
        return compare(p[i], q[i], C_LT) ? -1 : 1;
    }
    return n1 < n2 ? -1 : n1 > n2;
}

static const char *const cmp_sym[] = { "<", "<=", ">", ">=", "==", "!=" };

int compare(V a, V b, int op) {
    if (op == C_EQ) return eq(a, b);
    if (op == C_NE) return !eq(a, b);
    int c;
    if (is_num(a) && is_num(b)) { c = num_cmp(a, b); if (c == 2) return 0; }
    else if (is_obj(a, O_STR) && is_obj(b, O_STR)) {
        Str *x = as_str(a), *y = as_str(b);
        uint32_t n = x->len < y->len ? x->len : y->len;
        int m = memcmp(x->s, y->s, n);
        c = m ? (m < 0 ? -1 : 1) : x->len < y->len ? -1 : x->len > y->len;
    } else if (is_obj(a, O_LIST) && is_obj(b, O_LIST)) c = seq_cmp(as_list(a)->v, as_list(a)->len, as_list(b)->v, as_list(b)->len);
    else if (is_obj(a, O_TUPLE) && is_obj(b, O_TUPLE)) c = seq_cmp(((Tuple *)a.u.o)->v, ((Tuple *)a.u.o)->len, ((Tuple *)b.u.o)->v, ((Tuple *)b.u.o)->len);
    else if (is_obj(a, O_SET) && is_obj(b, O_SET)) {
        Dict *x = as_dict(a), *y = as_dict(b);
        int sub = 1, sup = 1; uint32_t pos = 0; V k;
        while (dict_next(x, &pos, &k, 0)) if (!dict_get(y, k, 0)) { sub = 0; break; }
        pos = 0;
        while (dict_next(y, &pos, &k, 0)) if (!dict_get(x, k, 0)) { sup = 0; break; }
        switch (op) {
        case C_LT: return sub && x->n < y->n;
        case C_LE: return sub;
        case C_GT: return sup && x->n > y->n;
        default: return sup;
        }
    } else {
        Str *nm[4] = { P->s_lt, P->s_le, P->s_gt, P->s_ge };
        static const int swap[4] = { C_GT, C_GE, C_LT, C_LE };
        V m;
        if (is_obj(a, O_INST) && lookup_special(a, nm[op], &m)) return truthy(call1(m, b));
        if (is_obj(b, O_INST) && lookup_special(b, nm[swap[op]], &m)) return truthy(call1(m, a));
        raise_new(P->e_type, "'%s' not supported between instances of '%s' and '%s'", cmp_sym[op], type_name(a), type_name(b));
    }
    switch (op) {
    case C_LT: return c < 0;
    case C_LE: return c <= 0;
    case C_GT: return c > 0;
    default: return c >= 0;
    }
}

uint64_t hash_of(V v) {
    switch (v.t) {
    case V_NONE: return 0x9E3779B97F4A7C15ull;
    case V_BOOL: case V_INT: { uint64_t x = (uint64_t)v.u.i; x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; return x; }
    case V_FLOAT: {
        double f = v.u.f;
        if (f == __builtin_floor(f) && f > -9.2e18 && f < 9.2e18) return hash_of(vint((int64_t)f));
        uint64_t u; memcpy(&u, &f, 8); u ^= u >> 29; u *= 0xbf58476d1ce4e5b9ull; return u ^ (u >> 32);
    }
    case V_OBJ: break;
    default: return 0;
    }
    switch (v.u.o->type) {
    case O_STR: return str_hash(as_str(v));
    case O_BIG: return big_hash((Big *)v.u.o);
    case O_TUPLE: {
        Tuple *t = (Tuple *)v.u.o;
        if (t->hash) return t->hash;
        uint64_t h = 0x345678;
        for (uint32_t i = 0; i < t->len; i++) h = (h ^ hash_of(t->v[i])) * 1000003ull;
        t->hash = h ? h : 1;
        return t->hash;
    }
    case O_SET:
        if (((Dict *)v.u.o)->frozen) {
            uint64_t h = 0; uint32_t pos = 0; V k;
            while (dict_next(as_dict(v), &pos, &k, 0)) h ^= hash_of(k) * 0x9E3779B97F4A7C15ull;
            return h;
        }
        /* fall through */
    case O_LIST: case O_DICT:
        raise_new(P->e_type, "unhashable type: '%s'", type_name(v));
    case O_INST: {
        V m;
        if (lookup_special(v, P->s_hash, &m)) { V r = call0(m); return hash_of(r); }
        if (lookup_special(v, P->s_eq, &m) && !((Inst *)v.u.o)->cls->is_exc) raise_new(P->e_type, "unhashable type: '%s'", type_name(v));
        break;
    }
    case O_BOUND: return hash_of(((Bound *)v.u.o)->self) ^ hash_of(((Bound *)v.u.o)->fn);
    }
    uint64_t x = (uint64_t)(uintptr_t)v.u.o;
    return (x >> 4) * 0x9E3779B97F4A7C15ull;
}

int truthy(V v) {
    switch (v.t) {
    case V_NONE: return 0;
    case V_BOOL: case V_INT: return v.u.i != 0;
    case V_FLOAT: return v.u.f != 0;
    case V_OBJ: break;
    default: return 0;
    }
    switch (v.u.o->type) {
    case O_STR: return as_str(v)->len != 0;
    case O_LIST: return as_list(v)->len != 0;
    case O_TUPLE: return ((Tuple *)v.u.o)->len != 0;
    case O_DICT: case O_SET: return as_dict(v)->n != 0;
    case O_BIG: return 1;
    case O_RANGE: { Range *r = (Range *)v.u.o; return r->step > 0 ? r->start < r->stop : r->start > r->stop; }
    case O_INST: {
        V m;
        if (lookup_special(v, P->s_bool, &m)) return truthy(call0(m));
        if (lookup_special(v, P->s_len, &m)) return truthy(call0(m));
        return 1;
    }
    }
    return 1;
}

/* ---- repr and str ---------------------------------------------------------------------------------- */
static int repr_depth;

static void str_repr_into(SB *s, Str *x) {
    int sq = 0, dq = 0;
    for (uint32_t i = 0; i < x->len; i++) { if (x->s[i] == '\'') sq = 1; if (x->s[i] == '"') dq = 1; }
    char q = sq && !dq ? '"' : '\'';
    sb_addc(s, q);
    for (uint32_t i = 0; i < x->len; i++) {
        unsigned char c = (unsigned char)x->s[i];
        if (c == (unsigned char)q || c == '\\') { sb_addc(s, '\\'); sb_addc(s, (char)c); }
        else if (c == '\n') sb_adds(s, "\\n");
        else if (c == '\t') sb_adds(s, "\\t");
        else if (c == '\r') sb_adds(s, "\\r");
        else if (c < 32 || c == 127) { char b[8]; snprintf(b, sizeof b, "\\x%02x", c); sb_adds(s, b); }
        else sb_addc(s, (char)c);
    }
    sb_addc(s, q);
}

void repr_into(SB *s, V v, int repr) {
    char b[64];
    switch (v.t) {
    case V_NONE: sb_adds(s, "None"); return;
    case V_BOOL: sb_adds(s, v.u.i ? "True" : "False"); return;
    case V_INT: snprintf(b, sizeof b, "%lld", (long long)v.u.i); sb_adds(s, b); return;
    case V_FLOAT: num_repr(v.u.f, b, sizeof b); sb_adds(s, b); return;
    case V_OBJ: break;
    default: sb_adds(s, "<unbound>"); return;
    }
    if (repr_depth > 40) { sb_adds(s, "..."); return; }
    Obj *o = v.u.o;
    switch (o->type) {
    case O_STR: if (repr) str_repr_into(s, (Str *)o); else sb_addstr(s, (Str *)o); return;
    case O_BIG: sb_addstr(s, int_to_str(v, 10)); return;
    case O_LIST: case O_TUPLE: {
        int lst = o->type == O_LIST;
        uint32_t n = lst ? ((List *)o)->len : ((Tuple *)o)->len;
        V *p = lst ? ((List *)o)->v : ((Tuple *)o)->v;
        sb_addc(s, lst ? '[' : '(');
        repr_depth++;
        for (uint32_t i = 0; i < n; i++) { if (i) sb_adds(s, ", "); repr_into(s, p[i], 1); }
        repr_depth--;
        if (!lst && n == 1) sb_addc(s, ',');
        sb_addc(s, lst ? ']' : ')');
        return;
    }
    case O_DICT: case O_SET: {
        Dict *d = (Dict *)o;
        if (o->type == O_SET && !d->n) { sb_adds(s, d->frozen ? "frozenset()" : "set()"); return; }
        if (o->type == O_SET && d->frozen) sb_adds(s, "frozenset(");
        sb_addc(s, '{');
        uint32_t pos = 0; V k, val; int first = 1;
        repr_depth++;
        while (dict_next(d, &pos, &k, &val)) {
            if (!first) sb_adds(s, ", ");
            first = 0;
            repr_into(s, k, 1);
            if (o->type == O_DICT) { sb_adds(s, ": "); repr_into(s, val, 1); }
        }
        repr_depth--;
        sb_addc(s, '}');
        if (o->type == O_SET && d->frozen) sb_addc(s, ')');
        return;
    }
    case O_FUNC: snprintf(b, sizeof b, "<function %s>", ((Func *)o)->name->s); sb_adds(s, b); return;
    case O_NATIVE: snprintf(b, sizeof b, "<built-in function %s>", ((Native *)o)->name); sb_adds(s, b); return;
    case O_BOUND: sb_adds(s, "<bound method>"); return;
    case O_CLASS: sb_adds(s, "<class '"); sb_addstr(s, ((Class *)o)->name); sb_adds(s, "'>"); return;
    case O_MODULE: sb_adds(s, "<module '"); sb_addstr(s, ((Module *)o)->name); sb_adds(s, "'>"); return;
    case O_RANGE: {
        Range *r = (Range *)o;
        if (r->step == 1) snprintf(b, sizeof b, "range(%lld, %lld)", (long long)r->start, (long long)r->stop);
        else snprintf(b, sizeof b, "range(%lld, %lld, %lld)", (long long)r->start, (long long)r->stop, (long long)r->step);
        sb_adds(s, b); return;
    }
    case O_SLICE: {
        Slice *sl = (Slice *)o;
        sb_adds(s, "slice("); repr_into(s, sl->lo, 1); sb_adds(s, ", "); repr_into(s, sl->hi, 1); sb_adds(s, ", "); repr_into(s, sl->step, 1); sb_addc(s, ')');
        return;
    }
    case O_FILE: sb_adds(s, "<file '"); sb_addstr(s, ((File *)o)->path); sb_adds(s, "'>"); return;
    case O_INST: {
        Inst *in = (Inst *)o;
        V m;
        if ((!repr && lookup_special(v, P->s_str, &m)) || lookup_special(v, P->s_repr, &m)) {
            repr_depth++;
            V r = call0(m);
            repr_depth--;
            if (!is_obj(r, O_STR)) raise_new(P->e_type, "__str__ returned non-string (type %s)", type_name(r));
            sb_addstr(s, as_str(r));
            return;
        }
        if (in->cls->is_exc) {
            Tuple *t = is_obj(in->native, O_TUPLE) ? (Tuple *)in->native.u.o : 0;
            if (repr) {
                sb_addstr(s, in->cls->name); sb_addc(s, '(');
                for (uint32_t i = 0; t && i < t->len; i++) { if (i) sb_adds(s, ", "); repr_into(s, t->v[i], 1); }
                sb_addc(s, ')');
            } else if (t && t->len == 1) repr_into(s, t->v[0], is_subclass(in->cls, P->e_key));
            else if (t && t->len > 1) repr_into(s, vobj(t), 1);
            return;
        }
        if (in->cls->builtin && in->native.t != V_NONE) { repr_into(s, in->native, repr); return; }
        sb_addc(s, '<'); sb_addstr(s, in->cls->name); sb_adds(s, " object>");
        return;
    }
    case O_PROP: sb_adds(s, "<property>"); return;
    }
    sb_adds(s, "<object>");
}

Str *to_str(V v) {
    if (is_obj(v, O_STR)) return as_str(v);
    if (v.t == V_INT) { char b[24]; snprintf(b, sizeof b, "%lld", (long long)v.u.i); return str_c(b); }
    SB s; sb_new(&s, 32);
    repr_into(&s, v, 0);
    return sb_str(&s);
}
Str *to_repr(V v) { SB s; sb_new(&s, 32); repr_into(&s, v, 1); return sb_str(&s); }

/* ---- format specs ------------------------------------------------------------------------------------ */
typedef struct { char fill, align, sign, type; int alt, zero, width, prec, group; } spec_t;

static void parse_spec(Str *sp, spec_t *f) {
    memset(f, 0, sizeof *f);
    f->fill = ' '; f->prec = -1;
    const char *p = sp ? sp->s : "";
    if (p[0] && p[1] && strchr("<>^=", p[1])) { f->fill = p[0]; f->align = p[1]; p += 2; }
    else if (p[0] && strchr("<>^=", p[0])) { f->align = *p++; }
    if (*p == '+' || *p == '-' || *p == ' ') f->sign = *p++;
    if (*p == '#') { f->alt = 1; p++; }
    if (*p == '0') { f->zero = 1; p++; }
    while (*p >= '0' && *p <= '9') f->width = f->width * 10 + (*p++ - '0');
    if (*p == ',' || *p == '_') f->group = *p++;
    if (*p == '.') { p++; f->prec = 0; while (*p >= '0' && *p <= '9') f->prec = f->prec * 10 + (*p++ - '0'); }
    if (*p) f->type = *p++;
    if (*p) raise_new(P->e_value, "Invalid format specifier '%s'", sp ? sp->s : "");
}

static void group_digits(SB *o, const char *digits, int n, char sep) {
    int lead = n % 3; if (!lead) lead = 3;
    for (int i = 0; i < n; i++) {
        if (i && sep && (i - lead) % 3 == 0 && i >= lead) sb_addc(o, sep);
        sb_addc(o, digits[i]);
    }
}

/* The body (no sign) padded to the width, with the sign where the alignment wants it. */
static Str *pad(spec_t *f, const char *sign, const char *body, uint32_t blen, int numeric) {
    uint32_t sl = (uint32_t)strlen(sign), len = sl + blen;
    char align = f->align ? f->align : numeric ? '>' : '<';
    char fill = f->fill;
    if (f->zero && !f->align && numeric) { align = '='; fill = '0'; }
    SB o; sb_new(&o, len + (uint32_t)f->width + 4);
    int padn = f->width > (int)len ? f->width - (int)len : 0;
    if (align == '=') { sb_adds(&o, sign); for (int i = 0; i < padn; i++) sb_addc(&o, fill); sb_add(&o, body, blen); }
    else {
        int left = align == '>' ? padn : align == '^' ? padn / 2 : 0, right = padn - left;
        for (int i = 0; i < left; i++) sb_addc(&o, fill);
        sb_adds(&o, sign); sb_add(&o, body, blen);
        for (int i = 0; i < right; i++) sb_addc(&o, fill);
    }
    return sb_str(&o);
}

static Str *format_float(double x, spec_t *f) {
    char b[600];
    int neg = __builtin_signbit(x) && !(x != x);
    double ax = neg ? -x : x;
    char t = f->type;
    int prec = f->prec;
    if (t == '%') { ax *= 100; }
    if (!t) {
        if (prec < 0) num_repr(ax, b, sizeof b);
        else {
            num_general(ax, prec ? prec : 1, b, sizeof b, 0, 0);
            if (!strchr(b, '.') && !strchr(b, 'e') && !strchr(b, 'n') && !strchr(b, 'i')) strcat(b, ".0");
        }
    } else if (t == 'f' || t == 'F' || t == '%') num_fixed(ax, prec < 0 ? 6 : prec, b, sizeof b);
    else if (t == 'e' || t == 'E') num_exp(ax, prec < 0 ? 6 : prec, b, sizeof b, t == 'E');
    else if (t == 'g' || t == 'G' || t == 'n') num_general(ax, prec < 0 ? 6 : prec, b, sizeof b, t == 'G', f->alt);
    else raise_new(P->e_value, "Unknown format code '%c' for object of type 'float'", t);
    if (t == 'F' || t == 'E' || t == 'G') for (char *p = b; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    if (t == '%') strcat(b, "%");
    const char *sign = neg ? "-" : f->sign == '+' ? "+" : f->sign == ' ' ? " " : "";
    if (f->group) {
        char *dot = b; while (*dot >= '0' && *dot <= '9') dot++;
        SB g; sb_new(&g, 64);
        group_digits(&g, b, (int)(dot - b), (char)f->group);
        sb_adds(&g, dot);
        return pad(f, sign, g.b, g.n, 1);
    }
    return pad(f, sign, b, (uint32_t)strlen(b), 1);
}

Str *format_value(V v, Str *spec) {
    V m;
    if (is_obj(v, O_INST) && getattr_opt(v, P->s_format, &m) && !((Inst *)v.u.o)->cls->is_exc) {
        V r = call1(m, vobj(spec ? spec : str_c("")));
        if (is_obj(r, O_STR)) return as_str(r);
    }
    spec_t f;
    parse_spec(spec, &f);
    if (!spec || !spec->len) return to_str(v);
    if (v.t == V_FLOAT) return format_float(v.u.f, &f);
    int as_int = v.t == V_BOOL ? f.type && f.type != 's' : is_int(v);
    if (as_int) {
        char t = f.type;
        if (t == 'e' || t == 'E' || t == 'f' || t == 'F' || t == 'g' || t == 'G' || t == '%') return format_float(int_to_double(v), &f);
        if (t == 'c') { int64_t c; fits_i64(v, &c); char ch = (char)c; return pad(&f, "", &ch, 1, 0); }
        int base = t == 'x' || t == 'X' ? 16 : t == 'o' ? 8 : t == 'b' ? 2 : 10;
        if (t && !strchr("dxXobn", t)) raise_new(P->e_value, "Unknown format code '%c' for object of type 'int'", t);
        int neg = int_cmp(v, vint(0)) < 0;
        Str *digits = int_to_str(neg ? int_neg(v) : v, base);
        SB body; sb_new(&body, digits->len + 8);
        if (f.alt && base != 10) sb_adds(&body, base == 16 ? (t == 'X' ? "0X" : "0x") : base == 8 ? "0o" : "0b");
        if (f.group) group_digits(&body, digits->s, (int)digits->len, (char)f.group);
        else sb_addstr(&body, digits);
        if (t == 'X') for (uint32_t i = 0; i < body.n; i++) if (body.b[i] >= 'a' && body.b[i] <= 'f') body.b[i] = (char)(body.b[i] - 32);
        const char *sign = neg ? "-" : f.sign == '+' ? "+" : f.sign == ' ' ? " " : "";
        return pad(&f, sign, body.b, body.n, 1);
    }
    if (f.type && f.type != 's') raise_new(P->e_value, "Unknown format code '%c' for object of type '%s'", f.type, type_name(v));
    Str *s = to_str(v);
    uint32_t n = f.prec >= 0 && (uint32_t)f.prec < s->len ? (uint32_t)f.prec : s->len;
    return pad(&f, "", s->s, n, 0);
}

/* "%s is %d" % (a, b) */
Str *percent_format(Str *fmt, V args) {
    SB o; sb_new(&o, fmt->len + 32);
    uint32_t ai = 0, nargs = 1;
    V *av = &args;
    if (is_obj(args, O_TUPLE)) { av = ((Tuple *)args.u.o)->v; nargs = ((Tuple *)args.u.o)->len; }
    int is_map = is_obj(args, O_DICT);
    for (uint32_t i = 0; i < fmt->len; i++) {
        char c = fmt->s[i];
        if (c != '%') { sb_addc(&o, c); continue; }
        if (++i >= fmt->len) raise_new(P->e_value, "incomplete format");
        if (fmt->s[i] == '%') { sb_addc(&o, '%'); continue; }
        V arg; int have = 0;
        if (fmt->s[i] == '(') {
            uint32_t j = i + 1;
            while (j < fmt->len && fmt->s[j] != ')') j++;
            if (!is_map) raise_new(P->e_type, "format requires a mapping");
            if (!dict_get(as_dict(args), vstrn(fmt->s + i + 1, j - i - 1), &arg)) raise_new(P->e_key, "%.*s", (int)(j - i - 1), fmt->s + i + 1);
            have = 1; i = j + 1;
        }
        spec_t f; memset(&f, 0, sizeof f); f.fill = ' '; f.prec = -1; f.align = '>';
        for (; i < fmt->len && strchr("-+ #0", fmt->s[i]); i++) {
            char fl = fmt->s[i];
            if (fl == '-') f.align = '<'; else if (fl == '+' || fl == ' ') f.sign = fl; else if (fl == '#') f.alt = 1; else f.zero = 1;
        }
        if (i < fmt->len && fmt->s[i] == '*') { int64_t w = 0; if (ai < nargs) fits_i64(av[ai++], &w); f.width = (int)w; i++; }
        while (i < fmt->len && fmt->s[i] >= '0' && fmt->s[i] <= '9') f.width = f.width * 10 + (fmt->s[i++] - '0');
        if (i < fmt->len && fmt->s[i] == '.') {
            i++; f.prec = 0;
            if (i < fmt->len && fmt->s[i] == '*') { int64_t pr = 0; if (ai < nargs) fits_i64(av[ai++], &pr); f.prec = (int)pr; i++; }
            while (i < fmt->len && fmt->s[i] >= '0' && fmt->s[i] <= '9') f.prec = f.prec * 10 + (fmt->s[i++] - '0');
        }
        while (i < fmt->len && strchr("hlL", fmt->s[i])) i++;
        if (i >= fmt->len) raise_new(P->e_value, "incomplete format");
        char t = fmt->s[i];
        if (!have) {
            if (ai >= nargs) raise_new(P->e_type, "not enough arguments for format string");
            arg = av[ai++];
        }
        if (f.zero && f.align == '<') f.zero = 0;
        if (f.zero) { f.align = '='; f.fill = '0'; }
        Str *piece;
        switch (t) {
        case 's': case 'r': case 'a': {
            Str *s = t == 's' ? to_str(arg) : to_repr(arg);
            uint32_t n = f.prec >= 0 && (uint32_t)f.prec < s->len ? (uint32_t)f.prec : s->len;
            f.zero = 0; if (f.align == '=') f.align = '>'; f.fill = ' ';
            piece = pad(&f, "", s->s, n, 0);
            break;
        }
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            V iv = arg;
            if (arg.t == V_FLOAT) iv = int_from_double(arg.u.f);
            else if (!is_int(arg)) raise_new(P->e_type, "%%%c format: a real number is required, not %s", t, type_name(arg));
            int base = t == 'x' || t == 'X' ? 16 : t == 'o' ? 8 : 10;
            int neg = int_cmp(iv, vint(0)) < 0;
            Str *d = int_to_str(neg ? int_neg(iv) : iv, base);
            SB b; sb_new(&b, d->len + 4);
            if (f.alt && base == 16) sb_adds(&b, t == 'X' ? "0X" : "0x");
            if (f.alt && base == 8) sb_adds(&b, "0o");
            for (int z = (int)d->len; z < f.prec; z++) sb_addc(&b, '0');
            sb_addstr(&b, d);
            if (t == 'X') for (uint32_t k = 0; k < b.n; k++) if (b.b[k] >= 'a') b.b[k] = (char)(b.b[k] - 32);
            piece = pad(&f, neg ? "-" : f.sign == '+' ? "+" : f.sign == ' ' ? " " : "", b.b, b.n, 1);
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': {
            double x = arg.t == V_FLOAT ? arg.u.f : is_int(arg) ? int_to_double(arg) : 0;
            if (arg.t != V_FLOAT && !is_int(arg)) raise_new(P->e_type, "must be real number, not %s", type_name(arg));
            f.type = t;
            if (f.prec < 0) f.prec = 6;
            piece = format_float(x, &f);
            break;
        }
        case 'c': {
            char ch;
            if (is_obj(arg, O_STR)) ch = as_str(arg)->len ? as_str(arg)->s[0] : 0;
            else { int64_t cv = 0; fits_i64(arg, &cv); ch = (char)cv; }
            piece = pad(&f, "", &ch, 1, 0);
            break;
        }
        default: raise_new(P->e_value, "unsupported format character '%c'", t);
        }
        sb_addstr(&o, piece);
    }
    if (ai < nargs && !is_map && is_obj(args, O_TUPLE)) raise_new(P->e_type, "not all arguments converted during string formatting");
    return sb_str(&o);
}
