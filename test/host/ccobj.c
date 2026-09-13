/* BoltCC's front end and its native backend, on the host, printing assembly.
 *
 * This is deliberately only half of the chain: BoltCC and BoltAS cannot live in
 * one translation unit -- both have a static `emit`, among other things -- so
 * this prints what the compiler produced and test/host/link.c assembles it.
 * Two programs, piped together, which is how a compiler driver works anyway.
 *
 *   ccobj module < FILE.c     one translation unit of a larger program
 *   ccobj program < FILE.c    a whole program, the way `cc -N` does it
 *
 * Prints the assembly on success, or `ERR` and whatever BoltCC said.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

size_t strlcpy(char *d, const char *s, size_t cap);
size_t strlcat(char *d, const char *s, size_t cap);

size_t strlcpy(char *d, const char *s, size_t cap) {
    size_t n = strlen(s);
    if (cap) {
        size_t c = n < cap - 1 ? n : cap - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return n;
}
size_t strlcat(char *d, const char *s, size_t cap) {
    size_t dl = strnlen(d, cap);
    if (dl == cap) return cap + strlen(s);
    return dl + strlcpy(d + dl, s, cap - dl);
}

void *kmalloc(unsigned long long n) { return malloc(n ? (size_t)n : 1); }
void  kfree(void *p) { free(p); }

/* BoltCC's diagnostics go to stderr here so stdout stays pure assembly and can
 * be redirected straight into the assembler. */
void kprintf(const char *fmt, ...);
void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
void kputc(char c);
void kputc(char c) { fputc(c, stderr); }

#include "../../kernel/ccnative.c"
#include "../../kernel/boltcc.c"

int main(int argc, char **argv) {
    int module = argc > 1 && !strcmp(argv[1], "module");

    char *src = malloc(1 << 20);
    size_t n = fread(src, 1, (1 << 20) - 1, stdin);
    src[n] = 0;

    char *asm_text = 0;
    int rc = module ? boltcc_module(BCC_C, src, &asm_text)
                    : boltcc_native(BCC_C, src, &asm_text);
    if (rc != 0 || !asm_text) {
        printf("ERR compile failed\n");
        return 1;
    }
    fputs(asm_text, stdout);
    return 0;
}
