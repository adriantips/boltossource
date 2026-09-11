#include <stdint.h>
#include "shell.h"
#include "commands.h"
#include "fs.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include <stdio.h>
#include <limits.h>

/* ===========================================================================
 *  BoltOS  -  kernel/shscript.c
 *  The shell becomes a language.
 *
 *  BoltShell could run a command, and a list of commands separated by `;`, and
 *  pipe one into another. It could not ask a question about the answer. That
 *  is the difference between a shell you type at and a shell you can automate
 *  with, and on a machine meant for development it is the difference between
 *  doing a job once and doing it every time.
 *
 *  What is here is the small, familiar core: variables and `$` expansion,
 *  `if`/`then`/`elif`/`else`/`fi`, `while` and `for` loops with `break` and
 *  `continue`, functions, positional parameters, `$?`, and `test` so a
 *  condition has something to be about.
 *
 *  The parse is deliberately simple, and it is what makes the same text work
 *  typed at the prompt and stored in a file: a script is split into *segments*
 *  at every `;` and newline (quotes respected), so
 *
 *      if test -f /etc/motd; then cat /etc/motd; fi
 *
 *  and the same thing written over five lines produce exactly the same list of
 *  segments, and one statement machine runs both. Keywords like `then` and
 *  `do` end up as segments of their own, which is why they need the `;` before
 *  them that every shell has always wanted.
 * ===========================================================================*/

#define MAXSEG    512        /* segments in one script                        */
#define VAR_MAX   48
#define VAR_NAME  24
#define VAR_VAL   192
#define FUNC_MAX  16
#define FUNC_BODY 1024
#define DEPTH_MAX 16         /* nested function calls / script includes       */

/* ------------------------------------------------------------- variables --- */
static struct { char name[VAR_NAME]; char val[VAR_VAL]; int used; } vars[VAR_MAX];

static int var_find(const char *n) {
    for (int i = 0; i < VAR_MAX; i++)
        if (vars[i].used && !strcmp(vars[i].name, n)) return i;
    return -1;
}
const char *sh_var_get(const char *name) {
    int i = var_find(name);
    return i < 0 ? 0 : vars[i].val;
}
int sh_var_set(const char *name, const char *val) {
    if (!name || !name[0]) return -1;
    if (strlen(name) >= VAR_NAME || (val && strlen(val) >= VAR_VAL) ||
        (name[0] >= '0' && name[0] <= '9')) return -1;
    for (const char *p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) return -1;
    int i = var_find(name);
    if (i < 0) for (i = 0; i < VAR_MAX; i++) if (!vars[i].used) break;
    if (i >= VAR_MAX) return -1;
    vars[i].used = 1;
    uint32_t k = 0;
    for (; name[k] && k < VAR_NAME - 1; k++) vars[i].name[k] = name[k];
    vars[i].name[k] = 0;
    k = 0;
    for (; val && val[k] && k < VAR_VAL - 1; k++) vars[i].val[k] = val[k];
    vars[i].val[k] = 0;
    return 0;
}
void sh_var_unset(const char *name) {
    int i = var_find(name);
    if (i >= 0) vars[i].used = 0;
}

/* ------------------------------------------------ status and parameters ---- */
static int  g_status;                  /* $? */
static int  g_exit_req, g_exit_code;   /* `exit N` unwinds every enclosing block */
static int  g_loop;                    /* 1 = break, 2 = continue                */
static int  g_return_req, g_return_code;
static int  script_depth, stmt_depth;
static int  steps_left;

static int step(void) {
    if (steps_left-- > 0) return 1;
    kprintf("sh: execution limit reached\n");
    g_exit_req = 1; g_exit_code = 2;
    return 0;
}

/* Positional parameters, one frame per script or function call.
 *
 * A frame keeps its own copy of the words. They arrive as pointers into the
 * tokenizer's buffer, which the very next command overwrites -- so borrowing
 * them meant `$1` read whatever the last command line happened to leave
 * behind, which is the sort of bug that looks like the shell forgetting. */
#define ARG_MAX_LEN 96
typedef struct { char *argv[10]; int argc; char store[10][ARG_MAX_LEN]; } argframe_t;
static argframe_t frames[DEPTH_MAX];
static int nframe;

static int frame_push(int argc, char **argv) {
    if (nframe >= DEPTH_MAX || argc > 10) return -1;
    argframe_t *f = &frames[nframe];
    for (int i = 0; i < argc; i++) {
        if (strlen(argv[i]) >= ARG_MAX_LEN) return -1;
        uint32_t k = 0;
        for (; argv[i][k] && k < ARG_MAX_LEN - 1; k++) f->store[i][k] = argv[i][k];
        f->store[i][k] = 0;
        f->argv[i] = f->store[i];
    }
    f->argc = argc;
    nframe++;
    return 0;
}

int  sh_status(void)        { return g_status; }
void sh_set_status(int s)   { g_status = s; }

static const char *positional(int n) {
    if (!nframe) return 0;
    const argframe_t *f = &frames[nframe - 1];
    if (n < 0 || n >= f->argc) return 0;
    return f->argv[n];
}

/* -------------------------------------------------------------- expansion -- */
static int isname(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* ---------------------------------------------------------- arithmetic ----
 *  `$((...))`. A shell without it can write a loop it cannot get out of: the
 *  counter has nothing to increment it. Integers only, with the precedence
 *  everyone expects, and a division by zero yields 0 rather than trapping --
 *  a script is not worth a fault.
 * ------------------------------------------------------------------------ */
static long ar_expr(const char **p);
static int ar_depth;

static void ar_ws(const char **p) { while (**p == ' ' || **p == '\t') (*p)++; }

static long ar_atom(const char **p) {
    ar_ws(p);
    if (**p == '(' || **p == '-' || **p == '+') {
        if (ar_depth >= 24) return 0;
        char c = *(*p)++; ar_depth++;
        long v = c == '(' ? ar_expr(p) : ar_atom(p);
        ar_depth--;
        if (c == '(') { ar_ws(p); if (**p == ')') (*p)++; }
        return c == '-' ? (long)(0ul - (unsigned long)v) : v;
    }
    long v = 0;
    int any = 0;
    while (**p >= '0' && **p <= '9') { v = (long)((unsigned long)v * 10 + (**p - '0')); (*p)++; any = 1; }
    if (!any) {
        /* A bare name inside $(( )) is a variable, as in every shell. */
        char name[VAR_NAME];
        int n = 0;
        while (isname(**p) && n < VAR_NAME - 1) name[n++] = *(*p)++;
        name[n] = 0;
        const char *val = n ? sh_var_get(name) : 0;
        v = val ? atoi(val) : 0;
    }
    return v;
}

static long ar_mul(const char **p) {
    long v = ar_atom(p);
    for (;;) {
        ar_ws(p);
        char c = **p;
        if (c != '*' && c != '/' && c != '%') return v;
        (*p)++;
        long r = ar_atom(p);
        if (c == '*') v = (long)((unsigned long)v * (unsigned long)r);
        else if (!r) v = 0;                 /* division by zero: 0, not a fault */
        else if (v == LONG_MIN && r == -1) v = c == '/' ? LONG_MIN : 0;
        else if (c == '/') v /= r;
        else v %= r;
    }
}

static long ar_add(const char **p) {
    long v = ar_mul(p);
    for (;;) {
        ar_ws(p);
        char c = **p;
        if (c != '+' && c != '-') return v;
        (*p)++;
        long r = ar_mul(p);
        v = (long)((c == '+') ? (unsigned long)v + (unsigned long)r :
                               (unsigned long)v - (unsigned long)r);
    }
}

/* Comparisons too, so `X=$((A < B))` works the way a script expects. */
static long ar_expr(const char **p) {
    long v = ar_add(p);
    for (;;) {
        ar_ws(p);
        const char *q = *p;
        int op = 0;
        if      (q[0] == '<' && q[1] == '=') { op = 1; *p += 2; }
        else if (q[0] == '>' && q[1] == '=') { op = 2; *p += 2; }
        else if (q[0] == '=' && q[1] == '=') { op = 3; *p += 2; }
        else if (q[0] == '!' && q[1] == '=') { op = 4; *p += 2; }
        else if (q[0] == '<')                { op = 5; *p += 1; }
        else if (q[0] == '>')                { op = 6; *p += 1; }
        else return v;
        long r = ar_add(p);
        switch (op) {
        case 1: v = v <= r; break;
        case 2: v = v >= r; break;
        case 3: v = v == r; break;
        case 4: v = v != r; break;
        case 5: v = v <  r; break;
        default: v = v > r; break;
        }
    }
}

/* One $-reference at *p (which points at the '$'), appended to out at *o.
 * Factored out because the tokenizer expands as it goes -- it has to know
 * which characters were inside single quotes, and only it does. */
void sh_expand_at(const char **p, char *out, int cap, int *o) {
    const char *in = *p + 1;                  /* past the '$' */
    char name[VAR_NAME];
    int n = 0;
    const char *val = 0;
    char num[24];

    if (in[0] == '(' && in[1] == '(') {
        in += 2;
        long v = ar_expr(&in);
        while (*in == ' ') in++;
        if (in[0] == ')' && in[1] == ')') in += 2;
        snprintf(num, sizeof num, "%ld", v);
        val = num;
        for (int k = 0; val[k] && *o < cap - 1; k++) out[(*o)++] = val[k];
        *p = in;
        return;
    }
    if (*in == '{') {
        in++;
        while (*in && *in != '}' && n < VAR_NAME - 1) name[n++] = *in++;
        if (*in == '}') in++;
        name[n] = 0;
        val = sh_var_get(name);
    } else if (*in == '?') {
        in++; snprintf(num, sizeof num, "%d", g_status); val = num;
    } else if (*in == '#') {
        in++; snprintf(num, sizeof num, "%d", nframe ? frames[nframe-1].argc - 1 : 0); val = num;
    } else if (*in >= '0' && *in <= '9') {
        val = positional(*in - '0'); in++;
    } else if (isname(*in)) {
        while (isname(*in) && n < VAR_NAME - 1) name[n++] = *in++;
        name[n] = 0;
        val = sh_var_get(name);
    } else {
        if (*o < cap - 1) out[(*o)++] = '$';   /* a lone $ is just a $ */
        *p = in;
        return;
    }
    for (int k = 0; val && val[k] && *o < cap - 1; k++) out[(*o)++] = val[k];
    *p = in;
}

/* $NAME, ${NAME}, $?, $#, $0..$9. An unset variable expands to nothing, which
 * is what every shell does and what makes `X=$X$Y` work from empty. */
int sh_expand(const char *in, char *out, int cap) {
    int o = 0;
    const char *p = in;
    while (*p && o < cap - 1) {
        if (*p == '$' && p[1]) { sh_expand_at(&p, out, cap, &o); continue; }
        out[o++] = *p++;
    }
    out[o] = 0;
    return o;
}

/* -------------------------------------------------------------- functions -- */
static struct { char name[VAR_NAME]; char body[FUNC_BODY]; int used; } funcs[FUNC_MAX];

void sh_script_reset(void) {
    memset(vars, 0, sizeof vars);
    memset(funcs, 0, sizeof funcs);
    g_status = 0;
    /* Active calls retain their frames until they unwind. */
}

static int func_find(const char *n) {
    for (int i = 0; i < FUNC_MAX; i++)
        if (funcs[i].used && !strcmp(funcs[i].name, n)) return i;
    return -1;
}
static int func_define(const char *name, const char *body) {
    int i = func_find(name);
    if (i < 0) for (i = 0; i < FUNC_MAX; i++) if (!funcs[i].used) break;
    if (i >= FUNC_MAX) { kprintf("sh: too many functions\n"); return -1; }
    funcs[i].used = 1;
    uint32_t k = 0;
    for (; name[k] && k < VAR_NAME - 1; k++) funcs[i].name[k] = name[k];
    funcs[i].name[k] = 0;
    k = 0;
    for (; body[k] && k < FUNC_BODY - 1; k++) funcs[i].body[k] = body[k];
    funcs[i].body[k] = 0;
    return 0;
}

/* ---------------------------------------------------------------- parsing -- */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = 0;
    return s;
}

/* Is `seg`'s first word exactly `kw`? */
static int kw_is(const char *seg, const char *kw) {
    uint32_t n = (uint32_t)strlen(kw);
    if (strncmp(seg, kw, n)) return 0;
    return seg[n] == 0 || seg[n] == ' ' || seg[n] == '\t';
}
static const char *kw_rest(const char *seg, const char *kw) {
    const char *p = seg + strlen(kw);
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Add one segment, splitting a leading keyword off first.
 *
 * `then`, `do` and `else` are written with the command right after them --
 * `if C; then echo yes; fi` -- so they arrive glued to the first command of
 * their body. Cutting them apart here is what lets the statement machine see
 * one uniform list where every keyword is a segment of its own, whether the
 * script was written on one line or on five. */
static void push_seg(char **seg, int *n, int max, char *s) {
again:
    s = trim(s);
    if (!s[0] || s[0] == '#') return;
    if (*n >= max) { *n = max + 1; return; }
    static const char *kws[] = { "then", "do", "else", 0 };
    for (int k = 0; kws[k]; k++) {
        uint32_t L = (uint32_t)strlen(kws[k]);
        if (strncmp(s, kws[k], L) || (s[L] != ' ' && s[L] != '\t')) continue;
        char *rest = s + L;
        *rest++ = 0;                      /* the space becomes the terminator */
        if (*n < max) seg[(*n)++] = s;
        s = rest;
        goto again;
    }
    if (*n < max) seg[(*n)++] = s;
}

/* Split a script into segments at every `;` and newline outside quotes. The
 * text is modified in place, which is why callers hand over a copy. */
static int split(char *text, char **seg, int max) {
    int n = 0;
    char *p = text;
    int quote = 0;
    char *start = p;
    for (;; p++) {
        char c = *p;
        /* A comment runs to the newline, even when it contains semicolons.
         * Blanking it out in place (rather than skipping it) keeps every
         * later offset into `text` valid -- getting this wrong once turned
         * a `#` inside a later quoted string into part of the comment. */
        if (!quote && c == '#' && (p == start || p[-1] == ' ' || p[-1] == '\t')) {
            while (*p && *p != '\n') *p++ = ' ';
            c = *p;
        }
        if (c == '\\' && p[1] && quote != '\'') { p++; continue; }
        if (!quote && (c == '"' || c == '\'')) { quote = c; continue; }
        else if (quote && c == quote) { quote = 0; continue; }
        if (c && (quote || (c != ';' && c != '\n'))) continue;
        char save = c;
        *p = 0;
        push_seg(seg, &n, max, start);
        if (n > max || (!save && quote)) { kprintf("sh: too many statements or unterminated quote\n"); return -1; }
        if (!save) break;
        start = p + 1;
    }
    return n;
}

/* ------------------------------------------------------------ the machine -- */
static int run_stmts(char **s, int n);

/* Where does the block opened at `i` end? Returns the index of its closer
 * (fi / done / }), or -1. Nesting is counted so an inner `if` does not steal
 * the outer one's `fi`. */
static int block_end(char **s, int i, int n) {
    int depth = 0;
    for (int k = i; k < n; k++) {
        if (kw_is(s[k], "if") || kw_is(s[k], "while") || kw_is(s[k], "for")) depth++;
        else if (!strcmp(s[k], "{")) depth++;
        else if (kw_is(s[k], "fi") || kw_is(s[k], "done") || !strcmp(s[k], "}")) {
            depth--;
            if (depth == 0) return k;
        }
    }
    return -1;
}

/* The next segment at this block's own level that is one of the keywords. */
static int find_at_level(char **s, int from, int to, const char *a, const char *b) {
    int depth = 0;
    for (int k = from; k < to; k++) {
        if (kw_is(s[k], "if") || kw_is(s[k], "while") || kw_is(s[k], "for")) { depth++; continue; }
        if (kw_is(s[k], "fi") || kw_is(s[k], "done")) { depth--; continue; }
        if (depth != 0) continue;
        if (a && kw_is(s[k], a)) return k;
        if (b && kw_is(s[k], b)) return k;
    }
    return -1;
}

/* One simple command: assignment, function call, or a real command line. */
static int run_simple(char *seg) {
    /* NAME=value, before any tokenizing: the value may contain anything. */
    int eq = -1;
    for (int i = 0; seg[i]; i++) {
        if (seg[i] == '=') { eq = i; break; }
        if (!isname(seg[i])) break;
    }
    if (eq > 0) {
        if (eq >= VAR_NAME) { kprintf("sh: variable name too long\n"); return 2; }
        char name[VAR_NAME];
        int n = eq < VAR_NAME - 1 ? eq : VAR_NAME - 1;
        memcpy(name, seg, (uint32_t)n);
        name[n] = 0;
        char val[VAR_VAL];
        char *av[1];
        int ac = sh_words(seg + eq + 1, val, sizeof val, av, 1, 0);
        if (ac < 0) return 2;
        return sh_var_set(name, ac ? av[0] : "") < 0 ? 1 : 0;
    }

    /* A function call: run its body with the arguments as $1.. */
    char first[VAR_NAME];
    int fn = 0;
    while (seg[fn] && seg[fn] != ' ' && seg[fn] != '\t' && fn < VAR_NAME - 1) { first[fn] = seg[fn]; fn++; }
    first[fn] = 0;
    int fi = func_find(first);
    if (fi >= 0) {
        if (nframe >= DEPTH_MAX) { kprintf("sh: functions nested too deeply\n"); return 1; }
        /* the call's words become the positional parameters */
        char args[VAR_VAL];
        char *av[10]; int ac = 0;
        ac = sh_words(seg, args, sizeof args, av, 10, 0);
        if (ac < 0) return 2;
        if (frame_push(ac, av) != 0) { kprintf("sh: functions nested too deeply\n"); return 1; }
        char *body = (char *)kmalloc(FUNC_BODY);
        int rc = 1;
        if (body) {
            memcpy(body, funcs[fi].body, FUNC_BODY);
            char **bs = (char **)kmalloc(sizeof(char *) * MAXSEG);
            if (bs) {
                int bn = split(body, bs, MAXSEG);
                rc = run_stmts(bs, bn);
                kfree(bs);
            }
            kfree(body);
        }
        if (g_return_req) { rc = g_return_code; g_return_req = 0; }
        nframe--;
        return rc;
    }

    return sh_exec_simple(seg);
}

/* Run a list of segments. Returns the status of the last command. */
static int run_block(char **s, int n) {
    int rc = 0;
    for (int i = 0; i < n; i++) {
        if (g_exit_req || g_loop || g_return_req) return rc;
        if (!step()) return 2;
        char *seg = s[i];
        if (!seg[0]) continue;

        /* ---- if COND; then ...; [elif COND; then ...;] [else ...;] fi ---- */
        if (kw_is(seg, "if")) {
            int end = block_end(s, i, n);
            if (end < 0 || strcmp(s[end], "fi")) { kprintf("sh: `if` with no `fi`\n"); return 2; }
            int cur = i;
            int done = 0;
            while (!done) {
                int th = find_at_level(s, cur + 1, end, "then", 0);
                if (th < 0) { kprintf("sh: `if` with no `then`\n"); return 1; }
                int nxt = find_at_level(s, th + 1, end, "elif", "else");
                int body_end = (nxt >= 0) ? nxt : end;

                char cond[256];
                snprintf(cond, sizeof cond, "%s",
                         kw_rest(s[cur], kw_is(s[cur], "elif") ? "elif" : "if"));
                rc = run_simple(cond);               /* the condition itself */
                if (g_exit_req || g_return_req) return rc;
                g_status = rc;
                if (rc == 0) {
                    rc = run_stmts(&s[th + 1], body_end - th - 1);
                    done = 1;
                } else if (nxt < 0) {
                    done = 1;
                } else if (kw_is(s[nxt], "else")) {
                    rc = run_stmts(&s[nxt + 1], end - nxt - 1);
                    done = 1;
                } else {
                    cur = nxt;                       /* elif: same shape again */
                }
            }
            i = end;
            continue;
        }
        /* ---- while COND; do ...; done ------------------------------------ */
        if (kw_is(seg, "while")) {
            int end = block_end(s, i, n);
            if (end < 0 || strcmp(s[end], "done")) { kprintf("sh: `while` with no `done`\n"); return 2; }
            int d = find_at_level(s, i + 1, end, "do", 0);
            if (d < 0) { kprintf("sh: `while` with no `do`\n"); return 1; }
            char cond[256];
            snprintf(cond, sizeof cond, "%s", kw_rest(seg, "while"));
            for (int guard = 0; guard < 100000; guard++) {
                if (!step()) return 2;
                char c2[256];
                snprintf(c2, sizeof c2, "%s", cond);
                int cr = run_simple(c2);
                g_status = cr;
                if (cr != 0 || g_exit_req) break;
                rc = run_stmts(&s[d + 1], end - d - 1);
                if (g_exit_req || g_return_req) break;
                if (g_loop == 1) { g_loop = 0; break; }
                if (g_loop == 2) g_loop = 0;
            }
            i = end;
            continue;
        }

        /* ---- for NAME in WORDS; do ...; done ----------------------------- */
        if (kw_is(seg, "for")) {
            int end = block_end(s, i, n);
            if (end < 0 || strcmp(s[end], "done")) { kprintf("sh: `for` with no `done`\n"); return 2; }
            int d = find_at_level(s, i + 1, end, "do", 0);
            if (d < 0) { kprintf("sh: `for` with no `do`\n"); return 1; }

            char head[256];
            char *words[24];
            int nw = sh_words(kw_rest(seg, "for"), head, sizeof head, words, 24, 0);
            if (nw < 2 || strcmp(words[1], "in")) { kprintf("sh: expected for NAME in WORDS\n"); return 2; }
            for (int word = 2; word < nw; word++) {
                sh_var_set(words[0], words[word]);
                rc = run_stmts(&s[d + 1], end - d - 1);
                if (g_exit_req || g_return_req) break;
                if (g_loop == 1) { g_loop = 0; break; }
                if (g_loop == 2) g_loop = 0;
            }
            i = end;
            continue;
        }

        /* ---- NAME() { ... } ---------------------------------------------- */
        {
            int len = (int)strlen(seg);
            if (len > 3 && seg[len - 1] == '{') {
                char nm[VAR_NAME];
                int k = 0;
                while (seg[k] && seg[k] != '(' && k < VAR_NAME - 1) { nm[k] = seg[k]; k++; }
                nm[k] = 0;
                if (seg[k] == '(' && k > 0) {
                    /* A definition's braces are not block_end's business: its
                     * openers are keywords, and here the opener is a trailing
                     * `{` on the header line. */
                    int close = -1;
                    int depth = 1;
                    for (int q = i + 1; q < n; q++) {
                        int l2 = (int)strlen(s[q]);
                        if (l2 && s[q][l2 - 1] == '{') depth++;
                        else if (!strcmp(s[q], "}")) { depth--; if (!depth) { close = q; break; } }
                    }
                    if (close < 0) { kprintf("sh: function `%s` with no closing `}`\n", nm); return 1; }
                    char body[FUNC_BODY];
                    int o = 0;
                    for (int q = i + 1; q < close; q++) {
                        if ((int)strlen(s[q]) + o + 2 >= FUNC_BODY) {
                            kprintf("sh: function body too long\n"); return 2;
                        }
                        for (const char *c = s[q]; *c && o < FUNC_BODY - 2; c++) body[o++] = *c;
                        body[o++] = '\n';
                    }
                    body[o] = 0;
                    rc = func_define(nm, body) < 0 ? 1 : 0;
                    g_status = rc;
                    i = close;
                    continue;
                }
            }
        }

        /* ---- break / continue / return / exit ---------------------------- */
        if (kw_is(seg, "break"))    { g_loop = 1; continue; }
        if (kw_is(seg, "continue")) { g_loop = 2; continue; }
        if (kw_is(seg, "return")) {
            char v[32]; sh_expand(kw_rest(seg, "return"), v, sizeof v);
            g_return_code = v[0] ? atoi(v) : g_status;
            g_return_req = 1;
            return g_return_code;
        }
        if (kw_is(seg, "exit")) {
            char v[32]; sh_expand(kw_rest(seg, "exit"), v, sizeof v);
            g_exit_code = v[0] ? atoi(v) : g_status;
            g_exit_req = 1;
            return g_exit_code;
        }

        /* keywords that are not statements on their own */
        if (kw_is(seg, "then") || kw_is(seg, "do") || kw_is(seg, "else") ||
            kw_is(seg, "fi") || kw_is(seg, "done") || !strcmp(seg, "}")) continue;

        rc = run_simple(seg);
        g_status = rc;
    }
    return rc;
}

static int run_stmts(char **s, int n) {
    if (stmt_depth >= 8) { kprintf("sh: blocks nested too deeply\n"); return 2; }
    stmt_depth++;
    int rc = run_block(s, n);
    stmt_depth--;
    g_status = rc;
    return rc;
}

/* --------------------------------------------------------------- entries -- */
int sh_run_script(const char *text, int argc, char **argv) {
    if (script_depth >= 8 || (argc > 0 && nframe >= DEPTH_MAX)) {
        kprintf("sh: scripts nested too deeply\n"); return 2;
    }
    uint32_t len = (uint32_t)strlen(text);
    if (len > 65536) { kprintf("sh: script exceeds 64 KiB\n"); return 2; }
    char *copy = (char *)kmalloc(len + 2);
    if (!copy) { kprintf("sh: out of memory\n"); return 1; }
    memcpy(copy, text, len + 1);

    char **segs = (char **)kmalloc(sizeof(char *) * MAXSEG);
    if (!segs) { kfree(copy); kprintf("sh: out of memory\n"); return 1; }
    int n = split(copy, segs, MAXSEG);
    if (n < 0) { kfree(segs); kfree(copy); return 2; }

    int pushed = (argc > 0 && frame_push(argc, argv) == 0);
    if (argc > 0 && !pushed) {
        kprintf("sh: too many or overlong script arguments\n");
        kfree(segs); kfree(copy); return 2;
    }

    if (!script_depth) steps_left = 10000;
    script_depth++;
    int rc = run_stmts(segs, n);
    script_depth--;
    if (g_exit_req) { rc = g_exit_code; g_exit_req = 0; }
    if (g_return_req) { rc = g_return_code; g_return_req = 0; }
    g_loop = 0;
    if (pushed) nframe--;

    kfree(segs);
    kfree(copy);
    g_status = rc;
    return rc;
}

/* ------------------------------------------------------------- builtins ---- */
/* `test` / `[` -- the thing a condition is about. */
int cmd_test(int argc, char **argv) {
    /* `[ ... ]` is `test ...` with a closing bracket that must be last. */
    if (!strcmp(argv[0], "[")) {
        if (argc < 2 || strcmp(argv[argc - 1], "]")) { kprintf("[: missing ']'\n"); return 2; }
        argc--;
    }
    if (argc == 1) return 1;                       /* nothing is false */

    if (argc == 2) return argv[1][0] ? 0 : 1;      /* a non-empty word is true */

    if (argc == 3) {
        const char *op = argv[1], *a = argv[2];
        if (!strcmp(op, "-z")) return a[0] ? 1 : 0;
        if (!strcmp(op, "-n")) return a[0] ? 0 : 1;
        if (!strcmp(op, "-f")) { fs_node *f = fs_lookup(a); return (f && !f->is_dir) ? 0 : 1; }
        if (!strcmp(op, "-d")) { fs_node *f = fs_lookup(a); return (f && f->is_dir) ? 0 : 1; }
        if (!strcmp(op, "-e")) return fs_lookup(a) ? 0 : 1;
        kprintf("test: unknown operator '%s'\n", op);
        return 2;
    }

    if (argc == 4) {
        const char *a = argv[1], *op = argv[2], *b = argv[3];
        if (!strcmp(op, "="))  return strcmp(a, b) == 0 ? 0 : 1;
        if (!strcmp(op, "!=")) return strcmp(a, b) != 0 ? 0 : 1;
        int x = atoi(a), y = atoi(b);
        if (!strcmp(op, "-eq")) return x == y ? 0 : 1;
        if (!strcmp(op, "-ne")) return x != y ? 0 : 1;
        if (!strcmp(op, "-lt")) return x <  y ? 0 : 1;
        if (!strcmp(op, "-le")) return x <= y ? 0 : 1;
        if (!strcmp(op, "-gt")) return x >  y ? 0 : 1;
        if (!strcmp(op, "-ge")) return x >= y ? 0 : 1;
        kprintf("test: unknown operator '%s'\n", op);
        return 2;
    }
    kprintf("test: too many arguments\n");
    return 2;
}

int cmd_true(int argc, char **argv)  { (void)argc; (void)argv; return 0; }
int cmd_false(int argc, char **argv) { (void)argc; (void)argv; return 1; }

int cmd_set(int argc, char **argv) {
    if (argc == 1) {
        int n = 0;
        for (int i = 0; i < VAR_MAX; i++)
            if (vars[i].used) { kprintf("%s=%s\n", vars[i].name, vars[i].val); n++; }
        if (!n) kprintf("(no variables set)\n");
        return 0;
    }
    if (argc == 3) return sh_var_set(argv[1], argv[2]) < 0 ? 1 : 0;
    kprintf("usage: set [NAME VALUE]      (or NAME=VALUE, which needs no command)\n");
    return 1;
}

int cmd_unset(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: unset NAME\n"); return 1; }
    for (int i = 1; i < argc; i++) sh_var_unset(argv[i]);
    return 0;
}

/* `sh FILE [args]` -- run a script. Also `source` / `.`, which are the same
 * thing here: there are no subshells, so a script already sees and keeps the
 * variables of the session that started it. */
int cmd_sh(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: sh FILE [args...]\n"
                "  Run a script: variables, if/elif/else, while, for, functions,\n"
                "  break/continue/return/exit, $?, $1.. and test. See: man sh\n");
        return 1;
    }
    fs_node *f = fs_lookup(argv[1]);
    if (!f || f->is_dir || !fs_may(f, FS_PERM_READ)) {
        kprintf("sh: cannot read %s\n", argv[1]);
        return 1;
    }
    char *text = (char *)kmalloc(f->size + 1);
    if (!text) { kprintf("sh: out of memory\n"); return 1; }
    if (f->data && f->size) memcpy(text, f->data, f->size);
    text[f->size] = 0;

    /* A leading #! line is a comment to us -- there is one shell. */
    int rc = sh_run_script(text, argc - 1, argv + 1);
    kfree(text);
    return rc;
}
