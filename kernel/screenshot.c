/* ===========================================================================
 *  BoltOS  -  kernel/screenshot.c
 *  Capture the panel, encode it, put it in the user's Pictures folder.
 *
 *  The interesting decision here is what "the screen" means. The compositor's
 *  backbuffer is the obvious source and the wrong one: it holds what the
 *  window manager drew, which is not what a person is looking at whenever a
 *  ring-3 program owns the panel (DOOM, the browser) or the desktop is not
 *  running at all (the text console, safe mode, the boot log). Reading VRAM
 *  back costs a few milliseconds and is the only version that never lies.
 * ========================================================================= */
#include <stdint.h>
#include <stdio.h>
#include "screenshot.h"
#include "framebuffer.h"
#include "png_enc.h"
#include "kheap.h"
#include "string.h"
#include "fs.h"
#include "users.h"
#include "notify.h"

const char *shot_strerror(int rc) {
    switch (rc) {
        case  0: return "ok";
        case -1: return "no display to capture";
        case -2: return "out of memory";
        case -3: return "could not encode the image";
        case -4: return "could not write the file";
        default: return "failed";
    }
}

void shot_dir(char *out, uint32_t cap) {
    const char *home = users_home();
    if (!home || !home[0] || (home[0] == '/' && !home[1])) home = "/home";
    snprintf(out, cap, "%s/Pictures", home);
}

/* ~/Pictures/screenshot-N.png for the lowest N that is free. Numbered rather
 * than timestamped because the clock can be wrong -- before NTP has run, every
 * screenshot in a session would collide on the same minute. */
static void next_path(char *out, uint32_t cap) {
    char dir[FS_PATH_MAX];
    shot_dir(dir, sizeof dir);
    for (int n = 1; n < 10000; n++) {
        snprintf(out, cap, "%s/screenshot-%d.png", dir, n);
        if (!fs_lookup(out)) return;
    }
    snprintf(out, cap, "%s/screenshot.png", dir);      /* overwrite as a last resort */
}

/* Create the directory chain for `path`'s parent if it is missing: a fresh
 * account has a Pictures folder, but a screenshot into an arbitrary -o path
 * should not fail because one directory does not exist yet. */
static int ensure_parent(const char *path) {
    char dir[FS_PATH_MAX];
    uint32_t n = 0;
    for (const char *p = path; *p && n < sizeof dir - 1; p++) dir[n++] = *p;
    dir[n] = 0;
    while (n && dir[n - 1] != '/') n--;
    if (n <= 1) return 0;                              /* the root always exists */
    dir[n - 1] = 0;
    if (fs_lookup(dir)) return 0;
    /* walk it, making each missing component */
    for (uint32_t i = 1; i <= n - 1; i++) {
        if (dir[i] && dir[i] != '/') continue;
        char save = dir[i];
        dir[i] = 0;
        if (!fs_lookup(dir) && !fs_create(dir, 1)) { dir[i] = save; return -1; }
        dir[i] = save;
    }
    return 0;
}

int shot_take(const char *path, char *saved, uint32_t cap) {
    if (!fb_present()) return -1;

    uint32_t w = fb_width(), h = fb_height();
    if (!w || !h) return -1;

    uint32_t *px = (uint32_t *)kmalloc((uint64_t)w * h * 4);
    if (!px) return -2;
    if (fb_capture(px) != 0) { kfree(px); return -1; }

    uint8_t *png = 0; uint32_t len = 0;
    int rc = png_encode(px, (int)w, (int)h, &png, &len);
    kfree(px);
    if (rc != 0 || !png) return -3;

    char target[FS_PATH_MAX];
    if (path && path[0]) snprintf(target, sizeof target, "%s", path);
    else                 next_path(target, sizeof target);

    if (ensure_parent(target) != 0) { kfree(png); return -4; }

    fs_node *f = fs_lookup(target);
    if (!f) {
        char leaf[FS_NAME_MAX];
        fs_node *d = fs_parent_of(target, leaf, sizeof leaf);
        if (!fs_may_create_in(d)) { kfree(png); return -4; }
        f = fs_create(target, 0);
    }
    if (!f || f->is_dir || fs_write(f, png, len) != 0) { kfree(png); return -4; }
    kfree(png);

    if (saved && cap) snprintf(saved, cap, "%s", target);
    return 0;
}
