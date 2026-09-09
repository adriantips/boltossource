#include <stdint.h>
#include "commands.h"
#include "proc.h"
#include "fs.h"
#include "kprintf.h"
#include "string.h"
#include <stdio.h>

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_run.c
 *  `run` -- start a ring-3 program and wait for it.
 *
 *  Every ring-3 program on the machine used to have a command of its own:
 *  `js` started /bin/js, `webx` started /bin/browser, `crash ring3` started
 *  /bin/crashme. There was no way to run a program the kernel had never heard
 *  of -- which is exactly what a third-party program is. An OS whose userland
 *  is a fixed list is not one you can write software for.
 *
 *  So: `run PATH [args]`, with the arguments passed through, and the exit
 *  status reported, because a shell that starts a program and cannot tell you
 *  what it did is only half a shell.
 * ===========================================================================*/

#define RUN_TIMEOUT_MS 60000

int cmd_run(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: run PATH [args...]\n"
                "  Start an ELF64 program in ring 3 and wait for it to finish.\n"
                "  A name with no '/' is looked for in /bin.\n"
                "  See SDK.md for building one.\n");
        return 1;
    }

    char path[192];
    if (strchr(argv[1], '/')) snprintf(path, sizeof path, "%s", argv[1]);
    else                      snprintf(path, sizeof path, "/bin/%s", argv[1]);

    fs_node *f = fs_lookup(path);
    if (!f || f->is_dir) { kprintf("run: %s: not found\n", path); return 1; }
    if (!fs_may(f, FS_PERM_READ)) {
        kprintf("run: %s: permission denied\n", path);
        return 1;
    }
    if (!f->data || f->size < 4 || f->data[0] != 0x7F || f->data[1] != 'E' ||
        f->data[2] != 'L' || f->data[3] != 'F') {
        kprintf("run: %s is not an ELF program\n", path);
        return 1;
    }

    if (!fs_may(f, FS_PERM_EXEC)) {
        kprintf("run: %s: permission denied (use chmod + execute bits)\n", path);
        return 126;
    }
    if (argc - 1 > PROC_MAX_ARGV) {
        kprintf("run: too many arguments (maximum %d including program)\n", PROC_MAX_ARGV);
        return 2;
    }

    /* argv[0] is the program, as the ABI says; the rest is passed through. */
    const char *av[PROC_MAX_ARGV];
    int n = 0;
    av[n++] = path;
    for (int i = 2; i < argc && n < PROC_MAX_ARGV; i++) av[n++] = argv[i];

    int pid = proc_exec_vec(path, n, av);
    if (pid < 0) { kprintf("run: %s: could not start\n", path); return 1; }

    int code = 0;
    if (proc_wait(pid, &code, RUN_TIMEOUT_MS) != 0) {
        kprintf("run: %s (pid %d) is still running after %d seconds\n",
                path, pid, RUN_TIMEOUT_MS / 1000);
        return 1;
    }
    kprintf("run: %s exited %d\n", path, code);
    return code;
}
