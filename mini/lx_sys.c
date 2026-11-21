/* ===========================================================================
 *  BoltOS Mini  -  mini/lx_sys.c
 *
 *  Linux's arm64 system calls, as a Linux program on a spare core sees them
 *  (lx.h): number in x8, arguments in x0..x5, the answer -- or -errno -- in
 *  x0. One thread, one process: clone says EAGAIN (musl's pthread_create
 *  passes that on, and programs that can do without a thread do), signals
 *  are remembered and never delivered, and a call this file doesn't know
 *  says ENOSYS once in the log ("[lx] syscall 123 isn't here").
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "lxint.h"
#include "string.h"
#include "stdio.h"

uint64_t lx_brk_lo, lx_brk_cur, lx_brk_hi;
static uint8_t  told[64];                      /* unknown syscalls already logged */
static uint64_t sigmask;
static uint8_t  sigact[65][32];
static uint64_t t_start;
#define BRK_SIZE (32u << 20)
#define PID 42

void lxsys_reset(void) {
    lx_brk_lo = lx_brk_cur = lx_brk_hi = 0;
    memset(told, 0, sizeof told);
    sigmask = 0;
    memset(sigact, 0, sizeof sigact);
    t_start = 0;
}

static void once(int nr, const char *what) {
    if (nr < 0 || nr >= 512 || (told[nr / 8] >> (nr % 8) & 1)) return;
    told[nr / 8] |= (uint8_t)(1 << (nr % 8));
    lx_log("syscall %d %s", nr, what);
}

#define UOK(p, n) do { if (!lx_uok((uint64_t)(p), (uint64_t)(n))) return -L_EFAULT; } while (0)
#define U8(p) ((uint8_t *)(uintptr_t)(p))

/* ---- descriptors ---------------------------------------------------------------------------- */
static lxfd_t *fdp(int64_t fd) {
    if (fd < 0 || fd >= LX_MAXFD || lx_fd[fd].kind == FD_NONE) return 0;
    return &lx_fd[fd];
}
static int fd_alloc(int from) {
    for (int i = from < 0 ? 0 : from; i < LX_MAXFD; i++) if (lx_fd[i].kind == FD_NONE) return i;
    return -L_EMFILE;
}
static int fd_close(int fd) {
    lxfd_t *f = fdp(fd);
    if (!f) return -L_EBADF;
    if (f->kind == FD_SOCK) { lxnet_close(f->sock); memset(f, 0, sizeof *f); return 0; }
    return lxfs_close(f);
}
static int is_file(const lxfd_t *f) { return f->kind == FD_PAK || f->kind == FD_MEM || f->kind == FD_SYN || f->kind == FD_DIR; }

static int64_t do_read(lxfd_t *f, void *buf, uint64_t n) {
    if (is_file(f)) {
        if ((f->flags & L_O_ACCMODE) == L_O_WRONLY) return -L_EBADF;
        int64_t r = lxfs_read(f, buf, n, f->pos);
        if (r > 0) f->pos += (uint64_t)r;
        return r;
    }
    if (f->kind == FD_SOCK) return lxnet_recvfrom(f->sock, buf, n, 0, 0, (f->flags & L_O_NONBLOCK) != 0, 0);
    return lxdev_read(f, buf, n);
}

static int64_t do_write(lxfd_t *f, const void *buf, uint64_t n) {
    if (is_file(f)) {
        if ((f->flags & L_O_ACCMODE) == 0 && !(f->flags & L_O_TRUNC)) return -L_EBADF;
        uint64_t at = (f->flags & L_O_APPEND) ? lxfs_size(f) : f->pos;
        int64_t r = lxfs_write(f, buf, n, at);
        if (r > 0) f->pos = at + (uint64_t)r;
        return r;
    }
    if (f->kind == FD_SOCK) return lxnet_sendto(f->sock, buf, n, 0, 0, (f->flags & L_O_NONBLOCK) != 0);
    return lxdev_write(f, buf, n);
}

static int64_t path_arg(int64_t dirfd, uint64_t up, char *out) {
    char in[256];
    int r = lx_ustr(up, in, sizeof in);
    if (r) return r;
    return lxfs_path((int)dirfd, in, out);
}

/* ---- memory ------------------------------------------------------------------------------------- */
#define PG(x) (((x) + 4095) & ~4095ull)

static int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, int64_t fd, uint64_t off) {
    if (!len) return -L_EINVAL;
    len = PG(len);
    lxfd_t *f = 0;
    if (!(flags & 0x20)) {                                   /* not MAP_ANONYMOUS: a file */
        f = fdp(fd);
        if (!f) return -L_EBADF;
        if (f->kind == FD_FB) { uint64_t l; return (int64_t)lxdev_fb_addr(&l); }
        if (!is_file(f) || f->kind == FD_DIR) return -L_EACCES;
        if (off & 4095) return -L_EINVAL;
    }
    uint64_t p;
    if (flags & (0x10 | 0x100000)) {                         /* MAP_FIXED, MAP_FIXED_NOREPLACE */
        if (addr & 4095 || !lx_uok(addr, len)) return -L_ENOMEM;
        if (flags & 0x100000) { if (lx_pages_at(addr, len)) return -L_EEXIST; }
        else if (lx_pages_force(addr, len)) return -L_ENOMEM;
        memset((void *)(uintptr_t)addr, 0, len);
        p = addr;
    } else {
        void *v = lx_pages(len);
        if (!v) { lx_log("mmap of %lu KiB: out of memory (%lu MiB free)", (unsigned long)(len >> 10), (unsigned long)(lx_mem_free() >> 20)); return -L_ENOMEM; }
        p = (uint64_t)(uintptr_t)v;
    }
    if (f) {
        uint64_t size = lxfs_size(f);
        if (off < size) {
            uint64_t k = size - off < len ? size - off : len;
            int64_t got = lxfs_read(f, (void *)(uintptr_t)p, k, off);
            if (got < 0) { if (!(flags & 0x10)) lx_unpages(p, len); return got; }
        }
        if (prot & 4) lxfs_note_map(f->path, p - off, len + off);
    }
    if (prot & 4) lx_sync_code(p, len);
    return (int64_t)p;
}

static int64_t sys_mremap(uint64_t old, uint64_t olen, uint64_t nlen, uint64_t flags) {
    olen = PG(olen); nlen = PG(nlen);
    if (old & 4095 || !nlen) return -L_EINVAL;
    if (nlen <= olen) { if (nlen < olen) lx_unpages(old + nlen, olen - nlen); return (int64_t)old; }
    if (lx_uok(old + olen, nlen - olen) && lx_pages_at(old + olen, nlen - olen) == 0) return (int64_t)old;
    if (!(flags & 1)) return -L_ENOMEM;
    void *v = lx_pages(nlen);
    if (!v) return -L_ENOMEM;
    memcpy(v, (void *)(uintptr_t)old, olen);
    lx_unpages(old, olen);
    return (int64_t)(uintptr_t)v;
}

static int64_t sys_brk(uint64_t want) {
    if (!lx_brk_lo) {
        void *v = lx_pages(BRK_SIZE);
        if (!v) return 0;
        lx_brk_lo = lx_brk_cur = (uint64_t)(uintptr_t)v;
        lx_brk_hi = lx_brk_lo + BRK_SIZE;
    }
    if (want >= lx_brk_lo && want <= lx_brk_hi) {
        if (want < lx_brk_cur) memset((void *)(uintptr_t)want, 0, lx_brk_cur - want);
        lx_brk_cur = want;
    }
    return (int64_t)lx_brk_cur;
}

/* ---- time --------------------------------------------------------------------------------------- */
static uint64_t clock_ns(int clk) {
    if (clk == 0 || clk == 5 || clk == 8) return lx_real_ns();       /* REALTIME, _COARSE, _ALARM */
    if (clk == 2 || clk == 3) return lx_now_ns() - t_start;          /* the process's and thread's CPU */
    return lx_now_ns();
}
static void put_ts(uint64_t p, uint64_t ns) {
    int64_t t[2] = { (int64_t)(ns / 1000000000ull), (int64_t)(ns % 1000000000ull) };
    memcpy(U8(p), t, 16);
}
static int64_t get_ts(uint64_t p, uint64_t *ns) {
    int64_t t[2];
    UOK(p, 16);
    memcpy(t, U8(p), 16);
    if (t[0] < 0 || t[1] < 0 || t[1] >= 1000000000) return -L_EINVAL;
    *ns = (uint64_t)t[0] * 1000000000ull + (uint64_t)t[1];
    return 0;
}

/* ---- waiting on several descriptors ---------------------------------------------------------------- */
static int readable(lxfd_t *f) {
    if (!f) return 0;
    if (f->kind == FD_SOCK) return lxnet_ready(f->sock);
    if (is_file(f)) return 1;
    return lxdev_ready(f);
}

static int writable(lxfd_t *f) { return f->kind != FD_SOCK || lxnet_writable(f->sock); }

static int64_t sys_ppoll(uint64_t fds, uint64_t n, uint64_t tsp) {
    if (n > LX_MAXFD) return -L_EINVAL;
    UOK(fds, n * 8);
    uint64_t end = ~0ull;
    if (tsp) { uint64_t ns; int64_t r = get_ts(tsp, &ns); if (r) return r; end = lx_now_ns() + ns; }
    for (;;) {
        int ready = 0;
        for (uint64_t i = 0; i < n; i++) {
            uint8_t *p = U8(fds + i * 8);
            int32_t fd; int16_t ev, re = 0;
            memcpy(&fd, p, 4); memcpy(&ev, p + 4, 2);
            if (fd >= 0) {
                lxfd_t *f = fdp(fd);
                if (!f) re = 0x20;                                           /* POLLNVAL */
                else {
                    if ((ev & 1) && readable(f)) re |= 1;                     /* POLLIN   */
                    if ((ev & 4) && writable(f)) re |= 4;                     /* POLLOUT  */
                }
            }
            memcpy(p + 6, &re, 2);
            if (re) ready++;
        }
        if (ready) return ready;
        if (lx_now_ns() >= end) return 0;
        if (lx_stopping()) return -L_EINTR;
        __asm__ volatile("wfe");
    }
}

static int64_t sys_pselect(int64_t nfds, uint64_t rd, uint64_t wr, uint64_t ex, uint64_t tsp) {
    if (nfds < 0 || nfds > 1024) return -L_EINVAL;
    uint64_t bytes = (uint64_t)(nfds + 63) / 64 * 8;
    uint64_t want_r[16] = { 0 }, want_w[16] = { 0 };
    if (rd) { UOK(rd, bytes); memcpy(want_r, U8(rd), bytes); }
    if (wr) { UOK(wr, bytes); memcpy(want_w, U8(wr), bytes); }
    if (ex) { UOK(ex, bytes); memset(U8(ex), 0, bytes); }
    uint64_t end = ~0ull;
    if (tsp) { uint64_t ns; int64_t r = get_ts(tsp, &ns); if (r) return r; end = lx_now_ns() + ns; }
    for (;;) {
        uint64_t got_r[16] = { 0 }, got_w[16] = { 0 };
        int ready = 0;
        for (int fd = 0; fd < nfds; fd++) {
            int wr_ = want_w[fd / 64] >> (fd % 64) & 1, rd_ = want_r[fd / 64] >> (fd % 64) & 1;
            if (!wr_ && !rd_) continue;
            lxfd_t *f = fdp(fd);
            if (!f) return -L_EBADF;
            if (rd_ && readable(f)) { got_r[fd / 64] |= 1ull << (fd % 64); ready++; }
            if (wr_ && writable(f)) { got_w[fd / 64] |= 1ull << (fd % 64); ready++; }
        }
        if (ready || lx_now_ns() >= end || lx_stopping()) {
            if (rd) memcpy(U8(rd), got_r, bytes);
            if (wr) memcpy(U8(wr), got_w, bytes);
            if (!ready && lx_stopping()) return -L_EINTR;
            return ready;
        }
        __asm__ volatile("wfe");
    }
}

/* ---- the rest ------------------------------------------------------------------------------------- */
static int64_t sys_uname(uint64_t p) {
    UOK(p, 390);
    static const char *const f[6] = { "Linux", "boltos-mini", "6.1.0-boltos", "#1 SMP BoltOS Mini", "aarch64", "(none)" };
    memset(U8(p), 0, 390);
    for (int i = 0; i < 6; i++) memcpy(U8(p) + 65 * i, f[i], strlen(f[i]));
    return 0;
}

static int64_t sys_statx(int64_t dirfd, uint64_t up, int64_t flags, uint64_t out) {
    uint8_t st[128];
    char path[256];
    char in[256];
    UOK(out, 256);
    int r = lx_ustr(up, in, sizeof in);
    if (r) return r;
    if (!in[0] && (flags & L_AT_EMPTY_PATH)) {
        lxfd_t *f = dirfd == L_AT_FDCWD ? 0 : fdp(dirfd);
        if (f) r = lxfs_fstat(f, st);
        else { r = lxfs_stat(lxfs_cwd(), st); }
    } else {
        r = lxfs_path((int)dirfd, in, path);
        if (!r) r = lxfs_stat(path, st);
    }
    if (r) return r;
    uint8_t *x = U8(out);
    memset(x, 0, 256);
    uint32_t mask = 0x7FF, blk = 4096, nlink, uid = 0, mode;
    uint64_t ino, size, blocks, t;
    memcpy(&ino, st + 8, 8); memcpy(&mode, st + 16, 4); memcpy(&nlink, st + 20, 4);
    memcpy(&size, st + 48, 8); memcpy(&blocks, st + 64, 8); memcpy(&t, st + 88, 8);
    uint16_t m16 = (uint16_t)mode;
    memcpy(x, &mask, 4); memcpy(x + 4, &blk, 4); memcpy(x + 16, &nlink, 4); memcpy(x + 20, &uid, 4); memcpy(x + 24, &uid, 4);
    memcpy(x + 28, &m16, 2); memcpy(x + 32, &ino, 8); memcpy(x + 40, &size, 8); memcpy(x + 48, &blocks, 8);
    memcpy(x + 64, &t, 8); memcpy(x + 80, &t, 8); memcpy(x + 96, &t, 8); memcpy(x + 112, &t, 8);
    uint32_t dev_major = 8, dev_minor = 1;
    memcpy(x + 136, &dev_major, 4); memcpy(x + 140, &dev_minor, 4);
    return 0;
}

static int64_t sys_prlimit(int64_t res, uint64_t nw, uint64_t old) {
    (void)nw;
    if (!old) return 0;
    UOK(old, 16);
    uint64_t v[2] = { ~0ull, ~0ull };
    if (res == 3) { v[0] = 8u << 20; }                        /* RLIMIT_STACK  */
    if (res == 7) { v[0] = v[1] = LX_MAXFD; }                 /* RLIMIT_NOFILE */
    if (res == 4) { v[0] = 0; }                               /* RLIMIT_CORE   */
    memcpy(U8(old), v, 16);
    return 0;
}

static int64_t sys_kill(int64_t sig) {
    if (sig == 0) return 0;
    static const int fatal[] = { 3, 4, 5, 6, 7, 8, 9, 11, 15, 0 };
    for (int i = 0; fatal[i]; i++)
        if (sig == fatal[i]) {
            lx_log("the program sent itself signal %d%s", (int)sig, sig == 6 ? " (abort)" : "");
            lx_exit(128 + (int)sig);
        }
    return 0;
}

static int64_t sys_futex(uint64_t uaddr, int64_t op, uint64_t val, uint64_t tsp) {
    int cmd = (int)(op & 0x7F);
    if (cmd == 0 || cmd == 9) {                               /* WAIT, WAIT_BITSET */
        UOK(uaddr, 4);
        if (*(volatile uint32_t *)(uintptr_t)uaddr != (uint32_t)val) return -L_EAGAIN;
        if (tsp) {
            uint64_t ns;
            if (get_ts(tsp, &ns)) return -L_EINVAL;
            lx_wait_until(cmd == 9 ? ns : lx_now_ns() + ns);
            return -L_ETIMEDOUT;
        }
        once(98, "waits with nobody to wake it (one thread)");
        return -L_EAGAIN;
    }
    return 0;                                                 /* WAKE, REQUEUE: nobody else */
}

int64_t lx_syscall(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    if (!t_start) t_start = lx_now_ns();
    lxfd_t *f;
    char path[256], path2[256];
    int64_t r;
    switch (nr) {
    /* ---- files ---- */
    case 56: {                                                /* openat */
        if ((r = path_arg((int64_t)a0, a1, path))) return r;
        int fd = fd_alloc(0);
        if (fd < 0) return fd;
        r = lxfs_open(path, (int)a2, (int)a3, &lx_fd[fd]);
        if (r) { memset(&lx_fd[fd], 0, sizeof lx_fd[fd]); return r; }
        return fd;
    }
    case 57: return fd_close((int)a0);                        /* close */
    case 436:                                                 /* close_range */
        for (uint64_t i = a0; i <= a1 && i < LX_MAXFD; i++) if (fdp((int64_t)i)) fd_close((int)i);
        return 0;
    case 63:                                                  /* read */
        if (!(f = fdp((int64_t)a0))) return -L_EBADF;
        UOK(a1, a2);
        return do_read(f, U8(a1), a2);
    case 64:                                                  /* write */
        if (!(f = fdp((int64_t)a0))) return -L_EBADF;
        UOK(a1, a2);
        return do_write(f, U8(a1), a2);
    case 65: case 66: {                                       /* readv, writev */
        if (!(f = fdp((int64_t)a0))) return -L_EBADF;
        if (a2 > 1024) return -L_EINVAL;
        UOK(a1, a2 * 16);
        int64_t total = 0;
        for (uint64_t i = 0; i < a2; i++) {
            uint64_t base, len;
            memcpy(&base, U8(a1 + i * 16), 8); memcpy(&len, U8(a1 + i * 16 + 8), 8);
            if (!len) continue;
            UOK(base, len);
            r = nr == 65 ? do_read(f, U8(base), len) : do_write(f, U8(base), len);
            if (r < 0) return total ? total : r;
            total += r;
            if ((uint64_t)r < len) break;
        }
        return total;
    }
    case 67: case 68:                                         /* pread64, pwrite64 */
        if (!(f = fdp((int64_t)a0))) return -L_EBADF;
        if (!is_file(f)) return -L_ESPIPE;
        UOK(a1, a2);
        return nr == 67 ? lxfs_read(f, U8(a1), a2, a3) : lxfs_write(f, U8(a1), a2, a3);
    case 62: {                                                /* lseek */
        if (!(f = fdp((int64_t)a0))) return -L_EBADF;
        if (!is_file(f) && f->kind != FD_FB) return -L_ESPIPE;
        int64_t base = a2 == 0 ? 0 : a2 == 1 ? (int64_t)f->pos : a2 == 2 ? (int64_t)lxfs_size(f) : -1;
        if (base < 0) return -L_EINVAL;
        int64_t np = base + (int64_t)a1;
        if (np < 0) return -L_EINVAL;
        f->pos = (uint64_t)np;
        return np;
    }
    case 80:                                                  /* fstat */
        if (!(f = fdp((int64_t)a0))) return -L_EBADF;
        UOK(a1, 128);
        return lxfs_fstat(f, U8(a1));
    case 79: {                                                /* newfstatat */
        char in[256];
        UOK(a2, 128);
        if ((r = lx_ustr(a1, in, sizeof in))) return r;
        if (!in[0] && (a3 & L_AT_EMPTY_PATH)) {
            if ((int64_t)a0 == L_AT_FDCWD) return lxfs_stat(lxfs_cwd(), U8(a2));
            if (!(f = fdp((int64_t)a0))) return -L_EBADF;
            return lxfs_fstat(f, U8(a2));
        }
        if ((r = lxfs_path((int)a0, in, path))) return r;
        return lxfs_stat(path, U8(a2));
    }
    case 291: return sys_statx((int64_t)a0, a1, (int64_t)a2, a4);
    case 61:                                                  /* getdents64 */
        if (!(f = fdp((int64_t)a0))) return -L_EBADF;
        UOK(a1, a2);
        return lxfs_getdents(f, U8(a1), a2);
    case 17: {                                                /* getcwd */
        const char *c = lxfs_cwd();
        uint64_t n = strlen(c) + 1;
        if (a1 < n) return -L_ERANGE;
        UOK(a0, n);
        memcpy(U8(a0), c, n);
        return (int64_t)n;
    }
    case 49:                                                  /* chdir */
        if ((r = path_arg(L_AT_FDCWD, a0, path))) return r;
        return lxfs_chdir(path);
    case 50:                                                  /* fchdir */
        if (!(f = fdp((int64_t)a0))) return -L_EBADF;
        return lxfs_chdir(f->path);
    case 34:                                                  /* mkdirat */
        if ((r = path_arg((int64_t)a0, a1, path))) return r;
        return lxfs_mkdir(path);
    case 35:                                                  /* unlinkat */
        if ((r = path_arg((int64_t)a0, a1, path))) return r;
        return lxfs_unlink(path, (a2 & L_AT_REMOVEDIR) != 0);
    case 38: case 276:                                        /* renameat, renameat2 */
        if ((r = path_arg((int64_t)a0, a1, path))) return r;
        if ((r = path_arg((int64_t)a2, a3, path2))) return r;
        return lxfs_rename(path, path2);
    case 48: case 439:                                        /* faccessat, faccessat2 */
        if ((r = path_arg((int64_t)a0, a1, path))) return r;
        return lxfs_access(path);
    case 78: {                                                /* readlinkat */
        if ((r = path_arg((int64_t)a0, a1, path))) return r;
        UOK(a2, a3);
        return lxfs_readlink(path, (char *)U8(a2), (int)a3);
    }
    case 46:                                                  /* ftruncate */
        if (!(f = fdp((int64_t)a0))) return -L_EBADF;
        return lxfs_truncate(f, a1);
    case 45: {                                                /* truncate */
        if ((r = path_arg(L_AT_FDCWD, a0, path))) return r;
        lxfd_t t;
        if ((r = lxfs_open(path, L_O_WRONLY, 0, &t))) return r;
        r = lxfs_truncate(&t, a1);
        lxfs_close(&t);
        return r;
    }
    case 82: case 83:                                         /* fsync, fdatasync */
        if (!(f = fdp((int64_t)a0))) return -L_EBADF;
        return lxfs_sync(f);
    case 81: lxfs_flush_all(); return 0;                      /* sync */
    case 23: case 24: case 25: {                              /* dup, dup3, fcntl */
        if (!(f = fdp((int64_t)a0))) return -L_EBADF;
        int to;
        if (nr == 25) {
            switch (a1) {
            case 1: return f->cloexec;                        /* F_GETFD */
            case 2: f->cloexec = a2 & 1; return 0;            /* F_SETFD */
            case 3: return f->flags;                          /* F_GETFL */
            case 4:
                f->flags = (f->flags & ~(uint32_t)(L_O_NONBLOCK | L_O_APPEND)) | ((uint32_t)a2 & (L_O_NONBLOCK | L_O_APPEND));
                if (f->kind == FD_SOCK) lxnet_set_nonblock(f->sock, (f->flags & L_O_NONBLOCK) != 0);
                return 0;
            case 5: case 6: case 7: return 0;                 /* locks: there's only us */
            case 0: case 1030: break;                         /* F_DUPFD(_CLOEXEC) */
            default: return -L_EINVAL;
            }
            to = fd_alloc((int)a2);
        } else if (nr == 23) to = fd_alloc(0);
        else {
            to = (int)a1;
            if (to == (int)a0) return -L_EINVAL;
            if (to < 0 || to >= LX_MAXFD) return -L_EBADF;
            if (fdp(to)) fd_close(to);
        }
        if (to < 0) return to;
        if (f->kind == FD_SOCK) return -L_EMFILE;              /* a socket stays one descriptor */
        lxfs_dup(f, &lx_fd[to]);
        lx_fd[to].cloexec = (nr == 25 && a1 == 1030) || (nr == 24 && (a2 & L_O_CLOEXEC));
        return to;
    }
    case 29:                                                  /* ioctl: the request is an int (musl sign-extends it) */
        if (!(f = fdp((int64_t)a0))) return -L_EBADF;
        a1 &= 0xFFFFFFFFull;
        if (a1 == 0x5421) {                                   /* FIONBIO */
            UOK(a2, 4);
            int on; memcpy(&on, U8(a2), 4);
            if (on) f->flags |= L_O_NONBLOCK; else f->flags &= ~(uint32_t)L_O_NONBLOCK;
            if (f->kind == FD_SOCK) lxnet_set_nonblock(f->sock, on != 0);
            return 0;
        }
        if (a1 == 0x541B) {                                   /* FIONREAD */
            UOK(a2, 4);
            int n = f->kind == FD_SOCK ? (lxnet_ready(f->sock) ? 1472 : 0) : 0;
            memcpy(U8(a2), &n, 4);
            return 0;
        }
        if (is_file(f) || f->kind == FD_SOCK) return -L_ENOTTY;
        return lxdev_ioctl(f, a1, a2);
    case 59: return -L_EMFILE;                                /* pipe2 */
    case 73: return sys_ppoll(a0, a1, a2);
    case 72: return sys_pselect((int64_t)a0, a1, a2, a3, a4);
    case 43: case 44: {                                       /* statfs, fstatfs */
        UOK(a1, 120);
        uint64_t s[15] = { 0x4d44, 4096, 1u << 20, 1u << 19, 1u << 19, 1u << 16, 1u << 15, 0, 255, 4096, 0, 0, 0, 0, 0 };
        memcpy(U8(a1), s, 120);
        return 0;
    }
    case 88: case 52: case 53: case 54: case 55: case 223: case 47: case 32:
        return 0;                                             /* utimensat, chmod/chown, fadvise, fallocate, flock */
    case 36: case 37: return -L_EPERM;                        /* symlinkat, linkat */

    /* ---- memory ---- */
    case 222: return sys_mmap(a0, a1, a2, a3, (int64_t)a4, a5);
    case 215: {                                               /* munmap */
        uint64_t fbl, fba = lxdev_fb_addr(&fbl);
        if (a0 & 4095) return -L_EINVAL;
        if (fba && a0 >= fba && a0 < fba + fbl) return 0;
        lx_unpages(a0, PG(a1));
        return 0;
    }
    case 216: return sys_mremap(a0, a1, a2, a3);
    case 214: return sys_brk(a0);
    case 226: return 0;                                       /* mprotect: every page is RWX */
    case 233:                                                 /* madvise */
        if (a2 == 4 && lx_uok(a0, a1)) memset(U8(a0), 0, PG(a1));   /* MADV_DONTNEED reads back zeros */
        return 0;
    case 227: case 228: case 229: case 230: case 231: return 0;   /* msync, mlock... */
    case 283: return 0;                                       /* membarrier: one core */

    /* ---- time ---- */
    case 113:                                                 /* clock_gettime */
        UOK(a1, 16);
        put_ts(a1, clock_ns((int)a0));
        return 0;
    case 114:                                                 /* clock_getres */
        if (a1) { UOK(a1, 16); put_ts(a1, 1); }
        return 0;
    case 169: {                                               /* gettimeofday */
        if (a0) {
            UOK(a0, 16);
            uint64_t ns = lx_real_ns();
            int64_t tv[2] = { (int64_t)(ns / 1000000000ull), (int64_t)(ns % 1000000000ull / 1000) };
            memcpy(U8(a0), tv, 16);
        }
        return 0;
    }
    case 101: case 115: {                                     /* nanosleep, clock_nanosleep */
        uint64_t ns, clk = nr == 115 ? a0 : 1, fl = nr == 115 ? a1 : 0, req = nr == 115 ? a2 : a0;
        if ((r = get_ts(req, &ns))) return r;
        uint64_t end = (fl & 1) ? (clk == 0 ? lx_now_ns() + (ns > lx_real_ns() ? ns - lx_real_ns() : 0) : ns) : lx_now_ns() + ns;
        lx_wait_until(end);
        return lx_stopping() ? -L_EINTR : 0;
    }
    case 153: {                                               /* times */
        uint64_t t = (lx_now_ns() - t_start) / 10000000ull;
        if (a0) { UOK(a0, 32); uint64_t v[4] = { t, 0, 0, 0 }; memcpy(U8(a0), v, 32); }
        return (int64_t)(lx_now_ns() / 10000000ull);
    }
    case 165: {                                               /* getrusage */
        UOK(a1, 144);
        memset(U8(a1), 0, 144);
        uint64_t us = (lx_now_ns() - t_start) / 1000;
        int64_t tv[2] = { (int64_t)(us / 1000000), (int64_t)(us % 1000000) };
        memcpy(U8(a1), tv, 16);
        return 0;
    }
    case 102: case 103: return 0;                             /* getitimer, setitimer */
    case 107: return -L_ENOSYS;                               /* timer_create */

    /* ---- the process ---- */
    case 93: case 94: lx_exit((int)(a0 & 0xFF));               /* exit, exit_group */
    case 96: return PID;                                      /* set_tid_address */
    case 99: return 0;                                        /* set_robust_list */
    case 293: return -L_ENOSYS;                               /* rseq */
    case 172: case 178: case 155: case 157: return PID;      /* getpid, gettid, getpgid, setsid */
    case 173: return 1;                                       /* getppid */
    case 174: case 175: case 176: case 177: return 0;         /* uid, euid, gid, egid */
    case 158: return 0;                                       /* getgroups */
    case 160: return sys_uname(a0);
    case 278: {                                               /* getrandom */
        UOK(a0, a1);
        lxfd_t t = { .kind = FD_RANDOM };
        return lxdev_read(&t, U8(a0), a1);
    }
    case 261: return sys_prlimit((int64_t)a1, a2, a3);
    case 163: return sys_prlimit((int64_t)a0, 0, a1);         /* getrlimit */
    case 164: return 0;                                       /* setrlimit */
    case 123:                                                 /* sched_getaffinity */
        if (a1 < 8) return -L_EINVAL;
        UOK(a2, 8);
        { uint64_t m = 1; memcpy(U8(a2), &m, 8); }
        return 8;
    case 122: case 124: case 118: case 119: case 120: case 121: case 125: case 126: case 140: case 141: case 92:
        return 0;                                             /* scheduling, priorities, personality */
    case 168:                                                 /* getcpu */
        if (a0) { UOK(a0, 4); memset(U8(a0), 0, 4); }
        if (a1) { UOK(a1, 4); memset(U8(a1), 0, 4); }
        return 0;
    case 179: {                                               /* sysinfo */
        UOK(a0, 112);
        uint8_t *s = U8(a0);
        memset(s, 0, 112);
        uint64_t up = now_ms() / 1000, tot = lx_mem_total(), fr = lx_mem_free();
        uint32_t unit = 1;
        uint16_t procs = 1;
        memcpy(s, &up, 8); memcpy(s + 32, &tot, 8); memcpy(s + 40, &fr, 8); memcpy(s + 80, &procs, 2); memcpy(s + 104, &unit, 4);
        return 0;
    }
    case 166: return 022;                                     /* umask */
    case 167:                                                 /* prctl */
        if (a0 == 16 && a1) {                                 /* PR_GET_NAME: the program's file name */
            UOK(a1, 16);
            const char *b = strrchr(lx_exe, '/');
            snprintf((char *)U8(a1), 16, "%s", b ? b + 1 : lx_exe);
        }
        return 0;
    case 134:                                                 /* rt_sigaction */
        if (a0 < 1 || a0 > 64) return -L_EINVAL;
        if (a2) { UOK(a2, 32); memcpy(U8(a2), sigact[a0], 32); }
        if (a1) { UOK(a1, 32); memcpy(sigact[a0], U8(a1), 32); }
        return 0;
    case 135:                                                 /* rt_sigprocmask */
        if (a2) { UOK(a2, 8); memcpy(U8(a2), &sigmask, 8); }
        if (a1) {
            uint64_t s; UOK(a1, 8); memcpy(&s, U8(a1), 8);
            if (a0 == 0) sigmask |= s; else if (a0 == 1) sigmask &= ~s; else sigmask = s;
        }
        return 0;
    case 132:                                                 /* sigaltstack */
        if (a1) { UOK(a1, 24); memset(U8(a1), 0, 24); int dis = 2; memcpy(U8(a1) + 8, &dis, 4); }
        return 0;
    case 136: if (a0) { UOK(a0, 8); memset(U8(a0), 0, 8); } return 0;   /* rt_sigpending */
    case 133: return -L_EINTR;                                /* rt_sigsuspend */
    case 137: return -L_EAGAIN;                               /* rt_sigtimedwait */
    case 129: return (int64_t)a0 == PID || (int64_t)a0 == 0 ? sys_kill((int64_t)a1) : -3;   /* kill: ESRCH */
    case 130: return sys_kill((int64_t)a1);                   /* tkill */
    case 131: return sys_kill((int64_t)a2);                   /* tgkill */
    case 98: return sys_futex(a0, (int64_t)a1, a2, a3);
    case 220: case 435:                                       /* clone, clone3 */
        once((int)nr, "(a new thread): one thread only, said EAGAIN");
        return -L_EAGAIN;
    case 221: once(221, "(execve): can't start another program"); return -L_ENOSYS;
    case 260: return -10;                                     /* wait4: ECHILD */

    /* ---- sockets ---- */
    case 198: {                                               /* socket */
        int s = lxnet_socket((int)a0, (int)a1, (int)a2);
        if (s < 0) return s;
        int fd = fd_alloc(0);
        if (fd < 0) { lxnet_close(s); return fd; }
        memset(&lx_fd[fd], 0, sizeof lx_fd[fd]);
        lx_fd[fd].kind = FD_SOCK; lx_fd[fd].sock = s;
        lx_fd[fd].flags = L_O_RDWR | ((a1 & 04000) ? L_O_NONBLOCK : 0);
        lx_fd[fd].cloexec = (a1 & 02000000) != 0;
        lxnet_set_nonblock(s, (a1 & 04000) != 0);
        return fd;
    }
    case 200: case 203: {                                     /* bind, connect */
        if (!(f = fdp((int64_t)a0)) || f->kind != FD_SOCK) return f ? -L_ENOTSOCK : -L_EBADF;
        UOK(a1, a2);
        return nr == 200 ? lxnet_bind(f->sock, U8(a1), (int)a2) : lxnet_connect(f->sock, U8(a1), (int)a2);
    }
    case 204: case 205: {                                     /* getsockname, getpeername */
        if (!(f = fdp((int64_t)a0)) || f->kind != FD_SOCK) return f ? -L_ENOTSOCK : -L_EBADF;
        UOK(a2, 4);
        uint32_t len; memcpy(&len, U8(a2), 4);
        UOK(a1, len);
        r = nr == 204 ? lxnet_getsockname(f->sock, U8(a1), &len) : lxnet_getpeername(f->sock, U8(a1), &len);
        memcpy(U8(a2), &len, 4);
        return r;
    }
    case 206:                                                 /* sendto */
        if (!(f = fdp((int64_t)a0)) || f->kind != FD_SOCK) return f ? -L_ENOTSOCK : -L_EBADF;
        UOK(a1, a2);
        if (a4) UOK(a4, a5);
        return lxnet_sendto(f->sock, U8(a1), a2, a4 ? U8(a4) : 0, (int)a5, (f->flags & L_O_NONBLOCK) || (a3 & 0x40));
    case 207: {                                               /* recvfrom */
        if (!(f = fdp((int64_t)a0)) || f->kind != FD_SOCK) return f ? -L_ENOTSOCK : -L_EBADF;
        UOK(a1, a2);
        uint32_t len = 0;
        if (a4) { UOK(a5, 4); memcpy(&len, U8(a5), 4); UOK(a4, len); }
        r = lxnet_recvfrom(f->sock, U8(a1), a2, a4 ? U8(a4) : 0, a4 ? &len : 0, (f->flags & L_O_NONBLOCK) || (a3 & 0x40), (a3 & 2) != 0);
        if (a4 && r >= 0) memcpy(U8(a5), &len, 4);
        return r;
    }
    case 211: case 212: {                                     /* sendmsg, recvmsg: one buffer at a time */
        if (!(f = fdp((int64_t)a0)) || f->kind != FD_SOCK) return f ? -L_ENOTSOCK : -L_EBADF;
        UOK(a1, 56);
        uint8_t *m = U8(a1);
        uint64_t name, iov, iovlen; uint32_t namelen;
        memcpy(&name, m, 8); memcpy(&namelen, m + 8, 4); memcpy(&iov, m + 16, 8); memcpy(&iovlen, m + 24, 8);
        if (iovlen < 1) return -L_EINVAL;
        UOK(iov, 16);
        uint64_t base, len;
        memcpy(&base, U8(iov), 8); memcpy(&len, U8(iov + 8), 8);
        UOK(base, len);
        if (name) UOK(name, namelen);
        int nb = (f->flags & L_O_NONBLOCK) || (a2 & 0x40);
        if (nr == 211) return lxnet_sendto(f->sock, U8(base), len, name ? U8(name) : 0, (int)namelen, nb);
        r = lxnet_recvfrom(f->sock, U8(base), len, name ? U8(name) : 0, name ? &namelen : 0, nb, (a2 & 2) != 0);
        if (r >= 0 && name) memcpy(m + 8, &namelen, 4);
        if (r >= 0) { uint64_t z = 0; memcpy(m + 40, &z, 8); memset(m + 48, 0, 4); }    /* no control data, no flags */
        return r;
    }
    case 208: {                                               /* setsockopt */
        if (!(f = fdp((int64_t)a0)) || f->kind != FD_SOCK) return f ? -L_ENOTSOCK : -L_EBADF;
        if (a4) UOK(a3, a4);
        return lxnet_setsockopt(f->sock, (int)a1, (int)a2, U8(a3), (int)a4);
    }
    case 209: {                                               /* getsockopt */
        if (!(f = fdp((int64_t)a0)) || f->kind != FD_SOCK) return f ? -L_ENOTSOCK : -L_EBADF;
        UOK(a4, 4);
        uint32_t len; memcpy(&len, U8(a4), 4);
        UOK(a3, len);
        r = lxnet_getsockopt(f->sock, (int)a1, (int)a2, U8(a3), &len);
        memcpy(U8(a4), &len, 4);
        return r;
    }
    case 210: return 0;                                       /* shutdown */
    case 199: case 201: case 202: case 242: return -L_EOPNOTSUPP;   /* socketpair, listen, accept(4) */

    case 19: case 20: case 26: case 74: case 85: case 279: case 437:
        once((int)nr, "(eventfd/epoll/inotify/signalfd/timerfd/memfd/openat2) isn't here");
        return -L_ENOSYS;
    }
    once((int)nr, "isn't here: said ENOSYS");
    return -L_ENOSYS;
}
