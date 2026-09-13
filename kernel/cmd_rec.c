#include <stdint.h>
#include <stdio.h>
#include "commands.h"
#include "record.h"
#include "framebuffer.h"
#include "boot.h"
#include "notify.h"
#include "kprintf.h"
#include "string.h"
#include "fs.h"

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_rec.c
 *  `rec` -- screen recording from a prompt.
 *
 *  Ctrl+Print Screen is how anyone will start one. This exists because a
 *  recording made to show somebody a bug usually wants to start *before* the
 *  thing you are about to type, and because a test cannot press Ctrl+PrtSc and
 *  then read anything back about what it produced.
 * ========================================================================= */

static void status(void) {
    if (!rec_active()) { kprintf("rec: not recording\n"); return; }
    uint32_t ms = rec_elapsed_ms();
    kprintf("rec: recording to %s\n", rec_path());
    kprintf("     %d frames, %u.%u s, %u KB, asked for %d fps\n",
            rec_frames(), ms / 1000, (ms % 1000) / 100,
            rec_bytes() / 1024, rec_fps());
}

int cmd_rec(int argc, char **argv) {
    if (boot_is_safe()) {
        kprintf("rec: the desktop is not running (safe mode)\n");
        return 1;
    }

    const char *sub = argc > 1 ? argv[1] : "status";

    if (strcmp(sub, "start") == 0) {
        int fps = 0;
        const char *path = 0;
        for (int i = 2; i < argc; i++) {
            if (argv[i][0] >= '0' && argv[i][0] <= '9') fps = atoi(argv[i]);
            else path = argv[i];
        }
        int rc = rec_start(path, fps);
        if (rc != 0) { kprintf("rec: %s\n", rec_strerror(rc)); return 1; }
        kprintf("rec: recording %ux%u to %s at %d fps\n",
                fb_width(), fb_height(), rec_path(), rec_fps());
        /* Worth saying out loud rather than leaving somebody to wonder why
         * everything got slower: the encoder shares the compositor's thread. */
        kprintf("     the desktop runs slower while this is on; `rec stop` ends it\n");
        return 0;
    }

    if (strcmp(sub, "stop") == 0) {
        char saved[FS_PATH_MAX];
        int rc = rec_stop(saved, sizeof saved);
        if (rc != 0) { kprintf("rec: %s\n", rec_strerror(rc)); return 1; }
        fs_node *f = fs_lookup(saved);
        kprintf("rec: %s  %u bytes\n", saved, f ? f->size : 0);
        char msg[NOTE_TEXT_MAX];
        snprintf(msg, sizeof msg, "Saved %s", saved);
        notify_success("Screen recording", msg);
        return 0;
    }

    if (strcmp(sub, "cancel") == 0) {
        if (!rec_active()) { kprintf("rec: not recording\n"); return 1; }
        rec_cancel();
        kprintf("rec: thrown away\n");
        return 0;
    }

    if (strcmp(sub, "status") == 0) { status(); return 0; }

    kprintf("usage: rec start [FPS] [PATH] | rec stop | rec cancel | rec status\n");
    return 1;
}
