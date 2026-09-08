#include <stdint.h>
#include "cpp.h"
#include "fs.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include <stdio.h>

/* ===========================================================================
 *  BoltOS  -  kernel/cpp.c
 *  The C preprocessor. See include/cpp.h for what it supports and why.
 *
 *  Structure: a growable output buffer, a macro table, a conditional stack,
 *  and one recursive routine that processes a file line by line. #include
 *  recurses into it with the included file's own directory, so a header can
 *  include its neighbours the way C programmers expect.
 *
 *  Macro expansion is done on the text of each line before it is emitted, with
 *  a per-expansion "currently expanding" mark on each macro so that a macro
 *  mentioning its own name expands once and then stops rather than looping for
 *  ever. That is the rule real preprocessors use, and without it a perfectly
 *  ordinary `#define getc getc` hangs the machine.
 * ===========================================================================*/

#define MAX_MACROS     512
#define MAX_PARAMS     16
#define MAX_INCDIRS    8
#define MAX_ONCE       128
#define MAX_DEPTH      24
#define MAX_COND       64
#define NAME_MAX_      64

typedef struct {
    char  name[NAME_MAX_];
    char *body;                       /* heap; 0 for a bare #define NAME     */
    char  params[MAX_PARAMS][NAME_MAX_];
    int   nparams;                    /* -1 = object-like, >=0 function-like */
    int   expanding;                  /* guards self-referential macros      */
    int   used;
} macro;

static macro   mtab[MAX_MACROS];
static char    incdir[MAX_INCDIRS][FS_PATH_MAX];
static int     nincdir;
static char    once[MAX_ONCE][FS_PATH_MAX];
static int     nonce;

/* ------------------------------------------------------------------ output */
typedef struct { char *b; uint32_t len, cap; int oom; } obuf;

static void ob_init(obuf *o) { o->b = (char *)kmalloc(4096); o->len = 0;
                               o->cap = o->b ? 4096 : 0; o->oom = o->b ? 0 : 1; }
static void ob_put(obuf *o, char c) {
    if (o->oom) return;
    if (o->len + 2 > o->cap) {
        uint32_t ncap = o->cap * 2;
        char *nb = (char *)kmalloc(ncap);
        if (!nb) { o->oom = 1; return; }
        memcpy(nb, o->b, o->len);
        kfree(o->b);
        o->b = nb; o->cap = ncap;
    }
    o->b[o->len++] = c;
}
static void ob_puts(obuf *o, const char *s) { while (*s) ob_put(o, *s++); }
static void ob_putn(obuf *o, const char *s, uint32_t n) { for (uint32_t i = 0; i < n; i++) ob_put(o, s[i]); }

/* ------------------------------------------------------------------ macros */
static int ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
static int ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static macro *macro_find(const char *name, int len) {
    for (int i = 0; i < MAX_MACROS; i++) {
        if (!mtab[i].used) continue;
        if ((int)strlen(mtab[i].name) == len && !strncmp(mtab[i].name, name, (uint32_t)len))
            return &mtab[i];
    }
    return 0;
}

static macro *macro_slot(const char *name, int len) {
    macro *m = macro_find(name, len);
    if (m) { if (m->body) { kfree(m->body); m->body = 0; } return m; }
    for (int i = 0; i < MAX_MACROS; i++)
        if (!mtab[i].used) {
            m = &mtab[i];
            memset(m, 0, sizeof(*m));
            if (len > NAME_MAX_ - 1) len = NAME_MAX_ - 1;
            memcpy(m->name, name, (uint32_t)len);
            m->name[len] = 0;
            m->used = 1;
            m->nparams = -1;
            return m;
        }
    return 0;
}

void cpp_reset_defines(void) {
    for (int i = 0; i < MAX_MACROS; i++) {
        if (mtab[i].body) kfree(mtab[i].body);
        memset(&mtab[i], 0, sizeof(mtab[i]));
    }
    nonce = 0;
}

void cpp_reset_include_dirs(void) { nincdir = 0; }

void cpp_add_include_dir(const char *dir) {
    if (nincdir >= MAX_INCDIRS) return;
    for (int i = 0; i < nincdir; i++) if (!strcmp(incdir[i], dir)) return;
    strncpy(incdir[nincdir], dir, FS_PATH_MAX - 1);
    incdir[nincdir][FS_PATH_MAX - 1] = 0;
    nincdir++;
}

void cpp_define_cli(const char *namevalue) {
    const char *eq = namevalue;
    while (*eq && *eq != '=') eq++;
    int nlen = (int)(eq - namevalue);
    if (!nlen) return;
    macro *m = macro_slot(namevalue, nlen);
    if (!m) return;
    m->nparams = -1;
    const char *val = (*eq == '=') ? eq + 1 : "1";
    uint32_t vl = (uint32_t)strlen(val);
    m->body = (char *)kmalloc(vl + 1);
    if (m->body) { memcpy(m->body, val, vl); m->body[vl] = 0; }
}

/* ------------------------------------------------------- macro expansion --*/
/* Expand macros in `in` (one logical line) into `out`. Recursion is bounded by
 * `depth`; a macro currently being expanded is left alone, which is what stops
 * `#define X X + 1` from running away. */
static void expand(const char *in, obuf *out, int depth);

/* Collect one macro argument, honouring nesting and strings, stopping at a
 * top-level comma or the closing paren. Returns the position after it. */
static const char *collect_arg(const char *p, obuf *arg, int *last) {
    int par = 0;
    while (*p) {
        if (*p == '"' || *p == '\'') {          /* copy a literal verbatim */
            char q = *p;
            ob_put(arg, *p++);
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) ob_put(arg, *p++);
                ob_put(arg, *p++);
            }
            if (*p) ob_put(arg, *p++);
            continue;
        }
        if (*p == '(') par++;
        if (*p == ')') { if (par == 0) { *last = 1; return p + 1; } par--; }
        if (*p == ',' && par == 0) return p + 1;
        ob_put(arg, *p++);
    }
    *last = 1;
    return p;
}

static void expand(const char *in, obuf *out, int depth) {
    if (depth > 16) { ob_puts(out, in); return; }
    const char *p = in;
    while (*p) {
        /* String and character literals pass through untouched -- a macro name
         * inside "quotes" is text, not a macro. */
        if (*p == '"' || *p == '\'') {
            char q = *p;
            ob_put(out, *p++);
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) ob_put(out, *p++);
                ob_put(out, *p++);
            }
            if (*p) ob_put(out, *p++);
            continue;
        }
        if (!ident_start(*p)) { ob_put(out, *p++); continue; }

        const char *s = p;
        while (ident_char(*p)) p++;
        int len = (int)(p - s);

        macro *m = macro_find(s, len);
        if (!m || m->expanding) { ob_putn(out, s, (uint32_t)len); continue; }

        if (m->nparams < 0) {                        /* object-like */
            m->expanding = 1;
            if (m->body) expand(m->body, out, depth + 1);
            m->expanding = 0;
            continue;
        }

        /* Function-like: only expands when the next thing really is a '('. */
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '(') { ob_putn(out, s, (uint32_t)len); continue; }
        q++;

        obuf args[MAX_PARAMS];
        int nargs = 0, last = 0;
        for (int i = 0; i < MAX_PARAMS; i++) { args[i].b = 0; args[i].len = 0; args[i].cap = 0; args[i].oom = 0; }
        while (!last && nargs < MAX_PARAMS) {
            ob_init(&args[nargs]);
            q = collect_arg(q, &args[nargs], &last);
            ob_put(&args[nargs], 0);
            nargs++;
        }
        /* `f()` with one empty argument is zero arguments. */
        if (nargs == 1 && m->nparams == 0 && args[0].b && args[0].b[0] == 0) nargs = 0;

        /* Substitute the parameters into the body, then expand the result. */
        obuf sub; ob_init(&sub);
        for (const char *b = m->body ? m->body : ""; *b; ) {
            if (*b == '#' && b[1] != '#') {          /* stringify */
                const char *bs = b + 1;
                while (*bs == ' ') bs++;
                if (ident_start(*bs)) {
                    const char *ns = bs;
                    while (ident_char(*bs)) bs++;
                    int nl = (int)(bs - ns);
                    int pi = -1;
                    for (int i = 0; i < m->nparams; i++)
                        if ((int)strlen(m->params[i]) == nl && !strncmp(m->params[i], ns, (uint32_t)nl)) pi = i;
                    if (pi >= 0 && pi < nargs) {
                        ob_put(&sub, '"');
                        for (const char *t = args[pi].b; t && *t; t++) {
                            if (*t == '"' || *t == '\\') ob_put(&sub, '\\');
                            ob_put(&sub, *t);
                        }
                        ob_put(&sub, '"');
                        b = bs;
                        continue;
                    }
                }
            }
            if (*b == '"' || *b == '\'') {
                char qq = *b;
                ob_put(&sub, *b++);
                while (*b && *b != qq) { if (*b == '\\' && b[1]) ob_put(&sub, *b++); ob_put(&sub, *b++); }
                if (*b) ob_put(&sub, *b++);
                continue;
            }
            if (!ident_start(*b)) { ob_put(&sub, *b++); continue; }
            const char *bs2 = b;
            while (ident_char(*b)) b++;
            int bl = (int)(b - bs2);
            int pi = -1;
            for (int i = 0; i < m->nparams; i++)
                if ((int)strlen(m->params[i]) == bl && !strncmp(m->params[i], bs2, (uint32_t)bl)) pi = i;
            if (pi >= 0 && pi < nargs) ob_puts(&sub, args[pi].b ? args[pi].b : "");
            else                       ob_putn(&sub, bs2, (uint32_t)bl);
        }
        ob_put(&sub, 0);

        m->expanding = 1;
        expand(sub.b ? sub.b : "", out, depth + 1);
        m->expanding = 0;

        if (sub.b) kfree(sub.b);
        for (int i = 0; i < nargs; i++) if (args[i].b) kfree(args[i].b);
        p = q;
    }
}

/* -------------------------------------------------- #if expression parsing */
/* A small precedence-climbing evaluator over the already-expanded text. Only
 * integers exist here, which is exactly what the standard says #if sees. */
typedef struct { const char *p; int bad; } ev;

static void ev_ws(ev *e) { while (*e->p == ' ' || *e->p == '\t') e->p++; }
static long ev_expr(ev *e, int minprec);

static long ev_primary(ev *e) {
    ev_ws(e);
    if (*e->p == '!') { e->p++; return !ev_primary(e); }
    if (*e->p == '-') { e->p++; return -ev_primary(e); }
    if (*e->p == '(') {
        e->p++;
        long v = ev_expr(e, 0);
        ev_ws(e);
        if (*e->p == ')') e->p++; else e->bad = 1;
        return v;
    }
    if (*e->p >= '0' && *e->p <= '9') {
        long v = 0;
        if (e->p[0] == '0' && (e->p[1] == 'x' || e->p[1] == 'X')) {
            e->p += 2;
            while (1) {
                char c = *e->p;
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if ((c | 32) >= 'a' && (c | 32) <= 'f') d = (c | 32) - 'a' + 10;
                else break;
                v = v * 16 + d; e->p++;
            }
        } else {
            while (*e->p >= '0' && *e->p <= '9') { v = v * 10 + (*e->p - '0'); e->p++; }
        }
        while (*e->p == 'u' || *e->p == 'U' || *e->p == 'l' || *e->p == 'L') e->p++;
        return v;
    }
    if (ident_start(*e->p)) {
        /* Any identifier still standing after expansion is an undefined macro,
         * which the standard says evaluates to 0. */
        while (ident_char(*e->p)) e->p++;
        return 0;
    }
    e->bad = 1;
    return 0;
}

static int ev_binop(const char *p, int *len) {
    if (p[0] == '&' && p[1] == '&') { *len = 2; return 1; }
    if (p[0] == '|' && p[1] == '|') { *len = 2; return 2; }
    if (p[0] == '=' && p[1] == '=') { *len = 2; return 3; }
    if (p[0] == '!' && p[1] == '=') { *len = 2; return 4; }
    if (p[0] == '<' && p[1] == '=') { *len = 2; return 5; }
    if (p[0] == '>' && p[1] == '=') { *len = 2; return 6; }
    if (p[0] == '<') { *len = 1; return 7; }
    if (p[0] == '>') { *len = 1; return 8; }
    if (p[0] == '+') { *len = 1; return 9; }
    if (p[0] == '-') { *len = 1; return 10; }
    if (p[0] == '*') { *len = 1; return 11; }
    if (p[0] == '/') { *len = 1; return 12; }
    return 0;
}
static int ev_prec(int op) {
    switch (op) {
    case 2: return 1;                     /* ||          */
    case 1: return 2;                     /* &&          */
    case 3: case 4: return 3;             /* == !=       */
    case 5: case 6: case 7: case 8: return 4;   /* < <= > >= */
    case 9: case 10: return 5;            /* + -         */
    case 11: case 12: return 6;           /* * /         */
    default: return 0;
    }
}

static long ev_expr(ev *e, int minprec) {
    long lhs = ev_primary(e);
    for (;;) {
        ev_ws(e);
        int len = 0, op = ev_binop(e->p, &len);
        if (!op || ev_prec(op) < minprec) return lhs;
        e->p += len;
        long rhs = ev_expr(e, ev_prec(op) + 1);
        switch (op) {
        case 1:  lhs = (lhs && rhs); break;
        case 2:  lhs = (lhs || rhs); break;
        case 3:  lhs = (lhs == rhs); break;
        case 4:  lhs = (lhs != rhs); break;
        case 5:  lhs = (lhs <= rhs); break;
        case 6:  lhs = (lhs >= rhs); break;
        case 7:  lhs = (lhs <  rhs); break;
        case 8:  lhs = (lhs >  rhs); break;
        case 9:  lhs = lhs + rhs; break;
        case 10: lhs = lhs - rhs; break;
        case 11: lhs = lhs * rhs; break;
        case 12: lhs = rhs ? lhs / rhs : 0; break;
        }
    }
}

/* Replace `defined X` / `defined(X)` with 1 or 0 before macro expansion, since
 * `defined` must see the name and not its replacement. */
static void resolve_defined(const char *in, obuf *out) {
    const char *p = in;
    while (*p) {
        if (ident_start(*p)) {
            const char *s = p;
            while (ident_char(*p)) p++;
            int len = (int)(p - s);
            if (len == 7 && !strncmp(s, "defined", 7)) {
                const char *q = p;
                while (*q == ' ' || *q == '\t') q++;
                int paren = (*q == '(');
                if (paren) { q++; while (*q == ' ' || *q == '\t') q++; }
                const char *ns = q;
                while (ident_char(*q)) q++;
                int nl = (int)(q - ns);
                if (paren) { while (*q == ' ' || *q == '\t') q++; if (*q == ')') q++; }
                ob_put(out, macro_find(ns, nl) ? '1' : '0');
                p = q;
                continue;
            }
            ob_putn(out, s, (uint32_t)len);
            continue;
        }
        ob_put(out, *p++);
    }
}

static int eval_cond(const char *text) {
    obuf d; ob_init(&d);
    resolve_defined(text, &d);
    ob_put(&d, 0);
    obuf x; ob_init(&x);
    expand(d.b ? d.b : "", &x, 0);
    ob_put(&x, 0);
    ev e = { x.b ? x.b : "", 0 };
    long v = ev_expr(&e, 0);
    if (d.b) kfree(d.b);
    if (x.b) kfree(x.b);
    return v != 0;
}

/* ------------------------------------------------------------- the driver */
typedef struct {
    obuf out;
    char err[192];
    int  failed;
} cppctx;

static void cpp_fail(cppctx *c, const char *file, int line, const char *msg, const char *det) {
    if (c->failed) return;
    c->failed = 1;
    snprintf(c->err, sizeof c->err, "%s:%d: %s%s%s",
             file ? file : "<input>", line, msg, det ? " " : "", det ? det : "");
}

static void dirname_of(const char *path, char *out, uint32_t cap) {
    uint32_t n = path ? (uint32_t)strlen(path) : 0;
    while (n && path[n - 1] != '/') n--;
    if (!n) { strncpy(out, ".", cap - 1); out[cap - 1] = 0; return; }
    if (n > cap - 1) n = cap - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

static void process(cppctx *c, const char *src, const char *path, int depth);

/* Locate and process an included file. */
static void do_include(cppctx *c, const char *spec, int angled,
                       const char *from, int line, int depth) {
    char cand[FS_PATH_MAX];
    fs_node *f = 0;

    if (!angled) {                                  /* the includer's own dir */
        char dir[FS_PATH_MAX];
        dirname_of(from, dir, sizeof dir);
        snprintf(cand, sizeof cand, "%s%s%s", dir,
                 (dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/", spec);
        f = fs_lookup(cand);
    }
    for (int i = 0; !f && i < nincdir; i++) {
        snprintf(cand, sizeof cand, "%s%s%s", incdir[i],
                 (incdir[i][0] && incdir[i][strlen(incdir[i]) - 1] == '/') ? "" : "/", spec);
        f = fs_lookup(cand);
    }
    if (!f) { f = fs_lookup(spec); if (f) { strncpy(cand, spec, sizeof cand - 1); cand[sizeof cand - 1] = 0; } }

    if (!f || f->is_dir || !f->data) { cpp_fail(c, from, line, "cannot find include", spec); return; }
    for (int i = 0; i < nonce; i++) if (!strcmp(once[i], cand)) return;   /* #pragma once */

    char *text = (char *)kmalloc(f->size + 1);
    if (!text) { cpp_fail(c, from, line, "out of memory including", spec); return; }
    memcpy(text, f->data, f->size);
    text[f->size] = 0;
    process(c, text, cand, depth + 1);
    kfree(text);
}

static void process(cppctx *c, const char *src, const char *path, int depth) {
    if (c->failed) return;
    if (depth > MAX_DEPTH) { cpp_fail(c, path, 0, "include nesting too deep", 0); return; }

    /* One entry per open conditional: whether this branch emits, and whether
     * any branch of this #if has already been taken. */
    struct { int emit, taken, parent; } cond[MAX_COND];
    int ncond = 0;
    int emitting = 1;

    const char *p = src;
    int line = 0;

    char buf[4096];
    while (*p || line == 0) {
        if (!*p) break;
        line++;

        /* Take one logical line, joining backslash continuations. */
        uint32_t n = 0;
        for (;;) {
            while (*p && *p != '\n' && n < sizeof buf - 1) buf[n++] = *p++;
            if (n && buf[n - 1] == '\\') { n--; if (*p == '\n') { p++; line++; } continue; }
            break;
        }
        if (*p == '\n') p++;
        buf[n] = 0;
        while (n && (buf[n-1] == '\r' || buf[n-1] == ' ' || buf[n-1] == '\t')) buf[--n] = 0;

        const char *l = buf;
        while (*l == ' ' || *l == '\t') l++;

        if (*l != '#') {
            if (emitting) {
                obuf x; ob_init(&x);
                expand(l == buf ? buf : l, &x, 0);
                ob_put(&x, 0);
                if (x.b) { ob_puts(&c->out, x.b); kfree(x.b); }
            }
            ob_put(&c->out, '\n');            /* keep line numbers lined up */
            continue;
        }

        /* A directive. */
        l++;
        while (*l == ' ' || *l == '\t') l++;
        const char *ds = l;
        while (ident_char(*l)) l++;
        int dlen = (int)(l - ds);
        while (*l == ' ' || *l == '\t') l++;

        #define IS(w) (dlen == (int)sizeof(w) - 1 && !strncmp(ds, w, sizeof(w) - 1))

        if (IS("ifdef") || IS("ifndef")) {
            const char *ns = l;
            while (ident_char(*l)) l++;
            int found = macro_find(ns, (int)(l - ns)) != 0;
            int want  = IS("ifdef") ? found : !found;
            if (ncond < MAX_COND) {
                cond[ncond].parent = emitting;
                cond[ncond].emit   = emitting && want;
                cond[ncond].taken  = cond[ncond].emit;
                emitting = cond[ncond].emit;
                ncond++;
            }
        } else if (IS("if")) {
            int want = emitting ? eval_cond(l) : 0;
            if (ncond < MAX_COND) {
                cond[ncond].parent = emitting;
                cond[ncond].emit   = emitting && want;
                cond[ncond].taken  = cond[ncond].emit;
                emitting = cond[ncond].emit;
                ncond++;
            }
        } else if (IS("elif")) {
            if (!ncond) { cpp_fail(c, path, line, "#elif without #if", 0); return; }
            int k = ncond - 1;
            int want = cond[k].parent && !cond[k].taken && eval_cond(l);
            cond[k].emit = want;
            if (want) cond[k].taken = 1;
            emitting = want;
        } else if (IS("else")) {
            if (!ncond) { cpp_fail(c, path, line, "#else without #if", 0); return; }
            int k = ncond - 1;
            cond[k].emit = cond[k].parent && !cond[k].taken;
            if (cond[k].emit) cond[k].taken = 1;
            emitting = cond[k].emit;
        } else if (IS("endif")) {
            if (!ncond) { cpp_fail(c, path, line, "#endif without #if", 0); return; }
            ncond--;
            emitting = ncond ? cond[ncond - 1].emit : 1;
        } else if (!emitting) {
            /* Everything else inside a false branch is skipped entirely --
             * including #include and #error, which is the whole point of
             * conditional compilation. */
        } else if (IS("include")) {
            char spec[FS_PATH_MAX];
            int angled = 0;
            if (*l == '"' || *l == '<') {
                angled = (*l == '<');
                char close = angled ? '>' : '"';
                l++;
                uint32_t k = 0;
                while (*l && *l != close && k < sizeof spec - 1) spec[k++] = *l++;
                spec[k] = 0;
                do_include(c, spec, angled, path, line, depth);
                if (c->failed) return;
            } else {
                cpp_fail(c, path, line, "malformed #include", 0);
                return;
            }
        } else if (IS("define")) {
            const char *ns = l;
            while (ident_char(*l)) l++;
            int nl = (int)(l - ns);
            if (!nl) { cpp_fail(c, path, line, "#define needs a name", 0); return; }
            macro *m = macro_slot(ns, nl);
            if (!m) { cpp_fail(c, path, line, "too many macros", 0); return; }
            m->nparams = -1;
            if (*l == '(') {                       /* function-like: no space */
                l++;
                m->nparams = 0;
                while (*l && *l != ')') {
                    while (*l == ' ' || *l == '\t') l++;
                    const char *ps = l;
                    while (ident_char(*l)) l++;
                    if (l > ps && m->nparams < MAX_PARAMS) {
                        int pl = (int)(l - ps);
                        if (pl > NAME_MAX_ - 1) pl = NAME_MAX_ - 1;
                        memcpy(m->params[m->nparams], ps, (uint32_t)pl);
                        m->params[m->nparams][pl] = 0;
                        m->nparams++;
                    }
                    while (*l == ' ' || *l == '\t') l++;
                    if (*l == ',') l++;
                }
                if (*l == ')') l++;
            }
            while (*l == ' ' || *l == '\t') l++;
            uint32_t bl = (uint32_t)strlen(l);
            m->body = (char *)kmalloc(bl + 1);
            if (m->body) { memcpy(m->body, l, bl); m->body[bl] = 0; }
        } else if (IS("undef")) {
            const char *ns = l;
            while (ident_char(*l)) l++;
            macro *m = macro_find(ns, (int)(l - ns));
            if (m) { if (m->body) kfree(m->body); memset(m, 0, sizeof(*m)); }
        } else if (IS("pragma")) {
            if (!strncmp(l, "once", 4) && path && nonce < MAX_ONCE) {
                strncpy(once[nonce], path, FS_PATH_MAX - 1);
                once[nonce][FS_PATH_MAX - 1] = 0;
                nonce++;
            }
        } else if (IS("error")) {
            cpp_fail(c, path, line, "#error", l);
            return;
        } else if (IS("warning")) {
            kprintf("%s:%d: warning: %s\n", path ? path : "<input>", line, l);
        } else if (IS("line")) {
            /* accepted and ignored */
        } else {
            cpp_fail(c, path, line, "unknown directive #", ds);
            return;
        }
        #undef IS

        ob_put(&c->out, '\n');                 /* directives keep their line */
    }

    if (ncond) cpp_fail(c, path, line, "unterminated #if", 0);
}

char *cpp_run(const char *src, const char *path, char *err, uint32_t errcap) {
    cppctx c;
    memset(&c, 0, sizeof c);
    ob_init(&c.out);
    if (c.out.oom) { if (err) snprintf(err, errcap, "out of memory"); return 0; }

    process(&c, src, path, 0);

    if (c.failed || c.out.oom) {
        if (err) snprintf(err, errcap, "%s", c.out.oom ? "out of memory" : c.err);
        if (c.out.b) kfree(c.out.b);
        return 0;
    }
    ob_put(&c.out, 0);
    if (err) err[0] = 0;
    return c.out.b;
}
