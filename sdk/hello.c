/* sdk/hello.c -- a third-party BoltOS program.
 *
 * Nothing in here is privileged and nothing in here is built into the kernel.
 * It is compiled outside BoltOS with sdk/build.sh, carried onto a machine by
 * whatever means (`download`, `pkg install`, a copy into /bin) and started
 * with `run` -- the path SDK.md describes, which is the path any program that
 * did not ship with the OS has to take.
 *
 *     sdk/build.sh -o build/sdkhello sdk/hello.c
 */
#include "ulibc.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "math.h"

int main(int argc, char **argv) {
    /* The ABI check the SDK asks for: say so plainly rather than failing in
     * whatever way an unimplemented syscall happens to fail. */
    int abi = bolt_abi();
    printf("sdkhello: BoltOS ABI %d (built against %d)\n", abi, BOLT_ABI_VERSION);
    if (abi != BOLT_ABI_VERSION) {
        printf("sdkhello: this kernel is not the one I was built for\n");
        return 2;
    }

    printf("sdkhello: pid %d, argc %d\n", getpid(), argc);
    for (int i = 0; i < argc; i++) printf("  argv[%d] = %s\n", i, argv[i]);

    /* The filesystem, through the same syscalls any program gets. */
    const char *path = "/tmp/sdkhello.txt";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { printf("sdkhello: cannot write %s\n", path); return 3; }
    const char *msg = "written by a program the kernel never heard of\n";
    write(fd, msg, strlen(msg));
    close(fd);

    char back[128];
    fd = open(path, O_RDONLY);
    if (fd < 0) { printf("sdkhello: cannot read %s back\n", path); return 4; }
    long n = read(fd, back, sizeof back - 1);
    close(fd);
    if (n <= 0) { printf("sdkhello: read nothing back\n"); return 5; }
    back[n] = 0;
    printf("sdkhello: read back %ld bytes: %s", n, back);

    /* Real floating point, because SSE is on and the register file is saved
     * across context switches. */
    double r = 2.0;
    printf("sdkhello: sqrt(2) = %f\n", sqrt(r));

    /* An exit status the shell reports, so a script can act on it. */
    int code = (argc > 1) ? atoi(argv[1]) : 0;
    printf("sdkhello: exiting %d\n", code);
    return code;
}
