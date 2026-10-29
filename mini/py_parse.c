/* ===========================================================================
 *  BoltOS Mini  -  mini/py_parse.c
 *
 *  Python source to a tree (py.h): the tokenizer, with INDENT and DEDENT
 *  from the leading spaces and lines joined inside brackets; a recursive-
 *  descent parser for the grammar, precedence level by level; and a pass
 *  over the finished tree that decides where every name lives -- a slot in
 *  its function's frame, a slot some frames out, a class body's namespace,
 *  or the module's globals -- by Python's rule: a name assigned anywhere
 *  in a function is that function's, unless it says global or nonlocal.
 *
 *  A syntax error raises SyntaxError with the line, as Python does.
 * ===========================================================================*/
#include <stdint.h>
#include "py.h"
#include "string.h"
#include "stdio.h"

/* ---- tokens --------------------------------------------------------------------------------- */
enum { T_EOF, T_NL, T_INDENT, T_DEDENT, T_NAME, T_NUM, T_STR, T_FSTR, T_OP };
typedef struct { uint8_t k; int line; Str *s; V v; const char *op; } Tok;

static const char *const OPS[] = {
    "**=", "//=", ">>=", "<<=", "...", "->", ":=", "**", "//", "<<", ">>", "<=", ">=", "==", "!=", "+=", "-=",
    "*=", "/=", "%=", "&=", "|=", "^=", "@=", "+", "-", "*", "/", "%", "@", "<", ">", "=", ".", ",", ":", ";",
    "(", ")", "[", "]", "{", "}", "~", "&", "|", "^", 0
};

typedef struct {
    const char *src, *p;
    int line;
    Tok *t; int n, cap;
    Str *file;
} Lex;

static void syntax(Str *file, int line, const char *msg) {
    py_set_line(line);
    V e = make_exc(P->e_syntax, vstr(msg));
    Inst *in = (Inst *)e.u.o;
    dict_set_str(in->dict, intern_c("lineno"), vint(line));
    char b[300];
    snprintf(b, sizeof b, "  File \"%s\", line %d\n", file ? file->s : "<string>", line);
    dict_set_str(in->dict, P->s_tb, vstr(b));
    raise(e);
}

static void tok_add(Lex *L, int k) {
    if (L->n == L->cap) {
        int nc = L->cap ? L->cap * 2 : 256;
        L->t = rt_realloc(L->t, sizeof(Tok) * (size_t)L->cap, sizeof(Tok) * (size_t)nc);
        L->cap = nc;
    }
    Tok *t = &L->t[L->n++];
    memset(t, 0, sizeof *t);
    t->k = (uint8_t)k; t->line = L->line;
}

static int idstart(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 128; }
static int idchar(int c) { return idstart(c) || (c >= '0' && c <= '9'); }

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

/* A string literal from after its opening quote(s). */
static void lex_string(Lex *L, int raw, int fmt, int bytes) {
    (void)bytes;
    char q = *L->p;
    int triple = L->p[1] == q && L->p[2] == q;
    L->p += triple ? 3 : 1;
    SB sb; sb_new(&sb, 32);
    int start_line = L->line;
    for (;;) {
        char c = *L->p;
        if (!c) syntax(L->file, start_line, triple ? "unterminated triple-quoted string literal" : "unterminated string literal");
        if (c == q && (!triple || (L->p[1] == q && L->p[2] == q))) { L->p += triple ? 3 : 1; break; }
        if (c == '\n') { if (!triple) syntax(L->file, L->line, "unterminated string literal"); L->line++; }
        if (c == '\\' && !raw) {
            char e = L->p[1];
            L->p += 2;
            switch (e) {
            case 'n': sb_addc(&sb, '\n'); break;
            case 't': sb_addc(&sb, '\t'); break;
            case 'r': sb_addc(&sb, '\r'); break;
            case '0': if (L->p[0] < '0' || L->p[0] > '7') { sb_addc(&sb, 0); break; } L->p--; /* fall through */
            case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
                int v = 0, k = 0;
                L->p--;
                while (k < 3 && *L->p >= '0' && *L->p <= '7') { v = v * 8 + (*L->p++ - '0'); k++; }
                sb_addc(&sb, (char)v); break;
            }
            case 'a': sb_addc(&sb, 7); break;
            case 'b': sb_addc(&sb, 8); break;
            case 'f': sb_addc(&sb, 12); break;
            case 'v': sb_addc(&sb, 11); break;
            case '\\': sb_addc(&sb, '\\'); break;
            case '\'': sb_addc(&sb, '\''); break;
            case '"': sb_addc(&sb, '"'); break;
            case '\n': L->line++; break;
            case 'x': {
                int h1 = hexval(L->p[0]), h2 = h1 >= 0 ? hexval(L->p[1]) : -1;
                if (h2 < 0) syntax(L->file, L->line, "invalid \\x escape");
                sb_addc(&sb, (char)(h1 * 16 + h2)); L->p += 2; break;
            }
            case 'u': case 'U': {
                int n = e == 'u' ? 4 : 8; uint32_t v = 0;
                for (int i = 0; i < n; i++) { int h = hexval(L->p[i]); if (h < 0) syntax(L->file, L->line, "invalid \\u escape"); v = v * 16 + (uint32_t)h; }
                L->p += n;
                sb_addc(&sb, v < 256 ? (char)v : '?'); break;
            }
            default:
                if (fmt && (e == '{' || e == '}')) { sb_addc(&sb, '\\'); sb_addc(&sb, e); }
                else { sb_addc(&sb, '\\'); sb_addc(&sb, e); }
            }
            continue;
        }
        sb_addc(&sb, c);
        L->p++;
    }
    tok_add(L, fmt ? T_FSTR : T_STR);
    L->t[L->n - 1].line = start_line;
    L->t[L->n - 1].s = sb_str(&sb);
}

static void lex(Lex *L) {
    int indents[64], ni = 1, depth = 0, at_line_start = 1;
    indents[0] = 0;
    L->line = 1;
    while (*L->p) {
        if (at_line_start && !depth) {
            /* measure the indentation; skip blank and comment-only lines */
            int col = 0;
            const char *q = L->p;
            while (*q == ' ' || *q == '\t' || *q == '\f') { col = *q == '\t' ? (col / 8 + 1) * 8 : col + 1; q++; }
            if (*q == '\n' || *q == '#' || *q == '\r' || !*q) {
                while (*q && *q != '\n') q++;
                if (*q == '\n') { q++; L->line++; }
                L->p = q;
                continue;
            }
            L->p = q;
            at_line_start = 0;
            if (col > indents[ni - 1]) {
                if (ni >= 64) syntax(L->file, L->line, "too many levels of indentation");
                indents[ni++] = col; tok_add(L, T_INDENT);
            } else {
                while (col < indents[ni - 1]) { ni--; tok_add(L, T_DEDENT); }
                if (col != indents[ni - 1]) syntax(L->file, L->line, "unindent does not match any outer indentation level");
            }
        }
        char c = *L->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f') { L->p++; continue; }
        if (c == '#') { while (*L->p && *L->p != '\n') L->p++; continue; }
        if (c == '\\' && (L->p[1] == '\n' || (L->p[1] == '\r' && L->p[2] == '\n'))) { L->p += L->p[1] == '\n' ? 2 : 3; L->line++; continue; }
        if (c == '\n') {
            L->p++;
            if (!depth) { if (L->n && L->t[L->n - 1].k != T_NL) tok_add(L, T_NL); at_line_start = 1; }
            L->line++;
            continue;
        }
        if (idstart((unsigned char)c)) {
            const char *s = L->p;
            while (idchar((unsigned char)*L->p)) L->p++;
            int n = (int)(L->p - s);
            /* string prefixes */
            if (n <= 2 && (*L->p == '\'' || *L->p == '"')) {
                int raw = 0, fmt = 0, by = 0, ok = 1;
                for (int i = 0; i < n; i++) {
                    char pc = (char)(s[i] | 32);
                    if (pc == 'r') raw = 1; else if (pc == 'f') fmt = 1; else if (pc == 'b') by = 1; else if (pc == 'u') { } else ok = 0;
                }
                if (ok) { lex_string(L, raw, fmt, by); continue; }
            }
            tok_add(L, T_NAME);
            L->t[L->n - 1].s = intern(s, (uint32_t)n);
            continue;
        }
        if ((c >= '0' && c <= '9') || (c == '.' && L->p[1] >= '0' && L->p[1] <= '9')) {
            const char *s = L->p;
            int isf = 0;
            if (c == '0' && ((L->p[1] | 32) == 'x' || (L->p[1] | 32) == 'o' || (L->p[1] | 32) == 'b')) {
                L->p += 2;
                while (hexval((unsigned char)*L->p) >= 0 || *L->p == '_') L->p++;
            } else {
                while ((*L->p >= '0' && *L->p <= '9') || *L->p == '_') L->p++;
                if (*L->p == '.') { isf = 1; L->p++; while ((*L->p >= '0' && *L->p <= '9') || *L->p == '_') L->p++; }
                if ((*L->p | 32) == 'e' && (L->p[1] == '+' || L->p[1] == '-' || (L->p[1] >= '0' && L->p[1] <= '9'))) {
                    isf = 1; L->p += 2;
                    while (*L->p >= '0' && *L->p <= '9') L->p++;
                }
            }
            if ((*L->p | 32) == 'j') syntax(L->file, L->line, "complex numbers aren't supported");
            char buf[400]; int n = 0;
            for (const char *q = s; q < L->p && n < 399; q++) if (*q != '_') buf[n++] = *q;
            buf[n] = 0;
            tok_add(L, T_NUM);
            Tok *t = &L->t[L->n - 1];
            if (isf) t->v = vfloat(num_parse(buf, 0));
            else {
                int ok;
                if (n > 1 && buf[0] == '0' && buf[1] >= '0' && buf[1] <= '9') {
                    for (int i = 0; i < n; i++) if (buf[i] != '0') syntax(L->file, L->line, "leading zeros in decimal integer literals are not permitted");
                }
                t->v = int_parse(buf, 0, &ok);
                if (!ok) syntax(L->file, L->line, "invalid number");
            }
            if (idstart((unsigned char)*L->p)) syntax(L->file, L->line, "invalid decimal literal");
            continue;
        }
        if (c == '\'' || c == '"') { lex_string(L, 0, 0, 0); continue; }
        int matched = 0;
        for (int i = 0; OPS[i]; i++) {
            size_t ol = strlen(OPS[i]);
            if (!strncmp(L->p, OPS[i], ol)) {
                tok_add(L, T_OP);
                L->t[L->n - 1].op = OPS[i];
                L->p += ol;
                if (c == '(' || c == '[' || c == '{') depth++;
                else if ((c == ')' || c == ']' || c == '}') && depth > 0) depth--;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            char m[64];
            if (c == '!') snprintf(m, sizeof m, "invalid syntax ('!' on its own: did you mean 'not'?)");
            else snprintf(m, sizeof m, "invalid character '%c'", c);
            syntax(L->file, L->line, m);
        }
    }
    if (L->n && L->t[L->n - 1].k != T_NL) tok_add(L, T_NL);
    while (ni > 1) { ni--; tok_add(L, T_DEDENT); }
    tok_add(L, T_EOF);
}

/* ---- the parser ------------------------------------------------------------------------------------- */
typedef struct Coll {                     /* names a function, lambda, class or comprehension sets */
    Str **names; int n, cap;
    Str **glob; int ng;
    Str **nonl; int nn;
    int kind;                             /* 0 module, 1 function, 2 class, 3 comprehension       */
    struct Coll *up;
} Coll;

typedef struct {
    Tok *t; int i, n;
    Str *file;
    Coll *coll;
    int in_func, in_loop;
} Ps;

static Tok *cur(Ps *p) { return &p->t[p->i]; }
static Tok *peek(Ps *p, int k) { return &p->t[p->i + k < p->n ? p->i + k : p->n - 1]; }
static void adv(Ps *p) { if (p->i < p->n - 1) p->i++; }
static int is_op(Ps *p, const char *o) { return cur(p)->k == T_OP && !strcmp(cur(p)->op, o); }
static int is_kw(Ps *p, const char *k) { return cur(p)->k == T_NAME && !strcmp(cur(p)->s->s, k); }
static int accept(Ps *p, const char *o) { if (is_op(p, o)) { adv(p); return 1; } return 0; }
static int accept_kw(Ps *p, const char *k) { if (is_kw(p, k)) { adv(p); return 1; } return 0; }

static void perr(Ps *p, const char *msg) {
    char b[200];
    Tok *t = cur(p);
    if (msg) snprintf(b, sizeof b, "%s", msg);
    else if (t->k == T_EOF) snprintf(b, sizeof b, "unexpected end of the file");
    else if (t->k == T_INDENT) snprintf(b, sizeof b, "unexpected indent");
    else if (t->k == T_DEDENT || t->k == T_NL) snprintf(b, sizeof b, "invalid syntax (the line ends too soon)");
    else if (t->k == T_OP) snprintf(b, sizeof b, "invalid syntax at '%s'", t->op);
    else if (t->k == T_NAME) snprintf(b, sizeof b, "invalid syntax at '%s'", t->s->s);
    else snprintf(b, sizeof b, "invalid syntax");
    syntax(p->file, t->line, b);
}
static void expect(Ps *p, const char *o) {
    if (!accept(p, o)) {
        char b[80];
        snprintf(b, sizeof b, "expected '%s'", o);
        perr(p, b);
    }
}

static const char *const KW[] = { "False", "None", "True", "and", "as", "assert", "break", "class", "continue", "def",
    "del", "elif", "else", "except", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda",
    "nonlocal", "not", "or", "pass", "raise", "return", "try", "while", "with", "yield", 0 };
static int is_keyword(Str *s) { for (int i = 0; KW[i]; i++) if (!strcmp(s->s, KW[i])) return 1; return 0; }

static Str *name(Ps *p) {
    if (cur(p)->k != T_NAME || is_keyword(cur(p)->s)) perr(p, cur(p)->k == T_NAME ? 0 : "expected a name");
    Str *s = cur(p)->s;
    adv(p);
    return s;
}

static Node *nn(Ps *p, int k) { Node *n = rt_alloc(sizeof(Node)); n->k = (uint8_t)k; n->line = cur(p)->line; return n; }
static void kid(Node *n, Node *c) {
    if ((n->nk & (n->nk - 1)) == 0) {
        int cap = n->nk ? n->nk * 2 : 2;
        n->kids = rt_realloc(n->kids, sizeof(Node *) * (size_t)n->nk, sizeof(Node *) * (size_t)cap);
    }
    n->kids[n->nk++] = c;
}

static void coll_add(Coll *c, Str *s) {
    for (int i = 0; i < c->n; i++) if (c->names[i] == s) return;
    if (c->n == c->cap) { int nc = c->cap ? c->cap * 2 : 8; c->names = rt_realloc(c->names, sizeof(Str *) * (size_t)c->cap, sizeof(Str *) * (size_t)nc); c->cap = nc; }
    c->names[c->n++] = s;
}

/* A target of an assignment: its names become the current scope's. */
static void bind(Ps *p, Node *t) {
    switch (t->k) {
    case N_NAME: coll_add(p->coll, t->s); t->flags |= 1; break;
    case N_TUPLE: case N_LIST: for (int i = 0; i < t->nk; i++) bind(p, t->kids[i]); break;
    case N_STAR: bind(p, t->a); break;
    case N_ATTR: case N_INDEX: break;
    default: syntax(p->file, t->line, t->k == N_CALL ? "cannot assign to a function call" : t->k == N_CONST ? "cannot assign to a literal" : "cannot assign to an expression");
    }
}

static Node *expr(Ps *p);
static Node *test_nocond(Ps *p);
static Node *or_expr(Ps *p);
static Node *star_expr(Ps *p);
static Node *target(Ps *p);
static Node *block(Ps *p);
static Node *statement(Ps *p);
static Node *fstring(Ps *p, Str *s, int line);

/* A parameter list, for def (in parentheses) or lambda (up to ':'). */
static void params(Ps *p, Node *def, const char *end, int lambda) {
    Str *pn[64]; Node *dflt[64], *kwd[64]; int npn = 0, nd = 0, star = 0, dstar = 0, kwonly = 0, after_star = 0;
    while (!is_op(p, end)) {
        if (accept(p, "**")) { pn[npn++] = name(p); dstar = 1; if (!lambda && accept(p, ":")) test_nocond(p); accept(p, ","); break; }
        if (accept(p, "*")) {
            if (is_op(p, ",")) { after_star = 1; adv(p); continue; }
            pn[npn++] = name(p); star = 1; after_star = 1;
            if (!lambda && accept(p, ":")) test_nocond(p);
            if (!accept(p, ",")) break;
            continue;
        }
        if (accept(p, "/")) { accept(p, ","); continue; }
        if (npn >= 60) perr(p, "too many parameters");
        Str *s = name(p);
        if (!lambda && accept(p, ":")) test_nocond(p);  /* annotations: read, not used */
        Node *d = 0;
        if (accept(p, "=")) d = test_nocond(p);
        if (after_star) { kwd[kwonly++] = d; pn[npn++] = s; }
        else {
            if (!d && nd) syntax(p->file, cur(p)->line, "a parameter without a default follows one with a default");
            if (d) dflt[nd++] = d;
            pn[npn++] = s;
        }
        if (!accept(p, ",")) break;
    }
    /* order in the frame: positional, keyword-only, *args, **kwargs */
    int npos = npn - kwonly - star - dstar;
    Str **all = rt_alloc(sizeof(Str *) * (size_t)(npn ? npn : 1));
    int k = 0, kwi = 0, posi = 0;
    Str *starname = 0, *dstarname = 0;
    /* pn holds them in source order: split back out */
    Str *kwn[64];
    {
        int seen_star = 0;
        for (int i = 0; i < npn; i++) {
            if (dstar && i == npn - 1) { dstarname = pn[i]; continue; }
            if (star && !starname && i == npos) { starname = pn[i]; seen_star = 1; continue; }
            (void)seen_star;
            if (posi < npos) all[posi++] = pn[i];
            else kwn[kwi++] = pn[i];
        }
    }
    k = posi;
    for (int i = 0; i < kwi; i++) all[k++] = kwn[i];
    if (starname) all[k++] = starname;
    if (dstarname) all[k++] = dstarname;
    for (int i = 0; i < k; i++) {
        for (int j = 0; j < i; j++) if (all[j] == all[i]) syntax(p->file, def->line, "duplicate argument in function definition");
        coll_add(p->coll, all[i]);
    }
    def->params = all; def->nparams = k; def->npos = npos; def->star = star; def->dstar = dstar; def->nkwonly = kwonly;
    def->defaults = rt_alloc(sizeof(Node *) * (size_t)(nd ? nd : 1)); memcpy(def->defaults, dflt, sizeof(Node *) * (size_t)nd); def->ndef = nd;
    def->kwdefs = rt_alloc(sizeof(Node *) * (size_t)(kwonly ? kwonly : 1)); memcpy(def->kwdefs, kwd, sizeof(Node *) * (size_t)kwonly);
}

static Coll *push_coll(Ps *p, int kind) {
    Coll *c = rt_alloc(sizeof(Coll));
    c->kind = kind; c->up = p->coll; p->coll = c;
    return c;
}
static void pop_coll(Ps *p, Node *owner) {
    Coll *c = p->coll;
    owner->slotnames = c->names; owner->nslots = c->n;
    /* the resolver needs the declarations: keep them on the node via its `c` link */
    Node *decl = rt_alloc(sizeof(Node));
    decl->k = N_GLOBAL;
    decl->params = c->glob; decl->nparams = c->ng;
    decl->kwdefs = (Node **)c->nonl; decl->nkwonly = c->nn;
    decl->depth = c->kind;
    owner->c = decl;
    p->coll = c->up;
}

static Node *lambda(Ps *p) {
    Node *n = nn(p, N_LAMBDA);
    push_coll(p, 1);
    params(p, n, ":", 1);
    expect(p, ":");
    int f = p->in_func; p->in_func = 1;
    n->a = expr(p);
    p->in_func = f;
    n->s = intern_c("<lambda>");
    n->file = p->file;
    pop_coll(p, n);
    return n;
}

/* [x for x in y if z] and the others; `first` is the element already read. */
static Node *comprehension(Ps *p, int kind, Node *first, Node *val) {
    Node *n = nn(p, N_COMP);
    n->op = (uint8_t)kind;
    n->a = first; n->b = val;
    n->line = first->line;
    push_coll(p, 3);
    int nclause = 0;
    while (is_kw(p, "for")) {
        adv(p);
        Node *cl = nn(p, N_FOR);
        /* the target list */
        Node *t = target(p);
        if (is_op(p, ",")) {
            Node *tu = nn(p, N_TUPLE); kid(tu, t);
            while (accept(p, ",") && !is_kw(p, "in")) kid(tu, target(p));
            t = tu;
        }
        bind(p, t);
        cl->a = t;
        if (!accept_kw(p, "in")) perr(p, "expected 'in'");
        /* the first iterable is the outer scope's: the resolver knows it by flags */
        cl->b = or_expr(p);
        if (!nclause) cl->flags = 1;
        while (is_kw(p, "if")) { adv(p); kid(cl, test_nocond(p)); }
        kid(n, cl);
        nclause++;
    }
    n->file = p->file;
    pop_coll(p, n);
    return n;
}

static Node *atom(Ps *p) {
    Tok *t = cur(p);
    Node *n;
    switch (t->k) {
    case T_NUM: n = nn(p, N_CONST); n->v = t->v; adv(p); return n;
    case T_STR: case T_FSTR: {
        /* adjacent literals join; f-strings among them make it an f-string */
        Node *fs = 0; SB sb; sb_new(&sb, 32); int any_f = 0;
        int line = t->line;
        while (cur(p)->k == T_STR || cur(p)->k == T_FSTR) {
            if (cur(p)->k == T_FSTR) {
                if (!fs) fs = nn(p, N_FSTR);
                if (sb.n) { Node *c = nn(p, N_CONST); c->v = vobj(sb_str(&sb)); kid(fs, c); sb.n = 0; }
                Node *part = fstring(p, cur(p)->s, cur(p)->line);
                for (int i = 0; i < part->nk; i++) kid(fs, part->kids[i]);
                any_f = 1;
            } else if (any_f) {
                Node *c = nn(p, N_CONST); c->v = vobj(cur(p)->s); kid(fs, c);
            } else sb_addstr(&sb, cur(p)->s);
            adv(p);
        }
        if (any_f) { if (sb.n) { Node *c = nn(p, N_CONST); c->v = vobj(sb_str(&sb)); kid(fs, c); } fs->line = line; return fs; }
        n = nn(p, N_CONST); n->v = vobj(sb_str(&sb)); n->line = line;
        return n;
    }
    case T_NAME:
        if (!strcmp(t->s->s, "None")) { adv(p); n = nn(p, N_CONST); n->v = vnone(); return n; }
        if (!strcmp(t->s->s, "True")) { adv(p); n = nn(p, N_CONST); n->v = vbool(1); return n; }
        if (!strcmp(t->s->s, "False")) { adv(p); n = nn(p, N_CONST); n->v = vbool(0); return n; }
        if (!strcmp(t->s->s, "yield")) syntax(p->file, t->line, "generators (yield) aren't supported yet: build a list and return it");
        if (!strcmp(t->s->s, "await") || !strcmp(t->s->s, "async")) syntax(p->file, t->line, "async code isn't supported");
        n = nn(p, N_NAME); n->s = name(p); return n;
    case T_OP: break;
    default: perr(p, 0);
    }
    if (accept(p, "...")) { n = nn(p, N_CONST); n->v = vnone(); return n; }
    if (accept(p, "(")) {
        if (accept(p, ")")) return nn(p, N_TUPLE);
        Node *first = star_expr(p);
        if (is_kw(p, "for")) { Node *c = comprehension(p, 'g', first, 0); expect(p, ")"); return c; }
        if (is_op(p, ":=") && first->k == N_NAME) {
            adv(p);
            Node *w = nn(p, N_WALRUS); w->a = first; w->b = expr(p); bind(p, first);
            first = w;
        }
        if (accept(p, ")")) { first->flags |= 2; return first; }        /* parenthesized */
        Node *tu = nn(p, N_TUPLE); kid(tu, first);
        while (accept(p, ",")) { if (is_op(p, ")")) break; kid(tu, star_expr(p)); }
        expect(p, ")");
        return tu;
    }
    if (accept(p, "[")) {
        n = nn(p, N_LIST);
        if (accept(p, "]")) return n;
        Node *first = star_expr(p);
        if (is_kw(p, "for")) { Node *c = comprehension(p, 'l', first, 0); expect(p, "]"); return c; }
        kid(n, first);
        while (accept(p, ",")) { if (is_op(p, "]")) break; kid(n, star_expr(p)); }
        expect(p, "]");
        return n;
    }
    if (accept(p, "{")) {
        if (accept(p, "}")) return nn(p, N_DICT);
        if (accept(p, "**")) {
            n = nn(p, N_DICT);
            Node *d = nn(p, N_DSTAR); d->a = or_expr(p); kid(n, d);
            goto more_dict;
        }
        Node *first = star_expr(p);
        if (accept(p, ":")) {
            Node *val = expr(p);
            if (is_kw(p, "for")) { Node *c = comprehension(p, 'd', first, val); expect(p, "}"); return c; }
            n = nn(p, N_DICT);
            Node *it = nn(p, N_ITEM); it->a = first; it->b = val; kid(n, it);
        more_dict:
            while (accept(p, ",")) {
                if (is_op(p, "}")) break;
                if (accept(p, "**")) { Node *d = nn(p, N_DSTAR); d->a = or_expr(p); kid(n, d); continue; }
                Node *k = expr(p); expect(p, ":");
                Node *it2 = nn(p, N_ITEM); it2->a = k; it2->b = expr(p); kid(n, it2);
            }
            expect(p, "}");
            return n;
        }
        if (is_kw(p, "for")) { Node *c = comprehension(p, 's', first, 0); expect(p, "}"); return c; }
        n = nn(p, N_SET); kid(n, first);
        while (accept(p, ",")) { if (is_op(p, "}")) break; kid(n, star_expr(p)); }
        expect(p, "}");
        return n;
    }
    perr(p, 0);
    return 0;
}

static Node *call_args(Ps *p, Node *fn) {
    Node *n = nn(p, N_CALL);
    n->a = fn; n->line = fn->line;
    while (!is_op(p, ")")) {
        if (accept(p, "**")) { Node *d = nn(p, N_DSTAR); d->a = expr(p); kid(n, d); }
        else if (accept(p, "*")) { Node *s = nn(p, N_STAR); s->a = expr(p); kid(n, s); }
        else if (cur(p)->k == T_NAME && peek(p, 1)->k == T_OP && !strcmp(peek(p, 1)->op, "=")) {
            Node *k = nn(p, N_KWARG); k->s = name(p); adv(p); k->a = expr(p); kid(n, k);
        } else {
            Node *a = expr(p);
            if (is_kw(p, "for")) a = comprehension(p, 'g', a, 0);
            else if (is_op(p, ":=") && a->k == N_NAME) { adv(p); Node *w = nn(p, N_WALRUS); w->a = a; w->b = expr(p); bind(p, a); a = w; }
            kid(n, a);
        }
        if (!accept(p, ",")) break;
    }
    expect(p, ")");
    return n;
}

static Node *subscript(Ps *p) {
    /* a, a:b, a:b:c, and tuples of them */
    Node *parts[16]; int np = 0;
    do {
        if (is_op(p, "]")) break;
        Node *lo = 0, *hi = 0, *st = 0;
        if (!is_op(p, ":")) lo = expr(p);
        if (accept(p, ":")) {
            Node *s = nn(p, N_SLICE);
            if (!is_op(p, "]") && !is_op(p, ":") && !is_op(p, ",")) hi = expr(p);
            if (accept(p, ":") && !is_op(p, "]") && !is_op(p, ",")) st = expr(p);
            s->a = lo; s->b = hi; s->c = st;
            lo = s;
        }
        if (np < 16) parts[np++] = lo;
    } while (accept(p, ","));
    expect(p, "]");
    if (np == 1) return parts[0];
    Node *tu = nn(p, N_TUPLE);
    for (int i = 0; i < np; i++) kid(tu, parts[i]);
    return tu;
}

static Node *primary(Ps *p) {
    Node *n = atom(p);
    for (;;) {
        if (accept(p, "(")) n = call_args(p, n);
        else if (accept(p, "[")) { Node *x = nn(p, N_INDEX); x->a = n; x->b = subscript(p); x->line = n->line; n = x; }
        else if (accept(p, ".")) { Node *x = nn(p, N_ATTR); x->a = n; x->s = name(p); n = x; }
        else return n;
    }
}

static Node *unary(Ps *p);
static Node *power(Ps *p) {
    Node *n = primary(p);
    if (accept(p, "**")) { Node *x = nn(p, N_BIN); x->op = B_POW; x->a = n; x->b = unary(p); return x; }
    return n;
}
static Node *unary(Ps *p) {
    int op = is_op(p, "-") ? U_NEG : is_op(p, "+") ? U_POS : is_op(p, "~") ? U_INV : -1;
    if (op < 0) return power(p);
    adv(p);
    Node *a = unary(p);
    if (op == U_NEG && a->k == N_CONST && (a->v.t == V_INT || a->v.t == V_FLOAT || is_obj(a->v, O_BIG))) {
        a->v = a->v.t == V_FLOAT ? vfloat(-a->v.u.f) : int_neg(a->v);
        return a;
    }
    Node *x = nn(p, N_UNARY); x->op = (uint8_t)op; x->a = a;
    return x;
}

typedef struct { const char *op; int code; } binop_t;
static Node *binary(Ps *p, int level);
static const binop_t L_TERM[] = { { "*", B_MUL }, { "/", B_DIV }, { "//", B_FLOORDIV }, { "%", B_MOD }, { "@", B_MATMUL }, { 0, 0 } };
static const binop_t L_ARITH[] = { { "+", B_ADD }, { "-", B_SUB }, { 0, 0 } };
static const binop_t L_SHIFT[] = { { "<<", B_LSHIFT }, { ">>", B_RSHIFT }, { 0, 0 } };
static const binop_t L_BAND[] = { { "&", B_AND }, { 0, 0 } };
static const binop_t L_BXOR[] = { { "^", B_XOR }, { 0, 0 } };
static const binop_t L_BOR[] = { { "|", B_OR }, { 0, 0 } };
static const binop_t *const LEVELS[] = { L_BOR, L_BXOR, L_BAND, L_SHIFT, L_ARITH, L_TERM };

static Node *binary(Ps *p, int level) {
    if (level == 6) return unary(p);
    Node *n = binary(p, level + 1);
    for (;;) {
        const binop_t *hit = 0;
        for (const binop_t *b = LEVELS[level]; b->op; b++) if (is_op(p, b->op)) { hit = b; break; }
        if (!hit) return n;
        int line = cur(p)->line;
        adv(p);
        Node *x = nn(p, N_BIN); x->op = (uint8_t)hit->code; x->a = n; x->b = binary(p, level + 1); x->line = line;
        n = x;
    }
}
static Node *or_expr(Ps *p) { return binary(p, 0); }

static Node *comparison(Ps *p) {
    Node *first = or_expr(p);
    Node *n = 0;
    uint8_t ops[32]; int no = 0;
    for (;;) {
        int op = -1;
        if (is_op(p, "<")) op = C_LT; else if (is_op(p, "<=")) op = C_LE; else if (is_op(p, ">")) op = C_GT;
        else if (is_op(p, ">=")) op = C_GE; else if (is_op(p, "==")) op = C_EQ; else if (is_op(p, "!=")) op = C_NE;
        else if (is_kw(p, "in")) op = C_IN;
        else if (is_kw(p, "not") && peek(p, 1)->k == T_NAME && !strcmp(peek(p, 1)->s->s, "in")) { adv(p); op = C_NOTIN; }
        else if (is_kw(p, "is")) { if (peek(p, 1)->k == T_NAME && !strcmp(peek(p, 1)->s->s, "not")) { adv(p); op = C_ISNOT; } else op = C_IS; }
        if (op < 0) break;
        adv(p);
        if (!n) { n = nn(p, N_CMP); n->a = first; n->line = first->line; }
        if (no < 32) ops[no++] = (uint8_t)op;
        kid(n, or_expr(p));
    }
    if (!n) return first;
    n->ops = rt_alloc_leaf((size_t)no);
    memcpy(n->ops, ops, (size_t)no);
    return n;
}
static Node *not_test(Ps *p) {
    if (accept_kw(p, "not")) { Node *x = nn(p, N_NOT); x->a = not_test(p); return x; }
    return comparison(p);
}
static Node *and_test(Ps *p) {
    Node *n = not_test(p);
    while (accept_kw(p, "and")) { Node *x = nn(p, N_AND); x->a = n; x->b = not_test(p); n = x; }
    return n;
}
static Node *or_test(Ps *p) {
    Node *n = and_test(p);
    while (accept_kw(p, "or")) { Node *x = nn(p, N_OR); x->a = n; x->b = and_test(p); n = x; }
    return n;
}
static Node *test_nocond(Ps *p) {
    if (is_kw(p, "lambda")) { adv(p); return lambda(p); }
    return or_test(p);
}
static Node *expr(Ps *p) {
    if (is_kw(p, "lambda")) { adv(p); return lambda(p); }
    Node *n = or_test(p);
    if (is_kw(p, "if") ) {
        adv(p);
        Node *x = nn(p, N_IFEXP); x->b = n; x->a = or_test(p);
        if (!accept_kw(p, "else")) perr(p, "expected 'else' after 'if' in an expression");
        x->c = expr(p);
        return x;
    }
    return n;
}
/* if/while conditions may assign: `while (line := f.readline()):` or without the parentheses */
static Node *cond(Ps *p) {
    Node *n = expr(p);
    if (is_op(p, ":=") && n->k == N_NAME) {
        adv(p);
        Node *w = nn(p, N_WALRUS); w->a = n; w->b = expr(p); bind(p, n);
        return w;
    }
    return n;
}

static Node *star_expr(Ps *p) {
    if (accept(p, "*")) { Node *s = nn(p, N_STAR); s->a = or_expr(p); return s; }
    return expr(p);
}
/* a for loop's target: no comparisons, so `in` ends it */
static Node *target(Ps *p) {
    if (accept(p, "*")) { Node *s = nn(p, N_STAR); s->a = or_expr(p); return s; }
    return or_expr(p);
}
/* a, b, *c -- a tuple when there's a comma */
static Node *expr_list(Ps *p) {
    Node *first = star_expr(p);
    if (!is_op(p, ",")) return first;
    Node *tu = nn(p, N_TUPLE); tu->line = first->line; kid(tu, first);
    while (accept(p, ",")) {
        if (cur(p)->k == T_NL || is_op(p, "=") || is_op(p, ")") || is_op(p, ";") || is_op(p, ":") || cur(p)->k == T_EOF) break;
        kid(tu, star_expr(p));
    }
    return tu;
}

/* f"...{expr!r:spec}...": the expression parts read with their own lexer */
static Node *fstring(Ps *p, Str *s, int line) {
    Node *n = nn(p, N_FSTR); n->line = line;
    SB lit; sb_new(&lit, 32);
    for (uint32_t i = 0; i < s->len; ) {
        char c = s->s[i];
        if (c == '{' && i + 1 < s->len && s->s[i + 1] == '{') { sb_addc(&lit, '{'); i += 2; continue; }
        if (c == '}' && i + 1 < s->len && s->s[i + 1] == '}') { sb_addc(&lit, '}'); i += 2; continue; }
        if (c == '}') syntax(p->file, line, "f-string: single '}' is not allowed");
        if (c != '{') { sb_addc(&lit, c); i++; continue; }
        if (lit.n) { Node *k = nn(p, N_CONST); k->v = vobj(sb_str(&lit)); kid(n, k); lit.n = 0; }
        /* find the expression's end: a top-level '!', ':' or '}' */
        uint32_t j = i + 1; int depth = 0; char inq = 0;
        while (j < s->len) {
            char d = s->s[j];
            if (inq) { if (d == inq) inq = 0; j++; continue; }
            if (d == '\'' || d == '"') inq = d;
            else if (d == '(' || d == '[' || d == '{') depth++;
            else if (d == ')' || d == ']' || (d == '}' && depth)) depth--;
            else if (!depth && (d == '}' || d == ':' || (d == '!' && j + 1 < s->len && s->s[j + 1] != '='))) break;
            j++;
        }
        if (j >= s->len) syntax(p->file, line, "f-string: expecting '}'");
        uint32_t ee = j;
        int self_doc = 0;
        while (ee > i + 1 && s->s[ee - 1] == ' ') ee--;
        if (ee > i + 1 && s->s[ee - 1] == '=' && (ee < i + 3 || (s->s[ee - 2] != '=' && s->s[ee - 2] != '!' && s->s[ee - 2] != '<' && s->s[ee - 2] != '>'))) { self_doc = 1; ee--; }
        Str *src = str_new(s->s + i + 1, ee - i - 1);
        if (!src->len) syntax(p->file, line, "f-string: empty expression not allowed");
        Lex L; memset(&L, 0, sizeof L);
        L.src = L.p = src->s; L.file = p->file;
        lex(&L);
        Ps sub = *p;
        sub.t = L.t; sub.n = L.n; sub.i = 0;
        for (int k = 0; k < L.n; k++) L.t[k].line = line;
        Node *e = expr_list(&sub);
        while (sub.t[sub.i].k == T_NL) sub.i++;
        if (sub.t[sub.i].k != T_EOF) syntax(p->file, line, "f-string: invalid expression");
        if (self_doc) { Node *k = nn(p, N_CONST); k->v = vobj(str_new(s->s + i + 1, j - i - 1)); kid(n, k); }
        Node *f = nn(p, N_FMT); f->a = e; f->line = line;
        if (self_doc) f->op = 'r';
        if (s->s[j] == '!') {
            f->op = (uint8_t)s->s[j + 1];
            if (f->op != 'r' && f->op != 's' && f->op != 'a') syntax(p->file, line, "f-string: invalid conversion character");
            j += 2;
        }
        if (s->s[j] == ':') {
            uint32_t k = j + 1; int d2 = 0;
            while (k < s->len && (s->s[k] != '}' || d2)) { if (s->s[k] == '{') d2++; else if (s->s[k] == '}') d2--; k++; }
            f->b = fstring(p, str_new(s->s + j + 1, k - j - 1), line);
            j = k;
        }
        if (j >= s->len || s->s[j] != '}') syntax(p->file, line, "f-string: expecting '}'");
        kid(n, f);
        i = j + 1;
    }
    if (lit.n) { Node *k = nn(p, N_CONST); k->v = vobj(sb_str(&lit)); kid(n, k); }
    return n;
}

/* ---- statements -------------------------------------------------------------------------------------- */
static void end_simple(Ps *p) {
    if (cur(p)->k == T_NL) { adv(p); return; }
    if (cur(p)->k == T_EOF || cur(p)->k == T_DEDENT) return;
    perr(p, 0);
}

static Node *dotted(Ps *p, Str **bound) {
    SB sb; sb_new(&sb, 32);
    Str *first = name(p);
    sb_addstr(&sb, first);
    while (accept(p, ".")) { sb_addc(&sb, '.'); sb_addstr(&sb, name(p)); }
    if (bound) *bound = first;
    Node *n = nn(p, N_CONST);
    n->s = intern(sb.b, sb.n);
    return n;
}

static Node *simple(Ps *p) {
    Tok *t = cur(p);
    if (t->k == T_NAME) {
        const char *k = t->s->s;
        if (!strcmp(k, "pass")) { adv(p); return nn(p, N_PASS); }
        if (!strcmp(k, "break")) { if (!p->in_loop) perr(p, "'break' outside loop"); adv(p); return nn(p, N_BREAK); }
        if (!strcmp(k, "continue")) { if (!p->in_loop) perr(p, "'continue' not properly in loop"); adv(p); return nn(p, N_CONTINUE); }
        if (!strcmp(k, "return")) {
            if (!p->in_func) perr(p, "'return' outside function");
            Node *n = nn(p, N_RETURN); adv(p);
            if (cur(p)->k != T_NL && !is_op(p, ";") && cur(p)->k != T_EOF) n->a = expr_list(p);
            return n;
        }
        if (!strcmp(k, "raise")) {
            Node *n = nn(p, N_RAISE); adv(p);
            if (cur(p)->k != T_NL && !is_op(p, ";")) { n->a = expr(p); if (accept_kw(p, "from")) n->b = expr(p); }
            return n;
        }
        if (!strcmp(k, "global") || !strcmp(k, "nonlocal")) {
            int g = k[0] == 'g';
            Node *n = nn(p, g ? N_GLOBAL : N_NONLOCAL); adv(p);
            do {
                Str *s = name(p);
                Coll *c = p->coll;
                Str ***arr = g ? &c->glob : &c->nonl; int *cnt = g ? &c->ng : &c->nn;
                *arr = rt_realloc(*arr, sizeof(Str *) * (size_t)*cnt, sizeof(Str *) * (size_t)(*cnt + 1));
                (*arr)[(*cnt)++] = s;
                if (!g && c->kind != 1) perr(p, "nonlocal declaration not allowed at module level");
            } while (accept(p, ","));
            return n;
        }
        if (!strcmp(k, "import")) {
            Node *n = nn(p, N_IMPORT); adv(p);
            do {
                Node *it = nn(p, N_ITEM);
                Str *firstname;
                Node *d = dotted(p, &firstname);
                it->s = d->s;
                Node *target = nn(p, N_NAME);
                if (accept_kw(p, "as")) { target->s = name(p); it->flags = 1; }
                else target->s = firstname;
                bind(p, target);
                it->a = target;
                kid(n, it);
            } while (accept(p, ","));
            return n;
        }
        if (!strcmp(k, "from")) {
            Node *n = nn(p, N_FROM); adv(p);
            while (accept(p, ".")) { }
            n->s = dotted(p, 0)->s;
            if (!accept_kw(p, "import")) perr(p, "expected 'import'");
            if (accept(p, "*")) { n->flags = 1; return n; }
            int paren = accept(p, "(");
            do {
                if (paren && is_op(p, ")")) break;
                Node *it = nn(p, N_ITEM);
                it->s = name(p);
                Node *target = nn(p, N_NAME);
                target->s = accept_kw(p, "as") ? name(p) : it->s;
                bind(p, target);
                it->a = target;
                kid(n, it);
            } while (accept(p, ","));
            if (paren) expect(p, ")");
            return n;
        }
        if (!strcmp(k, "del")) {
            Node *n = nn(p, N_DEL); adv(p);
            do { Node *x = primary(p); if (x->k == N_NAME) bind(p, x); kid(n, x); } while (accept(p, ",") && cur(p)->k != T_NL);
            return n;
        }
        if (!strcmp(k, "assert")) {
            Node *n = nn(p, N_ASSERT); adv(p);
            n->a = expr(p);
            if (accept(p, ",")) n->b = expr(p);
            return n;
        }
    }
    /* an expression, an assignment, an augmented or annotated one */
    Node *e = expr_list(p);
    static const struct { const char *op; int code; } AUG[] = {
        { "+=", B_ADD }, { "-=", B_SUB }, { "*=", B_MUL }, { "/=", B_DIV }, { "//=", B_FLOORDIV }, { "%=", B_MOD },
        { "**=", B_POW }, { "<<=", B_LSHIFT }, { ">>=", B_RSHIFT }, { "&=", B_AND }, { "|=", B_OR }, { "^=", B_XOR }, { "@=", B_MATMUL }, { 0, 0 } };
    for (int i = 0; AUG[i].op; i++)
        if (is_op(p, AUG[i].op)) {
            adv(p);
            if (e->k != N_NAME && e->k != N_ATTR && e->k != N_INDEX) syntax(p->file, e->line, "illegal expression for augmented assignment");
            bind(p, e);
            Node *n = nn(p, N_AUG); n->op = (uint8_t)AUG[i].code; n->a = e; n->b = expr_list(p); n->line = e->line;
            return n;
        }
    if (is_op(p, ":") && (e->k == N_NAME || e->k == N_ATTR || e->k == N_INDEX)) {
        adv(p);
        expr(p);                                      /* the annotation */
        Node *note = 0;
        if (e->k == N_NAME && p->coll->kind == 2) { note = nn(p, N_ITEM); note->flags = 2; note->s = e->s; }
        if (!accept(p, "=")) { if (e->k == N_NAME && p->coll->kind != 2) bind(p, e); return note ? note : nn(p, N_PASS); }
        bind(p, e);
        Node *n = nn(p, N_ASSIGN); kid(n, e); n->a = expr_list(p); n->line = e->line;
        if (!note) return n;
        Node *b = nn(p, N_BLOCK); kid(b, note); kid(b, n);
        return b;
    }
    if (is_op(p, "=")) {
        Node *n = nn(p, N_ASSIGN); n->line = e->line;
        Node *rhs = e;
        while (accept(p, "=")) {
            bind(p, rhs);
            kid(n, rhs);
            rhs = expr_list(p);
        }
        n->a = rhs;
        return n;
    }
    Node *n = nn(p, N_EXPR); n->a = e; n->line = e->line;
    return n;
}

static Node *simple_line(Ps *p) {
    Node *first = simple(p);
    if (!is_op(p, ";")) { end_simple(p); return first; }
    Node *b = nn(p, N_BLOCK); kid(b, first);
    while (accept(p, ";")) { if (cur(p)->k == T_NL) break; kid(b, simple(p)); }
    end_simple(p);
    return b;
}

static Node *block(Ps *p) {
    expect(p, ":");
    Node *b = nn(p, N_BLOCK);
    if (cur(p)->k != T_NL) { kid(b, simple_line(p)); return b; }
    adv(p);
    if (cur(p)->k != T_INDENT) perr(p, "expected an indented block");
    adv(p);
    while (cur(p)->k != T_DEDENT && cur(p)->k != T_EOF) kid(b, statement(p));
    if (cur(p)->k == T_DEDENT) adv(p);
    return b;
}

static Node *loop_block(Ps *p) { int l = p->in_loop; p->in_loop = 1; Node *b = block(p); p->in_loop = l; return b; }

static Node *funcdef(Ps *p, Node *decorators) {
    Node *n = nn(p, N_DEF);
    adv(p);
    n->s = name(p);
    Node *target = nn(p, N_NAME); target->s = n->s; bind(p, target);
    n->b = target;
    n->d = decorators;
    expect(p, "(");
    push_coll(p, 1);
    params(p, n, ")", 0);
    expect(p, ")");
    if (accept(p, "->")) expr(p);
    int f = p->in_func, l = p->in_loop; p->in_func = 1; p->in_loop = 0;
    n->a = block(p);
    p->in_func = f; p->in_loop = l;
    n->file = p->file;
    pop_coll(p, n);
    return n;
}

static Node *classdef(Ps *p, Node *decorators) {
    Node *n = nn(p, N_CLASS);
    adv(p);
    n->s = name(p);
    Node *target = nn(p, N_NAME); target->s = n->s; bind(p, target);
    n->b = target;
    n->d = decorators;
    if (accept(p, "(")) {
        while (!is_op(p, ")")) {
            if (cur(p)->k == T_NAME && peek(p, 1)->k == T_OP && !strcmp(peek(p, 1)->op, "=")) { adv(p); adv(p); expr(p); }
            else kid(n, expr(p));
            if (!accept(p, ",")) break;
        }
        expect(p, ")");
    }
    push_coll(p, 2);
    int f = p->in_func, l = p->in_loop; p->in_func = 0; p->in_loop = 0;
    n->a = block(p);
    p->in_func = f; p->in_loop = l;
    pop_coll(p, n);
    return n;
}

static Node *statement(Ps *p) {
    Tok *t = cur(p);
    if (t->k == T_INDENT) perr(p, "unexpected indent");
    if (t->k == T_OP && !strcmp(t->op, "@")) {
        Node *decs = nn(p, N_LIST);
        while (accept(p, "@")) { kid(decs, expr(p)); if (cur(p)->k != T_NL) perr(p, 0); adv(p); }
        if (is_kw(p, "def")) return funcdef(p, decs);
        if (is_kw(p, "class")) return classdef(p, decs);
        perr(p, "a decorator goes before def or class");
    }
    if (t->k != T_NAME) return simple_line(p);
    const char *k = t->s->s;
    if (!strcmp(k, "if") || !strcmp(k, "elif")) {
        Node *n = nn(p, N_IF); adv(p);
        n->a = cond(p);
        n->b = block(p);
        if (is_kw(p, "elif")) n->c = statement(p);
        else if (accept_kw(p, "else")) n->c = block(p);
        return n;
    }
    if (!strcmp(k, "while")) {
        Node *n = nn(p, N_WHILE); adv(p);
        n->a = cond(p); n->b = loop_block(p);
        if (accept_kw(p, "else")) n->c = block(p);
        return n;
    }
    if (!strcmp(k, "for")) {
        Node *n = nn(p, N_FOR); adv(p);
        Node *tg = target(p);
        if (is_op(p, ",")) { Node *tu = nn(p, N_TUPLE); kid(tu, tg); while (accept(p, ",") && !is_kw(p, "in")) kid(tu, target(p)); tg = tu; }
        bind(p, tg);
        n->a = tg;
        if (!accept_kw(p, "in")) perr(p, "expected 'in'");
        n->b = expr_list(p);
        n->c = loop_block(p);
        if (accept_kw(p, "else")) n->d = block(p);
        return n;
    }
    if (!strcmp(k, "def")) return funcdef(p, 0);
    if (!strcmp(k, "class")) return classdef(p, 0);
    if (!strcmp(k, "try")) {
        Node *n = nn(p, N_TRY); adv(p);
        n->a = block(p);
        while (is_kw(p, "except")) {
            Node *h = nn(p, N_EXCEPT); adv(p);
            if (!is_op(p, ":")) {
                h->a = expr(p);
                if (accept(p, ",")) { Node *tu = nn(p, N_TUPLE); kid(tu, h->a); do kid(tu, expr(p)); while (accept(p, ",")); h->a = tu; }
                if (accept_kw(p, "as")) { Node *tg = nn(p, N_NAME); tg->s = name(p); bind(p, tg); h->c = tg; }
            }
            h->b = block(p);
            kid(n, h);
        }
        if (accept_kw(p, "else")) n->c = block(p);
        if (accept_kw(p, "finally")) n->d = block(p);
        if (!n->nk && !n->d) perr(p, "expected 'except' or 'finally' block");
        return n;
    }
    if (!strcmp(k, "with")) {
        Node *n = nn(p, N_WITH); adv(p);
        do {
            Node *it = nn(p, N_ITEM);
            it->a = expr(p);
            if (accept_kw(p, "as")) { it->b = star_expr(p); bind(p, it->b); }
            kid(n, it);
        } while (accept(p, ","));
        n->a = block(p);
        return n;
    }
    if (!strcmp(k, "match")) {
        /* a soft keyword: only a statement when it looks like one */
        int j = 1, depth = 0;
        while (peek(p, j)->k != T_NL && peek(p, j)->k != T_EOF) {
            Tok *q = peek(p, j);
            if (q->k == T_OP && (!strcmp(q->op, "(") || !strcmp(q->op, "["))) depth++;
            if (q->k == T_OP && (!strcmp(q->op, ")") || !strcmp(q->op, "]"))) depth--;
            j++;
        }
        Tok *last = peek(p, j - 1);
        if (j > 1 && last->k == T_OP && !strcmp(last->op, ":") && !depth && peek(p, 1)->k != T_OP)
            syntax(p->file, t->line, "match statements aren't supported yet: use if/elif");
    }
    return simple_line(p);
}

/* ---- where names live -------------------------------------------------------------------------------- */
typedef struct RScope {
    Node *owner;                          /* def, lambda, comprehension or class; NULL: the module */
    int kind;                             /* 0 module, 1 function, 2 class, 3 comprehension      */
    struct RScope *up;
} RScope;

static int has_name(Str **arr, int n, Str *s) { for (int i = 0; i < n; i++) if (arr[i] == s) return 1; return 0; }
static int slot_of(Node *owner, Str *s) { for (int i = 0; i < owner->nslots; i++) if (owner->slotnames[i] == s) return i; return -1; }

static void resolve_name(Node *n, RScope *sc, Str *file) {
    Str *s = n->s;
    if (!sc || sc->kind == 0) { n->scope = S_GLOBAL; return; }
    Node *decl = sc->owner->c;
    if (has_name(decl->params, decl->nparams, s)) { n->scope = S_GLOBAL; return; }
    int nonlocal = has_name((Str **)decl->kwdefs, decl->nkwonly, s);
    if (sc->kind == 2) {
        if (slot_of(sc->owner, s) >= 0 && !nonlocal) { n->scope = S_CLASS; return; }
    } else if (!nonlocal) {
        int sl = slot_of(sc->owner, s);
        if (sl >= 0) { n->scope = S_LOCAL; n->slot = sl; return; }
    }
    /* outwards, past class bodies, counting the frames */
    int depth = 1;
    for (RScope *u = sc->up; u; u = u->up) {
        if (u->kind == 0) break;
        if (u->kind == 2) continue;
        Node *ud = u->owner->c;
        if (has_name(ud->params, ud->nparams, s)) break;
        int sl = slot_of(u->owner, s);
        if (sl >= 0) { n->scope = S_OUTER; n->slot = sl; n->depth = depth; return; }
        depth++;
    }
    if (nonlocal) syntax(file, n->line, "no binding for nonlocal name found");
    n->scope = S_GLOBAL;
}

static void resolve(Node *n, RScope *sc, Str *file);
static void resolve_kids(Node *n, RScope *sc, Str *file) { for (int i = 0; i < n->nk; i++) resolve(n->kids[i], sc, file); }

static void resolve(Node *n, RScope *sc, Str *file) {
    if (!n) return;
    switch (n->k) {
    case N_NAME: resolve_name(n, sc, file); return;
    case N_CONST: case N_PASS: case N_BREAK: case N_CONTINUE: case N_GLOBAL: case N_NONLOCAL: return;
    case N_DEF: case N_LAMBDA: {
        for (int i = 0; i < n->ndef; i++) resolve(n->defaults[i], sc, file);
        for (int i = 0; i < n->nkwonly; i++) resolve(n->kwdefs[i], sc, file);
        if (n->d) resolve(n->d, sc, file);
        if (n->b) resolve(n->b, sc, file);
        RScope in = { n, 1, sc };
        resolve(n->a, &in, file);
        return;
    }
    case N_CLASS: {
        resolve_kids(n, sc, file);
        if (n->d) resolve(n->d, sc, file);
        resolve(n->b, sc, file);
        RScope in = { n, 2, sc };
        resolve(n->a, &in, file);
        return;
    }
    case N_COMP: {
        RScope in = { n, 3, sc };
        for (int i = 0; i < n->nk; i++) {
            Node *cl = n->kids[i];
            resolve(cl->b, cl->flags & 1 ? sc : &in, file);
            resolve(cl->a, &in, file);
            resolve_kids(cl, &in, file);
        }
        resolve(n->a, &in, file);
        resolve(n->b, &in, file);
        return;
    }
    default:
        resolve(n->a, sc, file); resolve(n->b, sc, file); resolve(n->c, sc, file); resolve(n->d, sc, file);
        resolve_kids(n, sc, file);
    }
}

Node *parse_program(const char *src, Str *file) {
    Lex L; memset(&L, 0, sizeof L);
    L.src = L.p = src; L.file = file;
    lex(&L);
    Ps p; memset(&p, 0, sizeof p);
    p.t = L.t; p.n = L.n; p.file = file;
    Coll mod; memset(&mod, 0, sizeof mod);
    p.coll = &mod;
    Node *prog = nn(&p, N_BLOCK);
    while (cur(&p)->k == T_NL) adv(&p);
    while (cur(&p)->k != T_EOF) {
        if (cur(&p)->k == T_DEDENT) { adv(&p); continue; }
        kid(prog, statement(&p));
    }
    prog->file = file;
    resolve(prog, 0, file);
    return prog;
}
