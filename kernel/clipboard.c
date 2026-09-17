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

/* ---- history ------------------------------------------------------------- *
 *  Every write lands here too, newest first. `recording` is the guard that
 *  stops clip_hist_restore() -- which puts an old entry back by calling
 *  clip_set() -- from pushing a copy of it onto the front; without it,
 *  restoring the third entry three times leaves you with four of them.
 * -------------------------------------------------------------------------*/
typedef struct {
    int       kind;
    char     *text;        /* NUL-terminated; TEXT and FILES both live here */
    int       len;
    uint32_t *img;
    int       iw, ih;
    int       pinned;
} hist_t;

static hist_t   g_hist[CLIP_HIST];
static int      g_nhist;
static uint32_t g_imgbytes;       /* what the kept images add up to */
static int      recording = 1;

static void hist_push_text(int kind, const char *s, int len);
static void hist_push_image(const uint32_t *px, int w, int h);

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
    hist_push_text(CLIP_TEXT, g_clip, g_len);
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
    hist_push_image(g_img, w, h);
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
    /* clip_set has already pushed it as text; the only thing wrong with that
     * history entry is its label, and re-pushing would duplicate it. Not while
     * restoring, though -- nothing was pushed then, and stamping entry 0 would
     * relabel whatever happens to be sitting at the front. */
    if (recording && g_nhist) g_hist[0].kind = CLIP_FILES;
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

/* ===========================================================================
 *  History, the implementation.
 * ===========================================================================*/
static void hist_free(hist_t *h) {
    if (h->text) kfree(h->text);
    if (h->img)  { g_imgbytes -= (uint32_t)h->iw * (uint32_t)h->ih * 4; kfree(h->img); }
    memset(h, 0, sizeof *h);
}

static void hist_drop(int i) {
    if (i < 0 || i >= g_nhist) return;
    hist_free(&g_hist[i]);
    for (int k = i; k < g_nhist - 1; k++) g_hist[k] = g_hist[k + 1];
    memset(&g_hist[--g_nhist], 0, sizeof g_hist[0]);
}

/* Evict from the back, skipping pins. 0 if no slot could be freed, which means
 * every entry is pinned -- and then the right answer is to not record, rather
 * than to throw away something somebody asked to keep. */
static int hist_make_room(void) {
    if (g_nhist < CLIP_HIST) return 1;
    for (int i = g_nhist - 1; i >= 0; i--)
        if (!g_hist[i].pinned) { hist_drop(i); return 1; }
    return 0;
}

static int hist_free_image_bytes(uint32_t need) {
    while (g_imgbytes + need > CLIP_HIST_IMG) {
        int victim = -1;
        for (int i = g_nhist - 1; i >= 0; i--)
            if (g_hist[i].img && !g_hist[i].pinned) { victim = i; break; }
        if (victim < 0) return 0;
        hist_drop(victim);
    }
    return 1;
}

static void hist_to_front(int i) {
    if (i <= 0 || i >= g_nhist) return;
    hist_t t = g_hist[i];
    for (int k = i; k > 0; k--) g_hist[k] = g_hist[k - 1];
    g_hist[0] = t;
}

/* Copying the same thing twice moves it to the front rather than filling the
 * ring with it -- Ctrl+C on a selection you are re-reading is a common idle
 * habit and it used to flush everything else out. */
static void hist_push_text(int kind, const char *s, int len) {
    if (!recording || !s || len <= 0) return;
    for (int i = 0; i < g_nhist; i++) {
        hist_t *h = &g_hist[i];
        if (h->text && h->len == len && !memcmp(h->text, s, (uint32_t)len)) {
            h->kind = kind;
            hist_to_front(i);
            return;
        }
    }
    if (!hist_make_room()) return;
    char *copy = (char *)kmalloc((uint64_t)len + 1);
    if (!copy) return;                         /* a full heap is not worth a panic */
    memcpy(copy, s, (uint32_t)len);
    copy[len] = 0;

    for (int k = g_nhist; k > 0; k--) g_hist[k] = g_hist[k - 1];
    memset(&g_hist[0], 0, sizeof g_hist[0]);
    g_hist[0].kind = kind; g_hist[0].text = copy; g_hist[0].len = len;
    g_nhist++;
}

static void hist_push_image(const uint32_t *px, int w, int h) {
    if (!recording || !px || w <= 0 || h <= 0) return;
    uint32_t bytes = (uint32_t)w * (uint32_t)h * 4;
    if (bytes > CLIP_HIST_IMG) return;         /* too big to be worth keeping */
    if (!hist_free_image_bytes(bytes)) return;
    if (!hist_make_room()) return;

    uint32_t *copy = (uint32_t *)kmalloc(bytes);
    if (!copy) return;
    memcpy(copy, px, bytes);

    for (int k = g_nhist; k > 0; k--) g_hist[k] = g_hist[k - 1];
    memset(&g_hist[0], 0, sizeof g_hist[0]);
    g_hist[0].kind = CLIP_IMAGE; g_hist[0].img = copy;
    g_hist[0].iw = w; g_hist[0].ih = h;
    g_nhist++;
    g_imgbytes += bytes;
}

static int hvalid(int i) { return i >= 0 && i < g_nhist; }

int clip_hist_count(void)   { return g_nhist; }
int clip_hist_kind(int i)   { return hvalid(i) ? g_hist[i].kind : CLIP_EMPTY; }
int clip_hist_len(int i)    { return hvalid(i) ? g_hist[i].len : 0; }
int clip_hist_pinned(int i) { return hvalid(i) ? g_hist[i].pinned : 0; }

const char *clip_hist_text(int i) {
    if (!hvalid(i) || !g_hist[i].text) return "";
    return g_hist[i].text;
}

const uint32_t *clip_hist_image(int i, int *w, int *h) {
    if (!hvalid(i) || g_hist[i].kind != CLIP_IMAGE) return 0;
    if (w) *w = g_hist[i].iw;
    if (h) *h = g_hist[i].ih;
    return g_hist[i].img;
}

void clip_hist_pin(int i, int on) { if (hvalid(i)) g_hist[i].pinned = on ? 1 : 0; }
void clip_hist_remove(int i)      { hist_drop(i); }

void clip_hist_clear(void) {
    for (int i = g_nhist - 1; i >= 0; i--) if (!g_hist[i].pinned) hist_drop(i);
}

int clip_hist_restore(int i) {
    if (!hvalid(i)) return -1;
    hist_t *h = &g_hist[i];
    recording = 0;
    int rc = 0;
    if (h->kind == CLIP_IMAGE)      rc = clip_set_image(h->img, h->iw, h->ih);
    else if (h->kind == CLIP_FILES) rc = clip_set_files(h->text, h->len);
    else                            clip_set(h->text, h->len);
    recording = 1;
    if (rc != 0) return rc;
    /* It is the most recent thing copied now, whatever it was before. */
    hist_to_front(i);
    return 0;
}
