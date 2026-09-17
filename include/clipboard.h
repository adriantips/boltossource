#pragma once
#include <stdint.h>
/* ===========================================================================
 *  BoltOS system clipboard.
 *
 *  It used to hold text and nothing else, which meant that copying a picture
 *  or a file was not a thing the machine could do: Paint could not hand a
 *  drawing to Photos, and File Explorer's Ctrl+C did nothing at all. A
 *  clipboard that only carries one kind of thing is a text buffer.
 *
 *  So it is typed. One item at a time, of one of three kinds, and every reader
 *  asks what kind it is before taking it. Text stays exactly as it was -- the
 *  existing clip_set / clip_get / clip_len call sites are unchanged, and set
 *  the kind to CLIP_TEXT as a side effect.
 *
 *  Files are carried as their paths, newline-separated, which is deliberate:
 *  it means the file list is *also* text, so pasting into Notes gives you the
 *  paths and pasting into File Explorer copies the files. Nothing has to know
 *  about both representations.
 * ===========================================================================*/
#define CLIP_CAP       16384
#define CLIP_IMG_MAX   (4096 * 4096)     /* pixels; a cap, not an expectation */

enum { CLIP_EMPTY = 0, CLIP_TEXT, CLIP_IMAGE, CLIP_FILES };

int         clip_kind(void);                    /* CLIP_*                             */
const char *clip_kind_name(void);               /* "empty" / "text" / "image" / "files" */
void        clip_clear(void);

/* ---- text (unchanged) --------------------------------------------------- */
void        clip_set(const char *s, int len);   /* copy len bytes into the clipboard  */
const char *clip_get(void);                     /* NUL-terminated current contents    */
int         clip_len(void);                     /* byte length (0 == empty)           */

/* ---- images -------------------------------------------------------------- */
/* Copies w*h ARGB pixels into the clipboard's own storage, so the caller may
 * free or redraw its buffer immediately. 0 on success, -1 on bad size or OOM. */
int             clip_set_image(const uint32_t *px, int w, int h);
/* The stored image, or NULL when the clipboard holds something else. */
const uint32_t *clip_image(int *w, int *h);

/* ---- files --------------------------------------------------------------- */
/* `paths` is one absolute path per line. The clipboard also reads back as text,
 * so a paste into a text field gives the paths. 0 on success. */
int         clip_set_files(const char *paths, int len);
int         clip_file_count(void);
/* The i'th path into `out`; 0 if there is no such entry. */
int         clip_file_at(int i, char *out, uint32_t cap);

/* ---- history ------------------------------------------------------------- *
 *  A clipboard that holds one thing loses the thing you copied two minutes
 *  ago, which is the one you wanted. So every write is also pushed onto a
 *  ring, newest first, and the Clipboard History app puts any of them back.
 *
 *  Text costs nothing to keep. Images do not, so the ring holds a byte budget
 *  for them and evicts the oldest image when a new one will not fit -- a
 *  screenshot is three megabytes and sixteen of those is a third of the heap.
 *  Pinned entries are exempt from every kind of eviction, including Clear,
 *  which is the whole reason to pin one.
 * -------------------------------------------------------------------------*/
#define CLIP_HIST      16
#define CLIP_HIST_IMG  (24u * 1024 * 1024)   /* total bytes of kept images */

int             clip_hist_count(void);
int             clip_hist_kind(int i);                 /* CLIP_*; CLIP_EMPTY if no entry */
const char     *clip_hist_text(int i);                 /* "" for an image  */
int             clip_hist_len(int i);
const uint32_t *clip_hist_image(int i, int *w, int *h);
int             clip_hist_pinned(int i);
void            clip_hist_pin(int i, int on);
int             clip_hist_restore(int i);              /* entry i becomes the clipboard; 0 ok */
void            clip_hist_remove(int i);
void            clip_hist_clear(void);                 /* everything except the pins */
