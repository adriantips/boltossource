#include <stdint.h>
#include "commands.h"
#include "gui.h"
#include "boot.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_snap.c
 *  `snap` -- tiling from a prompt.
 *
 *  Win+arrow and a drag to the edge are how this gets used. The command is
 *  here for the same reason `desk` is: a session over SSH has no Win key, and
 *  a test that synthesises Win+Left mostly proves the keyboard driver works.
 *  Printing the rectangle back is the half that makes it testable at all.
 * ===========================================================================*/

static const struct { const char *name; int zone; } ZONES[] = {
    { "left",         SNAP_L    },
    { "right",        SNAP_R    },
    { "top-left",     SNAP_TL   },
    { "top-right",    SNAP_TR   },
    { "bottom-left",  SNAP_BL   },
    { "bottom-right", SNAP_BR   },
    { "max",          SNAP_MAX  },
    { "restore",      SNAP_NONE },
};
#define NZONE ((int)(sizeof(ZONES) / sizeof(ZONES[0])))

/* "restore" is the verb; "floating" is the state it leaves the window in, and
 * the table would happily report the former for a window nobody has ever
 * tiled. */
static const char *zone_name(int zone) {
    if (zone == SNAP_NONE) return "floating";
    for (int i = 0; i < NZONE; i++) if (ZONES[i].zone == zone) return ZONES[i].name;
    return "floating";
}

static void usage(void) {
    kprintf("usage: snap ZONE\n  zones:");
    for (int i = 0; i < NZONE; i++) kprintf(" %s", ZONES[i].name);
    kprintf("\n");
}

int cmd_snap(int argc, char **argv) {
    if (boot_is_safe()) {
        kprintf("snap: the desktop is not running (safe mode)\n");
        return 1;
    }

    char title[40];
    int x, y, w, h, zone;

    if (argc < 2) {
        if (gui_focused_rect(title, sizeof title, &x, &y, &w, &h, &zone) != 0) {
            kprintf("snap: no focused window\n");
            return 1;
        }
        kprintf("%s: %s  %dx%d+%d+%d\n", title, zone_name(zone), w, h, x, y);
        usage();
        return 0;
    }

    for (int i = 0; i < NZONE; i++) {
        if (strcmp(argv[1], ZONES[i].name) != 0) continue;
        if (gui_snap_focused(ZONES[i].zone) != 0) {
            kprintf("snap: no focused window\n");
            return 1;
        }
        gui_focused_rect(title, sizeof title, &x, &y, &w, &h, &zone);
        kprintf("snap: %s is %s  %dx%d+%d+%d\n", title, zone_name(zone), w, h, x, y);
        return 0;
    }

    kprintf("snap: no zone called %s\n", argv[1]);
    usage();
    return 1;
}
