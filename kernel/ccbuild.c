#include <stdint.h>
#include "ccbuild.h"
#include "cpp.h"
#include "boltcc.h"
#include "fs.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include <stdio.h>

/* ===========================================================================
 *  BoltOS  -  kernel/ccbuild.c
 *  Resolving a build target, and turning it into a translation unit.
 *
 *  Everything that builds C on BoltOS goes through here: `cc` (an explicit
 *  file list), `build` (a project directory) and the Code app's Run button.
 *  They differ only in where the file list comes from, and it matters that
 *  they do not differ in anything else -- a project that builds on the desktop
 *  and fails in the shell would be a bug nobody could find.
 *
 *  The build file is deliberately small. It is a list, not a language: there
 *  are no rules, no variables and no recipes, because BoltCC compiles a whole
 *  program in one pass and there is nothing for a dependency graph to skip.
 *  `make` belongs with separate compilation, and arrives with it.
 *
 *      # build.txt
 *      target hello            a target begins; everything until the next one
 *        src main.c util.c     sources, relative to the project directory
 *        include inc           an -I directory
 *        define DEBUG=1        a -D macro
 *
 *  Lines before the first `target` form a target named after the directory, so
 *  a one-target project is just `src main.c util.c`.
 * ===========================================================================*/

/* ------------------------------------------------------------- utilities --- */
static void sset(char *dst, uint32_t cap, const char *src) {
    uint32_t i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* dir + "/" + name, tolerating a trailing slash and an absolute `name`. */
static void pjoin(char *out, uint32_t cap, const char *dir, const char *name) {
    if (!dir || !dir[0] || name[0] == '/') { sset(out, cap, name); return; }
    uint32_t o = 0;
    for (const char *p = dir; *p && o < cap - 1; p++) out[o++] = *p;
    while (o > 1 && out[o - 1] == '/') o--;
    if (o < cap - 1) out[o++] = '/';
    for (const char *p = name; *p && o < cap - 1; p++) out[o++] = *p;
    out[o] = 0;
}

static const char *ext_of(const char *name) {
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    return dot ? dot : "";
}

/* Source files group by dialect: a translation unit is one language. */
static int dialect_of(const char *name) {
    const char *e = ext_of(name);
    if (!strcmp(e, ".c"))   return BCC_C;
    if (!strcmp(e, ".cpp") || !strcmp(e, ".cc") || !strcmp(e, ".cxx")) return BCC_CPP;
    if (!strcmp(e, ".cs"))  return BCC_CSHARP;
    return -1;
}

int ccb_is_source(const char *name) { return dialect_of(name) >= 0; }

int ccb_lang(const ccb_target_t *t) {
    if (!t->nsrc) return BCC_C;
    int d = dialect_of(t->src[0]);
    return d < 0 ? BCC_C : d;
}

void ccb_src_path(const ccb_target_t *t, int i, char *out, uint32_t cap) {
    if (i < 0 || i >= t->nsrc) { out[0] = 0; return; }
    pjoin(out, cap, t->dir, t->src[i]);
}

static char *slurp(const char *path) {
    fs_node *f = fs_lookup(path);
    if (!f || f->is_dir || !f->data) return 0;
    if (!fs_may(f, FS_PERM_READ)) return 0;
    char *b = (char *)kmalloc(f->size + 1);
    if (!b) return 0;
    memcpy(b, f->data, f->size);
    b[f->size] = 0;
    return b;
}

/* The last path component, which is what an unnamed target is called. */
static void basename_of(const char *path, char *out, uint32_t cap) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/' && p[1]) b = p + 1;
    if (!*b || !b[0]) { sset(out, cap, "project"); return; }
    uint32_t i = 0;
    for (; b[i] && b[i] != '/' && i < cap - 1; i++) out[i] = b[i];
    out[i] = 0;
    if (!out[0]) sset(out, cap, "project");
}

/* ------------------------------------------------------- build file parse --- */
/* One whitespace-separated token; a '#' begins a comment that runs to the end
 * of the line. Returns 0 at the end of the line. */
static int next_tok(const char **p, char *out, uint32_t cap) {
    const char *s = *p;
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    if (!*s || *s == '\n' || *s == '#') {
        while (*s && *s != '\n') s++;              /* eat the comment */
        *p = s;
        return 0;
    }
    uint32_t o = 0;
    while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n' && *s != '#') {
        if (o < cap - 1) out[o++] = *s;
        s++;
    }
    out[o] = 0;
    *p = s;
    return 1;
}

void ccb_add_source(ccb_target_t *t, const char *name) {
    if (t->nsrc < CCB_MAX_SRC) sset(t->src[t->nsrc++], CCB_SRC_MAX, name);
}
void ccb_add_include(ccb_target_t *t, const char *dir) {
    if (t->ninc < CCB_MAX_INC) pjoin(t->inc[t->ninc++], CCB_PATH_MAX, t->dir, dir);
}
void ccb_add_define(ccb_target_t *t, const char *nv) {
    if (t->ndef < CCB_MAX_DEF) sset(t->def[t->ndef++], 64, nv);
}
#define add_src ccb_add_source
#define add_inc ccb_add_include
#define add_def ccb_add_define

static void reset_lists(ccb_target_t *t) { t->nsrc = t->ninc = t->ndef = 0; }

/* Every source file in a directory, in the dialect of the first one found.
 * Mixing dialects in one unit is meaningless, so the rest are left out. */
static void scan_dir(const char *dir, ccb_target_t *t) {
    fs_node *d = fs_lookup(dir);
    if (!d || !d->is_dir) return;
    int want = -1;
    for (fs_node *c = d->child; c; c = c->next) {
        if (c->is_dir) continue;
        int dia = dialect_of(c->name);
        if (dia < 0) continue;
        if (want < 0) want = dia;
        else if (dia != want) continue;
        add_src(t, c->name);
    }
    t->implicit = 1;
}

int ccb_list(const char *dir, char names[][CCB_NAME_MAX], int max) {
    char path[CCB_PATH_MAX];
    pjoin(path, sizeof path, dir, CCB_FILE);
    char *text = slurp(path);
    if (!text) return 0;

    int n = 0;
    const char *p = text;
    while (*p) {
        char kw[CCB_NAME_MAX];
        if (next_tok(&p, kw, sizeof kw) && !strcmp(kw, "target")) {
            char nm[CCB_NAME_MAX];
            if (next_tok(&p, nm, sizeof nm) && n < max) sset(names[n++], CCB_NAME_MAX, nm);
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    kfree(text);
    return n;
}

int ccb_load(const char *dir, const char *want, ccb_target_t *t,
             char *err, uint32_t errcap) {
    memset(t, 0, sizeof *t);
    sset(t->dir, CCB_PATH_MAX, dir ? dir : "");
    if (err && errcap) err[0] = 0;

    fs_node *d = fs_lookup(t->dir);
    if (!d || !d->is_dir) {
        if (err) snprintf(err, errcap, "no such directory: %s", t->dir);
        return -1;
    }

    char path[CCB_PATH_MAX];
    pjoin(path, sizeof path, t->dir, CCB_FILE);
    char *text = slurp(path);
    if (!text) {
        /* No build file: the directory itself is the target. */
        basename_of(t->dir, t->name, CCB_NAME_MAX);
        if (want && want[0] && strcmp(want, t->name)) {
            if (err) snprintf(err, errcap, "no %s in %s, so no target '%s'",
                              CCB_FILE, t->dir, want);
            return -1;
        }
        scan_dir(t->dir, t);
        if (!t->nsrc) {
            if (err) snprintf(err, errcap, "no source files in %s", t->dir);
            return -1;
        }
        return 0;
    }

    /* Lines before the first `target` are a target named after the directory. */
    basename_of(t->dir, t->name, CCB_NAME_MAX);
    int matched = (!want || !want[0] || !strcmp(want, t->name));
    int got = 0;

    const char *p = text;
    int bad = 0;
    while (*p && !bad) {
        char kw[CCB_NAME_MAX];
        if (next_tok(&p, kw, sizeof kw)) {
            if (!strcmp(kw, "target")) {
                if (matched && got) break;         /* the wanted target is complete */
                char nm[CCB_NAME_MAX];
                if (!next_tok(&p, nm, sizeof nm)) {
                    if (err) snprintf(err, errcap, "%s: 'target' needs a name", CCB_FILE);
                    bad = 1;
                    break;
                }
                reset_lists(t);
                sset(t->name, CCB_NAME_MAX, nm);
                matched = (!want || !want[0]) ? 1 : !strcmp(want, nm);
            } else if (!strcmp(kw, "src") || !strcmp(kw, "source")) {
                char a[CCB_SRC_MAX];
                while (next_tok(&p, a, sizeof a)) { add_src(t, a); got = 1; }
            } else if (!strcmp(kw, "include")) {
                char a[CCB_PATH_MAX];
                while (next_tok(&p, a, sizeof a)) add_inc(t, a);
            } else if (!strcmp(kw, "define")) {
                char a[64];
                while (next_tok(&p, a, sizeof a)) add_def(t, a);
            } else {
                if (err) snprintf(err, errcap, "%s: unknown directive '%s'", CCB_FILE, kw);
                bad = 1;
                break;
            }
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    kfree(text);
    if (bad) return -1;

    if (!matched) {
        if (err) snprintf(err, errcap, "no target '%s' in %s", want, path);
        return -1;
    }
    if (!t->nsrc) {
        if (err) snprintf(err, errcap, "target '%s' has no sources", t->name);
        return -1;
    }
    return 0;
}

void ccb_from_list(ccb_target_t *t, const char *const *files, int nfile) {
    memset(t, 0, sizeof *t);
    sset(t->name, CCB_NAME_MAX, nfile ? files[0] : "unit");
    for (int i = 0; i < nfile && i < CCB_MAX_SRC; i++)
        sset(t->src[t->nsrc++], CCB_SRC_MAX, files[i]);
}

/* ------------------------------------------------------- the unity build --- */
/* Preprocess each source, then concatenate. BoltCC has no separate compilation
 * or linking step, so "linking" here is textual: one unit, compiled once. A
 * banner comment marks each boundary, which is what a compile error in the
 * combined unit is traced back through. */
char *ccb_unit(const ccb_target_t *t, char *err, uint32_t errcap) {
    if (err && errcap) err[0] = 0;
    if (!t->nsrc) {
        if (err) snprintf(err, errcap, "nothing to build");
        return 0;
    }

    cpp_reset_defines();
    cpp_reset_include_dirs();
    if (t->dir[0]) cpp_add_include_dir(t->dir);
    for (int i = 0; i < t->ninc; i++) cpp_add_include_dir(t->inc[i]);
    cpp_add_include_dir("/usr/include");
    cpp_add_include_dir("/include");
    for (int i = 0; i < t->ndef; i++) cpp_define_cli(t->def[i]);

    char *parts[CCB_MAX_SRC];
    for (int i = 0; i < t->nsrc; i++) parts[i] = 0;

    uint32_t total = 0;
    int failed = 0;
    for (int i = 0; i < t->nsrc && !failed; i++) {
        char path[CCB_PATH_MAX];
        ccb_src_path(t, i, path, sizeof path);
        char *raw = slurp(path);
        if (!raw) {
            if (err) snprintf(err, errcap, "cannot read %s", path);
            failed = 1;
            break;
        }
        parts[i] = cpp_run(raw, path, err, errcap);
        kfree(raw);
        if (!parts[i]) {
            if (err && !err[0]) snprintf(err, errcap, "preprocessing %s failed", path);
            failed = 1;
            break;
        }
        total += (uint32_t)strlen(parts[i]) + CCB_PATH_MAX + 16;
    }

    char *unit = 0;
    if (!failed) {
        unit = (char *)kmalloc(total + 1);
        if (!unit) {
            if (err) snprintf(err, errcap, "out of memory");
            failed = 1;
        }
    }
    if (!failed) {
        uint32_t o = 0;
        for (int i = 0; i < t->nsrc; i++) {
            char path[CCB_PATH_MAX];
            ccb_src_path(t, i, path, sizeof path);
            /* snprintf reports what it *would* have written, so a truncated
             * banner must not be allowed to walk `o` past the buffer. */
            int n = snprintf(unit + o, total - o, "/* --- %s --- */\n", path);
            if (n > 0) { o += (uint32_t)n; if (o > total) o = total; }
            for (const char *s = parts[i]; *s && o < total; s++) unit[o++] = *s;
            if (o < total) unit[o++] = '\n';
        }
        unit[o] = 0;
    }

    for (int i = 0; i < t->nsrc; i++) if (parts[i]) kfree(parts[i]);
    if (failed) { if (unit) kfree(unit); return 0; }
    return unit;
}
