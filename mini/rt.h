/* ===========================================================================
 *  BoltOS Mini  -  mini/rt.h
 *
 *  Running a program from the Code app: Python (py.c), C and C++ (cc.c).
 *  The program runs on one of the spare cores, so the screen, the network
 *  and Stop carry on while it works; nothing it does can hang the boot core.
 *
 *  The two sides:
 *    - the boot core (the app) calls run_start, then run_poll every loop,
 *      reads what the program printed with run_output, answers input() with
 *      run_input, and can run_stop it;
 *    - the program's core calls the rt_* functions: memory, output, input,
 *      files (done for it by the boot core), and rt_check, which a running
 *      interpreter calls often, to stop when asked.
 *
 *  Memory is a region of its own (the low big window when it's free, or a
 *  piece of the kernel heap), never the kernel heap itself, which only the
 *  boot core may touch. In it: a stack of 8 MiB for deep recursion, and a
 *  heap with a garbage collector (rt_alloc), which Python uses for every
 *  object and the C interpreter for its tree and its program's memory.
 * ===========================================================================*/
#pragma once
#include <stdint.h>
#include <stddef.h>

enum { LANG_NONE, LANG_PY, LANG_C, LANG_CPP };
enum { RUN_IDLE, RUN_STARTING, RUN_RUNNING, RUN_INPUT, RUN_DONE };

/* ---- the boot core's side ------------------------------------------------- */
int  run_start(int lang, const char *src, uint32_t len, const char *path);   /* 0, or why in run_message */
void run_stop(void);
int  run_poll(void);                    /* every loop: 1 when there's something new to show       */
int  run_state(void);                   /* RUN_*                                                  */
int  run_output(char *out, int cap);    /* what was printed since the last call                   */
void run_input(const char *line);       /* the line input() waits for                             */
const char *run_message(void);          /* "Finished in 0.42 s", "Stopped", the error             */
int  run_error_line(void);              /* the program's line an error was on, or 0               */
int  run_ok(void);                      /* it finished without an error                           */
uint64_t run_elapsed_ms(void);
const char *run_prompt(void);           /* what input() asked with                                */
int  lang_of(const char *path);         /* by the name's ending                                   */
const char *lang_name(int lang);
int  run_dev(const char *cmd, char *out, int cap);   /* GET /run?... for testing from the Mac */

/* ---- the program's side ----------------------------------------------------- */
typedef struct { uint64_t r[22]; } rt_jmp_t;
int  rt_setjmp(rt_jmp_t *j) __attribute__((returns_twice));
void rt_longjmp(rt_jmp_t *j, int v) __attribute__((noreturn));

void *rt_alloc(size_t n);               /* zeroed; collected when nothing points to it            */
void *rt_alloc_leaf(size_t n);          /* the same, for memory that holds no pointers (text)     */
void *rt_realloc(void *p, size_t old, size_t n);
void  rt_root(void **p);                /* a pointer outside the heap the collector must follow   */
void  rt_gc(void);
uint64_t rt_heap_used(void);
uint64_t rt_heap_size(void);

void rt_write(const char *s, size_t n); /* to the output                                          */
void rt_puts(const char *s);
void rt_printf(const char *fmt, ...);
int  rt_readline(const char *prompt, char *buf, int cap);   /* input(): -1 when stopped            */
void rt_check(void);                    /* stops the program if Stop was pressed                  */
int  rt_stopping(void);
uint64_t rt_ms(void);
void rt_sleep_ms(uint64_t ms);
/* Files on the card, through the boot core: data comes back in an rt_alloc_leaf
 * buffer. 0, or -1 with the reason in *why. */
int  rt_file_read(const char *path, char **data, uint32_t *len, const char **why);
int  rt_file_write(const char *path, const char *data, uint32_t len, int append, const char **why);
const char *rt_cwd(void);               /* the folder the program's file is in                    */
/* An error ends the program: the message, and its line (0 if none). */
void rt_fail(int line, const char *fmt, ...) __attribute__((noreturn));
int  rt_stack_left(void);               /* bytes of stack under the current frame                 */
uint32_t rt_rand(void);
void rt_seed(uint64_t s);

/* The interpreters (each returns 0 when the program finished, or exits
 * through rt_fail). */
int py_main(const char *src, const char *path);
int cc_main(const char *src, const char *path, int cpp);

/* Numbers as text, exactly (numfmt.c). */
int    num_repr(double x, char *out, int cap);                 /* Python's repr: the shortest that reads back */
int    num_fixed(double x, int prec, char *out, int cap);      /* %.*f, correctly rounded                    */
int    num_exp(double x, int prec, char *out, int cap, int upper);   /* %.*e                                 */
int    num_general(double x, int prec, char *out, int cap, int upper, int alt);  /* %.*g                     */
double num_parse(const char *s, const char **end);            /* strtod, correctly rounded                  */
