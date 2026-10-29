/* ===========================================================================
 *  BoltOS Mini  -  mini/py.h
 *
 *  Python, for the Code app: the parts of Python 3 people write programs
 *  with. Integers of any size, floats, strings, lists, tuples, dicts, sets,
 *  functions with defaults, *args and **kwargs, closures, lambdas, classes
 *  with inheritance and the special methods, exceptions with try/except/
 *  finally, comprehensions, f-strings and format(), slicing, with, and the
 *  modules math, random, time, sys, os, string, json and collections. A
 *  program's own .py files next to it import too.
 *
 *  Source is read into tokens (py_lex.c), the tokens into a tree
 *  (py_parse.c), which py_eval.c walks. Names are found by where they are
 *  in the source, not at run time: a function's own names are slots in its
 *  frame, an outer function's are a known number of frames out, and only
 *  module-level names go through a dict. py_obj.c has the objects and
 *  py_lib.c the built-in functions, methods and modules.
 *
 *  Everything lives on rt.c's collected heap. Values are 16 bytes: None,
 *  booleans, integers that fit 64 bits and floats are held in the value;
 *  everything else is a pointer to an object whose first byte says what
 *  it is.
 * ===========================================================================*/
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "rt.h"

/* ---- values -------------------------------------------------------------------------------- */
enum { V_NONE, V_BOOL, V_INT, V_FLOAT, V_OBJ, V_UNBOUND };
typedef struct Obj Obj;
typedef struct { uint8_t t; union { int64_t i; double f; Obj *o; } u; } V;

enum {                                    /* an object's first byte                       */
    O_STR = 1, O_LIST, O_TUPLE, O_DICT, O_SET, O_BIG, O_FUNC, O_NATIVE, O_BOUND, O_CLASS, O_INST,
    O_MODULE, O_RANGE, O_ITER, O_SLICE, O_FILE, O_PROP, O_STATICM, O_CLASSM, O_SUPER, O_CELLFRAME,
};
struct Obj { uint8_t type; };

typedef struct { uint8_t type, interned; uint32_t len, hash; char s[]; } Str;
typedef struct { uint8_t type; uint32_t len, cap; V *v; } List;
typedef struct { uint8_t type; uint32_t len; uint64_t hash; V v[]; } Tuple;
typedef struct { V key, val; uint64_t hash; } DEnt;
typedef struct { uint8_t type, frozen; uint32_t n, used, cap, icap; DEnt *e; int32_t *idx; } Dict;   /* also a set */
typedef struct { uint8_t type; int8_t sign; uint32_t n; uint32_t d[]; } Big;                       /* sign * d, base 2^32 */

typedef struct Node Node;
typedef struct Frame Frame;
typedef struct Class Class;

typedef V (*native_fn)(V *a, int n, V *kwn, V *kwv, int nk);
typedef struct { uint8_t type; const char *name; native_fn fn; V self; int has_self; } Native;

typedef struct {
    uint8_t type;
    Str    *name;
    Node   *def;                          /* the def or lambda node                         */
    Frame  *env;                          /* where it was made: its outer function's frame  */
    Dict   *globals;
    V      *defaults; int ndefaults;
    Dict   *kwdefaults;
    Class  *owner;                        /* the class it was defined in, for super()       */
} Func;

typedef struct { uint8_t type; V self; V fn; } Bound;

struct Class {
    uint8_t type;
    int     builtin;                      /* O_* of the built-in type it stands for, or 0   */
    Str    *name;
    Class **bases; int nbases;
    Class **mro;   int nmro;
    Dict   *dict;
    int     is_exc;
};

typedef struct { uint8_t type; Class *cls; Dict *dict; V native; } Inst;   /* native: an exception's args, a subclassed dict */
typedef struct { uint8_t type; Str *name; Dict *dict; } Module;
typedef struct { uint8_t type; int64_t start, stop, step; } Range;
typedef struct { uint8_t type; V lo, hi, step; } Slice;
typedef struct { uint8_t type; V get, set; } Prop;
typedef struct { uint8_t type; V fn; } Wrap;                     /* staticmethod, classmethod */
typedef struct { uint8_t type; Class *cls; V self; } Super;
typedef struct { uint8_t type, mode; Str *path; List *lines; char *buf; uint32_t len, cap, pos; int closed; } File;

enum { IT_SEQ, IT_STR, IT_RANGE, IT_DICT, IT_ENUM, IT_ZIP, IT_MAP, IT_FILTER, IT_REV, IT_USER, IT_COUNT };
typedef struct {
    uint8_t type, kind, dictpart;          /* dictpart: 0 keys, 1 values, 2 items             */
    V       src, fn;                       /* what it walks; map's or filter's function       */
    int64_t i, n, step;
    V      *more; int nmore;               /* zip's and map's other iterators                 */
} Iter;

/* ---- the tree ---------------------------------------------------------------------------------- */
enum {
    /* expressions */
    N_CONST, N_NAME, N_BIN, N_UNARY, N_AND, N_OR, N_NOT, N_CMP, N_CALL, N_ATTR, N_INDEX, N_SLICE,
    N_LIST, N_TUPLE, N_DICT, N_SET, N_COMP, N_LAMBDA, N_IFEXP, N_FSTR, N_FMT, N_STAR, N_DSTAR, N_WALRUS,
    /* statements */
    N_EXPR, N_ASSIGN, N_AUG, N_IF, N_WHILE, N_FOR, N_BREAK, N_CONTINUE, N_PASS, N_RETURN, N_DEF,
    N_CLASS, N_TRY, N_RAISE, N_GLOBAL, N_NONLOCAL, N_IMPORT, N_FROM, N_DEL, N_ASSERT, N_WITH, N_BLOCK,
    /* parts of the above */
    N_KWARG, N_EXCEPT, N_ITEM,
};
/* where a name is */
enum { S_GLOBAL, S_LOCAL, S_OUTER, S_CLASS };

struct Node {
    uint8_t  k, op, scope, flags;
    int      line;
    Node    *a, *b, *c, *d;                /* operands, bodies                               */
    Node   **kids; int nk;                 /* lists: elements, statements, arguments          */
    Str     *s;                            /* a name                                          */
    uint8_t *ops;                          /* a comparison's operators                        */
    V        v;                            /* a constant                                      */
    int      slot, depth;                  /* a local's slot; an outer one's frames out       */
    /* defs and lambdas: */
    int      nslots, nparams, npos, star, dstar, nkwonly;
    Str    **params;                       /* nparams names, then *args', then **kwargs'      */
    Node   **defaults; int ndef;           /* for the last ndef positional parameters         */
    Node   **kwdefs;                       /* nkwonly defaults (or NULL each)                 */
    Str     *file;                         /* where it came from, for tracebacks              */
    Str    **slotnames;
};

struct Frame {
    Frame   *env;                          /* the outer function's frame                     */
    V       *slots;
    Dict    *ns;                           /* a class body's namespace                        */
    Dict    *globals;
    Func    *fn;
};

/* comparison and operator codes */
enum { C_LT, C_LE, C_GT, C_GE, C_EQ, C_NE, C_IN, C_NOTIN, C_IS, C_ISNOT };
enum { B_ADD, B_SUB, B_MUL, B_DIV, B_FLOORDIV, B_MOD, B_POW, B_LSHIFT, B_RSHIFT, B_AND, B_OR, B_XOR, B_MATMUL };
enum { U_NEG, U_POS, U_INV };

/* ---- the shared state ------------------------------------------------------------------------------ */
typedef struct {
    Dict   *builtins, *modules, *interned;
    Class  *t_object, *t_type, *t_int, *t_float, *t_str, *t_bool, *t_list, *t_tuple, *t_dict, *t_set,
           *t_none, *t_func, *t_range, *t_module, *t_file, *t_iter, *t_slice, *t_frozenset;
    Class  *e_base, *e_exc, *e_type, *e_value, *e_key, *e_index, *e_zero, *e_name, *e_attr, *e_runtime,
           *e_stop, *e_assert, *e_notimpl, *e_recursion, *e_os, *e_notfound, *e_import, *e_overflow,
           *e_arith, *e_lookup, *e_syntax, *e_unbound, *e_memory, *e_exit, *e_kbd, *e_eof, *e_indent;
    Str    *main_file;
    const char *main_src;
    /* the source of every file read, for tracebacks */
    Str    *files[16]; const char *srcs[16]; int nfiles;
    /* names used everywhere */
    Str    *s_init, *s_str, *s_repr, *s_len, *s_iter, *s_next, *s_getitem, *s_setitem, *s_delitem,
           *s_contains, *s_call, *s_eq, *s_ne, *s_lt, *s_le, *s_gt, *s_ge, *s_hash, *s_bool, *s_enter,
           *s_exit, *s_name, *s_main, *s_args, *s_missing, *s_new, *s_add, *s_class, *s_dict, *s_doc,
           *s_getattr, *s_setattr, *s_neg, *s_abs, *s_int, *s_float, *s_format, *s_module, *s_self, *s_ctor,
           *s_tb;
    List   *open_files;                     /* written when the program ends, if nobody closed them */
} PyState;
extern PyState *P;

/* the exception being raised, and the stack of try blocks */
typedef struct Handler { rt_jmp_t jb; struct Handler *prev; } Handler;
extern Handler *py_handler;
extern V py_exc;                            /* the exception in flight                    */

/* ---- calls into each part ------------------------------------------------------------------------- */
/* py_obj.c */
V    vnone(void);
V    vbool(int b);
V    vint(int64_t i);
V    vfloat(double f);
V    vobj(void *o);
static inline int   is_obj(V v, int type) { return v.t == V_OBJ && v.u.o->type == type; }
static inline Str  *as_str(V v)  { return (Str *)v.u.o; }
static inline List *as_list(V v) { return (List *)v.u.o; }
static inline Dict *as_dict(V v) { return (Dict *)v.u.o; }
Str   *str_new(const char *s, uint32_t n);
Str   *str_c(const char *s);
Str   *intern(const char *s, uint32_t n);
Str   *intern_c(const char *s);
V      vstr(const char *s);
V      vstrn(const char *s, uint32_t n);
uint64_t str_hash(Str *s);
List  *list_new(uint32_t cap);
void   list_push(List *l, V x);
Tuple *tuple_new(uint32_t n);
Dict  *dict_new(void);
Dict  *set_new(void);
int    dict_get(Dict *d, V key, V *out);
void   dict_set(Dict *d, V key, V val);
int    dict_del(Dict *d, V key);
int    dict_get_str(Dict *d, Str *key, V *out);
void   dict_set_str(Dict *d, Str *key, V val);
int    dict_next(Dict *d, uint32_t *pos, V *key, V *val);   /* in insertion order */
uint64_t hash_of(V v);
int    eq(V a, V b);
int    compare(V a, V b, int op);                          /* C_LT etc: 1/0, raises TypeError */
int    truthy(V v);
Class *type_of(V v);
const char *type_name(V v);
int    is_instance(V v, Class *c);
int    is_subclass(Class *a, Class *b);
Str   *to_str(V v);
Str   *to_repr(V v);
void   sb_init(void);
/* text builder */
typedef struct { char *b; uint32_t n, cap; } SB;
void   sb_new(SB *s, uint32_t cap);
void   sb_add(SB *s, const char *p, uint32_t n);
void   sb_addc(SB *s, char c);
void   sb_adds(SB *s, const char *p);
void   sb_addstr(SB *s, Str *x);
Str   *sb_str(SB *s);
void   repr_into(SB *s, V v, int repr);
Str   *format_value(V v, Str *spec);
Str   *percent_format(Str *fmt, V args);
/* integers of any size */
V      big_from_i64(int64_t i);
V      big_norm(Big *b);
V      int_add(V a, V b);
V      int_sub(V a, V b);
V      int_mul(V a, V b);
V      int_floordiv(V a, V b);
V      int_mod(V a, V b);
V      int_pow(V a, V b);
V      int_neg(V a);
V      int_shl(V a, int64_t k);
V      int_shr(V a, int64_t k);
V      int_bitop(V a, V b, int op);
int    int_cmp(V a, V b);
double int_to_double(V a);
V      int_from_double(double d);
V      int_parse(const char *s, int base, int *ok);
Str   *int_to_str(V a, int base);
int    is_int(V v);                                        /* an int, small or big       */
int    fits_i64(V v, int64_t *out);
uint64_t big_hash(Big *b);
/* errors */
void   raise(V exc) __attribute__((noreturn));
void   raise_new(Class *c, const char *fmt, ...) __attribute__((noreturn));
V      make_exc(Class *c, V msg);
Class *class_new(const char *name, Class *base, int builtin);
int    class_set_bases(Class *c, Class **bases, int n);   /* 0, or -1 if no MRO exists     */

/* py_eval.c */
V    call(V fn, V *a, int n, V *kwn, V *kwv, int nk);
V    call0(V fn);
V    call1(V fn, V x);
V    call2(V fn, V x, V y);
V    getattr(V obj, Str *name);
int  getattr_opt(V obj, Str *name, V *out);
void setattr(V obj, Str *name, V val);
int  lookup_special(V obj, Str *name, V *out);           /* a class's method, bound to obj */
V    iter_of(V v);
int  iter_next(V it, V *out);
V    get_item(V obj, V key);
void set_item(V obj, V key, V val);
V    binop(int op, V a, V b);
Module *import_module(Str *name, int line);
void py_set_line(int line);
int  py_line(void);

/* py_lex.c, py_parse.c */
Node *parse_program(const char *src, Str *file);

/* py_lib.c */
void lib_init(void);
V    native(const char *name, native_fn fn);
V    method_of(V self, Str *name, int *found);          /* a built-in type's method          */
Module *builtin_module(Str *name);
V    list_sort(List *l, V key, int reverse);
V    seq_from(V it);                                       /* a list of what an iterable gives */
