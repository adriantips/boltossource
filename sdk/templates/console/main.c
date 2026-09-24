/* @NAME@ -- a BoltOS program.
 *
 * Built outside BoltOS and carried in, which is the only path a program that
 * did not ship with the kernel can take. `bolt build` compiles it, `bolt ship`
 * serves it, and `run @NAME@` starts it over there.
 *
 * The whole ABI is include/syscall.h and SDK.md explains it. The short version:
 * you get files, processes, sockets, the keyboard and the framebuffer, and no
 * dynamic loader -- this file plus whatever you add to `sources` in pkg.txt is
 * the entire program.
 */
#include "ulibc.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

int main(int argc, char **argv) {
    /* Ask the kernel what ABI it speaks before assuming it is this one. A
     * mismatch is worth saying out loud -- the alternative is finding out when
     * some syscall returns -1 for no visible reason. */
    if (bolt_abi() != BOLT_ABI_VERSION) {
        printf("@NAME@: built for BoltOS ABI %d, this kernel speaks %d\n",
               BOLT_ABI_VERSION, bolt_abi());
        return 1;
    }

    printf("@NAME@: running as pid %d\n", getpid());

    for (int i = 1; i < argc; i++)
        printf("  argv[%d] = %s\n", i, argv[i]);

    /* The filesystem, through the same syscalls everything else uses. */
    int fd = open("/tmp/@NAME@.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        const char *msg = "@NAME@ was here\n";
        write(fd, msg, strlen(msg));
        close(fd);
    }

    /* Whatever you return, the shell sees as $?. `run` prints it too. */
    return 0;
}
