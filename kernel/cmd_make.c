#include <stdint.h>
#include "commands.h"
#include "make.h"
#include "fs.h"
#include "kprintf.h"
#include "string.h"

/* `make` -- the front of kernel/make.c.
 *
 * Finds the makefile the way make always has (Makefile, then makefile, in the
 * directory -C named or the current one), collects the goals, and gets out of
 * the way.
 */

#define MAX_GOALS 16

static void usage(void) {
    kprintf("usage: make [-f FILE] [-C DIR] [-n] [-s] [TARGET ...]\n"
            "  -f FILE   use this makefile instead of ./Makefile\n"
            "  -C DIR    work in DIR\n"
            "  -n        print the recipes without running them\n"
            "  -s        run them without printing them\n"
            "\n"
            "Variables (= := +=), $(NAME), $@ $< $^, pattern rules with one %%,\n"
            "and .PHONY. Recipes are ordinary shell lines.\n"
            "\n"
            "A target is rebuilt when a prerequisite differs from what it was\n"
            "when the target was last built -- differs, not is newer, because\n"
            "file times here are uptime ticks and do not survive a reboot in an\n"
            "order that means anything. The record lives in .make.stamp beside\n"
            "the makefile; delete it and the next build does everything once.\n");
}

int cmd_make(int argc, char **argv) {
    const char *file = 0, *dir = 0;
    const char *goals[MAX_GOALS];
    int ngoal = 0, dry = 0, silent = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-f") && i + 1 < argc)      file = argv[++i];
        else if (!strcmp(a, "-C") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(a, "-n"))                 dry = 1;
        else if (!strcmp(a, "-s"))                 silent = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] == '-') { kprintf("make: unknown option %s\n", a); return 1; }
        else if (ngoal < MAX_GOALS) goals[ngoal++] = a;
        else { kprintf("make: too many targets (max %d)\n", MAX_GOALS); return 1; }
    }

    /* -C is done by moving, not by prefixing every path: a recipe is a shell
     * line and expects to run where the makefile lives, the way it would after
     * a `cd`. The old directory comes back whatever happens. */
    fs_node *back = 0;
    if (dir) {
        back = fs_cwd();
        if (fs_chdir(dir) != 0) { kprintf("make: %s: no such directory\n", dir); return 1; }
    }

    char path[FS_PATH_MAX];
    int rc = 1;
    if (file) strlcpy(path, file, sizeof path);
    else {
        static const char *names[] = { "Makefile", "makefile", "GNUmakefile" };
        const char *found = 0;
        for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
            fs_node *f = fs_lookup(names[i]);
            if (f && !f->is_dir) { found = names[i]; break; }
        }
        if (!found) {
            kprintf("make: no makefile here (looked for Makefile and makefile)\n");
            if (back) fs_set_cwd(back);
            return 1;
        }
        strlcpy(path, found, sizeof path);
    }

    char err[160] = "";
    rc = make_run(path, goals, ngoal, dry, silent, err, sizeof err);
    if (rc != 0 && err[0]) kprintf("make: %s: %s\n", path, err);

    if (back) fs_set_cwd(back);
    return rc == 0 ? 0 : 1;
}
