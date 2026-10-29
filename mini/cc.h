/* ===========================================================================
 *  BoltOS Mini  -  mini/cc.h
 *
 *  C and C++ for the Code app. The source is preprocessed, read into
 *  tokens and parsed the way a compiler's front end does it -- every
 *  expression gets its type as it is parsed, conversions become explicit
 *  casts, every variable an address -- and the typed tree is then walked
 *  (cc_eval.c) over memory laid out as a real C program's is: one byte
 *  array, with a guard page at 0, the globals and string literals, the
 *  heap malloc() hands out, and the stack at the top. So pointers,
 *  arrays, structs, unions and casts behave as C says, and a bad pointer
 *  is a "Segmentation fault" with the line it happened on, not a crash.
 *
 *  C++ on top: classes with methods, constructors and destructors,
 *  inheritance and virtual functions, references, new and delete,
 *  operator overloading, function overloading and default arguments,
 *  templates (made by reading the template's tokens again with its
 *  parameters filled in), lambdas, range-for, auto, try/catch/throw, and
 *  from the library std::string, vector, map, set, pair, iostream with
 *  iomanip, and the common <algorithm>s. Containers keep their elements
 *  side by side, so an iterator is a plain pointer.
 * ===========================================================================*/
#pragma once
#include <stdint.h>
#include "rt.h"

/* ---- types --------------------------------------------------------------------------------- */
enum {
    TY_VOID, TY_BOOL, TY_CHAR, TY_UCHAR, TY_SHORT, TY_USHORT, TY_INT, TY_UINT, TY_LONG, TY_ULONG,
    TY_FLOAT, TY_DOUBLE, TY_ENUM, TY_PTR, TY_ARRAY, TY_FUNC, TY_STRUCT, TY_REF, TY_NULLPTR,
    /* the C++ library's, kept in program memory as { data, size, cap } */
    TY_STRING, TY_VECTOR, TY_MAP, TY_SET, TY_PAIR, TY_OSTREAM, TY_ISTREAM, TY_LAMBDA,
};

typedef struct CType CType;
typedef struct CFunc CFunc;
typedef struct CNode CNode;

typedef struct { char *name; CType *ty; int offset; } CMember;

struct CType {
    int kind, size, align, is_union, is_const;
    CType *base;                       /* pointer, array, reference: what to; vector, set: element; map: key */
    CType *base2;                      /* map: value; pair: second (base is first)                          */
    int len;                           /* array: elements (-1 not known yet)                                 */
    char *name;                        /* struct, class                                                      */
    CMember *mem; int nmem, complete;
    CFunc **meth; int nmeth;           /* a class's methods, constructors and its destructor                 */
    CType *super;                      /* the class it derives from                                          */
    int polymorphic, class_id;         /* has virtual methods: its first 8 bytes say which class it is       */
    int owns;                          /* holds a container somewhere: copying and destroying it isn't memcpy */
    CType *ret; CType **params; int nparams, variadic;   /* function                                           */
    CType *origin;                     /* for an array decayed to a pointer, the array                       */
    CNode **minit; int nminit;         /* a class's member initializers (int x = 5; in its body)              */
    CFunc *dtor;                       /* a class's destructor                                               */
    int has_ctor;                      /* it declares a constructor: not an aggregate                        */
    CFunc *op_lt;                      /* operator< for it, a method or a free function: sorting, sets, maps  */
};

/* ---- the tree ---------------------------------------------------------------------------------- */
enum {
    /* expressions */
    C_NUM, C_FNUM, C_STR, C_VAR, C_ADDR, C_DEREF, C_MEMBER, C_ASSIGN, C_CAST, C_COND, C_COMMA, C_CALL,
    C_ADD, C_SUB, C_MUL, C_DIV, C_MOD, C_SHL, C_SHR, C_BAND, C_BOR, C_BXOR, C_EQ, C_NE, C_LT, C_LE,
    C_GT, C_GE, C_LOGAND, C_LOGOR, C_NOT, C_BITNOT, C_NEG, C_PREINC, C_POSTINC, C_PTRADD, C_PTRDIFF,
    C_BUILTIN, C_METHOD, C_NEW, C_DELETE, C_THIS, C_LAMBDA, C_COPY, C_FUNCADDR, C_INIT, C_VLA,
    /* statements */
    C_BLOCK, C_EXPR, C_IF, C_WHILE, C_DO, C_FOR, C_RANGEFOR, C_SWITCH, C_CASE, C_BREAK, C_CONTINUE,
    C_RETURN, C_DECL, C_GOTO, C_LABEL, C_TRY, C_THROW, C_NOP,
};

struct CNode {
    int k, line;
    CType *ty;                         /* an expression's type                                             */
    CNode *a, *b, *c, *d;
    CNode **kids; int nk;              /* arguments, statements                                            */
    int64_t ival; double fval;
    int64_t off;                       /* a local's frame offset, a global's address, a member's offset      */
    int global, op, flag;
    CFunc *fn;                         /* a call's function                                                */
    CNode *init;                       /* a declaration's initializer                                      */
    int *dtors; CType **dtor_ty; int ndtors;   /* a block's locals to destroy at its end: frame offsets   */
    char *name;
    int case_lo, case_hi;              /* switch: a case's value(s)                                        */
    CNode **cases; int ncases;         /* switch: its cases, in order; and default as flag                 */
};

struct CFunc {
    char *name;
    CType *ty;                         /* the function's type                                              */
    CType *owner;                      /* a method's class                                                 */
    int is_virtual, is_ctor, is_dtor, is_static, is_pure, is_const;
    char **pnames;
    CNode *body;
    int frame;                         /* bytes of locals                                                  */
    int *poff;                         /* the parameters' frame offsets                                    */
    CNode **pdefault;                  /* default arguments, per parameter (NULL: none)                    */
    int native;                        /* a library function: its number                                   */
    CNode *inits; int ninits;          /* a constructor's member initializers                              */
    int line;
    int ret_off;                       /* where a returned struct goes, in the frame                       */
    CFunc *next_overload;
};

/* ---- the program's memory ------------------------------------------------------------------------ */
typedef union { int64_t i; uint64_t u; double d; } CV;

extern uint8_t *cc_mem;
extern uint64_t cc_memsz;
void  cc_fault(uint64_t addr, uint64_t n, int write) __attribute__((noreturn));
static inline uint8_t *cc_at(uint64_t a, uint64_t n, int w) {
    if (a < 4096 || a + n > cc_memsz || a + n < a) cc_fault(a, n, w);
    return cc_mem + a;
}
uint64_t cc_malloc(uint64_t n);
void     cc_free(uint64_t a);
uint64_t cc_realloc(uint64_t a, uint64_t n);
uint64_t cc_heap_in_use(void);

/* values in memory */
CV   cc_load(CType *t, uint64_t a);
void cc_store(CType *t, uint64_t a, CV v);

/* containers and owned values */
void cc_copy_value(CType *t, uint64_t dst, uint64_t src);   /* into memory that holds nothing */
void cc_assign_value(CType *t, uint64_t dst, uint64_t src); /* over what's there              */
void cc_destroy(CType *t, uint64_t a);
uint64_t cc_temp(CType *t);                                  /* a temporary, freed at the statement's end */
void cc_str_set(uint64_t s, const char *p, uint64_t n);
const char *cc_str_ptr(uint64_t s, uint64_t *n);

/* the line being run, for errors */
extern int cc_line;
void cc_error(const char *fmt, ...) __attribute__((noreturn));

/* the library (cc_lib.c) */
enum { NB_NONE };
int    cc_lib_find(const char *name);                       /* a C library function's number, or 0 */
CV     cc_lib_call(int id, CNode *call, CV *args, int n);
const char *cc_lib_name(int id);
CType *cc_lib_type(int id);                                  /* its type, as the parser needs it */
void   cc_lib_init(void);
void   cc_out(const char *s, uint64_t n);                    /* stdout, buffered               */
void   cc_flush(void);
int    cc_format(char *out, int cap, const char *fmt, CV *args, int nargs, CType **types);  /* printf  */
int    cc_scanf(const char *fmt, CV *args, int nargs, int (*getc_)(void *), void *ctx);
int    cc_stdin_getc(void *ctx);
void   cc_stdin_unget(int c);

/* the evaluator (cc_eval.c) */
CV   cc_eval(CNode *n);
uint64_t cc_addr(CNode *n);
CV   cc_call(CFunc *f, CV *args, int n, uint64_t self, uint64_t ret_to);
int  cc_run_program(CFunc *mainf, CNode **ginit, int nginit);
void cc_init_at(uint64_t a, CType *t, CNode *init);
void cc_construct_default(CType *t, uint64_t a);
CV   cc_call_value(CV callee, CType *ct, CV *args, int n, uint64_t ret_to);   /* a lambda, a function pointer, a functor */
int  cc_compare(CType *t, uint64_t a, uint64_t b);                           /* <0 0 >0, with a class's operator< */
CFunc *cc_func_by_index(int i);
int  cc_func_index(CFunc *f);
extern CType *ty_manip;
void cc_lib_fix_type(CNode *n);                                             /* a generic library call's type */

/* types (cc.c) */
extern CType *ty_void, *ty_bool, *ty_char, *ty_uchar, *ty_short, *ty_ushort, *ty_int, *ty_uint, *ty_long,
             *ty_ulong, *ty_float, *ty_double, *ty_string, *ty_ostream, *ty_istream, *ty_nullptr;
CType *ty_ptr(CType *base);
CType *ty_ref(CType *base);
CType *ty_array(CType *base, int len);
CType *ty_vector(CType *elem);
CType *ty_set(CType *elem);
CType *ty_map(CType *k, CType *v);
CType *ty_pair(CType *a, CType *b);
int  ty_is_int(CType *t);
int  ty_is_flt(CType *t);
int  ty_is_num(CType *t);
int  ty_is_ptr(CType *t);                                    /* pointer or array               */
int  ty_owns(CType *t);
const char *ty_str(CType *t, char *buf, int cap);
CFunc *cc_find_method(CType *cls, const char *name, int virt_from_id);
CType *cc_class_by_id(int id);
