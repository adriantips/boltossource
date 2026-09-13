/* Drive BoltAS, BoltAR and BoltLD on the host so something else can check
 * what they produce.
 *
 * An assembler can be checked against another assembler one instruction at a
 * time (test/host/asm.c does exactly that). An object format cannot: it is
 * only right if a reader that was not written alongside it agrees, and the
 * readers worth asking -- nm, readelf, ld.lld -- all live on the host. So this
 * exposes the three tools as subcommands and test/t_toolchain.py points real
 * binutils at the files that come out.
 *
 * The sources here are the ones that ship. Only the heap and the two BSD
 * string routines are substituted.
 *
 *   asmobj IN.asm OUT.o          assemble to a relocatable
 *   asmexe IN.asm OUT            assemble straight to a program
 *   ar     OUT.a IN.o...         build an archive
 *   ld     OUT IN.o|IN.a...      link
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#include "../../kernel/ar.c"
#include "../../kernel/link.c"

static unsigned char *slurp(const char *path, unsigned *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n + 1);
    if (n && fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read %s\n", path); exit(2); }
    b[n] = 0;
    fclose(f);
    *len = (unsigned)n;
    return b;
}

static void spit(const char *path, const void *d, unsigned n) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    if (n) fwrite(d, 1, n, f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: link MODE OUT [IN...]\n"); return 2; }
    const char *mode = argv[1];
    const char *out  = argv[2];

    if (!strcmp(mode, "asmobj") || !strcmp(mode, "asmexe")) {
        /* argv[2] is the source here and argv[3] the destination, which is the
         * opposite of the other two modes. Worth the inconsistency: `asmobj
         * in.asm out.o` reads the way `as` does. */
        if (argc != 4) { fprintf(stderr, "usage: link %s IN.asm OUT\n", mode); return 2; }
        unsigned slen = 0;
        char *src = (char *)slurp(argv[2], &slen);
        char err[ASM_MAX_ERR] = "";
        uint32_t len = 0;
        uint8_t *img = mode[3] == 'o' ? asm_assemble_obj(src, &len, err, sizeof err)
                                      : asm_assemble(src, &len, err, sizeof err);
        if (!img) { printf("ERR %s\n", err[0] ? err : "assembly failed"); return 1; }
        spit(argv[3], img, len);
        printf("OK %u\n", len);
        return 0;
    }

    if (!strcmp(mode, "ar")) {
        ar_input_t in[32];
        int n = 0;
        for (int i = 3; i < argc && n < 32; i++) {
            unsigned l = 0;
            in[n].name = argv[i];
            in[n].data = slurp(argv[i], &l);
            in[n].len  = l;
            n++;
        }
        char err[AR_MAX_ERR] = "";
        uint32_t len = 0;
        uint8_t *img = ar_create(in, n, &len, err, sizeof err);
        if (!img) { printf("ERR %s\n", err[0] ? err : "archive failed"); return 1; }
        spit(out, img, len);
        printf("OK %u\n", len);
        return 0;
    }

    if (!strcmp(mode, "ld")) {
        ld_input_t in[32];
        int n = 0;
        const char *entry = 0;
        for (int i = 3; i < argc && n < 32; i++) {
            if (!strcmp(argv[i], "-e") && i + 1 < argc) { entry = argv[++i]; continue; }
            unsigned l = 0;
            in[n].name = argv[i];
            in[n].data = slurp(argv[i], &l);
            in[n].len  = l;
            n++;
        }
        char err[LD_MAX_ERR] = "";
        uint32_t len = 0;
        ld_stats_t st;
        memset(&st, 0, sizeof st);
        uint8_t *img = ld_link(in, n, entry, &len, &st, err, sizeof err);
        if (!img) { printf("ERR %s\n", err[0] ? err : "link failed"); return 1; }
        spit(out, img, len);
        printf("OK %u objects=%u fromlib=%u relocs=%u text=%u data=%u entry=%llx\n",
               len, st.objects, st.from_archives, st.relocs, st.text, st.data,
               (unsigned long long)st.entry);
        return 0;
    }

    fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
}
