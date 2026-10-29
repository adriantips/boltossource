/* ===========================================================================
 *  BoltOS Mini  -  mini/cc_lib.c
 *
 *  The library a C or C++ program from the Code app finds (cc.h): stdio's
 *  printf and scanf families (numbers printed exactly, numfmt.c), string.h,
 *  stdlib.h, math.h (mathx.c's), ctype.h and time.h; and for C++, iostream
 *  with iomanip's manipulators, std::string, vector, map and set, pair, and
 *  the <algorithm>s people reach for.
 *
 *  The containers keep { data, size, capacity } in the program's memory,
 *  elements side by side: a set or a map is kept sorted, so lookups are a
 *  binary search, iteration is in order (as std::map's is), and every
 *  iterator is a plain pointer to an element.
 * ===========================================================================*/
#include <stdint.h>
#include <stdarg.h>
#include "cc.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "math.h"
#define MATHX_REPLACE
#include "mathx.h"

static inline uint64_t ld64(uint64_t a) { uint64_t v; memcpy(&v, cc_at(a, 8, 0), 8); return v; }
static inline void st64(uint64_t a, uint64_t v) { memcpy(cc_at(a, 8, 1), &v, 8); }
static const char *cstr(uint64_t a) {
    /* a C string in program memory, checked to its end */
    if (a < 4096) cc_error("Segmentation fault: a null pointer used as a string");
    uint64_t i = a;
    while (*cc_at(i, 1, 0)) i++;
    return (const char *)cc_mem + a;
}

/* ---- output -------------------------------------------------------------------------------------------- */
static char obuf[4096]; static int on;
void cc_flush(void) { if (on) { rt_write(obuf, (size_t)on); on = 0; } }
void cc_out(const char *s, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        obuf[on++] = s[i];
        if (on == (int)sizeof obuf || s[i] == '\n') cc_flush();
    }
}

/* ---- input: a line at a time from the Code app ----------------------------------------------------------- */
static char ibuf[4096]; static int ipos, ilen, ieof;
static int unget_c = -1;
int cc_stdin_getc(void *ctx) {
    (void)ctx;
    if (unget_c >= 0) { int c = unget_c; unget_c = -1; return c; }
    if (ipos >= ilen) {
        if (ieof) return -1;
        cc_flush();
        char line[4000];
        int n = rt_readline("", line, sizeof line);
        if (n < 0) { ieof = 1; return -1; }
        rt_write(line, (size_t)n); rt_write("\n", 1);     /* what was typed stays in the output */
        memcpy(ibuf, line, (size_t)n); ibuf[n] = '\n';
        ilen = n + 1; ipos = 0;
    }
    return (unsigned char)ibuf[ipos++];
}
void cc_stdin_unget(int c) { unget_c = c; }

/* ---- printf ------------------------------------------------------------------------------------------------ */
typedef struct { char *b; int n, cap; } OB;
static void ob_c(OB *o, char c) { if (o->n < o->cap - 1) o->b[o->n] = c; o->n++; }
static void ob_s(OB *o, const char *s, int n) { for (int i = 0; i < n; i++) ob_c(o, s[i]); }
static void ob_pad(OB *o, int n, char c) { for (int i = 0; i < n; i++) ob_c(o, c); }

int cc_format(char *out, int cap, const char *f, CV *args, int nargs, CType **types) {
    OB o = { out, 0, cap };
    int ai = 0;
    for (; *f; f++) {
        if (*f != '%') { ob_c(&o, *f); continue; }
        f++;
        if (*f == '%') { ob_c(&o, '%'); continue; }
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0, width = -1, prec = -1;
        for (;; f++) {
            if (*f == '-') left = 1; else if (*f == '+') plus = 1; else if (*f == ' ') space = 1;
            else if (*f == '#') alt = 1; else if (*f == '0') zero = 1; else break;
        }
        if (*f == '*') { width = ai < nargs ? (int)args[ai++].i : 0; if (width < 0) { left = 1; width = -width; } f++; }
        else { while (*f >= '0' && *f <= '9') { if (width < 0) width = 0; width = width * 10 + (*f++ - '0'); } }
        if (*f == '.') { f++; prec = 0; if (*f == '*') { prec = ai < nargs ? (int)args[ai++].i : 0; f++; } else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
        int lng = 0;
        while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'j' || *f == 't' || *f == 'L' || *f == 'q') { if (*f == 'l' || *f == 'z' || *f == 'j' || *f == 't' || *f == 'q') lng++; if (*f == 'h') lng--; f++; }
        char c = *f;
        if (!c) break;
        if (ai >= nargs && c != 'n') { ob_s(&o, "(missing)", 9); continue; }
        CV a = args[ai];
        CType *t = types ? types[ai] : 0;
        ai++;
        char tmp[600]; int tn = 0;
        const char *sign = "";
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': case 'p': {
            if (t && ty_is_flt(t)) { cc_error("printf %%%c given a floating point number: use %%f (or cast it to int)", c); }
            if (t && t->kind == TY_STRING) cc_error("printf %%%c given a string", c);
            if (c == 'c') { tmp[0] = (char)a.i; tn = 1; prec = -1; zero = 0; break; }
            uint64_t v = a.u;
            int neg = 0;
            if (c == 'd' || c == 'i') {
                int64_t x = lng > 0 ? a.i : lng < 0 ? (lng < -1 ? (int8_t)a.i : (int16_t)a.i) : (int32_t)a.i;
                if (t && ty_is_int(t) && t->size == 8 && lng <= 0) x = a.i;
                neg = x < 0; v = neg ? 0 - (uint64_t)x : (uint64_t)x;
                sign = neg ? "-" : plus ? "+" : space ? " " : "";
            } else if (lng <= 0 && !(t && t->size == 8)) v = lng < 0 ? (lng < -1 ? (uint8_t)a.u : (uint16_t)a.u) : (uint32_t)a.u;
            int base = c == 'x' || c == 'X' || c == 'p' ? 16 : c == 'o' ? 8 : 10;
            char d[70]; int dn = 0;
            do { int dg = (int)(v % (uint64_t)base); d[dn++] = (char)(dg < 10 ? '0' + dg : (c == 'X' ? 'A' : 'a') + dg - 10); v /= (uint64_t)base; } while (v);
            if (prec == 0 && a.u == 0 && c != 'p') dn = 0;
            if (c == 'p') { tmp[tn++] = '0'; tmp[tn++] = 'x'; }
            else if (alt && base == 16 && a.u) { tmp[tn++] = '0'; tmp[tn++] = c; }
            else if (alt && base == 8) tmp[tn++] = '0';
            for (int z = dn; z < prec; z++) tmp[tn++] = '0';
            while (dn) tmp[tn++] = d[--dn];
            if (prec >= 0) zero = 0;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double x = t && ty_is_int(t) ? (double)a.i : a.d;
            if (t && ty_is_int(t)) cc_error("printf %%%c given an integer: use %%d (or cast it to double)", c);
            int neg = __builtin_signbit(x) && x == x;
            if (neg) x = -x;
            sign = neg ? "-" : plus ? "+" : space ? " " : "";
            if (c == 'f' || c == 'F') tn = num_fixed(x, prec < 0 ? 6 : prec, tmp, sizeof tmp);
            else if (c == 'e' || c == 'E') tn = num_exp(x, prec < 0 ? 6 : prec, tmp, sizeof tmp, c == 'E');
            else tn = num_general(x, prec < 0 ? 6 : prec, tmp, sizeof tmp, c == 'G', alt);
            if (tn >= (int)sizeof tmp) tn = (int)sizeof tmp - 1;
            if (c == 'F' || c == 'E' || c == 'G') for (int z = 0; z < tn; z++) if (tmp[z] >= 'a' && tmp[z] <= 'z') tmp[z] = (char)(tmp[z] - 32);
            if (x != x || x == __builtin_inf()) zero = 0;
            prec = -1;
            break;
        }
        case 's': {
            if (t && t->kind == TY_STRING) cc_error("printf %%s given a std::string: use s.c_str()");
            if (t && !ty_is_ptr(t) && t->kind != TY_ARRAY) cc_error("printf %%s given a number, not a string");
            const char *s = a.u ? cstr(a.u) : "(null)";
            int n = (int)strlen(s);
            if (prec >= 0 && prec < n) n = prec;
            if (width > n && !left) ob_pad(&o, width - n, ' ');
            ob_s(&o, s, n);
            if (width > n && left) ob_pad(&o, width - n, ' ');
            continue;
        }
        case 'n': if (ai - 1 < nargs) { uint64_t p = args[ai - 1].u; int32_t w = o.n; memcpy(cc_at(p, 4, 1), &w, 4); } continue;
        default: ob_c(&o, '%'); ob_c(&o, c); continue;
        }
        int sl = (int)strlen(sign), total = sl + tn;
        if (width > total && !left && !zero) ob_pad(&o, width - total, ' ');
        ob_s(&o, sign, sl);
        if (width > total && !left && zero) ob_pad(&o, width - total, '0');
        ob_s(&o, tmp, tn);
        if (width > total && left) ob_pad(&o, width - total, ' ');
    }
    if (cap > 0) out[o.n < cap - 1 ? o.n : cap - 1] = 0;
    return o.n;
}

/* ---- scanf ------------------------------------------------------------------------------------------------ */
typedef struct { const char *s; int i; } SS;
static int str_getc(void *ctx) { SS *x = ctx; return x->s[x->i] ? (unsigned char)x->s[x->i++] : -1; }
static int peeked = -2;

int cc_scanf(const char *f, CV *args, int nargs, int (*gc)(void *), void *ctx) {
    int got = 0, ai = 0, c = -2;
    #define GET() (c = gc(ctx))
    #define UNGET() do { if (gc == cc_stdin_getc) cc_stdin_unget(c); else ((SS *)ctx)->i--; } while (0)
    for (; *f; f++) {
        if (*f == ' ' || *f == '\t' || *f == '\n') {
            do GET(); while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
            if (c >= 0) UNGET();
            continue;
        }
        if (*f != '%') {
            GET();
            if (c != (unsigned char)*f) { if (c >= 0) UNGET(); return got; }
            continue;
        }
        f++;
        int skip = 0, width = 0, lng = 0;
        if (*f == '*') { skip = 1; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        while (*f == 'l' || *f == 'h' || *f == 'L' || *f == 'z') { if (*f == 'l' || *f == 'L' || *f == 'z') lng++; if (*f == 'h') lng--; f++; }
        char conv = *f;
        if (conv == '%') { do GET(); while (c == ' ' || c == '\n' || c == '\t'); if (c != '%') return got; continue; }
        if (conv != 'c' && conv != '[') { do GET(); while (c == ' ' || c == '\t' || c == '\n' || c == '\r'); if (c < 0) return got ? got : -1; UNGET(); }
        char tok[512]; int tn = 0;
        if (!width) width = 511;
        if (conv == 'c') {
            GET(); if (c < 0) return got ? got : -1;
            if (!skip && ai < nargs) { *cc_at(args[ai].u, 1, 1) = (uint8_t)c; ai++; got++; }
            continue;
        }
        if (conv == 's') {
            while (tn < width && GET() >= 0 && c != ' ' && c != '\t' && c != '\n' && c != '\r') tok[tn++] = (char)c;
            if (c >= 0 && tn < width) UNGET();
            if (!tn) return got;
            if (!skip && ai < nargs) { memcpy(cc_at(args[ai].u, (uint64_t)tn + 1, 1), tok, (size_t)tn); cc_mem[args[ai].u + tn] = 0; ai++; got++; }
            continue;
        }
        if (conv == '[') {
            f++;
            int neg = *f == '^'; if (neg) f++;
            char set[256] = { 0 };
            if (*f == ']') { set[']'] = 1; f++; }
            while (*f && *f != ']') { if (f[1] == '-' && f[2] && f[2] != ']') { for (int z = (unsigned char)f[0]; z <= (unsigned char)f[2]; z++) set[z] = 1; f += 3; } else set[(unsigned char)*f++] = 1; }
            while (tn < width && GET() >= 0 && (set[c] != 0) != neg) tok[tn++] = (char)c;
            if (c >= 0) UNGET();
            if (!tn) return got;
            if (!skip && ai < nargs) { memcpy(cc_at(args[ai].u, (uint64_t)tn + 1, 1), tok, (size_t)tn); cc_mem[args[ai].u + tn] = 0; ai++; got++; }
            continue;
        }
        /* numbers: take the characters a number can have */
        int isf = conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G';
        int hex = conv == 'x' || conv == 'X';
        while (tn < width && GET() >= 0) {
            int ok = (c >= '0' && c <= '9') || ((c == '-' || c == '+') && (tn == 0 || (isf && (tok[tn - 1] | 32) == 'e')));
            if (isf) ok = ok || c == '.' || ((c | 32) == 'e' && tn) || (c | 32) == 'i' || (c | 32) == 'n' || (c | 32) == 'f' || (c | 32) == 'a';
            if (hex || conv == 'i') ok = ok || ((c | 32) >= 'a' && (c | 32) <= 'f') || (c | 32) == 'x';
            if (!ok) break;
            tok[tn++] = (char)c;
        }
        if (c >= 0 && !(tn >= width)) UNGET();
        tok[tn] = 0;
        if (!tn || (tn == 1 && (tok[0] == '-' || tok[0] == '+'))) return got;
        if (skip) continue;
        if (ai >= nargs) return got;
        uint64_t p = args[ai++].u;
        if (isf) {
            double d = num_parse(tok, 0);
            if (lng) memcpy(cc_at(p, 8, 1), &d, 8); else { float fl = (float)d; memcpy(cc_at(p, 4, 1), &fl, 4); }
        } else {
            int64_t v = (int64_t)strtoull(tok[0] == '-' ? tok + 1 : tok, 0, hex ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10);
            if (tok[0] == '-') v = -v;
            if (lng > 0) memcpy(cc_at(p, 8, 1), &v, 8);
            else if (lng < 0) { int16_t s = (int16_t)v; memcpy(cc_at(p, 2, 1), &s, 2); }
            else { int32_t i = (int32_t)v; memcpy(cc_at(p, 4, 1), &i, 4); }
        }
        got++;
    }
    return got;
    #undef GET
    #undef UNGET
}

/* ---- the library's functions ---------------------------------------------------------------------------- */
#define LIBS(X) \
    X(printf,'i') X(puts,'i') X(putchar,'i') X(scanf,'i') X(getchar,'i') X(sprintf,'i') X(snprintf,'i') X(fprintf,'i') \
    X(sscanf,'i') X(fputs,'i') X(fputc,'i') X(putc,'i') X(getc,'i') X(fgetc,'i') X(fflush,'i') X(fgets,'c') X(gets,'c') \
    X(strlen,'u') X(strcmp,'i') X(strncmp,'i') X(strcasecmp,'i') X(strcpy,'c') X(strncpy,'c') X(strcat,'c') X(strncat,'c') \
    X(strchr,'c') X(strrchr,'c') X(strstr,'c') X(strdup,'c') X(strtok,'c') X(memcpy,'p') X(memmove,'p') X(memset,'p') \
    X(memcmp,'i') X(malloc,'p') X(calloc,'p') X(realloc,'p') X(free,'v') X(exit,'v') X(abort,'v') X(atoi,'i') X(atol,'l') \
    X(atoll,'l') X(atof,'d') X(strtol,'l') X(strtoll,'l') X(strtoul,'u') X(strtoull,'u') X(strtod,'d') X(strtof,'f') \
    X(labs,'l') X(llabs,'l') X(rand,'i') X(srand,'v') X(time,'l') X(clock,'l') X(qsort,'v') X(bsearch,'p') X(assert,'v') \
    X(sqrt,'d') X(pow,'d') X(sin,'d') X(cos,'d') X(tan,'d') X(asin,'d') X(acos,'d') X(atan,'d') X(atan2,'d') X(exp,'d') \
    X(log,'d') X(log10,'d') X(log2,'d') X(floor,'d') X(ceil,'d') X(fabs,'d') X(fmod,'d') X(round,'d') X(trunc,'d') \
    X(hypot,'d') X(cbrt,'d') X(sinh,'d') X(cosh,'d') X(tanh,'d') X(fmin,'d') X(fmax,'d') X(copysign,'d') X(lround,'l') \
    X(sqrtf,'f') X(powf,'f') X(fabsf,'f') X(sinf,'f') X(cosf,'f') X(floorf,'f') X(ceilf,'f') X(roundf,'f') X(expf,'f') X(logf,'f') \
    X(isnan,'i') X(isinf,'i') X(isfinite,'i') \
    X(toupper,'i') X(tolower,'i') X(isdigit,'i') X(isalpha,'i') X(isalnum,'i') X(isspace,'i') X(isupper,'i') X(islower,'i') \
    X(ispunct,'i') X(isxdigit,'i') X(isprint,'i') X(iscntrl,'i') X(isgraph,'i') X(sleep,'i') X(usleep,'i') \
    X(to_string,'s') X(stoi,'i') X(stol,'l') X(stoll,'l') X(stoul,'u') X(stod,'d') X(stof,'f') X(getline,'b') \
    X(swap,'v') X(min,'g') X(max,'g') X(abs,'g') X(sort,'v') X(stable_sort,'v') X(reverse,'v') X(fill,'v') X(iota,'v') \
    X(find,'g') X(count,'l') X(count_if,'l') X(accumulate,'g') X(max_element,'g') X(min_element,'g') X(make_pair,'g') \
    X(lower_bound,'g') X(upper_bound,'g') X(binary_search,'b') X(next_permutation,'b') X(prev_permutation,'b') \
    X(unique,'g') X(gcd,'l') X(__gcd,'l') X(lcm,'l') X(distance,'l') X(all_of,'b') X(any_of,'b') X(none_of,'b') \
    X(for_each,'v') X(transform,'g') X(find_if,'g') X(remove,'g') X(remove_if,'g') X(random_shuffle,'v') X(shuffle,'v') \
    X(advance,'v') X(next,'g') X(prev,'g') X(begin,'g') X(end,'g') X(equal,'b') X(copy,'g') X(rotate,'v') X(clamp,'g') \
    X(fixed_,'v') X(rand_r,'i') X(nth_element,'v') X(partial_sort,'v') X(minmax,'g') X(sum_,'v') X(exit_,'v')

#define L_ENUM(n, r) L_##n,
#define L_NAME(n, r) #n,
#define L_RET(n, r) r,
enum { L_NONE, LIBS(L_ENUM) L_COUNT };
static const char *const lib_names[] = { "", LIBS(L_NAME) };
static const char lib_ret[] = { 0, LIBS(L_RET) };

int cc_lib_find(const char *name) {
    for (int i = 1; i < L_COUNT; i++) if (!strcmp(lib_names[i], name)) return i;
    return 0;
}
const char *cc_lib_name(int id) { return id > 0 && id < L_COUNT ? lib_names[id] : "?"; }

static CType **lib_types;
CType *cc_lib_type(int id) {
    if (!lib_types[id]) {
        CType *ft = rt_alloc(sizeof(CType));
        ft->kind = TY_FUNC; ft->size = 1; ft->align = 1; ft->variadic = 1;
        switch (lib_ret[id]) {
        case 'i': ft->ret = ty_int; break; case 'l': ft->ret = ty_long; break; case 'u': ft->ret = ty_ulong; break;
        case 'd': ft->ret = ty_double; break; case 'f': ft->ret = ty_float; break; case 'v': ft->ret = ty_void; break;
        case 'p': ft->ret = ty_ptr(ty_void); break; case 'c': ft->ret = ty_ptr(ty_char); break; case 's': ft->ret = ty_string; break;
        case 'b': ft->ret = ty_bool; break; default: ft->ret = ty_int;
        }
        lib_types[id] = ft;
    }
    return lib_types[id];
}

static CType *kty(CNode *n, int i) { return i < n->nk ? n->kids[i]->ty : ty_int; }
static CType *strip(CType *t) { return t && t->kind == TY_REF ? t->base : t; }

void cc_lib_fix_type(CNode *n) {
    CType *a = strip(kty(n, 0)), *b = strip(kty(n, 1));
    switch (n->op) {
    case L_min: case L_max: case L_clamp:
        if (ty_is_flt(a) || ty_is_flt(b)) n->ty = ty_double;
        else if (a->kind == TY_STRING || a->kind == TY_STRUCT || a->kind == TY_PAIR) n->ty = a;
        else if (ty_is_int(a) && ty_is_int(b)) n->ty = a->size >= b->size ? (a->size < 4 ? ty_int : a) : b;
        else n->ty = a;
        if (n->ty->kind == TY_ARRAY) n->ty = ty_ptr(n->ty->base);
        break;
    case L_abs: n->ty = ty_is_flt(a) ? ty_double : a->size == 8 ? ty_long : ty_int; break;
    case L_accumulate: n->ty = strip(kty(n, 2)); if (n->ty->kind == TY_ARRAY) n->ty = ty_string; if (ty_is_int(n->ty) && n->ty->size < 4) n->ty = ty_int; break;
    case L_find: case L_max_element: case L_min_element: case L_lower_bound: case L_upper_bound: case L_unique:
    case L_find_if: case L_remove: case L_remove_if: case L_next: case L_prev: case L_begin: case L_end:
        n->ty = a->kind == TY_ARRAY ? ty_ptr(a->base) : a;
        if (n->op == L_begin || n->op == L_end) { if (a->kind == TY_VECTOR || a->kind == TY_SET) n->ty = ty_ptr(a->base); if (a->kind == TY_STRING) n->ty = ty_ptr(ty_char); }
        break;
    case L_transform: case L_copy: n->ty = strip(kty(n, 2)); if (n->ty->kind == TY_ARRAY) n->ty = ty_ptr(n->ty->base); break;
    case L_make_pair: {
        CType *x = a->kind == TY_ARRAY ? (a->base->kind == TY_CHAR ? ty_string : ty_ptr(a->base)) : a;
        CType *y = b->kind == TY_ARRAY ? (b->base->kind == TY_CHAR ? ty_string : ty_ptr(b->base)) : b;
        if (x->is_const) { CType *c = rt_alloc(sizeof(CType)); *c = *x; c->is_const = 0; x = c->kind == TY_INT ? ty_int : c->kind == TY_DOUBLE ? ty_double : c; }
        if (y->is_const) { CType *c = rt_alloc(sizeof(CType)); *c = *y; c->is_const = 0; y = c->kind == TY_INT ? ty_int : c->kind == TY_DOUBLE ? ty_double : c; }
        n->ty = ty_pair(x, y);
        break;
    }
    case L_minmax: n->ty = ty_pair(a, a); break;
    }
}

/* ---- containers ------------------------------------------------------------------------------------------- */
static CType *elem_of(CType *t) { return t->kind == TY_MAP ? ty_pair(t->base, t->base2) : t->kind == TY_STRING ? ty_char : t->base; }
static uint64_t esize(CType *t) { return (uint64_t)elem_of(t)->size; }
static uint64_t vsize(uint64_t a) { return ld64(a + 8); }
static uint64_t vdata(uint64_t a) { return ld64(a); }

static void reserve(uint64_t a, uint64_t es, uint64_t n) {
    uint64_t cap = ld64(a + 16);
    if (n <= cap && ld64(a)) return;
    uint64_t nc = cap ? cap : 4;
    while (nc < n) nc *= 2;
    uint64_t d = cc_malloc(nc * es);
    if (!d) cc_error("out of memory (a container of %llu)", (unsigned long long)n);
    uint64_t old = ld64(a);
    if (old) { memcpy(cc_mem + d, cc_mem + old, (size_t)(ld64(a + 8) * es)); cc_free(old); }
    st64(a, d); st64(a + 16, nc);
}

/* a new element at index i, zeroed, the rest moved up: its address */
static uint64_t open_at(uint64_t a, uint64_t es, uint64_t i) {
    uint64_t n = vsize(a);
    reserve(a, es, n + 1);
    uint64_t d = vdata(a);
    memmove(cc_mem + d + (i + 1) * es, cc_mem + d + i * es, (size_t)((n - i) * es));
    memset(cc_mem + d + i * es, 0, (size_t)es);
    st64(a + 8, n + 1);
    return d + i * es;
}

static void put_elem(CType *et, uint64_t dst, CNode *src_node, CV v) {
    (void)src_node;
    if (et->kind == TY_STRUCT || et->kind == TY_STRING || et->kind == TY_VECTOR || et->kind == TY_MAP || et->kind == TY_SET || et->kind == TY_PAIR) cc_copy_value(et, dst, v.u);
    else if (et->kind == TY_LAMBDA) memcpy(cc_at(dst, 16, 1), cc_at(v.u, 16, 0), 16);
    else cc_store(et, dst, v);
}

static int is_objk(CType *t) { int k = t->kind; return k == TY_STRUCT || k == TY_STRING || k == TY_VECTOR || k == TY_MAP || k == TY_SET || k == TY_PAIR || k == TY_LAMBDA; }

int cc_compare(CType *t, uint64_t a, uint64_t b) {
    switch (t->kind) {
    case TY_STRING: {
        uint64_t na, nb; const char *x = cc_str_ptr(a, &na), *y = cc_str_ptr(b, &nb);
        uint64_t n = na < nb ? na : nb;
        int c = memcmp(x, y, (size_t)n);
        return c ? (c < 0 ? -1 : 1) : na < nb ? -1 : na > nb;
    }
    case TY_PAIR: { int c = cc_compare(t->mem[0].ty, a, b); if (c) return c; return cc_compare(t->mem[1].ty, a + (uint64_t)t->mem[1].offset, b + (uint64_t)t->mem[1].offset); }
    case TY_VECTOR: {
        uint64_t na = vsize(a), nb = vsize(b), es = esize(t);
        for (uint64_t i = 0; i < na && i < nb; i++) { int c = cc_compare(t->base, vdata(a) + i * es, vdata(b) + i * es); if (c) return c; }
        return na < nb ? -1 : na > nb;
    }
    case TY_STRUCT: {
        CFunc *f = t->op_lt;
        if (!f) cc_error("%s has no operator< (sorting it, or keeping it in a set or a map's keys, needs one)", t->name);
        CV x[2] = { { .u = a }, { .u = b } }, y[2] = { { .u = b }, { .u = a } };
        int lt = f->owner ? (int)cc_call(f, &x[1], 1, a, 0).u : (int)cc_call(f, x, 2, 0, 0).u;
        if (lt & 1) return -1;
        int gt = f->owner ? (int)cc_call(f, &y[1], 1, b, 0).u : (int)cc_call(f, y, 2, 0, 0).u;
        return (gt & 1) ? 1 : 0;
    }
    }
    CV x = cc_load(t, a), y = cc_load(t, b);
    if (ty_is_flt(t)) return x.d < y.d ? -1 : x.d > y.d;
    if (t->kind == TY_ULONG || t->kind == TY_UINT || t->kind == TY_PTR) return x.u < y.u ? -1 : x.u > y.u;
    return x.i < y.i ? -1 : x.i > y.i;
}

/* binary search in a sorted set or map: where key is, or would go */
static uint64_t sorted_find(CType *t, uint64_t a, uint64_t key, int *found) {
    CType *kt = t->kind == TY_MAP ? t->base : t->base;
    uint64_t lo = 0, hi = vsize(a), es = esize(t), d = vdata(a);
    *found = 0;
    while (lo < hi) {
        uint64_t m = (lo + hi) / 2;
        int c = cc_compare(kt, d + m * es, key);
        if (c == 0) { *found = 1; return m; }
        if (c < 0) lo = m + 1; else hi = m;
    }
    return lo;
}

/* a key's slot in a map, made (with a default value) if missing */
static uint64_t map_at(CType *t, uint64_t a, uint64_t key, int create) {
    int found;
    uint64_t i = sorted_find(t, a, key, &found);
    CType *pt = ty_pair(t->base, t->base2);
    uint64_t es = (uint64_t)pt->size;
    if (found) return vdata(a) + i * es + (uint64_t)pt->mem[1].offset;
    if (!create) cc_error("map::at: the key isn't in the map");
    uint64_t e = open_at(a, es, i);
    cc_copy_value(t->base, e, key);
    if (t->base2->kind == TY_STRUCT) cc_construct_default(t->base2, e + (uint64_t)pt->mem[1].offset);
    return e + (uint64_t)pt->mem[1].offset;
}

static int set_insert(CType *t, uint64_t a, uint64_t src) {
    int found;
    CType *et = elem_of(t);
    uint64_t key = src;
    uint64_t i = sorted_find(t, a, key, &found);
    if (found) return 0;
    uint64_t e = open_at(a, (uint64_t)et->size, i);
    cc_copy_value(et, e, src);
    return 1;
}

/* a value of type t from v, kept somewhere it has an address */
static uint64_t addr_of_value(CType *t, CV v) {
    if (is_objk(t)) return v.u;
    uint64_t tmp = cc_temp(t);
    cc_store(t, tmp, v);
    return tmp;
}

/* x converted to element type et, where x is the value of a node of type xt */
static CV conv_to(CType *et, CType *xt, CV x) {
    xt = strip(xt);
    if (ty_is_flt(et) && !ty_is_flt(xt)) x.d = (double)x.i;
    else if (!ty_is_flt(et) && ty_is_flt(xt) && ty_is_num(et)) x.i = (int64_t)x.d;
    else if (et->kind == TY_STRING && xt->kind != TY_STRING) {
        uint64_t s = cc_temp(ty_string);
        if (xt->kind == TY_CHAR) { char c = (char)x.i; cc_str_set(s, &c, 1); }
        else { const char *p = cstr(x.u); cc_str_set(s, p, strlen(p)); }
        x.u = s;
    }
    return x;
}

void cc_lib_container_init(CType *t, uint64_t a, CNode *init) {
    CType *et = elem_of(t);
    if (t->kind == TY_STRING) {
        if (init->op == 2) {
            int64_t n = cc_eval(init->kids[0]).i; char c = (char)cc_eval(init->kids[1]).i;
            if (n < 0) cc_error("string(n, c) with a negative n");
            char *tmp = rt_alloc_leaf((size_t)n + 1); memset(tmp, c, (size_t)n);
            cc_str_set(a, tmp, (uint64_t)n);
        }
        return;
    }
    if (init->op == 1) { cc_copy_value(t, a, cc_eval(init->kids[0]).u); return; }
    if (init->op == 2) {
        int64_t n = init->nk ? cc_eval(init->kids[0]).i : 0;
        if (n < 0 || n > (1 << 28)) cc_error("vector(n) with n = %lld", (long long)n);
        reserve(a, (uint64_t)et->size, (uint64_t)n);
        CV fillv = init->nk > 1 ? cc_eval(init->kids[1]) : (CV){ .u = 0 };
        uint64_t d = vdata(a);
        for (int64_t i = 0; i < n; i++) {
            uint64_t e = d + (uint64_t)i * (uint64_t)et->size;
            memset(cc_mem + e, 0, (size_t)et->size);
            if (init->nk > 1) put_elem(et, e, 0, fillv);
            else if (et->kind == TY_STRUCT) cc_construct_default(et, e);
        }
        st64(a + 8, (uint64_t)n);
        return;
    }
    for (int i = 0; i < init->nk; i++) {
        CNode *k = init->kids[i];
        if (t->kind == TY_VECTOR) {
            uint64_t e = open_at(a, (uint64_t)et->size, vsize(a));
            cc_init_at(e, et, k);
        } else {
            uint64_t tmp = cc_temp(et);
            cc_init_at(tmp, et, k);
            if (t->kind == TY_SET) set_insert(t, a, tmp);
            else {
                int found;
                uint64_t ix = sorted_find(t, a, tmp, &found);
                if (!found) { uint64_t e = open_at(a, (uint64_t)et->size, ix); cc_copy_value(et, e, tmp); }
            }
        }
    }
}

/* ---- calling what was passed in: comparators, predicates ----------------------------------------------------- */
static CType *callee_params(CType *ct, int *np) {
    ct = strip(ct);
    if (ct->kind == TY_LAMBDA) { *np = ct->nparams; return (CType *)ct->params; }
    if (ct->kind == TY_PTR && ct->base->kind == TY_FUNC) { *np = ct->base->nparams; return (CType *)ct->base->params; }
    if (ct->kind == TY_STRUCT) { CFunc *op = cc_find_method(ct, "operator()", 0); if (op) { *np = op->ty->nparams; return (CType *)op->ty->params; } }
    *np = 0;
    return 0;
}

/* f(elements at addresses...) */
static CV call_with(CV f, CType *ct, CType *et, uint64_t *addrs, int n) {
    int np; CType **ps = (CType **)callee_params(ct, &np);
    CV args[4];
    for (int i = 0; i < n && i < 4; i++) {
        CType *pt = ps && i < np ? ps[i] : et;
        if (pt->kind == TY_REF || is_objk(et)) args[i].u = addrs[i];
        else {
            args[i] = cc_load(et, addrs[i]);
            if (ty_is_flt(pt) && !ty_is_flt(et)) args[i].d = (double)args[i].i;
        }
    }
    CType *rt = strip(ct)->kind == TY_LAMBDA ? strip(ct)->ret : 0;
    uint64_t rto = rt && is_objk(rt) ? cc_temp(rt) : 0;
    return cc_call_value(f, strip(ct), args, n, rto);
}

static int less_with(CV f, CType *ct, CType *et, uint64_t a, uint64_t b) {
    if (!ct) return cc_compare(et, a, b) < 0;
    uint64_t ad[2] = { a, b };
    return (call_with(f, ct, et, ad, 2).u & 1) != 0;
}

static void msort(CType *et, uint64_t base, uint64_t n, uint64_t tmp, CV f, CType *ct) {
    uint64_t es = (uint64_t)et->size;
    if (n < 2) return;
    if (n <= 8) {
        uint8_t *x = rt_alloc_leaf((size_t)es);
        for (uint64_t i = 1; i < n; i++) {
            uint64_t j = i;
            memcpy(x, cc_mem + base + i * es, (size_t)es);
            uint64_t xa = tmp;                              /* the element being placed, at a stable address */
            memcpy(cc_mem + xa, x, (size_t)es);
            while (j > 0 && less_with(f, ct, et, xa, base + (j - 1) * es)) { memcpy(cc_mem + base + j * es, cc_mem + base + (j - 1) * es, (size_t)es); j--; }
            memcpy(cc_mem + base + j * es, x, (size_t)es);
        }
        return;
    }
    rt_check();
    uint64_t m = n / 2;
    msort(et, base, m, tmp, f, ct);
    msort(et, base + m * es, n - m, tmp, f, ct);
    uint64_t i = 0, j = m, o = 0;
    while (i < m && j < n) {
        if (less_with(f, ct, et, base + j * es, base + i * es)) memcpy(cc_mem + tmp + o++ * es, cc_mem + base + j++ * es, (size_t)es);
        else memcpy(cc_mem + tmp + o++ * es, cc_mem + base + i++ * es, (size_t)es);
    }
    while (i < m) memcpy(cc_mem + tmp + o++ * es, cc_mem + base + i++ * es, (size_t)es);
    while (j < n) memcpy(cc_mem + tmp + o++ * es, cc_mem + base + j++ * es, (size_t)es);
    memcpy(cc_mem + base, cc_mem + tmp, (size_t)(n * es));
}

static void sort_range(CType *et, uint64_t lo, uint64_t hi, CV f, CType *ct) {
    uint64_t es = (uint64_t)et->size;
    if (hi < lo) cc_error("sort: the range ends before it starts");
    uint64_t n = (hi - lo) / es;
    if (n < 2) return;
    uint64_t tmp = cc_malloc(n * es + es);
    if (!tmp) cc_error("out of memory sorting %llu elements", (unsigned long long)n);
    msort(et, lo, n, tmp, f, ct);
    cc_free(tmp);
}

/* ---- iostream ----------------------------------------------------------------------------------------------- */
static struct { int prec, fixed, sci, width, left, boolalpha, base, showpoint; char fill; } io = { 6, 0, 0, 0, 0, 0, 10, 0, ' ' };
static int stream_ok = 1;
int cc_stream_ok(void) { return stream_ok; }

static void out_padded(const char *s, int n) {
    if (io.width > n && !io.left) for (int i = 0; i < io.width - n; i++) cc_out(&io.fill, 1);
    cc_out(s, (uint64_t)n);
    if (io.width > n && io.left) for (int i = 0; i < io.width - n; i++) cc_out(&io.fill, 1);
    io.width = 0;
}

static void print_value(CType *t, CV v) {
    char b[600]; int n;
    t = strip(t);
    if (t == ty_manip) {
        int code = (int)(v.u >> 32), arg = (int)(int32_t)v.u;
        switch (code) {
        case 0: cc_out("\n", 1); cc_flush(); break;                       /* endl */
        case 1: io.fixed = 1; io.sci = 0; break;
        case 2: io.sci = 1; io.fixed = 0; break;
        case 3: io.boolalpha = 1; break;
        case 4: io.boolalpha = 0; break;
        case 5: io.left = 1; break;
        case 6: io.left = 0; break;
        case 7: io.base = 16; break;
        case 8: io.base = 10; break;
        case 9: io.base = 8; break;
        case 10: io.showpoint = 1; break;
        case 11: cc_flush(); break;
        case 13: io.fixed = io.sci = 0; break;
        case 14: io.width = arg; break;
        case 15: io.prec = arg; break;
        case 16: io.fill = (char)arg; break;
        }
        return;
    }
    switch (t->kind) {
    case TY_BOOL: if (io.boolalpha) { out_padded(v.u ? "true" : "false", v.u ? 4 : 5); return; } n = snprintf(b, sizeof b, "%d", (int)(v.u != 0)); break;
    case TY_CHAR: case TY_UCHAR: b[0] = (char)v.i; n = 1; break;
    case TY_FLOAT: case TY_DOUBLE: {
        double d = v.d;
        int neg = __builtin_signbit(d) && d == d; if (neg) d = -d;
        char *p = b; if (neg) *p++ = '-';
        int cap = (int)sizeof b - 1;
        if (io.fixed) num_fixed(d, io.prec, p, cap);
        else if (io.sci) num_exp(d, io.prec, p, cap, 0);
        else num_general(d, io.prec ? io.prec : 1, p, cap, 0, io.showpoint);
        n = (int)strlen(b);
        break;
    }
    case TY_STRING: { uint64_t len; const char *s = cc_str_ptr(v.u, &len); out_padded(s, (int)len); return; }
    case TY_ARRAY: case TY_PTR:
        if (t->kind == TY_PTR && t->base->kind != TY_CHAR) { n = snprintf(b, sizeof b, "0x%llx", (unsigned long long)v.u); break; }
        if (!v.u) cc_error("cout << a null char pointer");
        { const char *s = cstr(v.u); out_padded(s, (int)strlen(s)); return; }
    case TY_NULLPTR: n = snprintf(b, sizeof b, "nullptr"); break;
    case TY_ULONG: case TY_UINT: case TY_USHORT:
        n = io.base == 16 ? snprintf(b, sizeof b, "%llx", (unsigned long long)v.u) : io.base == 8 ? snprintf(b, sizeof b, "%llo", (unsigned long long)v.u) : snprintf(b, sizeof b, "%llu", (unsigned long long)v.u);
        break;
    default:
        if (ty_is_int(t)) {
            n = io.base == 16 ? snprintf(b, sizeof b, "%llx", (unsigned long long)(t->size == 4 ? (uint32_t)v.i : v.u)) : io.base == 8 ? snprintf(b, sizeof b, "%llo", (unsigned long long)v.u) : snprintf(b, sizeof b, "%lld", (long long)v.i);
            break;
        }
        { char tn[80]; cc_error("cout can't print a %s", ty_str(t, tn, sizeof tn)); }
    }
    out_padded(b, n);
}

/* cin >> x: one word, as x's type reads it */
static void read_value(CType *t, uint64_t a) {
    char tok[4096]; int n = 0, c;
    do c = cc_stdin_getc(0); while (c == ' ' || c == '\n' || c == '\t' || c == '\r');
    if (c < 0) { stream_ok = 0; return; }
    if (t->kind == TY_CHAR || t->kind == TY_UCHAR) { *cc_at(a, 1, 1) = (uint8_t)c; return; }
    while (c >= 0 && c != ' ' && c != '\n' && c != '\t' && c != '\r' && n < 4095) { tok[n++] = (char)c; c = cc_stdin_getc(0); }
    if (c >= 0) cc_stdin_unget(c);
    tok[n] = 0;
    if (t->kind == TY_STRING) { cc_str_set(a, tok, (uint64_t)n); return; }
    if (ty_is_flt(t)) {
        const char *e; double d = num_parse(tok, &e);
        if (e == tok) { stream_ok = 0; return; }
        CV v; v.d = d; cc_store(t, a, v); return;
    }
    if (ty_is_int(t)) {
        char *e; int64_t v = strtoll(tok, &e, 10);
        if (e == tok) { stream_ok = 0; return; }
        CV x; x.i = v; cc_store(t, a, x); return;
    }
    char tn[80];
    cc_error("cin can't read a %s", ty_str(t, tn, sizeof tn));
}

/* ---- string and container methods --------------------------------------------------------------------------- */
static uint64_t npos = (uint64_t)-1;
void cc_throw_std(const char *cls, const char *msg) __attribute__((noreturn));
static void out_of_range(const char *fmt, ...) __attribute__((noreturn));
static void out_of_range(const char *fmt, ...) {
    char b[200]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    cc_throw_std("out_of_range", b);
}
static CV argv_(CNode *n, int i) { return cc_eval(n->kids[i]); }
static int64_t argi(CNode *n, int i) { CV v = argv_(n, i); return ty_is_flt(n->kids[i]->ty) ? (int64_t)v.d : v.i; }

/* the text of an argument that is a string, a char* or a char */
static const char *text_arg(CNode *n, int i, uint64_t *len, char *one) {
    CType *t = strip(n->kids[i]->ty);
    CV v = cc_eval(n->kids[i]);
    if (t->kind == TY_STRING) return cc_str_ptr(v.u, len);
    if (t->kind == TY_CHAR) { one[0] = (char)v.i; *len = 1; return one; }
    const char *s = cstr(v.u); *len = strlen(s);
    return s;
}

static CV string_method(CNode *n, uint64_t s) {
    const char *m = n->name;
    CV r; r.u = 0;
    uint64_t len; const char *p = cc_str_ptr(s, &len);
    char one[2];
    #define IS(x) (!strcmp(m, x))
    if (IS("size") || IS("length")) { r.u = len; return r; }
    if (IS("empty")) { r.u = len == 0; return r; }
    if (IS("capacity")) { r.u = ld64(s + 16); return r; }
    if (IS("clear")) { cc_str_set(s, "", 0); return r; }
    if (IS("c_str") || IS("data")) { if (!ld64(s)) cc_str_set(s, "", 0); r.u = ld64(s); return r; }
    if (IS("begin")) { if (!ld64(s)) cc_str_set(s, "", 0); r.u = ld64(s); return r; }
    if (IS("end")) { if (!ld64(s)) cc_str_set(s, "", 0); r.u = ld64(s) + len; return r; }
    if (IS("at") || IS("front") || IS("back")) {
        int64_t i = IS("at") ? argi(n, 0) : IS("front") ? 0 : (int64_t)len - 1;
        if (i < 0 || (uint64_t)i >= len) out_of_range("string::%s: index %lld is out of range (size %llu)", m, (long long)i, (unsigned long long)len);
        r.u = ld64(s) + (uint64_t)i; return r;
    }
    if (IS("push_back")) { char c = (char)argi(n, 0); char *t = rt_alloc_leaf((size_t)len + 2); memcpy(t, p, (size_t)len); t[len] = c; cc_str_set(s, t, len + 1); return r; }
    if (IS("pop_back")) { if (!len) cc_error("string::pop_back on an empty string"); cc_str_set(s, p, len - 1); return r; }
    if (IS("substr")) {
        uint64_t pos = n->nk > 0 ? (uint64_t)argi(n, 0) : 0, cnt = n->nk > 1 ? (uint64_t)argi(n, 1) : npos;
        if (pos > len) cc_error("string::substr: position %llu is past the end (size %llu)", (unsigned long long)pos, (unsigned long long)len);
        if (cnt > len - pos) cnt = len - pos;
        uint64_t t = cc_temp(ty_string);
        cc_str_set(t, p + pos, cnt);
        r.u = t; return r;
    }
    if (IS("find") || IS("rfind")) {
        uint64_t nl; const char *nd = text_arg(n, 0, &nl, one);
        p = cc_str_ptr(s, &len);
        int64_t from = n->nk > 1 ? argi(n, 1) : IS("find") ? 0 : (int64_t)len;
        r.u = npos;
        if (IS("find")) { for (int64_t i = from; i >= 0 && (uint64_t)i + nl <= len; i++) if (!memcmp(p + i, nd, (size_t)nl)) { r.u = (uint64_t)i; break; } }
        else { int64_t st = (int64_t)len - (int64_t)nl; if (st > from) st = from; for (int64_t i = st; i >= 0; i--) if (!memcmp(p + i, nd, (size_t)nl)) { r.u = (uint64_t)i; break; } }
        return r;
    }
    if (IS("find_first_of") || IS("find_last_of") || IS("find_first_not_of") || IS("find_last_not_of")) {
        uint64_t nl; const char *set = text_arg(n, 0, &nl, one);
        p = cc_str_ptr(s, &len);
        int want = !strstr(m, "not"), last = strstr(m, "last") != 0;
        r.u = npos;
        for (uint64_t k = 0; k < len; k++) {
            uint64_t i = last ? len - 1 - k : k;
            if ((memchr(set, p[i], (size_t)nl) != 0) == want) { r.u = i; break; }
        }
        return r;
    }
    if (IS("append") || IS("insert") || IS("replace") || IS("erase")) {
        uint64_t pos = 0, cnt = 0, nl = 0;
        const char *add = "";
        if (IS("append")) { pos = len; add = text_arg(n, 0, &nl, one); }
        else if (IS("insert")) { pos = (uint64_t)argi(n, 0); add = text_arg(n, 1, &nl, one); }
        else if (IS("replace")) { pos = (uint64_t)argi(n, 0); cnt = (uint64_t)argi(n, 1); add = text_arg(n, 2, &nl, one); }
        else {
            CType *t0 = n->nk ? strip(n->kids[0]->ty) : 0;
            if (t0 && t0->kind == TY_PTR) {
                /* erase(iterator) or erase(first, last) */
                uint64_t a0 = argv_(n, 0).u, a1 = n->nk > 1 ? argv_(n, 1).u : a0 + 1;
                pos = a0 - ld64(s); cnt = a1 - a0;
            } else { pos = n->nk ? (uint64_t)argi(n, 0) : 0; cnt = n->nk > 1 ? (uint64_t)argi(n, 1) : npos; }
        }
        p = cc_str_ptr(s, &len);
        if (pos > len) cc_error("string::%s: position %llu is past the end (size %llu)", m, (unsigned long long)pos, (unsigned long long)len);
        if (cnt > len - pos) cnt = len - pos;
        char *t = rt_alloc_leaf((size_t)(len - cnt + nl) + 1);
        memcpy(t, p, (size_t)pos); memcpy(t + pos, add, (size_t)nl); memcpy(t + pos + nl, p + pos + cnt, (size_t)(len - pos - cnt));
        cc_str_set(s, t, len - cnt + nl);
        r.u = s; return r;
    }
    if (IS("compare")) {
        uint64_t nl; const char *o = text_arg(n, 0, &nl, one);
        p = cc_str_ptr(s, &len);
        uint64_t k = len < nl ? len : nl;
        int c = memcmp(p, o, (size_t)k);
        r.i = c ? (c < 0 ? -1 : 1) : len < nl ? -1 : len > nl;
        return r;
    }
    if (IS("resize")) { uint64_t nn = (uint64_t)argi(n, 0); char c = n->nk > 1 ? (char)argi(n, 1) : 0; char *t = rt_alloc_leaf((size_t)nn + 1); memcpy(t, p, (size_t)(len < nn ? len : nn)); for (uint64_t i = len; i < nn; i++) t[i] = c; cc_str_set(s, t, nn); return r; }
    if (IS("reserve") || IS("shrink_to_fit")) return r;
    if (IS("swap")) { uint64_t o = argv_(n, 0).u; uint8_t tmp[24]; memcpy(tmp, cc_mem + s, 24); memcpy(cc_mem + s, cc_mem + o, 24); memcpy(cc_mem + o, tmp, 24); return r; }
    if (IS("assign")) { uint64_t nl; const char *t = text_arg(n, 0, &nl, one); char *c = rt_alloc_leaf((size_t)nl + 1); memcpy(c, t, (size_t)nl); cc_str_set(s, c, nl); r.u = s; return r; }
    cc_error("string has no method %s()", m);
    #undef IS
}

static CV vector_method(CNode *n, uint64_t v, CType *t) {
    const char *m = n->name;
    CType *et = t->base;
    uint64_t es = (uint64_t)et->size, sz = vsize(v);
    CV r; r.u = 0;
    #define IS(x) (!strcmp(m, x))
    if (IS("size")) { r.u = sz; return r; }
    if (IS("empty")) { r.u = sz == 0; return r; }
    if (IS("capacity")) { r.u = ld64(v + 16); return r; }
    if (IS("push_back") || IS("emplace_back") || IS("push_front") || IS("emplace_front")) {
        CV x;
        uint64_t tmp = 0;
        if (IS("emplace_back") && et->kind == TY_STRUCT && (n->nk != 1 || strip(n->kids[0]->ty) != et)) {
            /* emplace_back(args): the element made from them */
            tmp = cc_temp(et);
            CNode init; memset(&init, 0, sizeof init);
            init.k = C_INIT; init.ty = et; init.kids = n->kids; init.nk = n->nk;
            /* the constructor that takes them */
            for (int i = 0; i < et->nmeth; i++) if (et->meth[i]->is_ctor && et->meth[i]->ty->nparams == n->nk) init.fn = et->meth[i];
            if (!init.fn && et->has_ctor) cc_error("no constructor of %s takes %d arguments", et->name, n->nk);
            if (init.fn) { CV args[16]; for (int i = 0; i < n->nk && i < 16; i++) { args[i] = cc_eval(n->kids[i]); CType *pt = init.fn->ty->params[i]; args[i] = pt->kind == TY_REF ? (CV){ .u = addr_of_value(strip(pt), conv_to(strip(pt), n->kids[i]->ty, args[i])) } : conv_to(pt, n->kids[i]->ty, args[i]); } cc_call(init.fn, args, n->nk, tmp, 0); }
            else { /* an aggregate: members in order */
                for (int i = 0; i < n->nk && i < et->nmem; i++) { CV a = conv_to(et->mem[i].ty, n->kids[i]->ty, cc_eval(n->kids[i])); put_elem(et->mem[i].ty, tmp + (uint64_t)et->mem[i].offset, 0, a); }
            }
            x.u = tmp;
        } else if (IS("emplace_back") && et->kind == TY_PAIR && n->nk == 2) {
            tmp = cc_temp(et);
            put_elem(et->mem[0].ty, tmp, 0, conv_to(et->mem[0].ty, n->kids[0]->ty, cc_eval(n->kids[0])));
            put_elem(et->mem[1].ty, tmp + (uint64_t)et->mem[1].offset, 0, conv_to(et->mem[1].ty, n->kids[1]->ty, cc_eval(n->kids[1])));
            x.u = tmp;
        } else {
            if (n->nk != 1) cc_error("%s takes one value", m);
            if (n->kids[0]->k == C_INIT) { tmp = cc_temp(et); cc_init_at(tmp, et, n->kids[0]); x.u = tmp; }
            else x = conv_to(et, n->kids[0]->ty, cc_eval(n->kids[0]));
        }
        /* the value may be inside this vector: copied before it can move */
        uint64_t src = is_objk(et) ? x.u : 0;
        if (src && src >= vdata(v) && src < vdata(v) + sz * es) { uint64_t c = cc_temp(et); cc_copy_value(et, c, src); x.u = c; }
        int front = IS("push_front") || IS("emplace_front");
        uint64_t e = open_at(v, es, front ? 0 : sz);
        put_elem(et, e, 0, x);
        return r;
    }
    if (IS("pop_back") || IS("pop_front")) {
        if (!sz) cc_error("vector::%s on an empty vector", m);
        uint64_t i = IS("pop_back") ? sz - 1 : 0;
        if (et->owns || (et->kind == TY_STRUCT && et->dtor)) cc_destroy(et, vdata(v) + i * es);
        memmove(cc_mem + vdata(v) + i * es, cc_mem + vdata(v) + (i + 1) * es, (size_t)((sz - i - 1) * es));
        st64(v + 8, sz - 1);
        return r;
    }
    if (IS("clear")) { if (et->owns || (et->kind == TY_STRUCT && et->dtor)) for (uint64_t i = sz; i > 0; i--) cc_destroy(et, vdata(v) + (i - 1) * es); st64(v + 8, 0); return r; }
    if (IS("at") || IS("front") || IS("back")) {
        int64_t i = IS("at") ? argi(n, 0) : IS("front") ? 0 : (int64_t)sz - 1;
        if (i < 0 || (uint64_t)i >= sz) out_of_range("vector::%s: index %lld is out of range (size %llu)", m, (long long)i, (unsigned long long)sz);
        r.u = vdata(v) + (uint64_t)i * es; return r;
    }
    if (IS("begin") || IS("data")) { r.u = vdata(v); if (!r.u) { reserve(v, es, 1); r.u = vdata(v); } return r; }
    if (IS("end")) { if (!vdata(v)) reserve(v, es, 1); r.u = vdata(v) + sz * es; return r; }
    if (IS("rbegin") || IS("rend")) cc_error("reverse iterators aren't supported: use reverse() or an index");
    if (IS("resize")) {
        int64_t nn = argi(n, 0);
        if (nn < 0) cc_error("vector::resize with a negative size");
        CV fillv = n->nk > 1 ? conv_to(et, n->kids[1]->ty, cc_eval(n->kids[1])) : (CV){ .u = 0 };
        if ((uint64_t)nn < sz) { if (et->owns) for (uint64_t i = sz; i > (uint64_t)nn; i--) cc_destroy(et, vdata(v) + (i - 1) * es); st64(v + 8, (uint64_t)nn); return r; }
        reserve(v, es, (uint64_t)nn);
        for (uint64_t i = sz; i < (uint64_t)nn; i++) {
            uint64_t e = vdata(v) + i * es;
            memset(cc_mem + e, 0, (size_t)es);
            if (n->nk > 1) put_elem(et, e, 0, fillv); else if (et->kind == TY_STRUCT) cc_construct_default(et, e);
        }
        st64(v + 8, (uint64_t)nn);
        return r;
    }
    if (IS("reserve")) { reserve(v, es, (uint64_t)argi(n, 0)); return r; }
    if (IS("shrink_to_fit")) return r;
    if (IS("assign")) {
        int64_t nn = argi(n, 0);
        CV fillv = conv_to(et, n->kids[1]->ty, cc_eval(n->kids[1]));
        if (et->owns) for (uint64_t i = sz; i > 0; i--) cc_destroy(et, vdata(v) + (i - 1) * es);
        st64(v + 8, 0);
        reserve(v, es, (uint64_t)nn);
        for (int64_t i = 0; i < nn; i++) { uint64_t e = vdata(v) + (uint64_t)i * es; memset(cc_mem + e, 0, (size_t)es); put_elem(et, e, 0, fillv); }
        st64(v + 8, (uint64_t)nn);
        return r;
    }
    if (IS("insert")) {
        uint64_t it = argv_(n, 0).u, i = (it - vdata(v)) / es;
        if (i > sz) cc_error("vector::insert: the position is outside the vector");
        CV x = conv_to(et, n->kids[n->nk - 1]->ty, cc_eval(n->kids[n->nk - 1]));
        int64_t cnt = n->nk == 3 ? argi(n, 1) : 1;
        for (int64_t k = 0; k < cnt; k++) { uint64_t e = open_at(v, es, i + (uint64_t)k); put_elem(et, e, 0, x); }
        r.u = vdata(v) + i * es; return r;
    }
    if (IS("erase")) {
        uint64_t a0 = argv_(n, 0).u, a1 = n->nk > 1 ? argv_(n, 1).u : a0 + es;
        uint64_t i = (a0 - vdata(v)) / es, j = (a1 - vdata(v)) / es;
        if (i > sz || j > sz || j < i) cc_error("vector::erase: the position is outside the vector");
        if (et->owns) for (uint64_t k = i; k < j; k++) cc_destroy(et, vdata(v) + k * es);
        memmove(cc_mem + vdata(v) + i * es, cc_mem + vdata(v) + j * es, (size_t)((sz - j) * es));
        st64(v + 8, sz - (j - i));
        r.u = vdata(v) + i * es; return r;
    }
    if (IS("swap")) { uint64_t o = argv_(n, 0).u; uint8_t tmp[24]; memcpy(tmp, cc_mem + v, 24); memcpy(cc_mem + v, cc_mem + o, 24); memcpy(cc_mem + o, tmp, 24); return r; }
    cc_error("vector has no method %s()", m);
    #undef IS
}

static CV assoc_method(CNode *n, uint64_t a, CType *t) {
    const char *m = n->name;
    CType *et = elem_of(t), *kt = t->base;
    uint64_t es = (uint64_t)et->size, sz = vsize(a);
    CV r; r.u = 0;
    #define IS(x) (!strcmp(m, x))
    if (IS("size")) { r.u = sz; return r; }
    if (IS("empty")) { r.u = sz == 0; return r; }
    if (IS("clear")) { if (et->owns) for (uint64_t i = sz; i > 0; i--) cc_destroy(et, vdata(a) + (i - 1) * es); st64(a + 8, 0); return r; }
    if (IS("begin")) { if (!vdata(a)) reserve(a, es, 1); r.u = vdata(a); return r; }
    if (IS("end")) { if (!vdata(a)) reserve(a, es, 1); r.u = vdata(a) + sz * es; return r; }
    if (IS("insert") || IS("emplace")) {
        uint64_t src;
        if (n->nk == 2 && t->kind == TY_MAP) {
            src = cc_temp(et);
            put_elem(t->base, src, 0, conv_to(t->base, n->kids[0]->ty, cc_eval(n->kids[0])));
            put_elem(t->base2, src + (uint64_t)et->mem[1].offset, 0, conv_to(t->base2, n->kids[1]->ty, cc_eval(n->kids[1])));
        } else if (n->kids[0]->k == C_INIT) { src = cc_temp(et); cc_init_at(src, et, n->kids[0]); }
        else {
            CType *xt = strip(n->kids[0]->ty);
            CV x = cc_eval(n->kids[0]);
            if (t->kind == TY_MAP && xt != et) {
                /* a pair of other types: converted member by member */
                src = cc_temp(et);
                CV f0 = cc_load(xt->mem[0].ty, x.u), f1 = cc_load(xt->mem[1].ty, x.u + (uint64_t)xt->mem[1].offset);
                put_elem(t->base, src, 0, conv_to(t->base, xt->mem[0].ty, f0));
                put_elem(t->base2, src + (uint64_t)et->mem[1].offset, 0, conv_to(t->base2, xt->mem[1].ty, f1));
            } else src = addr_of_value(et, conv_to(et, xt, x));
        }
        r.u = (uint64_t)set_insert(t, a, src);
        return r;
    }
    /* the rest take a key */
    uint64_t key = 0;
    if (n->nk) {
        CType *xt = strip(n->kids[0]->ty);
        if (xt->kind == TY_PTR && xt->base->kind == et->kind && xt->base->size == et->size && IS("erase")) {
            /* erase(iterator) */
            uint64_t it = cc_eval(n->kids[0]).u, i = (it - vdata(a)) / es;
            if (i >= sz) cc_error("erase: the iterator is past the end");
            if (et->owns) cc_destroy(et, it);
            memmove(cc_mem + it, cc_mem + it + es, (size_t)((sz - i - 1) * es));
            st64(a + 8, sz - 1);
            r.u = 1;
            return r;
        }
        key = addr_of_value(kt, conv_to(kt, xt, cc_eval(n->kids[0])));
    }
    int found;
    uint64_t i = sorted_find(t, a, key, &found);
    if (IS("count") || IS("contains")) { r.u = (uint64_t)found; return r; }
    if (IS("find")) { r.u = vdata(a) + (found ? i : sz) * es; if (!vdata(a)) { reserve(a, es, 1); r.u = vdata(a); } return r; }
    if (IS("lower_bound")) { r.u = vdata(a) + i * es; return r; }
    if (IS("upper_bound")) { r.u = vdata(a) + (found ? i + 1 : i) * es; return r; }
    if (IS("erase")) {
        if (!found) return r;
        uint64_t e = vdata(a) + i * es;
        if (et->owns) cc_destroy(et, e);
        memmove(cc_mem + e, cc_mem + e + es, (size_t)((sz - i - 1) * es));
        st64(a + 8, sz - 1);
        r.u = 1;
        return r;
    }
    if (IS("at")) { if (!found) out_of_range("map::at: the key isn't in the map"); r.u = vdata(a) + i * es + (uint64_t)et->mem[1].offset; return r; }
    cc_error("%s has no method %s()", t->kind == TY_MAP ? "map" : "set", m);
    #undef IS
}

/* ---- the C library -------------------------------------------------------------------------------------------- */
rt_jmp_t *cc_exit_jmp;
static int exit_code;
int cc_lib_exit_code(int *code) { *code = exit_code; return 0; }

static uint32_t crand_state = 1;
static int crand(void) { crand_state = crand_state * 1103515245u + 12345u; return (int)((crand_state >> 1) & 0x7FFFFFFF); }

static uint64_t strtok_at;

static void check_range(uint64_t a, uint64_t n) { if (n) cc_at(a, n, 1); }

static CV lib_call(CNode *n) {
    CV r; r.u = 0;
    int id = n->op;
    CV av[64]; CType *ty[64];
    int na = n->nk < 64 ? n->nk : 64;
    /* most take their arguments as values; the ones that change a variable take it as an address */
    int by_addr = id == L_swap || id == L_getline;
    for (int i = 0; i < na; i++) {
        ty[i] = strip(n->kids[i]->ty);
        if (n->kids[i]->k == C_INIT) { CType *et = n->kids[i]->ty; if (!et) { av[i].u = 0; continue; } av[i] = cc_eval(n->kids[i]); continue; }
        if (by_addr) av[i].u = cc_addr(n->kids[i]);
        else av[i] = cc_eval(n->kids[i]);
    }
    #define ARGD(q_) (ty_is_flt(ty[q_]) ? av[q_].d : (double)av[q_].i)
    #define ARGI(q_) (ty_is_flt(ty[q_]) ? (int64_t)av[q_].d : av[q_].i)
    #define NEED(k) do { if (na < (k)) cc_error("%s() needs %d argument%s", lib_names[id], (k), (k) == 1 ? "" : "s"); } while (0)
    char buf[8192];
    switch (id) {
    case L_printf: NEED(1); { int k = cc_format(buf, sizeof buf, cstr(av[0].u), av + 1, na - 1, ty + 1); cc_out(buf, (uint64_t)(k < (int)sizeof buf ? k : (int)sizeof buf - 1)); r.i = k; } break;
    case L_fprintf: NEED(2); { int k = cc_format(buf, sizeof buf, cstr(av[1].u), av + 2, na - 2, ty + 2); cc_out(buf, (uint64_t)(k < (int)sizeof buf ? k : (int)sizeof buf - 1)); r.i = k; } break;
    case L_sprintf: NEED(2); { int k = cc_format(buf, sizeof buf, cstr(av[1].u), av + 2, na - 2, ty + 2); check_range(av[0].u, (uint64_t)k + 1); memcpy(cc_mem + av[0].u, buf, (size_t)k + 1); r.i = k; } break;
    case L_snprintf: NEED(3); { int k = cc_format(buf, sizeof buf, cstr(av[2].u), av + 3, na - 3, ty + 3); int64_t cap = ARGI(1); if (cap > 0) { int w = k < cap - 1 ? k : (int)cap - 1; check_range(av[0].u, (uint64_t)w + 1); memcpy(cc_mem + av[0].u, buf, (size_t)w); cc_mem[av[0].u + w] = 0; } r.i = k; } break;
    case L_puts: NEED(1); { const char *s = cstr(av[0].u); cc_out(s, strlen(s)); cc_out("\n", 1); } break;
    case L_fputs: NEED(1); { const char *s = cstr(av[0].u); cc_out(s, strlen(s)); } break;
    case L_putchar: case L_fputc: case L_putc: NEED(1); { char c = (char)av[0].i; cc_out(&c, 1); r.i = (uint8_t)c; } break;
    case L_getchar: case L_getc: case L_fgetc: r.i = cc_stdin_getc(0); break;
    case L_scanf: NEED(1); r.i = cc_scanf(cstr(av[0].u), av + 1, na - 1, cc_stdin_getc, 0); break;
    case L_sscanf: NEED(2); { SS ss = { cstr(av[0].u), 0 }; r.i = cc_scanf(cstr(av[1].u), av + 2, na - 2, str_getc, &ss); } break;
    case L_fgets: case L_gets: {
        NEED(1);
        int64_t cap = id == L_fgets ? ARGI(1) : 4096;
        int k = 0, c = 0;
        while (k < cap - 1 && (c = cc_stdin_getc(0)) >= 0) { *cc_at(av[0].u + (uint64_t)k, 1, 1) = (uint8_t)c; k++; if (c == '\n') break; }
        if (id == L_gets && k && cc_mem[av[0].u + k - 1] == '\n') k--;
        *cc_at(av[0].u + (uint64_t)k, 1, 1) = 0;
        r.u = k || c >= 0 ? av[0].u : 0;
        break;
    }
    case L_fflush: cc_flush(); break;
    case L_strlen: NEED(1); r.u = strlen(cstr(av[0].u)); break;
    case L_strcmp: NEED(2); { int c = strcmp(cstr(av[0].u), cstr(av[1].u)); r.i = c < 0 ? -1 : c > 0; } break;
    case L_strcasecmp: NEED(2); { int c = strcasecmp(cstr(av[0].u), cstr(av[1].u)); r.i = c < 0 ? -1 : c > 0; } break;
    case L_strncmp: NEED(3); { int c = strncmp((const char *)cc_mem + av[0].u, (const char *)cc_mem + av[1].u, (size_t)ARGI(2)); cstr(av[0].u); r.i = c < 0 ? -1 : c > 0; } break;
    case L_strcpy: NEED(2); { const char *s = cstr(av[1].u); uint64_t k = strlen(s) + 1; check_range(av[0].u, k); memmove(cc_mem + av[0].u, s, (size_t)k); r.u = av[0].u; } break;
    case L_strncpy: NEED(3); { uint64_t k = (uint64_t)ARGI(2); check_range(av[0].u, k); const char *s = cstr(av[1].u); uint64_t sl = strlen(s); for (uint64_t i = 0; i < k; i++) cc_mem[av[0].u + i] = i < sl ? (uint8_t)s[i] : 0; r.u = av[0].u; } break;
    case L_strcat: NEED(2); { uint64_t dl = strlen(cstr(av[0].u)); const char *s = cstr(av[1].u); uint64_t k = strlen(s) + 1; check_range(av[0].u + dl, k); memmove(cc_mem + av[0].u + dl, s, (size_t)k); r.u = av[0].u; } break;
    case L_strncat: NEED(3); { uint64_t dl = strlen(cstr(av[0].u)); const char *s = cstr(av[1].u); uint64_t k = strlen(s); if (k > (uint64_t)ARGI(2)) k = (uint64_t)ARGI(2); check_range(av[0].u + dl, k + 1); memmove(cc_mem + av[0].u + dl, s, (size_t)k); cc_mem[av[0].u + dl + k] = 0; r.u = av[0].u; } break;
    case L_strchr: case L_strrchr: NEED(2); { const char *s = cstr(av[0].u); const char *p = id == L_strchr ? strchr(s, (int)av[1].i) : strrchr(s, (int)av[1].i); if (!(char)av[1].i) p = s + strlen(s); r.u = p ? av[0].u + (uint64_t)(p - s) : 0; } break;
    case L_strstr: NEED(2); { const char *s = cstr(av[0].u); const char *p = strstr(s, cstr(av[1].u)); r.u = p ? av[0].u + (uint64_t)(p - s) : 0; } break;
    case L_strdup: NEED(1); { const char *s = cstr(av[0].u); uint64_t k = strlen(s) + 1; r.u = cc_malloc(k); memcpy(cc_mem + r.u, cc_mem + av[0].u, (size_t)k); } break;
    case L_strtok: {
        NEED(2);
        uint64_t s = av[0].u ? av[0].u : strtok_at;
        if (!s) break;
        const char *delim = cstr(av[1].u);
        while (cc_mem[s] && strchr(delim, cc_mem[s])) s++;
        if (!cc_mem[s]) { strtok_at = 0; break; }
        uint64_t e = s;
        while (cc_mem[e] && !strchr(delim, cc_mem[e])) e++;
        if (cc_mem[e]) { cc_mem[e] = 0; strtok_at = e + 1; } else strtok_at = 0;
        r.u = s;
        break;
    }
    case L_memcpy: case L_memmove: NEED(3); { uint64_t k = (uint64_t)ARGI(2); check_range(av[0].u, k); cc_at(av[1].u, k ? k : 1, 0); memmove(cc_mem + av[0].u, cc_mem + av[1].u, (size_t)k); r.u = av[0].u; } break;
    case L_memset: NEED(3); { uint64_t k = (uint64_t)ARGI(2); check_range(av[0].u, k); memset(cc_mem + av[0].u, (int)av[1].i, (size_t)k); r.u = av[0].u; } break;
    case L_memcmp: NEED(3); { uint64_t k = (uint64_t)ARGI(2); cc_at(av[0].u, k ? k : 1, 0); cc_at(av[1].u, k ? k : 1, 0); int c = memcmp(cc_mem + av[0].u, cc_mem + av[1].u, (size_t)k); r.i = c < 0 ? -1 : c > 0; } break;
    case L_malloc: NEED(1); r.u = cc_malloc((uint64_t)ARGI(0)); break;
    case L_calloc: NEED(2); r.u = cc_malloc((uint64_t)(ARGI(0) * ARGI(1))); break;
    case L_realloc: NEED(2); r.u = cc_realloc(av[0].u, (uint64_t)ARGI(1)); break;
    case L_free: NEED(1); cc_free(av[0].u); break;
    case L_exit: case L_exit_: exit_code = na ? (int)av[0].i : 0; cc_flush(); rt_longjmp(cc_exit_jmp, 1);
    case L_abort: cc_flush(); cc_error("abort() was called");
    case L_assert: NEED(1); if (!av[0].u) cc_error("Assertion failed"); break;
    case L_atoi: NEED(1); r.i = (int32_t)strtol(cstr(av[0].u), 0, 10); break;
    case L_atol: case L_atoll: NEED(1); r.i = strtoll(cstr(av[0].u), 0, 10); break;
    case L_atof: NEED(1); r.d = num_parse(cstr(av[0].u), 0); break;
    case L_strtol: case L_strtoll: case L_strtoul: case L_strtoull: {
        NEED(1);
        const char *s = cstr(av[0].u); char *e;
        int base = na > 2 ? (int)av[2].i : 10;
        r.i = id == L_strtoul || id == L_strtoull ? (int64_t)strtoull(s, &e, base) : strtoll(s, &e, base);
        if (na > 1 && av[1].u) st64(av[1].u, av[0].u + (uint64_t)(e - s));
        break;
    }
    case L_strtod: case L_strtof: { NEED(1); const char *s = cstr(av[0].u), *e; r.d = num_parse(s, &e); if (id == L_strtof) r.d = (double)(float)r.d; if (na > 1 && av[1].u) st64(av[1].u, av[0].u + (uint64_t)(e - s)); break; }
    case L_labs: case L_llabs: NEED(1); r.i = av[0].i < 0 ? -av[0].i : av[0].i; break;
    case L_abs: NEED(1); if (ty_is_flt(ty[0])) r.d = fabs(av[0].d); else { r.i = av[0].i < 0 ? -av[0].i : av[0].i; if (n->ty->kind == TY_INT) r.i = (int32_t)r.i; } break;
    case L_rand: r.i = crand(); break;
    case L_rand_r: r.i = crand(); break;
    case L_srand: NEED(1); crand_state = (uint32_t)av[0].u; break;
    case L_time: { extern int clock_known(void); extern uint64_t clock_unix(void); r.i = clock_known() ? (int64_t)clock_unix() : (int64_t)(rt_ms() / 1000); if (na && av[0].u) st64(av[0].u, r.u); break; }
    case L_clock: r.i = (int64_t)rt_ms() * 1000; break;
    case L_sleep: NEED(1); cc_flush(); rt_sleep_ms((uint64_t)ARGI(0) * 1000); break;
    case L_usleep: NEED(1); cc_flush(); rt_sleep_ms((uint64_t)ARGI(0) / 1000); break;
    case L_qsort: {
        NEED(4);
        uint64_t base = av[0].u, cnt = (uint64_t)ARGI(1), es = (uint64_t)ARGI(2);
        if (!es || !cnt) break;
        CType *et = rt_alloc(sizeof(CType)); et->kind = TY_ARRAY; et->size = (int)es; et->align = 1; et->len = 1; et->base = ty_uchar;
        check_range(base, cnt * es);
        /* the comparator takes two const void* and returns <0 0 >0 */
        uint64_t tmp = cc_malloc(cnt * es + es);
        CV f = av[3];
        /* merge sort with it */
        struct { uint64_t base, tmp, es; CV f; } q = { base, tmp, es, f };
        uint64_t width;
        for (width = 1; width < cnt; width *= 2) {
            for (uint64_t lo = 0; lo < cnt; lo += 2 * width) {
                uint64_t mid = lo + width < cnt ? lo + width : cnt, hi = lo + 2 * width < cnt ? lo + 2 * width : cnt;
                uint64_t i = lo, j = mid, o = lo;
                while (i < mid && j < hi) {
                    CV args[2] = { { .u = q.base + j * es }, { .u = q.base + i * es } };
                    int64_t c = (int32_t)cc_call_value(f, ty[3], args, 2, 0).i;
                    if (c < 0) memcpy(cc_mem + tmp + o++ * es, cc_mem + base + j++ * es, (size_t)es);
                    else memcpy(cc_mem + tmp + o++ * es, cc_mem + base + i++ * es, (size_t)es);
                }
                while (i < mid) memcpy(cc_mem + tmp + o++ * es, cc_mem + base + i++ * es, (size_t)es);
                while (j < hi) memcpy(cc_mem + tmp + o++ * es, cc_mem + base + j++ * es, (size_t)es);
            }
            memcpy(cc_mem + base, cc_mem + tmp, (size_t)(cnt * es));
            rt_check();
        }
        cc_free(tmp);
        break;
    }
    case L_bsearch: {
        NEED(5);
        uint64_t key = av[0].u, base = av[1].u, cnt = (uint64_t)ARGI(2), es = (uint64_t)ARGI(3), lo = 0, hi = cnt;
        while (lo < hi) {
            uint64_t m = (lo + hi) / 2;
            CV args[2] = { { .u = key }, { .u = base + m * es } };
            int64_t c = (int32_t)cc_call_value(av[4], ty[4], args, 2, 0).i;
            if (!c) { r.u = base + m * es; break; }
            if (c < 0) hi = m; else lo = m + 1;
        }
        break;
    }
    case L_sqrt: case L_sqrtf: NEED(1); r.d = __builtin_sqrt(ARGD(0)); break;
    case L_pow: case L_powf: NEED(2); r.d = pow(ARGD(0), ARGD(1)); break;
    case L_sin: case L_sinf: NEED(1); r.d = sin(ARGD(0)); break;
    case L_cos: case L_cosf: NEED(1); r.d = cos(ARGD(0)); break;
    case L_tan: NEED(1); r.d = tan(ARGD(0)); break;
    case L_asin: NEED(1); r.d = asin(ARGD(0)); break;
    case L_acos: NEED(1); r.d = acos(ARGD(0)); break;
    case L_atan: NEED(1); r.d = atan(ARGD(0)); break;
    case L_atan2: NEED(2); r.d = atan2(ARGD(0), ARGD(1)); break;
    case L_exp: case L_expf: NEED(1); r.d = exp(ARGD(0)); break;
    case L_log: case L_logf: NEED(1); r.d = log(ARGD(0)); break;
    case L_log10: NEED(1); r.d = log10(ARGD(0)); break;
    case L_log2: NEED(1); r.d = log2(ARGD(0)); break;
    case L_floor: case L_floorf: NEED(1); r.d = __builtin_floor(ARGD(0)); break;
    case L_ceil: case L_ceilf: NEED(1); r.d = __builtin_ceil(ARGD(0)); break;
    case L_fabs: case L_fabsf: NEED(1); r.d = __builtin_fabs(ARGD(0)); break;
    case L_fmod: NEED(2); r.d = fmod(ARGD(0), ARGD(1)); break;
    case L_round: case L_roundf: NEED(1); { double x = ARGD(0); r.d = x < 0 ? -__builtin_floor(-x + 0.5) : __builtin_floor(x + 0.5); } break;
    case L_lround: NEED(1); { double x = ARGD(0); r.i = (int64_t)(x < 0 ? -__builtin_floor(-x + 0.5) : __builtin_floor(x + 0.5)); } break;
    case L_trunc: NEED(1); r.d = __builtin_trunc(ARGD(0)); break;
    case L_hypot: NEED(2); r.d = hypot(ARGD(0), ARGD(1)); break;
    case L_cbrt: NEED(1); r.d = cbrt(ARGD(0)); break;
    case L_sinh: NEED(1); r.d = sinh(ARGD(0)); break;
    case L_cosh: NEED(1); r.d = cosh(ARGD(0)); break;
    case L_tanh: NEED(1); r.d = tanh(ARGD(0)); break;
    case L_fmin: NEED(2); r.d = ARGD(0) < ARGD(1) ? ARGD(0) : ARGD(1); break;
    case L_fmax: NEED(2); r.d = ARGD(0) > ARGD(1) ? ARGD(0) : ARGD(1); break;
    case L_copysign: NEED(2); r.d = __builtin_copysign(ARGD(0), ARGD(1)); break;
    case L_isnan: NEED(1); r.i = ARGD(0) != ARGD(0); break;
    case L_isinf: NEED(1); r.i = ARGD(0) == __builtin_inf() || ARGD(0) == -__builtin_inf(); break;
    case L_isfinite: NEED(1); { double x = ARGD(0); r.i = x == x && x != __builtin_inf() && x != -__builtin_inf(); } break;
    case L_toupper: NEED(1); r.i = av[0].i >= 'a' && av[0].i <= 'z' ? av[0].i - 32 : av[0].i; if (n->ty->kind == TY_CHAR) r.i = (int8_t)r.i; break;
    case L_tolower: NEED(1); r.i = av[0].i >= 'A' && av[0].i <= 'Z' ? av[0].i + 32 : av[0].i; break;
    case L_isdigit: NEED(1); r.i = av[0].i >= '0' && av[0].i <= '9'; break;
    case L_isalpha: NEED(1); r.i = (av[0].i | 32) >= 'a' && (av[0].i | 32) <= 'z'; break;
    case L_isalnum: NEED(1); r.i = ((av[0].i | 32) >= 'a' && (av[0].i | 32) <= 'z') || (av[0].i >= '0' && av[0].i <= '9'); break;
    case L_isspace: NEED(1); r.i = av[0].i == ' ' || (av[0].i >= 9 && av[0].i <= 13); break;
    case L_isupper: NEED(1); r.i = av[0].i >= 'A' && av[0].i <= 'Z'; break;
    case L_islower: NEED(1); r.i = av[0].i >= 'a' && av[0].i <= 'z'; break;
    case L_ispunct: NEED(1); r.i = av[0].i > 32 && av[0].i < 127 && !(((av[0].i | 32) >= 'a' && (av[0].i | 32) <= 'z') || (av[0].i >= '0' && av[0].i <= '9')); break;
    case L_isxdigit: NEED(1); r.i = (av[0].i >= '0' && av[0].i <= '9') || ((av[0].i | 32) >= 'a' && (av[0].i | 32) <= 'f'); break;
    case L_isprint: NEED(1); r.i = av[0].i >= 32 && av[0].i < 127; break;
    case L_isgraph: NEED(1); r.i = av[0].i > 32 && av[0].i < 127; break;
    case L_iscntrl: NEED(1); r.i = av[0].i < 32 || av[0].i == 127; break;

    /* ---- C++ ---- */
    case L_to_string: {
        NEED(1);
        uint64_t s = cc_temp(ty_string);
        int k;
        if (ty_is_flt(ty[0])) k = num_fixed(av[0].d, 6, buf, 600);
        else if (ty[0]->kind == TY_ULONG || ty[0]->kind == TY_UINT) k = snprintf(buf, 64, "%llu", (unsigned long long)av[0].u);
        else k = snprintf(buf, 64, "%lld", (long long)av[0].i);
        cc_str_set(s, buf, (uint64_t)k);
        r.u = s;
        break;
    }
    case L_stoi: case L_stol: case L_stoll: case L_stoul: {
        NEED(1);
        uint64_t len; const char *s = ty[0]->kind == TY_STRING ? cc_str_ptr(av[0].u, &len) : cstr(av[0].u);
        char *e; int64_t v = strtoll(s, &e, na > 2 ? (int)av[2].i : 10);
        if (e == s) { char b[160]; snprintf(b, sizeof b, "%s: \"%.80s\" isn't a number", lib_names[id], s); cc_throw_std("invalid_argument", b); }
        r.i = id == L_stoi ? (int32_t)v : v;
        break;
    }
    case L_stod: case L_stof: {
        NEED(1);
        uint64_t len; const char *s = ty[0]->kind == TY_STRING ? cc_str_ptr(av[0].u, &len) : cstr(av[0].u), *e;
        r.d = num_parse(s, &e);
        if (e == s) { char b[160]; snprintf(b, sizeof b, "%s: \"%.80s\" isn't a number", lib_names[id], s); cc_throw_std("invalid_argument", b); }
        if (id == L_stof) r.d = (double)(float)r.d;
        break;
    }
    case L_getline: {
        NEED(2);
        char delim = na > 2 ? (char)cc_eval(n->kids[2]).i : '\n';
        int c, k = 0;
        char *line = rt_alloc_leaf(65536);
        while ((c = cc_stdin_getc(0)) >= 0 && c != delim && k < 65535) line[k++] = (char)c;
        stream_ok = c >= 0 || k > 0;
        cc_str_set(av[1].u, line, (uint64_t)k);
        r.u = (uint64_t)stream_ok;
        break;
    }
    case L_swap: {
        NEED(2);
        CType *t = ty[0];
        uint64_t sz = (uint64_t)(t->size > 0 ? t->size : 8);
        uint8_t *tmp = rt_alloc_leaf((size_t)sz);
        memcpy(tmp, cc_at(av[0].u, sz, 1), (size_t)sz);
        memcpy(cc_mem + av[0].u, cc_at(av[1].u, sz, 1), (size_t)sz);
        memcpy(cc_mem + av[1].u, tmp, (size_t)sz);
        break;
    }
    case L_min: case L_max: case L_clamp: {
        NEED(2);
        CType *t = n->ty;
        if (ty_is_num(t)) {
            CV a = conv_to(t, ty[0], av[0]), b = conv_to(t, ty[1], av[1]);
            if (id == L_clamp) {
                CV c = conv_to(t, ty[2], av[2]);
                if (ty_is_flt(t)) r.d = a.d < b.d ? b.d : a.d > c.d ? c.d : a.d; else r.i = a.i < b.i ? b.i : a.i > c.i ? c.i : a.i;
                break;
            }
            int lt = ty_is_flt(t) ? a.d < b.d : (t->kind == TY_ULONG || t->kind == TY_UINT) ? a.u < b.u : a.i < b.i;
            r = (id == L_min) == lt ? a : b;
            if (id == L_min && !lt && !(ty_is_flt(t) ? b.d < a.d : b.i < a.i)) r = a;
            if (id == L_max && lt == 0 && !(ty_is_flt(t) ? a.d < b.d || a.d > b.d : a.i != b.i)) r = a;
        } else {
            int c = cc_compare(t, av[0].u, av[1].u);
            r = id == L_min ? (c <= 0 ? av[0] : av[1]) : (c >= 0 ? av[0] : av[1]);
            uint64_t s = cc_temp(t); cc_copy_value(t, s, r.u); r.u = s;
        }
        break;
    }
    case L_sort: case L_stable_sort: case L_partial_sort: case L_nth_element: {
        NEED(2);
        CType *et = ty[0]->kind == TY_ARRAY ? ty[0]->base : ty[0]->base;
        int ci = id == L_partial_sort || id == L_nth_element ? 3 : 2;
        uint64_t hi = id == L_partial_sort || id == L_nth_element ? av[2].u : av[1].u;
        sort_range(et, av[0].u, hi, na > ci ? av[ci] : (CV){ .u = 0 }, na > ci ? ty[ci] : 0);
        break;
    }
    case L_reverse: {
        NEED(2);
        CType *et = ty[0]->base; uint64_t es = (uint64_t)et->size;
        uint64_t a = av[0].u, b = av[1].u;
        uint8_t *tmp = rt_alloc_leaf((size_t)es);
        while (a + es <= b - es && b >= es + a) { b -= es; memcpy(tmp, cc_at(a, es, 1), (size_t)es); memcpy(cc_mem + a, cc_at(b, es, 1), (size_t)es); memcpy(cc_mem + b, tmp, (size_t)es); a += es; if (a >= b) break; }
        break;
    }
    case L_fill: { NEED(3); CType *et = ty[0]->base; uint64_t es = (uint64_t)et->size; CV x = conv_to(et, ty[2], av[2]); for (uint64_t p = av[0].u; p < av[1].u; p += es) { if (et->owns) cc_destroy(et, p); memset(cc_mem + p, 0, (size_t)es); put_elem(et, p, 0, x); } break; }
    case L_iota: { NEED(3); CType *et = ty[0]->base; uint64_t es = (uint64_t)et->size; CV x = conv_to(et, ty[2], av[2]); for (uint64_t p = av[0].u; p < av[1].u; p += es) { cc_store(et, p, x); if (ty_is_flt(et)) x.d += 1; else x.i++; } break; }
    case L_find: case L_count: {
        NEED(3);
        CType *et = ty[0]->base; uint64_t es = (uint64_t)et->size;
        uint64_t key = addr_of_value(et, conv_to(et, ty[2], av[2]));
        int64_t cnt = 0; r.u = av[1].u;
        for (uint64_t p = av[0].u; p < av[1].u; p += es)
            if (cc_compare(et, p, key) == 0) { if (id == L_find) { r.u = p; break; } cnt++; }
        if (id == L_count) r.i = cnt;
        break;
    }
    case L_find_if: case L_count_if: case L_all_of: case L_any_of: case L_none_of: case L_remove_if: {
        NEED(3);
        CType *et = ty[0]->base; uint64_t es = (uint64_t)et->size;
        int64_t cnt = 0; int any = 0, all = 1;
        r.u = av[1].u;
        uint64_t w = av[0].u;
        for (uint64_t p = av[0].u; p < av[1].u; p += es) {
            uint64_t ad[1] = { p };
            int hit = (call_with(av[2], ty[2], et, ad, 1).u & 1) != 0;
            if (id == L_remove_if) { if (!hit) { if (w != p) memmove(cc_mem + w, cc_mem + p, (size_t)es); w += es; } continue; }
            if (hit) { cnt++; any = 1; if (id == L_find_if) { r.u = p; break; } } else all = 0;
        }
        if (id == L_count_if) r.i = cnt;
        else if (id == L_all_of) r.i = all;
        else if (id == L_any_of) r.i = any;
        else if (id == L_none_of) r.i = !any;
        else if (id == L_remove_if) r.u = w;
        break;
    }
    case L_remove: {
        NEED(3);
        CType *et = ty[0]->base; uint64_t es = (uint64_t)et->size, w = av[0].u;
        uint64_t key = addr_of_value(et, conv_to(et, ty[2], av[2]));
        for (uint64_t p = av[0].u; p < av[1].u; p += es) if (cc_compare(et, p, key) != 0) { if (w != p) memmove(cc_mem + w, cc_mem + p, (size_t)es); w += es; }
        r.u = w;
        break;
    }
    case L_for_each: { NEED(3); CType *et = ty[0]->base; uint64_t es = (uint64_t)et->size; for (uint64_t p = av[0].u; p < av[1].u; p += es) { uint64_t ad[1] = { p }; call_with(av[2], ty[2], et, ad, 1); } break; }
    case L_transform: {
        NEED(4);
        CType *et = ty[0]->base, *ot = ty[2]->base; uint64_t es = (uint64_t)et->size, os = (uint64_t)ot->size, o = av[2].u;
        for (uint64_t p = av[0].u; p < av[1].u; p += es, o += os) { uint64_t ad[1] = { p }; CV x = call_with(av[3], ty[3], et, ad, 1); CType *rt = strip(ty[3])->kind == TY_LAMBDA ? strip(ty[3])->ret : ty_int; put_elem(ot, o, 0, conv_to(ot, rt ? rt : ty_int, x)); }
        r.u = o;
        break;
    }
    case L_copy: { NEED(3); CType *et = ty[0]->base; uint64_t es = (uint64_t)et->size, o = av[2].u; for (uint64_t p = av[0].u; p < av[1].u; p += es, o += es) put_elem(et, o, 0, cc_load(et, p)); r.u = o; break; }
    case L_accumulate: {
        NEED(3);
        CType *et = ty[0]->base, *at = n->ty; uint64_t es = (uint64_t)et->size;
        CV acc = conv_to(at, ty[2], av[2]);
        if (at->kind == TY_STRING) { uint64_t s = cc_temp(ty_string); cc_copy_value(ty_string, s, acc.u); acc.u = s; }
        for (uint64_t p = av[0].u; p < av[1].u; p += es) {
            if (na > 3) {
                uint64_t accaddr = addr_of_value(at, acc);
                uint64_t ad[2] = { accaddr, p };
                int np; CType **ps = (CType **)callee_params(ty[3], &np);
                CV args[2];
                args[0] = ps && np > 0 && ps[0]->kind != TY_REF && !is_objk(at) ? acc : (CV){ .u = accaddr };
                args[1] = ps && np > 1 && ps[1]->kind != TY_REF && !is_objk(et) ? cc_load(et, p) : (CV){ .u = p };
                (void)ad;
                acc = conv_to(at, strip(ty[3])->kind == TY_LAMBDA ? strip(ty[3])->ret : at, cc_call_value(av[3], strip(ty[3]), args, 2, is_objk(at) ? cc_temp(at) : 0));
                continue;
            }
            CV x = cc_load(et, p);
            if (at->kind == TY_STRING) { uint64_t l1, l2; const char *a1 = cc_str_ptr(acc.u, &l1), *a2 = cc_str_ptr(x.u, &l2); char *t = rt_alloc_leaf((size_t)(l1 + l2) + 1); memcpy(t, a1, (size_t)l1); memcpy(t + l1, a2, (size_t)l2); cc_str_set(acc.u, t, l1 + l2); }
            else if (ty_is_flt(at)) acc.d += ty_is_flt(et) ? x.d : (double)x.i;
            else acc.i += ty_is_flt(et) ? (int64_t)x.d : x.i;
        }
        if (ty_is_int(at) && at->size == 4) acc.i = (int32_t)acc.i;
        r = acc;
        break;
    }
    case L_max_element: case L_min_element: {
        NEED(2);
        CType *et = ty[0]->base; uint64_t es = (uint64_t)et->size, best = av[0].u;
        for (uint64_t p = av[0].u + es; p < av[1].u; p += es) {
            int better = id == L_max_element ? less_with(na > 2 ? av[2] : (CV){ .u = 0 }, na > 2 ? ty[2] : 0, et, best, p) : less_with(na > 2 ? av[2] : (CV){ .u = 0 }, na > 2 ? ty[2] : 0, et, p, best);
            if (better) best = p;
        }
        r.u = best;
        break;
    }
    case L_make_pair: case L_minmax: {
        NEED(2);
        CType *pt = n->ty;
        uint64_t s = cc_temp(pt);
        put_elem(pt->mem[0].ty, s, 0, conv_to(pt->mem[0].ty, ty[0], av[0]));
        put_elem(pt->mem[1].ty, s + (uint64_t)pt->mem[1].offset, 0, conv_to(pt->mem[1].ty, ty[1], av[1]));
        if (id == L_minmax && cc_compare(pt->mem[0].ty, s, s + (uint64_t)pt->mem[1].offset) > 0) {
            uint64_t es = (uint64_t)pt->mem[0].ty->size; uint8_t *tmp = rt_alloc_leaf((size_t)es);
            memcpy(tmp, cc_mem + s, (size_t)es); memcpy(cc_mem + s, cc_mem + s + (uint64_t)pt->mem[1].offset, (size_t)es); memcpy(cc_mem + s + (uint64_t)pt->mem[1].offset, tmp, (size_t)es);
        }
        r.u = s;
        break;
    }
    case L_lower_bound: case L_upper_bound: case L_binary_search: {
        NEED(3);
        CType *et = ty[0]->base; uint64_t es = (uint64_t)et->size;
        uint64_t key = addr_of_value(et, conv_to(et, ty[2], av[2]));
        uint64_t lo = 0, hi = (av[1].u - av[0].u) / es;
        CV f = na > 3 ? av[3] : (CV){ .u = 0 }; CType *ft = na > 3 ? ty[3] : 0;
        while (lo < hi) {
            uint64_t m = (lo + hi) / 2, p = av[0].u + m * es;
            int go_right = id == L_upper_bound ? !less_with(f, ft, et, key, p) : less_with(f, ft, et, p, key);
            if (go_right) lo = m + 1; else hi = m;
        }
        if (id == L_binary_search) r.i = lo < (av[1].u - av[0].u) / es && !less_with(f, ft, et, key, av[0].u + lo * es);
        else r.u = av[0].u + lo * es;
        break;
    }
    case L_next_permutation: case L_prev_permutation: {
        NEED(2);
        CType *et = ty[0]->base; uint64_t es = (uint64_t)et->size, b = av[0].u, cnt = (av[1].u - av[0].u) / es;
        int prev = id == L_prev_permutation;
        #define LT(x, y) (prev ? cc_compare(et, b + (y) * es, b + (x) * es) < 0 : cc_compare(et, b + (x) * es, b + (y) * es) < 0)
        if (cnt < 2) { r.i = 0; break; }
        int64_t i = (int64_t)cnt - 2;
        while (i >= 0 && !LT(i, i + 1)) i--;
        uint8_t *tmp = rt_alloc_leaf((size_t)es);
        #define SWAP(x, y) do { memcpy(tmp, cc_mem + b + (x) * es, (size_t)es); memcpy(cc_mem + b + (x) * es, cc_mem + b + (y) * es, (size_t)es); memcpy(cc_mem + b + (y) * es, tmp, (size_t)es); } while (0)
        if (i >= 0) {
            uint64_t j = cnt - 1;
            while (!LT(i, j)) j--;
            SWAP((uint64_t)i, j);
        }
        for (uint64_t x = (uint64_t)(i + 1), y = cnt - 1; x < y; x++, y--) SWAP(x, y);
        r.i = i >= 0;
        #undef LT
        #undef SWAP
        break;
    }
    case L_unique: {
        NEED(2);
        CType *et = ty[0]->base; uint64_t es = (uint64_t)et->size, w = av[0].u;
        for (uint64_t p = av[0].u; p < av[1].u; p += es) if (p == av[0].u || cc_compare(et, w - es, p) != 0) { if (w != p) memmove(cc_mem + w, cc_mem + p, (size_t)es); w += es; }
        r.u = w;
        break;
    }
    case L_gcd: case L___gcd: case L_lcm: {
        NEED(2);
        int64_t a = ARGI(0) < 0 ? -ARGI(0) : ARGI(0), b = ARGI(1) < 0 ? -ARGI(1) : ARGI(1), x = a, y = b;
        while (y) { int64_t t = x % y; x = y; y = t; }
        r.i = id == L_lcm ? (x ? a / x * b : 0) : x;
        break;
    }
    case L_distance: { NEED(2); CType *et = ty[0]->base; r.i = (av[1].i - av[0].i) / (et->size ? et->size : 1); break; }
    case L_next: case L_prev: { NEED(1); CType *et = ty[0]->base; int64_t k = na > 1 ? ARGI(1) : 1; r.u = av[0].u + (uint64_t)((id == L_next ? k : -k) * et->size); break; }
    case L_advance: cc_error("advance() isn't supported: use it += n");
    case L_begin: case L_end: {
        NEED(1);
        CType *t = ty[0];
        if (t->kind == TY_ARRAY) { r.u = av[0].u + (id == L_end ? (uint64_t)t->size : 0); break; }
        uint64_t es = t->kind == TY_STRING ? 1 : esize(t);
        if (!vdata(av[0].u)) reserve(av[0].u, es, 1);
        r.u = vdata(av[0].u) + (id == L_end ? vsize(av[0].u) * es : 0);
        break;
    }
    case L_equal: { NEED(3); CType *et = ty[0]->base; uint64_t es = (uint64_t)et->size; r.i = 1; for (uint64_t p = av[0].u, q = av[2].u; p < av[1].u; p += es, q += es) if (cc_compare(et, p, q)) { r.i = 0; break; } break; }
    case L_rotate: {
        NEED(3);
        CType *et = ty[0]->base; uint64_t es = (uint64_t)et->size, a = av[0].u, m = av[1].u, e = av[2].u;
        uint64_t n1 = m - a, tot = e - a;
        uint8_t *tmp = rt_alloc_leaf((size_t)tot);
        memcpy(tmp, cc_at(m, e - m, 0), (size_t)(e - m)); memcpy(tmp + (e - m), cc_at(a, n1, 0), (size_t)n1);
        memcpy(cc_mem + a, tmp, (size_t)tot);
        (void)es;
        break;
    }
    case L_random_shuffle: case L_shuffle: {
        NEED(2);
        CType *et = ty[0]->base; uint64_t es = (uint64_t)et->size, cnt = (av[1].u - av[0].u) / es;
        uint8_t *tmp = rt_alloc_leaf((size_t)es);
        for (uint64_t i = cnt; i > 1; i--) { uint64_t j = rt_rand() % i; uint64_t x = av[0].u + (i - 1) * es, y = av[0].u + j * es; memcpy(tmp, cc_mem + x, (size_t)es); memcpy(cc_mem + x, cc_mem + y, (size_t)es); memcpy(cc_mem + y, tmp, (size_t)es); }
        break;
    }
    default: cc_error("%s() isn't available", lib_names[id]);
    }
    return r;
    #undef ARGD
    #undef ARGI
    #undef NEED
}

int cc_lib_native_call(int id, CV *args, int n, CV *out) {
    /* a library function through a pointer: the one-number ones */
    CV r; r.u = 0;
    int64_t a = n ? args[0].i : 0;
    switch (id) {
    case L_toupper: r.i = a >= 'a' && a <= 'z' ? a - 32 : a; break;
    case L_tolower: r.i = a >= 'A' && a <= 'Z' ? a + 32 : a; break;
    case L_isdigit: r.i = a >= '0' && a <= '9'; break;
    case L_isalpha: r.i = (a | 32) >= 'a' && (a | 32) <= 'z'; break;
    case L_isspace: r.i = a == ' ' || (a >= 9 && a <= 13); break;
    case L_abs: r.i = a < 0 ? -a : a; break;
    case L_sqrt: r.d = __builtin_sqrt(args[0].d); break;
    default: cc_error("%s() can't be called through a pointer here", lib_names[id]);
    }
    *out = r;
    return 0;
}

/* ---- the C_BUILTIN node -------------------------------------------------------------------------------------- */
CV cc_lib_builtin(CNode *n) {
    CV r; r.u = 0;
    if (n->op == -1 && n->flag == 0) {
        /* setw(n), setprecision(n), setfill(c) */
        r.u = (uint64_t)n->ival << 32 | (uint32_t)cc_eval(n->a).i;
        return r;
    }
    switch (n->flag) {
    case 0: return lib_call(n);
    case 1: {
        /* operators on strings, vectors and pairs */
        CType *ta = strip(n->a->ty), *tb = strip(n->b->ty);
        CV a = cc_eval(n->a), b = cc_eval(n->b);
        if (n->op == C_ADD) {
            char one[2][2];
            uint64_t la, lb;
            const char *pa = ta->kind == TY_STRING ? cc_str_ptr(a.u, &la) : ta->kind == TY_CHAR ? (one[0][0] = (char)a.i, la = 1, one[0]) : (la = strlen(cstr(a.u)), cstr(a.u));
            const char *pb = tb->kind == TY_STRING ? cc_str_ptr(b.u, &lb) : tb->kind == TY_CHAR ? (one[1][0] = (char)b.i, lb = 1, one[1]) : (lb = strlen(cstr(b.u)), cstr(b.u));
            if ((ta->kind != TY_STRING && !ty_is_ptr(ta) && ta->kind != TY_CHAR && ta->kind != TY_ARRAY) || (tb->kind != TY_STRING && !ty_is_ptr(tb) && tb->kind != TY_CHAR && tb->kind != TY_ARRAY))
                cc_error("a string can only be added to a string, a char* or a char (use to_string() for numbers)");
            uint64_t s = cc_temp(ty_string);
            char *t = rt_alloc_leaf((size_t)(la + lb) + 1);
            memcpy(t, pa, (size_t)la); memcpy(t + la, pb, (size_t)lb);
            cc_str_set(s, t, la + lb);
            r.u = s;
            return r;
        }
        int c;
        if (ta->kind == TY_STRING || tb->kind == TY_STRING) {
            uint64_t la, lb; char one[2];
            const char *pa = ta->kind == TY_STRING ? cc_str_ptr(a.u, &la) : ta->kind == TY_CHAR ? (one[0] = (char)a.i, la = 1, one) : (la = strlen(cstr(a.u)), cstr(a.u));
            char two[2];
            const char *pb = tb->kind == TY_STRING ? cc_str_ptr(b.u, &lb) : tb->kind == TY_CHAR ? (two[0] = (char)b.i, lb = 1, two) : (lb = strlen(cstr(b.u)), cstr(b.u));
            uint64_t k = la < lb ? la : lb;
            int m = memcmp(pa, pb, (size_t)k);
            c = m ? (m < 0 ? -1 : 1) : la < lb ? -1 : la > lb;
        } else {
            if (ta != tb) cc_error("those two can't be compared");
            c = cc_compare(ta, a.u, b.u);
        }
        switch (n->op) {
        case C_EQ: r.i = c == 0; break; case C_NE: r.i = c != 0; break; case C_LT: r.i = c < 0; break;
        case C_LE: r.i = c <= 0; break; case C_GT: r.i = c > 0; break; default: r.i = c >= 0; break;
        }
        return r;
    }
    case 2: {
        CType *t = strip(n->a->ty);
        uint64_t obj = cc_addr(n->a);
        if (t->kind == TY_STRING) return string_method(n, obj);
        if (t->kind == TY_VECTOR) return vector_method(n, obj, t);
        return assoc_method(n, obj, t);
    }
    case 3: {
        cc_eval(n->a);                                     /* what's to the left prints first */
        CV v = cc_eval(n->b);
        print_value(n->b->ty, v);
        return r;
    }
    case 4: {
        cc_eval(n->a);
        CType *t = strip(n->b->ty);
        uint64_t a = cc_addr(n->b);
        if (stream_ok) read_value(t, a);
        return r;
    }
    case 5: {
        /* cin.ignore(), cin.get(), cout.precision(n), cout.flush(), cin.fail() ... */
        const char *m = n->name;
        if (!strcmp(m, "ignore")) { int64_t k = n->nk ? cc_eval(n->kids[0]).i : 1; int d = n->nk > 1 ? (int)cc_eval(n->kids[1]).i : -1; for (int64_t i = 0; i < k && i < 1000000; i++) { int c = cc_stdin_getc(0); if (c < 0 || c == d) break; } }
        else if (!strcmp(m, "get")) { r.i = cc_stdin_getc(0); if (n->nk) { uint64_t a = cc_addr(n->kids[0]); *cc_at(a, 1, 1) = (uint8_t)r.i; } }
        else if (!strcmp(m, "peek")) { r.i = cc_stdin_getc(0); if (r.i >= 0) cc_stdin_unget((int)r.i); }
        else if (!strcmp(m, "precision")) { r.i = io.prec; if (n->nk) io.prec = (int)cc_eval(n->kids[0]).i; }
        else if (!strcmp(m, "flush")) cc_flush();
        else if (!strcmp(m, "fail") || !strcmp(m, "eof") || !strcmp(m, "bad")) r.i = !stream_ok;
        else if (!strcmp(m, "good")) r.i = stream_ok;
        else if (!strcmp(m, "clear")) stream_ok = 1;
        else if (!strcmp(m, "setf") || !strcmp(m, "unsetf")) {
            int on = m[0] == 's';
            int f = n->nk ? (int)cc_eval(n->kids[0]).i : 0;
            switch (f) {
            case 1: io.fixed = on; if (on) io.sci = 0; break;
            case 2: io.sci = on; if (on) io.fixed = 0; break;
            case 3: io.boolalpha = on; break;
            case 4: io.left = on; break;
            case 5: io.left = !on; break;
            case 6: io.showpoint = on; break;
            case 7: if (!on) io.fixed = io.sci = 0; break;
            }
        }
        else if (!strcmp(m, "tie") || !strcmp(m, "sync_with_stdio")) { }
        else if (!strcmp(m, "width")) { if (n->nk) io.width = (int)cc_eval(n->kids[0]).i; }
        else if (!strcmp(m, "put")) { char c = (char)cc_eval(n->kids[0]).i; cc_out(&c, 1); }
        else if (!strcmp(m, "write")) { uint64_t p = cc_eval(n->kids[0]).u; int64_t k = cc_eval(n->kids[1]).i; cc_out((const char *)cc_at(p, (uint64_t)k, 0), (uint64_t)k); }
        else cc_error("streams have no method %s() here", m);
        return r;
    }
    case 6: {
        /* s[i], v[i], m[key] */
        CType *t = strip(n->a->ty);
        uint64_t obj = cc_addr(n->a);
        if (t->kind == TY_MAP) {
            CV k = cc_eval(n->b);
            r.u = map_at(t, obj, addr_of_value(t->base, k), 1);
            return r;
        }
        int64_t i = cc_eval(n->b).i;
        uint64_t es = t->kind == TY_STRING ? 1 : (uint64_t)t->base->size, sz = vsize(obj);
        if (i < 0 || (uint64_t)i >= sz + (t->kind == TY_STRING)) cc_error("%s[%lld]: the index is out of range (size %llu)", t->kind == TY_STRING ? "string" : "vector", (long long)i, (unsigned long long)sz);
        if (t->kind == TY_STRING && (uint64_t)i == sz && !vdata(obj)) cc_str_set(obj, "", 0);
        r.u = vdata(obj) + (uint64_t)i * es;
        return r;
    }
    }
    cc_error("an unknown library operation");
}

void cc_lib_init(void) {
    lib_types = rt_alloc(sizeof(CType *) * L_COUNT);
    rt_root((void **)&lib_types);
    on = 0; ipos = ilen = 0; ieof = 0; unget_c = -1; stream_ok = 1; exit_code = 0; strtok_at = 0; crand_state = 1;
    io.prec = 6; io.fixed = io.sci = io.width = io.left = io.boolalpha = io.showpoint = 0; io.base = 10; io.fill = ' ';
    (void)peeked;
}
