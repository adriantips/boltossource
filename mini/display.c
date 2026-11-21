/* ===========================================================================
 *  BoltOS Mini - mini/display.c
 *
 *  Resolution policy for the screen surfaces used by QEMU ramfb and VNC.
 *  The H618 HDMI scanout driver is not present yet; its future modeset path
 *  will feed the same screen dimensions and use the selected mode as its
 *  output timing.
 * ===========================================================================*/
#include "mini.h"
#include "stdio.h"

typedef struct { int w, h; const char *name; } mode_t;
static const mode_t modes[] = {
    { 480,   320, "480 × 320" },
    { 1024,  600, "1024 × 600" },
    { 1280,  720, "1280 × 720" },
    { 1600,  900, "1600 × 900" },
    { 1920, 1080, "1920 × 1080" },
};
static int selected;
static int automatic = 1;

int display_mode_count(void) { return (int)(sizeof modes / sizeof modes[0]); }
int display_mode_index(void) { return selected; }
int display_is_auto(void) { return automatic; }
const char *display_mode_name(int i) {
    return i >= 0 && i < display_mode_count() ? modes[i].name : modes[0].name;
}

/* Auto currently has a safe fallback. When the H618 HDMI driver is added,
 * this is where its EDID preferred-mode result will be selected. */
int display_set_auto(int on) {
    automatic = !!on;
    if (automatic) selected = 0;
    const mode_t *m = &modes[selected];
    if (scr_w != m->w || scr_h != m->h) {
        if (screen_init(m->w, m->h) != 0) return -1;
    }
    shell_request_frame();
    return 0;
}

int display_set_mode(int i) {
    if (i < 0 || i >= display_mode_count()) return -1;
    const mode_t *m = &modes[i];
    if (screen_init(m->w, m->h) != 0) return -1;
    selected = i;
    automatic = 0;
    shell_request_frame();
    return 0;
}

/* Called after the card's saved settings have been restored. */
int display_restore(int i, int auto_mode) {
    if (i < 0 || i >= display_mode_count()) i = 0;
    selected = i;
    automatic = !!auto_mode;
    if (automatic) selected = 0;
    const mode_t *m = &modes[selected];
    if (scr_w == m->w && scr_h == m->h) return 0;
    return screen_init(m->w, m->h);
}
