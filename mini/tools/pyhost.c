/* ===========================================================================
 *  BoltOS Mini  -  mini/tools/pyhost.c
 *
 *  The Code app's Python and C/C++ on the Mac, for testing them against the
 *  real thing: the same interpreter files and collector the Pi runs, with
 *  rt.h's program side done here on top of stdio. Apple Silicon only (the
 *  collector's stack walk and the context switch are AArch64).
 *
 *    cc -O2 -Imini -o build/pyhost mini/tools/pyhost.c mini/gc.c mini/numfmt.c \
 *       mini/py_obj.c mini/py_parse.c mini/py_eval.c mini/py_lib.c mini/cc*.c
 *    build/pyhost prog.py [input lines...]     (or prog.c, prog.cpp)
 * ===========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include "rt.h"

__asm__(
    ".text\n.globl _rt_setjmp\n.p2align 2\n_rt_setjmp:\n"
    "  stp x19, x20, [x0, #0]\n  stp x21, x22, [x0, #16]\n  stp x23, x24, [x0, #32]\n"
    "  stp x25, x26, [x0, #48]\n  stp x27, x28, [x0, #64]\n  stp x29, x30, [x0, #80]\n"
    "  mov x2, sp\n  str x2, [x0, #96]\n"
    "  stp d8, d9, [x0, #104]\n  stp d10, d11, [x0, #120]\n  stp d12, d13, [x0, #136]\n  stp d14, d15, [x0, #152]\n"
    "  mov x0, #0\n  ret\n"
    ".globl _rt_longjmp\n.p2align 2\n_rt_longjmp:\n"
    "  ldp x19, x20, [x0, #0]\n  ldp x21, x22, [x0, #16]\n  ldp x23, x24, [x0, #32]\n"
    "  ldp x25, x26, [x0, #48]\n  ldp x27, x28, [x0, #64]\n  ldp x29, x30, [x0, #80]\n"
    "  ldr x2, [x0, #96]\n  mov sp, x2\n"
    "  ldp d8, d9, [x0, #104]\n  ldp d10, d11, [x0, #120]\n  ldp d12, d13, [x0, #136]\n  ldp d14, d15, [x0, #152]\n"
    "  mov x0, x1\n  cbnz x0, 1f\n  mov x0, #1\n1: br x30\n"
    ".globl _rt_on_stack\n.p2align 2\n_rt_on_stack:\n"
    "  stp x29, x30, [sp, #-16]!\n  mov x29, sp\n"
    "  mov x3, sp\n  mov sp, x2\n  sub sp, sp, #16\n  str x3, [sp]\n"
    "  mov x4, x0\n  mov x0, x1\n  blr x4\n"
    "  ldr x3, [sp]\n  mov sp, x3\n  ldp x29, x30, [sp], #16\n  ret\n");
void rt_on_stack(void (*fn)(void *), void *arg, void *top);
void gc_init(uint8_t *base, uint64_t size, uint8_t *stack_top);

static rt_jmp_t top;
static char errbuf[4096];
static int errline;
static uint8_t *stack_lo, *stack_hi;
static char **inputs; static int ninputs;
static char cwd[512] = ".";
static const char *prog_src, *prog_path;
static int lang, result;

void rt_write(const char *s, size_t n) { fwrite(s, 1, n, stdout); }
void rt_puts(const char *s) { fputs(s, stdout); }
void rt_printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void rt_check(void) { }
int rt_stopping(void) { return 0; }
uint64_t rt_ms(void) { struct timeval tv; gettimeofday(&tv, 0); return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000; }
void rt_sleep_ms(uint64_t ms) { struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000 }; nanosleep(&t, 0); }
int rt_stack_left(void) { uint8_t x; return (int)(&x - stack_lo); }
const char *rt_cwd(void) { return cwd; }
static uint64_t rng = 88172645463325252ull;
void rt_seed(uint64_t s) { rng = s ? s : 1; }
uint32_t rt_rand(void) { rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27; return (uint32_t)((rng * 0x2545F4914F6CDD1Dull) >> 32); }
int clock_known(void) { return 1; }
uint64_t clock_unix(void) { return (uint64_t)time(0); }
void clock_local(int *y, int *mo, int *d, int *h, int *mi, int *s, int *wd) {
    time_t t = time(0); struct tm *tm = localtime(&t);
    *y = tm->tm_year + 1900; *mo = tm->tm_mon + 1; *d = tm->tm_mday; *h = tm->tm_hour; *mi = tm->tm_min; *s = tm->tm_sec; *wd = tm->tm_wday;
}

int rt_readline(const char *prompt, char *buf, int cap) {
    if (prompt && *prompt) fputs(prompt, stdout);
    if (ninputs) { snprintf(buf, (size_t)cap, "%s", *inputs++); ninputs--; return (int)strlen(buf); }
    if (!fgets(buf, cap, stdin)) return -1;
    size_t l = strlen(buf); if (l && buf[l - 1] == '\n') buf[--l] = 0;
    return (int)l;
}

static void host_path(const char *p, char *out, size_t cap) {
    /* the program's own folder stands in for the card's */
    const char *base = strrchr(p, '/');
    snprintf(out, cap, "%s/%s", cwd, base ? base + 1 : p);
}

int rt_file_read(const char *path, char **data, uint32_t *len, const char **why) {
    char hp[1024]; host_path(path, hp, sizeof hp);
    FILE *f = fopen(hp, "rb");
    if (!f) { if (why) *why = "There's no file called that"; return -1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = rt_alloc_leaf((size_t)n + 1);
    fread(b, 1, (size_t)n, f); b[n] = 0; fclose(f);
    *data = b; if (len) *len = (uint32_t)n;
    return 0;
}
int rt_file_write(const char *path, const char *data, uint32_t len, int append, const char **why) {
    char hp[1024]; host_path(path, hp, sizeof hp);
    FILE *f = fopen(hp, append ? "ab" : "wb");
    if (!f) { if (why) *why = "can't write"; return -1; }
    fwrite(data, 1, len, f); fclose(f);
    return 0;
}

void rt_fail(int line, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(errbuf, sizeof errbuf, fmt, ap);
    va_end(ap);
    errline = line;
    rt_longjmp(&top, 1);
}

static void on_stack(void *arg) {
    (void)arg;
    if (rt_setjmp(&top) == 0) {
        result = lang == LANG_PY ? py_main(prog_src, prog_path) : cc_main(prog_src, prog_path, lang == LANG_CPP);
        if (result) fprintf(stdout, "[exit code %d]\n", result);
    } else {
        fflush(stdout);
        fprintf(stdout, "%s\n[error on line %d]\n", errbuf, errline);
        result = 1;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: pyhost prog.py|prog.c|prog.cpp [input lines...]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)n + 1); fread(src, 1, (size_t)n, f); src[n] = 0; fclose(f);
    const char *slash = strrchr(argv[1], '/');
    if (slash) snprintf(cwd, sizeof cwd, "%.*s", (int)(slash - argv[1]), argv[1]);
    inputs = argv + 2; ninputs = argc - 2;
    const char *dot = strrchr(argv[1], '.');
    lang = dot && !strcmp(dot, ".py") ? LANG_PY : dot && !strcmp(dot, ".c") ? LANG_C : LANG_CPP;
    size_t region = 512u << 20, stack = 8u << 20;
    uint8_t *mem = malloc(region);
    stack_lo = mem; stack_hi = mem + stack;
    gc_init(mem + stack, region - stack, stack_hi);
    prog_src = src;
    char vpath[600]; snprintf(vpath, sizeof vpath, "/Code/%s", slash ? slash + 1 : argv[1]);
    prog_path = vpath;
    setvbuf(stdout, 0, _IOFBF, 1 << 16);
    rt_on_stack(on_stack, 0, stack_hi);
    fflush(stdout);
    return result;
}

/* until the C interpreter is linked in */
__attribute__((weak)) int cc_main(const char *src, const char *path, int cpp) { (void)src; (void)path; (void)cpp; rt_fail(0, "no C here"); }
