#include <stdint.h>
#include "commands.h"
#include "gui.h"
#include "settings.h"
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

    kprintf("usage: display | display scale NAME | display res NAME\n");
    return 1;
}
