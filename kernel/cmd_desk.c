#include <stdint.h>
#include "commands.h"
#include "gui.h"
#include "boot.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_desk.c
 *  `desk` -- the virtual desktops, from a prompt.
 *
 *  The keyboard shortcuts are the way anybody will actually use these. The
 *  command exists because the Terminal is a window like any other, because a
 *  session over SSH has no keyboard shortcuts to press, and because a test
 *  that has to synthesise Win+2 to check anything is a test that mostly checks
 *  the keyboard driver.
 * ===========================================================================*/

int cmd_desk(int argc, char **argv) {
    if (boot_is_safe()) {
        kprintf("desk: the desktop is not running (safe mode)\n");
        return 1;
    }

    int n = gui_desk_count(), cur = gui_desk_current();

    if (argc < 2 || strcmp(argv[1], "list") == 0) {
        for (int d = 0; d < n; d++) {
            char names[240];
            int c = gui_desk_windows(d, names, sizeof names);
            kprintf("%c %d  %d window%s%s%s\n",
                    d == cur ? '*' : ' ', d + 1, c, c == 1 ? "" : "s",
                    c ? "  " : "", c ? names : "");
        }
        kprintf("usage: desk N | desk move N\n");
        return 0;
    }

    if (strcmp(argv[1], "move") == 0) {
        if (argc < 3) { kprintf("usage: desk move N   (1..%d)\n", n); return 1; }
        int d = argv[2][0] - '0';
        if (argv[2][1] || d < 1 || d > n) { kprintf("desk: no desktop %s\n", argv[2]); return 1; }
        int rc = gui_desk_send(d - 1);
        if (rc == -2) { kprintf("desk: no focused window to move\n"); return 1; }
        if (rc != 0)  { kprintf("desk: already on desktop %d\n", d); return 1; }
        kprintf("desk: moved to desktop %d\n", d);
        return 0;
    }

    int d = argv[1][0] - '0';
    if (argv[1][1] || d < 1 || d > n) {
        kprintf("desk: no desktop %s (there are %d)\n", argv[1], n);
        return 1;
    }
    if (gui_desk_goto(d - 1) != 0) { kprintf("desk: cannot switch\n"); return 1; }
    kprintf("desk: on desktop %d\n", d);
    return 0;
}
