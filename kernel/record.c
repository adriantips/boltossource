/* ===========================================================================
 *  BoltOS  -  kernel/record.c
 *  Recording the screen: sample, diff, encode, write an APNG.
 *
 *  The two buffers are the whole design. `cur` is what the panel looks like
 *  now, `prev` is what it looked like when the last frame was written, and the
 *  only thing that goes into the file is the rectangle where they differ. A
 *  desktop changes inside one window at a time, so that rectangle is usually a
 *  few per cent of the screen -- which is the difference between ten seconds
 *  of recording being 400 KB and being 30 MB.
 *
 *  A frame nobody can distinguish from the one before it is not written at
 *  all: the previous frame's duration is extended instead. Sitting still
 *  costs nothing, which is what makes recording a terminal session practical.
 * ========================================================================= */
#include <stdint.h>
#include <stdio.h>
#include "record.h"
#include "framebuffer.h"
#include "png_enc.h"
#include "screenshot.h"
#include "kheap.h"
#include "string.h"
#include "fs.h"
#include "pit.h"
#include "users.h"
#include "notify.h"

static png_anim  *anim;
static uint32_t  *cur, *prev;
static int        rec_w, rec_h;
static int        interval_ms;
static int        fps_asked;
static uint64_t   t_start, t_next, t_frame;
static char       out_path[FS_PATH_MAX];
static int        stopping;          /* re-entry guard: rec_pump runs from the
                                        compositor and rec_stop from a command */

const char *rec_strerror(int rc) {
    switch (rc) {
        case  0: return "ok";
        case -1: return "already recording";
        case -2: return "no display to record";
        case -3: return "out of memory";
        case -4: return "could not write the file";
        case -5: return "could not start the encoder";
        case -6: return "nothing was captured";
        /* -7 rather than reusing -1: rec_start's "already recording" and
         * rec_stop's "not recording" are opposite conditions, and sharing a
         * code made `rec stop` with nothing running say "already recording". */
        case -7: return "not recording";
        default: return "failed";
    }
}

int      rec_active(void) { return anim != 0; }
int      rec_frames(void) { return png_anim_frames(anim); }
uint32_t rec_bytes(void)  { return png_anim_bytes(anim); }
int      rec_fps(void)    { return fps_asked; }
const char *rec_path(void) { return out_path; }
uint32_t rec_elapsed_ms(void) { return anim ? (uint32_t)(pit_ticks() - t_start) : 0; }

static void rec_free(void) {
    kfree(cur);  cur  = 0;
    kfree(prev); prev = 0;
}

/* ~/Videos/screencast-N.png for the lowest free N. Numbered for the same
 * reason screenshots are: the clock may not have been set yet. */
static void next_path(char *out, uint32_t cap) {
    const char *home = users_home();
    if (!home || !home[0] || (home[0] == '/' && !home[1])) home = "/home";
    for (int n = 1; n < 10000; n++) {
        snprintf(out, cap, "%s/Videos/screencast-%d.png", home, n);
        if (!fs_lookup(out)) return;
    }
    snprintf(out, cap, "%s/Videos/screencast.png", home);
}

int rec_start(const char *path, int fps) {
    if (anim) return -1;
    if (!fb_present()) return -2;

    int w = (int)fb_width(), h = (int)fb_height();
    if (w <= 0 || h <= 0) return -2;

    if (fps <= 0)  fps = 5;
    if (fps > 30)  fps = 30;
    fps_asked   = fps;
    interval_ms = 1000 / fps;

    cur  = (uint32_t *)kmalloc((uint64_t)w * h * 4);
    prev = (uint32_t *)kmalloc((uint64_t)w * h * 4);
    if (!cur || !prev) { rec_free(); return -3; }

    anim = png_anim_begin(w, h);
    if (!anim) { rec_free(); return -5; }

    rec_w = w; rec_h = h;
    if (path && path[0]) snprintf(out_path, sizeof out_path, "%s", path);
    else                 next_path(out_path, sizeof out_path);

    t_start = t_next = t_frame = pit_ticks();
    stopping = 0;
    rec_pump();                     /* frame 0 now, so a short recording has one */
    return 0;
}

/* The bounding box of everything that differs between cur and prev. Rows are
 * compared whole first -- memcmp on 4 KB is cheap and most rows match -- and
 * only the rows that differ are walked column by column. Returns 0 when the
 * two images are identical. */
static int diff_rect(int *ox, int *oy, int *ow, int *oh) {
    int y0 = -1, y1 = -1, x0 = rec_w, x1 = -1;
    uint32_t rowbytes = (uint32_t)rec_w * 4;

    for (int y = 0; y < rec_h; y++) {
        const uint32_t *a = cur + (uint64_t)y * rec_w;
        const uint32_t *b = prev + (uint64_t)y * rec_w;
        if (memcmp(a, b, rowbytes) == 0) continue;
        if (y0 < 0) y0 = y;
        y1 = y;
        int lx = 0;           while (lx < rec_w && a[lx] == b[lx]) lx++;
        int rx = rec_w - 1;   while (rx > lx && a[rx] == b[rx]) rx--;
        if (lx < x0) x0 = lx;
        if (rx > x1) x1 = rx;
    }
    if (y0 < 0) return 0;
    *ox = x0; *oy = y0; *ow = x1 - x0 + 1; *oh = y1 - y0 + 1;
    return 1;
}

void rec_pump(void) {
    if (!anim || stopping) return;

    uint64_t now = pit_ticks();
    if (png_anim_frames(anim) && now < t_next) return;
    t_next = now + (uint64_t)interval_ms;

    if (fb_capture(cur) != 0) return;

    /* However long the newest frame has been up, that is its duration. This is
     * measured rather than assumed because the encoder cannot always keep to
     * the interval, and a recording that claims 5 fps while managing 2 plays
     * back at half speed. */
    if (png_anim_frames(anim)) png_anim_hold(anim, (uint32_t)(now - t_frame));

    int x, y, w, h;
    int first = (png_anim_frames(anim) == 0);
    if (first) { x = y = 0; w = rec_w; h = rec_h; }
    else if (!diff_rect(&x, &y, &w, &h)) return;    /* nothing moved; it just stays up */

    if (png_anim_add(anim, cur, rec_w, x, y, w, h) != 0) {
        notify_error("Screen recording", "the encoder failed; the recording was kept");
        return;
    }
    /* Only the rectangle just written catches prev up. The rest of prev is
     * still what the last frame said the screen was, which is exactly what the
     * next diff has to compare against. */
    for (int r = 0; r < h; r++) {
        memcpy(prev + (uint64_t)(y + r) * rec_w + x,
               cur  + (uint64_t)(y + r) * rec_w + x, (uint32_t)w * 4);
    }
    t_frame = pit_ticks();

    /* Stop ourselves before the heap has an opinion. The file is written, not
     * thrown away -- somebody recording a bug wants the part that fitted. */
    if (png_anim_bytes(anim) >= REC_MAX_BYTES || png_anim_frames(anim) >= REC_MAX_FRAMES) {
        char saved[FS_PATH_MAX];
        int rc = rec_stop(saved, sizeof saved);
        char msg[NOTE_TEXT_MAX];
        if (rc == 0) snprintf(msg, sizeof msg, "Reached the size limit; saved %s", saved);
        else         snprintf(msg, sizeof msg, "Reached the size limit and %s", rec_strerror(rc));
        notify_warn("Screen recording", msg);
    }
}

int rec_stop(char *saved, uint32_t cap) {
    if (!anim) return -7;
    stopping = 1;

    if (png_anim_frames(anim)) png_anim_hold(anim, (uint32_t)(pit_ticks() - t_frame));

    uint8_t *png = 0; uint32_t len = 0;
    int rc = png_anim_finish(anim, &png, &len);      /* frees anim either way */
    anim = 0;
    rec_free();
    stopping = 0;
    if (rc != 0 || !png) return -6;

    if (shot_ensure_parent(out_path) != 0) { kfree(png); return -4; }
    fs_node *f = fs_lookup(out_path);
    if (!f) {
        char leaf[FS_NAME_MAX];
        fs_node *d = fs_parent_of(out_path, leaf, sizeof leaf);
        if (!fs_may_create_in(d)) { kfree(png); return -4; }
        f = fs_create(out_path, 0);
    }
    if (!f || f->is_dir || fs_write(f, png, len) != 0) { kfree(png); return -4; }
    kfree(png);

    if (saved && cap) snprintf(saved, cap, "%s", out_path);
    return 0;
}

void rec_cancel(void) {
    if (!anim) return;
    stopping = 1;
    png_anim_abort(anim);
    anim = 0;
    rec_free();
    stopping = 0;
}
