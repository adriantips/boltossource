/* ===========================================================================
 *  BoltOS Mini  -  mini/webutil.c
 *
 *  Text, URLs and JSON for the browser and the player (see web.h).
 * ===========================================================================*/
#include <stdint.h>
#include "web.h"
#include "kheap.h"
#include "string.h"
#include "stdio.h"

/* ---- text ----------------------------------------------------------------------- */
static const uint16_t cp1252_hi[32] = {          /* 0x80..0x9F                       */
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

int uni_to_cp1252(uint32_t cp) {
    if (cp == '\t' || cp == '\n' || cp == '\r') return (int)cp;
    if (cp < 0x20) return 0;
    if (cp < 0x7F) return (int)cp;
    if (cp >= 0xA0 && cp <= 0xFF) return (int)cp;
    for (int i = 0; i < 32; i++) if (cp1252_hi[i] == cp) return 0x80 + i;
    switch (cp) {
    case 0x2010: case 0x2011: case 0x2012: case 0x2212: return '-';
    case 0x2015: return 0x97;
    case 0x2032: return '\'';
    case 0x2033: return '"';
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: return ' ';
    case 0x2192: return 0x81;                       /* the font's borrowed arrows */
    case 0x2190: return 0x8D;
    case 0x2191: return 0x8F;
    case 0x2193: return 0x90;
    case 0x2713: case 0x2714: return 0x9D;
    case 0x25B6: case 0x25BA: return '>';
    case 0x00B7: case 0x2027: case 0x30FB: return 0xB7;
    }
    /* Joiners, variation selectors, emoji and pictographs: nothing to draw. */
    if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x200E || cp == 0x200F || cp == 0xFEFF) return 0;
    if ((cp >= 0xFE00 && cp <= 0xFE0F) || cp >= 0x1F000 || (cp >= 0x2600 && cp <= 0x27BF) ||
        (cp >= 0x2B00 && cp <= 0x2BFF) || (cp >= 0xE0000)) return 0;
    if (cp >= 0x0300 && cp <= 0x036F) return 0;     /* combining marks           */
    return -1;
}

int utf8_to_cp1252(const char *in, int len, char *out, int cap) {
    int o = 0;
    const uint8_t *p = (const uint8_t *)in, *e = p + len;
    while (p < e && o < cap - 1) {
        uint32_t cp; int n;
        uint8_t b = *p;
        if (b < 0x80) { cp = b; n = 1; }
        else if ((b & 0xE0) == 0xC0 && p + 1 < e && (p[1] & 0xC0) == 0x80) { cp = (uint32_t)(b & 0x1F) << 6 | (p[1] & 0x3F); n = 2; }
        else if ((b & 0xF0) == 0xE0 && p + 2 < e && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = (uint32_t)(b & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F); n = 3;
        } else if ((b & 0xF8) == 0xF0 && p + 3 < e && (p[1] & 0xC0) == 0x80) {
            cp = (uint32_t)(b & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 | (uint32_t)(p[2] & 0x3F) << 6 | (p[3] & 0x3F); n = 4;
        } else { cp = b; n = 1; }                   /* not UTF-8: take it as cp1252 */
        p += n;
        if (n == 1 && b >= 0x80) { out[o++] = (char)b; continue; }
        int c = uni_to_cp1252(cp);
        if (c > 0) out[o++] = (char)c;
        else if (c < 0) out[o++] = '?';
    }
    out[o] = 0;
    return o;
}

static const struct { const char *name; uint16_t cp; } ents[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "hellip", 0x2026 }, { "mdash", 0x2014 },
    { "ndash", 0x2013 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D },
    { "laquo", 0xAB }, { "raquo", 0xBB }, { "middot", 0xB7 }, { "bull", 0x2022 }, { "deg", 0xB0 },
    { "euro", 0x20AC }, { "pound", 0xA3 }, { "yen", 0xA5 }, { "cent", 0xA2 }, { "sect", 0xA7 },
    { "times", 0xD7 }, { "divide", 0xF7 }, { "plusmn", 0xB1 }, { "frac12", 0xBD }, { "eacute", 0xE9 },
    { "egrave", 0xE8 }, { "aacute", 0xE1 }, { "agrave", 0xE0 }, { "ouml", 0xF6 }, { "uuml", 0xFC },
    { "auml", 0xE4 }, { "szlig", 0xDF }, { "ntilde", 0xF1 }, { "ccedil", 0xE7 }, { "iacute", 0xED },
    { "oacute", 0xF3 }, { "uacute", 0xFA }, { "rarr", 0x2192 }, { "larr", 0x2190 }, { "thinsp", 0x2009 },
    { "ensp", 0x2002 }, { "emsp", 0x2003 }, { "zwj", 0x200D }, { "zwnj", 0x200C }, { "shy", 0 },
};

void entities_decode(char *s) {
    char *w = s;
    for (char *r = s; *r; ) {
        if (*r != '&') { *w++ = *r++; continue; }
        char *semi = r + 1;
        while (*semi && *semi != ';' && semi - r < 12) semi++;
        if (*semi != ';') { *w++ = *r++; continue; }
        uint32_t cp = 0xFFFFFFFFu;
        if (r[1] == '#') {
            cp = 0;
            if (r[2] == 'x' || r[2] == 'X') {
                for (char *q = r + 3; q < semi; q++) {
                    int h = (*q >= '0' && *q <= '9') ? *q - '0' : (*q >= 'a' && *q <= 'f') ? *q - 'a' + 10 :
                            (*q >= 'A' && *q <= 'F') ? *q - 'A' + 10 : -1;
                    if (h < 0) { cp = 0xFFFFFFFFu; break; }
                    cp = cp * 16 + (uint32_t)h;
                }
            } else for (char *q = r + 2; q < semi; q++) {
                if (*q < '0' || *q > '9') { cp = 0xFFFFFFFFu; break; }
                cp = cp * 10 + (uint32_t)(*q - '0');
            }
        } else {
            int n = (int)(semi - r - 1);
            for (unsigned i = 0; i < sizeof ents / sizeof ents[0]; i++)
                if ((int)strlen(ents[i].name) == n && !strncmp(ents[i].name, r + 1, (size_t)n)) { cp = ents[i].cp; break; }
        }
        if (cp == 0xFFFFFFFFu) { *w++ = *r++; continue; }
        int c = cp ? uni_to_cp1252(cp) : 0;
        if (c > 0) *w++ = (char)c;
        else if (c < 0) *w++ = '?';
        r = semi + 1;
    }
    *w = 0;
}

/* ---- URLs --------------------------------------------------------------------------- */
static int is_scheme(const char *u) { return !strncmp(u, "http://", 7) || !strncmp(u, "https://", 8); }

void url_host(const char *url, char *out, int cap) {
    const char *s = strstr(url, "://");
    s = s ? s + 3 : url;
    int i = 0;
    while (*s && *s != '/' && *s != '?' && *s != '#' && i < cap - 1) out[i++] = *s++;
    out[i] = 0;
}

void url_resolve(const char *base, const char *href, char *out, int cap) {
    while (*href == ' ' || *href == '\n' || *href == '\t') href++;
    if (is_scheme(href)) { snprintf(out, (size_t)cap, "%s", href); return; }
    const char *sch = !strncmp(base, "https://", 8) ? "https:" : "http:";
    if (href[0] == '/' && href[1] == '/') { snprintf(out, (size_t)cap, "%s%s", sch, href); return; }
    char host[200];
    url_host(base, host, sizeof host);
    if (href[0] == '/') { snprintf(out, (size_t)cap, "%s//%s%s", sch, host, href); return; }
    /* relative: the base's directory (or the base itself for ?query and #frag) */
    const char *path = strstr(base, "://");
    path = path ? strchr(path + 3, '/') : 0;
    char dir[512] = "/";
    if (path) {
        int n = 0;
        while (path[n] && path[n] != '?' && path[n] != '#' && n < (int)sizeof dir - 1) n++;
        if (href[0] == '?' || href[0] == '#') { memcpy(dir, path, (size_t)n); dir[n] = 0; }
        else {
            int last = 0;
            for (int i = 0; i < n; i++) if (path[i] == '/') last = i;
            memcpy(dir, path, (size_t)last + 1); dir[last + 1] = 0;
        }
    }
    if (href[0] == '#') { snprintf(out, (size_t)cap, "%s//%s%s", sch, host, dir); return; }
    char joined[1024];
    snprintf(joined, sizeof joined, "%s%s", dir, href);
    /* fold "./" and "../" */
    char *segs[64]; int ns = 0;
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", joined);
    char *q = strchr(tmp, '?');
    char query[512] = "";
    if (q) { snprintf(query, sizeof query, "%s", q); *q = 0; }
    int trailing = tmp[0] && tmp[strlen(tmp) - 1] == '/';
    for (char *t = tmp; *t; ) {
        while (*t == '/') t++;
        if (!*t) break;
        char *e = t; while (*e && *e != '/') e++;
        char c = *e; *e = 0;
        if (!strcmp(t, "..")) { if (ns) ns--; }
        else if (strcmp(t, ".") && ns < 64) segs[ns++] = t;
        if (!c) break;
        t = e + 1;
    }
    int o = snprintf(out, (size_t)cap, "%s//%s", sch, host);
    for (int i = 0; i < ns && o < cap; i++) o += snprintf(out + o, (size_t)(cap - o), "/%s", segs[i]);
    if ((trailing || !ns) && o < cap) o += snprintf(out + o, (size_t)(cap - o), "/");
    if (o < cap) snprintf(out + o, (size_t)(cap - o), "%s", query);
}

void url_encode(const char *in, char *out, int cap) {
    static const char hx[] = "0123456789ABCDEF";
    int o = 0;
    for (const uint8_t *p = (const uint8_t *)in; *p && o < cap - 4; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') out[o++] = (char)*p;
        else if (*p == ' ') out[o++] = '+';
        else if (*p >= 0x80) {                       /* cp1252 back to UTF-8 */
            uint32_t cp = *p >= 0xA0 ? *p : cp1252_hi[*p - 0x80];
            uint8_t u[3]; int n;
            if (cp < 0x800) { u[0] = (uint8_t)(0xC0 | cp >> 6); u[1] = (uint8_t)(0x80 | (cp & 63)); n = 2; }
            else { u[0] = (uint8_t)(0xE0 | cp >> 12); u[1] = (uint8_t)(0x80 | ((cp >> 6) & 63)); u[2] = (uint8_t)(0x80 | (cp & 63)); n = 3; }
            for (int i = 0; i < n && o < cap - 4; i++) { out[o++] = '%'; out[o++] = hx[u[i] >> 4]; out[o++] = hx[u[i] & 15]; }
        } else { out[o++] = '%'; out[o++] = hx[*p >> 4]; out[o++] = hx[*p & 15]; }
    }
    out[o] = 0;
}

static int hexv(char c) {
    return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
}

void url_decode(const char *in, int len, char *out, int cap) {
    int o = 0;
    for (int i = 0; i < len && in[i] && o < cap - 1; i++) {
        if (in[i] == '%' && i + 2 < len && hexv(in[i + 1]) >= 0 && hexv(in[i + 2]) >= 0) {
            out[o++] = (char)(hexv(in[i + 1]) * 16 + hexv(in[i + 2])); i += 2;
        } else if (in[i] == '+') out[o++] = ' ';
        else out[o++] = in[i];
    }
    out[o] = 0;
}

int url_param(const char *url, const char *name, char *out, int cap) {
    const char *q = strchr(url, '?');
    if (!q) return 0;
    int nl = (int)strlen(name);
    for (const char *p = q + 1; *p; ) {
        if (!strncmp(p, name, (size_t)nl) && p[nl] == '=') {
            const char *v = p + nl + 1, *e = v;
            while (*e && *e != '&' && *e != '#') e++;
            url_decode(v, (int)(e - v), out, cap);
            return 1;
        }
        while (*p && *p != '&') p++;
        if (*p) p++;
    }
    return 0;
}

/* ---- JSON ------------------------------------------------------------------------------ */
struct jarena { struct jarena *next; uint32_t used, cap; uint8_t mem[]; };

static void *ja_alloc(jarena **a, uint32_t n) {
    n = (n + 7) & ~7u;
    if (!*a || (*a)->used + n > (*a)->cap) {
        uint32_t cap = n > (1u << 20) ? n : (1u << 20);
        jarena *b = kmalloc(sizeof(jarena) + cap);
        if (!b) return 0;
        b->next = *a; b->used = 0; b->cap = cap;
        *a = b;
    }
    void *p = (*a)->mem + (*a)->used;
    (*a)->used += n;
    return p;
}

void json_free(jarena *a) {
    while (a) { jarena *n = a->next; kfree(a); a = n; }
}

typedef struct { const char *p, *e; jarena **a; int depth; } jp_t;

static void jws(jp_t *j) { while (j->p < j->e && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) j->p++; }

static void put_utf8(char *o, int *k, uint32_t cp) {
    if (cp < 0x80) o[(*k)++] = (char)cp;
    else if (cp < 0x800) { o[(*k)++] = (char)(0xC0 | cp >> 6); o[(*k)++] = (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o[(*k)++] = (char)(0xE0 | cp >> 12); o[(*k)++] = (char)(0x80 | ((cp >> 6) & 63)); o[(*k)++] = (char)(0x80 | (cp & 63)); }
    else { o[(*k)++] = (char)(0xF0 | cp >> 18); o[(*k)++] = (char)(0x80 | ((cp >> 12) & 63));
           o[(*k)++] = (char)(0x80 | ((cp >> 6) & 63)); o[(*k)++] = (char)(0x80 | (cp & 63)); }
}

static const char *jstr(jp_t *j) {                  /* at the opening quote */
    const char *s = ++j->p, *q = s;
    int esc = 0;
    while (q < j->e && *q != '"') { if (*q == '\\') { esc = 1; q++; } q++; }
    if (q >= j->e) return 0;
    int n = (int)(q - s);
    char *o = ja_alloc(j->a, (uint32_t)n + 1);
    if (!o) return 0;
    if (!esc) { memcpy(o, s, (size_t)n); o[n] = 0; j->p = q + 1; return o; }
    int k = 0;
    for (const char *r = s; r < q; r++) {
        if (*r != '\\') { o[k++] = *r; continue; }
        r++;
        switch (*r) {
        case 'n': o[k++] = '\n'; break;
        case 't': o[k++] = '\t'; break;
        case 'r': break;
        case 'b': case 'f': break;
        case 'u': {
            uint32_t cp = 0;
            for (int i = 1; i <= 4 && r + i < q; i++) cp = cp * 16 + (uint32_t)(hexv(r[i]) < 0 ? 0 : hexv(r[i]));
            r += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && r + 6 < q && r[1] == '\\' && r[2] == 'u') {
                uint32_t lo = 0;
                for (int i = 3; i <= 6; i++) lo = lo * 16 + (uint32_t)(hexv(r[i]) < 0 ? 0 : hexv(r[i]));
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); r += 6; }
            }
            if (k + 4 <= n) put_utf8(o, &k, cp);
            break;
        }
        default: o[k++] = *r; break;
        }
    }
    o[k] = 0;
    j->p = q + 1;
    return o;
}

static jv *jval(jp_t *j) {
    jws(j);
    if (j->p >= j->e || ++j->depth > 200) return 0;
    jv *v = ja_alloc(j->a, sizeof(jv));
    if (!v) return 0;
    memset(v, 0, sizeof *v);
    char c = *j->p;
    if (c == '{' || c == '[') {
        int obj = c == '{';
        v->t = obj ? J_OBJ : J_ARR;
        j->p++;
        jv **tail = &v->kid;
        for (;;) {
            jws(j);
            if (j->p < j->e && *j->p == (obj ? '}' : ']')) { j->p++; break; }
            const char *key = 0;
            if (obj) {
                if (j->p >= j->e || *j->p != '"' || !(key = jstr(j))) return 0;
                jws(j);
                if (j->p >= j->e || *j->p != ':') return 0;
                j->p++;
            }
            jv *k = jval(j);
            if (!k) return 0;
            k->key = key;
            *tail = k; tail = &k->next;
            v->n++;
            jws(j);
            if (j->p < j->e && *j->p == ',') { j->p++; continue; }
            if (j->p < j->e && *j->p == (obj ? '}' : ']')) { j->p++; break; }
            return 0;
        }
    } else if (c == '"') {
        v->t = J_STR;
        if (!(v->s = jstr(j))) return 0;
    } else if (c == 't' && j->e - j->p >= 4 && !strncmp(j->p, "true", 4)) { v->t = J_TRUE; j->p += 4; }
    else if (c == 'f' && j->e - j->p >= 5 && !strncmp(j->p, "false", 5)) { v->t = J_FALSE; j->p += 5; }
    else if (c == 'n' && j->e - j->p >= 4 && !strncmp(j->p, "null", 4)) { v->t = J_NULL; j->p += 4; }
    else if (c == '-' || (c >= '0' && c <= '9')) {
        const char *s = j->p;
        while (j->p < j->e && (*j->p == '-' || *j->p == '+' || *j->p == '.' || *j->p == 'e' || *j->p == 'E' ||
                               (*j->p >= '0' && *j->p <= '9'))) j->p++;
        int n = (int)(j->p - s);
        char *o = ja_alloc(j->a, (uint32_t)n + 1);
        if (!o) return 0;
        memcpy(o, s, (size_t)n); o[n] = 0;
        v->t = J_NUM; v->s = o;
    } else return 0;
    j->depth--;
    return v;
}

jv *json_parse(const char *s, int len, jarena **arena) {
    jp_t j = { s, s + len, arena, 0 };
    *arena = 0;
    jv *v = jval(&j);
    if (!v) { json_free(*arena); *arena = 0; }
    return v;
}

jv *json_get(const jv *o, const char *key) {
    if (!o || o->t != J_OBJ) return 0;
    for (jv *k = o->kid; k; k = k->next) if (k->key && !strcmp(k->key, key)) return k;
    return 0;
}

jv *json_at(const jv *a, int i) {
    if (!a || (a->t != J_ARR && a->t != J_OBJ)) return 0;
    jv *k = a->kid;
    while (k && i-- > 0) k = k->next;
    return k;
}

jv *json_path(const jv *v, const char *path) {
    char part[64];
    while (v && *path) {
        int n = 0;
        while (*path && *path != '.' && n < 63) part[n++] = *path++;
        part[n] = 0;
        if (*path == '.') path++;
        if (v->t == J_ARR && part[0] >= '0' && part[0] <= '9') {
            int i = 0;
            for (char *d = part; *d >= '0' && *d <= '9'; d++) i = i * 10 + (*d - '0');
            v = json_at(v, i);
        }
        else v = json_get(v, part);
    }
    return (jv *)v;
}

const char *json_s(const jv *v) { return v && v->t == J_STR ? v->s : ""; }

long long json_i(const jv *v) {
    if (!v) return 0;
    const char *s = v->s;
    if (!s) return v->t == J_TRUE;
    long long r = 0; int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') r = r * 10 + (*s++ - '0');
    return neg ? -r : r;
}

static int each_rec(const jv *v, const char *key, int (*fn)(const jv *, void *), void *ud, int depth) {
    if (!v || depth > 200) return 0;
    for (jv *k = v->kid; k; k = k->next) {
        if (v->t == J_OBJ && k->key && !strcmp(k->key, key) && fn(k, ud)) return 1;
        if ((k->t == J_OBJ || k->t == J_ARR) && each_rec(k, key, fn, ud, depth + 1)) return 1;
    }
    return 0;
}

void json_each(const jv *root, const char *key, int (*fn)(const jv *, void *), void *ud) {
    each_rec(root, key, fn, ud, 0);
}

int json_text(const jv *v, char *out, int cap) {
    out[0] = 0;
    if (!v) return 0;
    jv *st = json_get(v, "simpleText");
    if (st) return utf8_to_cp1252(json_s(st), (int)strlen(json_s(st)), out, cap);
    jv *c = json_get(v, "content");
    if (c && c->t == J_STR) return utf8_to_cp1252(c->s, (int)strlen(c->s), out, cap);
    jv *runs = json_get(v, "runs");
    int o = 0;
    for (jv *r = runs ? runs->kid : 0; r && o < cap - 1; r = r->next) {
        const char *t = json_s(json_get(r, "text"));
        o += utf8_to_cp1252(t, (int)strlen(t), out + o, cap - o);
    }
    if (v->t == J_STR) o = utf8_to_cp1252(v->s, (int)strlen(v->s), out, cap);
    return o;
}
