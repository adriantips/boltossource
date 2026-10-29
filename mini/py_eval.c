/* ===========================================================================
 *  BoltOS Mini  -  mini/py_eval.c
 *
 *  Running the tree py_parse.c made. Statements return how they ended
 *  (normally, break, continue, return); expressions return a value. A
 *  raised exception is a longjmp to the innermost try (py.h's Handler),
 *  which puts the call stack back the way it was when the try began.
 *
 *  Calls bind arguments the way Python does -- positional, keyword,
 *  defaults, keyword-only, *args and **kwargs -- into a new frame whose
 *  slots the parser numbered. Attribute lookup follows the class's MRO,
 *  with properties, static and class methods, __getattr__, and super().
 *
 *  A small record per call (function, file, line) is what the traceback
 *  is made from, and the line an error leaves for the editor to show.
 * ===========================================================================*/
#include <stdint.h>
#include "py.h"
#include "string.h"
#include "stdio.h"
#include "math.h"
#define MATHX_REPLACE
#include "mathx.h"

enum { F_NORMAL, F_BREAK, F_CONTINUE, F_RETURN };

typedef struct { Str *fn, *file; int line; } CallRec;
#define MAXCALLS 2000
static CallRec calls[MAXCALLS];
static int ncalls;
static V ret_val;
static uint32_t ticks;
Frame *py_cur_frame;
static List *handling;                                     /* the exceptions being handled, innermost last */

static V eval(Node *n, Frame *f);
static int exec(Node *n, Frame *f);

void py_set_line(int line) { if (ncalls) calls[ncalls - 1].line = line; }
int py_line(void) { return ncalls ? calls[ncalls - 1].line : 0; }

static inline void tick(void) { if ((++ticks & 1023) == 0) rt_check(); }

/* ---- the traceback ------------------------------------------------------------------------------- */
static const char *src_of(Str *file) {
    for (int i = 0; i < P->nfiles; i++) if (P->files[i] == file) return P->srcs[i];
    return 0;
}

static void src_line(SB *sb, Str *file, int line) {
    const char *s = src_of(file);
    if (!s || line < 1) return;
    for (int l = 1; l < line && *s; s++) if (*s == '\n') l++;
    while (*s == ' ' || *s == '\t') s++;
    const char *e = s;
    while (*e && *e != '\n' && *e != '\r') e++;
    if (e == s) return;
    sb_adds(sb, "    "); sb_add(sb, s, (uint32_t)(e - s)); sb_addc(sb, '\n');
}

Str *traceback_now(void) {
    SB sb; sb_new(&sb, 256);
    int from = ncalls > 30 ? ncalls - 30 : 0;
    if (from) { char b[80]; snprintf(b, sizeof b, "  ... %d calls further out ...\n", from); sb_adds(&sb, b); }
    for (int i = from; i < ncalls; i++) {
        char b[300];
        snprintf(b, sizeof b, "  File \"%s\", line %d, in %s\n", calls[i].file ? calls[i].file->s : "?", calls[i].line,
                 calls[i].fn ? calls[i].fn->s : "?");
        sb_adds(&sb, b);
        src_line(&sb, calls[i].file, calls[i].line);
    }
    return sb_str(&sb);
}

static int exc_line(V exc) {
    /* the innermost line in the program's own file */
    (void)exc;
    for (int i = ncalls - 1; i >= 0; i--) if (calls[i].file == P->main_file) return calls[i].line;
    return 0;
}

static Str *exc_text(V exc) {
    Inst *e = (Inst *)exc.u.o;
    SB sb; sb_new(&sb, 256);
    V tb;
    sb_adds(&sb, "Traceback (most recent call last):\n");
    if (dict_get_str(e->dict, P->s_tb, &tb) && is_obj(tb, O_STR)) sb_addstr(&sb, as_str(tb));
    sb_addstr(&sb, e->cls->name);
    Str *msg = to_str(exc);
    if (msg->len) { sb_adds(&sb, ": "); sb_addstr(&sb, msg); }
    return sb_str(&sb);
}

void py_uncaught(V exc) {
    int line = 0;
    V ln;
    Inst *e = (Inst *)exc.u.o;
    if (e->cls == P->e_syntax && dict_get_str(e->dict, intern_c("lineno"), &ln)) line = (int)ln.u.i;
    else line = exc_line(exc);
    rt_fail(line, "%s", exc_text(exc)->s);
}

/* ---- names ---------------------------------------------------------------------------------------- */
static void name_error(Str *s) {
    raise_new(P->e_name, "name '%s' is not defined", s->s);
}

static V load_global(Frame *f, Str *s) {
    V v;
    if (dict_get_str(f->globals, s, &v)) return v;
    if (dict_get_str(P->builtins, s, &v)) return v;
    name_error(s);
    return vnone();
}

static Frame *outer(Frame *f, int depth) {
    while (depth-- > 0 && f) f = f->env;
    return f;
}

static V load_name(Node *n, Frame *f) {
    V v;
    switch (n->scope) {
    case S_LOCAL:
        v = f->slots[n->slot];
        if (v.t == V_UNBOUND) raise_new(P->e_unbound, "cannot access local variable '%s' where it is not associated with a value", n->s->s);
        return v;
    case S_OUTER: {
        Frame *o = outer(f, n->depth);
        if (!o || !o->slots) name_error(n->s);
        v = o->slots[n->slot];
        if (v.t == V_UNBOUND) raise_new(P->e_name, "cannot access free variable '%s' where it is not associated with a value in enclosing scope", n->s->s);
        return v;
    }
    case S_CLASS:
        if (f->ns && dict_get_str(f->ns, n->s, &v)) return v;
        return load_global(f, n->s);
    default:
        return load_global(f, n->s);
    }
}

static void store_name(Node *n, Frame *f, V v) {
    switch (n->scope) {
    case S_LOCAL: f->slots[n->slot] = v; return;
    case S_OUTER: { Frame *o = outer(f, n->depth); if (o && o->slots) o->slots[n->slot] = v; return; }
    case S_CLASS: if (f->ns) { dict_set_str(f->ns, n->s, v); return; } /* fall through */
    default: dict_set_str(f->globals, n->s, v);
    }
}

static void del_name(Node *n, Frame *f) {
    switch (n->scope) {
    case S_LOCAL:
        if (f->slots[n->slot].t == V_UNBOUND) raise_new(P->e_unbound, "cannot access local variable '%s' where it is not associated with a value", n->s->s);
        f->slots[n->slot].t = V_UNBOUND; return;
    case S_OUTER: { Frame *o = outer(f, n->depth); if (o && o->slots) o->slots[n->slot].t = V_UNBOUND; return; }
    case S_CLASS: if (f->ns && dict_del(f->ns, vobj(n->s))) return; /* fall through */
    default: if (!dict_del(f->globals, vobj(n->s))) name_error(n->s);
    }
}

/* ---- attributes ------------------------------------------------------------------------------------ */
static int class_lookup(Class *c, Str *name, V *out) {
    for (int i = 0; i < c->nmro; i++) if (dict_get_str(c->mro[i]->dict, name, out)) return 1;
    return 0;
}

static V bind_method(V fn, V self) {
    Bound *b = rt_alloc(sizeof(Bound));
    b->type = O_BOUND; b->self = self; b->fn = fn;
    return vobj(b);
}

int lookup_special(V obj, Str *name, V *out) {
    if (!is_obj(obj, O_INST)) return 0;
    V m;
    if (!class_lookup(((Inst *)obj.u.o)->cls, name, &m)) return 0;
    if (is_obj(m, O_FUNC) || is_obj(m, O_NATIVE)) *out = bind_method(m, obj);
    else if (is_obj(m, O_STATICM)) *out = ((Wrap *)m.u.o)->fn;
    else *out = m;
    return 1;
}

static V attr_error(V obj, Str *name) {
    if (is_obj(obj, O_MODULE)) raise_new(P->e_attr, "module '%s' has no attribute '%s'", ((Module *)obj.u.o)->name->s, name->s);
    if (is_obj(obj, O_CLASS)) raise_new(P->e_attr, "type object '%s' has no attribute '%s'", ((Class *)obj.u.o)->name->s, name->s);
    raise_new(P->e_attr, "'%s' object has no attribute '%s'", type_name(obj), name->s);
    return vnone();
}

static int attr_of_class_value(V m, V self, Class *cls, V *out) {
    if (is_obj(m, O_FUNC) || is_obj(m, O_NATIVE)) { *out = self.t == V_UNBOUND ? m : bind_method(m, self); return 1; }
    if (is_obj(m, O_STATICM)) { *out = ((Wrap *)m.u.o)->fn; return 1; }
    if (is_obj(m, O_CLASSM)) { *out = bind_method(((Wrap *)m.u.o)->fn, vobj(cls)); return 1; }
    if (is_obj(m, O_PROP)) {
        if (self.t == V_UNBOUND) { *out = m; return 1; }
        Prop *p = (Prop *)m.u.o;
        if (p->get.t == V_NONE) raise_new(P->e_attr, "property has no getter");
        *out = call1(p->get, self);
        return 1;
    }
    *out = m;
    return 1;
}

int getattr_opt(V obj, Str *name, V *out) {
    if (obj.t == V_OBJ) {
        switch (obj.u.o->type) {
        case O_INST: {
            Inst *in = (Inst *)obj.u.o;
            V m;
            int incls = class_lookup(in->cls, name, &m);
            if (incls && is_obj(m, O_PROP)) return attr_of_class_value(m, obj, in->cls, out);
            if (dict_get_str(in->dict, name, out)) return 1;
            if (incls) return attr_of_class_value(m, obj, in->cls, out);
            if (name == P->s_class) { *out = vobj(in->cls); return 1; }
            if (name == P->s_dict) { *out = vobj(in->dict); return 1; }
            if (in->cls->is_exc && name == P->s_args) { *out = in->native; return 1; }
            int found;
            if (in->cls->builtin && in->native.t != V_NONE) { V r = method_of(in->native, name, &found); if (found) { *out = r; return 1; } }
            V ga;
            if (class_lookup(in->cls, P->s_getattr, &ga) && name != P->s_getattr) { *out = call2(ga, obj, vobj(name)); return 1; }
            return 0;
        }
        case O_CLASS: {
            Class *c = (Class *)obj.u.o;
            V m;
            if (class_lookup(c, name, &m)) {
                if (is_obj(m, O_CLASSM)) { *out = bind_method(((Wrap *)m.u.o)->fn, obj); return 1; }
                if (is_obj(m, O_STATICM)) { *out = ((Wrap *)m.u.o)->fn; return 1; }
                *out = m; return 1;
            }
            if (name == P->s_name) { *out = vobj(c->name); return 1; }
            if (name == P->s_dict) { *out = vobj(c->dict); return 1; }
            if (!strcmp(name->s, "__mro__")) { Tuple *t = tuple_new((uint32_t)c->nmro); for (int i = 0; i < c->nmro; i++) t->v[i] = vobj(c->mro[i]); *out = vobj(t); return 1; }
            if (!strcmp(name->s, "__bases__")) { Tuple *t = tuple_new((uint32_t)c->nbases); for (int i = 0; i < c->nbases; i++) t->v[i] = vobj(c->bases[i]); *out = vobj(t); return 1; }
            int found;
            V r = method_of(obj, name, &found);         /* str.upper, dict.fromkeys ... */
            if (found) { *out = r; return 1; }
            return 0;
        }
        case O_MODULE: return dict_get_str(((Module *)obj.u.o)->dict, name, out);
        case O_SUPER: {
            Super *s = (Super *)obj.u.o;
            Class *start = is_obj(s->self, O_CLASS) ? (Class *)s->self.u.o : type_of(s->self);
            int i = 0;
            while (i < start->nmro && start->mro[i] != s->cls) i++;
            for (i++; i < start->nmro; i++) {
                V m;
                if (dict_get_str(start->mro[i]->dict, name, &m)) return attr_of_class_value(m, s->self, start, out);
            }
            if (name == P->s_init) { *out = native("object.__init__", 0); return 1; }
            return 0;
        }
        case O_FUNC:
            if (name == P->s_name) { *out = vobj(((Func *)obj.u.o)->name); return 1; }
            if (name == P->s_doc) { *out = vnone(); return 1; }
            break;
        case O_NATIVE:
            if (name == P->s_name) { *out = vstr(((Native *)obj.u.o)->name); return 1; }
            break;
        case O_PROP: {
            V prop_attr(V prop, Str *name, int *found);
            int found;
            V r = prop_attr(obj, name, &found);
            if (found) { *out = r; return 1; }
            return 0;
        }
        }
    }
    int found;
    V r = method_of(obj, name, &found);
    if (found) { *out = r; return 1; }
    if (name == P->s_class) { *out = vobj(type_of(obj)); return 1; }
    return 0;
}

V getattr(V obj, Str *name) {
    V v;
    if (getattr_opt(obj, name, &v)) return v;
    return attr_error(obj, name);
}

void setattr(V obj, Str *name, V val) {
    if (is_obj(obj, O_INST)) {
        Inst *in = (Inst *)obj.u.o;
        V m;
        if (class_lookup(in->cls, name, &m) && is_obj(m, O_PROP)) {
            Prop *p = (Prop *)m.u.o;
            if (p->set.t == V_NONE) raise_new(P->e_attr, "property '%s' of '%s' object has no setter", name->s, in->cls->name->s);
            call2(p->set, obj, val);
            return;
        }
        V sa;
        if (class_lookup(in->cls, P->s_setattr, &sa)) { V a[3] = { obj, vobj(name), val }; call(sa, a, 3, 0, 0, 0); return; }
        dict_set_str(in->dict, name, val);
        return;
    }
    if (is_obj(obj, O_CLASS)) { dict_set_str(((Class *)obj.u.o)->dict, name, val); return; }
    if (is_obj(obj, O_MODULE)) { dict_set_str(((Module *)obj.u.o)->dict, name, val); return; }
    if (is_obj(obj, O_FUNC)) return;
    raise_new(P->e_attr, "'%s' object has no attribute '%s'", type_name(obj), name->s);
}

/* ---- calls ------------------------------------------------------------------------------------------ */
static V call_func(Func *fn, V *a, int n, V *kwn, V *kwv, int nk);

V call(V fn, V *a, int n, V *kwn, V *kwv, int nk) {
    if (fn.t == V_OBJ) {
        switch (fn.u.o->type) {
        case O_FUNC: return call_func((Func *)fn.u.o, a, n, kwn, kwv, nk);
        case O_NATIVE: {
            Native *nt = (Native *)fn.u.o;
            if (!nt->fn) return vnone();
            if (nt->has_self) {
                V b[n + 1];
                b[0] = nt->self;
                for (int i = 0; i < n; i++) b[i + 1] = a[i];
                return nt->fn(b, n + 1, kwn, kwv, nk);
            }
            return nt->fn(a, n, kwn, kwv, nk);
        }
        case O_BOUND: {
            Bound *bm = (Bound *)fn.u.o;
            V b[n + 1];
            b[0] = bm->self;
            for (int i = 0; i < n; i++) b[i + 1] = a[i];
            return call(bm->fn, b, n + 1, kwn, kwv, nk);
        }
        case O_CLASS: {
            Class *c = (Class *)fn.u.o;
            V ctor;
            /* a built-in type called directly: int("5"), list(x)... (its own dict, not a subclass's) */
            if (dict_get_str(c->dict, P->s_ctor, &ctor)) return call(ctor, a, n, kwn, kwv, nk);
            Inst *in = rt_alloc(sizeof(Inst));
            in->type = O_INST; in->cls = c; in->dict = dict_new(); in->native = vnone();
            V self = vobj(in);
            if (c->is_exc) {
                Tuple *t = tuple_new((uint32_t)n);
                for (int i = 0; i < n; i++) t->v[i] = a[i];
                in->native = vobj(t);
            } else if (c->builtin == O_DICT) in->native = vobj(dict_new());
            else if (c->builtin == O_LIST) in->native = vobj(list_new(4));
            V init;
            if (class_lookup(c, P->s_init, &init) && (is_obj(init, O_FUNC) || is_obj(init, O_NATIVE))) {
                V b[n + 1];
                b[0] = self;
                for (int i = 0; i < n; i++) b[i + 1] = a[i];
                V r = call(init, b, n + 1, kwn, kwv, nk);
                if (r.t != V_NONE) raise_new(P->e_type, "__init__() should return None, not '%s'", type_name(r));
            } else if ((n || nk) && !c->is_exc && !c->builtin) raise_new(P->e_type, "%s() takes no arguments", c->name->s);
            return self;
        }
        case O_INST: {
            V m;
            if (lookup_special(fn, P->s_call, &m)) return call(m, a, n, kwn, kwv, nk);
            break;
        }
        case O_STATICM: return call(((Wrap *)fn.u.o)->fn, a, n, kwn, kwv, nk);
        }
    }
    raise_new(P->e_type, "'%s' object is not callable", type_name(fn));
    return vnone();
}

V call0(V fn) { return call(fn, 0, 0, 0, 0, 0); }
V call1(V fn, V x) { return call(fn, &x, 1, 0, 0, 0); }
V call2(V fn, V x, V y) { V a[2] = { x, y }; return call(fn, a, 2, 0, 0, 0); }

static Str *kw_name(V k) { return is_obj(k, O_STR) ? as_str(k) : 0; }

static int param_index(Node *d, Str *k) {
    int lim = d->npos + d->nkwonly;
    for (int i = 0; i < lim; i++) if (d->params[i] == k) return i;
    for (int i = 0; i < lim; i++) if (d->params[i]->len == k->len && !memcmp(d->params[i]->s, k->s, k->len)) return i;
    return -1;
}

static V run_body(Func *fn, Frame *fr) {
    Node *d = fn->def;
    if (d->k == N_LAMBDA) return eval(d->a, fr);
    int r = exec(d->a, fr);
    if (r == F_RETURN) { V v = ret_val; ret_val = vnone(); return v; }
    return vnone();
}

static V call_func(Func *fn, V *a, int n, V *kwn, V *kwv, int nk) {
    Node *d = fn->def;
    tick();
    if (ncalls >= MAXCALLS - 1 || rt_stack_left() < 192 * 1024) raise_new(P->e_recursion, "maximum recursion depth exceeded");
    Frame *fr = rt_alloc(sizeof(Frame));
    fr->env = fn->env; fr->globals = fn->globals; fr->fn = fn;
    int ns = d->nslots;
    fr->slots = rt_alloc(sizeof(V) * (size_t)(ns ? ns : 1));
    for (int i = 0; i < ns; i++) fr->slots[i].t = V_UNBOUND;
    int npos = d->npos;
    /* positional */
    int take = n < npos ? n : npos;
    for (int i = 0; i < take; i++) fr->slots[i] = a[i];
    int starslot = npos + d->nkwonly, dstarslot = starslot + d->star;
    if (n > npos) {
        if (!d->star) {
            raise_new(P->e_type, "%s() takes %d positional argument%s but %d %s given", fn->name->s, npos, npos == 1 ? "" : "s", n, n == 1 ? "was" : "were");
        }
        Tuple *t = tuple_new((uint32_t)(n - npos));
        for (int i = npos; i < n; i++) t->v[i - npos] = a[i];
        fr->slots[starslot] = vobj(t);
    } else if (d->star) fr->slots[starslot] = vobj(tuple_new(0));
    Dict *kwd = 0;
    if (d->dstar) { kwd = dict_new(); fr->slots[dstarslot] = vobj(kwd); }
    for (int i = 0; i < nk; i++) {
        Str *k = kw_name(kwn[i]);
        if (!k) raise_new(P->e_type, "keywords must be strings");
        int pi = param_index(d, k);
        if (pi >= 0) {
            if (fr->slots[pi].t != V_UNBOUND) raise_new(P->e_type, "%s() got multiple values for argument '%s'", fn->name->s, k->s);
            fr->slots[pi] = kwv[i];
        } else if (kwd) dict_set(kwd, kwn[i], kwv[i]);
        else raise_new(P->e_type, "%s() got an unexpected keyword argument '%s'", fn->name->s, k->s);
    }
    /* defaults, and what's still missing */
    int first_def = npos - fn->ndefaults;
    for (int i = 0; i < npos; i++) {
        if (fr->slots[i].t != V_UNBOUND) continue;
        if (i >= first_def) fr->slots[i] = fn->defaults[i - first_def];
        else {
            int missing = 0;
            for (int j = i; j < first_def; j++) if (fr->slots[j].t == V_UNBOUND) missing++;
            raise_new(P->e_type, "%s() missing %d required positional argument%s: '%s'", fn->name->s, missing, missing == 1 ? "" : "s", d->params[i]->s);
        }
    }
    for (int i = 0; i < d->nkwonly; i++) {
        int si = npos + i;
        if (fr->slots[si].t != V_UNBOUND) continue;
        V dv;
        if (fn->kwdefaults && dict_get_str(fn->kwdefaults, d->params[si], &dv)) fr->slots[si] = dv;
        else raise_new(P->e_type, "%s() missing 1 required keyword-only argument: '%s'", fn->name->s, d->params[si]->s);
    }
    CallRec *cr = &calls[ncalls++];
    cr->fn = fn->name; cr->file = d->file ? d->file : P->main_file; cr->line = d->line;
    Frame *saved = py_cur_frame;
    py_cur_frame = fr;
    V r = run_body(fn, fr);
    py_cur_frame = saved;
    ncalls--;
    return r;
}

/* the arguments of a call node, with * and ** spread out */
typedef struct { V *a; int n, cap; V *kn, *kv; int nk, kcap; } Args;

static void args_push(Args *x, V v) {
    if (x->n == x->cap) { int nc = x->cap * 2; V *na = rt_alloc(sizeof(V) * (size_t)nc); memcpy(na, x->a, sizeof(V) * (size_t)x->n); x->a = na; x->cap = nc; }
    x->a[x->n++] = v;
}
static void args_kw(Args *x, V k, V v) {
    if (x->nk == x->kcap) {
        int nc = x->kcap * 2;
        V *a = rt_alloc(sizeof(V) * (size_t)nc), *b = rt_alloc(sizeof(V) * (size_t)nc);
        memcpy(a, x->kn, sizeof(V) * (size_t)x->nk); memcpy(b, x->kv, sizeof(V) * (size_t)x->nk);
        x->kn = a; x->kv = b; x->kcap = nc;
    }
    x->kn[x->nk] = k; x->kv[x->nk++] = v;
}

static void eval_args(Node *call_node, Frame *f, Args *x, V *pa, V *pk, V *pv) {
    x->a = pa; x->cap = 8; x->n = 0; x->kn = pk; x->kv = pv; x->nk = 0; x->kcap = 8;   /* pa has 8 free, pk and pv 8 */
    for (int i = 0; i < call_node->nk; i++) {
        Node *k = call_node->kids[i];
        if (k->k == N_STAR) {
            V it = iter_of(eval(k->a, f)), v;
            while (iter_next(it, &v)) args_push(x, v);
        } else if (k->k == N_KWARG) args_kw(x, vobj(k->s), eval(k->a, f));
        else if (k->k == N_DSTAR) {
            V d = eval(k->a, f);
            if (!is_obj(d, O_DICT)) raise_new(P->e_type, "argument after ** must be a mapping, not %s", type_name(d));
            uint32_t pos = 0; V kk, vv;
            while (dict_next(as_dict(d), &pos, &kk, &vv)) {
                if (!is_obj(kk, O_STR)) raise_new(P->e_type, "keywords must be strings");
                args_kw(x, vobj(intern(as_str(kk)->s, as_str(kk)->len)), vv);
            }
        } else args_push(x, eval(k, f));
    }
}

static V eval_call(Node *n, Frame *f) {
    V pa[9], pk[8], pv[8];
    Args x;
    Node *fn = n->a;
    if (fn->k == N_ATTR) {
        /* obj.method(...): no bound method made on the way */
        V obj = eval(fn->a, f);
        if (is_obj(obj, O_INST)) {
            Inst *in = (Inst *)obj.u.o;
            V m;
            if (!dict_get_str(in->dict, fn->s, &m)) {
                for (int i = 0; i < in->cls->nmro; i++) {
                    if (dict_get_str(in->cls->mro[i]->dict, fn->s, &m)) {
                        if (is_obj(m, O_FUNC)) {
                            eval_args(n, f, &x, pa + 1, pk, pv);
                            /* self goes in front: pa[0] is free when the array is still ours */
                            V *a = x.a == pa + 1 ? pa : rt_alloc(sizeof(V) * (size_t)(x.n + 1));
                            if (a != pa) memcpy(a + 1, x.a, sizeof(V) * (size_t)x.n);
                            a[0] = obj;
                            py_set_line(n->line);
                            return call_func((Func *)m.u.o, a, x.n + 1, x.kn, x.kv, x.nk);
                        }
                        goto generic;
                    }
                }
            }
        }
    generic:;
        V callee = getattr(obj, fn->s);
        eval_args(n, f, &x, pa, pk, pv);
        py_set_line(n->line);
        return call(callee, x.a, x.n, x.kn, x.kv, x.nk);
    }
    V callee = eval(fn, f);
    eval_args(n, f, &x, pa, pk, pv);
    py_set_line(n->line);
    return call(callee, x.a, x.n, x.kn, x.kv, x.nk);
}

/* ---- iteration --------------------------------------------------------------------------------------- */
static Iter *iter_new(int kind, V src) {
    Iter *it = rt_alloc(sizeof(Iter));
    it->type = O_ITER; it->kind = (uint8_t)kind; it->src = src;
    return it;
}

V iter_of(V v) {
    if (v.t == V_OBJ) {
        switch (v.u.o->type) {
        case O_LIST: case O_TUPLE: return vobj(iter_new(IT_SEQ, v));
        case O_STR: return vobj(iter_new(IT_STR, v));
        case O_RANGE: {
            Range *r = (Range *)v.u.o;
            Iter *it = iter_new(IT_RANGE, v);
            it->i = r->start; it->n = r->stop; it->step = r->step;
            return vobj(it);
        }
        case O_DICT: case O_SET: return vobj(iter_new(IT_DICT, v));
        case O_ITER: return v;
        case O_FILE: {
            V lines = call0(getattr(v, intern_c("readlines")));
            return vobj(iter_new(IT_SEQ, lines));
        }
        case O_INST: {
            V m;
            Inst *in = (Inst *)v.u.o;
            if (lookup_special(v, P->s_iter, &m)) {
                V r = call0(m);
                if (is_obj(r, O_INST)) return vobj(iter_new(IT_USER, r));
                return iter_of(r);
            }
            if (lookup_special(v, P->s_getitem, &m)) { Iter *it = iter_new(IT_USER, v); it->fn = m; it->kind = IT_COUNT; return vobj(it); }
            if (in->cls->builtin && in->native.t != V_NONE) return iter_of(in->native);
            break;
        }
        }
    }
    raise_new(P->e_type, "'%s' object is not iterable", type_name(v));
    return vnone();
}

int iter_next(V itv, V *out) {
    Iter *it = (Iter *)itv.u.o;
    tick();
    switch (it->kind) {
    case IT_SEQ: {
        V s = it->src;
        uint32_t len = is_obj(s, O_LIST) ? as_list(s)->len : ((Tuple *)s.u.o)->len;
        if ((uint64_t)it->i >= len) return 0;
        *out = is_obj(s, O_LIST) ? as_list(s)->v[it->i] : ((Tuple *)s.u.o)->v[it->i];
        it->i++;
        return 1;
    }
    case IT_STR: {
        Str *s = as_str(it->src);
        if ((uint64_t)it->i >= s->len) return 0;
        *out = vstrn(s->s + it->i, 1);
        it->i++;
        return 1;
    }
    case IT_RANGE:
        if (it->step > 0 ? it->i >= it->n : it->i <= it->n) return 0;
        *out = vint(it->i);
        it->i += it->step;
        return 1;
    case IT_DICT: {
        Dict *d = as_dict(it->src);
        uint32_t pos = (uint32_t)it->i;
        V k, v;
        if (it->n == 0) it->n = d->n + 1;                  /* remember the size: changing it is an error */
        if ((int64_t)d->n + 1 != it->n) raise_new(P->e_runtime, "dictionary changed size during iteration");
        if (!dict_next(d, &pos, &k, &v)) return 0;
        it->i = pos;
        if (it->dictpart == 0) *out = k;
        else if (it->dictpart == 1) *out = v;
        else { Tuple *t = tuple_new(2); t->v[0] = k; t->v[1] = v; *out = vobj(t); }
        return 1;
    }
    case IT_ENUM: {
        V x;
        if (!iter_next(it->src, &x)) return 0;
        Tuple *t = tuple_new(2);
        t->v[0] = vint(it->i++); t->v[1] = x;
        *out = vobj(t);
        return 1;
    }
    case IT_ZIP: {
        Tuple *t = tuple_new((uint32_t)it->nmore);
        for (int i = 0; i < it->nmore; i++) if (!iter_next(it->more[i], &t->v[i])) return 0;
        *out = vobj(t);
        return 1;
    }
    case IT_MAP: {
        V args[8];
        for (int i = 0; i < it->nmore && i < 8; i++) if (!iter_next(it->more[i], &args[i])) return 0;
        *out = call(it->fn, args, it->nmore, 0, 0, 0);
        return 1;
    }
    case IT_FILTER: {
        V x;
        while (iter_next(it->src, &x)) {
            if (it->fn.t == V_NONE ? truthy(x) : truthy(call1(it->fn, x))) { *out = x; return 1; }
        }
        return 0;
    }
    case IT_REV: {
        if (it->i < 0) return 0;
        V s = it->src;
        if (is_obj(s, O_STR)) *out = vstrn(as_str(s)->s + it->i, 1);
        else if (is_obj(s, O_RANGE)) { Range *r = (Range *)s.u.o; *out = vint(r->start + it->i * r->step); }
        else {
            uint32_t len = is_obj(s, O_LIST) ? as_list(s)->len : ((Tuple *)s.u.o)->len;
            if ((uint64_t)it->i >= len) { it->i = (int64_t)len - 1; if (it->i < 0) return 0; }
            *out = is_obj(s, O_LIST) ? as_list(s)->v[it->i] : ((Tuple *)s.u.o)->v[it->i];
        }
        it->i--;
        return 1;
    }
    case IT_COUNT: {
        /* the old protocol: __getitem__(0), (1)... until IndexError */
        Handler h; h.prev = py_handler; py_handler = &h;
        int saved = ncalls;
        if (rt_setjmp(&h.jb) == 0) {
            *out = call1(it->fn, vint(it->i));
            py_handler = h.prev;
            it->i++;
            return 1;
        }
        py_handler = h.prev; ncalls = saved;
        if (is_instance(py_exc, P->e_index) || is_instance(py_exc, P->e_stop)) return 0;
        raise(py_exc);
    }
    case IT_USER: {
        V m;
        if (!lookup_special(it->src, P->s_next, &m)) raise_new(P->e_type, "iter() returned non-iterator of type '%s'", type_name(it->src));
        Handler h; h.prev = py_handler; py_handler = &h;
        int saved = ncalls;
        if (rt_setjmp(&h.jb) == 0) {
            *out = call0(m);
            py_handler = h.prev;
            return 1;
        }
        py_handler = h.prev; ncalls = saved;
        if (is_instance(py_exc, P->e_stop)) return 0;
        raise(py_exc);
    }
    }
    return 0;
}

/* ---- items ---------------------------------------------------------------------------------------------- */
static int64_t index_of(V k, uint32_t len, const char *what) {
    int64_t i;
    if (!fits_i64(k, &i)) {
        if (is_obj(k, O_BIG)) raise_new(P->e_index, "cannot fit 'int' into an index-sized integer");
        V m;
        if (is_obj(k, O_INST) && lookup_special(k, intern_c("__index__"), &m)) return index_of(call0(m), len, what);
        raise_new(P->e_type, "%s indices must be integers or slices, not %s", what, type_name(k));
    }
    if (i < 0) i += len;
    if (i < 0 || i >= (int64_t)len) raise_new(P->e_index, "%s index out of range", what);
    return i;
}

/* start, stop, step of a slice over len items, as Python works them out */
int slice_bounds(Slice *s, int64_t len, int64_t *start, int64_t *stop, int64_t *step);
int slice_bounds(Slice *s, int64_t len, int64_t *start, int64_t *stop, int64_t *step) {
    int64_t st = 1;
    if (s->step.t != V_NONE && !fits_i64(s->step, &st)) raise_new(P->e_type, "slice indices must be integers or None");
    if (st == 0) raise_new(P->e_value, "slice step cannot be zero");
    int64_t lo, hi;
    if (s->lo.t == V_NONE) lo = st > 0 ? 0 : len - 1;
    else {
        if (!fits_i64(s->lo, &lo)) raise_new(P->e_type, "slice indices must be integers or None");
        if (lo < 0) { lo += len; if (lo < 0) lo = st > 0 ? 0 : -1; }
        else if (lo >= len) lo = st > 0 ? len : len - 1;
    }
    if (s->hi.t == V_NONE) hi = st > 0 ? len : -1;
    else {
        if (!fits_i64(s->hi, &hi)) raise_new(P->e_type, "slice indices must be integers or None");
        if (hi < 0) { hi += len; if (hi < 0) hi = st > 0 ? 0 : -1; }
        else if (hi >= len) hi = st > 0 ? len : len - 1;
    }
    *start = lo; *stop = hi; *step = st;
    if (st > 0) return hi > lo ? (int)((hi - lo + st - 1) / st) : 0;
    return lo > hi ? (int)((lo - hi - st - 1) / -st) : 0;
}

V get_item(V obj, V key) {
    if (obj.t == V_OBJ) {
        switch (obj.u.o->type) {
        case O_LIST: case O_TUPLE: {
            int lst = obj.u.o->type == O_LIST;
            uint32_t len = lst ? as_list(obj)->len : ((Tuple *)obj.u.o)->len;
            V *v = lst ? as_list(obj)->v : ((Tuple *)obj.u.o)->v;
            if (is_obj(key, O_SLICE)) {
                int64_t a, b, st;
                int n = slice_bounds((Slice *)key.u.o, len, &a, &b, &st);
                if (lst) { List *l = list_new((uint32_t)n); for (int i = 0; i < n; i++) l->v[i] = v[a + i * st]; l->len = (uint32_t)n; return vobj(l); }
                Tuple *t = tuple_new((uint32_t)n);
                for (int i = 0; i < n; i++) t->v[i] = v[a + i * st];
                return vobj(t);
            }
            return v[index_of(key, len, lst ? "list" : "tuple")];
        }
        case O_STR: {
            Str *s = as_str(obj);
            if (is_obj(key, O_SLICE)) {
                int64_t a, b, st;
                int n = slice_bounds((Slice *)key.u.o, s->len, &a, &b, &st);
                if (st == 1) return vstrn(s->s + a, (uint32_t)n);
                Str *r = str_new(0, (uint32_t)n);
                for (int i = 0; i < n; i++) r->s[i] = s->s[a + i * st];
                return vobj(r);
            }
            return vstrn(s->s + index_of(key, s->len, "string"), 1);
        }
        case O_DICT: {
            V v;
            if (dict_get(as_dict(obj), key, &v)) return v;
            raise(make_exc(P->e_key, key));
        }
        case O_RANGE: {
            Range *r = (Range *)obj.u.o;
            int64_t len = r->step > 0 ? (r->stop > r->start ? (r->stop - r->start + r->step - 1) / r->step : 0)
                                      : (r->start > r->stop ? (r->start - r->stop - r->step - 1) / -r->step : 0);
            if (is_obj(key, O_SLICE)) {
                int64_t a, b, st;
                int n = slice_bounds((Slice *)key.u.o, len, &a, &b, &st);
                List *l = list_new((uint32_t)n);
                for (int i = 0; i < n; i++) list_push(l, vint(r->start + (a + i * st) * r->step));
                return vobj(l);
            }
            return vint(r->start + index_of(key, (uint32_t)len, "range object") * r->step);
        }
        case O_INST: {
            V m;
            Inst *in = (Inst *)obj.u.o;
            if (lookup_special(obj, P->s_getitem, &m)) return call1(m, key);
            if (in->cls->builtin == O_DICT && is_obj(in->native, O_DICT)) {
                V v;
                if (dict_get(as_dict(in->native), key, &v)) return v;
                if (lookup_special(obj, P->s_missing, &m)) return call1(m, key);
                raise(make_exc(P->e_key, key));
            }
            if (in->native.t != V_NONE && in->cls->builtin) return get_item(in->native, key);
            break;
        }
        case O_CLASS: return obj;                          /* list[int] in an annotation */
        }
    }
    raise_new(P->e_type, "'%s' object is not subscriptable", type_name(obj));
    return vnone();
}

void set_item(V obj, V key, V val) {
    if (is_obj(obj, O_LIST)) {
        List *l = as_list(obj);
        if (is_obj(key, O_SLICE)) {
            int64_t a, b, st;
            int n = slice_bounds((Slice *)key.u.o, l->len, &a, &b, &st);
            List *src = as_list(seq_from(val));
            if (st != 1) {
                if ((int)src->len != n) raise_new(P->e_value, "attempt to assign sequence of size %u to extended slice of size %d", src->len, n);
                for (int i = 0; i < n; i++) l->v[a + i * st] = src->v[i];
                return;
            }
            if (b < a) b = a;
            uint32_t nl = l->len - (uint32_t)(b - a) + src->len;
            V *nv = rt_alloc(sizeof(V) * (nl ? nl : 1));
            memcpy(nv, l->v, sizeof(V) * (size_t)a);
            memcpy(nv + a, src->v, sizeof(V) * src->len);
            memcpy(nv + a + src->len, l->v + b, sizeof(V) * (size_t)(l->len - b));
            l->v = nv; l->len = nl; l->cap = nl ? nl : 1;
            return;
        }
        l->v[index_of(key, l->len, "list assignment")] = val;
        return;
    }
    if (is_obj(obj, O_DICT)) { dict_set(as_dict(obj), key, val); return; }
    if (is_obj(obj, O_INST)) {
        V m;
        Inst *in = (Inst *)obj.u.o;
        if (lookup_special(obj, P->s_setitem, &m)) { call2(m, key, val); return; }
        if (in->native.t != V_NONE && in->cls->builtin) { set_item(in->native, key, val); return; }
    }
    raise_new(P->e_type, "'%s' object does not support item assignment", type_name(obj));
}

static void del_item(V obj, V key) {
    if (is_obj(obj, O_LIST)) {
        List *l = as_list(obj);
        if (is_obj(key, O_SLICE)) {
            int64_t a, b, st;
            int n = slice_bounds((Slice *)key.u.o, l->len, &a, &b, &st);
            if (!n) return;
            /* keep what isn't in the slice */
            uint8_t *gone = rt_alloc_leaf(l->len);
            for (int i = 0; i < n; i++) gone[a + i * st] = 1;
            uint32_t k = 0;
            for (uint32_t i = 0; i < l->len; i++) if (!gone[i]) l->v[k++] = l->v[i];
            l->len = k;
            return;
        }
        int64_t i = index_of(key, l->len, "list assignment");
        memmove(l->v + i, l->v + i + 1, sizeof(V) * (size_t)(l->len - i - 1));
        l->len--;
        return;
    }
    if (is_obj(obj, O_DICT)) { if (!dict_del(as_dict(obj), key)) raise(make_exc(P->e_key, key)); return; }
    if (is_obj(obj, O_INST)) {
        V m;
        Inst *in = (Inst *)obj.u.o;
        if (lookup_special(obj, P->s_delitem, &m)) { call1(m, key); return; }
        if (in->native.t != V_NONE && in->cls->builtin) { del_item(in->native, key); return; }
    }
    raise_new(P->e_type, "'%s' object doesn't support item deletion", type_name(obj));
}

static int contains(V cont, V item) {
    if (cont.t == V_OBJ) {
        switch (cont.u.o->type) {
        case O_LIST: case O_TUPLE: {
            int lst = cont.u.o->type == O_LIST;
            uint32_t len = lst ? as_list(cont)->len : ((Tuple *)cont.u.o)->len;
            for (uint32_t i = 0; i < len; i++) {
                V *v = lst ? as_list(cont)->v : ((Tuple *)cont.u.o)->v;
                if (eq(v[i], item)) return 1;
            }
            return 0;
        }
        case O_STR: {
            if (!is_obj(item, O_STR)) raise_new(P->e_type, "'in <string>' requires string as left operand, not %s", type_name(item));
            Str *h = as_str(cont), *nd = as_str(item);
            if (!nd->len) return 1;
            for (uint32_t i = 0; i + nd->len <= h->len; i++) if (!memcmp(h->s + i, nd->s, nd->len)) return 1;
            return 0;
        }
        case O_DICT: case O_SET: return dict_get(as_dict(cont), item, 0);
        case O_RANGE: {
            Range *r = (Range *)cont.u.o;
            int64_t x;
            if (!fits_i64(item, &x)) { if (item.t == V_FLOAT && item.u.f == __builtin_floor(item.u.f)) x = (int64_t)item.u.f; else return 0; }
            if (r->step > 0 ? (x < r->start || x >= r->stop) : (x > r->start || x <= r->stop)) return 0;
            return (x - r->start) % r->step == 0;
        }
        case O_INST: {
            V m;
            if (lookup_special(cont, P->s_contains, &m)) return truthy(call1(m, item));
            Inst *in = (Inst *)cont.u.o;
            if (in->native.t != V_NONE && in->cls->builtin) return contains(in->native, item);
            break;
        }
        }
    }
    V it = iter_of(cont), x;
    while (iter_next(it, &x)) if (eq(x, item)) return 1;
    return 0;
}

/* ---- operators ----------------------------------------------------------------------------------------- */
static const char *const OPSYM[] = { "+", "-", "*", "/", "//", "%", "**", "<<", ">>", "&", "|", "^", "@" };
static const char *const OPNAME[] = { "add", "sub", "mul", "truediv", "floordiv", "mod", "pow", "lshift", "rshift", "and", "or", "xor", "matmul" };

static double fmod_py(double a, double b) {
    double m = fmod(a, b);
    if (m != 0 && ((m < 0) != (b < 0))) m += b;
    return m;
}

static V seq_repeat(V s, int64_t k) {
    if (k < 0) k = 0;
    if (is_obj(s, O_STR)) {
        Str *x = as_str(s);
        if ((uint64_t)x->len * (uint64_t)k > (64u << 20)) raise_new(P->e_memory, "the string would be too long");
        Str *r = str_new(0, (uint32_t)(x->len * k));
        for (int64_t i = 0; i < k; i++) memcpy(r->s + i * x->len, x->s, x->len);
        return vobj(r);
    }
    int lst = is_obj(s, O_LIST);
    uint32_t len = lst ? as_list(s)->len : ((Tuple *)s.u.o)->len;
    V *v = lst ? as_list(s)->v : ((Tuple *)s.u.o)->v;
    if ((uint64_t)len * (uint64_t)k > (16u << 20)) raise_new(P->e_memory, "the list would be too long");
    uint32_t n = (uint32_t)(len * k);
    if (lst) {
        List *l = list_new(n);
        for (int64_t i = 0; i < k; i++) memcpy(l->v + i * len, v, sizeof(V) * len);
        l->len = n;
        return vobj(l);
    }
    Tuple *t = tuple_new(n);
    for (int64_t i = 0; i < k; i++) memcpy(t->v + i * len, v, sizeof(V) * len);
    return vobj(t);
}

static int is_number(V v) { return v.t == V_INT || v.t == V_BOOL || v.t == V_FLOAT || is_obj(v, O_BIG); }

V binop(int op, V a, V b) {
    /* small integers first: the common case */
    if ((a.t == V_INT || a.t == V_BOOL) && (b.t == V_INT || b.t == V_BOOL)) {
        int64_t x = a.u.i, y = b.u.i, r;
        switch (op) {
        case B_ADD: if (!__builtin_add_overflow(x, y, &r)) return vint(r); break;
        case B_SUB: if (!__builtin_sub_overflow(x, y, &r)) return vint(r); break;
        case B_MUL: if (!__builtin_mul_overflow(x, y, &r)) return vint(r); break;
        }
    }
    if (is_int(a) && is_int(b)) {
        switch (op) {
        case B_ADD: return int_add(a, b);
        case B_SUB: return int_sub(a, b);
        case B_MUL: return int_mul(a, b);
        case B_DIV: {
            double y = int_to_double(b);
            if (y == 0) raise_new(P->e_zero, "division by zero");
            if (a.t != V_OBJ && b.t != V_OBJ && a.u.i % b.u.i == 0 && a.u.i > -(1ll << 53) && a.u.i < (1ll << 53)) return vfloat((double)(a.u.i / b.u.i));
            return vfloat(int_to_double(a) / y);
        }
        case B_FLOORDIV: return int_floordiv(a, b);
        case B_MOD: return int_mod(a, b);
        case B_POW:
            if (int_cmp(b, vint(0)) < 0) {
                double x = int_to_double(a);
                if (x == 0) raise_new(P->e_zero, "0.0 cannot be raised to a negative power");
                return vfloat(pow(x, int_to_double(b)));
            }
            return int_pow(a, b);
        case B_LSHIFT: { int64_t k; if (!fits_i64(b, &k)) raise_new(P->e_overflow, "shift count too large"); return int_shl(a, k); }
        case B_RSHIFT: { int64_t k; if (!fits_i64(b, &k)) return vint(int_cmp(a, vint(0)) < 0 ? -1 : 0); return int_shr(a, k); }
        case B_AND: case B_OR: case B_XOR: return int_bitop(a, b, op);
        }
    }
    if (is_number(a) && is_number(b) && op <= B_POW) {
        double x = a.t == V_FLOAT ? a.u.f : int_to_double(a), y = b.t == V_FLOAT ? b.u.f : int_to_double(b);
        switch (op) {
        case B_ADD: return vfloat(x + y);
        case B_SUB: return vfloat(x - y);
        case B_MUL: return vfloat(x * y);
        case B_DIV: if (y == 0) raise_new(P->e_zero, "float division by zero"); return vfloat(x / y);
        case B_FLOORDIV: if (y == 0) raise_new(P->e_zero, "float floor division by zero"); return vfloat(floor(x / y));
        case B_MOD: if (y == 0) raise_new(P->e_zero, "float modulo"); return vfloat(fmod_py(x, y));
        case B_POW:
            if (x == 0 && y < 0) raise_new(P->e_zero, "0.0 cannot be raised to a negative power");
            if (x < 0 && y != floor(y)) raise_new(P->e_value, "a negative number to a fractional power is a complex number, which isn't supported");
            return vfloat(pow(x, y));
        }
    }
    if (a.t == V_OBJ && b.t == V_OBJ) {
        int ta = a.u.o->type, tb = b.u.o->type;
        if (op == B_ADD && ta == tb && (ta == O_STR)) {
            Str *x = as_str(a), *y = as_str(b), *r = str_new(0, x->len + y->len);
            memcpy(r->s, x->s, x->len); memcpy(r->s + x->len, y->s, y->len);
            return vobj(r);
        }
        if (op == B_ADD && ta == tb && ta == O_LIST) {
            List *x = as_list(a), *y = as_list(b), *r = list_new(x->len + y->len);
            memcpy(r->v, x->v, sizeof(V) * x->len); memcpy(r->v + x->len, y->v, sizeof(V) * y->len);
            r->len = x->len + y->len;
            return vobj(r);
        }
        if (op == B_ADD && ta == tb && ta == O_TUPLE) {
            Tuple *x = (Tuple *)a.u.o, *y = (Tuple *)b.u.o, *r = tuple_new(x->len + y->len);
            memcpy(r->v, x->v, sizeof(V) * x->len); memcpy(r->v + x->len, y->v, sizeof(V) * y->len);
            return vobj(r);
        }
        if (ta == O_SET && tb == O_SET && (op == B_OR || op == B_AND || op == B_SUB || op == B_XOR)) {
            Dict *x = as_dict(a), *y = as_dict(b), *r = set_new();
            r->frozen = x->frozen;
            uint32_t pos = 0; V k;
            if (op == B_OR) { while (dict_next(x, &pos, &k, 0)) dict_set(r, k, vnone()); pos = 0; while (dict_next(y, &pos, &k, 0)) dict_set(r, k, vnone()); }
            else if (op == B_AND) { while (dict_next(x, &pos, &k, 0)) if (dict_get(y, k, 0)) dict_set(r, k, vnone()); }
            else if (op == B_SUB) { while (dict_next(x, &pos, &k, 0)) if (!dict_get(y, k, 0)) dict_set(r, k, vnone()); }
            else {
                while (dict_next(x, &pos, &k, 0)) if (!dict_get(y, k, 0)) dict_set(r, k, vnone());
                pos = 0; while (dict_next(y, &pos, &k, 0)) if (!dict_get(x, k, 0)) dict_set(r, k, vnone());
            }
            return vobj(r);
        }
        if (ta == O_DICT && tb == O_DICT && op == B_OR) {
            Dict *r = dict_new(); uint32_t pos = 0; V k, v;
            while (dict_next(as_dict(a), &pos, &k, &v)) dict_set(r, k, v);
            pos = 0; while (dict_next(as_dict(b), &pos, &k, &v)) dict_set(r, k, v);
            return vobj(r);
        }
    }
    if (op == B_MUL) {
        if ((is_obj(a, O_STR) || is_obj(a, O_LIST) || is_obj(a, O_TUPLE)) && is_int(b)) { int64_t k; if (!fits_i64(b, &k)) raise_new(P->e_overflow, "too many repeats"); return seq_repeat(a, k); }
        if ((is_obj(b, O_STR) || is_obj(b, O_LIST) || is_obj(b, O_TUPLE)) && is_int(a)) { int64_t k; if (!fits_i64(a, &k)) raise_new(P->e_overflow, "too many repeats"); return seq_repeat(b, k); }
    }
    if (op == B_MOD && is_obj(a, O_STR)) return vobj(percent_format(as_str(a), b));
    /* the classes' own: __add__, then the other's __radd__ */
    char nm[24];
    V m;
    if (is_obj(a, O_INST)) {
        snprintf(nm, sizeof nm, "__%s__", OPNAME[op]);
        if (lookup_special(a, intern_c(nm), &m)) { V r = call1(m, b); if (!(r.t == V_OBJ && r.u.o == (Obj *)0)) return r; }
    }
    if (is_obj(b, O_INST)) {
        snprintf(nm, sizeof nm, "__r%s__", OPNAME[op]);
        if (lookup_special(b, intern_c(nm), &m)) return call1(m, a);
    }
    if (op == B_ADD && (is_obj(a, O_STR) || is_obj(a, O_LIST) || is_obj(a, O_TUPLE)))
        raise_new(P->e_type, "can only concatenate %s (not \"%s\") to %s", type_name(a), type_name(b), type_name(a));
    raise_new(P->e_type, "unsupported operand type(s) for %s: '%s' and '%s'", OPSYM[op], type_name(a), type_name(b));
    return vnone();
}

/* globals() and locals(), for py_lib.c */
V py_globals(void);
V py_globals(void) { return vobj(py_cur_frame->globals); }
V py_locals(void);
V py_locals(void) {
    Frame *f = py_cur_frame;
    if (!f->fn || !f->slots) return vobj(f->ns ? f->ns : f->globals);
    Dict *d = dict_new();
    Node *def = f->fn->def;
    for (int i = 0; i < def->nslots; i++) if (f->slots[i].t != V_UNBOUND) dict_set_str(d, def->slotnames[i], f->slots[i]);
    return vobj(d);
}

static V unop(int op, V a) {
    switch (a.t) {
    case V_INT: case V_BOOL:
        if (op == U_NEG) return int_neg(vint(a.u.i));
        if (op == U_POS) return vint(a.u.i);
        return vint(~a.u.i);
    case V_FLOAT:
        if (op == U_NEG) return vfloat(-a.u.f);
        if (op == U_POS) return a;
        break;
    }
    if (is_obj(a, O_BIG)) {
        if (op == U_NEG) return int_neg(a);
        if (op == U_POS) return a;
        return int_sub(int_neg(a), vint(1));
    }
    V m;
    static const char *const nm[3] = { "__neg__", "__pos__", "__invert__" };
    if (is_obj(a, O_INST) && lookup_special(a, intern_c(nm[op]), &m)) return call0(m);
    raise_new(P->e_type, "bad operand type for unary %s: '%s'", op == U_NEG ? "-" : op == U_POS ? "+" : "~", type_name(a));
    return vnone();
}

static int cmp_op(int op, V a, V b) {
    switch (op) {
    case C_IN: return contains(b, a);
    case C_NOTIN: return !contains(b, a);
    case C_IS: return a.t == b.t && (a.t == V_OBJ ? a.u.o == b.u.o : a.t == V_FLOAT ? a.u.f == b.u.f : a.u.i == b.u.i);
    case C_ISNOT: return !cmp_op(C_IS, a, b);
    case C_EQ: if (a.t == V_INT && b.t == V_INT) return a.u.i == b.u.i; return eq(a, b);
    case C_NE: if (a.t == V_INT && b.t == V_INT) return a.u.i != b.u.i; return !eq(a, b);
    }
    if (a.t == V_INT && b.t == V_INT) {
        switch (op) {
        case C_LT: return a.u.i < b.u.i;
        case C_LE: return a.u.i <= b.u.i;
        case C_GT: return a.u.i > b.u.i;
        default: return a.u.i >= b.u.i;
        }
    }
    return compare(a, b, op);
}

/* ---- assignment ------------------------------------------------------------------------------------------ */
static void assign(Node *t, V v, Frame *f) {
    switch (t->k) {
    case N_NAME: store_name(t, f, v); return;
    case N_ATTR: setattr(eval(t->a, f), t->s, v); return;
    case N_INDEX: {
        V obj = eval(t->a, f);
        V key = t->b->k == N_SLICE ? eval(t->b, f) : eval(t->b, f);
        set_item(obj, key, v);
        return;
    }
    case N_TUPLE: case N_LIST: {
        /* unpacking, with at most one *rest */
        int star = -1;
        for (int i = 0; i < t->nk; i++) if (t->kids[i]->k == N_STAR) { if (star >= 0) raise_new(P->e_syntax, "multiple starred expressions in assignment"); star = i; }
        V *items; uint32_t n;
        if (is_obj(v, O_TUPLE)) { items = ((Tuple *)v.u.o)->v; n = ((Tuple *)v.u.o)->len; }
        else if (is_obj(v, O_LIST)) { items = as_list(v)->v; n = as_list(v)->len; }
        else { List *l = as_list(seq_from(v)); items = l->v; n = l->len; }
        if (star < 0) {
            if (n != (uint32_t)t->nk) {
                if (n > (uint32_t)t->nk) raise_new(P->e_value, "too many values to unpack (expected %d)", t->nk);
                raise_new(P->e_value, "not enough values to unpack (expected %d, got %u)", t->nk, n);
            }
            if (t->nk == 2 && n == 2) { V x = items[0], y = items[1]; assign(t->kids[0], x, f); assign(t->kids[1], y, f); return; }
            V copy[n ? n : 1];
            memcpy(copy, items, sizeof(V) * n);
            for (int i = 0; i < t->nk; i++) assign(t->kids[i], copy[i], f);
            return;
        }
        int before = star, after = t->nk - star - 1;
        if ((int)n < before + after) raise_new(P->e_value, "not enough values to unpack (expected at least %d, got %u)", before + after, n);
        List *rest = list_new(n - (uint32_t)(before + after));
        for (uint32_t i = (uint32_t)before; i < n - (uint32_t)after; i++) list_push(rest, items[i]);
        V *copy = rt_alloc(sizeof(V) * (n ? n : 1));
        memcpy(copy, items, sizeof(V) * n);
        for (int i = 0; i < before; i++) assign(t->kids[i], copy[i], f);
        assign(t->kids[star]->a, vobj(rest), f);
        for (int i = 0; i < after; i++) assign(t->kids[star + 1 + i], copy[n - (uint32_t)after + (uint32_t)i], f);
        return;
    }
    case N_STAR: raise_new(P->e_syntax, "starred assignment target must be in a list or tuple");
    }
    raise_new(P->e_syntax, "cannot assign to that");
}

/* ---- making functions and classes ------------------------------------------------------------------------- */
static Frame *def_env(Frame *f) { return f->ns ? f->env : f; }

static V make_func(Node *d, Frame *f) {
    Func *fn = rt_alloc(sizeof(Func));
    fn->type = O_FUNC;
    fn->name = d->s;
    fn->def = d;
    fn->env = def_env(f);
    fn->globals = f->globals;
    if (d->ndef) {
        fn->defaults = rt_alloc(sizeof(V) * (size_t)d->ndef);
        for (int i = 0; i < d->ndef; i++) fn->defaults[i] = eval(d->defaults[i], f);
        fn->ndefaults = d->ndef;
    }
    for (int i = 0; i < d->nkwonly; i++) {
        if (!d->kwdefs[i]) continue;
        if (!fn->kwdefaults) fn->kwdefaults = dict_new();
        dict_set_str(fn->kwdefaults, d->params[d->npos + i], eval(d->kwdefs[i], f));
    }
    return vobj(fn);
}

static V decorate(Node *decs, V v, Frame *f) {
    if (!decs) return v;
    for (int i = decs->nk - 1; i >= 0; i--) v = call1(eval(decs->kids[i], f), v);
    return v;
}

static V make_class(Node *n, Frame *f) {
    int nb = n->nk;
    Class *bases[16];
    if (nb > 16) nb = 16;
    for (int i = 0; i < nb; i++) {
        V b = eval(n->kids[i], f);
        if (!is_obj(b, O_CLASS)) raise_new(P->e_type, "bases must be classes, not %s", type_name(b));
        bases[i] = (Class *)b.u.o;
        Class *bc = bases[i];
        if (bc == P->t_int || bc == P->t_float || bc == P->t_str || bc == P->t_tuple || bc == P->t_bool || bc == P->t_set)
            raise_new(P->e_type, "classes can't be based on '%s' here yet", bc->name->s);
    }
    if (!nb) { bases[0] = P->t_object; nb = 1; }
    Class *c = class_new(n->s->s, 0, 0);
    if (class_set_bases(c, bases, nb)) raise_new(P->e_type, "Cannot create a consistent method resolution order (MRO)");
    Frame *cf = rt_alloc(sizeof(Frame));
    cf->ns = c->dict; cf->env = def_env(f); cf->globals = f->globals;
    dict_set_str(c->dict, P->s_module, vobj(P->s_main));
    dict_set_str(c->dict, intern_c("__qualname__"), vobj(n->s));
    exec(n->a, cf);
    /* the functions in it learn their class, for super() */
    uint32_t pos = 0; V k, v;
    while (dict_next(c->dict, &pos, &k, &v)) {
        V fnv = v;
        if (is_obj(v, O_STATICM) || is_obj(v, O_CLASSM)) fnv = ((Wrap *)v.u.o)->fn;
        if (is_obj(v, O_PROP)) { Prop *pr = (Prop *)v.u.o; if (is_obj(pr->get, O_FUNC)) ((Func *)pr->get.u.o)->owner = c; if (is_obj(pr->set, O_FUNC)) ((Func *)pr->set.u.o)->owner = c; }
        if (is_obj(fnv, O_FUNC)) ((Func *)fnv.u.o)->owner = c;
    }
    return decorate(n->d, vobj(c), f);
}

/* super() with no arguments: the class of the function we're in, and its first argument */
V py_super0(void);
V py_super0(void) {
    Frame *f = py_cur_frame;
    while (f && (!f->fn || !f->fn->owner)) f = f->env;
    if (!f || !f->fn || f->fn->def->nparams < 1) raise_new(P->e_runtime, "super(): no arguments");
    Super *s = rt_alloc(sizeof(Super));
    s->type = O_SUPER; s->cls = f->fn->owner; s->self = f->slots[0];
    return vobj(s);
}

/* ---- imports --------------------------------------------------------------------------------------------- */
Module *import_module(Str *name, int line) {
    (void)line;
    V got;
    if (dict_get_str(P->modules, name, &got)) return (Module *)got.u.o;
    Module *m = builtin_module(name);
    if (!m) {
        /* the program's own: name.py next to it */
        char path[300];
        const char *dir = rt_cwd();
        snprintf(path, sizeof path, "%s/%s.py", strcmp(dir, "/") ? dir : "", name->s);
        for (char *q = path + strlen(dir) + 1; *q; q++) if (*q == '.' && strcmp(q, ".py")) *q = '/';
        char *src; uint32_t len; const char *why;
        if (rt_file_read(path, &src, &len, &why)) raise_new(P->e_notfound, "No module named '%s'", name->s);
        m = rt_alloc(sizeof(Module));
        m->type = O_MODULE; m->name = name; m->dict = dict_new();
        dict_set_str(P->modules, name, vobj(m));
        Str *file = str_c(path);
        if (P->nfiles < 16) { P->files[P->nfiles] = file; P->srcs[P->nfiles++] = src; }
        Node *prog = parse_program(src, file);
        dict_set_str(m->dict, P->s_name, vobj(name));
        Frame *mf = rt_alloc(sizeof(Frame));
        mf->globals = m->dict;
        CallRec *cr = &calls[ncalls++];
        cr->fn = intern_c("<module>"); cr->file = file; cr->line = 1;
        Frame *saved = py_cur_frame;
        exec(prog, mf);
        py_cur_frame = saved;
        ncalls--;
        return m;
    }
    dict_set_str(P->modules, name, vobj(m));
    return m;
}

/* A module written in Python and built in (py_lib.c's collections, itertools...). */
Module *import_source(Module *m, const char *src);
Module *import_source(Module *m, const char *src) {
    Str *file = str_c(m->name->s);
    Node *prog = parse_program(src, file);
    Frame *mf = rt_alloc(sizeof(Frame));
    mf->globals = m->dict;
    CallRec *cr = &calls[ncalls++];
    cr->fn = intern_c("<module>"); cr->file = file; cr->line = 1;
    Frame *saved = py_cur_frame;
    exec(prog, mf);
    py_cur_frame = saved;
    ncalls--;
    return m;
}

/* ---- expressions ------------------------------------------------------------------------------------------ */
static V eval_comp(Node *n, Frame *f);

static V build_seq(Node *n, Frame *f, int kind) {
    List *l = list_new((uint32_t)n->nk);
    for (int i = 0; i < n->nk; i++) {
        Node *k = n->kids[i];
        if (k->k == N_STAR) { V it = iter_of(eval(k->a, f)), v; while (iter_next(it, &v)) list_push(l, v); }
        else list_push(l, eval(k, f));
    }
    if (kind == O_LIST) return vobj(l);
    if (kind == O_TUPLE) { Tuple *t = tuple_new(l->len); memcpy(t->v, l->v, sizeof(V) * l->len); return vobj(t); }
    Dict *s = set_new();
    for (uint32_t i = 0; i < l->len; i++) dict_set(s, l->v[i], vnone());
    return vobj(s);
}

static V eval(Node *n, Frame *f) {
    switch (n->k) {
    case N_CONST: return n->v;
    case N_NAME: return load_name(n, f);
    case N_BIN: {
        V a = eval(n->a, f), b = eval(n->b, f);
        if (a.t == V_INT && b.t == V_INT) {
            int64_t r;
            switch (n->op) {
            case B_ADD: if (!__builtin_add_overflow(a.u.i, b.u.i, &r)) return vint(r); break;
            case B_SUB: if (!__builtin_sub_overflow(a.u.i, b.u.i, &r)) return vint(r); break;
            case B_MUL: if (!__builtin_mul_overflow(a.u.i, b.u.i, &r)) return vint(r); break;
            case B_MOD: if (b.u.i > 0 && a.u.i >= 0) return vint(a.u.i % b.u.i); break;
            case B_FLOORDIV: if (b.u.i > 0 && a.u.i >= 0) return vint(a.u.i / b.u.i); break;
            }
        } else if (a.t == V_FLOAT && b.t == V_FLOAT) {
            switch (n->op) {
            case B_ADD: return vfloat(a.u.f + b.u.f);
            case B_SUB: return vfloat(a.u.f - b.u.f);
            case B_MUL: return vfloat(a.u.f * b.u.f);
            }
        }
        py_set_line(n->line);
        return binop(n->op, a, b);
    }
    case N_UNARY: return unop(n->op, eval(n->a, f));
    case N_NOT: return vbool(!truthy(eval(n->a, f)));
    case N_AND: { V a = eval(n->a, f); return truthy(a) ? eval(n->b, f) : a; }
    case N_OR: { V a = eval(n->a, f); return truthy(a) ? a : eval(n->b, f); }
    case N_CMP: {
        V a = eval(n->a, f);
        for (int i = 0; i < n->nk; i++) {
            V b = eval(n->kids[i], f);
            if (!cmp_op(n->ops[i], a, b)) return vbool(0);
            a = b;
        }
        return vbool(1);
    }
    case N_IFEXP: return truthy(eval(n->a, f)) ? eval(n->b, f) : eval(n->c, f);
    case N_CALL: return eval_call(n, f);
    case N_ATTR: return getattr(eval(n->a, f), n->s);
    case N_INDEX: {
        V obj = eval(n->a, f), key = eval(n->b, f);
        if (is_obj(obj, O_LIST) && key.t == V_INT) {
            List *l = as_list(obj);
            int64_t i = key.u.i < 0 ? key.u.i + l->len : key.u.i;
            if (i >= 0 && i < (int64_t)l->len) return l->v[i];
        }
        py_set_line(n->line);
        return get_item(obj, key);
    }
    case N_SLICE: {
        Slice *s = rt_alloc(sizeof(Slice));
        s->type = O_SLICE;
        s->lo = n->a ? eval(n->a, f) : vnone();
        s->hi = n->b ? eval(n->b, f) : vnone();
        s->step = n->c ? eval(n->c, f) : vnone();
        return vobj(s);
    }
    case N_LIST: return build_seq(n, f, O_LIST);
    case N_TUPLE: return build_seq(n, f, O_TUPLE);
    case N_SET: return build_seq(n, f, O_SET);
    case N_DICT: {
        Dict *d = dict_new();
        for (int i = 0; i < n->nk; i++) {
            Node *k = n->kids[i];
            if (k->k == N_DSTAR) {
                V m = eval(k->a, f);
                if (!is_obj(m, O_DICT)) raise_new(P->e_type, "'%s' object is not a mapping", type_name(m));
                uint32_t pos = 0; V kk, vv;
                while (dict_next(as_dict(m), &pos, &kk, &vv)) dict_set(d, kk, vv);
            } else { V key = eval(k->a, f); dict_set(d, key, eval(k->b, f)); }
        }
        return vobj(d);
    }
    case N_COMP: return eval_comp(n, f);
    case N_LAMBDA: return make_func(n, f);
    case N_FSTR: {
        SB sb; sb_new(&sb, 64);
        for (int i = 0; i < n->nk; i++) {
            Node *k = n->kids[i];
            if (k->k == N_CONST) { sb_addstr(&sb, as_str(k->v)); continue; }
            V v = eval(k->a, f);
            if (k->op == 'r' || k->op == 'a') v = vobj(to_repr(v));
            else if (k->op == 's') v = vobj(to_str(v));
            Str *spec = k->b ? as_str(eval(k->b, f)) : 0;
            sb_addstr(&sb, spec && spec->len ? format_value(v, spec) : to_str(v));
        }
        return vobj(sb_str(&sb));
    }
    case N_WALRUS: { V v = eval(n->b, f); store_name(n->a, f, v); return v; }
    case N_STAR: raise_new(P->e_syntax, "can't use starred expression here");
    case N_DSTAR: raise_new(P->e_syntax, "can't use ** here");
    }
    raise_new(P->e_syntax, "this expression isn't supported");
    return vnone();
}

/* a comprehension runs in a frame of its own */
static void comp_clause(Node *n, Frame *cf, int ci, V *acc) {
    if (ci == n->nk) {
        switch (n->op) {
        case 'd': { V k = eval(n->a, cf); dict_set(as_dict(*acc), k, eval(n->b, cf)); break; }
        case 's': dict_set(as_dict(*acc), eval(n->a, cf), vnone()); break;
        default: list_push(as_list(*acc), eval(n->a, cf));
        }
        return;
    }
    Node *cl = n->kids[ci];
    V src = ci == 0 ? cf->slots[n->nslots] : eval(cl->b, cf);
    V it = iter_of(src), x;
    while (iter_next(it, &x)) {
        assign(cl->a, x, cf);
        int ok = 1;
        for (int i = 0; i < cl->nk && ok; i++) ok = truthy(eval(cl->kids[i], cf));
        if (ok) comp_clause(n, cf, ci + 1, acc);
    }
}

static V eval_comp(Node *n, Frame *f) {
    Frame *cf = rt_alloc(sizeof(Frame));
    cf->env = def_env(f); cf->globals = f->globals; cf->fn = f->fn;
    cf->slots = rt_alloc(sizeof(V) * (size_t)(n->nslots + 1));
    for (int i = 0; i < n->nslots; i++) cf->slots[i].t = V_UNBOUND;
    cf->slots[n->nslots] = eval(n->kids[0]->b, f);           /* the first iterable is the outer scope's */
    V acc = n->op == 'd' ? vobj(dict_new()) : n->op == 's' ? vobj(set_new()) : vobj(list_new(8));
    comp_clause(n, cf, 0, &acc);
    return acc;
}

/* ---- statements -------------------------------------------------------------------------------------------- */

static int exec_try(Node *n, Frame *f) {
    Handler h;
    volatile uint32_t saved_hl = handling->len;
    volatile int saved_calls = ncalls;
    Frame *volatile saved_frame = py_cur_frame;
    int flow = F_NORMAL;
    h.prev = py_handler; py_handler = &h;
    if (rt_setjmp(&h.jb) == 0) {
        flow = exec(n->a, f);
        py_handler = h.prev;
        if (flow == F_NORMAL && n->c) flow = exec(n->c, f);
        return flow;
    }
    py_handler = h.prev; ncalls = saved_calls; py_cur_frame = saved_frame; handling->len = saved_hl;
    V exc = py_exc;
    for (int i = 0; i < n->nk; i++) {
        Node *hn = n->kids[i];
        int match = 0;
        if (!hn->a) match = 1;
        else {
            V t = eval(hn->a, f);
            if (is_obj(t, O_CLASS)) match = is_instance(exc, (Class *)t.u.o);
            else if (is_obj(t, O_TUPLE)) {
                Tuple *tu = (Tuple *)t.u.o;
                for (uint32_t k = 0; k < tu->len && !match; k++) if (is_obj(tu->v[k], O_CLASS)) match = is_instance(exc, (Class *)tu->v[k].u.o);
            } else raise_new(P->e_type, "catching classes that do not inherit from BaseException is not allowed");
        }
        if (!match) continue;
        if (hn->c) assign(hn->c, exc, f);
        list_push(handling, exc);
        flow = exec(hn->b, f);
        handling->len--;
        if (hn->c && hn->c->scope == S_LOCAL) f->slots[hn->c->slot].t = V_UNBOUND;
        return flow;
    }
    raise(exc);
    return F_NORMAL;
}

static int exec_try_finally(Node *n, Frame *f) {
    Handler h;
    volatile uint32_t saved_hl = handling->len;
    volatile int saved_calls = ncalls;
    Frame *volatile saved_frame = py_cur_frame;
    h.prev = py_handler; py_handler = &h;
    if (rt_setjmp(&h.jb) == 0) {
        int flow = n->nk || n->c ? exec_try(n, f) : exec(n->a, f);
        py_handler = h.prev;
        V rv = ret_val;
        int fin = exec(n->d, f);
        if (fin != F_NORMAL) return fin;
        ret_val = rv;
        return flow;
    }
    py_handler = h.prev; ncalls = saved_calls; py_cur_frame = saved_frame; handling->len = saved_hl;
    V exc = py_exc;
    int fin = exec(n->d, f);
    if (fin != F_NORMAL) return fin;                        /* a return in finally swallows it */
    raise(exc);
    return F_NORMAL;
}

static int exec_with(Node *n, Frame *f, int item) {
    if (item == n->nk) return exec(n->a, f);
    Node *it = n->kids[item];
    V ctx = eval(it->a, f);
    V enter = getattr(ctx, P->s_enter), exitm = getattr(ctx, P->s_exit);
    V val = call0(enter);
    if (it->b) assign(it->b, val, f);
    Handler h;
    volatile int saved_calls = ncalls;
    Frame *volatile saved_frame = py_cur_frame;
    h.prev = py_handler; py_handler = &h;
    if (rt_setjmp(&h.jb) == 0) {
        int flow = exec_with(n, f, item + 1);
        py_handler = h.prev;
        V rv = ret_val;
        V a[3] = { vnone(), vnone(), vnone() };
        call(exitm, a, 3, 0, 0, 0);
        ret_val = rv;
        return flow;
    }
    py_handler = h.prev; ncalls = saved_calls; py_cur_frame = saved_frame;
    V exc = py_exc;
    V a[3] = { vobj(type_of(exc)), exc, vnone() };
    if (truthy(call(exitm, a, 3, 0, 0, 0))) return F_NORMAL;
    raise(exc);
    return F_NORMAL;
}

static int exec_for(Node *n, Frame *f) {
    V src = eval(n->b, f);
    int flow;
    /* lists, tuples and ranges walked directly */
    if (is_obj(src, O_RANGE)) {
        Range *r = (Range *)src.u.o;
        for (int64_t i = r->start; r->step > 0 ? i < r->stop : i > r->stop; i += r->step) {
            tick();
            if (n->a->k == N_NAME && n->a->scope == S_LOCAL) f->slots[n->a->slot] = vint(i);
            else assign(n->a, vint(i), f);
            flow = exec(n->c, f);
            if (flow == F_BREAK) return F_NORMAL;
            if (flow == F_RETURN) return flow;
        }
    } else if (is_obj(src, O_LIST) || is_obj(src, O_TUPLE)) {
        int lst = is_obj(src, O_LIST);
        for (uint32_t i = 0; ; i++) {
            uint32_t len = lst ? as_list(src)->len : ((Tuple *)src.u.o)->len;
            if (i >= len) break;
            tick();
            V x = lst ? as_list(src)->v[i] : ((Tuple *)src.u.o)->v[i];
            assign(n->a, x, f);
            flow = exec(n->c, f);
            if (flow == F_BREAK) return F_NORMAL;
            if (flow == F_RETURN) return flow;
        }
    } else {
        V it = iter_of(src), x;
        while (iter_next(it, &x)) {
            assign(n->a, x, f);
            flow = exec(n->c, f);
            if (flow == F_BREAK) return F_NORMAL;
            if (flow == F_RETURN) return flow;
        }
    }
    if (n->d) return exec(n->d, f);
    return F_NORMAL;
}

static int exec(Node *n, Frame *f) {
    switch (n->k) {
    case N_BLOCK:
        for (int i = 0; i < n->nk; i++) {
            int r = exec(n->kids[i], f);
            if (r) return r;
        }
        return F_NORMAL;
    case N_EXPR: py_set_line(n->line); eval(n->a, f); return F_NORMAL;
    case N_ASSIGN: {
        py_set_line(n->line);
        V v = eval(n->a, f);
        for (int i = 0; i < n->nk; i++) assign(n->kids[i], v, f);
        return F_NORMAL;
    }
    case N_AUG: {
        py_set_line(n->line);
        Node *t = n->a;
        V obj = vnone(), key = vnone(), cur;
        if (t->k == N_NAME) cur = load_name(t, f);
        else if (t->k == N_ATTR) { obj = eval(t->a, f); cur = getattr(obj, t->s); }
        else { obj = eval(t->a, f); key = eval(t->b, f); cur = get_item(obj, key); }
        V rhs = eval(n->b, f), r;
        V m;
        char nm[24];
        if (n->op == B_ADD && is_obj(cur, O_LIST)) {
            /* in place, as Python does */
            V it = iter_of(rhs), x;
            List *l = as_list(cur);
            if (is_obj(rhs, O_LIST)) { List *src = as_list(rhs); uint32_t k = src->len; for (uint32_t i = 0; i < k; i++) list_push(l, src->v[i]); }
            else while (iter_next(it, &x)) list_push(l, x);
            r = cur;
        } else if (is_obj(cur, O_INST) && (snprintf(nm, sizeof nm, "__i%s__", OPNAME[n->op]), lookup_special(cur, intern_c(nm), &m))) r = call1(m, rhs);
        else r = binop(n->op, cur, rhs);
        if (t->k == N_NAME) store_name(t, f, r);
        else if (t->k == N_ATTR) setattr(obj, t->s, r);
        else set_item(obj, key, r);
        return F_NORMAL;
    }
    case N_IF:
        py_set_line(n->line);
        if (truthy(eval(n->a, f))) return exec(n->b, f);
        if (n->c) return exec(n->c, f);
        return F_NORMAL;
    case N_WHILE:
        for (;;) {
            py_set_line(n->line);
            tick();
            if (!truthy(eval(n->a, f))) break;
            int r = exec(n->b, f);
            if (r == F_BREAK) return F_NORMAL;
            if (r == F_RETURN) return r;
        }
        if (n->c) return exec(n->c, f);
        return F_NORMAL;
    case N_FOR: py_set_line(n->line); return exec_for(n, f);
    case N_BREAK: return F_BREAK;
    case N_CONTINUE: return F_CONTINUE;
    case N_PASS: case N_GLOBAL: case N_NONLOCAL: return F_NORMAL;
    case N_RETURN: py_set_line(n->line); ret_val = n->a ? eval(n->a, f) : vnone(); return F_RETURN;
    case N_DEF: {
        py_set_line(n->line);
        V fn = make_func(n, f);
        assign(n->b, decorate(n->d, fn, f), f);
        return F_NORMAL;
    }
    case N_CLASS: py_set_line(n->line); assign(n->b, make_class(n, f), f); return F_NORMAL;
    case N_TRY: py_set_line(n->line); return n->d ? exec_try_finally(n, f) : exec_try(n, f);
    case N_WITH: py_set_line(n->line); return exec_with(n, f, 0);
    case N_RAISE: {
        py_set_line(n->line);
        if (!n->a) {
            if (!handling->len) raise_new(P->e_runtime, "No active exception to reraise");
            raise(handling->v[handling->len - 1]);
        }
        V e = eval(n->a, f);
        if (is_obj(e, O_CLASS)) e = call0(e);
        if (n->b && is_obj(e, O_INST)) dict_set_str(((Inst *)e.u.o)->dict, intern_c("__cause__"), eval(n->b, f));
        if (is_obj(e, O_INST)) dict_del(((Inst *)e.u.o)->dict, vobj(P->s_tb));
        raise(e);
    }
    case N_IMPORT:
        py_set_line(n->line);
        for (int i = 0; i < n->nk; i++) {
            Node *it = n->kids[i];
            /* import a.b binds a; import a.b as c binds a.b */
            const char *dot = strchr(it->s->s, '.');
            Module *m = import_module(it->s, n->line);
            if (dot && !it->flags) m = import_module(intern(it->s->s, (uint32_t)(dot - it->s->s)), n->line);
            assign(it->a, vobj(m), f);
        }
        return F_NORMAL;
    case N_FROM: {
        py_set_line(n->line);
        Module *m = import_module(n->s, n->line);
        if (n->flags & 1) {
            uint32_t pos = 0; V k, v;
            while (dict_next(m->dict, &pos, &k, &v)) if (is_obj(k, O_STR) && as_str(k)->s[0] != '_') dict_set(f->ns ? f->ns : f->globals, k, v);
            return F_NORMAL;
        }
        for (int i = 0; i < n->nk; i++) {
            Node *it = n->kids[i];
            V v;
            if (!dict_get_str(m->dict, it->s, &v)) raise_new(P->e_import, "cannot import name '%s' from '%s'", it->s->s, n->s->s);
            assign(it->a, v, f);
        }
        return F_NORMAL;
    }
    case N_DEL:
        py_set_line(n->line);
        for (int i = 0; i < n->nk; i++) {
            Node *t = n->kids[i];
            if (t->k == N_NAME) del_name(t, f);
            else if (t->k == N_INDEX) del_item(eval(t->a, f), eval(t->b, f));
            else if (t->k == N_ATTR) {
                V o = eval(t->a, f);
                if (is_obj(o, O_INST) && dict_del(((Inst *)o.u.o)->dict, vobj(t->s))) continue;
                raise_new(P->e_attr, "'%s' object has no attribute '%s'", type_name(o), t->s->s);
            } else if (t->k == N_TUPLE || t->k == N_LIST) {
                for (int k = 0; k < t->nk; k++) { Node *x = t->kids[k]; if (x->k == N_NAME) del_name(x, f); }
            }
        }
        return F_NORMAL;
    case N_ITEM:                                           /* `x: int` in a class body: noted for dataclasses */
        if (n->flags == 2) {
            Dict *ns = f->ns ? f->ns : f->globals;
            V ann;
            Str *key = intern_c("__annotations__");
            if (!dict_get_str(ns, key, &ann) || !is_obj(ann, O_DICT)) { ann = vobj(dict_new()); dict_set_str(ns, key, ann); }
            dict_set(as_dict(ann), vobj(n->s), vnone());
        }
        return F_NORMAL;
    case N_ASSERT:
        py_set_line(n->line);
        if (!truthy(eval(n->a, f))) raise(make_exc(P->e_assert, n->b ? eval(n->b, f) : (V){ .t = V_UNBOUND }));
        return F_NORMAL;
    }
    py_set_line(n->line);
    eval(n, f);
    return F_NORMAL;
}

/* ---- the program -------------------------------------------------------------------------------------------- */
void py_init_state(void);
void py_close_files(void);

int py_main(const char *src, const char *path) {
    ncalls = 0; ticks = 0; ret_val = vnone(); py_handler = 0; py_exc = vnone();
    P = rt_alloc(sizeof(PyState));
    rt_root((void **)&P);
    rt_root((void **)&py_exc.u.o);
    rt_root((void **)&ret_val.u.o);
    rt_root((void **)&handling);
    py_init_state();
    handling = list_new(4);
    P->main_file = str_c(path);
    P->main_src = src;
    P->files[0] = P->main_file; P->srcs[0] = src; P->nfiles = 1;
    Module *main = rt_alloc(sizeof(Module));
    main->type = O_MODULE; main->name = P->s_main; main->dict = dict_new();
    dict_set_str(main->dict, P->s_name, vobj(P->s_main));
    dict_set_str(P->modules, P->s_main, vobj(main));
    Frame *mf = rt_alloc(sizeof(Frame));
    mf->globals = main->dict;
    py_cur_frame = mf;
    calls[0].fn = intern_c("<module>"); calls[0].file = P->main_file; calls[0].line = 1;
    ncalls = 1;
    Handler h;
    h.prev = 0; py_handler = &h;
    if (rt_setjmp(&h.jb) == 0) {
        Node *prog = parse_program(src, P->main_file);
        exec(prog, mf);
        py_close_files();
        py_handler = 0;
        return 0;
    }
    py_handler = 0;
    V exc = py_exc;
    {
        /* what the program wrote is kept, even when it ended in an error */
        Handler h2; h2.prev = 0; py_handler = &h2;
        if (rt_setjmp(&h2.jb) == 0) py_close_files();
        py_handler = 0;
    }
    if (is_instance(exc, P->e_exit)) {
        Inst *e = (Inst *)exc.u.o;
        Tuple *t = is_obj(e->native, O_TUPLE) ? (Tuple *)e->native.u.o : 0;
        V code = t && t->len ? t->v[0] : vnone();
        if (code.t == V_NONE || (code.t == V_INT && code.u.i == 0) || (code.t == V_BOOL && !code.u.i)) return 0;
        if (code.t == V_INT) return (int)code.u.i;
        Str *s = to_str(code);
        rt_write(s->s, s->len); rt_write("\n", 1);
        return 1;
    }
    py_uncaught(exc);
    return 1;
}
