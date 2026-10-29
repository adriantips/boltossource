/* ===========================================================================
 *  BoltOS Mini  -  mini/cc.c
 *
 *  C and C++, the front end (cc.h): tokens, the preprocessor, and a parser
 *  that types every expression as it reads it. Conversions are made into
 *  casts here, pointer arithmetic is scaled here, a reference is a pointer
 *  that's dereferenced where it's used, and every variable gets its place:
 *  a frame offset for a local, an address for a global. cc_eval.c then has
 *  nothing to decide but what to do.
 *
 *  Templates are kept as the tokens they were written with; using one with
 *  some types reads those tokens again with the types in place, once per
 *  set of types. The bits of the C++ library that are easiest written in
 *  C++ (stack, queue, priority_queue, the exceptions) come from a prelude
 *  read before the program.
 * ===========================================================================*/
#include <stdint.h>
#include <stdarg.h>
#include "cc.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

int cc_line;
static int is_cpp;
static const char *cur_file;

void cc_error(const char *fmt, ...) {
    char b[600];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    cc_flush();
    rt_fail(cc_line, "%s:%d: %s", cur_file ? cur_file : "program", cc_line, b);
}

static void *zalloc(size_t n) { return rt_alloc(n); }
static char *sdup(const char *s, int n) { char *d = rt_alloc_leaf((size_t)n + 1); memcpy(d, s, (size_t)n); d[n] = 0; return d; }

/* ---- types ------------------------------------------------------------------------------------ */
static CType T_void = { .kind = TY_VOID, .size = 1, .align = 1 }, T_bool = { .kind = TY_BOOL, .size = 1, .align = 1 }, T_char = { .kind = TY_CHAR, .size = 1, .align = 1 }, T_uchar = { .kind = TY_UCHAR, .size = 1, .align = 1 },
             T_short = { .kind = TY_SHORT, .size = 2, .align = 2 }, T_ushort = { .kind = TY_USHORT, .size = 2, .align = 2 }, T_int = { .kind = TY_INT, .size = 4, .align = 4 }, T_uint = { .kind = TY_UINT, .size = 4, .align = 4 },
             T_long = { .kind = TY_LONG, .size = 8, .align = 8 }, T_ulong = { .kind = TY_ULONG, .size = 8, .align = 8 }, T_float = { .kind = TY_FLOAT, .size = 4, .align = 4 }, T_double = { .kind = TY_DOUBLE, .size = 8, .align = 8 },
             T_string = { .kind = TY_STRING, .size = 24, .align = 8 }, T_ostream = { .kind = TY_OSTREAM, .size = 8, .align = 8 }, T_istream = { .kind = TY_ISTREAM, .size = 8, .align = 8 },
             T_nullptr = { .kind = TY_NULLPTR, .size = 8, .align = 8 };
CType *ty_void = &T_void, *ty_bool = &T_bool, *ty_char = &T_char, *ty_uchar = &T_uchar, *ty_short = &T_short,
      *ty_ushort = &T_ushort, *ty_int = &T_int, *ty_uint = &T_uint, *ty_long = &T_long, *ty_ulong = &T_ulong,
      *ty_float = &T_float, *ty_double = &T_double, *ty_string = &T_string, *ty_ostream = &T_ostream,
      *ty_istream = &T_istream, *ty_nullptr = &T_nullptr;

static CType *newty(int kind, int size, int align) { CType *t = zalloc(sizeof(CType)); t->kind = kind; t->size = size; t->align = align; return t; }
/* one pointer type (and one reference type) per type, so they compare by address */
typedef struct { CType *base, *t; int kind; } dtab_t;
static dtab_t *tab;                       /* on the collected heap, so it has to be a root */
static int used;
static CType **made; static int nmade;
void cc_types_reset(void) {
    tab = rt_alloc(sizeof(dtab_t) * 4096); used = 0;
    made = rt_alloc(sizeof(CType *) * 256); nmade = 0;
    rt_root((void **)&tab); rt_root((void **)&made);
}
static CType *derived(int kind, CType *b) {
    unsigned h = (unsigned)(((uintptr_t)b >> 4) * 2654435761u + (unsigned)kind) & 4095;
    for (int i = 0; i < 4096; i++) {
        unsigned k = (h + (unsigned)i) & 4095;
        if (!tab[k].t) {
            CType *t = newty(kind, 8, 8); t->base = b;
            if (used < 3500) { tab[k].base = b; tab[k].t = t; tab[k].kind = kind; used++; }
            return t;
        }
        if (tab[k].base == b && tab[k].kind == kind) return tab[k].t;
    }
    CType *t = newty(kind, 8, 8); t->base = b;
    return t;
}
void cc_types_reset(void);
CType *ty_ptr(CType *b) { return derived(TY_PTR, b); }
CType *ty_ref(CType *b) { if (b->kind == TY_REF) return b; return derived(TY_REF, b); }
CType *ty_array(CType *b, int len) { CType *t = newty(TY_ARRAY, len > 0 ? b->size * len : 0, b->align); t->base = b; t->len = len; t->owns = b->owns; return t; }
static CType *cont(int kind, CType *a, CType *b) {
    /* one type per element type, so they compare equal by pointer */
    for (int i = 0; i < nmade; i++) if (made[i]->kind == kind && made[i]->base == a && made[i]->base2 == b) return made[i];
    CType *t = newty(kind, kind == TY_PAIR ? 0 : 24, 8);
    t->base = a; t->base2 = b; t->owns = 1;
    if (kind == TY_PAIR) {
        int o2 = (a->size + b->align - 1) / b->align * b->align;
        t->size = (o2 + b->size + 7) / 8 * 8;
        t->align = a->align > b->align ? a->align : b->align;
        t->owns = a->owns || b->owns;
        t->mem = zalloc(sizeof(CMember) * 2); t->nmem = 2; t->complete = 1;
        t->mem[0].name = "first"; t->mem[0].ty = a; t->mem[0].offset = 0;
        t->mem[1].name = "second"; t->mem[1].ty = b; t->mem[1].offset = o2;
    }
    if (nmade < 256) made[nmade++] = t;
    return t;
}
CType *ty_vector(CType *e) { return cont(TY_VECTOR, e, 0); }
CType *ty_set(CType *e) { return cont(TY_SET, e, 0); }
CType *ty_map(CType *k, CType *v) { return cont(TY_MAP, k, v); }
CType *ty_pair(CType *a, CType *b) { return cont(TY_PAIR, a, b); }
int ty_is_int(CType *t) { return t->kind >= TY_BOOL && t->kind <= TY_ULONG ? 1 : t->kind == TY_ENUM; }
int ty_is_flt(CType *t) { return t->kind == TY_FLOAT || t->kind == TY_DOUBLE; }
int ty_is_num(CType *t) { return ty_is_int(t) || ty_is_flt(t); }
int ty_is_ptr(CType *t) { return t->kind == TY_PTR || t->kind == TY_ARRAY || t->kind == TY_NULLPTR; }
static int is_unsigned(CType *t) { return t->kind == TY_UCHAR || t->kind == TY_USHORT || t->kind == TY_UINT || t->kind == TY_ULONG || t->kind == TY_BOOL; }
int ty_owns(CType *t) { return t->owns; }
static int is_obj(CType *t) { return t->kind == TY_STRUCT || t->kind == TY_STRING || t->kind == TY_VECTOR || t->kind == TY_MAP || t->kind == TY_SET || t->kind == TY_PAIR || t->kind == TY_LAMBDA || t->kind == TY_ARRAY; }

const char *ty_str(CType *t, char *b, int cap) {
    static const char *const names[] = { "void", "bool", "char", "unsigned char", "short", "unsigned short", "int",
        "unsigned int", "long", "unsigned long", "float", "double", "enum", "*", "[]", "function", "struct", "&",
        "nullptr_t", "string", "vector", "map", "set", "pair", "ostream", "istream", "lambda" };
    char x[120];
    switch (t->kind) {
    case TY_PTR: snprintf(b, (size_t)cap, "%s*", ty_str(t->base, x, sizeof x)); break;
    case TY_REF: snprintf(b, (size_t)cap, "%s&", ty_str(t->base, x, sizeof x)); break;
    case TY_ARRAY: snprintf(b, (size_t)cap, "%s[%d]", ty_str(t->base, x, sizeof x), t->len); break;
    case TY_STRUCT: snprintf(b, (size_t)cap, "%s", t->name ? t->name : "struct"); break;
    case TY_VECTOR: snprintf(b, (size_t)cap, "vector<%s>", ty_str(t->base, x, sizeof x)); break;
    default: snprintf(b, (size_t)cap, "%s", names[t->kind]);
    }
    return b;
}

/* ---- tokens ------------------------------------------------------------------------------------- */
enum { K_EOF, K_ID, K_NUM, K_FNUM, K_STR, K_P };
typedef struct {
    uint8_t k, bol, space, isf;
    int line, len;
    char *s;                              /* the text; a string literal's bytes            */
    int64_t ival; double fval;
    CType *ty;                            /* a number's or character's type               */
    int16_t hide;                         /* macro expansion: don't expand this name again */
} Tok;

typedef struct { Tok *t; int n, cap; } TV;
static void tv_push(TV *v, Tok t) {
    if (v->n == v->cap) { int nc = v->cap ? v->cap * 2 : 1024; v->t = rt_realloc(v->t, sizeof(Tok) * (size_t)v->cap, sizeof(Tok) * (size_t)nc); v->cap = nc; }
    v->t[v->n++] = t;
}

static const char *const PUNCT[] = { "<<=", ">>=", "...", "->", "::", "++", "--", "<<", ">>", "<=", ">=", "==", "!=",
    "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##", 0 };

static int hexd(int c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

static int escape(const char **pp) {
    const char *p = *pp;
    int c = *p++;
    switch (c) {
    case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break; case '0': case '1': case '2':
    case '3': case '4': case '5': case '6': case '7': {
        c -= '0';
        for (int i = 0; i < 2 && *p >= '0' && *p <= '7'; i++) c = c * 8 + (*p++ - '0');
        break;
    }
    case 'x': c = 0; while (hexd((unsigned char)*p) >= 0) c = c * 16 + hexd((unsigned char)*p++); break;
    case 'a': c = 7; break; case 'b': c = 8; break; case 'f': c = 12; break; case 'v': c = 11; break;
    case 'e': c = 27; break;
    default: break;                    /* \\ \' \" \? */
    }
    *pp = p;
    return c & 0xFF;
}

static void tokenize(const char *src, TV *out) {
    const char *p = src;
    int line = 1, bol = 1, space = 0;
    while (*p) {
        if (*p == '\n') { line++; p++; bol = 1; space = 1; continue; }
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\f' || *p == '\v') { p++; space = 1; continue; }
        if (p[0] == '\\' && p[1] == '\n') { p += 2; line++; continue; }
        if (p[0] == '\\' && p[1] == '\r' && p[2] == '\n') { p += 3; line++; continue; }
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; space = 1; continue; }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) { if (*p == '\n') line++; p++; }
            if (!*p) { cc_line = line; cc_error("unterminated comment"); }
            p += 2; space = 1; continue;
        }
        Tok t; memset(&t, 0, sizeof t);
        t.line = line; t.bol = (uint8_t)bol; t.space = (uint8_t)space; t.hide = -1;
        bol = 0; space = 0;
        if ((*p >= '0' && *p <= '9') || (*p == '.' && p[1] >= '0' && p[1] <= '9')) {
            const char *s = p;
            int isf = 0;
            if (p[0] == '0' && (p[1] | 32) == 'x') { p += 2; while (hexd((unsigned char)*p) >= 0 || *p == '\'') p++; }
            else if (p[0] == '0' && (p[1] | 32) == 'b') { p += 2; while (*p == '0' || *p == '1' || *p == '\'') p++; }
            else {
                while ((*p >= '0' && *p <= '9') || *p == '\'') p++;
                if (*p == '.') { isf = 1; p++; while (*p >= '0' && *p <= '9') p++; }
                if ((*p | 32) == 'e' && (p[1] == '+' || p[1] == '-' || (p[1] >= '0' && p[1] <= '9'))) { isf = 1; p += 2; while (*p >= '0' && *p <= '9') p++; }
            }
            char buf[100]; int n = 0;
            for (const char *q = s; q < p && n < 99; q++) if (*q != '\'') buf[n++] = *q;
            buf[n] = 0;
            int u = 0, l = 0, f = 0;
            while (*p && strchr("uUlLfF", *p)) { if ((*p | 32) == 'u') u = 1; else if ((*p | 32) == 'l') l++; else f = 1; p++; }
            if (isf || (f && !(buf[0] == '0' && (buf[1] | 32) == 'x'))) {
                t.k = K_FNUM; t.fval = num_parse(buf, 0); t.ty = f ? ty_float : ty_double;
                if (f) t.fval = (double)(float)t.fval;
            } else {
                t.k = K_NUM;
                uint64_t v;
                if (buf[0] == '0' && (buf[1] | 32) == 'b') { v = 0; for (int i = 2; buf[i]; i++) v = v * 2 + (uint64_t)(buf[i] - '0'); }
                else v = strtoull(buf, 0, buf[0] == '0' && (buf[1] | 32) == 'x' ? 16 : buf[0] == '0' ? 8 : 10);
                t.ival = (int64_t)v;
                if (l) t.ty = u ? ty_ulong : ty_long;
                else if (u) t.ty = v > 0xFFFFFFFFull ? ty_ulong : ty_uint;
                else t.ty = v > 0x7FFFFFFFull ? ty_long : ty_int;
            }
            t.s = sdup(s, (int)(p - s)); t.len = (int)(p - s);
            tv_push(out, t);
            continue;
        }
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_' || (unsigned char)*p >= 128) {
            const char *s = p;
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || (unsigned char)*p >= 128) p++;
            int n = (int)(p - s);
            /* L"..", u8".." : the prefix goes */
            if ((n == 1 && (*s == 'L' || *s == 'u' || *s == 'U')) || (n == 2 && s[0] == 'u' && s[1] == '8')) {
                if (*p == '"' || *p == '\'') goto literal;
            }
            t.k = K_ID; t.s = sdup(s, n); t.len = n;
            tv_push(out, t);
            continue;
        }
    literal:
        if (*p == '"') {
            p++;
            char *b = rt_alloc_leaf(256); int cap = 256, n = 0;
            while (*p && *p != '"') {
                if (*p == '\n') { cc_line = line; cc_error("missing terminating \" character"); }
                int c = *p == '\\' ? (p++, escape(&p)) : (unsigned char)*p++;
                if (n + 2 > cap) { b = rt_realloc(b, (size_t)cap, (size_t)cap * 2); cap *= 2; }
                b[n++] = (char)c;
            }
            if (!*p) { cc_line = line; cc_error("missing terminating \" character"); }
            p++;
            b[n] = 0;
            t.k = K_STR; t.s = b; t.len = n;
            tv_push(out, t);
            continue;
        }
        if (*p == '\'') {
            p++;
            int c = *p == '\\' ? (p++, escape(&p)) : (unsigned char)*p++;
            int v = c;
            while (*p && *p != '\'' && *p != '\n') { v = v << 8 | (unsigned char)*p++; }
            if (*p != '\'') { cc_line = line; cc_error("missing terminating ' character"); }
            p++;
            t.k = K_NUM; t.ival = (c & 0x80) && v == c ? (int64_t)(signed char)c : v; t.ty = is_cpp ? ty_char : ty_int;
            t.s = "'"; t.len = 1;
            tv_push(out, t);
            continue;
        }
        int matched = 0;
        for (int i = 0; PUNCT[i]; i++) {
            int n = (int)strlen(PUNCT[i]);
            if (!strncmp(p, PUNCT[i], (size_t)n)) { t.k = K_P; t.s = (char *)PUNCT[i]; t.len = n; p += n; matched = 1; break; }
        }
        if (!matched) {
            if (!strchr("+-*/%&|^~!=<>?:;,.()[]{}#", *p)) { cc_line = line; cc_error("stray '%c' in program", *p); }
            t.k = K_P; t.s = sdup(p, 1); t.len = 1; p++;
        }
        tv_push(out, t);
    }
    Tok e; memset(&e, 0, sizeof e);
    e.k = K_EOF; e.line = line; e.bol = 1; e.s = ""; e.hide = -1;
    tv_push(out, e);
}

/* ---- the preprocessor ------------------------------------------------------------------------------ */
typedef struct { char *name; int fn, nparams, variadic; char **params; Tok *body; int nbody; int line; } Macro;
static Macro *macros; static int nmacros, macro_cap;

static Macro *find_macro(const char *s, int len) {
    for (int i = nmacros - 1; i >= 0; i--) if (macros[i].name && (int)strlen(macros[i].name) == len && !memcmp(macros[i].name, s, (size_t)len)) return &macros[i];
    return 0;
}
static void def_macro(const char *name, const char *body) {
    TV tv = { 0 };
    tokenize(body, &tv);
    if (nmacros == macro_cap) { int nc = macro_cap ? macro_cap * 2 : 64; macros = rt_realloc(macros, sizeof(Macro) * (size_t)macro_cap, sizeof(Macro) * (size_t)nc); macro_cap = nc; }
    Macro *m = &macros[nmacros++];
    memset(m, 0, sizeof *m);
    m->name = sdup(name, (int)strlen(name));
    m->body = tv.t; m->nbody = tv.n - 1;
}

static int tok_is(Tok *t, const char *s) { return (t->k == K_P || t->k == K_ID) && t->len == (int)strlen(s) && !memcmp(t->s, s, (size_t)t->len); }

/* #if's expression, on already-expanded tokens */
typedef struct { Tok *t; int i, n; } PE;
static int64_t pe_cond(PE *e);
static int64_t pe_prim(PE *e) {
    if (e->i >= e->n) return 0;
    Tok *t = &e->t[e->i++];
    if (tok_is(t, "(")) { int64_t v = pe_cond(e); if (e->i < e->n && tok_is(&e->t[e->i], ")")) e->i++; return v; }
    if (tok_is(t, "!")) return !pe_prim(e);
    if (tok_is(t, "-")) return -pe_prim(e);
    if (tok_is(t, "~")) return ~pe_prim(e);
    if (tok_is(t, "+")) return pe_prim(e);
    if (t->k == K_NUM) return t->ival;
    return 0;                                 /* an unknown name is 0 */
}
static int64_t pe_bin(PE *e, int lvl) {
    static const char *const L[][4] = { { "||" }, { "&&" }, { "|" }, { "^" }, { "&" }, { "==", "!=" }, { "<", ">", "<=", ">=" },
                                        { "<<", ">>" }, { "+", "-" }, { "*", "/", "%" } };
    if (lvl == 10) return pe_prim(e);
    int64_t a = pe_bin(e, lvl + 1);
    for (;;) {
        if (e->i >= e->n) return a;
        Tok *t = &e->t[e->i];
        int k = -1;
        for (int j = 0; j < 4 && L[lvl][j]; j++) if (tok_is(t, L[lvl][j])) k = j;
        if (k < 0) return a;
        e->i++;
        int64_t b = pe_bin(e, lvl + 1);
        const char *o = L[lvl][k];
        if (!strcmp(o, "||")) a = a || b; else if (!strcmp(o, "&&")) a = a && b; else if (!strcmp(o, "|")) a |= b;
        else if (!strcmp(o, "^")) a ^= b; else if (!strcmp(o, "&")) a &= b; else if (!strcmp(o, "==")) a = a == b;
        else if (!strcmp(o, "!=")) a = a != b; else if (!strcmp(o, "<")) a = a < b; else if (!strcmp(o, ">")) a = a > b;
        else if (!strcmp(o, "<=")) a = a <= b; else if (!strcmp(o, ">=")) a = a >= b; else if (!strcmp(o, "<<")) a <<= b;
        else if (!strcmp(o, ">>")) a >>= b; else if (!strcmp(o, "+")) a += b; else if (!strcmp(o, "-")) a -= b;
        else if (!strcmp(o, "*")) a *= b; else if (b) a = !strcmp(o, "/") ? a / b : a % b;
    }
}
static int64_t pe_cond(PE *e) {
    int64_t c = pe_bin(e, 0);
    if (e->i < e->n && tok_is(&e->t[e->i], "?")) {
        e->i++;
        int64_t a = pe_cond(e);
        if (e->i < e->n && tok_is(&e->t[e->i], ":")) e->i++;
        int64_t b = pe_cond(e);
        return c ? a : b;
    }
    return c;
}

static void expand(Tok *in, int n, TV *out, int depth);

/* A macro's use at in[*i]: its arguments read, the body substituted and expanded again. */
static int expand_one(Tok *in, int n, int *i, TV *out, int depth) {
    Tok *t = &in[*i];
    Macro *m = find_macro(t->s, t->len);
    if (!m || depth > 40 || t->hide == (int16_t)(m - macros)) return 0;
    int mi = (int)(m - macros);
    if (m->fn) {
        int j = *i + 1;
        if (j >= n || !tok_is(&in[j], "(")) return 0;
        /* the arguments */
        int argstart[64], argend[64], na = 0, d = 0;
        j++;
        argstart[0] = j;
        for (; j < n; j++) {
            if (tok_is(&in[j], "(")) d++;
            else if (tok_is(&in[j], ")")) { if (!d) break; d--; }
            else if (tok_is(&in[j], ",") && !d && !(m->variadic && na == m->nparams - 1)) { argend[na] = j; if (na < 63) na++; argstart[na] = j + 1; }
        }
        if (j >= n) { cc_line = t->line; cc_error("unterminated call to the macro %s", m->name); }
        argend[na] = j;
        if (argend[na] > argstart[na] || na) na++;
        TV body = { 0 };
        for (int b = 0; b < m->nbody; b++) {
            Tok *bt = &m->body[b];
            int pi = -1;
            if (bt->k == K_ID) for (int q = 0; q < m->nparams; q++) if (!strcmp(m->params[q], bt->s)) pi = q;
            if (bt->k == K_ID && m->variadic && !strcmp(bt->s, "__VA_ARGS__")) pi = m->nparams - 1;
            if (tok_is(bt, "#") && b + 1 < m->nbody) {
                /* #param: as a string */
                int pq = -1;
                for (int q = 0; q < m->nparams; q++) if (!strcmp(m->params[q], m->body[b + 1].s)) pq = q;
                if (pq >= 0) {
                    char s[1024]; int sn = 0;
                    for (int a = argstart[pq]; pq < na && a < argend[pq] && sn < 1000; a++) {
                        if (a > argstart[pq] && in[a].space) s[sn++] = ' ';
                        if (in[a].k == K_STR) { s[sn++] = '"'; for (int z = 0; z < in[a].len && sn < 1000; z++) s[sn++] = in[a].s[z]; s[sn++] = '"'; }
                        else for (int z = 0; z < in[a].len && sn < 1000; z++) s[sn++] = in[a].s[z];
                    }
                    Tok st = *bt; st.k = K_STR; st.s = sdup(s, sn); st.len = sn; st.line = t->line;
                    tv_push(&body, st);
                    b++;
                    continue;
                }
            }
            if (tok_is(bt, "##") && body.n && b + 1 < m->nbody) {
                /* paste the last token and the next */
                Tok *nx = &m->body[++b];
                Tok *prev = &body.t[body.n - 1];
                char s[256];
                const char *rhs = nx->s; int rl = nx->len;
                int pq = -1;
                if (nx->k == K_ID) for (int q = 0; q < m->nparams; q++) if (!strcmp(m->params[q], nx->s)) pq = q;
                if (pq >= 0 && pq < na && argend[pq] > argstart[pq]) { rhs = in[argstart[pq]].s; rl = in[argstart[pq]].len; }
                snprintf(s, sizeof s, "%.*s%.*s", prev->len, prev->s, rl, rhs);
                TV pt = { 0 };
                tokenize(s, &pt);
                body.n--;
                for (int z = 0; z < pt.n - 1; z++) { pt.t[z].line = t->line; tv_push(&body, pt.t[z]); }
                continue;
            }
            if (pi >= 0) {
                if (pi < na) {
                    TV ex = { 0 };
                    expand(in + argstart[pi], argend[pi] - argstart[pi], &ex, depth + 1);
                    for (int z = 0; z < ex.n; z++) { Tok c = ex.t[z]; c.line = t->line; tv_push(&body, c); }
                }
                continue;
            }
            Tok c = *bt; c.line = t->line; c.bol = 0;
            tv_push(&body, c);
        }
        for (int z = 0; z < body.n; z++) if (body.t[z].k == K_ID && body.t[z].hide < 0 && find_macro(body.t[z].s, body.t[z].len) == m) body.t[z].hide = (int16_t)mi;
        expand(body.t, body.n, out, depth + 1);
        *i = j;
        return 1;
    }
    TV body = { 0 };
    for (int b = 0; b < m->nbody; b++) {
        Tok c = m->body[b]; c.line = t->line; c.bol = 0; c.space = b ? c.space : t->space;
        if (c.k == K_ID && find_macro(c.s, c.len) == m) c.hide = (int16_t)mi;
        tv_push(&body, c);
    }
    expand(body.t, body.n, out, depth + 1);
    return 1;
}

static void expand(Tok *in, int n, TV *out, int depth) {
    for (int i = 0; i < n; i++) {
        if (in[i].k == K_ID && expand_one(in, n, &i, out, depth)) continue;
        if (in[i].k == K_ID && !strcmp(in[i].s, "__LINE__")) { Tok c = in[i]; c.k = K_NUM; c.ival = c.line; c.ty = ty_int; tv_push(out, c); continue; }
        tv_push(out, in[i]);
    }
}

static int read_include(const char *name, char **src);

static void preprocess(Tok *t, int n, TV *out, int depth) {
    int skip[64], taken[64], sp = 0;              /* the #if stack */
    #define SKIPPING (sp > 0 && skip[sp - 1])
    int i = 0;
    while (i < n && t[i].k != K_EOF) {
        if (!(tok_is(&t[i], "#") && t[i].bol)) {
            /* a run of ordinary tokens up to the next directive, expanded */
            int j = i;
            while (j < n && t[j].k != K_EOF && !(tok_is(&t[j], "#") && t[j].bol)) j++;
            if (!SKIPPING) expand(t + i, j - i, out, 0);
            i = j;
            continue;
        }
        int line = t[i].line;
        i++;
        int e = i;
        while (e < n && t[e].k != K_EOF && !t[e].bol) e++;   /* the directive's tokens: [i, e) */
        const char *d = i < e && t[i].k == K_ID ? t[i].s : "";
        cc_line = line;
        if (!strcmp(d, "ifdef") || !strcmp(d, "ifndef")) {
            int def = i + 1 < e && find_macro(t[i + 1].s, t[i + 1].len) != 0;
            int on = d[2] == 'd' ? def : !def;
            if (sp < 64) { skip[sp] = SKIPPING || !on; taken[sp] = on; sp++; }
        } else if (!strcmp(d, "if") || !strcmp(d, "elif")) {
            int on = 0;
            if (!strcmp(d, "elif") && sp) { if (taken[sp - 1] || (sp > 1 && skip[sp - 2])) { skip[sp - 1] = 1; i = e; continue; } }
            /* defined(X) first, then the macros, then the arithmetic */
            TV a = { 0 };
            for (int k = i + 1; k < e; k++) {
                if (t[k].k == K_ID && !strcmp(t[k].s, "defined")) {
                    int par = k + 1 < e && tok_is(&t[k + 1], "(");
                    int nm = k + 1 + par;
                    Tok c = t[k]; c.k = K_NUM; c.ival = nm < e && find_macro(t[nm].s, t[nm].len) != 0; c.ty = ty_int;
                    tv_push(&a, c);
                    k = nm + par;
                    continue;
                }
                tv_push(&a, t[k]);
            }
            TV x = { 0 };
            expand(a.t, a.n, &x, 0);
            PE pe = { x.t, 0, x.n };
            on = pe_cond(&pe) != 0;
            if (!strcmp(d, "if")) { if (sp < 64) { skip[sp] = SKIPPING || !on; taken[sp] = on; sp++; } }
            else if (sp) { skip[sp - 1] = !on; if (on) taken[sp - 1] = 1; }
        } else if (!strcmp(d, "else")) {
            if (sp) { skip[sp - 1] = taken[sp - 1] || (sp > 1 && skip[sp - 2]); taken[sp - 1] = 1; }
        } else if (!strcmp(d, "endif")) {
            if (sp) sp--;
        } else if (SKIPPING) {
        } else if (!strcmp(d, "define")) {
            if (i + 1 >= e || t[i + 1].k != K_ID) cc_error("macro names must be identifiers");
            Tok *nm = &t[i + 1];
            if (nmacros == macro_cap) { int nc = macro_cap ? macro_cap * 2 : 64; macros = rt_realloc(macros, sizeof(Macro) * (size_t)macro_cap, sizeof(Macro) * (size_t)nc); macro_cap = nc; }
            Macro *m = &macros[nmacros++];
            memset(m, 0, sizeof *m);
            m->name = nm->s;
            int k = i + 2;
            if (k < e && tok_is(&t[k], "(") && !t[k].space) {
                m->fn = 1;
                m->params = zalloc(sizeof(char *) * 32);
                k++;
                while (k < e && !tok_is(&t[k], ")")) {
                    if (tok_is(&t[k], "...")) { m->params[m->nparams++] = "__VA_ARGS__"; m->variadic = 1; }
                    else if (t[k].k == K_ID && m->nparams < 32) m->params[m->nparams++] = t[k].s;
                    k++;
                    if (k < e && tok_is(&t[k], ",")) k++;
                }
                k++;
            }
            m->body = t + k; m->nbody = e - k;
        } else if (!strcmp(d, "undef")) {
            Macro *m = i + 1 < e ? find_macro(t[i + 1].s, t[i + 1].len) : 0;
            if (m) m->name = 0;
        } else if (!strcmp(d, "include")) {
            if (i + 1 < e && t[i + 1].k == K_STR) {
                char *src;
                if (depth > 8) cc_error("#include nested too deeply");
                if (read_include(t[i + 1].s, &src) == 0) {
                    TV inc = { 0 };
                    tokenize(src, &inc);
                    preprocess(inc.t, inc.n, out, depth + 1);
                    cc_line = line;
                }
                /* a standard header written with quotes is the same as with <> */
            }
            /* <stdio.h> and the others: what they declare is built in */
        } else if (!strcmp(d, "error")) {
            char msg[300]; int mn = 0;
            for (int k = i + 1; k < e && mn < 280; k++) mn += snprintf(msg + mn, sizeof msg - (size_t)mn, "%s%.*s", k > i + 1 ? " " : "", t[k].len, t[k].s);
            msg[mn] = 0;
            cc_error("#error %s", msg);
        } else if (!strcmp(d, "pragma") || !strcmp(d, "line") || !*d) {
        } else cc_error("unknown directive #%s", d);
        i = e;
    }
    #undef SKIPPING
}

/* ---- the parser's state ------------------------------------------------------------------------------- */
typedef struct Var {
    char *name;
    CType *ty;
    int local, is_typedef, is_enum, depth;
    int64_t off;                          /* frame offset, or address                  */
    int64_t ival;                         /* an enum constant                          */
    CFunc *fn;
    struct Var *next;
} Var;

typedef struct Scope { Var *vars; Var *tags; struct Scope *up; CNode *block; } Scope;

typedef struct Templ {
    char *name;
    char **params; int nparams;
    Tok **pdefault; int *ndefault;        /* default type arguments, as tokens           */
    Tok *body; int nbody;                 /* from `class` or the return type to the end  */
    int is_class;
    struct Inst { CType **args; void *made; } *inst; int ninst;
    struct Templ *next;
} Templ;

static Tok *tk; static int ntk, pos;
static Scope *scope;
static CFunc *cur_fn;                     /* the function being parsed                 */
static CType *cur_class;                  /* the class whose method is being parsed    */
static int frame_size, lambda_depth;
static CFunc **funcs; static int nfuncs, func_cap;
static Templ *templates;
static CType **classes; static int nclasses;
static uint64_t data_top;                 /* globals and literals go below this        */
static CNode **global_init; static int nginit, ginit_cap;
static int loop_depth, switch_depth;
static CType **tmpl_bind_ty; static char **tmpl_bind_name; static int ntmpl_bind;
static CType *cur_inst_type; static const char *cur_inst_base;   /* the class template being instantiated */

CType *cc_class_by_id(int id) { return id > 0 && id <= nclasses ? classes[id - 1] : 0; }

static Tok *cur(void) { return &tk[pos]; }
static Tok *peekt(int k) { return &tk[pos + k < ntk ? pos + k : ntk - 1]; }
static int at(const char *s) { return tok_is(cur(), s); }
static int at_k(int k, const char *s) { return tok_is(peekt(k), s); }
static void next(void) { if (pos < ntk - 1) pos++; cc_line = cur()->line; }
static int accept(const char *s) { if (at(s)) { next(); return 1; } return 0; }
static void syntax(const char *fmt, ...) __attribute__((noreturn));
static void syntax(const char *fmt, ...) {
    char b[300];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    cc_line = cur()->line;
    cc_error("%s", b);
}
static void expect(const char *s) {
    if (accept(s)) return;
    if (cur()->k == K_EOF) syntax("expected '%s' at the end of the file", s);
    syntax("expected '%s' before '%.*s'", s, cur()->len, cur()->s);
}
static char *ident(void) {
    if (cur()->k != K_ID) syntax("expected a name before '%.*s'", cur()->len, cur()->s);
    char *s = cur()->s;
    next();
    return s;
}
/* `>>` closing two template argument lists */
static int split_gt;
static void expect_gt(void) {
    if (split_gt) { split_gt = 0; next(); return; }
    if (at(">>")) { split_gt = 1; return; }
    if (at(">>=")) syntax("'>>=' after a template");
    expect(">");
}

static CNode *node(int k) { CNode *n = zalloc(sizeof(CNode)); n->k = k; n->line = cur()->line; return n; }
static void add_kid(CNode *p, CNode *c) {
    if ((p->nk & (p->nk - 1)) == 0) p->kids = rt_realloc(p->kids, sizeof(CNode *) * (size_t)p->nk, sizeof(CNode *) * (size_t)(p->nk ? p->nk * 2 : 2));
    p->kids[p->nk++] = c;
}

static void push_scope(void) { Scope *s = zalloc(sizeof(Scope)); s->up = scope; scope = s; }
static void pop_scope(void) { scope = scope->up; }

static Var *find_var(const char *name) {
    for (Scope *s = scope; s; s = s->up)
        for (Var *v = s->vars; v; v = v->next) if (!strcmp(v->name, name)) return v;
    return 0;
}
static Var *find_tag(const char *name) {
    for (Scope *s = scope; s; s = s->up)
        for (Var *v = s->tags; v; v = v->next) if (!strcmp(v->name, name)) return v;
    return 0;
}
static Var *add_var(const char *name, CType *ty, int local) {
    Var *v = zalloc(sizeof(Var));
    v->name = (char *)name; v->ty = ty; v->local = local; v->depth = lambda_depth;
    v->next = scope->vars; scope->vars = v;
    return v;
}
static void add_tag(const char *name, CType *ty) {
    Var *v = zalloc(sizeof(Var));
    v->name = (char *)name; v->ty = ty;
    v->next = scope->tags; scope->tags = v;
}

static int align_to(int n, int a) { return (n + a - 1) / a * a; }
static int64_t alloc_local(CType *t) {
    int a = t->align < 1 ? 1 : t->align;
    frame_size = align_to(frame_size, a);
    int64_t off = frame_size;
    frame_size += t->size > 0 ? t->size : 1;
    return off;
}
static uint64_t alloc_global(int size, int align) {
    data_top = (uint64_t)align_to((int)data_top, align < 1 ? 1 : align);
    uint64_t a = data_top;
    data_top += (uint64_t)(size > 0 ? size : 1);
    if (data_top > cc_memsz / 4) cc_error("the program's global data is too big");
    return a;
}

static CFunc *add_func(CFunc *f) {
    if (nfuncs == func_cap) { int nc = func_cap ? func_cap * 2 : 128; funcs = rt_realloc(funcs, sizeof(CFunc *) * (size_t)func_cap, sizeof(CFunc *) * (size_t)nc); func_cap = nc; }
    funcs[nfuncs++] = f;
    return f;
}
int cc_func_index(CFunc *f);
int cc_func_index(CFunc *f) { for (int i = 0; i < nfuncs; i++) if (funcs[i] == f) return i + 1; add_func(f); return nfuncs; }
CFunc *cc_func_by_index(int i);
CFunc *cc_func_by_index(int i) { return i > 0 && i <= nfuncs ? funcs[i - 1] : 0; }

/* ---- types in declarations ------------------------------------------------------------------------------ */
static CType *declspec(int *is_typedef, int *is_static, int need);
static CType *declarator(CType *base, char **name);
static CType *abstract_type(void);
static CNode *expr(void);
static CNode *assign(void);
static CNode *cond_expr(void);
static CNode *stmt(void);
static CNode *compound(int new_scope);
static void parse_class_body(CType *t);
static CType *instantiate_class(Templ *tp, CType **args, int nargs);
static Templ *find_template(const char *name);
static CType *template_args_type(Templ *tp);
static int64_t const_expr(void);

static int is_type_name(Tok *t) {
    if (t->k != K_ID) return 0;
    static const char *const kw[] = { "void", "bool", "_Bool", "char", "short", "int", "long", "float", "double", "signed",
        "unsigned", "struct", "union", "enum", "const", "volatile", "static", "extern", "typedef", "inline", "register",
        "auto", "class", "string", "vector", "map", "set", "pair", "unordered_map", "unordered_set", "multiset",
        "size_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "constexpr", "virtual", "std", "typename", "ostream", "istream", "function", "FILE", "time_t", "clock_t",
        "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t", "explicit", "mutable", "thread_local", "noexcept",
        "long long", "wchar_t", "char16_t", "char32_t", "ifstream", "ofstream", "stringstream", "istringstream",
        "ostringstream", "array", "deque", "list", "multimap", "tuple", 0 };
    for (int i = 0; kw[i]; i++) if (!strcmp(t->s, kw[i])) return 1;
    Var *v = find_var(t->s);
    if (v && v->is_typedef) return 1;
    for (int i = 0; i < ntmpl_bind; i++) if (!strcmp(tmpl_bind_name[i], t->s)) return 1;
    Templ *tp = find_template(t->s);
    return tp && tp->is_class;
}

static CType *bound_param(const char *name) {
    for (int i = ntmpl_bind - 1; i >= 0; i--) if (!strcmp(tmpl_bind_name[i], name)) return tmpl_bind_ty[i];
    return 0;
}

static CType *struct_decl(int is_union, int is_class_kw);
static CType *enum_decl(void);

/* vector<int>, map<string, int>, pair<int,int>, function<...> */
static CType *std_template(const char *nm) {
    expect("<");
    if (!strcmp(nm, "function")) {
        /* function<int(int, int)>: any callable */
        int d = 1;
        while (d && cur()->k != K_EOF) {
            if (at("<")) d++;
            else if (at(">")) { if (--d == 0) break; }
            else if (at(">>")) { if (d <= 2) { if (d == 1) { split_gt = 1; break; } d -= 2; if (!d) { next(); return newty(TY_LAMBDA, 16, 8); } } else d -= 2; }
            next();
        }
        expect_gt();
        CType *t = newty(TY_LAMBDA, 16, 8);
        return t;
    }
    CType *a = abstract_type(), *b = 0;
    if (accept(",")) b = abstract_type();
    while (accept(",")) abstract_type();            /* allocators, hashers, comparators */
    expect_gt();
    if (!strcmp(nm, "vector") || !strcmp(nm, "deque") || !strcmp(nm, "list") || !strcmp(nm, "array")) return ty_vector(a);
    if (!strcmp(nm, "set") || !strcmp(nm, "unordered_set") || !strcmp(nm, "multiset")) return ty_set(a);
    if (!strcmp(nm, "map") || !strcmp(nm, "unordered_map") || !strcmp(nm, "multimap")) { if (!b) syntax("map takes two types"); return ty_map(a, b); }
    if (!strcmp(nm, "pair")) { if (!b) syntax("pair takes two types"); return ty_pair(a, b); }
    syntax("'%s' isn't available", nm);
}

static CType *declspec(int *is_typedef, int *is_static, int need) {
    int sgn = 0, uns = 0, shrt = 0, lng = 0, base = -1, cnst = 0;
    CType *ty = 0;
    if (is_typedef) *is_typedef = 0;
    if (is_static) *is_static = 0;
    int any = 0;
    for (;;) {
        Tok *t = cur();
        if (t->k != K_ID) break;
        const char *s = t->s;
        if (!strcmp(s, "typedef")) { if (is_typedef) *is_typedef = 1; next(); any = 1; continue; }
        if (!strcmp(s, "static")) { if (is_static) *is_static = 1; next(); any = 1; continue; }
        if (!strcmp(s, "const") || !strcmp(s, "constexpr")) { cnst = 1; next(); any = 1; continue; }
        if (!strcmp(s, "extern") || !strcmp(s, "inline") || !strcmp(s, "volatile") || !strcmp(s, "register") ||
            !strcmp(s, "virtual") || !strcmp(s, "explicit") || !strcmp(s, "mutable") || !strcmp(s, "typename") ||
            !strcmp(s, "thread_local") || !strcmp(s, "friend")) { next(); any = 1; continue; }
        if (!strcmp(s, "std") && at_k(1, "::")) { next(); next(); continue; }
        if (ty || base >= 0) {
            /* `long long`, `unsigned int` keep going; a second type name doesn't */
            if (!strcmp(s, "int") || !strcmp(s, "long") || !strcmp(s, "short") || !strcmp(s, "char") || !strcmp(s, "double") ||
                !strcmp(s, "unsigned") || !strcmp(s, "signed")) { } else break;
            if (ty) break;
        }
        if (!strcmp(s, "signed")) { sgn = 1; next(); any = 1; continue; }
        if (!strcmp(s, "unsigned")) { uns = 1; next(); any = 1; continue; }
        if (!strcmp(s, "short")) { shrt = 1; next(); any = 1; continue; }
        if (!strcmp(s, "long")) { lng++; next(); any = 1; continue; }
        if (!strcmp(s, "void")) { base = TY_VOID; next(); any = 1; continue; }
        if (!strcmp(s, "bool") || !strcmp(s, "_Bool")) { base = TY_BOOL; next(); any = 1; continue; }
        if (!strcmp(s, "char") || !strcmp(s, "wchar_t") || !strcmp(s, "char16_t") || !strcmp(s, "char32_t")) { base = TY_CHAR; next(); any = 1; continue; }
        if (!strcmp(s, "int")) { base = TY_INT; next(); any = 1; continue; }
        if (!strcmp(s, "float")) { base = TY_FLOAT; next(); any = 1; continue; }
        if (!strcmp(s, "double")) { base = TY_DOUBLE; next(); any = 1; continue; }
        if (!strcmp(s, "auto")) { ty = 0; base = -2; next(); any = 1; continue; }
        if (!strcmp(s, "struct") || !strcmp(s, "union") || !strcmp(s, "class")) { next(); ty = struct_decl(!strcmp(s, "union"), s[0] == 'c'); any = 1; continue; }
        if (!strcmp(s, "enum")) { next(); ty = enum_decl(); any = 1; continue; }
        if (!strcmp(s, "string")) { ty = ty_string; next(); any = 1; continue; }
        if (!strcmp(s, "ostream") || !strcmp(s, "ofstream") || !strcmp(s, "ostringstream")) { ty = ty_ostream; next(); any = 1; continue; }
        if (!strcmp(s, "istream") || !strcmp(s, "ifstream") || !strcmp(s, "istringstream")) { ty = ty_istream; next(); any = 1; continue; }
        if (!strcmp(s, "stringstream")) syntax("stringstream isn't available: use to_string and string's + instead");
        if (!strcmp(s, "size_t") || !strcmp(s, "uint64_t") || !strcmp(s, "uintptr_t")) { ty = ty_ulong; next(); any = 1; continue; }
        if (!strcmp(s, "int64_t") || !strcmp(s, "ssize_t") || !strcmp(s, "ptrdiff_t") || !strcmp(s, "intptr_t") || !strcmp(s, "time_t") || !strcmp(s, "clock_t")) { ty = ty_long; next(); any = 1; continue; }
        if (!strcmp(s, "int32_t")) { ty = ty_int; next(); any = 1; continue; }
        if (!strcmp(s, "uint32_t")) { ty = ty_uint; next(); any = 1; continue; }
        if (!strcmp(s, "int16_t")) { ty = ty_short; next(); any = 1; continue; }
        if (!strcmp(s, "uint16_t")) { ty = ty_ushort; next(); any = 1; continue; }
        if (!strcmp(s, "int8_t")) { ty = ty_char; next(); any = 1; continue; }
        if (!strcmp(s, "uint8_t")) { ty = ty_uchar; next(); any = 1; continue; }
        if (!strcmp(s, "FILE")) { ty = ty_long; next(); any = 1; continue; }
        if (!strcmp(s, "tuple")) syntax("tuple isn't available: use pair or a struct");
        if (!strcmp(s, "vector") || !strcmp(s, "map") || !strcmp(s, "set") || !strcmp(s, "pair") || !strcmp(s, "unordered_map") ||
            !strcmp(s, "unordered_set") || !strcmp(s, "multiset") || !strcmp(s, "multimap") || !strcmp(s, "function") ||
            !strcmp(s, "deque") || !strcmp(s, "list") || !strcmp(s, "array")) {
            if (!strcmp(s, "array")) {
                /* array<int, 5>: a vector of that many */
                next(); expect("<");
                CType *e = abstract_type(); expect(",");
                const_expr();
                expect_gt();
                ty = ty_vector(e); any = 1; continue;
            }
            next(); ty = std_template(s); any = 1; continue;
        }
        CType *bp = bound_param(s);
        if (bp) { ty = bp; next(); any = 1; continue; }
        Templ *tp = find_template(s);
        if (tp && tp->is_class) {
            /* inside the template itself, its bare name is the class being made */
            if (!at_k(1, "<") && cur_inst_type && !strcmp(cur_inst_base, s)) { next(); ty = cur_inst_type; any = 1; continue; }
            next(); ty = template_args_type(tp); any = 1; continue;
        }
        Var *v = find_var(s);
        if (v && v->is_typedef) {
            /* a class's own name, inside it, is the constructor: not a type there */
            if (cur_class && v->ty == cur_class && at_k(1, "(") && !cur_fn) break;
            ty = v->ty; next(); any = 1;
            /* Outer::Inner */
            continue;
        }
        break;
    }
    if (!any) { if (need) syntax("expected a type before '%.*s'", cur()->len, cur()->s); return 0; }
    if (!ty) {
        if (base == -2) return 0;                 /* auto: from the initializer */
        if (base == TY_VOID) ty = ty_void;
        else if (base == TY_BOOL) ty = ty_bool;
        else if (base == TY_CHAR) ty = uns ? ty_uchar : ty_char;
        else if (base == TY_FLOAT) ty = ty_float;
        else if (base == TY_DOUBLE) ty = ty_double;
        else if (shrt) ty = uns ? ty_ushort : ty_short;
        else if (lng) ty = uns ? ty_ulong : ty_long;
        else ty = uns ? ty_uint : ty_int;
        (void)sgn;
    }
    /* const is kept on numbers and pointers (to catch assignments); other types stay the one type */
    if (cnst && (ty_is_num(ty) || ty->kind == TY_PTR) && !ty->is_const) { CType *c = zalloc(sizeof(CType)); *c = *ty; c->is_const = 1; ty = c; }
    return ty;
}

/* the same type without const, so pointer comparisons work */
static CType *unconst(CType *t) {
    if (!t || !t->is_const) return t;
    switch (t->kind) {
    case TY_VOID: return ty_void; case TY_BOOL: return ty_bool; case TY_CHAR: return ty_char; case TY_UCHAR: return ty_uchar;
    case TY_SHORT: return ty_short; case TY_USHORT: return ty_ushort; case TY_INT: return ty_int; case TY_UINT: return ty_uint;
    case TY_LONG: return ty_long; case TY_ULONG: return ty_ulong; case TY_FLOAT: return ty_float; case TY_DOUBLE: return ty_double;
    case TY_STRING: return ty_string;
    }
    CType *c = zalloc(sizeof(CType)); *c = *t; c->is_const = 0;
    return c;
}

static CType *array_dims(CType *t) {
    if (!accept("[")) return t;
    int len = -1;
    if (!at("]")) len = (int)const_expr();
    expect("]");
    t = array_dims(t);
    return ty_array(t, len);
}

static CType *func_params(CType *ret, CFunc *proto);

/* *, &, the name, [] and () after a declaration's type */
static CType *declarator(CType *ty, char **name) {
    for (;;) {
        if (accept("*")) { ty = ty_ptr(ty); while (accept("const") || accept("volatile") || accept("restrict") || accept("__restrict")) { } continue; }
        if (accept("&")) { ty = ty_ref(ty); continue; }
        if (accept("&&")) { ty = ty_ref(ty); continue; }
        break;
    }
    if (at("(") && (at_k(1, "*") || at_k(1, "&"))) {
        /* (*name)(params): a pointer to a function, or to an array */
        next(); next();
        char *nm = 0;
        if (cur()->k == K_ID) nm = ident();
        expect(")");
        if (at("(")) { CType *ft = func_params(ty, 0); if (name) *name = nm; return ty_ptr(ft); }
        ty = array_dims(ty);
        if (name) *name = nm;
        return ty_ptr(ty);
    }
    if (cur()->k == K_ID && !strcmp(cur()->s, "operator") && name) {
        /* operator<< and friends, as a free function */
        next();
        char op[16] = "operator";
        if (at("(")) { next(); expect(")"); strcat(op, "()"); }
        else if (at("[")) { next(); expect("]"); strcat(op, "[]"); }
        else { strncat(op, cur()->s, (size_t)cur()->len); next(); }
        *name = sdup(op, (int)strlen(op));
    }
    else if (cur()->k == K_ID && (!is_type_name(cur()) || (name && at_k(1, "::"))) && strcmp(cur()->s, "const")) { if (name) *name = ident(); else ident(); }
    else if (name) *name = 0;
    /* Class::name for a method defined outside */
    while (name && *name && at("::") && peekt(1)->k == K_ID) { next(); char *b = rt_alloc_leaf(strlen(*name) + strlen(cur()->s) + 3); snprintf(b, strlen(*name) + strlen(cur()->s) + 3, "%s::%s", *name, cur()->s); *name = b; next(); }
    if (name && *name && at("::") && at_k(1, "~")) { next(); next(); char *dn = ident(); char *b = rt_alloc_leaf(strlen(*name) + strlen(dn) + 4); snprintf(b, strlen(*name) + strlen(dn) + 4, "%s::~%s", *name, dn); *name = b; }
    return array_dims(ty);
}

static CType *abstract_type(void) {
    CType *t = declspec(0, 0, 1);
    if (!t) syntax("auto isn't allowed here");
    for (;;) {
        if (accept("*")) { t = ty_ptr(t); while (accept("const")) { } continue; }
        if (accept("&") || accept("&&")) { t = ty_ref(t); continue; }
        break;
    }
    if (at("(") && at_k(1, "*")) { next(); next(); expect(")"); return ty_ptr(func_params(t, 0)); }
    return array_dims(t);
}

static CType *func_type(CType *ret) { CType *t = newty(TY_FUNC, 1, 1); t->ret = ret; return t; }

/* (int a, char *b, ...): the type, and with `proto` the names and defaults too */
static CType *func_params(CType *ret, CFunc *proto) {
    expect("(");
    CType *ft = func_type(ret);
    CType *ps[64]; char *nm[64]; CNode *df[64]; int n = 0;
    if (at("void") && at_k(1, ")")) next();
    while (!at(")")) {
        if (accept("...")) { ft->variadic = 1; break; }
        CType *bt = declspec(0, 0, 1);
        if (!bt) syntax("a parameter's type can't be auto");
        char *pn = 0;
        CType *t = declarator(bt, &pn);
        if (t->kind == TY_ARRAY) t = ty_ptr(t->base);
        if (t->kind == TY_FUNC) t = ty_ptr(t);
        CNode *d = 0;
        if (accept("=")) d = assign();
        if (n < 64) { ps[n] = t; nm[n] = pn; df[n] = d; n++; }
        if (!accept(",")) break;
    }
    expect(")");
    ft->params = zalloc(sizeof(CType *) * (size_t)(n ? n : 1));
    memcpy(ft->params, ps, sizeof(CType *) * (size_t)n);
    ft->nparams = n;
    if (proto) {
        proto->pnames = zalloc(sizeof(char *) * (size_t)(n ? n : 1));
        memcpy(proto->pnames, nm, sizeof(char *) * (size_t)n);
        proto->pdefault = zalloc(sizeof(CNode *) * (size_t)(n ? n : 1));
        memcpy(proto->pdefault, df, sizeof(CNode *) * (size_t)n);
    }
    /* const, override, noexcept, = 0 after it are for the caller to see */
    return ft;
}

/* ---- conversions -------------------------------------------------------------------------------------- */
static CNode *new_cast(CNode *e, CType *t);
static CType *rank_type(CType *t) {
    if (t->kind == TY_ENUM || t->kind == TY_BOOL || t->kind == TY_CHAR || t->kind == TY_UCHAR || t->kind == TY_SHORT || t->kind == TY_USHORT) return ty_int;
    return unconst(t);
}

/* where a reference is used, what it refers to */
static CNode *deref_ref(CNode *e) {
    if (e && e->ty && e->ty->kind == TY_REF) {
        CNode *d = node(C_DEREF); d->a = e; d->ty = e->ty->base; d->line = e->line;
        return d;
    }
    return e;
}
static CNode *decay(CNode *e) {
    e = deref_ref(e);
    if (e->ty->kind == TY_ARRAY) {
        CNode *a = node(C_ADDR); a->a = e; a->ty = ty_ptr(e->ty->base); a->line = e->line;
        return a;
    }
    if (e->ty->kind == TY_FUNC) { CNode *a = node(C_ADDR); a->a = e; a->ty = ty_ptr(e->ty); a->line = e->line; return a; }
    return e;
}

static CNode *new_cast(CNode *e, CType *t) {
    e = decay(e);
    t = unconst(t);
    if (e->ty == t) return e;
    if (e->k == C_NUM && ty_is_int(t)) {
        CNode *c = node(C_NUM); *c = *e; c->ty = t;
        switch (t->kind) {
        case TY_BOOL: c->ival = c->ival != 0; break;
        case TY_CHAR: c->ival = (int8_t)c->ival; break; case TY_UCHAR: c->ival = (uint8_t)c->ival; break;
        case TY_SHORT: c->ival = (int16_t)c->ival; break; case TY_USHORT: c->ival = (uint16_t)c->ival; break;
        case TY_INT: case TY_ENUM: c->ival = (int32_t)c->ival; break; case TY_UINT: c->ival = (uint32_t)c->ival; break;
        }
        return c;
    }
    if (e->k == C_NUM && ty_is_flt(t)) { CNode *c = node(C_FNUM); c->line = e->line; c->fval = is_unsigned(e->ty) ? (double)(uint64_t)e->ival : (double)e->ival; c->ty = t; if (t->kind == TY_FLOAT) c->fval = (double)(float)c->fval; return c; }
    if (e->k == C_FNUM && ty_is_flt(t)) { CNode *c = node(C_FNUM); *c = *e; c->ty = t; if (t->kind == TY_FLOAT) c->fval = (double)(float)c->fval; return c; }
    CNode *c = node(C_CAST); c->a = e; c->ty = t; c->line = e->line;
    return c;
}

static void usual(CNode **a, CNode **b) {
    *a = decay(*a); *b = decay(*b);
    CType *x = rank_type((*a)->ty), *y = rank_type((*b)->ty), *r;
    if (x->kind == TY_DOUBLE || y->kind == TY_DOUBLE) r = ty_double;
    else if (x->kind == TY_FLOAT || y->kind == TY_FLOAT) r = ty_float;
    else if (x->kind == TY_ULONG || y->kind == TY_ULONG) r = ty_ulong;
    else if (x->kind == TY_LONG || y->kind == TY_LONG) r = (x->kind == TY_UINT || y->kind == TY_UINT) ? ty_long : ty_long;
    else if (x->kind == TY_UINT || y->kind == TY_UINT) r = ty_uint;
    else r = ty_int;
    *a = new_cast(*a, r); *b = new_cast(*b, r);
}

static int is_lvalue(CNode *e) { return e->k == C_VAR || e->k == C_DEREF || e->k == C_MEMBER || (e->k == C_CALL && e->ty && e->ty->kind == TY_REF) || e->k == C_THIS; }

static void need_lvalue(CNode *e, const char *what) {
    if (!is_lvalue(e) && !(e->ty && is_obj(e->ty))) syntax("%s needs a variable (an lvalue) on its left", what);
}

/* the value converted to t, for an assignment, an argument or a return */
static CNode *convert(CNode *e, CType *t, const char *what) {
    char b1[80], b2[80];
    if (t->kind == TY_REF) {
        CType *bt = unconst(t->base);
        CNode *v = deref_ref(e);
        if (is_obj(bt) || is_lvalue(v)) {
            if (!is_lvalue(v) && !is_obj(v->ty)) syntax("%s: a reference needs a variable, not a value", what);
            /* a class's address for a base-class reference is the same address */
            CNode *a = node(C_ADDR); a->a = v; a->ty = ty_ptr(v->ty); a->line = e->line;
            CType *vt = unconst(v->ty);
            int same = vt == bt || (ty_is_num(bt) && ty_is_num(vt) && bt->size == vt->size && ty_is_flt(bt) == ty_is_flt(vt));
            if (!same && bt->kind == TY_STRUCT && vt->kind == TY_STRUCT) for (CType *s2 = vt->super; s2; s2 = s2->super) if (s2 == bt) same = 1;
            if (!same) {
                /* a T& from something else (a "literal" for a string&): bound to a converted temporary */
                CNode *c = node(C_COPY); c->a = convert(v, bt, what); c->ty = ty_ptr(bt); c->flag = 1; c->line = e->line;
                return c;
            }
            return a;
        }
        /* const int& x = 5: a temporary */
        CNode *c = node(C_COPY); c->a = convert(v, bt, what); c->ty = ty_ptr(bt); c->flag = 1; c->line = e->line;
        return c;
    }
    e = decay(e);
    CType *from = unconst(e->ty);
    t = unconst(t);
    if (from == t) return e;
    if (ty_is_num(t) && ty_is_num(from)) return new_cast(e, t);
    if (t->kind == TY_BOOL && (ty_is_ptr(from) || from->kind == TY_NULLPTR)) return new_cast(e, t);
    if (t->kind == TY_PTR && (from->kind == TY_PTR || from->kind == TY_NULLPTR)) { CNode *c = new_cast(e, t); return c; }
    if (t->kind == TY_PTR && ty_is_int(from) && e->k == C_NUM && e->ival == 0) return new_cast(e, t);
    if (t->kind == TY_PTR && ty_is_int(from)) return new_cast(e, t);           /* C allows it, with a warning */
    if (ty_is_int(t) && from->kind == TY_PTR) return new_cast(e, t);
    if (t->kind == TY_STRING && from->kind == TY_PTR && unconst(from->base)->kind == TY_CHAR) { CNode *c = node(C_CAST); c->a = e; c->ty = ty_string; c->line = e->line; return c; }
    if (t->kind == TY_STRING && from->kind == TY_CHAR) { CNode *c = node(C_CAST); c->a = e; c->ty = ty_string; c->flag = 1; c->line = e->line; return c; }
    if (t->kind == TY_LAMBDA && (from->kind == TY_LAMBDA || (from->kind == TY_PTR && from->base->kind == TY_FUNC))) { if (from->kind == TY_LAMBDA) return e; CNode *c = node(C_CAST); c->a = e; c->ty = t; return c; }
    if (t->kind == TY_PTR && t->base->kind == TY_FUNC && from->kind == TY_LAMBDA) { CNode *c = node(C_CAST); c->a = e; c->ty = t; return c; }
    if (t->kind == TY_STRUCT && from->kind == TY_STRUCT) {
        for (CType *s = from; s; s = s->super) if (s == t) { CNode *c = node(C_CAST); c->a = e; c->ty = t; c->flag = 2; return c; }
    }
    if (t->kind == from->kind && (t->kind == TY_VECTOR || t->kind == TY_MAP || t->kind == TY_SET || t->kind == TY_PAIR) && t == from) return e;
    if (t->kind == TY_PAIR && from->kind == TY_PAIR) {
        /* pair<int,int> from pair<int,long>: member by member */
        CNode *c = node(C_CAST); c->a = e; c->ty = t; c->flag = 3; return c;
    }
    if (t->kind == TY_STRUCT && e->k == C_INIT) { e->ty = t; return e; }
    syntax("%s: can't convert %s to %s", what, ty_str(from, b1, sizeof b1), ty_str(t, b2, sizeof b2));
}

/* ---- names in expressions ---------------------------------------------------------------------------- */
static CNode *var_node(Var *v) {
    CNode *n = node(C_VAR);
    n->ty = v->ty; n->off = v->off; n->global = !v->local; n->name = v->name;
    n->flag = lambda_depth - v->depth;          /* frames out, through lambdas */
    return n;
}

static CMember *find_member(CType *t, const char *name, CType **owner) {
    for (CType *c = t; c; c = c->super) {
        for (int i = 0; i < c->nmem; i++) if (!strcmp(c->mem[i].name, name)) { if (owner) *owner = c; return &c->mem[i]; }
    }
    return 0;
}

CFunc *cc_find_method(CType *cls, const char *name, int vid) {
    if (vid) { CType *dyn = cc_class_by_id(vid); if (dyn) cls = dyn; }
    for (CType *c = cls; c; c = c->super)
        for (int i = 0; i < c->nmeth; i++) if (!strcmp(c->meth[i]->name, name)) return c->meth[i];
    return 0;
}

static CNode *this_node(void) {
    CNode *n = node(C_THIS);
    n->ty = ty_ptr(cur_class);
    n->flag = lambda_depth;
    return n;
}

static CNode *member_of(CNode *obj, const char *name);
/* *this: the object a method runs on */
static CNode *self_obj(void) { CNode *t = this_node(); CNode *d = node(C_DEREF); d->a = t; d->ty = cur_class; return d; }
static CNode *call_args(CFunc *f, CNode *callee_obj, int is_method, CNode **args, int nargs);
static CNode *method_call(CNode *obj, const char *name);
static CNode *call_overloaded(CFunc *f, CNode *obj, int is_method);
static CNode *primary_template_call(Templ *tp);
static CType T_manip = { .kind = TY_INT, .size = 4, .align = 4 };
CType *ty_manip = &T_manip;

/* ---- expressions ---------------------------------------------------------------------------------------- */
static CNode *binop(int k, CNode *a, CNode *b, CType *t) {
    CNode *n = node(k); n->a = a; n->b = b; n->ty = t; n->line = a->line;
    return n;
}

static CNode *num(int64_t v, CType *t) { CNode *n = node(C_NUM); n->ival = v; n->ty = t; return n; }

static CNode *to_bool(CNode *e) {
    e = decay(e);
    if (e->ty->kind == TY_BOOL) return e;
    if (e->ty->kind == TY_STRUCT) {
        CFunc *op = cc_find_method(e->ty, "operator bool", 0);
        if (op) return call_args(op, e, 1, 0, 0);
        syntax("a %s can't be used as a condition", e->ty->name ? e->ty->name : "struct");
    }
    if (e->ty->kind == TY_STRING || e->ty->kind == TY_VECTOR) syntax("a container can't be used as a condition: use !x.empty()");
    if (e->ty->kind == TY_ISTREAM) { CNode *c = node(C_CAST); c->a = e; c->ty = ty_bool; return c; }
    return new_cast(e, ty_bool);
}

static CNode *user_op(const char *op, CNode *a, CNode *b) {
    /* a + b on a class: a.operator+(b), or operator+(a, b) */
    char nm[24]; snprintf(nm, sizeof nm, "operator%s", op);
    CNode *x = deref_ref(a);
    if (x->ty->kind == TY_STRUCT) {
        CFunc *f = cc_find_method(x->ty, nm, 0);
        if (f) { CNode *args[1] = { b }; return call_args(f, x, 1, args, b ? 1 : 0); }
    }
    Var *v = find_var(nm);
    if (v && v->fn) {
        CNode *args[2] = { a, b };
        /* the overload whose first parameter takes a */
        for (CFunc *f = v->fn; f; f = f->next_overload) {
            if (f->ty->nparams != (b ? 2 : 1)) continue;
            CType *p0 = f->ty->params[0]; if (p0->kind == TY_REF) p0 = p0->base;
            CType *at_ = deref_ref(a)->ty;
            if (unconst(p0) != unconst(at_) && !(p0->kind == TY_OSTREAM && at_->kind == TY_OSTREAM)) continue;
            if (b) {
                CType *p1 = f->ty->params[1]; if (p1->kind == TY_REF) p1 = p1->base;
                CType *bt = deref_ref(b)->ty;
                if (unconst(p1)->kind == TY_STRUCT && unconst(p1) != unconst(bt)) {
                    int ok = 0;
                    for (CType *s = bt; s; s = s->super) if (s == unconst(p1)) ok = 1;
                    if (!ok) continue;
                }
            }
            return call_args(f, 0, 0, args, b ? 2 : 1);
        }
    }
    return 0;
}

static CNode *arith(int k, const char *op, CNode *a, CNode *b) {
    CNode *u;
    CType *ta = deref_ref(a)->ty, *tb = deref_ref(b)->ty;
    if ((ta->kind == TY_STRUCT || tb->kind == TY_STRUCT || (ta->kind == TY_OSTREAM && tb->kind == TY_STRUCT)) && (u = user_op(op, a, b))) return u;
    if (ta->kind == TY_STRING || tb->kind == TY_STRING || ta->kind == TY_VECTOR || tb->kind == TY_VECTOR || ta->kind == TY_PAIR) {
        /* string + string, string + char*, string == ..., vectors and pairs compared */
        a = deref_ref(a); b = deref_ref(b);
        CNode *n = node(C_BUILTIN);
        n->op = k; n->a = a; n->b = b; n->flag = 1;                         /* flag 1: an operator on objects */
        n->ty = k == C_ADD ? ty_string : ty_bool;
        if (k == C_ADD && ta->kind != TY_STRING && tb->kind != TY_STRING) syntax("can't add those");
        if (k != C_ADD && k != C_EQ && k != C_NE && k != C_LT && k != C_LE && k != C_GT && k != C_GE) syntax("'%s' doesn't work on strings", op);
        return n;
    }
    a = decay(a); b = decay(b);
    if (k == C_ADD || k == C_SUB) {
        if (ty_is_ptr(a->ty) && ty_is_int(b->ty)) { CNode *n = binop(C_PTRADD, a, new_cast(b, ty_long), a->ty); if (k == C_SUB) { n->b = binop(C_NEG, n->b, 0, ty_long); } return n; }
        if (k == C_ADD && ty_is_int(a->ty) && ty_is_ptr(b->ty)) return binop(C_PTRADD, b, new_cast(a, ty_long), b->ty);
        if (k == C_SUB && ty_is_ptr(a->ty) && ty_is_ptr(b->ty)) { CNode *n = binop(C_PTRDIFF, a, b, ty_long); n->ival = a->ty->base->size ? a->ty->base->size : 1; return n; }
    }
    if (k >= C_EQ && k <= C_GE && (ty_is_ptr(a->ty) || ty_is_ptr(b->ty) || a->ty->kind == TY_NULLPTR || b->ty->kind == TY_NULLPTR)) {
        return binop(k, new_cast(a, ty_ulong), new_cast(b, ty_ulong), ty_bool);
    }
    if (!ty_is_num(a->ty) || !ty_is_num(b->ty)) {
        char b1[80], b2[80];
        syntax("'%s' doesn't work on %s and %s", op, ty_str(a->ty, b1, sizeof b1), ty_str(b->ty, b2, sizeof b2));
    }
    if (k == C_SHL || k == C_SHR) {
        a = new_cast(a, rank_type(a->ty)); b = new_cast(b, ty_long);
        return binop(k, a, b, a->ty);
    }
    usual(&a, &b);
    if ((k == C_MOD || k == C_BAND || k == C_BOR || k == C_BXOR) && ty_is_flt(a->ty)) syntax("'%s' needs integers, not floating point", op);
    if (k >= C_EQ && k <= C_GE) return binop(k, a, b, ty_bool);
    if (a->k == C_NUM && b->k == C_NUM && k != C_DIV && k != C_MOD) {
        int64_t x = a->ival, y = b->ival, r = 0;
        switch (k) {
        case C_ADD: r = (int64_t)((uint64_t)x + (uint64_t)y); break; case C_SUB: r = (int64_t)((uint64_t)x - (uint64_t)y); break;
        case C_MUL: r = (int64_t)((uint64_t)x * (uint64_t)y); break; case C_BAND: r = x & y; break; case C_BOR: r = x | y; break;
        case C_BXOR: r = x ^ y; break; default: goto no_fold;
        }
        return new_cast(num(r, ty_long), a->ty);
    }
no_fold:
    return binop(k, a, b, a->ty);
}

/* x op= y, and x = y */
static CNode *assignment(CNode *lhs, int op, const char *opname, CNode *rhs) {
    lhs = deref_ref(lhs);
    if (lhs->ty->kind == TY_STRUCT && !op) {
        CFunc *f = cc_find_method(lhs->ty, "operator=", 0);
        if (f) { CNode *args[1] = { rhs }; return call_args(f, lhs, 1, args, 1); }
    }
    if (lhs->ty->kind == TY_STRUCT && op) {
        char nm[24]; snprintf(nm, sizeof nm, "%s=", opname);
        CNode *u = user_op(nm, lhs, rhs);
        if (u) return u;
        u = arith(op, opname, lhs, rhs);
        return assignment(lhs, 0, "=", u);
    }
    need_lvalue(lhs, "assignment");
    if (lhs->ty->is_const && lhs->k == C_VAR) syntax("'%s' is const: it can't be changed", lhs->name ? lhs->name : "it");
    if (lhs->ty->kind == TY_ARRAY) syntax("an array can't be assigned to: copy it element by element (or memcpy)");
    CNode *n = node(C_ASSIGN);
    n->a = lhs; n->ty = lhs->ty; n->line = lhs->line;
    if (op) {
        if (lhs->ty->kind == TY_STRING && op == C_ADD) { n->op = C_ADD; n->b = deref_ref(rhs); return n; }   /* s += x */
        /* a op= b is a = a op b, with a worked out once (the evaluator keeps its address) */
        CNode *v = node(C_DEREF); v->ty = lhs->ty; v->flag = 1;   /* flag 1: the address the assignment found */
        CNode *r = arith(op, opname, v, rhs);
        n->b = convert(r, lhs->ty, "assignment");
        n->op = op;
        return n;
    }
    n->b = convert(rhs, lhs->ty, "assignment");
    return n;
}

static CNode *postfix(void);
static CNode *unary(void);
static CNode *cast_expr(void);
static CNode *parse_lambda(void);

static CNode *initializer_list(CType *t);
static CNode *typed_init(CType *t, CNode *src);

static CNode *primary(void) {
    Tok *t = cur();
    if (accept("(")) {
        CNode *e = expr();
        expect(")");
        return e;
    }
    if (t->k == K_NUM) { next(); return num(t->ival, t->ty); }
    if (t->k == K_FNUM) { next(); CNode *n = node(C_FNUM); n->fval = t->fval; n->ty = t->ty; return n; }
    if (t->k == K_STR) {
        /* "a" "b" joined; kept in the data segment */
        char *buf = 0; int len = 0;
        while (cur()->k == K_STR) {
            buf = rt_realloc(buf, (size_t)len, (size_t)(len + cur()->len + 1));
            memcpy(buf + len, cur()->s, (size_t)cur()->len); len += cur()->len;
            next();
        }
        uint64_t a = alloc_global(len + 1, 1);
        memcpy(cc_mem + a, buf ? buf : "", (size_t)len); cc_mem[a + len] = 0;
        CNode *n = node(C_STR); n->off = (int64_t)a; n->ival = len;
        n->ty = ty_array(ty_char, len + 1);
        return n;
    }
    if (t->k == K_ID) {
        const char *s = t->s;
        if (!strcmp(s, "sizeof")) {
            next();
            CType *st;
            if (at("(") && is_type_name(peekt(1))) { next(); st = abstract_type(); expect(")"); }
            else { CNode *e = unary(); st = deref_ref(e)->ty; }
            if (st->size <= 0 && st->kind == TY_ARRAY) syntax("sizeof of an array whose size isn't known");
            return num(st->kind == TY_REF ? st->base->size : st->size, ty_ulong);
        }
        if (!strcmp(s, "true") || !strcmp(s, "false")) { next(); return num(s[0] == 't', ty_bool); }
        if (!strcmp(s, "nullptr") || !strcmp(s, "NULL")) { next(); CNode *n = num(0, is_cpp ? ty_nullptr : ty_ptr(ty_void)); return n; }
        if (!strcmp(s, "this")) { next(); if (!cur_class) syntax("'this' outside a class"); return this_node(); }
        if (!strcmp(s, "std") && at_k(1, "::")) { next(); next(); return primary(); }
        if (is_cpp && (!strcmp(s, "static_cast") || !strcmp(s, "reinterpret_cast") || !strcmp(s, "const_cast") || !strcmp(s, "dynamic_cast"))) {
            next(); expect("<"); CType *ct = abstract_type(); expect_gt(); expect("(");
            CNode *e = expr(); expect(")");
            if (ct->kind == TY_REF) return e;
            return new_cast(e, ct);
        }
        /* a type used as a function: int(x), string("a"), Point(1, 2), vector<int>(5) */
        if (is_type_name(t) && strcmp(s, "auto")) {
            int save = pos;
            CType *ct = declspec(0, 0, 0);
            if (ct && (at("(") || at("{"))) {
                int brace = at("{");
                next();
                CNode *args[32]; int na = 0;
                while (!at(brace ? "}" : ")")) { if (na < 32) args[na++] = assign(); if (!accept(",")) break; }
                expect(brace ? "}" : ")");
                if (ty_is_num(ct) || ct->kind == TY_PTR) { if (!na) return new_cast(num(0, ty_int), ct); return new_cast(args[0], ct); }
                CNode *n = node(C_INIT); n->ty = ct; n->flag = brace;
                for (int i = 0; i < na; i++) add_kid(n, args[i]);
                CNode *ti = typed_init(ct, n);
                if (ti->k != C_INIT) {
                    /* string("x") and the like: a value of the type, as a temporary */
                    CNode *c = node(C_COPY); c->a = ti; c->ty = ty_ptr(ct); c->flag = 1;
                    CNode *d = node(C_DEREF); d->a = c; d->ty = ct;
                    return d;
                }
                return ti;
            }
            pos = save;
        }
        if (at_k(1, "::") && find_var(s) && find_var(s)->is_typedef) {
            /* Class::static_member or Class::method */
            CType *ct = find_var(s)->ty;
            next(); next();
            char *mn = ident();
            char full[160]; snprintf(full, sizeof full, "%s::%s", ct->name, mn);
            Var *v = find_var(full);
            if (v) { if (v->fn && at("(")) return call_overloaded(v->fn, 0, 0); return var_node(v); }
            CFunc *f = cc_find_method(ct, mn, 0);
            if (f && cur_class && at("(")) return call_overloaded(f, self_obj(), 1);
            syntax("%s has no member '%s'", ct->name, mn);
        }
        if (!strcmp(s, "new")) {
            next();
            CType *nt = declspec(0, 0, 1);
            while (accept("*")) nt = ty_ptr(nt);
            CNode *n = node(C_NEW); n->ty = ty_ptr(nt);
            if (accept("[")) { n->a = new_cast(expr(), ty_long); expect("]"); n->flag = 1; return n; }
            CNode *init = node(C_INIT); init->ty = nt;
            if (accept("(")) { while (!at(")")) { add_kid(init, assign()); if (!accept(",")) break; } expect(")"); }
            else if (accept("{")) { init->flag = 1; while (!at("}")) { add_kid(init, assign()); if (!accept(",")) break; } expect("}"); }
            n->b = init->nk || nt->kind == TY_STRUCT || is_obj(nt) ? typed_init(nt, init) : 0;
            return n;
        }
        if (!strcmp(s, "delete")) {
            next();
            int arr = 0;
            if (accept("[")) { expect("]"); arr = 1; }
            CNode *n = node(C_DELETE); n->a = decay(cast_expr()); n->flag = arr; n->ty = ty_void;
            if (n->a->ty->kind != TY_PTR) syntax("delete needs a pointer");
            return n;
        }
        if (!strcmp(s, "throw")) {
            next();
            CNode *n = node(C_THROW); n->ty = ty_void;
            if (!at(";")) n->a = assign();
            return n;
        }
        if (!strcmp(s, "cout") || !strcmp(s, "cerr") || !strcmp(s, "clog")) { next(); CNode *n = node(C_NUM); n->ty = ty_ostream; n->ival = s[1] == 'o' ? 1 : 2; return n; }
        if (!strcmp(s, "cin")) { next(); CNode *n = node(C_NUM); n->ty = ty_istream; return n; }
        if ((!strcmp(s, "ios") || !strcmp(s, "ios_base")) && at_k(1, "::")) {
            /* ios::fixed and the like, for setf and unsetf */
            next(); next();
            char *f = ident();
            static const char *const flags[] = { "fixed", "scientific", "boolalpha", "left", "right", "showpoint", "floatfield", "adjustfield", 0 };
            for (int i = 0; flags[i]; i++) if (!strcmp(f, flags[i])) return num(i + 1, ty_int);
            return num(0, ty_int);
        }
        {
            /* iostream manipulators: endl, fixed, setw(n), setprecision(n), ... */
            static const char *const manip[] = { "endl", "fixed", "scientific", "boolalpha", "noboolalpha", "left", "right",
                "hex", "dec", "oct", "showpoint", "flush", "ws", "defaultfloat", "setw", "setprecision", "setfill", 0 };
            for (int i = 0; manip[i]; i++) if (!strcmp(s, manip[i]) && !find_var(s)) {
                next();
                CNode *n = node(C_NUM); n->ty = ty_manip; n->ival = (int64_t)i << 32;
                if (i >= 14) { expect("("); CNode *a = new_cast(assign(), ty_int); expect(")"); CNode *b = node(C_BUILTIN); b->op = -1; b->a = a; b->ival = i; b->ty = ty_manip; return b; }
                return n;
            }
        }
        next();
        Var *v = find_var(s);
        if (!v && cur_class) {
            /* a member of this class, without this-> */
            CType *own;
            CMember *m = find_member(cur_class, s, &own);
            if (m) return member_of(self_obj(), s);
            CFunc *f = cc_find_method(cur_class, s, 0);
            if (f && at("(")) return call_overloaded(f, self_obj(), 1);
            char full[160]; snprintf(full, sizeof full, "%s::%s", cur_class->name, s);
            Var *sv = find_var(full);
            if (sv) { if (sv->fn && at("(")) return call_overloaded(sv->fn, 0, 0); return var_node(sv); }
        }
        if (!v) {
            Templ *tp = find_template(s);
            if (tp && !tp->is_class) {
                pos--;
                return primary_template_call(tp);
            }
            int lib = cc_lib_find(s);
            if (lib) {
                CNode *n = node(C_BUILTIN); n->op = lib; n->name = (char *)s; n->ty = cc_lib_type(lib)->ret;
                if (!at("(")) { CNode *fa = node(C_FUNCADDR); fa->ival = -lib; fa->ty = ty_ptr(cc_lib_type(lib)); return fa; }
                next();
                while (!at(")")) { add_kid(n, at("{") ? initializer_list(0) : assign()); if (!accept(",")) break; }
                expect(")");
                for (int i = 0; i < n->nk; i++) if (n->kids[i]->k != C_INIT) n->kids[i] = deref_ref(n->kids[i]);
                cc_lib_fix_type(n);
                return n;
            }
            pos--;
            syntax("'%s' isn't declared here", s);
        }
        if (v->is_enum) return num(v->ival, ty_int);
        if (v->is_typedef) syntax("'%s' is a type", s);
        if (v->fn) {
            if (at("(")) return call_overloaded(v->fn, 0, 0);
            CNode *n = node(C_FUNCADDR); n->fn = v->fn; n->ty = ty_ptr(v->fn->ty);
            return n;
        }
        return var_node(v);
    }
    if (at("[") && is_cpp) return parse_lambda();
    if (at("{") && is_cpp) {
        /* {1, 2}: the type comes from where it goes */
        return initializer_list(0);
    }
    if (t->k == K_EOF) syntax("the file ends in the middle of an expression");
    syntax("expected an expression before '%.*s'", t->len, t->s);
}

/* ---- members, methods and calls ------------------------------------------------------------------------ */
static CNode *member_of(CNode *obj, const char *name) {
    obj = deref_ref(obj);
    CType *t = obj->ty;
    if (t->kind != TY_STRUCT && t->kind != TY_PAIR) {
        char b[80];
        syntax("a %s has no member '%s'", ty_str(t, b, sizeof b), name);
    }
    CMember *m = find_member(t, name, 0);
    if (!m) syntax("%s has no member named '%s'", t->name ? t->name : "the struct", name);
    CNode *n = node(C_MEMBER);
    n->a = obj; n->off = m->offset; n->ty = m->ty; n->name = m->name;
    return n;
}

/* how well an argument fits a parameter: higher is better, -1 not at all */
static int fit(CNode *arg, CType *p) {
    CType *a = deref_ref(arg)->ty;
    if (p->kind == TY_REF) p = p->base;
    p = unconst(p); a = unconst(a);
    if (a == p) return 10;
    if (a->kind == TY_ARRAY && p->kind == TY_PTR && unconst(a->base) == unconst(p->base)) return 9;
    if (ty_is_int(a) && ty_is_int(p)) return a->size == p->size ? 8 : 6;
    if (ty_is_flt(a) && ty_is_flt(p)) return 7;
    if (ty_is_num(a) && ty_is_num(p)) return 4;
    if (p->kind == TY_STRING && (a->kind == TY_ARRAY || a->kind == TY_PTR) && unconst(a->base)->kind == TY_CHAR) return 5;
    if (p->kind == TY_PTR && (a->kind == TY_PTR || a->kind == TY_NULLPTR || a->kind == TY_ARRAY)) return 3;
    if (p->kind == TY_STRUCT && a->kind == TY_STRUCT) { for (CType *s = a->super; s; s = s->super) if (s == p) return 5; return -1; }
    if (p->kind == TY_LAMBDA && (a->kind == TY_LAMBDA || a->kind == TY_PTR)) return 5;
    if (p->kind == a->kind && (p->kind == TY_VECTOR || p->kind == TY_MAP || p->kind == TY_SET || p->kind == TY_PAIR)) return p == a ? 10 : 2;
    if (arg->k == C_INIT) return 2;
    return -1;
}

static CFunc *choose(CFunc *f, CNode **args, int n) {
    CFunc *best = 0; int bs = -1;
    for (CFunc *g = f; g; g = g->next_overload) {
        CType *ft = g->ty;
        int need = 0;
        for (int i = 0; i < ft->nparams; i++) if (!g->pdefault || !g->pdefault[i]) need = i + 1;
        if (n < need || (n > ft->nparams && !ft->variadic)) continue;
        int sc = 1;
        for (int i = 0; i < n && i < ft->nparams; i++) {
            int x = fit(args[i], ft->params[i]);
            if (x < 0) { sc = -1; break; }
            sc += x;
        }
        if (sc > bs) { bs = sc; best = g; }
    }
    return best;
}

static CNode *call_args(CFunc *f, CNode *obj, int is_method, CNode **args, int nargs) {
    CType *ft = f->ty;
    CNode *n = node(is_method ? C_METHOD : C_CALL);
    n->fn = f;
    if (is_method) {
        n->a = deref_ref(obj);
        n->flag = f->is_virtual;
    }
    char what[120];
    for (int i = 0; i < ft->nparams; i++) {
        snprintf(what, sizeof what, "argument %d of %s()", i + 1, f->name);
        if (i < nargs) add_kid(n, convert(args[i], ft->params[i], what));
        else if (f->pdefault && f->pdefault[i]) add_kid(n, convert(f->pdefault[i], ft->params[i], what));
        else syntax("too few arguments to %s()", f->name);
    }
    if (nargs > ft->nparams) {
        if (!ft->variadic) syntax("too many arguments to %s()", f->name);
        for (int i = ft->nparams; i < nargs; i++) {
            CNode *a = decay(args[i]);
            if (a->ty->kind == TY_FLOAT) a = new_cast(a, ty_double);
            else if (ty_is_int(a->ty) && a->ty->size < 4) a = new_cast(a, ty_int);
            add_kid(n, a);
        }
    }
    n->ty = ft->ret;
    return n;
}

static int parse_args(CNode **args, int cap) {
    expect("(");
    int n = 0;
    while (!at(")")) {
        CNode *a = assign();
        if (n < cap) args[n++] = a;
        if (!accept(",")) break;
    }
    expect(")");
    return n;
}

static CNode *call_overloaded(CFunc *f, CNode *obj, int is_method) {
    CNode *args[32];
    int line = cur()->line;
    int n = parse_args(args, 32);
    CFunc *g = choose(f, args, n);
    if (!g) { cc_line = line; syntax("no version of %s() takes these %d argument%s", f->name, n, n == 1 ? "" : "s"); }
    if (is_method && g->is_static) is_method = 0;
    if (!is_method && g->owner && !g->is_static && cur_class) { obj = self_obj(); is_method = 1; }
    return call_args(g, obj, is_method, args, n);
}

/* obj.name(...) on the library's types: string, vector, map, set */
static CNode *lib_method(CNode *obj, const char *name) {
    CNode *n = node(C_BUILTIN);
    n->a = obj; n->name = (char *)name; n->flag = 2;          /* flag 2: a container's method */
    CType *t = obj->ty;
    if (at("(")) {
        next();
        while (!at(")")) { add_kid(n, at("{") ? initializer_list(0) : deref_ref(assign())); if (!accept(",")) break; }
        expect(")");
    }
    CType *e = t->base;
    int k = t->kind;
    const char *s = name;
    #define IS(x) (!strcmp(s, x))
    if (IS("size") || IS("length") || IS("capacity") || IS("count")) n->ty = ty_ulong;
    else if (IS("empty") || IS("contains")) n->ty = ty_bool;
    else if (IS("clear") || IS("push_back") || IS("pop_back") || IS("emplace_back") || IS("resize") || IS("reserve") || IS("assign") || IS("swap") || IS("shrink_to_fit") || IS("push_front") || IS("pop_front") || IS("emplace_front")) n->ty = ty_void;
    else if (k == TY_STRING) {
        if (IS("c_str") || IS("data")) n->ty = ty_ptr(ty_char);
        else if (IS("substr") || IS("append") || IS("insert") || IS("erase") || IS("replace")) n->ty = IS("substr") ? ty_string : ty_ref(ty_string);
        else if (IS("find") || IS("rfind") || IS("find_first_of") || IS("find_last_of") || IS("find_first_not_of") || IS("find_last_not_of")) n->ty = ty_ulong;
        else if (IS("at") || IS("front") || IS("back")) n->ty = ty_ref(ty_char);
        else if (IS("begin") || IS("end")) n->ty = ty_ptr(ty_char);
        else if (IS("compare")) n->ty = ty_int;
        else syntax("string has no method '%s'", s);
    } else if (k == TY_VECTOR) {
        if (IS("at") || IS("front") || IS("back")) n->ty = ty_ref(e);
        else if (IS("begin") || IS("end") || IS("data") || IS("insert") || IS("erase") || IS("rbegin") || IS("rend")) n->ty = ty_ptr(e);
        else syntax("vector has no method '%s'", s);
    } else if (k == TY_MAP || k == TY_SET) {
        CType *el = k == TY_MAP ? ty_pair(t->base, t->base2) : e;
        if (IS("find") || IS("begin") || IS("end") || IS("lower_bound") || IS("upper_bound") || IS("rbegin") || IS("rend")) n->ty = ty_ptr(el);
        else if (IS("insert") || IS("emplace")) n->ty = ty_bool;
        else if (IS("erase")) n->ty = ty_ulong;
        else if (IS("at") && k == TY_MAP) n->ty = ty_ref(t->base2);
        else syntax("%s has no method '%s'", k == TY_MAP ? "map" : "set", s);
    } else syntax("no method '%s' here", s);
    #undef IS
    return n;
}

static CNode *method_call(CNode *obj, const char *name) {
    obj = deref_ref(obj);
    CType *t = obj->ty;
    if (t->kind == TY_STRING || t->kind == TY_VECTOR || t->kind == TY_MAP || t->kind == TY_SET) return lib_method(obj, name);
    if (t->kind == TY_STRUCT) {
        CFunc *f = cc_find_method(t, name, 0);
        if (!f) syntax("%s has no method '%s'", t->name, name);
        return call_overloaded(f, obj, 1);
    }
    if (t->kind == TY_ISTREAM || t->kind == TY_OSTREAM) {
        /* cin.ignore(), cin.get(), cout.precision(): taken as calls */
        CNode *n = node(C_BUILTIN); n->name = (char *)name; n->flag = 5; n->a = obj; n->ty = ty_int;
        if (at("(")) { next(); while (!at(")")) { add_kid(n, deref_ref(assign())); if (!accept(",")) break; } expect(")"); }
        return n;
    }
    char b[80];
    syntax("a %s has no methods", ty_str(t, b, sizeof b));
}

static CNode *postfix(void) {
    CNode *e = primary();
    for (;;) {
        int line = cur()->line;
        if (accept("[")) {
            CNode *ix = expr();
            expect("]");
            CNode *o = deref_ref(e);
            if (o->ty->kind == TY_STRUCT) {
                CNode *u = user_op("[]", o, ix);
                if (!u) syntax("%s has no operator[]", o->ty->name);
                e = u; continue;
            }
            if (o->ty->kind == TY_STRING || o->ty->kind == TY_VECTOR || o->ty->kind == TY_MAP) {
                CNode *n = node(C_BUILTIN); n->flag = 6; n->a = o; n->b = o->ty->kind == TY_MAP ? convert(ix, o->ty->base, "the key") : new_cast(ix, ty_long);
                n->ty = o->ty->kind == TY_STRING ? ty_ref(ty_char) : o->ty->kind == TY_VECTOR ? ty_ref(o->ty->base) : ty_ref(o->ty->base2);
                n->line = line;
                e = n; continue;
            }
            CNode *a = decay(o), *b = decay(ix);
            if (ty_is_int(a->ty) && ty_is_ptr(b->ty)) { CNode *t = a; a = b; b = t; }
            if (!ty_is_ptr(a->ty)) syntax("subscripted value is not an array or a pointer");
            CNode *d = node(C_DEREF); d->a = binop(C_PTRADD, a, new_cast(b, ty_long), a->ty); d->ty = a->ty->base; d->line = line;
            e = d;
            continue;
        }
        if (at("(")) {
            /* a call through a pointer or a lambda */
            CNode *f = deref_ref(e);
            if (f->ty->kind == TY_STRUCT) {
                CFunc *op = cc_find_method(f->ty, "operator()", 0);
                if (!op) syntax("%s can't be called", f->ty->name);
                e = call_overloaded(op, f, 1);
                continue;
            }
            f = decay(f);
            CType *ft = f->ty->kind == TY_PTR && f->ty->base->kind == TY_FUNC ? f->ty->base : f->ty->kind == TY_LAMBDA ? f->ty : 0;
            if (!ft) syntax("called object is not a function");
            CNode *args[32]; int n = parse_args(args, 32);
            CNode *c = node(C_CALL); c->a = f; c->line = line;
            for (int i = 0; i < n; i++) add_kid(c, i < ft->nparams ? convert(args[i], ft->params[i], "an argument") : decay(args[i]));
            c->ty = ft->ret ? ft->ret : ty_int;
            if (ft->kind == TY_LAMBDA && !ft->ret) c->ty = ty_int;
            e = c;
            continue;
        }
        if (at(".") || at("->")) {
            int arrow = at("->");
            next();
            CNode *o = deref_ref(e);
            if (arrow) {
                o = decay(o);
                if (o->ty->kind == TY_STRUCT && cc_find_method(o->ty, "operator->", 0)) syntax("operator-> isn't supported");
                if (o->ty->kind != TY_PTR) syntax("'->' needs a pointer");
                CNode *d = node(C_DEREF); d->a = o; d->ty = o->ty->base; d->line = line;
                o = d;
            }
            if (accept("~")) { ident(); expect("("); expect(")"); CNode *n = node(C_NOP); n->ty = ty_void; e = n; continue; }
            char *nm = ident();
            if (o->ty->kind == TY_PAIR || (o->ty->kind == TY_STRUCT && !at("("))) {
                if (o->ty->kind == TY_STRUCT && !find_member(o->ty, nm, 0) && cc_find_method(o->ty, nm, 0)) syntax("%s is a method: call it with ()", nm);
                e = member_of(o, nm);
                continue;
            }
            e = method_call(o, nm);
            continue;
        }
        if (at("++") || at("--")) {
            int inc = at("++");
            next();
            CNode *o = deref_ref(e);
            if (o->ty->kind == TY_STRUCT) {
                CNode *u = user_op(inc ? "++" : "--", o, num(0, ty_int));
                if (!u) syntax("%s has no operator%s", o->ty->name, inc ? "++" : "--");
                e = u; continue;
            }
            need_lvalue(o, inc ? "++" : "--");
            CNode *n = node(C_POSTINC); n->a = o; n->ty = o->ty; n->ival = inc ? 1 : -1;
            if (o->ty->kind == TY_PTR) n->ival *= o->ty->base->size ? o->ty->base->size : 1;
            e = n;
            continue;
        }
        if (at("::") && e->k == C_NOP) { next(); continue; }
        return e;
    }
}

static CNode *unary(void) {
    int line = cur()->line;
    if (accept("+")) return decay(cast_expr());
    if (accept("-")) {
        CNode *e = deref_ref(cast_expr());
        if (e->ty->kind == TY_STRUCT) { CNode *u = user_op("-", e, 0); if (u) return u; }
        e = decay(e);
        if (!ty_is_num(e->ty)) syntax("unary '-' needs a number");
        if (e->k == C_NUM) return new_cast(num(-e->ival, rank_type(e->ty)), rank_type(e->ty));
        if (e->k == C_FNUM) { CNode *f = node(C_FNUM); *f = *e; f->fval = -f->fval; return f; }
        e = new_cast(e, rank_type(e->ty));
        return binop(C_NEG, e, 0, e->ty);
    }
    if (accept("!")) {
        CNode *e = deref_ref(cast_expr());
        if (e->ty->kind == TY_STRUCT) { CNode *u = user_op("!", e, 0); if (u) return u; }
        return binop(C_NOT, to_bool(e), 0, is_cpp ? ty_bool : ty_int);
    }
    if (accept("~")) { CNode *e = decay(cast_expr()); if (!ty_is_int(e->ty)) syntax("'~' needs an integer"); e = new_cast(e, rank_type(e->ty)); return binop(C_BITNOT, e, 0, e->ty); }
    if (accept("*")) {
        CNode *e = deref_ref(cast_expr());
        if (e->ty->kind == TY_STRUCT) { CNode *u = user_op("*", e, 0); if (u) return u; }
        e = decay(e);
        if (e->ty->kind != TY_PTR) syntax("'*' needs a pointer");
        if (e->ty->base->kind == TY_FUNC) return e;
        if (e->ty->base->kind == TY_VOID) syntax("a void* can't be dereferenced: cast it first");
        CNode *d = node(C_DEREF); d->a = e; d->ty = e->ty->base; d->line = line;
        return d;
    }
    if (accept("&")) {
        CNode *e = deref_ref(cast_expr());
        if (e->k == C_FUNCADDR) return e;
        if (e->ty->kind == TY_FUNC) { CNode *a = node(C_ADDR); a->a = e; a->ty = ty_ptr(e->ty); return a; }
        if (!is_lvalue(e) && !is_obj(e->ty)) syntax("'&' needs a variable");
        CNode *a = node(C_ADDR); a->a = e; a->ty = ty_ptr(e->ty); a->line = line;
        return a;
    }
    if (at("++") || at("--")) {
        int inc = at("++");
        next();
        CNode *e = deref_ref(unary());
        if (e->ty->kind == TY_STRUCT) {
            CNode *u = user_op(inc ? "++" : "--", e, 0);
            if (!u) syntax("%s has no operator%s", e->ty->name, inc ? "++" : "--");
            return u;
        }
        need_lvalue(e, inc ? "++" : "--");
        CNode *n = node(C_PREINC); n->a = e; n->ty = e->ty; n->ival = inc ? 1 : -1;
        if (e->ty->kind == TY_PTR) n->ival *= e->ty->base->size ? e->ty->base->size : 1;
        return n;
    }
    return postfix();
}

static CNode *cast_expr(void) {
    if (at("(") && is_type_name(peekt(1)) && !(peekt(1)->k == K_ID && find_var(peekt(1)->s) && !find_var(peekt(1)->s)->is_typedef)) {
        int save = pos;
        next();
        CType *t = declspec(0, 0, 0);
        if (t) {
            while (accept("*")) t = ty_ptr(t);
            if (accept("&")) t = ty_ref(t);
            if (accept(")")) {
                if (at("{")) return initializer_list(t);       /* (struct P){1, 2} */
                CNode *e = cast_expr();
                if (t->kind == TY_REF) return e;
                if (t->kind == TY_VOID) { CNode *c = node(C_CAST); c->a = decay(e); c->ty = ty_void; return c; }
                if (t->kind == TY_STRUCT || t->kind == TY_STRING) return convert(e, t, "the cast");
                return new_cast(e, t);
            }
        }
        pos = save;
    }
    return unary();
}

static CNode *mul(void) {
    CNode *e = cast_expr();
    for (;;) {
        if (accept("*")) e = arith(C_MUL, "*", e, cast_expr());
        else if (accept("/")) e = arith(C_DIV, "/", e, cast_expr());
        else if (accept("%")) e = arith(C_MOD, "%", e, cast_expr());
        else return e;
    }
}
static CNode *add(void) {
    CNode *e = mul();
    for (;;) {
        if (accept("+")) e = arith(C_ADD, "+", e, mul());
        else if (accept("-")) e = arith(C_SUB, "-", e, mul());
        else return e;
    }
}

/* cout << x, cin >> x */
static CNode *stream_op(CNode *s, CNode *v, int out) {
    CNode *vv = deref_ref(v);
    if (out && vv->ty->kind == TY_STRUCT) {
        CNode *u = user_op("<<", s, v);
        if (u) return u;
        syntax("no operator<< prints a %s", vv->ty->name);
    }
    if (!out && vv->ty->kind == TY_STRUCT) {
        CNode *u = user_op(">>", s, v);
        if (u) return u;
    }
    CNode *n = node(C_BUILTIN);
    n->flag = out ? 3 : 4; n->a = s; n->b = out ? decay(vv) : vv; n->ty = out ? ty_ostream : ty_istream;
    if (!out && !is_lvalue(vv) && !is_obj(vv->ty)) syntax("cin >> needs a variable");
    return n;
}

static CNode *shift(void) {
    CNode *e = add();
    for (;;) {
        if (at("<<")) {
            next();
            if (deref_ref(e)->ty->kind == TY_OSTREAM) { e = stream_op(e, add(), 1); continue; }
            e = arith(C_SHL, "<<", e, add());
        } else if (at(">>") && !split_gt) {
            next();
            if (deref_ref(e)->ty->kind == TY_ISTREAM) { e = stream_op(e, add(), 0); continue; }
            e = arith(C_SHR, ">>", e, add());
        } else return e;
    }
}
static CNode *rel(void) {
    CNode *e = shift();
    for (;;) {
        if (at("<") && !at_k(1, "<")) { next(); e = arith(C_LT, "<", e, shift()); }
        else if (accept("<=")) e = arith(C_LE, "<=", e, shift());
        else if (at(">") && !split_gt) { next(); e = arith(C_GT, ">", e, shift()); }
        else if (accept(">=")) e = arith(C_GE, ">=", e, shift());
        else return e;
    }
}
static CNode *equality(void) {
    CNode *e = rel();
    for (;;) {
        if (accept("==")) e = arith(C_EQ, "==", e, rel());
        else if (accept("!=")) e = arith(C_NE, "!=", e, rel());
        else return e;
    }
}
static CNode *bitand_(void) { CNode *e = equality(); while (at("&") && !at_k(1, "&")) { next(); e = arith(C_BAND, "&", e, equality()); } return e; }
static CNode *bitxor_(void) { CNode *e = bitand_(); while (accept("^")) e = arith(C_BXOR, "^", e, bitand_()); return e; }
static CNode *bitor_(void) { CNode *e = bitxor_(); while (at("|") && !at_k(1, "|")) { next(); e = arith(C_BOR, "|", e, bitxor_()); } return e; }
static CNode *logand(void) {
    CNode *e = bitor_();
    while (accept("&&")) { CNode *r = bitor_(); e = binop(C_LOGAND, to_bool(e), to_bool(r), is_cpp ? ty_bool : ty_int); }
    return e;
}
static CNode *logor(void) {
    CNode *e = logand();
    while (accept("||")) { CNode *r = logand(); e = binop(C_LOGOR, to_bool(e), to_bool(r), is_cpp ? ty_bool : ty_int); }
    return e;
}
static CNode *cond_expr(void) {
    CNode *c = logor();
    if (!accept("?")) return c;
    CNode *a = expr();
    expect(":");
    CNode *b = cond_expr();
    CNode *n = node(C_COND);
    n->a = to_bool(c);
    CNode *x = deref_ref(a), *y = deref_ref(b);
    if (ty_is_num(decay(x)->ty) && ty_is_num(decay(y)->ty)) { usual(&x, &y); n->b = x; n->c = y; n->ty = x->ty; }
    else if (x->ty->kind == TY_STRING || y->ty->kind == TY_STRING) { n->b = convert(x, ty_string, "?:"); n->c = convert(y, ty_string, "?:"); n->ty = ty_string; }
    else { n->b = decay(x); n->c = decay(y); n->ty = n->b->ty; if (n->c->ty->kind == TY_PTR && n->b->ty->kind == TY_NULLPTR) n->ty = n->c->ty; if (n->ty != n->c->ty && n->c->ty->kind != TY_NULLPTR && !(ty_is_ptr(n->ty) && ty_is_ptr(n->c->ty))) n->c = convert(n->c, n->ty, "?:"); }
    return n;
}

static CNode *assign(void) {
    CNode *e = cond_expr();
    static const struct { const char *op; int k; const char *base; } A[] = {
        { "=", 0, "=" }, { "+=", C_ADD, "+" }, { "-=", C_SUB, "-" }, { "*=", C_MUL, "*" }, { "/=", C_DIV, "/" },
        { "%=", C_MOD, "%" }, { "<<=", C_SHL, "<<" }, { ">>=", C_SHR, ">>" }, { "&=", C_BAND, "&" }, { "|=", C_BOR, "|" },
        { "^=", C_BXOR, "^" }, { 0, 0, 0 } };
    for (int i = 0; A[i].op; i++)
        if (at(A[i].op)) {
            next();
            CNode *r = at("{") && is_cpp ? initializer_list(deref_ref(e)->ty) : assign();
            return assignment(e, A[i].k, A[i].base, r);
        }
    return e;
}

static CNode *expr(void) {
    CNode *e = assign();
    while (accept(",")) { CNode *r = assign(); CNode *n = node(C_COMMA); n->a = e; n->b = r; n->ty = r->ty; e = n; }
    return e;
}

static int64_t const_expr(void) {
    CNode *e = cond_expr();
    e = decay(e);
    if (e->k == C_NUM) return e->ival;
    if (e->k == C_CAST && e->a->k == C_NUM) return e->a->ival;
    /* small expressions of constants */
    CV v = cc_eval(e);
    return ty_is_flt(e->ty) ? (int64_t)v.d : v.i;
}

/* {1, 2, 3}: of type t when it's known */
static CNode *initializer_list(CType *t) {
    expect("{");
    CNode *n = node(C_INIT);
    n->ty = t; n->flag = 1;
    while (!at("}")) {
        CNode *el;
        if (at(".") && peekt(1)->k == K_ID) {
            /* .field = value */
            next(); char *f = ident(); expect("=");
            el = at("{") ? initializer_list(0) : assign();
            CNode *d = node(C_NOP); d->name = f; d->a = el;
            el = d;
        } else if (at("[")) {
            next(); int64_t ix = const_expr(); expect("]"); expect("=");
            el = at("{") ? initializer_list(0) : assign();
            CNode *d = node(C_NOP); d->ival = ix; d->flag = 1; d->a = el;
            el = d;
        } else el = at("{") ? initializer_list(0) : assign();
        add_kid(n, el);
        if (!accept(",")) break;
    }
    expect("}");
    return n;
}

/* ---- lambdas ------------------------------------------------------------------------------------------------ */
static CType *lambda_ret;
static CNode *function_body(CFunc *f, CType *ft, int is_lambda);

static CNode *parse_lambda(void) {
    expect("[");
    while (!at("]")) next();                           /* captures: everything is seen by reference */
    expect("]");
    CFunc *f = zalloc(sizeof(CFunc));
    f->name = "lambda";
    f->line = cur()->line;
    CType *ft;
    if (at("(")) ft = func_params(0, f);
    else { ft = func_type(0); f->pnames = zalloc(sizeof(char *)); f->pdefault = zalloc(sizeof(CNode *)); }
    while (cur()->k == K_ID && (!strcmp(cur()->s, "mutable") || !strcmp(cur()->s, "constexpr") || !strcmp(cur()->s, "noexcept"))) next();
    if (accept("->")) ft->ret = abstract_type();
    f->ty = ft;
    CType *saved_ret = lambda_ret;
    lambda_ret = ft->ret;
    function_body(f, ft, 1);
    if (!ft->ret) ft->ret = lambda_ret ? lambda_ret : ty_void;
    lambda_ret = saved_ret;
    add_func(f);
    CType *lt = newty(TY_LAMBDA, 16, 8);
    lt->ret = ft->ret; lt->params = ft->params; lt->nparams = ft->nparams;
    CNode *n = node(C_LAMBDA); n->fn = f; n->ty = lt;
    return n;
}

/* ---- templates -------------------------------------------------------------------------------------------------- */
static Templ *find_template(const char *name) { for (Templ *t = templates; t; t = t->next) if (!strcmp(t->name, name)) return t; return 0; }

static Scope *global_scope;
static void parse_toplevel(void);

typedef struct { Tok *tk; int ntk, pos; Scope *scope; CFunc *fn; CType *cls; int frame, lambda_depth, split; CType *lret; int loop, sw; } PState;
static void save_state(PState *s) { s->tk = tk; s->ntk = ntk; s->pos = pos; s->scope = scope; s->fn = cur_fn; s->cls = cur_class; s->frame = frame_size; s->lambda_depth = lambda_depth; s->split = split_gt; s->lret = lambda_ret; s->loop = loop_depth; s->sw = switch_depth; }
static void load_state(PState *s) { tk = s->tk; ntk = s->ntk; pos = s->pos; scope = s->scope; cur_fn = s->fn; cur_class = s->cls; frame_size = s->frame; lambda_depth = s->lambda_depth; split_gt = s->split; lambda_ret = s->lret; loop_depth = s->loop; switch_depth = s->sw; cc_line = cur()->line; }

static char *mangle(Templ *tp, CType **args) {
    char b[400]; int n = snprintf(b, sizeof b, "%s<", tp->name);
    for (int i = 0; i < tp->nparams && n < 380; i++) { char t[100]; n += snprintf(b + n, sizeof b - (size_t)n, "%s%s", i ? "," : "", ty_str(args[i], t, sizeof t)); }
    snprintf(b + n, sizeof b - (size_t)n, ">");
    return sdup(b, (int)strlen(b));
}

static char *inst_name;                          /* what the definition being instantiated is called */

static void *instantiate(Templ *tp, CType **args) {
    for (int i = 0; i < tp->ninst; i++) {
        int same = 1;
        for (int j = 0; j < tp->nparams; j++) if (tp->inst[i].args[j] != args[j]) same = 0;
        if (same) return tp->inst[i].made;
    }
    if (tp->ninst > 200) cc_error("too many uses of the template %s", tp->name);
    PState st; save_state(&st);
    int saved_bind = ntmpl_bind;
    for (int i = 0; i < tp->nparams; i++) { tmpl_bind_name[ntmpl_bind] = tp->params[i]; tmpl_bind_ty[ntmpl_bind++] = args[i]; }
    tk = tp->body; ntk = tp->nbody; pos = 0; cc_line = tk[0].line;
    scope = global_scope; cur_fn = 0; cur_class = 0; lambda_depth = 0; split_gt = 0; loop_depth = 0; switch_depth = 0;
    char *saved_inst = inst_name;
    inst_name = mangle(tp, args);
    struct Inst *in = rt_alloc(sizeof(struct Inst) * (size_t)(tp->ninst + 1));
    if (tp->ninst) memcpy(in, tp->inst, sizeof(struct Inst) * (size_t)tp->ninst);
    in[tp->ninst].args = rt_alloc(sizeof(CType *) * (size_t)tp->nparams);
    memcpy(in[tp->ninst].args, args, sizeof(CType *) * (size_t)tp->nparams);
    in[tp->ninst].made = 0;
    tp->inst = in;
    int me = tp->ninst++;
    void *made;
    if (tp->is_class) {
        /* the class type exists before its body, for members that point to it */
        CType *t = newty(TY_STRUCT, 0, 1);
        t->name = inst_name;
        tp->inst[me].made = t;
        add_tag(inst_name, t);
        Var *v = add_var(inst_name, t, 0); v->is_typedef = 1;
        /* class Name { ... };  -- read from after the name */
        next();                                     /* class / struct */
        next();                                     /* the template's own name */
        int saved_tb = ntmpl_bind;
        CType *saved_it = cur_inst_type; const char *saved_ib = cur_inst_base;
        cur_inst_type = t; cur_inst_base = tp->name;
        parse_class_body(t);
        cur_inst_type = saved_it; cur_inst_base = saved_ib;
        ntmpl_bind = saved_tb;
        accept(";");
        made = t;
    } else {
        parse_toplevel();
        Var *v = find_var(inst_name);
        made = v ? v->fn : 0;
        tp->inst[me].made = made;
    }
    inst_name = saved_inst;
    ntmpl_bind = saved_bind;
    load_state(&st);
    return made;
}

static CType *instantiate_class(Templ *tp, CType **args, int nargs) {
    CType *full[16];
    for (int i = 0; i < tp->nparams; i++) {
        if (i < nargs) { full[i] = args[i]; continue; }
        if (!tp->pdefault[i]) syntax("%s needs %d type%s", tp->name, tp->nparams, tp->nparams == 1 ? "" : "s");
        /* a default: its tokens, read with the earlier parameters bound */
        PState st; save_state(&st);
        int saved = ntmpl_bind;
        for (int j = 0; j < i; j++) { tmpl_bind_name[ntmpl_bind] = tp->params[j]; tmpl_bind_ty[ntmpl_bind++] = full[j]; }
        tk = tp->pdefault[i]; ntk = tp->ndefault[i]; pos = 0;
        full[i] = abstract_type();
        ntmpl_bind = saved;
        load_state(&st);
    }
    return instantiate(tp, full);
}

/* Name<T, U> as a type */
static CType *template_args_type(Templ *tp) {
    expect("<");
    CType *args[16]; int n = 0;
    while (!at(">") && !at(">>") && !split_gt) {
        if (cur()->k == K_NUM) { CType *c = newty(TY_INT, 4, 4); c->len = (int)cur()->ival; c->name = "constant"; args[n++] = c; next(); }
        else if (n < 16) args[n++] = abstract_type();
        if (!accept(",")) break;
    }
    expect_gt();
    return instantiate_class(tp, args, n);
}

/* f<T>(args) or f(args) with T worked out from them */
static CNode *primary_template_call(Templ *tp) {
    next();
    CType *args[16] = { 0 }; int nexp = 0;
    if (at("<")) {
        next();
        while (!at(">") && !at(">>")) { if (nexp < 16) args[nexp++] = abstract_type(); if (!accept(",")) break; }
        expect_gt();
    }
    CNode *a[32]; int na = parse_args(a, 32);
    /* deduce: the function's parameter list, as written, against the arguments' types */
    int p = 0, depth = 0;
    while (p < tp->nbody && !(tok_is(&tp->body[p], "(") && p > 0 && !strcmp(tp->body[p - 1].s, tp->name))) p++;
    p++;
    int argi = 0, seg = p;
    for (int q = p; q < tp->nbody && argi <= na; q++) {
        Tok *t = &tp->body[q];
        if (tok_is(t, "(") || tok_is(t, "<")) depth++;
        if ((tok_is(t, ")") || tok_is(t, ">")) && depth) { depth--; continue; }
        int end = (tok_is(t, ",") && !depth) || (tok_is(t, ")") && !depth);
        if (!end) continue;
        if (argi < na) {
            CType *at_ = unconst(decay(deref_ref(a[argi]))->ty);
            for (int z = seg; z < q; z++) {
                for (int k = 0; k < tp->nparams; k++) {
                    if (tp->body[z].k != K_ID || strcmp(tp->body[z].s, tp->params[k]) || args[k]) continue;
                    CType *d = at_;
                    if (z > seg && tok_is(&tp->body[z - 1], "<") && z > seg + 1) {
                        const char *ctr = tp->body[z - 2].s;
                        if (!strcmp(ctr, "vector") || !strcmp(ctr, "set")) d = at_->base;
                        if (!strcmp(ctr, "map")) d = at_->base;
                    } else if (z + 1 < q && tok_is(&tp->body[z + 1], "*")) d = at_->kind == TY_PTR ? at_->base : at_;
                    if (decay(deref_ref(a[argi]))->ty->kind == TY_PTR && at_->kind == TY_PTR && unconst(at_->base)->kind == TY_CHAR && !(z + 1 < q && tok_is(&tp->body[z + 1], "*"))) d = at_;
                    args[k] = d;
                }
            }
        }
        argi++; seg = q + 1;
        if (tok_is(t, ")")) break;
    }
    for (int k = 0; k < tp->nparams; k++)
        if (!args[k]) {
            if (tp->pdefault[k]) { PState st; save_state(&st); tk = tp->pdefault[k]; ntk = tp->ndefault[k]; pos = 0; args[k] = abstract_type(); load_state(&st); }
            else syntax("can't tell what %s is for %s()", tp->params[k], tp->name);
        }
    CFunc *f = instantiate(tp, args);
    if (!f) syntax("the template %s didn't make a function", tp->name);
    CFunc *g = choose(f, a, na);
    if (!g) syntax("%s() doesn't take these arguments", tp->name);
    return call_args(g, 0, 0, a, na);
}

static void template_decl(void) {
    expect("<");
    Templ *tp = zalloc(sizeof(Templ));
    tp->params = zalloc(sizeof(char *) * 16);
    tp->pdefault = zalloc(sizeof(Tok *) * 16);
    tp->ndefault = zalloc(sizeof(int) * 16);
    while (!at(">")) {
        if (accept("typename") || accept("class")) { }
        else { declspec(0, 0, 1); }                    /* int N: taken as a type parameter too */
        if (tp->nparams < 16) tp->params[tp->nparams] = ident();
        if (accept("=")) {
            int s = pos, d = 0;
            while (!(d == 0 && (at(",") || at(">")))) { if (at("<")) d++; if (at(">")) d--; if (at(">>")) d -= 2; next(); }
            int n = pos - s;
            Tok *dt = rt_alloc(sizeof(Tok) * (size_t)(n + 1));
            memcpy(dt, tk + s, sizeof(Tok) * (size_t)n);
            memset(&dt[n], 0, sizeof(Tok)); dt[n].k = K_EOF; dt[n].s = "";
            tp->pdefault[tp->nparams] = dt; tp->ndefault[tp->nparams] = n + 1;
        }
        tp->nparams++;
        if (!accept(",")) break;
    }
    expect(">");
    /* the declaration, to its end */
    int s = pos;
    tp->is_class = at("class") || at("struct");
    if (tp->is_class) tp->name = peekt(1)->s;
    int d = 0, seen_brace = 0;
    for (;;) {
        if (cur()->k == K_EOF) syntax("the template never ends");
        if (!tp->is_class && !tp->name && cur()->k == K_ID && at_k(1, "(") && !seen_brace) tp->name = cur()->s;
        if (at("{")) { d++; seen_brace = 1; }
        else if (at("}")) { d--; if (!d && seen_brace) { next(); if (tp->is_class) accept(";"); break; } }
        else if (at(";") && !d && !seen_brace) { next(); break; }
        next();
    }
    if (!tp->name) syntax("a template needs a name");
    int n = pos - s;
    tp->body = rt_alloc(sizeof(Tok) * (size_t)(n + 1));
    memcpy(tp->body, tk + s, sizeof(Tok) * (size_t)n);
    memset(&tp->body[n], 0, sizeof(Tok)); tp->body[n].k = K_EOF; tp->body[n].s = ""; tp->body[n].line = tk[pos - 1].line;
    tp->nbody = n + 1;
    tp->next = templates; templates = tp;
}

/* ---- initializers ------------------------------------------------------------------------------------------ */
static CFunc *find_ctor(CType *t, CNode **args, int n) {
    CFunc *all = 0, *last = 0;
    for (int i = 0; i < t->nmeth; i++) {
        CFunc *f = t->meth[i];
        if (!f->is_ctor) continue;
        /* chain them for choose() without disturbing the class's list */
        CFunc *c = zalloc(sizeof(CFunc)); *c = *f; c->next_overload = 0;
        if (!all) all = c; else last->next_overload = c;
        last = c;
    }
    if (!all) return 0;
    CFunc *g = choose(all, args, n);
    if (!g) return 0;
    for (int i = 0; i < t->nmeth; i++) if (t->meth[i]->is_ctor && t->meth[i]->ty == g->ty && t->meth[i]->pnames == g->pnames) return t->meth[i];
    return 0;
}

static CNode *typed_init(CType *t, CNode *src);

static CNode *ctor_init(CType *t, CNode *src) {
    CFunc *c = find_ctor(t, src->kids, src->nk);
    if (!c) {
        if (src->nk == 0 && !t->has_ctor) return src;
        if (src->nk == 1 && deref_ref(src->kids[0])->ty == t) { src->kids[0] = deref_ref(src->kids[0]); src->op = 1; return src; }   /* a copy */
        syntax("no constructor of %s takes these %d argument%s", t->name, src->nk, src->nk == 1 ? "" : "s");
    }
    src->fn = c;
    CNode *conv = call_args(c, 0, 0, src->kids, src->nk);
    src->kids = conv->kids; src->nk = conv->nk;
    return src;
}

static CNode *typed_init(CType *t, CNode *src) {
    if (!src) return 0;
    if (t->kind == TY_REF) return convert(src, t, "the initializer");
    if (src->k != C_INIT) {
        CNode *e = deref_ref(src);
        if (t->kind == TY_ARRAY && e->k == C_STR) return e;
        if (t->kind == TY_STRUCT && e->ty != t && t->has_ctor) {
            /* Point p = 5: a converting constructor */
            CNode *one = node(C_INIT); one->ty = t; add_kid(one, src);
            int sub = 0;
            for (CType *s = e->ty; s && s->kind == TY_STRUCT; s = s->super) if (s == t) sub = 1;
            if (!sub) return ctor_init(t, one);
        }
        if (t->kind == TY_STRUCT && t->has_ctor && e->ty == t) {
            CNode *one = node(C_INIT); one->ty = t; add_kid(one, src);
            CFunc *c = find_ctor(t, one->kids, 1);
            if (c && c->ty->nparams == 1 && c->ty->params[0]->kind == TY_REF) return ctor_init(t, one);
        }
        return convert(src, t, "the initializer");
    }
    src->ty = t;
    switch (t->kind) {
    case TY_ARRAY: {
        int idx = 0;
        for (int i = 0; i < src->nk; i++) {
            CNode *k = src->kids[i];
            if (k->k == C_NOP && k->flag == 1) { idx = (int)k->ival; k = k->a; }
            if (t->len >= 0 && idx >= t->len) syntax("too many elements in the array's initializer");
            CNode *w = node(C_NOP); w->off = (int64_t)idx * t->base->size; w->a = typed_init(t->base, k);
            src->kids[i] = w;
            idx++;
        }
        src->ival = idx;
        return src;
    }
    case TY_STRUCT: {
        if (t->has_ctor) return ctor_init(t, src);
        int mi = 0;
        for (int i = 0; i < src->nk; i++) {
            CNode *k = src->kids[i];
            CMember *m;
            if (k->k == C_NOP && k->name) { m = find_member(t, k->name, 0); if (!m) syntax("%s has no member named '%s'", t->name ? t->name : "the struct", k->name); k = k->a; mi = (int)(m - t->mem) + 1; }
            else {
                if (mi >= t->nmem) syntax("too many values in the initializer of %s", t->name ? t->name : "the struct");
                m = &t->mem[mi++];
            }
            CNode *w = node(C_NOP); w->off = m->offset; w->a = typed_init(m->ty, k);
            src->kids[i] = w;
            if (t->is_union) break;
        }
        return src;
    }
    case TY_PAIR:
        if (src->nk != 2) syntax("a pair is made of two values");
        src->kids[0] = typed_init(t->base, src->kids[0]);
        src->kids[1] = typed_init(t->base2, src->kids[1]);
        return src;
    case TY_VECTOR: case TY_SET:
        if (src->flag) { for (int i = 0; i < src->nk; i++) src->kids[i] = typed_init(t->base, src->kids[i]); return src; }
        /* vector<int> v(n) or v(n, value), or from another one */
        if (src->nk == 1 && deref_ref(src->kids[0])->ty == t) { src->op = 1; src->kids[0] = deref_ref(src->kids[0]); return src; }
        if (src->nk >= 1) src->kids[0] = new_cast(src->kids[0], ty_long);
        if (src->nk >= 2) src->kids[1] = typed_init(t->base, src->kids[1]);
        if (src->nk > 2) syntax("vector(n, value) takes at most two arguments");
        src->op = 2;
        return src;
    case TY_MAP:
        for (int i = 0; i < src->nk; i++) src->kids[i] = typed_init(ty_pair(t->base, t->base2), src->kids[i]);
        return src;
    case TY_STRING:
        if (src->nk == 2 && ty_is_int(decay(src->kids[0])->ty)) { src->kids[0] = new_cast(src->kids[0], ty_long); src->kids[1] = new_cast(src->kids[1], ty_char); src->op = 2; return src; }
        if (src->nk == 1) return convert(src->kids[0], ty_string, "the string");
        if (!src->nk) return src;
        syntax("string() takes one or two arguments here");
    default:
        if (!src->nk) return new_cast(num(0, ty_int), t);
        if (src->nk > 1) syntax("one value for a %s", t->kind == TY_PTR ? "pointer" : "number");
        return convert(src->kids[0], t, "the initializer");
    }
}

/* ---- statements ------------------------------------------------------------------------------------------------- */
static CNode *declaration(int is_static_ok);

static void note_dtor(CNode *blk, int64_t off, CType *t) {
    if (!blk) return;
    if (!(t->owns || (t->kind == TY_STRUCT && t->dtor) || t->kind == TY_LAMBDA)) return;
    if (t->kind == TY_LAMBDA) return;
    blk->dtors = rt_realloc(blk->dtors, sizeof(int) * (size_t)blk->ndtors, sizeof(int) * (size_t)(blk->ndtors + 1));
    blk->dtor_ty = rt_realloc(blk->dtor_ty, sizeof(CType *) * (size_t)blk->ndtors, sizeof(CType *) * (size_t)(blk->ndtors + 1));
    blk->dtors[blk->ndtors] = (int)off; blk->dtor_ty[blk->ndtors++] = t;
}

static CNode *compound(int new_scope) {
    CNode *b = node(C_BLOCK);
    expect("{");
    if (new_scope) push_scope();
    CNode *saved = scope->block;
    scope->block = b;
    while (!at("}")) {
        if (cur()->k == K_EOF) syntax("expected '}' at the end of the file");
        add_kid(b, stmt());
    }
    next();
    scope->block = saved;
    if (new_scope) pop_scope();
    return b;
}

static int is_decl_start(void) {
    Tok *t = cur();
    if (t->k != K_ID) return 0;
    if (!strcmp(t->s, "static") || !strcmp(t->s, "const") || !strcmp(t->s, "auto") || !strcmp(t->s, "struct") ||
        !strcmp(t->s, "enum") || !strcmp(t->s, "union") || !strcmp(t->s, "typedef") || !strcmp(t->s, "unsigned") ||
        !strcmp(t->s, "signed") || !strcmp(t->s, "constexpr") || !strcmp(t->s, "class")) return 1;
    if (!is_type_name(t)) return 0;
    /* a type followed by ( is a cast or a constructed value: an expression */
    if (at_k(1, "(") && strcmp(t->s, "string") && !find_template(t->s) && !at_k(2, "*") && !at_k(2, "&")) return 0;
    if (at_k(1, "::") && strcmp(t->s, "std")) return 0;
    return 1;
}

static CNode *for_stmt(void) {
    CNode *n = node(C_FOR);
    expect("(");
    push_scope();
    CNode *saved = scope->block;
    CNode *blk = node(C_BLOCK);
    scope->block = blk;
    /* for (decl : range) */
    if (is_decl_start()) {
        int save = pos;
        CType *bt = declspec(0, 0, 0);
        char *nm = 0;
        int byref = 0;
        for (;;) {
            if (accept("*")) { if (bt) bt = ty_ptr(bt); continue; }
            if (accept("&") || accept("&&")) { byref = 1; continue; }
            if (accept("const")) continue;
            break;
        }
        if (at("[")) {
            /* for (auto& [k, v] : m) */
            next(); char *k1 = ident(); expect(","); char *k2 = ident(); expect("]");
            if (accept(":")) {
                CNode *range = deref_ref(expr());
                expect(")");
                CType *et = range->ty->kind == TY_MAP ? ty_pair(range->ty->base, range->ty->base2) : range->ty->base;
                if (!et || et->kind != TY_PAIR) syntax("[a, b] needs a map or pairs");
                CNode *rf = node(C_RANGEFOR); rf->a = range; rf->ty = ty_ref(et); rf->off = alloc_local(rf->ty); rf->flag = 1;
                Var *pv = add_var("$pair", ty_ref(et), 1); pv->off = rf->off;
                CNode *pn = deref_ref(var_node(pv));
                Var *a = add_var(k1, ty_ref(et->base), 1); a->off = alloc_local(a->ty);
                Var *b2 = add_var(k2, ty_ref(et->base2), 1); b2->off = alloc_local(b2->ty);
                /* the bindings point into the pair at each turn */
                CNode *ba = node(C_ASSIGN); ba->a = var_node(a); ba->a->ty = ty_ptr(et->base); ba->b = binop(C_ADDR, member_of(pn, "first"), 0, ty_ptr(et->base)); ba->ty = ba->a->ty; ba->b->k = C_ADDR; ba->b->a = member_of(pn, "first");
                CNode *bb = node(C_ASSIGN); bb->a = var_node(b2); bb->a->ty = ty_ptr(et->base2); bb->b = node(C_ADDR); bb->b->a = member_of(pn, "second"); bb->b->ty = ty_ptr(et->base2); bb->ty = bb->a->ty;
                loop_depth++;
                CNode *body = stmt();
                loop_depth--;
                CNode *wrap = node(C_BLOCK);
                CNode *e1 = node(C_EXPR); e1->a = ba; add_kid(wrap, e1);
                CNode *e2 = node(C_EXPR); e2->a = bb; add_kid(wrap, e2);
                add_kid(wrap, body);
                rf->b = wrap;
                scope->block = saved;
                pop_scope();
                add_kid(blk, rf);
                return blk;
            }
            pos = save;
        } else {
            nm = cur()->k == K_ID ? cur()->s : 0;
            if (nm && at_k(1, ":")) {
                next(); next();
                CNode *range = at("{") ? initializer_list(0) : deref_ref(expr());
                expect(")");
                if (range->k == C_INIT) {
                    /* for (int x : {1, 2, 3}) */
                    CType *et = bt ? bt : decay(range->kids[0])->ty;
                    range = typed_init(ty_vector(unconst(et)), range);
                    CNode *tmp = node(C_COPY); tmp->a = range; tmp->ty = range->ty; tmp->flag = 2;
                    range = tmp;
                }
                CType *rt = range->ty, *et;
                if (rt->kind == TY_ARRAY) et = rt->base;
                else if (rt->kind == TY_VECTOR || rt->kind == TY_SET) et = rt->base;
                else if (rt->kind == TY_STRING) et = ty_char;
                else if (rt->kind == TY_MAP) et = ty_pair(rt->base, rt->base2);
                else { char b[80]; syntax("can't loop over a %s", ty_str(rt, b, sizeof b)); }
                CType *vt = bt ? bt : et;
                if (byref) vt = ty_ref(vt);
                CNode *rf = node(C_RANGEFOR); rf->a = range; rf->ty = vt;
                Var *v = add_var(nm, vt, 1); v->off = alloc_local(vt);
                rf->off = v->off; rf->flag = byref;
                if (!byref && vt != et && !(ty_is_num(vt) && ty_is_num(et))) syntax("the loop variable's type doesn't match the elements");
                rf->d = (CNode *)et;
                if (!byref) note_dtor(blk, v->off, vt);
                loop_depth++;
                rf->b = stmt();
                loop_depth--;
                scope->block = saved;
                pop_scope();
                add_kid(blk, rf);
                return blk;
            }
        }
        pos = save;
        n->a = declaration(0);
    } else if (!accept(";")) { CNode *e = node(C_EXPR); e->a = expr(); n->a = e; expect(";"); }
    if (!at(";")) n->b = to_bool(expr());
    expect(";");
    if (!at(")")) n->c = expr();
    expect(")");
    loop_depth++;
    n->d = stmt();
    loop_depth--;
    scope->block = saved;
    pop_scope();
    add_kid(blk, n);
    return blk;
}

static CNode *stmt(void) {
    Tok *t = cur();
    cc_line = t->line;
    if (at("{")) return compound(1);
    if (accept(";")) { CNode *n = node(C_NOP); return n; }
    if (t->k == K_ID) {
        const char *s = t->s;
        if (!strcmp(s, "if")) {
            next(); expect("(");
            CNode *n = node(C_IF);
            if (is_decl_start()) { syntax("a declaration in if() isn't supported: declare it before"); }
            n->a = to_bool(expr()); expect(")");
            n->b = stmt();
            if (cur()->k == K_ID && !strcmp(cur()->s, "else")) { next(); n->c = stmt(); }
            return n;
        }
        if (!strcmp(s, "while")) {
            next(); expect("(");
            CNode *n = node(C_WHILE); n->a = to_bool(expr()); expect(")");
            loop_depth++; n->b = stmt(); loop_depth--;
            return n;
        }
        if (!strcmp(s, "do")) {
            next();
            CNode *n = node(C_DO);
            loop_depth++; n->b = stmt(); loop_depth--;
            if (!(cur()->k == K_ID && !strcmp(cur()->s, "while"))) syntax("expected 'while' after do's body");
            next(); expect("("); n->a = to_bool(expr()); expect(")"); expect(";");
            return n;
        }
        if (!strcmp(s, "for")) { next(); return for_stmt(); }
        if (!strcmp(s, "switch")) {
            next(); expect("(");
            CNode *n = node(C_SWITCH);
            CNode *c = decay(deref_ref(expr()));
            if (!ty_is_int(c->ty)) syntax("switch needs an integer (or a char or an enum)");
            n->a = new_cast(c, ty_long);
            expect(")");
            switch_depth++; loop_depth++;
            n->b = stmt();
            switch_depth--; loop_depth--;
            return n;
        }
        if (!strcmp(s, "case") || !strcmp(s, "default")) {
            if (!switch_depth) syntax("'%s' outside a switch", s);
            next();
            CNode *n = node(C_CASE);
            if (s[0] == 'c') {
                n->case_lo = n->case_hi = (int)const_expr();
                if (accept("...")) n->case_hi = (int)const_expr();
            } else n->flag = 1;
            expect(":");
            return n;
        }
        if (!strcmp(s, "break")) { next(); if (!loop_depth) syntax("'break' outside a loop or switch"); expect(";"); return node(C_BREAK); }
        if (!strcmp(s, "continue")) { next(); if (!loop_depth) syntax("'continue' outside a loop"); expect(";"); return node(C_CONTINUE); }
        if (!strcmp(s, "return")) {
            next();
            CNode *n = node(C_RETURN);
            CType *rt = cur_fn ? cur_fn->ty->ret : 0;
            if (lambda_depth && cur_fn && !strcmp(cur_fn->name, "lambda")) {
                if (!at(";")) {
                    CNode *e = expr();
                    if (!rt) { rt = lambda_ret = lambda_ret ? lambda_ret : unconst(decay(deref_ref(e))->ty); cur_fn->ty->ret = rt; }
                    n->a = convert(e, rt, "the return value");
                }
                expect(";");
                return n;
            }
            if (!at(";")) {
                CNode *e = at("{") ? initializer_list(rt) : expr();
                if (!rt || rt->kind == TY_VOID) { if (deref_ref(e)->ty->kind != TY_VOID) syntax("a void function can't return a value"); n->a = e; }
                else n->a = e->k == C_INIT ? typed_init(rt, e) : convert(e, rt, "the return value");
            } else if (rt && rt->kind != TY_VOID && !(cur_fn && cur_fn->is_ctor)) syntax("this function has to return a value");
            expect(";");
            return n;
        }
        if (!strcmp(s, "goto")) syntax("goto isn't supported");
        if (!strcmp(s, "try")) {
            next();
            CNode *n = node(C_TRY);
            n->a = compound(1);
            while (cur()->k == K_ID && !strcmp(cur()->s, "catch")) {
                next(); expect("(");
                CNode *h = node(C_CASE);
                push_scope();
                if (accept("...")) h->flag = 1;
                else {
                    CType *ct = declspec(0, 0, 1);
                    char *nm = 0;
                    CType *vt = declarator(ct, &nm);
                    h->ty = vt->kind == TY_REF ? vt->base : vt;
                    if (nm) { Var *v = add_var(nm, h->ty, 1); v->off = alloc_local(h->ty); h->off = v->off; h->name = nm; }
                }
                expect(")");
                h->a = compound(0);
                pop_scope();
                add_kid(n, h);
            }
            if (!n->nk) syntax("try needs a catch");
            return n;
        }
        if (!strcmp(s, "using") || !strcmp(s, "typedef")) { CNode *d = declaration(1); return d ? d : node(C_NOP); }
        if (cur()->k == K_ID && at_k(1, ":") && !at_k(2, ":") && !is_type_name(cur())) syntax("labels (and goto) aren't supported");
        if (is_decl_start()) { CNode *d = declaration(1); return d ? d : node(C_NOP); }
    }
    CNode *n = node(C_EXPR);
    n->a = expr();
    expect(";");
    return n;
}

/* ---- declarations ----------------------------------------------------------------------------------------------- */
static CType *enum_decl(void) {
    char *tag = 0;
    if (cur()->k == K_ID && strcmp(cur()->s, "class") && strcmp(cur()->s, "struct")) tag = ident();
    else if (accept("class") || accept("struct")) tag = ident();
    if (accept(":")) declspec(0, 0, 1);
    CType *t = newty(TY_ENUM, 4, 4);
    t->name = tag;
    if (!at("{")) {
        Var *v = tag ? find_tag(tag) : 0;
        if (v) return v->ty;
        return t;
    }
    next();
    int64_t val = 0;
    while (!at("}")) {
        char *nm = ident();
        if (accept("=")) val = const_expr();
        Var *v = add_var(nm, ty_int, 0); v->is_enum = 1; v->ival = val;
        if (tag) {
            /* Color::Red for enum class */
            char full[160]; snprintf(full, sizeof full, "%s::%s", tag, nm);
            Var *w = add_var(sdup(full, (int)strlen(full)), ty_int, 0); w->is_enum = 1; w->ival = val;
        }
        val++;
        if (!accept(",")) break;
    }
    expect("}");
    if (tag) { add_tag(tag, t); Var *v = add_var(tag, t, 0); v->is_typedef = 1; }
    return t;
}

typedef struct { CFunc *f; int body, inits; } Deferred;

static CNode *function_body(CFunc *f, CType *ft, int is_lambda) {
    int saved_frame = frame_size;
    CFunc *saved_fn = cur_fn;
    int saved_loop = loop_depth, saved_sw = switch_depth;
    if (is_lambda) lambda_depth++;
    frame_size = 0; cur_fn = f; loop_depth = 0; switch_depth = 0;
    push_scope();
    f->poff = zalloc(sizeof(int) * (size_t)(ft->nparams ? ft->nparams : 1));
    for (int i = 0; i < ft->nparams; i++) {
        CType *pt = ft->params[i];
        f->poff[i] = (int)alloc_local(pt);
        if (f->pnames && f->pnames[i]) { Var *v = add_var(f->pnames[i], pt, 1); v->off = f->poff[i]; }
    }
    if (f->is_ctor && accept(":")) {
        /* member initializers: name(args), Base(args) */
        CNode **inits = 0; int ni = 0;
        do {
            char *mn = ident();
            int brace = at("{");
            CNode *src = node(C_INIT); src->flag = brace;
            next();
            while (!at(brace ? "}" : ")")) { add_kid(src, assign()); if (!accept(",")) break; }
            expect(brace ? "}" : ")");
            CNode *in = node(C_NOP);
            CType *own;
            CMember *m = find_member(f->owner, mn, &own);
            if (m && own == f->owner) { in->off = m->offset; in->ty = m->ty; in->name = mn; in->a = m->ty->kind == TY_REF ? convert(src->kids[0], m->ty, "the member's initializer") : typed_init(m->ty, src->nk == 1 && !brace && m->ty->kind != TY_VECTOR && m->ty->kind != TY_STRUCT ? src->kids[0] : src); }
            else if (f->owner->super && !strcmp(f->owner->super->name, mn)) { in->flag = 1; in->ty = f->owner->super; in->a = typed_init(f->owner->super, src); }
            else if (m) { in->off = m->offset; in->ty = m->ty; in->a = typed_init(m->ty, src->nk == 1 && !brace ? src->kids[0] : src); }
            else syntax("%s has no member or base called '%s'", f->owner->name, mn);
            inits = rt_realloc(inits, sizeof(CNode *) * (size_t)ni, sizeof(CNode *) * (size_t)(ni + 1));
            inits[ni++] = in;
        } while (accept(","));
        f->inits = (CNode *)inits; f->ninits = ni;
    }
    if (!at("{") && is_lambda) syntax("a lambda needs a { body }");
    f->body = compound(0);
    f->frame = frame_size;
    pop_scope();
    frame_size = saved_frame; cur_fn = saved_fn; loop_depth = saved_loop; switch_depth = saved_sw;
    if (is_lambda) lambda_depth--;
    return f->body;
}

static int skip_balanced(void) {
    /* from a '{' to just past its '}' (or a ( ... ) group); where it started */
    int s = pos;
    const char *open = cur()->s;
    const char *close = !strcmp(open, "{") ? "}" : ")";
    int d = 0;
    for (;;) {
        if (cur()->k == K_EOF) syntax("missing '%s'", close);
        if (at(open)) d++;
        else if (at(close) && --d == 0) { next(); break; }
        next();
    }
    return s;
}

static void add_method(CType *t, CFunc *f) {
    if (!strcmp(f->name, "operator<")) t->op_lt = f;
    t->meth = rt_realloc(t->meth, sizeof(CFunc *) * (size_t)t->nmeth, sizeof(CFunc *) * (size_t)(t->nmeth + 1));
    t->meth[t->nmeth++] = f;
    /* overloads of a name, chained from its first */
    for (int i = 0; i < t->nmeth - 1; i++)
        if (!strcmp(t->meth[i]->name, f->name)) { CFunc *g = t->meth[i]; while (g->next_overload) g = g->next_overload; g->next_overload = f; break; }
}

static void parse_class_body(CType *t) {
    if (accept(":")) {
        do {
            while (cur()->k == K_ID && (!strcmp(cur()->s, "public") || !strcmp(cur()->s, "private") || !strcmp(cur()->s, "protected") || !strcmp(cur()->s, "virtual"))) next();
            CType *b = declspec(0, 0, 1);
            if (b->kind != TY_STRUCT || !b->complete) syntax("a class can only derive from a complete class");
            if (!t->super) t->super = b;
            else syntax("one base class at most here");
        } while (accept(","));
    }
    expect("{");
    /* does it have virtual methods? (it needs to say which class it is, then) */
    int d = 0;
    for (int i = pos; i < ntk; i++) {
        if (tok_is(&tk[i], "{")) d++;
        else if (tok_is(&tk[i], "}")) { if (!d) break; d--; }
        else if (tk[i].k == K_ID && !strcmp(tk[i].s, "virtual")) t->polymorphic = 1;
    }
    if (t->super && t->super->polymorphic) t->polymorphic = 1;
    int off = t->super ? t->super->size : 0, al = t->super ? t->super->align : 1;
    if (t->polymorphic && !t->super) { off = 8; al = 8; }
    if (t->super) { t->owns = t->super->owns; }
    CMember mem[200]; int nm = 0;
    if (t->super) { /* the base's members are found through super */ }
    Deferred defer[200]; int nd = 0;
    struct { int idx; int tokpos; } minit[100]; int nmi = 0;
    CType *saved_class = cur_class;
    cur_class = t;
    push_scope();
    while (!at("}")) {
        if (cur()->k == K_EOF) syntax("the class never ends");
        if (cur()->k == K_ID && (!strcmp(cur()->s, "public") || !strcmp(cur()->s, "private") || !strcmp(cur()->s, "protected")) && at_k(1, ":")) { next(); next(); continue; }
        if (accept(";")) continue;
        if (cur()->k == K_ID && (!strcmp(cur()->s, "using") || !strcmp(cur()->s, "typedef"))) { declaration(1); continue; }
        if (cur()->k == K_ID && !strcmp(cur()->s, "enum")) { next(); enum_decl(); accept(";"); continue; }
        if (cur()->k == K_ID && !strcmp(cur()->s, "template")) syntax("member templates aren't supported");
        int is_friend = 0, is_virtual = 0, is_static = 0;
        for (;;) {
            if (accept("friend")) is_friend = 1;
            else if (accept("virtual")) is_virtual = 1;
            else if (accept("static")) is_static = 1;
            else if (accept("explicit") || accept("inline") || accept("constexpr") || accept("mutable")) { }
            else break;
        }
        if (is_friend && (at("class") || at("struct"))) { while (!at(";")) next(); next(); continue; }
        /* a constructor or the destructor */
        int is_ctor = cur()->k == K_ID && (!strcmp(cur()->s, t->name) || (cur_inst_type == t && !strcmp(cur()->s, cur_inst_base))) && at_k(1, "(");
        int is_dtor = at("~");
        CType *rt = 0;
        char *name = 0;
        if (is_dtor) { next(); ident(); name = "~"; }
        else if (is_ctor) { next(); name = t->name; }
        else {
            rt = declspec(0, 0, 1);
            if (!rt) syntax("a member needs a type");
            if (at("operator") || (cur()->k == K_ID && !strcmp(cur()->s, "operator"))) {
                next();
                char op[16] = "operator";
                if (at("(")) { next(); expect(")"); strcat(op, "()"); }
                else if (at("[")) { next(); expect("]"); strcat(op, "[]"); }
                else if (cur()->k == K_ID) { strcat(op, " "); strcat(op, cur()->s); next(); }
                else { strncat(op, cur()->s, (size_t)cur()->len); next(); }
                name = sdup(op, (int)strlen(op));
            }
        }
        if (!name && rt && (at("*") || at("&") || at("&&"))) {
            /* the pointer/reference may belong to an operator's return type */
            int save = pos;
            while (accept("*") || accept("&") || accept("&&")) { }
            if (cur()->k == K_ID && !strcmp(cur()->s, "operator")) {
                pos = save;
                while (at("*") || at("&") || at("&&")) { rt = at("*") ? ty_ptr(rt) : ty_ref(rt); next(); }
                next();
                char op[16] = "operator";
                if (at("(")) { next(); expect(")"); strcat(op, "()"); }
                else if (at("[")) { next(); expect("]"); strcat(op, "[]"); }
                else { strncat(op, cur()->s, (size_t)cur()->len); next(); }
                name = sdup(op, (int)strlen(op));
            } else pos = save;
        }
        if (!name && rt && cur()->k == K_ID && !strcmp(cur()->s, "operator")) {
            /* operator bool() */
            next(); CType *ct = abstract_type(); (void)ct; name = "operator bool"; rt = ty_bool;
        }
        int la = 0;
        while (at_k(la, "*") || at_k(la, "&") || at_k(la, "&&")) la++;
        if (name || at_k(la + 1, "(") || at("(")) {
            CType *ret = rt;
            if (!name) {
                /* a method: Type name(params) */
                for (;;) { if (accept("*")) ret = ty_ptr(ret); else if (accept("&") || accept("&&")) ret = ty_ref(ret); else break; }
                name = ident();
            }
            if (at("(")) {
                CFunc *f = zalloc(sizeof(CFunc));
                f->name = name; f->line = cur()->line;
                f->ty = func_params(is_ctor || is_dtor ? ty_void : ret, f);
                f->owner = is_friend ? 0 : t;
                f->is_ctor = is_ctor; f->is_dtor = is_dtor; f->is_static = is_static;
                f->is_virtual = is_virtual;
                while (cur()->k == K_ID && (!strcmp(cur()->s, "const") || !strcmp(cur()->s, "override") || !strcmp(cur()->s, "final") || !strcmp(cur()->s, "noexcept"))) {
                    if (!strcmp(cur()->s, "override")) f->is_virtual = 1;
                    next();
                }
                /* a method overriding a virtual one is virtual too */
                if (t->super && !is_friend) { CFunc *b = cc_find_method(t->super, name, 0); if (b && b->is_virtual) f->is_virtual = 1; }
                if (accept("=")) {
                    if (at("default") || at("delete")) { next(); expect(";"); if (is_ctor) t->has_ctor = 1; continue; }
                    const_expr(); f->is_pure = 1; expect(";");
                    add_method(t, f);
                    continue;
                }
                if (is_friend) {
                    /* an operator<< defined in the class: a free function, read after it */
                    f->owner = 0;
                    Var *v = find_var(name);
                    if (v && v->fn) { CFunc *g = v->fn; while (g->next_overload) g = g->next_overload; g->next_overload = f; }
                    else { Scope *s = scope; scope = global_scope; Var *nv = add_var(name, f->ty, 0); nv->fn = f; scope = s; }
                    if (!strcmp(name, "operator<") && !t->op_lt) t->op_lt = f;
                    add_func(f);
                } else {
                    if (is_ctor) t->has_ctor = 1;
                    if (is_dtor) t->dtor = f;
                    add_method(t, f);
                    add_func(f);
                }
                if (at("{") || at(":")) {
                    defer[nd].f = f; defer[nd].inits = at(":") ? pos : -1;
                    if (at(":")) { while (!at("{")) { if (at("(")) skip_balanced(); else next(); } }
                    defer[nd].body = skip_balanced();
                    nd++;
                } else expect(";");
                continue;
            }
            pos--;                                             /* a field after all */
            if (is_dtor || is_ctor) syntax("expected '('");
        }
        /* fields: int x, y = 0, *p; */
        for (;;) {
            char *fname = 0;
            CType *ft = declarator(rt, &fname);
            if (!fname) syntax("a member needs a name");
            if (is_static) {
                char full[200]; snprintf(full, sizeof full, "%s::%s", t->name, fname);
                Scope *s = scope; scope = global_scope;
                Var *v = add_var(sdup(full, (int)strlen(full)), ft, 0);
                v->off = (int64_t)alloc_global(ft->size, ft->align);
                scope = s;
                if (accept("=")) {
                    CNode *e = assign();
                    CNode *dn = node(C_DECL); dn->off = v->off; dn->global = 1; dn->ty = ft; dn->init = typed_init(ft, e);
                    global_init = rt_realloc(global_init, sizeof(CNode *) * (size_t)nginit, sizeof(CNode *) * (size_t)(nginit + 1));
                    global_init[nginit++] = dn;
                }
            } else {
                int a2 = ft->align < 1 ? 1 : ft->align;
                if (t->is_union) { mem[nm].offset = 0; if (ft->size > off) off = ft->size; }
                else { off = align_to(off, a2); mem[nm].offset = off; off += ft->size; }
                if (a2 > al) al = a2;
                mem[nm].name = fname; mem[nm].ty = ft;
                if (ft->owns || (ft->kind == TY_STRUCT && ft->dtor)) t->owns = 1;
                if (at("=") || at("{")) {
                    minit[nmi].idx = nm; minit[nmi].tokpos = pos; nmi++;
                    if (at("{")) skip_balanced();
                    else { next(); int dd = 0; while (!((at(",") || at(";")) && !dd)) { if (at("(") || at("{") || at("[")) dd++; if (at(")") || at("}") || at("]")) dd--; next(); } }
                }
                if (nm < 199) nm++;
            }
            if (!accept(",")) break;
        }
        expect(";");
    }
    next();
    t->mem = zalloc(sizeof(CMember) * (size_t)(nm ? nm : 1));
    memcpy(t->mem, mem, sizeof(CMember) * (size_t)nm);
    t->nmem = nm;
    t->size = align_to(off ? off : 1, al);
    t->align = al;
    t->complete = 1;
    if (t->dtor || (t->super && t->super->dtor)) t->owns = 1;
    classes = rt_realloc(classes, sizeof(CType *) * (size_t)nclasses, sizeof(CType *) * (size_t)(nclasses + 1));
    classes[nclasses++] = t;
    t->class_id = nclasses;
    pop_scope();
    /* now the bodies, which may use anything in the class */
    int end = pos;
    if (nmi) {
        CFunc *init_fn = zalloc(sizeof(CFunc)); init_fn->name = "member initializers"; init_fn->ty = func_type(ty_void); init_fn->owner = t;
        CFunc *saved_fn = cur_fn; int saved_frame = frame_size;
        cur_fn = init_fn; frame_size = 0;
        t->minit = zalloc(sizeof(CNode *) * (size_t)nmi); t->nminit = nmi;
        for (int i = 0; i < nmi; i++) {
            pos = minit[i].tokpos;
            CMember *m = &t->mem[minit[i].idx];
            CNode *src;
            if (at("{")) src = initializer_list(0);
            else { next(); src = assign(); }
            CNode *in = node(C_NOP); in->off = m->offset; in->ty = m->ty; in->a = typed_init(m->ty, src);
            t->minit[i] = in;
        }
        cur_fn = saved_fn; frame_size = saved_frame;
    }
    for (int i = 0; i < nd; i++) {
        CFunc *f = defer[i].f;
        pos = defer[i].inits >= 0 ? defer[i].inits : defer[i].body;
        cur_class = f->owner ? t : 0;
        if (!f->owner) cur_class = 0;
        Scope *s = scope; scope = global_scope;
        function_body(f, f->ty, 0);
        scope = s;
    }
    pos = end;
    cur_class = saved_class;
    cc_line = cur()->line;
}

static CType *struct_decl(int is_union, int is_class_kw) {
    (void)is_class_kw;
    char *tag = 0;
    if (cur()->k == K_ID && !is_type_name(cur())) tag = ident();
    else if (cur()->k == K_ID && find_tag(cur()->s)) tag = ident();
    else if (cur()->k == K_ID && !at("{")) tag = ident();
    if (!at("{") && !at(":")) {
        Var *v = tag ? find_tag(tag) : 0;
        if (v) return v->ty;
        CType *t = newty(TY_STRUCT, 0, 1); t->name = tag; t->is_union = is_union;
        if (tag) { add_tag(tag, t); Var *tv = add_var(tag, t, 0); tv->is_typedef = 1; }
        return t;
    }
    CType *t = 0;
    if (tag) { for (Var *v = scope->tags; v; v = v->next) if (!strcmp(v->name, tag) && !v->ty->complete) t = v->ty; }
    if (!t) { t = newty(TY_STRUCT, 0, 1); t->name = tag ? tag : "(anonymous)"; t->is_union = is_union; if (tag) { add_tag(tag, t); Var *tv = add_var(tag, t, 0); tv->is_typedef = 1; } }
    parse_class_body(t);
    return t;
}

/* One declaration statement: int a = 1, *b; typedefs; using; local or global. */
static CNode *declaration(int is_static_ok) {
    (void)is_static_ok;
    if (cur()->k == K_ID && !strcmp(cur()->s, "using")) {
        next();
        if (accept("namespace")) { ident(); while (accept("::")) ident(); expect(";"); return 0; }
        char *nm = ident();
        if (accept("=")) { CType *t = abstract_type(); Var *v = add_var(nm, t, 0); v->is_typedef = 1; }
        else while (!at(";")) next();
        expect(";");
        return 0;
    }
    int is_typedef, is_static;
    CType *base = declspec(&is_typedef, &is_static, 0);
    int is_auto = !base;
    if (accept(";")) return 0;                         /* struct X { ... }; */
    CNode *list = node(C_BLOCK);
    list->flag = 1;                                    /* declarations: not a scope of their own */
    for (;;) {
        char *nm = 0;
        CType *t = declarator(is_auto ? ty_int : base, &nm);
        if (is_auto) {
            /* auto x = e: x's type is e's */
            if (!at("=") && !at("{") && !at("(")) syntax("auto needs an initializer");
            int ref = t->kind == TY_REF;
            next();
            CNode *e = at("{") ? 0 : assign();
            if (!e) syntax("auto x{...} isn't supported: use auto x = ...");
            if (at(")")) next();
            CType *et = deref_ref(e)->ty;
            if (et->kind == TY_ARRAY && !ref) et = ty_ptr(et->base);
            et = unconst(et);
            t = ref ? ty_ref(et) : et;
            if (t->kind == TY_VOID) syntax("can't make a variable from a void value");
            if (!nm) syntax("a declaration needs a name");
            Var *v = add_var(nm, t, cur_fn != 0);
            CNode *d = node(C_DECL); d->ty = t; d->name = nm;
            if (cur_fn) { v->off = alloc_local(t); d->off = v->off; }
            else { v->off = (int64_t)alloc_global(t->size, t->align); d->off = v->off; d->global = 1; }
            d->init = typed_init(t, e);
            if (cur_fn) { note_dtor(scope->block, d->off, t); add_kid(list, d); }
            else { global_init = rt_realloc(global_init, sizeof(CNode *) * (size_t)nginit, sizeof(CNode *) * (size_t)(nginit + 1)); global_init[nginit++] = d; }
            if (!accept(",")) break;
            continue;
        }
        if (!nm) syntax("a declaration needs a name");
        if (is_typedef) { Var *v = add_var(nm, t, 0); v->is_typedef = 1; if (!accept(",")) break; continue; }
        if (at("(") && (!cur_fn || is_type_name(peekt(1)) || at_k(1, ")"))) {
            /* a function's prototype (or definition, at the top) */
            CFunc *f = zalloc(sizeof(CFunc));
            f->name = nm; f->line = cur()->line;
            f->ty = func_params(t, f);
            while (accept("const") || accept("noexcept") || accept("override")) { }
            Var *prev = find_var(nm);
            if (prev && prev->fn) {
                CFunc *g = prev->fn, *same = 0;
                for (; g; g = g->next_overload) {
                    int eqs = g->ty->nparams == f->ty->nparams;
                    for (int i = 0; eqs && i < f->ty->nparams; i++) if (unconst(g->ty->params[i]) != unconst(f->ty->params[i]) && !(g->ty->params[i]->kind == f->ty->params[i]->kind && g->ty->params[i]->kind == TY_PTR)) eqs = 0;
                    if (eqs) same = g;
                }
                if (same) { if (at("{")) { same->pnames = f->pnames; for (int i = 0; i < f->ty->nparams; i++) if (f->pdefault[i]) same->pdefault[i] = f->pdefault[i]; f = same; } }
                else { g = prev->fn; while (g->next_overload) g = g->next_overload; g->next_overload = f; add_func(f); }
            } else {
                Scope *s = scope; scope = global_scope;
                Var *v = add_var(inst_name ? inst_name : nm, f->ty, 0); v->fn = f;
                if (!strcmp(nm, "operator<") && f->ty->nparams == 2) { CType *p0 = f->ty->params[0]; if (p0->kind == TY_REF) p0 = p0->base; p0 = unconst(p0); if (p0->kind == TY_STRUCT && !p0->op_lt) p0->op_lt = f; }
                if (inst_name) f->name = inst_name;
                scope = s;
                add_func(f);
            }
            if (at("{")) {
                if (cur_fn) syntax("a function can't be defined inside another");
                if (f->body) syntax("%s() is defined twice", nm);
                function_body(f, f->ty, 0);
                return list;
            }
            if (!accept(",")) break;
            continue;
        }
        if (strchr(nm, ':')) {
            /* Class::method(...) { } defined outside the class, or a static member's definition */
            char cls[100]; snprintf(cls, sizeof cls, "%.*s", (int)(strchr(nm, ':') - nm), nm);
            const char *mn = strrchr(nm, ':') + 1;
            Var *cv = find_var(cls);
            if (!cv || !cv->is_typedef || cv->ty->kind != TY_STRUCT) syntax("'%s' isn't a class", cls);
            CType *ct = cv->ty;
            if (at("(")) {
                CFunc *proto = zalloc(sizeof(CFunc));
                CType *saved = cur_class; cur_class = ct;
                CType *ft = func_params(t, proto);
                while (accept("const") || accept("noexcept") || accept("override")) { }
                int is_d = mn[0] == '~';
                CFunc *f = 0;
                for (int i = 0; i < ct->nmeth; i++) {
                    CFunc *g = ct->meth[i];
                    int match = is_d ? g->is_dtor : !strcmp(g->name, !strcmp(mn, ct->name) ? ct->name : mn) && g->ty->nparams == ft->nparams;
                    if (match && !g->body) { f = g; break; }
                }
                if (!f) syntax("%s has no declared method %s with %d parameter%s", cls, mn, ft->nparams, ft->nparams == 1 ? "" : "s");
                f->pnames = proto->pnames;
                Scope *s = scope; scope = global_scope;
                function_body(f, f->ty, 0);
                scope = s;
                cur_class = saved;
                return list;
            }
            Var *sv = find_var(nm);
            if (sv && accept("=")) {
                CNode *dn = node(C_DECL); dn->off = sv->off; dn->global = 1; dn->ty = sv->ty; dn->init = typed_init(sv->ty, assign());
                global_init = rt_realloc(global_init, sizeof(CNode *) * (size_t)nginit, sizeof(CNode *) * (size_t)(nginit + 1));
                global_init[nginit++] = dn;
            }
            if (!accept(",")) break;
            continue;
        }
        if (t->kind == TY_VOID) syntax("variable '%s' declared void", nm);
        if (t->kind == TY_STRUCT && !t->complete) syntax("'%s' has an incomplete type", nm);
        CNode *init = 0;
        int static_local = is_static && cur_fn;
        /* the name is known from here on: int *p = malloc(sizeof *p) */
        Var *v = add_var(nm, t, cur_fn && !static_local);
        if (accept("=")) init = at("{") ? initializer_list(0) : assign();
        else if (at("{") && is_cpp) init = initializer_list(0);
        else if (at("(") && cur_fn) {
            /* Point p(1, 2); vector<int> v(10); */
            next();
            CNode *ci = node(C_INIT);
            while (!at(")")) { add_kid(ci, assign()); if (!accept(",")) break; }
            expect(")");
            init = ci;
            if (ty_is_num(t) || t->kind == TY_PTR) init = ci->nk ? ci->kids[0] : 0;
        } else if (t->kind == TY_STRUCT && t->has_ctor) { init = node(C_INIT); }
        if (t->kind == TY_ARRAY && t->len < 0) {
            /* int a[] = {...}, char s[] = "..." */
            if (!init) syntax("'%s' needs a size", nm);
            int n = init->k == C_INIT ? init->nk : init->k == C_STR ? (int)init->ival + 1 : 0;
            if (init->k == C_INIT) for (int i = 0; i < init->nk; i++) if (init->kids[i]->k == C_NOP && init->kids[i]->flag == 1 && init->kids[i]->ival + 1 > n) n = (int)init->kids[i]->ival + 1;
            t = ty_array(t->base, n);
        }
        if (t->kind == TY_ARRAY && t->size > (int)(cc_memsz / 8)) syntax("the array %s is too big", nm);
        if (t->kind == TY_ARRAY && cur_fn && t->size > (2 << 20) && !static_local) syntax("the array %s is too big for the stack: make it global or static, or malloc it", nm);
        v->ty = t;
        CNode *d = node(C_DECL); d->ty = t; d->name = nm;
        if (cur_fn && !static_local) { v->off = alloc_local(t); d->off = v->off; }
        else { v->off = (int64_t)alloc_global(t->size, t->align); d->off = v->off; d->global = 1; }
        if (t->kind == TY_REF && !init) syntax("the reference %s needs something to refer to", nm);
        d->init = init ? typed_init(t, init) : 0;
        if (static_local) d->flag = 1;                 /* initialized once */
        if (cur_fn) { note_dtor(scope->block, d->off, t); add_kid(list, d); }
        else { global_init = rt_realloc(global_init, sizeof(CNode *) * (size_t)nginit, sizeof(CNode *) * (size_t)(nginit + 1)); global_init[nginit++] = d; }
        if (!accept(",")) break;
    }
    expect(";");
    return list;
}

static void parse_toplevel(void) {
    cc_line = cur()->line;
    if (accept(";")) return;
    if (cur()->k == K_ID && !strcmp(cur()->s, "template")) { next(); template_decl(); return; }
    if (cur()->k == K_ID && !strcmp(cur()->s, "namespace")) {
        next(); if (cur()->k == K_ID) ident();
        expect("{");
        while (!at("}")) { if (cur()->k == K_EOF) syntax("the namespace never ends"); parse_toplevel(); }
        next();
        return;
    }
    if (cur()->k == K_ID && !strcmp(cur()->s, "extern") && at_k(1, "\"") ) { next(); }
    if (cur()->k == K_ID && !strcmp(cur()->s, "extern") && peekt(1)->k == K_STR) {
        next(); next();
        if (accept("{")) { while (!at("}")) parse_toplevel(); next(); }
        return;
    }
    if (cur()->k == K_ID && !strcmp(cur()->s, "using")) { declaration(1); return; }
    if (!is_decl_start() && !(cur()->k == K_ID && at_k(1, "("))) syntax("expected a declaration before '%.*s'", cur()->len, cur()->s);
    if (cur()->k == K_ID && at_k(1, "(") && !is_type_name(cur())) {
        /* main() with no return type, as old C allows */
        if (!strcmp(cur()->s, "main")) { Tok *t = cur(); (void)t; }
        syntax("'%s' needs a return type (int %s?)", cur()->s, cur()->s);
    }
    declaration(1);
}

/* ---- files -------------------------------------------------------------------------------------------------------- */
static int read_include(const char *name, char **src) {
    char path[300];
    const char *dir = rt_cwd();
    snprintf(path, sizeof path, "%s/%s", strcmp(dir, "/") ? dir : "", name);
    uint32_t len; const char *why;
    if (rt_file_read(path, src, &len, &why) == 0) return 0;
    /* <bits/stdc++.h> and the like, quoted: built in */
    static const char *const known[] = { "stdio.h", "stdlib.h", "string.h", "math.h", "ctype.h", "time.h", "stdbool.h",
        "limits.h", "assert.h", "stdint.h", "iostream", "vector", "string", "algorithm", "map", "set", 0 };
    for (int i = 0; known[i]; i++) if (!strcmp(name, known[i])) return -1;
    cc_error("can't find \"%s\" next to the program", name);
}

static const char CPP_PRELUDE[] =
    "class exception { public: string msg; exception() {} exception(const string& m) : msg(m) {}\n"
    "  virtual string what() const { return msg; } };\n"
    "class logic_error : public exception { public: logic_error(const string& m) : exception(m) {} };\n"
    "class runtime_error : public exception { public: runtime_error(const string& m) : exception(m) {} };\n"
    "class invalid_argument : public logic_error { public: invalid_argument(const string& m) : logic_error(m) {} };\n"
    "class out_of_range : public logic_error { public: out_of_range(const string& m) : logic_error(m) {} };\n"
    "class length_error : public logic_error { public: length_error(const string& m) : logic_error(m) {} };\n"
    "class domain_error : public logic_error { public: domain_error(const string& m) : logic_error(m) {} };\n"
    "class overflow_error : public runtime_error { public: overflow_error(const string& m) : runtime_error(m) {} };\n"
    "class bad_alloc : public exception { public: bad_alloc() : exception(\"bad_alloc\") {} };\n"
    "template <typename T> struct less { bool operator()(const T& a, const T& b) const { return a < b; } };\n"
    "template <typename T> struct greater { bool operator()(const T& a, const T& b) const { return b < a; } };\n"
    "template <typename T> class stack { vector<T> v; public:\n"
    "  void push(const T& x) { v.push_back(x); } void pop() { if (v.empty()) throw out_of_range(\"stack is empty\"); v.pop_back(); }\n"
    "  T& top() { if (v.empty()) throw out_of_range(\"stack is empty\"); return v.back(); }\n"
    "  bool empty() const { return v.empty(); } size_t size() const { return v.size(); } };\n"
    "template <typename T> class queue { vector<T> v; size_t h = 0; public:\n"
    "  void push(const T& x) { v.push_back(x); }\n"
    "  void pop() { if (h >= v.size()) throw out_of_range(\"queue is empty\"); h++; if (h > 64 && h * 2 > v.size()) { v.erase(v.begin(), v.begin() + h); h = 0; } }\n"
    "  T& front() { return v[h]; } T& back() { return v.back(); }\n"
    "  bool empty() const { return h >= v.size(); } size_t size() const { return v.size() - h; } };\n"
    "template <typename T, typename C = vector<T>, typename Cmp = less<T> > class priority_queue { vector<T> v; Cmp cmp; public:\n"
    "  void push(const T& x) { v.push_back(x); size_t i = v.size() - 1;\n"
    "    while (i > 0) { size_t p = (i - 1) / 2; if (!cmp(v[p], v[i])) break; T t = v[p]; v[p] = v[i]; v[i] = t; i = p; } }\n"
    "  const T& top() { if (v.empty()) throw out_of_range(\"priority_queue is empty\"); return v[0]; }\n"
    "  void pop() { if (v.empty()) throw out_of_range(\"priority_queue is empty\"); v[0] = v.back(); v.pop_back(); size_t i = 0, n = v.size();\n"
    "    for (;;) { size_t l = 2 * i + 1, r = l + 1, m = i; if (l < n && cmp(v[m], v[l])) m = l; if (r < n && cmp(v[m], v[r])) m = r;\n"
    "      if (m == i) break; T t = v[m]; v[m] = v[i]; v[i] = t; i = m; } }\n"
    "  bool empty() const { return v.empty(); } size_t size() const { return v.size(); } };\n";

static void parse_source(const char *src, const char *name) {
    TV raw = { 0 }, pp = { 0 };
    const char *saved_file = cur_file;
    cur_file = name;
    tokenize(src, &raw);
    preprocess(raw.t, raw.n, &pp, 0);
    Tok e; memset(&e, 0, sizeof e); e.k = K_EOF; e.s = ""; e.line = raw.n ? raw.t[raw.n - 1].line : 1; e.hide = -1;
    tv_push(&pp, e);
    tk = pp.t; ntk = pp.n; pos = 0;
    cc_line = tk[0].line;
    while (cur()->k != K_EOF) parse_toplevel();
    cur_file = saved_file;
}

uint64_t cc_data_top(void);
uint64_t cc_data_top(void) { return data_top; }

int cc_main(const char *src, const char *path, int cpp) {
    is_cpp = cpp;
    cur_file = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    nmacros = macro_cap = 0; macros = 0; nfuncs = func_cap = 0; funcs = 0; templates = 0; classes = 0; nclasses = 0;
    global_init = 0; nginit = ginit_cap = 0; ntmpl_bind = 0; inst_name = 0; lambda_depth = 0; cur_fn = 0; cur_class = 0;
    /* the parser's own pointers into the collected heap */
    rt_root((void **)&funcs); rt_root((void **)&templates); rt_root((void **)&classes); rt_root((void **)&global_init);
    rt_root((void **)&macros); rt_root((void **)&tk); rt_root((void **)&global_scope); rt_root((void **)&scope);
    rt_root((void **)&cur_fn); rt_root((void **)&cur_class); rt_root((void **)&lambda_ret); rt_root((void **)&inst_name); rt_root((void **)&cur_inst_type);
    cur_inst_type = 0; cur_inst_base = 0;
    rt_root((void **)&tmpl_bind_ty); rt_root((void **)&tmpl_bind_name);
    tmpl_bind_ty = rt_alloc(sizeof(CType *) * 64); tmpl_bind_name = rt_alloc(sizeof(char *) * 64);
    scope = 0;
    cc_types_reset();
    /* the program's memory */
    uint64_t want = rt_heap_size() / 3;
    if (want > (64u << 20)) want = 64u << 20;
    if (want < (8u << 20)) want = 8u << 20;
    cc_memsz = want;
    cc_mem = rt_alloc_leaf(cc_memsz);
    rt_root((void **)&cc_mem);
    data_top = 4096;
    cc_lib_init();
    push_scope();
    global_scope = scope;
    static const char *const defs[][2] = {
        { "EOF", "(-1)" }, { "INT_MAX", "2147483647" }, { "INT_MIN", "(-2147483647-1)" }, { "UINT_MAX", "4294967295U" },
        { "LONG_MAX", "9223372036854775807L" }, { "LONG_MIN", "(-9223372036854775807L-1)" }, { "LLONG_MAX", "9223372036854775807LL" },
        { "LLONG_MIN", "(-9223372036854775807LL-1)" }, { "ULLONG_MAX", "18446744073709551615ULL" }, { "ULONG_MAX", "18446744073709551615UL" },
        { "SHRT_MAX", "32767" }, { "SHRT_MIN", "(-32768)" }, { "CHAR_MAX", "127" }, { "CHAR_MIN", "(-128)" }, { "UCHAR_MAX", "255" },
        { "CHAR_BIT", "8" }, { "SIZE_MAX", "18446744073709551615UL" }, { "RAND_MAX", "2147483647" }, { "EXIT_SUCCESS", "0" },
        { "EXIT_FAILURE", "1" }, { "CLOCKS_PER_SEC", "1000000L" }, { "M_PI", "3.14159265358979323846" }, { "M_E", "2.7182818284590452354" },
        { "M_SQRT2", "1.41421356237309504880" }, { "INFINITY", "(1.0/0.0)" }, { "NAN", "(0.0/0.0)" }, { "DBL_MAX", "1.7976931348623157e308" },
        { "DBL_MIN", "2.2250738585072014e-308" }, { "DBL_EPSILON", "2.220446049250313e-16" }, { "FLT_MAX", "3.402823466e+38F" },
        { "FLT_EPSILON", "1.192092896e-07F" }, { "INT32_MAX", "2147483647" }, { "INT64_MAX", "9223372036854775807L" },
        { "UINT32_MAX", "4294967295U" }, { "BUFSIZ", "8192" }, { "stdin", "((FILE*)0)" }, { "stdout", "((FILE*)1)" },
        { "stderr", "((FILE*)2)" }, { "__STDC__", "1" }, { "__BOLTOS__", "1" }, { "assert(x)", "" }, { 0, 0 } };
    for (int i = 0; defs[i][0]; i++) if (strcmp(defs[i][0], "assert(x)")) def_macro(defs[i][0], defs[i][1]);
    if (cpp) def_macro("__cplusplus", "201703L");
    else { def_macro("true", "1"); def_macro("false", "0"); def_macro("bool", "_Bool"); }
    if (cpp) parse_source(CPP_PRELUDE, "<prelude>");
    parse_source(src, cur_file);
    Var *m = find_var("main");
    if (!m || !m->fn || !m->fn->body) { cc_line = 0; cc_error("there's no main() function"); }
    /* globals' initializers run first, then main */
    return cc_run_program(m->fn, global_init, nginit);
}
