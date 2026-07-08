#include <stdint.h>
#include <stddef.h>

/* The compiler may emit calls to these for struct copies / zeroing even in a
 * freestanding build, so they must exist. */
void *memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *a = d; const uint8_t *b = s;
    while (n--) *a++ = *b++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    uint8_t *a = d; const uint8_t *b = s;
    if (a < b) { while (n--) *a++ = *b++; }
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}
char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}
char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i + 1 < n && s[i]; i++) d[i] = s[i];
    if (n) d[i] = 0;
    return d;
}
char *strcat(char *d, const char *s) {
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++)) {}
    return r;
}
/* strlcat-style: append s to d without exceeding cap bytes total (incl NUL). */
char *kstrlcat(char *d, const char *s, size_t cap) {
    size_t dl = 0;
    while (dl < cap && d[dl]) dl++;
    size_t i = 0;
    while (dl + i + 1 < cap && s[i]) { d[dl + i] = s[i]; i++; }
    if (dl + i < cap) d[dl + i] = 0;
    return d;
}
char *strchr(const char *s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return (c == 0) ? (char *)s : 0;
}
char *strrchr(const char *s, int c) {
    const char *last = 0;
    for (; *s; s++) if (*s == (char)c) last = s;
    if (c == 0) return (char *)s;
    return (char *)last;
}
int atoi(const char *s) {
    int sign = 1, v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v * sign;
}
