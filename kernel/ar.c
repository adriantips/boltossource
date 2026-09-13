#include <stdint.h>
#include <stddef.h>
#include "ar.h"
#include "kheap.h"
#include "string.h"

/* BoltAR. The format is described in include/ar.h; this is the reader and the
 * writer, together, because they are the only two things that have to agree
 * about it and keeping them apart is how a format drifts. */

int ar_is_archive(const uint8_t *d, uint32_t len) {
    return d && len >= 8 && memcmp(d, "!<arch>\n", 8) == 0;
}

static uint32_t ar_dec(const char *p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') break;
        v = v * 10 + (uint32_t)(p[i] - '0');
    }
    return v;
}

int ar_next(const uint8_t *ar, uint32_t arlen, uint32_t *pos,
            const char **longnames, uint32_t *longlen, ar_member_t *m) {
    if (*pos >= arlen) return *pos == arlen ? 0 : -1;
    if (arlen - *pos < 60) return -1;

    const char *h = (const char *)(ar + *pos);
    if (h[58] != '`' || h[59] != '\n') return -1;

    uint32_t size = ar_dec(h + 48, 10);
    uint32_t body = *pos + 60;
    if (size > arlen - body) return -1;

    memset(m, 0, sizeof *m);
    m->data = ar + body;
    m->len  = size;
    m->kind = AR_FILE;

    if (h[0] == '/' && h[1] == '/') {
        m->kind = AR_LONGNAMES;
        if (longnames) { *longnames = (const char *)m->data; *longlen = size; }
    } else if (h[0] == '/' && (h[1] == ' ' || h[1] == '/')) {
        m->kind = AR_INDEX;
    } else if (h[0] == '/' && h[1] >= '0' && h[1] <= '9') {
        uint32_t at = ar_dec(h + 1, 15);
        const char *tab = longnames ? *longnames : 0;
        uint32_t tl = longlen ? *longlen : 0;
        uint32_t n = 0;
        while (tab && at + n < tl && tab[at + n] != '/' && tab[at + n] != '\n' &&
               n + 1 < sizeof m->name) {
            m->name[n] = tab[at + n];
            n++;
        }
        m->name[n] = 0;
    } else {
        uint32_t n = 0;
        while (n < 16 && h[n] != '/' && h[n] != ' ' && n + 1 < sizeof m->name) {
            m->name[n] = h[n];
            n++;
        }
        m->name[n] = 0;
    }

    /* A member with an odd length is followed by one padding byte, which is
     * not part of it and is not counted in the size. Forgetting that shifts
     * every later header by one and the archive reads as garbage from there
     * down -- a fun half hour. */
    *pos = body + size + (size & 1);
    return 1;
}

/* ---- writing ------------------------------------------------------------- */

/* The name as it goes in the header. Anything over 15 characters, or with a
 * slash in it, has to live in the "//" table instead. */
static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void field(char *dst, int width, const char *s) {
    int i = 0;
    for (; s[i] && i < width; i++) dst[i] = s[i];
    for (; i < width; i++) dst[i] = ' ';
}

static void field_u(char *dst, int width, uint32_t v) {
    char tmp[16];
    int n = 0;
    if (!v) tmp[n++] = '0';
    while (v && n < (int)sizeof tmp) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    char s[17];
    int o = 0;
    while (n) s[o++] = tmp[--n];
    s[o] = 0;
    field(dst, width, s);
}

uint8_t *ar_create(const ar_input_t *in, int n, uint32_t *len,
                   char *err, uint32_t errcap) {
    if (n <= 0) { strlcpy(err, "nothing to archive", errcap); return 0; }

    uint32_t longsz = 0;
    for (int i = 0; i < n; i++) {
        const char *b = base_name(in[i].name);
        uint32_t bl = (uint32_t)strlen(b);
        if (bl >= AR_NAME_MAX) { strlcpy(err, "a member name is too long", errcap); return 0; }
        if (bl > 15) longsz += bl + 2;              /* name, '/', '\n' */
    }
    uint32_t longpad = longsz & 1;

    uint32_t total = 8;
    if (longsz) total += 60 + longsz + longpad;
    for (int i = 0; i < n; i++) total += 60 + in[i].len + (in[i].len & 1);

    uint8_t *out = (uint8_t *)kmalloc(total);
    if (!out) { strlcpy(err, "out of memory building the archive", errcap); return 0; }
    memset(out, 0, total);
    memcpy(out, "!<arch>\n", 8);

    uint32_t at = 8, longat = 0;
    char *longtab = 0;

    if (longsz) {
        char *h = (char *)(out + at);
        field(h + 0, 16, "//");
        field(h + 16, 12, "0");
        field(h + 28, 6, "0");
        field(h + 34, 6, "0");
        field(h + 40, 8, "0");
        field_u(h + 48, 10, longsz);
        h[58] = '`'; h[59] = '\n';
        longtab = (char *)(out + at + 60);
        at += 60 + longsz + longpad;
        if (longpad) longtab[longsz] = '\n';
    }

    for (int i = 0; i < n; i++) {
        const char *b = base_name(in[i].name);
        uint32_t bl = (uint32_t)strlen(b);
        char *h = (char *)(out + at);

        if (bl > 15) {
            char ref[18];
            ref[0] = '/';
            uint32_t v = longat, o = 1;
            char tmp[12];
            int t = 0;
            if (!v) tmp[t++] = '0';
            while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
            while (t) ref[o++] = tmp[--t];
            ref[o] = 0;
            field(h + 0, 16, ref);
            memcpy(longtab + longat, b, bl);
            longtab[longat + bl] = '/';
            longtab[longat + bl + 1] = '\n';
            longat += bl + 2;
        } else {
            char nm[18];
            strlcpy(nm, b, sizeof nm);
            strlcat(nm, "/", sizeof nm);
            field(h + 0, 16, nm);
        }
        field(h + 16, 12, "0");               /* mtime: nothing reads it here */
        field(h + 28, 6, "0");
        field(h + 34, 6, "0");
        field(h + 40, 8, "100644");
        field_u(h + 48, 10, in[i].len);
        h[58] = '`'; h[59] = '\n';

        if (in[i].len) memcpy(out + at + 60, in[i].data, in[i].len);
        if (in[i].len & 1) out[at + 60 + in[i].len] = '\n';
        at += 60 + in[i].len + (in[i].len & 1);
    }

    *len = total;
    return out;
}
