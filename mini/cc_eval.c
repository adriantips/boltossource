/* ===========================================================================
 *  BoltOS Mini  -  mini/cc_eval.c
 *
 *  Running the typed tree cc.c made (cc.h). The program's memory is one
 *  byte array: nothing below 4 KiB (a null pointer's neighbourhood), then
 *  the string literals and globals, then the heap malloc() and new use,
 *  and the stack at the top. A frame is the function's locals with two
 *  words in front of it: `this`, and for a lambda the frame it was made
 *  in, which is how it reaches the variables around it.
 *
 *  Objects -- structs, classes, strings, containers -- are handled by
 *  address: an expression of such a type evaluates to where the object is,
 *  a temporary if it's a fresh value. Temporaries live until the end of
 *  the statement that made them, as C++ says, and locals that own memory
 *  are destroyed when their block ends.
 * ===========================================================================*/
#include <stdint.h>
#include "cc.h"
#include "string.h"
#include "stdio.h"

uint8_t *cc_mem;
uint64_t cc_memsz;
static uint64_t sp, fp, stack_lo, heap_lo, heap_top, heap_hi;
static CV ret_val;
static uint64_t ret_to_addr;
static uint32_t ticks;

enum { X_NORMAL, X_BREAK, X_CONTINUE, X_RETURN };

static inline uint64_t ld64(uint64_t a) { uint64_t v; memcpy(&v, cc_at(a, 8, 0), 8); return v; }
static inline void st64(uint64_t a, uint64_t v) { memcpy(cc_at(a, 8, 1), &v, 8); }

void cc_fault(uint64_t addr, uint64_t n, int write) {
    (void)n;
    if (addr < 4096) cc_error("Segmentation fault: %s a null pointer", write ? "writing through" : "reading through");
    if (addr >= stack_lo - 65536 && addr < stack_lo) cc_error("Stack overflow (the recursion went too deep)");
    cc_error("Segmentation fault: %s address 0x%llx, outside the program's memory", write ? "writing to" : "reading", (unsigned long long)addr);
}

/* ---- the heap: free lists per 16-byte size up to 4 KiB, first fit above ---------------------------------- */
#define HDR 16
#define MAGIC_USED 0xB01700A110C0FFEEull
#define MAGIC_FREE 0xB01700F4EEF4EEF4ull
#define NBIN 257
static uint64_t bins[NBIN];                /* small free blocks by size/16; bins[0]: big ones, a list */
static uint64_t in_use;

uint64_t cc_heap_in_use(void) { return in_use; }

uint64_t cc_malloc(uint64_t n) {
    if (n == 0) n = 1;
    if (n > heap_hi - heap_lo) return 0;
    uint64_t sz = (n + 15) & ~15ull;
    uint64_t b = 0;
    if (sz / 16 < NBIN && bins[sz / 16]) {
        /* a free one of this size: the next was kept in its first word */
        b = bins[sz / 16];
        bins[sz / 16] = ld64(b + HDR);
    } else if (sz / 16 >= NBIN) {
        /* the big list: first fit */
        uint64_t prev = 0, cur = bins[0];
        while (cur) {
            uint64_t csz = ld64(cur);
            if (csz >= sz) {
                uint64_t nx = ld64(cur + HDR);
                if (prev) st64(prev + HDR, nx); else bins[0] = nx;
                if (csz >= sz + 256 + HDR) {
                    uint64_t rest = cur + HDR + sz, rsz = csz - sz - HDR;
                    st64(rest, rsz); st64(rest + 8, MAGIC_FREE);
                    if (rsz / 16 < NBIN) { st64(rest + HDR, bins[rsz / 16]); bins[rsz / 16] = rest; }
                    else { st64(rest + HDR, bins[0]); bins[0] = rest; }
                    st64(cur, sz);
                }
                b = cur;
                break;
            }
            prev = cur; cur = ld64(cur + HDR);
        }
    }
    if (!b) {
        if (heap_top + HDR + sz > heap_hi) return 0;
        b = heap_top;
        heap_top += HDR + sz;
        st64(b, sz);
    }
    st64(b + 8, MAGIC_USED);
    uint64_t real = ld64(b);
    memset(cc_mem + b + HDR, 0, real);
    in_use += real;
    return b + HDR;
}

void cc_free(uint64_t a) {
    if (!a) return;
    if (a < heap_lo + HDR || a >= heap_top) cc_error("free(): that pointer wasn't from malloc or new");
    uint64_t b = a - HDR;
    uint64_t m = ld64(b + 8);
    if (m == MAGIC_FREE) cc_error("free(): double free (that memory was already given back)");
    if (m != MAGIC_USED) cc_error("free(): invalid pointer (not the start of what malloc gave)");
    uint64_t sz = ld64(b);
    in_use -= sz;
    st64(b + 8, MAGIC_FREE);
    if (sz / 16 < NBIN) { st64(a, bins[sz / 16]); bins[sz / 16] = b; }
    else { st64(a, bins[0]); bins[0] = b; }
}

uint64_t cc_realloc(uint64_t a, uint64_t n) {
    if (!a) return cc_malloc(n);
    uint64_t old = ld64(a - HDR);
    if (ld64(a - HDR + 8) != MAGIC_USED) cc_error("realloc(): invalid pointer");
    if (n <= old) return a;
    uint64_t b = cc_malloc(n);
    if (!b) return 0;
    memcpy(cc_mem + b, cc_mem + a, old);
    cc_free(a);
    return b;
}

/* ---- loads and stores ------------------------------------------------------------------------------------ */
CV cc_load(CType *t, uint64_t a) {
    CV v; v.u = 0;
    switch (t->kind) {
    case TY_BOOL: case TY_UCHAR: v.u = *cc_at(a, 1, 0); break;
    case TY_CHAR: v.i = (int8_t)*cc_at(a, 1, 0); break;
    case TY_SHORT: { int16_t x; memcpy(&x, cc_at(a, 2, 0), 2); v.i = x; break; }
    case TY_USHORT: { uint16_t x; memcpy(&x, cc_at(a, 2, 0), 2); v.u = x; break; }
    case TY_INT: case TY_ENUM: { int32_t x; memcpy(&x, cc_at(a, 4, 0), 4); v.i = x; break; }
    case TY_UINT: { uint32_t x; memcpy(&x, cc_at(a, 4, 0), 4); v.u = x; break; }
    case TY_FLOAT: { float x; memcpy(&x, cc_at(a, 4, 0), 4); v.d = x; break; }
    case TY_DOUBLE: memcpy(&v.d, cc_at(a, 8, 0), 8); break;
    case TY_LONG: case TY_ULONG: case TY_PTR: case TY_REF: case TY_NULLPTR: case TY_OSTREAM: case TY_ISTREAM:
        memcpy(&v.u, cc_at(a, 8, 0), 8); break;
    default: v.u = a;                      /* an object: its address */
    }
    return v;
}

void cc_store(CType *t, uint64_t a, CV v) {
    switch (t->kind) {
    case TY_BOOL: *cc_at(a, 1, 1) = v.u != 0; break;
    case TY_CHAR: case TY_UCHAR: *cc_at(a, 1, 1) = (uint8_t)v.u; break;
    case TY_SHORT: case TY_USHORT: { uint16_t x = (uint16_t)v.u; memcpy(cc_at(a, 2, 1), &x, 2); break; }
    case TY_INT: case TY_UINT: case TY_ENUM: { uint32_t x = (uint32_t)v.u; memcpy(cc_at(a, 4, 1), &x, 4); break; }
    case TY_FLOAT: { float x = (float)v.d; memcpy(cc_at(a, 4, 1), &x, 4); break; }
    case TY_DOUBLE: memcpy(cc_at(a, 8, 1), &v.d, 8); break;
    case TY_LONG: case TY_ULONG: case TY_PTR: case TY_REF: case TY_NULLPTR: case TY_OSTREAM: case TY_ISTREAM:
        memcpy(cc_at(a, 8, 1), &v.u, 8); break;
    default:
        if (t->size > 0 && v.u != a) memmove(cc_at(a, (uint64_t)t->size, 1), cc_at(v.u, (uint64_t)t->size, 0), (size_t)t->size);
    }
}

/* ---- strings and owned values -------------------------------------------------------------------------------- */
static uint64_t empty_cstr;

void cc_str_set(uint64_t s, const char *p, uint64_t n) {
    uint64_t data = ld64(s), cap = ld64(s + 16);
    if (n + 1 > cap || !data) {
        uint64_t nc = cap ? cap : 15;
        while (nc < n + 1) nc *= 2;
        uint64_t nd = cc_malloc(nc + 1);
        if (!nd) cc_error("out of memory (a string of %llu characters)", (unsigned long long)n);
        if (data) cc_free(data);
        data = nd; cap = nc;
        st64(s, data); st64(s + 16, cap);
    }
    memmove(cc_mem + data, p, (size_t)n);
    cc_mem[data + n] = 0;
    st64(s + 8, n);
}

const char *cc_str_ptr(uint64_t s, uint64_t *n) {
    uint64_t data = ld64(s), len = ld64(s + 8);
    if (n) *n = len;
    if (!data) return (const char *)cc_mem + empty_cstr;
    return (const char *)cc_at(data, len + 1, 0);
}

static uint64_t elem_size(CType *t) {
    if (t->kind == TY_MAP) { CType *p = ty_pair(t->base, t->base2); return (uint64_t)p->size; }
    if (t->kind == TY_STRING) return 1;
    return (uint64_t)t->base->size;
}
static CType *elem_type(CType *t) { return t->kind == TY_MAP ? ty_pair(t->base, t->base2) : t->kind == TY_STRING ? ty_char : t->base; }

void cc_destroy(CType *t, uint64_t a) {
    switch (t->kind) {
    case TY_STRING: cc_free(ld64(a)); st64(a, 0); st64(a + 8, 0); st64(a + 16, 0); return;
    case TY_VECTOR: case TY_SET: case TY_MAP: {
        CType *et = elem_type(t);
        uint64_t data = ld64(a), n = ld64(a + 8), es = elem_size(t);
        if (et->owns || (et->kind == TY_STRUCT && et->dtor)) for (uint64_t i = n; i > 0; i--) cc_destroy(et, data + (i - 1) * es);
        cc_free(data);
        st64(a, 0); st64(a + 8, 0); st64(a + 16, 0);
        return;
    }
    case TY_PAIR:
        if (t->mem[1].ty->owns) cc_destroy(t->mem[1].ty, a + (uint64_t)t->mem[1].offset);
        if (t->mem[0].ty->owns) cc_destroy(t->mem[0].ty, a);
        return;
    case TY_ARRAY:
        if (t->base->owns || (t->base->kind == TY_STRUCT && t->base->dtor))
            for (int i = t->len - 1; i >= 0; i--) cc_destroy(t->base, a + (uint64_t)i * (uint64_t)t->base->size);
        return;
    case TY_STRUCT:
        for (CType *c = t; c; c = c->super) {
            if (c->dtor && c->dtor->body) cc_call(c->dtor, 0, 0, a, 0);
            for (int i = c->nmem - 1; i >= 0; i--) {
                CType *mt = c->mem[i].ty;
                if (mt->owns || (mt->kind == TY_STRUCT && mt->dtor)) cc_destroy(mt, a + (uint64_t)c->mem[i].offset);
            }
        }
        return;
    }
}

/* dst holds nothing (zeroed); after this it's a copy of src */
void cc_copy_value(CType *t, uint64_t dst, uint64_t src) {
    if (dst == src) return;
    switch (t->kind) {
    case TY_STRING: { uint64_t n; const char *p = cc_str_ptr(src, &n); st64(dst, 0); st64(dst + 8, 0); st64(dst + 16, 0); if (n) cc_str_set(dst, p, n); return; }
    case TY_VECTOR: case TY_SET: case TY_MAP: {
        CType *et = elem_type(t);
        uint64_t n = ld64(src + 8), es = elem_size(t), sdata = ld64(src);
        st64(dst, 0); st64(dst + 8, 0); st64(dst + 16, 0);
        if (!n) return;
        uint64_t d = cc_malloc(n * es);
        if (!d) cc_error("out of memory copying a container of %llu", (unsigned long long)n);
        st64(dst, d); st64(dst + 8, n); st64(dst + 16, n);
        if (et->owns) for (uint64_t i = 0; i < n; i++) cc_copy_value(et, d + i * es, sdata + i * es);
        else memcpy(cc_mem + d, cc_at(sdata, n * es, 0), (size_t)(n * es));
        return;
    }
    case TY_PAIR: case TY_STRUCT: case TY_ARRAY:
        if (!t->owns) { memmove(cc_at(dst, (uint64_t)t->size, 1), cc_at(src, (uint64_t)t->size, 0), (size_t)t->size); }
        else if (t->kind == TY_ARRAY) { for (int i = 0; i < t->len; i++) cc_copy_value(t->base, dst + (uint64_t)i * (uint64_t)t->base->size, src + (uint64_t)i * (uint64_t)t->base->size); }
        else {
            memmove(cc_at(dst, (uint64_t)t->size, 1), cc_at(src, (uint64_t)t->size, 0), (size_t)t->size);
            for (CType *c = t; c; c = c->super)
                for (int i = 0; i < c->nmem; i++) {
                    CType *mt = c->mem[i].ty;
                    if (!mt->owns) continue;
                    uint64_t o = (uint64_t)c->mem[i].offset;
                    memset(cc_mem + dst + o, 0, (size_t)mt->size);
                    cc_copy_value(mt, dst + o, src + o);
                }
        }
        if (t->kind == TY_STRUCT && t->polymorphic) st64(dst, (uint64_t)t->class_id);
        return;
    }
    cc_store(t, dst, cc_load(t, src));
}

void cc_assign_value(CType *t, uint64_t dst, uint64_t src) {
    if (dst == src) return;
    if (!t->owns && t->kind != TY_STRING) {
        uint64_t vid = t->kind == TY_STRUCT && t->polymorphic ? ld64(dst) : 0;
        memmove(cc_at(dst, (uint64_t)t->size, 1), cc_at(src, (uint64_t)t->size, 0), (size_t)t->size);
        if (vid) st64(dst, vid);
        return;
    }
    if (t->kind == TY_STRING) { uint64_t n; const char *p = cc_str_ptr(src, &n); char *tmp = rt_alloc_leaf((size_t)n + 1); memcpy(tmp, p, (size_t)n); cc_str_set(dst, tmp, n); return; }
    /* copy into a fresh one first: src may live inside dst */
    uint64_t tmp = cc_malloc((uint64_t)t->size);
    cc_copy_value(t, tmp, src);
    uint64_t vid = t->kind == TY_STRUCT && t->polymorphic ? ld64(dst) : 0;
    cc_destroy(t, dst);
    memcpy(cc_mem + dst, cc_mem + tmp, (size_t)t->size);
    if (vid) st64(dst, vid);
    cc_free(tmp);
}

/* ---- temporaries ------------------------------------------------------------------------------------------- */
static struct { CType *t; uint64_t a; } *temps;
static int ntemps, temps_cap;

uint64_t cc_temp(CType *t) {
    if (ntemps == temps_cap) {
        int nc = temps_cap ? temps_cap * 2 : 64;
        void *n = rt_alloc_leaf(sizeof(*temps) * (size_t)nc);
        if (temps) memcpy(n, temps, sizeof(*temps) * (size_t)ntemps);
        temps = n; temps_cap = nc;
    }
    uint64_t a = cc_malloc((uint64_t)(t->size > 0 ? t->size : 8));
    if (!a) cc_error("out of memory");
    temps[ntemps].t = t; temps[ntemps].a = a; ntemps++;
    return a;
}

static void temps_release(int mark) {
    while (ntemps > mark) {
        ntemps--;
        CType *t = temps[ntemps].t;
        if (t->owns || (t->kind == TY_STRUCT && t->dtor) || t->kind == TY_STRING) cc_destroy(t, temps[ntemps].a);
        cc_free(temps[ntemps].a);
    }
}

/* ---- frames -------------------------------------------------------------------------------------------------- */
static uint64_t frame_at(int depth) {
    uint64_t f = fp;
    for (int i = 0; i < depth; i++) f = ld64(f - 8);
    return f;
}

/* ---- construction ------------------------------------------------------------------------------------------ */
static CV eval(CNode *n);
static int exec(CNode *n);

static CFunc *default_ctor(CType *t) {
    for (int i = 0; i < t->nmeth; i++) {
        CFunc *f = t->meth[i];
        if (!f->is_ctor) continue;
        int need = 0;
        for (int k = 0; k < f->ty->nparams; k++) if (!f->pdefault || !f->pdefault[k]) need = k + 1;
        if (!need) return f;
    }
    return 0;
}

/* what a constructor does before its body: the base, which class it is, the members */
static void prologue(CType *t, uint64_t a, CFunc *ctor) {
    CNode **inits = ctor ? (CNode **)ctor->inits : 0;
    int ni = ctor ? ctor->ninits : 0;
    if (t->super) {
        CNode *bi = 0;
        for (int i = 0; i < ni; i++) if (inits[i]->flag == 1) bi = inits[i];
        if (bi) cc_init_at(a, t->super, bi->a);
        else cc_construct_default(t->super, a);
    }
    if (t->polymorphic) st64(a, (uint64_t)t->class_id);
    for (int m = 0; m < t->nmem; m++) {
        CMember *mm = &t->mem[m];
        CNode *in = 0;
        for (int i = 0; i < ni; i++) if (inits[i]->flag != 1 && inits[i]->off == mm->offset && inits[i]->ty == mm->ty) in = inits[i];
        if (!in) for (int i = 0; i < t->nminit; i++) if (t->minit[i]->off == mm->offset) in = t->minit[i];
        uint64_t ma = a + (uint64_t)mm->offset;
        if (in) {
            if (mm->ty->kind == TY_REF) { CV v = eval(in->a); st64(ma, v.u); }
            else cc_init_at(ma, mm->ty, in->a);
        } else if (mm->ty->kind == TY_STRUCT) cc_construct_default(mm->ty, ma);
        else if (mm->ty->kind == TY_ARRAY && mm->ty->base->kind == TY_STRUCT)
            for (int k = 0; k < mm->ty->len; k++) cc_construct_default(mm->ty->base, ma + (uint64_t)k * (uint64_t)mm->ty->base->size);
    }
}

void cc_construct_default(CType *t, uint64_t a) {
    if (t->kind == TY_ARRAY) {
        if (t->base->kind == TY_STRUCT) for (int k = 0; k < t->len; k++) cc_construct_default(t->base, a + (uint64_t)k * (uint64_t)t->base->size);
        return;
    }
    if (t->kind != TY_STRUCT) return;
    CFunc *c = default_ctor(t);
    if (c) {
        CV args[16];
        int n = c->ty->nparams;
        for (int i = 0; i < n && i < 16; i++) {
            CV v = eval(c->pdefault[i]);
            args[i] = v;
        }
        cc_call(c, args, n, a, 0);
        return;
    }
    if (t->has_ctor) cc_error("%s has no constructor that takes no arguments", t->name);
    prologue(t, a, 0);
}

static void store_value(CType *t, uint64_t a, CNode *e) {
    if (t->kind == TY_REF) { st64(a, eval(e).u); return; }
    CV v = eval(e);
    if (t->kind == TY_STRUCT || t->kind == TY_STRING || t->kind == TY_VECTOR || t->kind == TY_MAP || t->kind == TY_SET || t->kind == TY_PAIR) cc_copy_value(t, a, v.u);
    else if (t->kind == TY_LAMBDA || t->kind == TY_ARRAY) memmove(cc_at(a, (uint64_t)t->size, 1), cc_at(v.u, (uint64_t)t->size, 0), (size_t)t->size);
    else cc_store(t, a, v);
}

void cc_lib_container_init(CType *t, uint64_t a, CNode *init);

/* memory at a (zeroed) becomes a t, from what `init` says */
void cc_init_at(uint64_t a, CType *t, CNode *init) {
    if (!init) { cc_construct_default(t, a); return; }
    if (init->k != C_INIT) {
        if (t->kind == TY_ARRAY && init->k == C_STR) {
            uint64_t n = (uint64_t)init->ival + 1;
            if (t->len >= 0 && n > (uint64_t)t->len) n = (uint64_t)t->len;
            memcpy(cc_at(a, n, 1), cc_mem + init->off, (size_t)n);
            return;
        }
        store_value(t, a, init);
        return;
    }
    switch (t->kind) {
    case TY_STRUCT:
        if (init->fn) {
            CV args[32]; int n = init->nk < 32 ? init->nk : 32;
            for (int i = 0; i < n; i++) args[i] = eval(init->kids[i]);
            cc_call(init->fn, args, n, a, 0);
            return;
        }
        if (init->op == 1) { cc_copy_value(t, a, eval(init->kids[0]).u); return; }
        if (t->has_ctor || t->nminit || t->super || t->polymorphic) { if (!init->nk) { cc_construct_default(t, a); return; } }
        if (t->polymorphic) st64(a, (uint64_t)t->class_id);
        for (int i = 0; i < t->nminit; i++) cc_init_at(a + (uint64_t)t->minit[i]->off, t->minit[i]->ty, t->minit[i]->a);
        for (int i = 0; i < init->nk; i++) {
            CNode *k = init->kids[i];
            CMember *m = 0;
            for (int j = 0; j < t->nmem; j++) if (t->mem[j].offset == k->off) { m = &t->mem[j]; if (!t->is_union) break; }
            CType *mt = k->a->ty && m ? m->ty : (m ? m->ty : k->a->ty);
            for (int j = 0; j < t->nmem; j++) if (t->mem[j].offset == k->off && t->mem[j].ty == k->a->ty) mt = t->mem[j].ty;
            if (mt->owns) cc_destroy(mt, a + (uint64_t)k->off);
            cc_init_at(a + (uint64_t)k->off, mt, k->a);
        }
        return;
    case TY_ARRAY:
        for (int i = 0; i < init->nk; i++) cc_init_at(a + (uint64_t)init->kids[i]->off, t->base, init->kids[i]->a);
        if (t->base->kind == TY_STRUCT && t->base->has_ctor)
            for (int i = (int)init->ival; i < t->len; i++) cc_construct_default(t->base, a + (uint64_t)i * (uint64_t)t->base->size);
        return;
    case TY_PAIR:
        cc_init_at(a, t->mem[0].ty, init->kids[0]);
        cc_init_at(a + (uint64_t)t->mem[1].offset, t->mem[1].ty, init->kids[1]);
        return;
    case TY_STRING: case TY_VECTOR: case TY_SET: case TY_MAP:
        cc_lib_container_init(t, a, init);
        return;
    default:
        if (init->nk) store_value(t, a, init->kids[0]);
        return;
    }
}

/* ---- calls ---------------------------------------------------------------------------------------------------- */
static int depth;

CV cc_call(CFunc *f, CV *args, int n, uint64_t self, uint64_t ret_to) {
    if (!f->body) {
        if (f->is_pure) cc_error("pure virtual method %s called", f->name);
        cc_error("%s() is declared but never defined", f->name);
    }
    if (rt_stack_left() < 256 * 1024 || ++depth > 100000) { depth--; cc_error("Stack overflow (the recursion went too deep)"); }
    if ((++ticks & 255) == 0) rt_check();
    uint64_t frame = ((uint64_t)f->frame + 16 + 15) & ~15ull;
    if (sp - frame < stack_lo) { depth--; cc_error("Stack overflow (the recursion went too deep, or the locals are too big)"); }
    uint64_t saved_fp = fp, saved_sp = sp, saved_ret_to = ret_to_addr;
    sp -= frame;
    memset(cc_mem + sp, 0, (size_t)frame);
    uint64_t nfp = sp + 16;
    st64(nfp - 16, self);
    st64(nfp - 8, 0);
    CType *ft = f->ty;
    for (int i = 0; i < ft->nparams && i < n; i++) {
        CType *pt = ft->params[i];
        uint64_t pa = nfp + (uint64_t)f->poff[i];
        if (pt->kind == TY_REF) st64(pa, args[i].u);
        else if (pt->kind == TY_STRUCT || pt->kind == TY_STRING || pt->kind == TY_VECTOR || pt->kind == TY_MAP || pt->kind == TY_SET || pt->kind == TY_PAIR) cc_copy_value(pt, pa, args[i].u);
        else if (pt->kind == TY_LAMBDA) memcpy(cc_at(pa, 16, 1), cc_at(args[i].u, 16, 0), 16);
        else cc_store(pt, pa, args[i]);
    }
    fp = nfp;
    ret_to_addr = ret_to;
    if (f->is_ctor && f->owner) prologue(f->owner, self, f);
    ret_val.u = 0;
    int mark = ntemps;
    CV r = exec(f->body) == X_RETURN ? ret_val : (CV){ .u = 0 };
    temps_release(mark);
    for (int i = 0; i < ft->nparams && i < n; i++) {
        CType *pt = ft->params[i];
        if (pt->kind != TY_REF && (pt->owns || (pt->kind == TY_STRUCT && pt->dtor))) cc_destroy(pt, nfp + (uint64_t)f->poff[i]);
    }
    fp = saved_fp; sp = saved_sp; ret_to_addr = saved_ret_to;
    depth--;
    return r;
}

/* a lambda's call: its frame's link is the frame it was made in */
static CV call_lambda(CFunc *f, uint64_t env, CV *args, int n, uint64_t ret_to) {
    uint64_t saved_sp = sp;
    /* cc_call makes the frame; the link is written after, before anything runs */
    if (!f->body) cc_error("a lambda without a body");
    uint64_t frame = ((uint64_t)f->frame + 16 + 15) & ~15ull;
    if (sp - frame < stack_lo) cc_error("Stack overflow (the recursion went too deep)");
    if (rt_stack_left() < 256 * 1024) cc_error("Stack overflow (the recursion went too deep)");
    uint64_t saved_fp = fp, saved_ret_to = ret_to_addr;
    sp -= frame;
    memset(cc_mem + sp, 0, (size_t)frame);
    uint64_t nfp = sp + 16;
    st64(nfp - 16, env ? ld64(env - 16) : 0);          /* `this`, as the enclosing method's */
    st64(nfp - 8, env);
    CType *ft = f->ty;
    for (int i = 0; i < ft->nparams && i < n; i++) {
        CType *pt = ft->params[i];
        uint64_t pa = nfp + (uint64_t)f->poff[i];
        if (pt->kind == TY_REF) st64(pa, args[i].u);
        else if (pt->kind == TY_STRUCT || pt->kind == TY_STRING || pt->kind == TY_VECTOR || pt->kind == TY_MAP || pt->kind == TY_SET || pt->kind == TY_PAIR) cc_copy_value(pt, pa, args[i].u);
        else cc_store(pt, pa, args[i]);
    }
    fp = nfp; ret_to_addr = ret_to;
    ret_val.u = 0;
    int mark = ntemps;
    CV r = exec(f->body) == X_RETURN ? ret_val : (CV){ .u = 0 };
    temps_release(mark);
    fp = saved_fp; sp = saved_sp; ret_to_addr = saved_ret_to;
    return r;
}

int cc_lib_native_call(int id, CV *args, int n, CV *out);

CV cc_call_value(CV callee, CType *ct, CV *args, int n, uint64_t ret_to) {
    if (ct && ct->kind == TY_LAMBDA) {
        uint64_t idx = ld64(callee.u), env = ld64(callee.u + 8);
        if ((int64_t)idx < 0) { CV out; out.u = 0; cc_lib_native_call((int)-(int64_t)idx, args, n, &out); return out; }
        CFunc *f = cc_func_by_index((int)idx);
        if (!f) cc_error("calling an empty std::function");
        if (!strcmp(f->name, "lambda")) return call_lambda(f, env, args, n, ret_to);
        return cc_call(f, args, n, 0, ret_to);
    }
    if (ct && ct->kind == TY_STRUCT) {
        CFunc *op = cc_find_method(ct, "operator()", 0);
        if (!op) cc_error("%s can't be called", ct->name);
        return cc_call(op, args, n, callee.u, ret_to);
    }
    int64_t idx = callee.i;
    if (idx < 0) { CV out; out.u = 0; cc_lib_native_call((int)-idx, args, n, &out); return out; }
    CFunc *f = cc_func_by_index((int)idx);
    if (!f) cc_error("calling a null function pointer");
    return cc_call(f, args, n, 0, ret_to);
}

static CFunc *virtual_target(CFunc *f, uint64_t self) {
    CType *dyn = cc_class_by_id((int)ld64(self));
    if (!dyn) return f;
    for (CType *c = dyn; c; c = c->super)
        for (int i = 0; i < c->nmeth; i++) {
            CFunc *g = c->meth[i];
            if (!strcmp(g->name, f->name) && g->ty->nparams == f->ty->nparams && (g->body || g == f)) return g;
        }
    return f;
}

/* ---- exceptions ------------------------------------------------------------------------------------------------ */
typedef struct Catch { rt_jmp_t jb; struct Catch *prev; } Catch;
static Catch *catching;
static CType *thrown_ty;
static uint64_t thrown_at;

static void throw_value(CType *t, uint64_t src) {
    thrown_ty = t;
    thrown_at = cc_malloc((uint64_t)(t->size > 0 ? t->size : 8));
    if (t->kind == TY_STRUCT || t->kind == TY_STRING || t->kind == TY_VECTOR || t->kind == TY_PAIR) cc_copy_value(t, thrown_at, src);
    else memcpy(cc_mem + thrown_at, cc_mem + src, (size_t)t->size);
    if (!catching) {
        char b[300], tn[80];
        if (t->kind == TY_STRUCT) {
            CFunc *w = cc_find_method(t, "what", 0);
            if (w) {
                uint64_t r = cc_temp(ty_string);
                cc_call(virtual_target(w, thrown_at), 0, 0, thrown_at, r);
                snprintf(b, sizeof b, "terminate called after throwing an instance of '%s'\n  what():  %s", t->name, cc_str_ptr(r, 0));
            } else snprintf(b, sizeof b, "terminate called after throwing an instance of '%s'", t->name);
        } else if (t->kind == TY_STRING) snprintf(b, sizeof b, "terminate called after throwing a string: %s", cc_str_ptr(thrown_at, 0));
        else if (t->kind == TY_PTR && t->base->kind == TY_CHAR) snprintf(b, sizeof b, "terminate called after throwing \"%s\"", (char *)cc_mem + ld64(thrown_at));
        else if (ty_is_int(t)) snprintf(b, sizeof b, "terminate called after throwing an int (%lld)", (long long)cc_load(t, thrown_at).i);
        else snprintf(b, sizeof b, "terminate called after throwing a %s", ty_str(t, tn, sizeof tn));
        cc_flush();
        cc_error("%s", b);
    }
    rt_longjmp(&catching->jb, 1);
}

/* the library's errors, as the C++ exceptions they are (out_of_range...) when a
 * try is there to catch them; else they end the program with what they say */
void cc_throw_std(const char *cls, const char *msg);
void cc_throw_std(const char *cls, const char *msg) {
    CType *t = 0;
    for (int i = 1; cc_class_by_id(i); i++) if (cc_class_by_id(i)->name && !strcmp(cc_class_by_id(i)->name, cls)) t = cc_class_by_id(i);
    if (!t || !catching) { cc_flush(); cc_error("terminate called after throwing an instance of 'std::%s'\n  what():  %s", cls, msg); }
    uint64_t s = cc_temp(ty_string);
    cc_str_set(s, msg, strlen(msg));
    uint64_t obj = cc_temp(t);
    CFunc *c = 0;
    for (int i = 0; i < t->nmeth; i++) if (t->meth[i]->is_ctor && t->meth[i]->ty->nparams == 1) c = t->meth[i];
    CV arg = { .u = s };
    if (c) cc_call(c, &arg, 1, obj, 0); else cc_construct_default(t, obj);
    throw_value(t, obj);
}

static int catch_matches(CType *want, CType *got) {
    if (want == got) return 1;
    if (want->kind == TY_STRUCT && got->kind == TY_STRUCT) { for (CType *c = got; c; c = c->super) if (c == want) return 1; if (want->name && got->name && !strcmp(want->name, got->name)) return 1; }
    if (ty_is_int(want) && ty_is_int(got) && want->size == got->size) return 1;
    if (want->kind == TY_PTR && got->kind == TY_PTR && want->base->kind == got->base->kind) return 1;
    if (want->kind == TY_STRING && got->kind == TY_STRING) return 1;
    return 0;
}

/* ---- expressions ------------------------------------------------------------------------------------------------- */
static uint64_t assign_stack[16]; static int assign_sp;

static int is_unsigned_kind(CType *t) {
    return t->kind == TY_UCHAR || t->kind == TY_USHORT || t->kind == TY_UINT || t->kind == TY_ULONG || t->kind == TY_BOOL || t->kind == TY_PTR || t->kind == TY_NULLPTR;
}

static int64_t norm_int(CType *t, int64_t v) {
    switch (t->kind) {
    case TY_BOOL: return v != 0;
    case TY_CHAR: return (int8_t)v; case TY_UCHAR: return (uint8_t)v;
    case TY_SHORT: return (int16_t)v; case TY_USHORT: return (uint16_t)v;
    case TY_INT: case TY_ENUM: return (int32_t)v; case TY_UINT: return (uint32_t)v;
    }
    return v;
}

uint64_t cc_addr(CNode *n) {
    switch (n->k) {
    case C_VAR: return n->global ? (uint64_t)n->off : frame_at(n->flag) + (uint64_t)n->off;
    case C_DEREF: {
        if (n->flag == 1) return assign_stack[assign_sp - 1];
        CV v = eval(n->a);
        if (v.u < 4096) {
            if (n->a->ty && n->a->ty->kind == TY_REF) cc_error("a reference to nothing");
            cc_error("Segmentation fault: dereferencing a null pointer");
        }
        return v.u;
    }
    case C_MEMBER: return cc_addr(n->a) + (uint64_t)n->off;
    case C_STR: return (uint64_t)n->off;
    case C_THIS: {
        uint64_t f = frame_at(n->flag);
        return ld64(f - 16);
    }
    default: {
        /* an object-valued expression: where it is */
        CV v = eval(n);
        if (n->ty && n->ty->kind == TY_REF) return v.u;
        if (n->ty && (n->ty->kind == TY_STRUCT || n->ty->kind == TY_STRING || n->ty->kind == TY_VECTOR || n->ty->kind == TY_MAP ||
                      n->ty->kind == TY_SET || n->ty->kind == TY_PAIR || n->ty->kind == TY_LAMBDA || n->ty->kind == TY_ARRAY)) return v.u;
        /* a scalar: kept in a temporary so it has an address */
        uint64_t t = cc_temp(n->ty);
        cc_store(n->ty, t, v);
        return t;
    }
    }
}

static int is_objty(CType *t) {
    int k = t->kind;
    return k == TY_STRUCT || k == TY_STRING || k == TY_VECTOR || k == TY_MAP || k == TY_SET || k == TY_PAIR || k == TY_LAMBDA || k == TY_ARRAY;
}

CV cc_lib_builtin(CNode *n);

static CV eval_args_call(CNode *n, CV *args) {
    for (int i = 0; i < n->nk; i++) args[i] = eval(n->kids[i]);
    return (CV){ .u = 0 };
}

static CV eval(CNode *n) {
    CV v, a, b;
    v.u = 0;
    switch (n->k) {
    case C_NUM: v.i = n->ival; return v;
    case C_FNUM: v.d = n->fval; return v;
    case C_STR: v.u = (uint64_t)n->off; return v;
    case C_VAR: case C_MEMBER: case C_DEREF: {
        uint64_t ad = cc_addr(n);
        if (is_objty(n->ty)) { v.u = ad; return v; }
        if (n->ty->kind == TY_FUNC) { v.u = ad; return v; }
        return cc_load(n->ty, ad);
    }
    case C_THIS: v.u = cc_addr(n); return v;
    case C_ADDR:
        if (n->a->k == C_FUNCADDR) return eval(n->a);
        if (n->a->ty && n->a->ty->kind == TY_FUNC) { if (n->a->k == C_DEREF) return eval(n->a->a); return eval(n->a); }
        v.u = cc_addr(n->a);
        return v;
    case C_FUNCADDR: v.i = n->fn ? cc_func_index(n->fn) : n->ival; return v;
    case C_ASSIGN: {
        uint64_t ad;
        CType *t = n->ty;
        ad = cc_addr(n->a);
        if (t->kind == TY_STRING && n->op == C_ADD) {
            /* s += x */
            CNode *r = n->b;
            uint64_t len; const char *p0 = cc_str_ptr(ad, &len);
            (void)p0;
            CV x = eval(r);
            char one[2]; const char *add; uint64_t an;
            if (r->ty->kind == TY_STRING) add = cc_str_ptr(x.u, &an);
            else if (ty_is_int(r->ty)) { one[0] = (char)x.i; add = one; an = 1; }
            else { uint64_t pa = x.u; add = (const char *)cc_at(pa, 1, 0); an = strlen(add); }
            char *tmp = rt_alloc_leaf((size_t)(len + an) + 1);
            memcpy(tmp, cc_str_ptr(ad, 0), (size_t)len); memcpy(tmp + len, add, (size_t)an);
            cc_str_set(ad, tmp, len + an);
            v.u = ad;
            return v;
        }
        if (n->op) { if (assign_sp >= 16) cc_error("assignments nested too deeply"); assign_stack[assign_sp++] = ad; }
        CV x = eval(n->b);
        if (n->op) assign_sp--;
        if (is_objty(t)) {
            if (t->kind == TY_LAMBDA) memcpy(cc_at(ad, 16, 1), cc_at(x.u, 16, 0), 16);
            else cc_assign_value(t, ad, x.u);
            v.u = ad;
            return v;
        }
        cc_store(t, ad, x);
        return cc_load(t, ad);
    }
    case C_CAST: {
        CType *from = n->a->ty, *to = n->ty;
        if (to->kind == TY_VOID) { eval(n->a); return v; }
        if (to->kind == TY_STRING) {
            uint64_t s = cc_temp(ty_string);
            a = eval(n->a);
            if (n->flag == 1) { char c = (char)a.i; cc_str_set(s, &c, 1); }
            else { if (a.u < 4096) cc_error("a string made from a null pointer"); const char *p = (const char *)cc_at(a.u, 1, 0); uint64_t len = 0; while (cc_at(a.u + len, 1, 0) && p[len]) len++; cc_str_set(s, p, len); }
            v.u = s;
            return v;
        }
        if (to->kind == TY_STRUCT && n->flag == 2) return eval(n->a);     /* derived to base: the same address */
        if (to->kind == TY_PAIR && n->flag == 3) {
            uint64_t s = cc_temp(to), src = eval(n->a).u;
            CType *f0 = from->mem[0].ty, *f1 = from->mem[1].ty;
            CV x0 = cc_load(f0, src), x1 = cc_load(f1, src + (uint64_t)from->mem[1].offset);
            CType *t0 = to->mem[0].ty, *t1 = to->mem[1].ty;
            if (ty_is_flt(t0) && !ty_is_flt(f0)) x0.d = (double)x0.i;
            if (ty_is_flt(t1) && !ty_is_flt(f1)) x1.d = (double)x1.i;
            if (is_objty(t0)) cc_copy_value(t0, s, x0.u); else cc_store(t0, s, x0);
            if (is_objty(t1)) cc_copy_value(t1, s + (uint64_t)to->mem[1].offset, x1.u); else cc_store(t1, s + (uint64_t)to->mem[1].offset, x1);
            v.u = s;
            return v;
        }
        if (to->kind == TY_LAMBDA) {
            /* a function pointer as a std::function */
            uint64_t s = cc_temp(to);
            a = eval(n->a);
            st64(s, a.u); st64(s + 8, 0);
            v.u = s;
            return v;
        }
        if (to->kind == TY_BOOL && from->kind == TY_ISTREAM) { extern int cc_stream_ok(void); eval(n->a); v.i = cc_stream_ok(); return v; }
        a = eval(n->a);
        if (ty_is_flt(to)) {
            if (ty_is_flt(from)) v.d = to->kind == TY_FLOAT ? (double)(float)a.d : a.d;
            else if (from->kind == TY_ULONG) v.d = (double)a.u;
            else v.d = (double)a.i;
            if (to->kind == TY_FLOAT) v.d = (double)(float)v.d;
            return v;
        }
        if (ty_is_flt(from)) {
            double d = a.d;
            if (to->kind == TY_BOOL) { v.i = d != 0; return v; }
            if (d != d) { v.i = to->kind == TY_INT ? INT32_MIN : INT64_MIN; return v; }
            if (to->kind == TY_ULONG) v.u = d >= 9.2233720368547758e18 ? (uint64_t)(d - 9.2233720368547758e18) + 0x8000000000000000ull : (uint64_t)(d < 0 ? (int64_t)d : (int64_t)d);
            else if (d >= 9.2e18) v.i = INT64_MAX; else if (d <= -9.2e18) v.i = INT64_MIN; else v.i = (int64_t)d;
            v.i = norm_int(to, v.i);
            return v;
        }
        if (to->kind == TY_BOOL) { v.i = a.u != 0; return v; }
        v.i = norm_int(to, a.i);
        return v;
    }
    case C_COND: return eval(n->a).u ? eval(n->b) : eval(n->c);
    case C_COMMA: eval(n->a); return eval(n->b);
    case C_LOGAND: v.i = eval(n->a).u && eval(n->b).u; return v;
    case C_LOGOR: v.i = eval(n->a).u || eval(n->b).u; return v;
    case C_NOT: v.i = !eval(n->a).u; return v;
    case C_BITNOT: v.i = norm_int(n->ty, ~eval(n->a).i); return v;
    case C_NEG: a = eval(n->a); if (ty_is_flt(n->ty)) v.d = -a.d; else v.i = norm_int(n->ty, (int64_t)(0 - a.u)); return v;
    case C_ADD: case C_SUB: case C_MUL: case C_DIV: case C_MOD: case C_SHL: case C_SHR: case C_BAND: case C_BOR: case C_BXOR: {
        a = eval(n->a); b = eval(n->b);
        CType *t = n->ty;
        if (ty_is_flt(t)) {
            switch (n->k) {
            case C_ADD: v.d = a.d + b.d; break; case C_SUB: v.d = a.d - b.d; break;
            case C_MUL: v.d = a.d * b.d; break; case C_DIV: v.d = a.d / b.d; break; default: break;
            }
            if (t->kind == TY_FLOAT) v.d = (double)(float)v.d;
            return v;
        }
        int uns = t->kind == TY_UINT || t->kind == TY_ULONG;
        switch (n->k) {
        case C_ADD: v.u = a.u + b.u; break;
        case C_SUB: v.u = a.u - b.u; break;
        case C_MUL: v.u = a.u * b.u; break;
        case C_DIV: case C_MOD:
            if (!b.u) cc_error("Floating point exception: integer division by zero");
            if (uns) { uint64_t x = t->kind == TY_UINT ? (uint32_t)a.u : a.u, y = t->kind == TY_UINT ? (uint32_t)b.u : b.u; v.u = n->k == C_DIV ? x / y : x % y; }
            else if (a.i == INT64_MIN && b.i == -1) v.i = n->k == C_DIV ? INT64_MIN : 0;
            else v.i = n->k == C_DIV ? a.i / b.i : a.i % b.i;
            break;
        case C_SHL: v.u = a.u << (b.u & 63); break;
        case C_SHR:
            if (uns) v.u = (t->kind == TY_UINT ? (uint32_t)a.u : a.u) >> (b.u & 63);
            else v.i = a.i >> (b.u & 63);
            break;
        case C_BAND: v.u = a.u & b.u; break;
        case C_BOR: v.u = a.u | b.u; break;
        case C_BXOR: v.u = a.u ^ b.u; break;
        }
        v.i = norm_int(t, v.i);
        return v;
    }
    case C_EQ: case C_NE: case C_LT: case C_LE: case C_GT: case C_GE: {
        a = eval(n->a); b = eval(n->b);
        CType *t = n->a->ty;
        int c;
        if (ty_is_flt(t)) c = a.d < b.d ? -1 : a.d > b.d ? 1 : a.d == b.d ? 0 : 2;
        else if (is_unsigned_kind(t)) c = a.u < b.u ? -1 : a.u > b.u;
        else c = a.i < b.i ? -1 : a.i > b.i;
        switch (n->k) {
        case C_EQ: v.i = c == 0; break; case C_NE: v.i = c != 0; break;
        case C_LT: v.i = c == -1; break; case C_LE: v.i = c == -1 || c == 0; break;
        case C_GT: v.i = c == 1; break; default: v.i = c == 1 || c == 0; break;
        }
        return v;
    }
    case C_PTRADD: {
        a = eval(n->a); b = eval(n->b);
        uint64_t es = n->ty->base && n->ty->base->size > 0 ? (uint64_t)n->ty->base->size : 1;
        v.u = a.u + (uint64_t)(b.i * (int64_t)es);
        return v;
    }
    case C_PTRDIFF: a = eval(n->a); b = eval(n->b); v.i = (a.i - b.i) / (n->ival ? n->ival : 1); return v;
    case C_PREINC: case C_POSTINC: {
        uint64_t ad = cc_addr(n->a);
        CType *t = n->ty;
        CV old = cc_load(t, ad), nw;
        if (ty_is_flt(t)) nw.d = old.d + (double)n->ival;
        else if (t->kind == TY_BOOL) nw.i = n->ival > 0 ? 1 : !old.i;
        else nw.i = norm_int(t, old.i + n->ival);
        cc_store(t, ad, nw);
        return n->k == C_PREINC ? cc_load(t, ad) : old;
    }
    case C_CALL: {
        CV args[48];
        int nargs = n->nk < 48 ? n->nk : 48;
        CType *rt = n->ty;
        uint64_t rto = rt && is_objty(rt) && rt->kind != TY_ARRAY ? cc_temp(rt) : 0;
        if (n->fn) {
            for (int i = 0; i < nargs; i++) args[i] = eval(n->kids[i]);
            cc_line = n->line;
            CV r = cc_call(n->fn, args, nargs, 0, rto);
            cc_line = n->line;
            if (rto) r.u = rto;
            return r;
        }
        CV callee = eval(n->a);
        eval_args_call(n, args);
        cc_line = n->line;
        CV r = cc_call_value(callee, n->a->ty, args, nargs, rto);
        cc_line = n->line;
        if (rto) r.u = rto;
        return r;
    }
    case C_METHOD: {
        uint64_t self = cc_addr(n->a);
        if (self < 4096) cc_error("Segmentation fault: calling %s() through a null pointer", n->fn->name);
        CV args[48];
        int nargs = n->nk < 48 ? n->nk : 48;
        for (int i = 0; i < nargs; i++) args[i] = eval(n->kids[i]);
        CType *rt = n->ty;
        uint64_t rto = rt && is_objty(rt) && rt->kind != TY_ARRAY ? cc_temp(rt) : 0;
        CFunc *f = n->flag || n->fn->is_virtual ? virtual_target(n->fn, self) : n->fn;
        cc_line = n->line;
        CV r = cc_call(f, args, nargs, self, rto);
        cc_line = n->line;
        if (rto) r.u = rto;
        return r;
    }
    case C_BUILTIN: return cc_lib_builtin(n);
    case C_NEW: {
        CType *t = n->ty->base;
        if (n->flag) {
            int64_t cnt = eval(n->a).i;
            if (cnt < 0) cc_error("new[] with a negative size");
            uint64_t sz = (uint64_t)cnt * (uint64_t)(t->size ? t->size : 1);
            uint64_t p = cc_malloc(sz + 16);
            if (!p) cc_error("out of memory (new[] of %lld)", (long long)cnt);
            st64(p, (uint64_t)cnt);
            p += 16;
            if (t->kind == TY_STRUCT) for (int64_t i = 0; i < cnt; i++) cc_construct_default(t, p + (uint64_t)i * (uint64_t)t->size);
            v.u = p;
            return v;
        }
        uint64_t p = cc_malloc((uint64_t)(t->size ? t->size : 1));
        if (!p) cc_error("out of memory (new %s)", t->name ? t->name : "");
        cc_init_at(p, t, n->b);
        v.u = p;
        return v;
    }
    case C_DELETE: {
        uint64_t p = eval(n->a).u;
        if (!p) return v;
        CType *t = n->a->ty->base;
        if (n->flag) {
            uint64_t base = p - 16, cnt = ld64(base);
            if (t->kind == TY_STRUCT || t->owns) for (uint64_t i = cnt; i > 0; i--) cc_destroy(t, p + (i - 1) * (uint64_t)t->size);
            cc_free(base);
            return v;
        }
        if (t->kind == TY_STRUCT && t->polymorphic) {
            CType *dyn = cc_class_by_id((int)ld64(p));
            if (dyn) t = dyn;
        }
        if (t->kind == TY_STRUCT || t->owns) cc_destroy(t, p);
        cc_free(p);
        return v;
    }
    case C_LAMBDA: {
        uint64_t s = cc_temp(n->ty);
        st64(s, (uint64_t)cc_func_index(n->fn));
        st64(s + 8, fp);
        v.u = s;
        return v;
    }
    case C_INIT: {
        uint64_t s = cc_temp(n->ty);
        cc_init_at(s, n->ty, n);
        v.u = s;
        return v;
    }
    case C_COPY: {
        if (n->flag == 2) { uint64_t s = cc_temp(n->ty); cc_init_at(s, n->ty, n->a); v.u = s; return v; }
        CType *t = n->ty->base;
        uint64_t s = cc_temp(t);
        store_value(t, s, n->a);
        v.u = s;
        return v;
    }
    case C_THROW: {
        if (!n->a) {
            if (!thrown_ty) cc_error("throw; with no exception being handled");
            CType *t = thrown_ty; uint64_t src = thrown_at;
            throw_value(t, src);
        }
        CType *t = n->a->ty->kind == TY_REF ? n->a->ty->base : n->a->ty;
        if (t->kind == TY_ARRAY) {
            /* throw "message": a const char* */
            CType *pt = ty_ptr(ty_char);
            uint64_t s = cc_temp(pt); st64(s, eval(n->a).u);
            throw_value(pt, s);
        }
        uint64_t src = cc_addr(n->a);
        throw_value(t, src);
        return v;
    }
    case C_NOP: return v;
    }
    cc_error("can't evaluate this expression (%d)", n->k);
}

CV cc_eval(CNode *n) { return eval(n); }

/* ---- statements ----------------------------------------------------------------------------------------------------- */
typedef struct BlockCtx { CNode *blk; uint64_t live; uint64_t base; struct BlockCtx *up; } BlockCtx;
static BlockCtx *cur_blk;

static void block_end(BlockCtx *bc) {
    CNode *b = bc->blk;
    for (int i = b->ndtors - 1; i >= 0; i--) {
        if (i < 64 && !(bc->live & (1ull << i))) continue;
        uint64_t a = bc->base + (uint64_t)b->dtors[i];
        cc_destroy(b->dtor_ty[i], a);
        memset(cc_mem + a, 0, (size_t)b->dtor_ty[i]->size);
    }
}

static void mark_live(CNode *d) {
    for (BlockCtx *bc = cur_blk; bc; bc = bc->up) {
        if (bc->base != fp) return;
        for (int i = 0; i < bc->blk->ndtors && i < 64; i++) if (bc->blk->dtors[i] == (int)d->off) { bc->live |= 1ull << i; return; }
    }
}

static int exec_block(CNode *n) {
    BlockCtx bc = { n, 0, fp, cur_blk };
    int own = n->ndtors > 0;
    if (own) cur_blk = &bc;
    int r = X_NORMAL;
    for (int i = 0; i < n->nk; i++) {
        r = exec(n->kids[i]);
        if (r) break;
    }
    if (own) { cur_blk = bc.up; block_end(&bc); }
    return r;
}

static int loop_body(CNode *body) {
    int r = exec(body);
    if (r == X_BREAK) return X_BREAK;
    if (r == X_RETURN) return X_RETURN;
    return X_NORMAL;
}

static int cond(CNode *c) {
    int mark = ntemps;
    int r = eval(c).u != 0;
    temps_release(mark);
    return r;
}

static int exec(CNode *n) {
    if ((++ticks & 1023) == 0) rt_check();
    cc_line = n->line;
    switch (n->k) {
    case C_BLOCK: return exec_block(n);
    case C_EXPR: { int mark = ntemps; eval(n->a); temps_release(mark); return X_NORMAL; }
    case C_DECL: {
        uint64_t ad = n->global ? (uint64_t)n->off : fp + (uint64_t)n->off;
        if (n->flag == 1) { if (n->ival) return X_NORMAL; n->ival = 1; }
        int mark = ntemps;
        if (!n->global && n->ty->kind != TY_REF) memset(cc_at(ad, (uint64_t)(n->ty->size > 0 ? n->ty->size : 1), 1), 0, (size_t)(n->ty->size > 0 ? n->ty->size : 1));
        if (n->init) cc_init_at(ad, n->ty, n->init);
        else cc_construct_default(n->ty, ad);
        temps_release(mark);
        if (!n->global) mark_live(n);
        return X_NORMAL;
    }
    case C_IF:
        if (cond(n->a)) return exec(n->b);
        if (n->c) return exec(n->c);
        return X_NORMAL;
    case C_WHILE:
        while (cond(n->a)) {
            int r = loop_body(n->b);
            if (r == X_BREAK) break;
            if (r == X_RETURN) return r;
            if ((++ticks & 255) == 0) rt_check();
        }
        return X_NORMAL;
    case C_DO:
        do {
            int r = loop_body(n->b);
            if (r == X_BREAK) break;
            if (r == X_RETURN) return r;
            if ((++ticks & 255) == 0) rt_check();
        } while (cond(n->a));
        return X_NORMAL;
    case C_FOR:
        if (n->a) exec(n->a);
        for (;;) {
            if (n->b && !cond(n->b)) break;
            int r = loop_body(n->d);
            if (r == X_BREAK) break;
            if (r == X_RETURN) return r;
            if (n->c) { int mark = ntemps; eval(n->c); temps_release(mark); }
            if ((++ticks & 255) == 0) rt_check();
        }
        return X_NORMAL;
    case C_RANGEFOR: {
        int mark = ntemps;
        CType *rt = n->a->ty;
        uint64_t ra = rt->kind == TY_REF ? eval(n->a).u : cc_addr(n->a);
        if (rt->kind == TY_REF) rt = rt->base;
        CType *et = (CType *)n->d;
        if (!et) et = rt->kind == TY_MAP ? ty_pair(rt->base, rt->base2) : rt->kind == TY_STRING ? ty_char : rt->base;
        uint64_t es = (uint64_t)et->size;
        CType *vt = n->ty;
        uint64_t va = fp + (uint64_t)n->off;
        int r = X_NORMAL;
        for (uint64_t i = 0; ; i++) {
            uint64_t data, cnt;
            if (rt->kind == TY_ARRAY) { data = ra; cnt = (uint64_t)rt->len; }
            else { data = ld64(ra); cnt = ld64(ra + 8); }
            if (i >= cnt) break;
            uint64_t ea = data + i * es;
            if (n->flag) st64(va, ea);
            else if (is_objty(vt)) { cc_destroy(vt, va); memset(cc_mem + va, 0, (size_t)vt->size); cc_copy_value(vt, va, ea); }
            else {
                CV x = cc_load(et, ea);
                if (ty_is_flt(vt) && !ty_is_flt(et)) x.d = (double)x.i;
                else if (!ty_is_flt(vt) && ty_is_flt(et)) x.i = (int64_t)x.d;
                cc_store(vt, va, x);
            }
            r = loop_body(n->b);
            if (r == X_BREAK) { r = X_NORMAL; break; }
            if (r == X_RETURN) break;
            if ((++ticks & 255) == 0) rt_check();
        }
        if (!n->flag && is_objty(vt)) { cc_destroy(vt, va); memset(cc_mem + va, 0, (size_t)vt->size); }
        temps_release(mark);
        return r;
    }
    case C_SWITCH: {
        int mark = ntemps;
        int64_t x = eval(n->a).i;
        temps_release(mark);
        CNode *body = n->b;
        if (body->k != C_BLOCK) return X_NORMAL;
        int start = -1, dflt = -1;
        for (int i = 0; i < body->nk; i++) {
            CNode *k = body->kids[i];
            if (k->k != C_CASE) continue;
            if (k->flag) dflt = i;
            else if (x >= k->case_lo && x <= k->case_hi && start < 0) start = i;
        }
        if (start < 0) start = dflt;
        if (start < 0) return X_NORMAL;
        BlockCtx bc = { body, 0, fp, cur_blk };
        if (body->ndtors) cur_blk = &bc;
        int r = X_NORMAL;
        for (int i = start; i < body->nk; i++) { r = exec(body->kids[i]); if (r) break; }
        if (body->ndtors) { cur_blk = bc.up; block_end(&bc); }
        if (r == X_BREAK) r = X_NORMAL;
        return r;
    }
    case C_CASE: case C_NOP: return X_NORMAL;
    case C_BREAK: return X_BREAK;
    case C_CONTINUE: return X_CONTINUE;
    case C_RETURN:
        if (n->a) {
            int mark = ntemps;
            CType *t = n->a->ty;
            if (ret_to_addr && is_objty(t) && t->kind != TY_LAMBDA) {
                uint64_t src = eval(n->a).u;
                cc_copy_value(t->kind == TY_REF ? t->base : t, ret_to_addr, src);
                ret_val.u = ret_to_addr;
            } else if (ret_to_addr && t->kind == TY_LAMBDA) {
                uint64_t src = eval(n->a).u;
                memcpy(cc_mem + ret_to_addr, cc_mem + src, 16);
                ret_val.u = ret_to_addr;
            } else ret_val = eval(n->a);
            temps_release(mark);
        }
        return X_RETURN;
    case C_TRY: {
        Catch c;
        c.prev = catching;
        volatile uint64_t saved_fp = fp, saved_sp = sp, saved_rt = ret_to_addr;
        volatile int saved_temps = ntemps, saved_depth = depth, saved_asp = assign_sp;
        BlockCtx *volatile saved_blk = cur_blk;
        catching = &c;
        if (rt_setjmp(&c.jb) == 0) {
            int r = exec(n->a);
            catching = c.prev;
            return r;
        }
        catching = c.prev;
        fp = saved_fp; sp = saved_sp; ret_to_addr = saved_rt; depth = saved_depth; assign_sp = saved_asp; cur_blk = saved_blk;
        temps_release(saved_temps);
        CType *t = thrown_ty; uint64_t ex = thrown_at;
        for (int i = 0; i < n->nk; i++) {
            CNode *h = n->kids[i];
            if (!h->flag && !catch_matches(h->ty, t)) continue;
            if (h->name) {
                uint64_t va = fp + (uint64_t)h->off;
                memset(cc_mem + va, 0, (size_t)h->ty->size);
                if (is_objty(h->ty)) cc_copy_value(h->ty, va, ex);
                else cc_store(h->ty, va, cc_load(t, ex));
            }
            int r = exec(h->a);
            if (h->name && is_objty(h->ty)) cc_destroy(h->ty, fp + (uint64_t)h->off);
            return r;
        }
        throw_value(t, ex);
        return X_NORMAL;
    }
    case C_THROW: eval(n); return X_NORMAL;
    }
    eval(n);
    return X_NORMAL;
}

/* ---- the program ---------------------------------------------------------------------------------------------- */
int cc_lib_exit_code(int *code);

int cc_run_program(CFunc *mainf, CNode **ginit, int nginit) {
    extern uint64_t cc_data_top(void);
    /* the heap from the end of the data to the stack */
    heap_lo = (cc_data_top() + 4095) & ~4095ull;
    uint64_t stack = cc_memsz / 8;
    if (stack > (8u << 20)) stack = 8u << 20;
    stack_lo = cc_memsz - stack;
    heap_top = heap_lo; heap_hi = stack_lo - 65536;
    memset(bins, 0, sizeof bins);
    in_use = 0;
    sp = cc_memsz; fp = sp;
    ntemps = 0; temps = 0; temps_cap = 0; catching = 0; thrown_ty = 0; depth = 0; assign_sp = 0; cur_blk = 0;
    rt_root((void **)&temps);
    empty_cstr = cc_malloc(16);
    rt_jmp_t done;
    extern rt_jmp_t *cc_exit_jmp;
    cc_exit_jmp = &done;
    int code = 0;
    if (rt_setjmp(&done) == 0) {
        for (int i = 0; i < nginit; i++) exec(ginit[i]);
        CV args[2] = { { .i = 1 }, { .u = 0 } };
        CV r = cc_call(mainf, args, mainf->ty->nparams > 2 ? 2 : mainf->ty->nparams, 0, 0);
        code = mainf->ty->ret->kind == TY_VOID ? 0 : (int)r.i;
    } else cc_lib_exit_code(&code);
    cc_flush();
    return code;
}
