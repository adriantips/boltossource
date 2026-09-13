#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Screenshots.
 *
 *  Up to now the only way to get a picture of BoltOS was shot.py, running on
 *  the host, talking to QEMU's monitor. That is a way to photograph a virtual
 *  machine, not a feature of an operating system: on real hardware there was
 *  no way at all, and inside the OS there was nothing to point at.
 *
 *  So: capture the panel, encode a PNG, write it into the user's Pictures
 *  directory, and say so. Print Screen does it from anywhere in the desktop;
 *  `screenshot` does it from a terminal.
 * ========================================================================= */

/* Capture the screen to `path`, or to the next free
 * ~/Pictures/screenshot-N.png when `path` is NULL.
 *
 * Returns 0 and, if `saved` is non-NULL, fills it with the path actually used
 * (cap bytes). Negative on failure:
 *   -1 no panel to capture      -2 out of memory
 *   -3 PNG encoding failed      -4 could not create or write the file
 * The caller decides how to report it -- shot_strerror() puts words to it. */
int  shot_take(const char *path, char *saved, uint32_t cap);
const char *shot_strerror(int rc);

/* Create the directory chain for `path`'s parent if any of it is missing.
 * Shared with the screen recorder, which writes files into ~/Videos the same
 * way this writes them into ~/Pictures, and there is no reason for two of
 * these. 0, or -1 if a directory could not be made. */
int  shot_ensure_parent(const char *path);

/* Where an unnamed screenshot goes for the signed-in account, e.g.
 * "/home/ada/Pictures". Falls back to /home/Pictures with no session. */
void shot_dir(char *out, uint32_t cap);
