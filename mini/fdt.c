/* ===========================================================================
 *  BoltOS Mini  -  mini/fdt.c
 *
 *  Just enough of a flattened-device-tree reader to tell the machines apart
 *  and size RAM: walk the structure block, find a node by path (a component
 *  matches "name" or "name@unit"), return a property. Everything Mini drives
 *  sits at an address it already knows; the tree is consulted, not obeyed.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "string.h"

#define FDT_MAGIC      0xD00DFEEDu
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9

static uint32_t be32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

int fdt_valid(const void *fdt) {
    return fdt && ((uintptr_t)fdt & 3) == 0 && be32(fdt) == FDT_MAGIC;
}

/* Does path component `want` (no slashes) name node `name`? */
static int comp_match(const char *name, const char *want, int wlen) {
    if (strncmp(name, want, (size_t)wlen) != 0) return 0;
    return name[wlen] == 0 || (name[wlen] == '@' && !memchr(want, '@', (size_t)wlen));
}

static const void *find_prop(const void *fdt, const char *path, const char *prop, int *len_out) {
    if (!fdt_valid(fdt)) return 0;
    const uint8_t *base = (const uint8_t *)fdt;
    const uint8_t *p = base + be32(base + 8);
    const char *strs = (const char *)base + be32(base + 12);

    /* Split the path into components once. */
    const char *comps[8]; int clen[8]; int ncomp = 0;
    for (const char *s = path; *s && ncomp < 8; ) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *e = s; while (*e && *e != '/') e++;
        comps[ncomp] = s; clen[ncomp] = (int)(e - s); ncomp++;
        s = e;
    }

    int depth = -1;          /* depth of the current node; root is 0       */
    int matched = 0;         /* how many leading components match the path */
    for (int guard = 0; guard < 200000; guard++) {
        uint32_t tok = be32(p); p += 4;
        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            size_t n = strlen(name);
            p += (n + 4) & ~3u;
            depth++;
            if (depth == 0) matched = 0;
            else if (matched == depth - 1 && depth <= ncomp &&
                     comp_match(name, comps[depth - 1], clen[depth - 1]))
                matched = depth;
        } else if (tok == FDT_END_NODE) {
            if (matched == depth) matched = depth - 1;
            depth--;
        } else if (tok == FDT_PROP) {
            uint32_t len = be32(p), nameoff = be32(p + 4);
            const uint8_t *val = p + 8;
            p += 8 + ((len + 3) & ~3u);
            if (depth == ncomp && matched == ncomp && strcmp(strs + nameoff, prop) == 0) {
                if (len_out) *len_out = (int)len;
                return val;
            }
        } else if (tok == FDT_NOP) {
        } else {
            break;
        }
    }
    return 0;
}

const char *fdt_prop_str(const void *fdt, const char *path, const char *prop) {
    int len = 0;
    const char *v = (const char *)find_prop(fdt, path, prop, &len);
    return (v && len > 0 && v[len - 1] == 0) ? v : 0;
}

const char *fdt_root_compat(const void *fdt) { return fdt_prop_str(fdt, "/", "compatible"); }
const char *fdt_model(const void *fdt)       { return fdt_prop_str(fdt, "/", "model"); }

int fdt_compat_has(const void *fdt, const char *s) {
    int len = 0;
    const char *v = (const char *)find_prop(fdt, "/", "compatible", &len);
    for (int i = 0; v && i < len; ) {
        if (strcmp(v + i, s) == 0) return 1;
        i += (int)strlen(v + i) + 1;
    }
    return 0;
}

int fdt_memory(const void *fdt, uint64_t *base, uint64_t *size) {
    int len = 0;
    const uint8_t *ac = find_prop(fdt, "/", "#address-cells", 0);
    const uint8_t *sc = find_prop(fdt, "/", "#size-cells", 0);
    uint32_t na = ac ? be32(ac) : 2, ns = sc ? be32(sc) : 1;
    const uint8_t *reg = find_prop(fdt, "/memory", "reg", &len);
    if (!reg || len < (int)(4 * (na + ns))) return 0;
    uint64_t b = 0, s = 0;
    for (uint32_t i = 0; i < na; i++) b = (b << 32) | be32(reg + 4 * i);
    for (uint32_t i = 0; i < ns; i++) s = (s << 32) | be32(reg + 4 * (na + i));
    *base = b; *size = s;
    return 1;
}
