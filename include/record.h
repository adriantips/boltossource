#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Screen recording.
 *
 *  shot.py and shot_uefi.py record a BoltOS session by asking QEMU for frames.
 *  That is a way to film a virtual machine; it is not something the operating
 *  system can do, and on real hardware there was nothing to ask.
 *
 *  So: sample the panel from the compositor's own loop, keep only the
 *  rectangle that changed since the last sample, and write the lot as an
 *  animated PNG. APNG rather than a video codec because the machine already
 *  has a PNG encoder and does not have an H.264 one, because the first frame
 *  of an APNG is a valid still image for anything that cannot play it, and
 *  because every browser -- including the one in this OS -- plays the rest.
 *
 *  Honest limits, which `rec status` also prints: the encoder runs on the
 *  compositor's thread, so a recording makes the desktop slower while it is
 *  running, and the frame rate is a request rather than a promise. Frame
 *  durations are measured rather than assumed, so the result plays back at the
 *  speed the machine actually managed instead of the speed it was asked for.
 * ========================================================================= */

#define REC_MAX_BYTES  (8u * 1024 * 1024)   /* stop before the heap notices */
#define REC_MAX_FRAMES 1800

/* Start recording to `path`, or to the next free ~/Videos/screencast-N.png
 * when it is NULL. `fps` is the sampling rate (1..30; 0 picks 5).
 * 0 on success, or:
 *   -1 already recording   -2 no panel   -3 out of memory
 *   -5 could not start the encoder */
int rec_start(const char *path, int fps);

/* Stop and write the file. Fills `saved` with the path used. 0, or:
 *   -7 not recording   -4 could not write the file   -6 nothing was captured */
int rec_stop(char *saved, uint32_t cap);

void rec_cancel(void);              /* throw it away without writing anything */
const char *rec_strerror(int rc);

int      rec_active(void);
int      rec_frames(void);
uint32_t rec_bytes(void);
uint32_t rec_elapsed_ms(void);
int      rec_fps(void);
const char *rec_path(void);         /* where it will be written */

/* Called from the compositor loop. Samples the panel when the interval has
 * elapsed and does nothing at all when no recording is running. */
void rec_pump(void);
