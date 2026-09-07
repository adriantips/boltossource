#include "ulibc.h"
#include "string.h"

/* ===========================================================================
 *  /bin/crashme  -  a ring-3 program that faults on purpose.
 *
 *  Exists so per-process fault isolation can be demonstrated and regression
 *  tested: run it, watch the kernel reap it and report the fault, and confirm
 *  the machine (and the desktop, and everything else running) carries straight
 *  on. Drive it from the shell with `crash ring3 [null|div|ud|stack]`.
 *
 *  Each mode raises a different CPU exception, because they take different
 *  paths through the fault handler:
 *    null   #PF  (14) -- a bad data pointer, the ordinary case
 *    div    #DE  (0)  -- a fault with no error code
 *    ud     #UD  (6)  -- a bad instruction, i.e. a corrupt code path
 *    stack  #PF  (14) -- unbounded recursion off the end of the user stack
 * ===========================================================================*/

static int recurse(int depth) {
    volatile char pad[4096];              /* burn a page per frame */
    pad[0] = (char)depth;
    return recurse(depth + 1) + pad[0];
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "null";

    printf("crashme: pid=%d about to fault (%s)\n", getpid(), mode);

    if (!strcmp(mode, "div")) {
        /* Written in assembly on purpose. `1 / z` with a volatile zero is
         * undefined behaviour, and at -O2 clang is entitled to fold it to a
         * constant rather than emit a div -- which it does, so the C version
         * silently never faulted. This raises a genuine #DE. */
        __asm__ volatile("xorl %%edx, %%edx\n\t"
                         "movl $1, %%eax\n\t"
                         "xorl %%ecx, %%ecx\n\t"
                         "divl %%ecx"
                         ::: "eax", "ecx", "edx", "cc");
    } else if (!strcmp(mode, "ud")) {
        __asm__ volatile("ud2");
    } else if (!strcmp(mode, "stack")) {
        printf("crashme: recursing off the end of the stack\n");
        return recurse(0);
    } else {
        volatile int *p = (volatile int *)0;
        *p = 0x1234;                      /* write to the null page */
    }

    printf("crashme: still alive -- the fault did not happen\n");
    return 1;
}
