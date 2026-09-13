#include <stdint.h>
#include <stddef.h>
#include "make.h"
#include "fs.h"
#include "shell.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  BoltMake -- `target: prerequisites` and a recipe, which is the whole idea
 *  and has been since 1976.
 *
 *  Variables (= := +=), $(NAME) and ${NAME}, $@ $< $^, pattern rules with one
 *  %, .PHONY, comments, and a default goal that is the first real target in
 *  the file. Recipes go to sh_run_script(), so a recipe line is an ordinary
 *  BoltShell line: pipes, redirection and shell variables all work in one
 *  without this file knowing anything about them.
 *
 *  ---- why this does not compare timestamps --------------------------------
 *
 *  Every other make decides a target is out of date when a prerequisite is
 *  *newer* than it. That needs a clock that only goes forwards, and this
 *  machine does not have one where it counts: fs_node.mtime is pit ticks since
 *  boot, and it is persisted. So a file edited early in today's session
 *  carries a smaller number than the object built from it last week, and
 *  "newer" reads backwards -- the target looks current and make quietly
 *  declines to rebuild it. Silently not building is the single worst thing a
 *  build tool can do, so this one does not do it.
 *
 *  Instead each target records what its prerequisites looked like -- size and
 *  mtime -- the last time its recipe ran, in a .make.stamp beside the
 *  makefile, and rebuilds when any of them differs. Different, not newer: it
 *  needs no ordering at all, so it survives a reboot, an mtime that goes
 *  backwards, and a file restored from a copy. redo and tup work this way for
 *  the same reason. Delete the stamp file and everything rebuilds once, which
 *  is the worst it can get wrong.
 * ===========================================================================*/

#define MK_MAX_VARS    64
#define MK_MAX_RULES   96
#define MK_MAX_PRE     24
#define MK_MAX_CMDS    12
#define MK_MAX_STAMPS  384
#define MK_MAX_DEPTH   16
#define MK_LINE        512

/* A prerequisite list is MK_MAX_PRE * FS_PATH_MAX -- six kilobytes -- and
 * build() recurses. On a 64 KB kernel stack that is a stack overflow at about
 * nine levels of dependency, which is not deep for a makefile. So each level's
 * list lives on the heap and is allocated the first time that depth is
 * reached. */
typedef char PreList[MK_MAX_PRE][FS_PATH_MAX];

typedef struct { char name[40]; char *value; } Var;

typedef struct {
    char *target;                       /* may hold one '%' */
    char *pre[MK_MAX_PRE]; int npre;
    char *cmd[MK_MAX_CMDS]; int ncmd;
    int   phony, pattern, done;
    int   share_from;                   /* `a b: dep` -- whose recipe is mine */
} Rule;

typedef struct {
    const char *tgt, *pre;
    uint32_t    size;
    uint64_t    mtime;
    int         live;
} Stamp;

/* Strings this file makes up itself. A chunk list rather than one buffer that
 * doubles: the rules, the variables and the stamps all hold pointers into it,
 * and stamp_put() adds more of them *during* the build, long after the parse
 * handed those pointers out. A realloc there would move every string already
 * in use, which is a bug that shows up as a rule built for a target with a
 * shredded name. Chunks never move. */
typedef struct Chunk { struct Chunk *next; uint32_t len, cap; } Chunk;

typedef struct {
    char  *text;                        /* the makefile, chopped up in place */
    Var    vars[MK_MAX_VARS];   int nvar;
    Rule   rules[MK_MAX_RULES]; int nrule;
    Stamp  stamps[MK_MAX_STAMPS]; int nstamp;

    Chunk *chunks;
    PreList *scratch[MK_MAX_DEPTH];
    char   stampdir[FS_PATH_MAX];

    int    dry, silent, quiet, failed, built;
    char   err[160];
} MK;

/* ---- odds and ends ------------------------------------------------------- */

static int mk_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static char *mk_dup(MK *m, const char *s, uint32_t n) {
    Chunk *c = m->chunks;
    if (!c || c->len + n + 1 > c->cap) {
        uint32_t want = n + 1 > 4096 ? n + 1 : 4096;
        c = (Chunk *)kmalloc((uint32_t)sizeof(Chunk) + want);
        if (!c) { m->failed = 1; strlcpy(m->err, "out of memory", sizeof m->err); return 0; }
        c->next = m->chunks;
        c->len  = 0;
        c->cap  = want;
        m->chunks = c;
    }
    char *at = (char *)(c + 1) + c->len;
    memcpy(at, s, n);
    at[n] = 0;
    c->len += n + 1;
    return at;
}

static const char *var_get(MK *m, const char *name) {
    for (int i = 0; i < m->nvar; i++)
        if (strcmp(m->vars[i].name, name) == 0) return m->vars[i].value;
    return 0;
}

static void var_set(MK *m, const char *name, const char *value, int append) {
    for (int i = 0; i < m->nvar; i++) {
        if (strcmp(m->vars[i].name, name)) continue;
        if (!append) { m->vars[i].value = (char *)value; return; }
        char joined[MK_LINE];
        joined[0] = 0;
        strlcat(joined, m->vars[i].value ? m->vars[i].value : "", sizeof joined);
        if (joined[0]) strlcat(joined, " ", sizeof joined);
        strlcat(joined, value, sizeof joined);
        m->vars[i].value = mk_dup(m, joined, (uint32_t)strlen(joined));
        return;
    }
    if (m->nvar >= MK_MAX_VARS) return;
    strlcpy(m->vars[m->nvar].name, name, sizeof m->vars[m->nvar].name);
    m->vars[m->nvar].value = (char *)value;
    m->nvar++;
}

/* $(NAME), ${NAME}, $@, $<, $^ and $$. An unknown name expands to nothing,
 * which is make's rule and is occasionally the reason a recipe runs with an
 * argument missing rather than refusing -- as elsewhere, matching the tool
 * people already know beats being cleverer than it. */
static void mk_expand(MK *m, const char *in, char *out, int cap,
                      const char *tgt, const char *first, const char *all) {
    int o = 0;
    for (const char *p = in; *p && o < cap - 1; ) {
        if (*p != '$') { out[o++] = *p++; continue; }
        p++;
        if (*p == '$') { out[o++] = '$'; p++; continue; }
        const char *val = 0;
        if (*p == '@')      { val = tgt;   p++; }
        else if (*p == '<') { val = first; p++; }
        else if (*p == '^') { val = all;   p++; }
        else if (*p == '(' || *p == '{') {
            char close = (*p == '(') ? ')' : '}';
            p++;
            char name[40];
            int n = 0;
            while (*p && *p != close && n + 1 < (int)sizeof name) name[n++] = *p++;
            name[n] = 0;
            if (*p == close) p++;
            val = var_get(m, name);
        } else {
            /* A bare $x is a one-letter variable in make. Nothing here defines
             * one, so it disappears -- same as an unknown $(NAME). */
            if (*p) p++;
        }
        for (const char *v = val; v && *v && o < cap - 1; v++) out[o++] = *v;
    }
    out[o] = 0;
}

/* ---- the stamp file ------------------------------------------------------ */

static void stamp_path(MK *m, char *out, uint32_t cap) {
    strlcpy(out, m->stampdir, cap);
    if (out[0] && out[strlen(out) - 1] != '/') strlcat(out, "/", cap);
    strlcat(out, ".make.stamp", cap);
}

static uint64_t parse_u64(const char **p) {
    uint64_t v = 0;
    while (**p == ' ') (*p)++;
    while (**p >= '0' && **p <= '9') { v = v * 10 + (uint64_t)(**p - '0'); (*p)++; }
    return v;
}

static void stamp_load(MK *m) {
    char path[FS_PATH_MAX];
    stamp_path(m, path, sizeof path);
    fs_node *f = fs_lookup(path);
    if (!f || f->is_dir || !f->size || !fs_may(f, FS_PERM_READ)) return;

    char *text = mk_dup(m, (const char *)f->data, f->size);
    if (!text) return;
    char *p = text;
    while (*p && m->nstamp < MK_MAX_STAMPS) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;

        char *t = line;
        while (*t == ' ') t++;
        char *sp = strchr(t, ' ');
        if (!sp) continue;
        *sp = 0;
        char *pre = sp + 1;
        sp = strchr(pre, ' ');
        if (!sp) continue;
        *sp = 0;
        const char *rest = sp + 1;
        uint64_t size = parse_u64(&rest);
        uint64_t mt = parse_u64(&rest);

        Stamp *s = &m->stamps[m->nstamp++];
        s->tgt = t; s->pre = pre;
        s->size = (uint32_t)size; s->mtime = mt;
        s->live = 1;
    }
}

static Stamp *stamp_find(MK *m, const char *tgt, const char *pre) {
    for (int i = 0; i < m->nstamp; i++)
        if (m->stamps[i].live && !strcmp(m->stamps[i].tgt, tgt) &&
            !strcmp(m->stamps[i].pre, pre)) return &m->stamps[i];
    return 0;
}

static void stamp_put(MK *m, const char *tgt, const char *pre,
                      uint32_t size, uint64_t mtime) {
    Stamp *s = stamp_find(m, tgt, pre);
    if (!s) {
        if (m->nstamp >= MK_MAX_STAMPS) return;
        s = &m->stamps[m->nstamp++];
        s->tgt  = mk_dup(m, tgt, (uint32_t)strlen(tgt));
        s->pre  = mk_dup(m, pre, (uint32_t)strlen(pre));
        s->live = 1;
        if (!s->tgt || !s->pre) { m->nstamp--; return; }
    }
    s->size  = size;
    s->mtime = mtime;
}

static void utoa64(uint64_t v, char *out) {
    char tmp[24];
    int n = 0;
    if (!v) tmp[n++] = '0';
    while (v && n < (int)sizeof tmp) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int o = 0;
    while (n) out[o++] = tmp[--n];
    out[o] = 0;
}

static void stamp_save(MK *m) {
    if (m->dry || !m->built) return;

    uint32_t cap = 4096, len = 0;
    char *buf = (char *)kmalloc(cap);
    if (!buf) return;
    for (int i = 0; i < m->nstamp; i++) {
        Stamp *s = &m->stamps[i];
        if (!s->live) continue;
        char line[FS_PATH_MAX * 2];
        char a[24], b[24];
        utoa64(s->size, a);
        utoa64(s->mtime, b);
        line[0] = 0;
        strlcat(line, s->tgt, sizeof line);
        strlcat(line, " ", sizeof line);
        strlcat(line, s->pre, sizeof line);
        strlcat(line, " ", sizeof line);
        strlcat(line, a, sizeof line);
        strlcat(line, " ", sizeof line);
        strlcat(line, b, sizeof line);
        strlcat(line, "\n", sizeof line);
        uint32_t n = (uint32_t)strlen(line);
        if (len + n + 1 > cap) {
            uint32_t nc = cap * 2;
            while (nc < len + n + 1) nc *= 2;
            char *nb = (char *)kmalloc(nc);
            if (!nb) { kfree(buf); return; }
            memcpy(nb, buf, len);
            kfree(buf);
            buf = nb; cap = nc;
        }
        memcpy(buf + len, line, n);
        len += n;
    }

    char path[FS_PATH_MAX];
    stamp_path(m, path, sizeof path);
    fs_node *f = fs_lookup(path);
    if (!f) f = fs_create(path, 0);
    if (f && !f->is_dir && fs_may(f, FS_PERM_WRITE)) fs_write(f, buf, len);
    kfree(buf);
}

/* ---- parsing ------------------------------------------------------------- */

static Rule *rule_find(MK *m, const char *target) {
    for (int i = 0; i < m->nrule; i++)
        if (!m->rules[i].pattern && !strcmp(m->rules[i].target, target))
            return &m->rules[i];
    return 0;
}

static void mark_phony(MK *m, const char *names) {
    char one[FS_PATH_MAX];
    const char *p = names;
    while (*p) {
        while (mk_space(*p)) p++;
        int n = 0;
        while (*p && !mk_space(*p) && n + 1 < (int)sizeof one) one[n++] = *p++;
        one[n] = 0;
        if (!n) break;
        Rule *r = rule_find(m, one);
        if (r) r->phony = 1;
        else {
            /* .PHONY often comes before the rule it names. Remembering it as a
             * bodiless rule is the cheapest way to have it still be true when
             * the real one turns up. */
            if (m->nrule >= MK_MAX_RULES) return;
            Rule *nr = &m->rules[m->nrule++];
            memset(nr, 0, sizeof *nr);
            nr->target = mk_dup(m, one, (uint32_t)n);
            nr->phony = 1;
        }
    }
}

static int parse(MK *m, char *text) {
    m->text = text;
    Rule *cur = 0;
    char *p = text;

    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;

        char *end = line + strlen(line);
        while (end > line && mk_space(end[-1])) *--end = 0;

        /* A tab means a recipe, as it has always meant. So do leading spaces,
         * once a rule is open -- which is not how make(1) behaves and is a
         * deliberate difference. Tabs are awkward to get into a file on this
         * machine: `write` takes no escapes and the interactive shell reads a
         * typed Tab as a completion request, so a makefile written from the
         * shell cannot contain one. "Missing separator" is make's most famous
         * papercut anyway, and nothing in this subset needs an indented line
         * to mean anything else. */
        if (line[0] == '\t' || (cur && (line[0] == ' ') && line[1])) {
            char *c = line + 1;
            while (mk_space(*c)) c++;
            if (!*c || *c == '#') continue;
            if (!cur) {
                strlcpy(m->err, "a recipe line before any rule", sizeof m->err);
                return -1;
            }
            if (cur->ncmd < MK_MAX_CMDS) cur->cmd[cur->ncmd++] = c;
            continue;
        }

        char *s = line;
        while (mk_space(*s)) s++;
        if (!*s || *s == '#') continue;
        cur = 0;

        /* An assignment is an '=' with no ':' before it -- except for ':=',
         * where the colon is part of the operator. */
        char *eq = strchr(s, '=');
        char *colon = strchr(s, ':');
        if (eq && (!colon || colon > eq || (colon + 1 == eq))) {
            int append = (eq > s && eq[-1] == '+');
            char *nend = eq;
            if (append || (eq > s && eq[-1] == ':')) nend--;
            while (nend > s && mk_space(nend[-1])) nend--;
            char save = *nend;
            *nend = 0;
            char name[40];
            strlcpy(name, s, sizeof name);
            *nend = save;

            char *v = eq + 1;
            while (mk_space(*v)) v++;
            char expanded[MK_LINE];
            mk_expand(m, v, expanded, sizeof expanded, "", "", "");
            char *val = mk_dup(m, expanded, (uint32_t)strlen(expanded));
            if (!val) return -1;
            var_set(m, name, val, append);
            continue;
        }

        if (!colon) {
            strlcpy(m->err, "a line that is neither a rule nor an assignment", sizeof m->err);
            return -1;
        }

        *colon = 0;
        char targets[MK_LINE], prereqs[MK_LINE];
        mk_expand(m, s, targets, sizeof targets, "", "", "");
        mk_expand(m, colon + 1, prereqs, sizeof prereqs, "", "", "");

        if (!strncmp(targets, ".PHONY", 6)) { mark_phony(m, prereqs); continue; }

        /* Several targets on one line share one recipe, which is how
         * `a.o b.o: common.h` is written. The recipe attaches to the last of
         * them and the earlier ones are pointed at it, here, while it is still
         * known that they were on one line. Working it out afterwards from
         * matching prerequisite lists does not work: `all: prog` followed by
         * `install: prog` looks identical and is not the same thing, and the
         * version that guessed gave `all` install's recipe. */
        int mine[MK_MAX_PRE];
        int ntarget = 0;
        const char *t = targets;
        while (*t) {
            while (mk_space(*t)) t++;
            char one[FS_PATH_MAX];
            int n = 0;
            while (*t && !mk_space(*t) && n + 1 < (int)sizeof one) one[n++] = *t++;
            one[n] = 0;
            if (!n) break;
            if (m->nrule >= MK_MAX_RULES) {
                strlcpy(m->err, "too many rules", sizeof m->err);
                return -1;
            }
            Rule *existing = rule_find(m, one);
            Rule *r;
            if (existing && !existing->ncmd && !existing->npre) r = existing;
            else {
                r = &m->rules[m->nrule++];
                memset(r, 0, sizeof *r);
                r->target = mk_dup(m, one, (uint32_t)n);
                if (!r->target) return -1;
                if (existing) r->phony = existing->phony;
            }
            r->share_from = -1;
            r->pattern = strchr(r->target, '%') != 0;
            if (ntarget < MK_MAX_PRE) mine[ntarget] = (int)(r - m->rules);
            ntarget++;

            const char *q = prereqs;
            while (*q && r->npre < MK_MAX_PRE) {
                while (mk_space(*q)) q++;
                char pn[FS_PATH_MAX];
                int k = 0;
                while (*q && !mk_space(*q) && k + 1 < (int)sizeof pn) pn[k++] = *q++;
                pn[k] = 0;
                if (!k) break;
                r->pre[r->npre] = mk_dup(m, pn, (uint32_t)k);
                if (!r->pre[r->npre]) return -1;
                r->npre++;
            }
            cur = r;
        }
        for (int k = 0; k + 1 < ntarget && k + 1 <= MK_MAX_PRE; k++)
            m->rules[mine[k]].share_from = mine[ntarget - 1];
    }

    /* The recipe went to the last target on its line; hand it to the others. */
    for (int i = 0; i < m->nrule; i++) {
        Rule *r = &m->rules[i];
        if (r->ncmd || r->share_from < 0 || r->share_from >= m->nrule) continue;
        Rule *src = &m->rules[r->share_from];
        for (int c = 0; c < src->ncmd; c++) r->cmd[c] = src->cmd[c];
        r->ncmd = src->ncmd;
    }
    return 0;
}

/* ---- building ------------------------------------------------------------ */

/* `%.o` against `main.o` gives the stem "main". Returns 0 if it does not
 * match. One % per pattern, which is all make ever had either. */
static int stem_of(const char *pattern, const char *name, char *stem, uint32_t cap) {
    const char *pc = strchr(pattern, '%');
    if (!pc) return 0;
    uint32_t pre = (uint32_t)(pc - pattern);
    uint32_t suf = (uint32_t)strlen(pc + 1);
    uint32_t nl = (uint32_t)strlen(name);
    if (nl < pre + suf) return 0;
    if (strncmp(pattern, name, pre)) return 0;
    if (suf && strcmp(name + nl - suf, pc + 1)) return 0;
    uint32_t sl = nl - pre - suf;
    if (sl + 1 > cap) return 0;
    memcpy(stem, name + pre, sl);
    stem[sl] = 0;
    return 1;
}

static void stem_into(const char *pattern, const char *stem, char *out, uint32_t cap) {
    const char *pc = strchr(pattern, '%');
    if (!pc) { strlcpy(out, pattern, cap); return; }
    uint32_t pre = (uint32_t)(pc - pattern);
    if (pre + 1 > cap) pre = cap - 1;
    memcpy(out, pattern, pre);
    out[pre] = 0;
    strlcat(out, stem, cap);
    strlcat(out, pc + 1, cap);
}

static int build(MK *m, const char *target, int depth);

/* One rule, already matched to a concrete target and a concrete prerequisite
 * list. Returns 0 if the target is now current. */
/* This depth's prerequisite list, made on first use. */
static PreList *scratch_at(MK *m, int depth) {
    if (depth < 0 || depth >= MK_MAX_DEPTH) return 0;
    if (!m->scratch[depth]) {
        m->scratch[depth] = (PreList *)kmalloc(sizeof(PreList));
        if (!m->scratch[depth]) { m->failed = 1; strlcpy(m->err, "out of memory", sizeof m->err); }
    }
    return m->scratch[depth];
}

static int run_rule(MK *m, const char *target, Rule *r,
                    char pre[][FS_PATH_MAX], int npre, int depth) {
    for (int i = 0; i < npre; i++)
        if (build(m, pre[i], depth + 1) != 0) return -1;

    fs_node *tf = fs_lookup(target);
    int stale = r->phony || !tf || tf->is_dir;

    for (int i = 0; i < npre && !stale; i++) {
        fs_node *pf = fs_lookup(pre[i]);
        if (!pf) { stale = 1; break; }
        Stamp *s = stamp_find(m, target, pre[i]);
        if (!s || s->size != pf->size || s->mtime != pf->mtime) stale = 1;
    }

    if (!stale) return 0;
    if (!r->ncmd) {
        /* A rule with prerequisites and no recipe is a dependency statement,
         * not a build step. Nothing to run and nothing to complain about. */
        return 0;
    }

    char all[MK_LINE];
    all[0] = 0;
    for (int i = 0; i < npre; i++) {
        if (i) strlcat(all, " ", sizeof all);
        strlcat(all, pre[i], sizeof all);
    }

    for (int c = 0; c < r->ncmd; c++) {
        const char *raw = r->cmd[c];
        int hush = 0;
        while (*raw == '@' || *raw == '-') { if (*raw == '@') hush = 1; raw++; }

        char line[MK_LINE];
        mk_expand(m, raw, line, sizeof line, target, npre ? pre[0] : "", all);
        if (!line[0]) continue;

        if (!m->silent && !hush) kprintf("%s\n", line);
        if (m->dry) continue;

        int rc = sh_run_script(line, 0, 0);
        if (rc != 0) {
            kprintf("make: *** [%s] Error %d\n", target, rc);
            m->failed = 1;
            return -1;
        }
    }

    m->built = 1;
    if (!m->dry) {
        for (int i = 0; i < npre; i++) {
            /* Re-looked-up on purpose: the recipe may have replaced the file,
             * and the node it had before is not necessarily the node it has
             * now. */
            fs_node *pf = fs_lookup(pre[i]);
            if (pf) stamp_put(m, target, pre[i], pf->size, pf->mtime);
        }
    }
    return 0;
}

static int build(MK *m, const char *target, int depth) {
    if (m->failed) return -1;
    if (depth > MK_MAX_DEPTH) {
        kprintf("make: %s: dependencies nested too deeply (a cycle?)\n", target);
        m->failed = 1;
        return -1;
    }

    PreList *pre = scratch_at(m, depth);
    if (!pre) return -1;

    Rule *r = rule_find(m, target);
    if (r) {
        /* Three states, not two. Without the middle one a cycle is not an
         * error, it is a rule that quietly reports success because it is
         * already being built -- and the recipe then runs against a
         * prerequisite that does not exist yet, which is a much worse message
         * to have to debug than "this depends on itself". */
        if (r->done == 1) {
            kprintf("make: circular dependency: '%s' depends on itself\n", target);
            m->failed = 1;
            return -1;
        }
        if (r->done == 2) return 0;
        r->done = 1;
        for (int i = 0; i < r->npre; i++) strlcpy((*pre)[i], r->pre[i], FS_PATH_MAX);
        int rc = run_rule(m, target, r, *pre, r->npre, depth);
        r->done = 2;
        return rc;
    }

    /* No explicit rule. A pattern rule counts if its stem also produces
     * prerequisites that exist or can themselves be made. */
    for (int i = 0; i < m->nrule; i++) {
        Rule *pr = &m->rules[i];
        if (!pr->pattern) continue;
        char stem[FS_PATH_MAX];
        if (!stem_of(pr->target, target, stem, sizeof stem)) continue;

        int ok = 1;
        for (int k = 0; k < pr->npre; k++) {
            stem_into(pr->pre[k], stem, (*pre)[k], FS_PATH_MAX);
            if (!fs_lookup((*pre)[k]) && !rule_find(m, (*pre)[k])) { ok = 0; break; }
        }
        if (!ok) continue;
        return run_rule(m, target, pr, *pre, pr->npre, depth);
    }

    if (fs_lookup(target)) return 0;        /* a source file; nothing to do */

    kprintf("make: no rule to make target '%s'\n", target);
    m->failed = 1;
    return -1;
}

/* ---- the entry point ----------------------------------------------------- */

static int mk_depth;   /* a recipe that runs `make` must not do so forever */

int make_run(const char *path, const char *const *goals, int ngoal,
             int dry, int silent, char *err, uint32_t errcap) {
    if (mk_depth >= 8) {
        strlcpy(err, "make called itself too many times", errcap);
        return -1;
    }

    fs_node *f = fs_lookup(path);
    if (!f || f->is_dir || !fs_may(f, FS_PERM_READ)) {
        strlcpy(err, "cannot read the makefile", errcap);
        return -1;
    }

    MK *m = (MK *)kmalloc(sizeof(MK));
    char *text = (char *)kmalloc(f->size + 1);
    if (!m || !text) {
        if (m) kfree(m);
        if (text) kfree(text);
        strlcpy(err, "out of memory", errcap);
        return -1;
    }
    memset(m, 0, sizeof *m);
    if (f->size) memcpy(text, f->data, f->size);
    text[f->size] = 0;
    m->dry = dry;
    m->silent = silent;

    /* The stamp file belongs beside the makefile, not in whatever directory
     * happens to be current. */
    strlcpy(m->stampdir, path, sizeof m->stampdir);
    char *slash = strrchr(m->stampdir, '/');
    if (slash) *slash = 0;
    else m->stampdir[0] = 0;

    mk_depth++;
    int rc = 0;
    if (parse(m, text) != 0) {
        strlcpy(err, m->err[0] ? m->err : "could not read the makefile", errcap);
        rc = -1;
    } else {
        stamp_load(m);

        if (ngoal > 0) {
            for (int i = 0; i < ngoal && !m->failed; i++) build(m, goals[i], 0);
        } else {
            const char *first = 0;
            for (int i = 0; i < m->nrule; i++) {
                if (m->rules[i].pattern) continue;
                if (m->rules[i].target[0] == '.') continue;
                first = m->rules[i].target;
                break;
            }
            if (!first) { strlcpy(err, "no targets in this makefile", errcap); rc = -1; }
            else build(m, first, 0);
        }

        if (m->failed) rc = -1;
        else if (!m->built && !dry) kprintf("make: nothing to be done\n");
        stamp_save(m);
    }
    mk_depth--;

    for (int i = 0; i < MK_MAX_DEPTH; i++)
        if (m->scratch[i]) kfree(m->scratch[i]);
    for (Chunk *c = m->chunks; c; ) { Chunk *nx = c->next; kfree(c); c = nx; }
    kfree(text);
    kfree(m);
    return rc;
}
