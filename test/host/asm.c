/* Encode instructions with the real assembler so something else can check the
 * bytes. kernel/asm.c is the one that ships; only the heap under it and the
 * three string routines it calls are substituted.
 *
 * An assembler is not a component you can test by asking whether it succeeded.
 * It either emits the bytes the processor decodes as the instruction you wrote
 * or it emits different ones, and both of those look identical from the
 * outside until something jumps into the wrong place. The only cheap oracle for
 * that is another assembler, so this prints hex and test/t_asm.py holds the
 * answer sheet -- NASM itself -- up against it.
 *
 * Reads one instruction per line on stdin. Prints, per line, either the hex of
 * the .text that instruction assembled to or `ERR <message>`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* asm.c's three string calls. memcpy/memset/strcmp come from the host libc and
 * match the kernel's prototypes, so only the BSD pair and the allocator need
 * standing in for. */
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

#include "../../kernel/asm.c"

/* The first PT_LOAD covers the ELF header and .text together, so .text is
 * everything past the 0x1000 the header segment pads out to. */
static void dump_text(const unsigned char *img, unsigned len) {
    if (len < 0x1000 + 64) { printf("ERR image too short\n"); return; }
    unsigned long long filesz = 0;
    for (int i = 7; i >= 0; i--) filesz = (filesz << 8) | img[64 + 32 + i];
    if (filesz < TEXT_OFF) { printf("ERR no text\n"); return; }
    unsigned n = (unsigned)(filesz - TEXT_OFF);
    for (unsigned i = 0; i < n; i++) printf("%02x", img[TEXT_OFF + i]);
    printf("\n");
}

int main(void) {
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;

        char src[1024];
        snprintf(src, sizeof src, "section .text\nglobal _start\n_start:\n%s\n", line);

        char err[ASM_MAX_ERR] = "";
        uint32_t len = 0;
        uint8_t *img = asm_assemble(src, &len, err, sizeof err);
        if (!img) printf("ERR %s\n", err[0] ? err : "assembly failed");
        else { dump_text(img, len); free(img); }
        fflush(stdout);
    }
    return 0;
}
