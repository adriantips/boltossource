#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS Mini  -  mini/web.h
 *
 *  What the browser (app_browser.c) and the video player (youtube.c, h264.c)
 *  share: text from the web made drawable, URLs, a JSON reader, and fetching.
 *
 *  Mini's fonts are Windows-1252, the kernel's own encoding, so text off the
 *  web is UTF-8 going in and cp1252 coming out: curly quotes, dashes, accents
 *  and the euro survive; emoji and scripts the fonts lack are dropped.
 * ===========================================================================*/

/* ---- text --------------------------------------------------------------------- */
int  uni_to_cp1252(uint32_t cp);                    /* a byte, 0 to drop, -1 none */
int  utf8_to_cp1252(const char *in, int len, char *out, int cap);  /* returns length */
void entities_decode(char *s);                      /* in place, cp1252 out      */

/* ---- URLs ----------------------------------------------------------------------- */
void url_resolve(const char *base, const char *href, char *out, int cap);
void url_host(const char *url, char *out, int cap);
void url_encode(const char *in, char *out, int cap);       /* a query value     */
void url_decode(const char *in, int len, char *out, int cap);
int  url_param(const char *url, const char *name, char *out, int cap);   /* 1 if found */

/* ---- JSON ------------------------------------------------------------------------ *
 *  A whole document parsed into a tree in one arena, freed in one call.
 *  Strings are UTF-8 with the escapes undone.                                  */
enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct jv {
    uint8_t     t;
    int         n;                 /* members or elements                       */
    const char *key;               /* as an object member                       */
    const char *s;                 /* J_STR: the text; J_NUM: as written        */
    struct jv  *kid, *next;
} jv;
typedef struct jarena jarena;

jv         *json_parse(const char *s, int len, jarena **arena);   /* NULL on error */
void        json_free(jarena *a);
jv         *json_get(const jv *o, const char *key);
jv         *json_at(const jv *a, int i);
/* "a.b.0.c": members by name, elements by index. */
jv         *json_path(const jv *v, const char *path);
const char *json_s(const jv *v);                   /* "" unless a string          */
long long   json_i(const jv *v);
/* Depth-first: every object member named `key`, handed to fn; stop when fn returns 1. */
void        json_each(const jv *root, const char *key, int (*fn)(const jv *val, void *ud), void *ud);
/* The text of YouTube's {"runs":[{"text":..}..]} or {"simpleText":..} */
int         json_text(const jv *v, char *out, int cap);

/* ---- fetching (mini/netcompat.c) ---------------------------------------------------- */
/* GET (or POST with a body) following up to five redirects. `final` gets the
 * URL that answered. Returns the body length, or < 0 with *status explaining
 * (-3 DNS, -4 connect, -5 TLS). */
int web_request(const char *method, const char *url, const char *headers, const void *body, int body_len,
                char *out, int cap, int *status, char *final, int final_cap);
