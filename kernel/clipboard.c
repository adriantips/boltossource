/* ===========================================================================
 *  BoltOS  -  kernel/clipboard.c
 *  One item, of one of three kinds, shared by every app.
 *
 *  Text lives in a static buffer: no allocation, no failure path, and copying
 *  a selection must not be able to fail. An image cannot work that way -- a
 *  full screen is three megabytes -- so it is heap, freed when the clipboard
 *  is next written. Files are paths, and paths are text, so they are stored in
 *  the text buffer and only the kind differs. That is the whole reason to
 *  store them that way: pasting a file selection into Notes gives you a list
 *  of paths without Notes knowing anything about files.
 * ===========================================================================*/
#include "clipboard.h"
#include "kheap.h"
#include "string.h"

static char      g_clip[CLIP_CAP];
static int       g_len;
static int       g_kind;
static uint32_t *g_img;
static int       g_iw, g_ih;

static void drop_image(void) {
    if (g_img) { kfree(g_img); g_img = 0; }
    g_iw = g_ih = 0;
}

int clip_kind(void) { return g_kind; }

const char *clip_kind_name(void) {
    switch (g_kind) {
        case CLIP_TEXT:  return "text";
        case CLIP_IMAGE: return "image";
        case CLIP_FILES: return "files";
        default:         return "empty";
    }
}

void clip_clear(void) {
    drop_image();
    g_clip[0] = 0; g_len = 0; g_kind = CLIP_EMPTY;
}

/* ------------------------------------------------------------------ text --- */
void clip_set(const char *s, int len) {
    drop_image();
    if (!s || len <= 0) { g_len = 0; g_clip[0] = 0; g_kind = CLIP_EMPTY; return; }
    if (len > CLIP_CAP - 1) len = CLIP_CAP - 1;
    for (int i = 0; i < len; i++) g_clip[i] = s[i];
    g_clip[len] = 0;
    g_len = len;
    g_kind = CLIP_TEXT;
}

/* An image and a file list both read back as text, because every existing
 * paste target expects text and a clipboard that returned nothing to them
 * would be a regression. An image has no sensible text form, so it says so. */
const char *clip_get(void) {
    if (g_kind == CLIP_IMAGE) return "[image]";
    return g_clip;
}

int clip_len(void) {
    if (g_kind == CLIP_IMAGE) return 7;          /* strlen("[image]") */
    return g_len;
}

/* ----------------------------------------------------------------- image --- */
int clip_set_image(const uint32_t *px, int w, int h) {
    if (!px || w <= 0 || h <= 0) return -1;
    if ((uint64_t)w * h > CLIP_IMG_MAX) return -1;

    uint32_t *copy = (uint32_t *)kmalloc((uint64_t)w * h * 4);
    if (!copy) return -1;                        /* the old contents survive */
    memcpy(copy, px, (uint64_t)w * h * 4);

    drop_image();
    g_img = copy; g_iw = w; g_ih = h;
    g_clip[0] = 0; g_len = 0;
    g_kind = CLIP_IMAGE;
    return 0;
}

const uint32_t *clip_image(int *w, int *h) {
    if (g_kind != CLIP_IMAGE) return 0;
    if (w) *w = g_iw;
    if (h) *h = g_ih;
    return g_img;
}

/* ----------------------------------------------------------------- files --- */
int clip_set_files(const char *paths, int len) {
    if (!paths || len <= 0) return -1;
    clip_set(paths, len);                        /* stores it, sets CLIP_TEXT */
    if (!g_len) return -1;
    g_kind = CLIP_FILES;
    return 0;
}

int clip_file_count(void) {
    if (g_kind != CLIP_FILES || !g_len) return 0;
    int n = 0;
    for (int i = 0; i < g_len; i++) {
        if (g_clip[i] == '\n') continue;
        /* start of a line */
        if (i == 0 || g_clip[i - 1] == '\n') n++;
    }
    return n;
}

int clip_file_at(int i, char *out, uint32_t cap) {
    if (g_kind != CLIP_FILES || !out || !cap) return 0;
    int line = 0;
    for (int p = 0; p < g_len; ) {
        while (p < g_len && g_clip[p] == '\n') p++;
        if (p >= g_len) break;
        int e = p;
        while (e < g_len && g_clip[e] != '\n') e++;
        if (line == i) {
            uint32_t n = (uint32_t)(e - p);
            if (n > cap - 1) n = cap - 1;
            for (uint32_t k = 0; k < n; k++) out[k] = g_clip[p + k];
            out[n] = 0;
            return 1;
        }
        line++;
        p = e;
    }
    return 0;
}
