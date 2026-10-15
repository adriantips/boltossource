#include <stdint.h>
#include "commands.h"
#include "gui.h"
#include "settings.h"
#include "backlight.h"
#include "boot.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_display.c
 *  `display` -- the panel, the desktop on it, and the scale between them.
 *
 *  The Settings app has had the resolution chips for a long time and the scale
 *  chips since this commit. Neither is reachable from a prompt, which matters
 *  more than it sounds: a machine whose panel came up at a size that makes the
 *  desktop unusable cannot be fixed by clicking something on that desktop, and
 *  a session over SSH has no desktop to click at all.
 * ===========================================================================*/

static void show(void) {
    int pw = gui_panel_w(), ph = gui_panel_h();
    int pct = settings_scale_percent(g_settings.scale_index, pw, ph);
    kprintf("panel    %d x %d\n", pw, ph);
    kprintf("desktop  %d x %d\n", gui_screen_w(), gui_screen_h());
    kprintf("scale    %s (%d%%)", settings_scale_name(g_settings.scale_index), pct);
    /* A scale that would leave less than 640x480 to work in is declined, and
     * saying "200%" while showing a native desktop would be a lie. */
    if (gui_screen_w() == pw && pct != 100)
        kprintf("  -- declined: %d%% leaves too little desktop", pct);
    kprintf("\n");

    kprintf("bright   %d%% (%s)\n", backlight_get(), backlight_name());
    /* What is on the panel, not what /home/.wallpaper says: the boot restore
     * stopped writing that file with the glass desktop, and a swap that
     * follows a mode change never did, so it can name a picture nobody is
     * looking at. */
    const char *wp = gui_wallpaper_path();
    kprintf("wallpaper %s\n", wp && wp[0] ? wp : "none (a colour field)");
    int us = gui_frame_copy_us(), bl = gui_blend_us();
    kprintf("motion   %s, animations %s (a frame copy takes %d.%d ms, a blend %d.%d)\n",
            settings_motion_name(g_settings.motion), gui_motion_reduced() ? "off" : "on",
            us / 1000, us / 100 % 10, bl / 1000, bl / 100 % 10);
    int fr, last, avg, worst;
    gui_frame_stats(&fr, &last, &avg, &worst);
    kprintf("frames   %d since last asked; last %d.%d ms, mean %d.%d ms, worst %d.%d ms\n",
            fr, last / 10, last % 10, avg / 10, avg % 10, worst / 10, worst % 10);
    int st[4], nd;
    gui_frame_stages(st, &nd);
    kprintf("worst    windows %d.%d, composite %d.%d, shell %d.%d, present %d.%d ms; %d rects\n",
            st[0] / 10, st[0] % 10, st[1] / 10, st[1] % 10, st[2] / 10, st[2] % 10,
            st[3] / 10, st[3] % 10, nd);
    kprintf("scales  ");
    for (int i = 0; i < settings_scale_count(); i++)
        kprintf(" %s%s", settings_scale_name(i), i == g_settings.scale_index ? "*" : "");
    kprintf("\nmodes   ");
    for (int i = 0; i < settings_res_count(); i++)
        kprintf(" %s%s", settings_res_name(i), i == g_settings.res_index ? "*" : "");
    kprintf("\n");
}

int cmd_display(int argc, char **argv) {
    if (boot_is_safe()) {
        kprintf("display: the desktop is not running (safe mode)\n");
        return 1;
    }

    if (argc < 2) { show(); return 0; }

    if (strcmp(argv[1], "bench") == 0) { gui_bench(); return 0; }

    if (strcmp(argv[1], "scale") == 0) {
        if (argc < 3) { kprintf("usage: display scale NAME   (e.g. 150%%, auto)\n"); return 1; }
        for (int i = 0; i < settings_scale_count(); i++) {
            const char *nm = settings_scale_name(i);
            if (strcasecmp(argv[2], nm) != 0) continue;
            g_settings.scale_index = i;
            settings_apply();
            show();
            return 0;
        }
        kprintf("display: no scale called %s\n", argv[2]);
        return 1;
    }

    if (strcmp(argv[1], "brightness") == 0 || strcmp(argv[1], "bright") == 0) {
        if (argc < 3) { kprintf("usage: display brightness 10..100\n"); return 1; }
        int pct = atoi(argv[2]);
        if (pct < 10 || pct > 100) {
            /* Refused rather than clamped: somebody who types 5 wants 5, and a
             * screen they cannot read well enough to undo it with is worse
             * than being told no. */
            kprintf("display: brightness is 10 to 100 (%s)\n", argv[2]);
            return 1;
        }
        if (backlight_set(pct) != 0) { kprintf("display: the backlight refused\n"); return 1; }
        show();
        return 0;
    }

    if (strcmp(argv[1], "motion") == 0) {
        if (argc < 3) { kprintf("usage: display motion auto|full|reduced\n"); return 1; }
        for (int i = 0; i < MOTION_COUNT; i++) {
            if (strcasecmp(argv[2], settings_motion_name(i)) != 0) continue;
            g_settings.motion = i;
            gui_motion_apply();
            show();
            return 0;
        }
        kprintf("display: motion is auto, full or reduced (%s)\n", argv[2]);
        return 1;
    }

    if (strcmp(argv[1], "res") == 0) {
        if (argc < 3) { kprintf("usage: display res NAME   (e.g. 1080p)\n"); return 1; }
        for (int i = 0; i < settings_res_count(); i++) {
            if (strcasecmp(argv[2], settings_res_name(i)) != 0) continue;
            g_settings.res_index = i;
            settings_apply();
            show();
            return 0;
        }
        kprintf("display: no mode called %s\n", argv[2]);
        return 1;
    }

    kprintf("usage: display | display scale NAME | display res NAME"
            " | display brightness PCT | display motion MODE\n");
    return 1;
}
