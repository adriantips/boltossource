/* ===========================================================================
 *  BoltOS Mini  -  mini/tools/lxtest.c
 *
 *  A Linux program that exercises Mini's Linux layer (mini/lx*.c): built
 *  for aarch64 musl twice, static-pie and dynamic, packed with
 *  mini/tools/mklxpak.py, and run with GET /lx?run=/test/lxtest. It prints
 *  one line per check and "lxtest: N ok, M failed" at the end.
 *
 *      gcc -O2 -static-pie -o lxtest-static lxtest.c   (Alpine, aarch64)
 *      gcc -O2 -o lxtest lxtest.c
 *
 *  Argument "crash" ends it with a NULL write, to see Mini survive that;
 *  "net HOST" looks HOST up (musl's resolver: UDP to /etc/resolv.conf's
 *  server) and asks an NTP server the time over UDP.
 * ===========================================================================*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/random.h>
#include <dlfcn.h>
#include <math.h>
#include <setjmp.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int ok, bad;
#define CHECK(cond, ...) do { if (cond) { ok++; printf("ok   "); } else { bad++; printf("FAIL "); } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

int main(int argc, char **argv) {
    printf("lxtest: argc %d, argv[0] %s, HOME=%s\n", argc, argv[0], getenv("HOME") ? getenv("HOME") : "(none)");
    if (argc > 1 && !strcmp(argv[1], "crash")) { printf("writing to NULL\n"); fflush(stdout); *(volatile int *)0 = 1; }
    if (argc > 1 && !strcmp(argv[1], "spin")) { for (;;) { } }
    if (argc > 2 && !strcmp(argv[1], "net")) {
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *ai = 0;
        int r = getaddrinfo(argv[2], "123", &hints, &ai);
        char ip[32] = "?";
        if (!r) inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, ip, sizeof ip);
        CHECK(r == 0, "getaddrinfo %s: %s", argv[2], r ? gai_strerror(r) : ip);
        if (!r) {
            int s = socket(AF_INET, SOCK_DGRAM, 0);
            unsigned char q[48] = { 0x1B }, a[48];
            sendto(s, q, sizeof q, 0, ai->ai_addr, ai->ai_addrlen);
            struct pollfd pf = { s, POLLIN, 0 };
            int ready = poll(&pf, 1, 3000);
            struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t n = ready > 0 ? recvfrom(s, a, sizeof a, 0, (struct sockaddr *)&from, &fl) : -1;
            unsigned long secs = n == 48 ? ((unsigned long)a[40] << 24 | a[41] << 16 | a[42] << 8 | a[43]) - 2208988800ul : 0;
            CHECK(n == 48, "NTP over UDP: %zd bytes, unix time %lu", n, secs);
            close(s);
        }
        printf("lxtest: %d ok, %d failed\n", ok, bad);
        return bad;
    }

    struct utsname u;
    CHECK(uname(&u) == 0, "uname: %s %s %s", u.sysname, u.release, u.machine);

    char *small = malloc(100), *big = malloc(64 << 20);
    CHECK(small && big, "malloc 100 B and 64 MiB");
    if (big) { memset(big, 0x5A, 64 << 20); CHECK(big[(64 << 20) - 1] == 0x5A, "64 MiB written"); free(big); }
    free(small);
    void *m = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED && ((char *)m)[12345] == 0, "anonymous mmap is zeroed");
    if (m != MAP_FAILED) { void *r = mremap(m, 1 << 20, 4 << 20, MREMAP_MAYMOVE); CHECK(r != MAP_FAILED, "mremap"); munmap(r, 4 << 20); }

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    struct timespec ten = { 0, 10 * 1000 * 1000 };
    nanosleep(&ten, 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    CHECK(ms >= 9 && ms < 100, "nanosleep 10 ms took %ld ms", ms);
    struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt);
    CHECK(rt.tv_sec > 0, "realtime %ld", (long)rt.tv_sec);

    unsigned char rnd[16] = { 0 };
    CHECK(getrandom(rnd, sizeof rnd, 0) == 16, "getrandom %02x%02x%02x%02x", rnd[0], rnd[1], rnd[2], rnd[3]);

    char cwd[256];
    CHECK(getcwd(cwd, sizeof cwd) != 0, "getcwd %s", cwd);
    char exe[256] = { 0 };
    ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1);
    CHECK(el > 0, "/proc/self/exe -> %s", exe);

    FILE *f = fopen("/etc/resolv.conf", "r");
    char line[128] = { 0 };
    if (f) { fgets(line, sizeof line, f); fclose(f); line[strcspn(line, "\n")] = 0; }
    CHECK(f != 0, "/etc/resolv.conf: %s", line);

    DIR *d = opendir("/");
    int n = 0; char names[200] = { 0 };
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) { n++; if (strlen(names) + strlen(e->d_name) + 2 < sizeof names) { strcat(names, e->d_name); strcat(names, " "); } }
        closedir(d);
    }
    CHECK(n >= 3, "ls /: %s", names);

    struct stat st;
    CHECK(stat(argv[0], &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 1000, "stat %s: %ld bytes", argv[0], (long)st.st_size);
    CHECK(stat("/no/such/file", &st) < 0 && errno == ENOENT, "stat of a missing file: ENOENT");

    /* A file of its own: written, read back, appended, renamed, removed. */
    mkdir("/tmp", 0755);
    int fd = open("/tmp/lxtest.txt", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    CHECK(fd >= 0, "create /tmp/lxtest.txt");
    if (fd >= 0) { write(fd, "hello ", 6); close(fd); }
    fd = open("/tmp/lxtest.txt", O_WRONLY | O_APPEND);
    if (fd >= 0) { write(fd, "mini\n", 5); close(fd); }
    f = fopen("/tmp/lxtest.txt", "r");
    line[0] = 0;
    if (f) { fgets(line, sizeof line, f); fclose(f); }
    CHECK(!strcmp(line, "hello mini\n"), "read back: %.20s", line);
    CHECK(rename("/tmp/lxtest.txt", "/tmp/lxtest2.txt") == 0, "rename");
    CHECK(unlink("/tmp/lxtest2.txt") == 0 && access("/tmp/lxtest2.txt", F_OK) < 0, "unlink");

    /* A file mapped: the program's own first bytes are its ELF header. */
    fd = open(argv[0], O_RDONLY);
    void *map = fd >= 0 ? mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    CHECK(map != MAP_FAILED && !memcmp(map, "\x7f" "ELF", 4), "mmap of a file");
    if (fd >= 0) close(fd);

    volatile double x = 2.0;
    CHECK(fabs(sqrt(x) - 1.41421356) < 1e-6 && fabs(sin(x) - 0.909297) < 1e-5, "floating point");

    jmp_buf j;
    volatile int jumped = 0;
    if (!setjmp(j)) { jumped = 1; longjmp(j, 1); }
    CHECK(jumped == 1, "setjmp/longjmp");

    void *h = dlopen("libgcc_s.so.1", RTLD_NOW);
    CHECK(h != 0 || argv[0][strlen(argv[0]) - 1] == 'c', "dlopen libgcc_s: %s", h ? "loaded" : dlerror());

    printf("lxtest: %d ok, %d failed\n", ok, bad);
    return bad ? 1 : 0;
}
