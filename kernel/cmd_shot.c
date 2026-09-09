#include <stdint.h>
#include <stdio.h>
#include "commands.h"
#include "screenshot.h"
#include "framebuffer.h"
#include "notify.h"
#include "kprintf.h"
#include "string.h"
#include "fs.h"

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_shot.c
 *  `screenshot` -- the same capture Print Screen does, from a terminal.
 * ===========================================================================*/

int cmd_screenshot(int argc, char **argv) {
    const char *path = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            kprintf("usage: screenshot [PATH]\n"
                    "  Capture the screen to a PNG. With no PATH, writes the next\n"
                    "  free ~/Pictures/screenshot-N.png. Print Screen does the same\n"
                    "  thing from the desktop.\n");
            return 0;
        }
        if (argv[i][0] == '-') { kprintf("screenshot: unknown option %s\n", argv[i]); return 1; }
        path = argv[i];
    }

    char saved[FS_PATH_MAX];
    int rc = shot_take(path, saved, sizeof saved);
    if (rc != 0) {
        kprintf("screenshot: %s\n", shot_strerror(rc));
        return 1;
    }

    fs_node *f = fs_lookup(saved);
    uint32_t bytes = f ? f->size : 0;
    kprintf("screenshot: %s  %ux%u  %u bytes\n", saved, fb_width(), fb_height(), bytes);

    char msg[NOTE_TEXT_MAX];
    snprintf(msg, sizeof msg, "Saved %s", saved);
    notify_success("Screenshot", msg);
    return 0;
}
