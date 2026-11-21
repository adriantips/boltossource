/* ===========================================================================
 *  BoltOS Mini  -  mini/lxint.h
 *
 *  What the Linux layer's files share (lx.c, lx_sys.c, lx_fs.c, lx_dev.c,
 *  lx_net.c). Everything here but the mailbox and the output ring is touched
 *  only by the program's core while it runs.
 * ===========================================================================*/
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "lx.h"
#include "store.h"

/* ---- Linux's numbers --------------------------------------------------------- */
#define L_EPERM 1
#define L_ENOENT 2
#define L_EINTR 4
#define L_EIO 5
#define L_EBADF 9
#define L_EAGAIN 11
#define L_ENOMEM 12
#define L_EACCES 13
#define L_EFAULT 14
#define L_EEXIST 17
#define L_EXDEV 18
#define L_ENOTDIR 20
#define L_EISDIR 21
#define L_EINVAL 22
#define L_EMFILE 24
#define L_ENOTTY 25
#define L_ENOSPC 28
#define L_ESPIPE 29
#define L_EROFS 30
#define L_ERANGE 34
#define L_ENAMETOOLONG 36
#define L_ENOSYS 38
#define L_ENOTEMPTY 39
#define L_ENOTSOCK 88
#define L_EDESTADDRREQ 89
#define L_EMSGSIZE 90
#define L_ENOPROTOOPT 92
#define L_EPROTONOSUPPORT 93
#define L_EOPNOTSUPP 95
#define L_EAFNOSUPPORT 97
#define L_EADDRINUSE 98
#define L_EADDRNOTAVAIL 99
#define L_ENETUNREACH 101
#define L_ECONNREFUSED 111
#define L_ETIMEDOUT 110

#define L_O_ACCMODE   3
#define L_O_WRONLY    1
#define L_O_RDWR      2
#define L_O_CREAT     0100
#define L_O_EXCL      0200
#define L_O_TRUNC     01000
#define L_O_APPEND    02000
#define L_O_NONBLOCK  04000
#define L_O_DIRECTORY 040000           /* arm64's values */
#define L_O_CLOEXEC   02000000
#define L_AT_FDCWD    (-100)
#define L_AT_REMOVEDIR 0x200
#define L_AT_EMPTY_PATH 0x1000

#define L_S_IFMT  0170000
#define L_S_IFDIR 0040000
#define L_S_IFREG 0100000
#define L_S_IFCHR 0020000
#define L_S_IFSOCK 0140000

/* ---- memory ------------------------------------------------------------------- */
#define LX_PAGE 4096ull
/* The program's memory: both big windows, page by page (lx.c). */
void    *lx_pages(uint64_t bytes);                     /* zeroed; 0 if none        */
int      lx_pages_at(uint64_t addr, uint64_t bytes);    /* claim exactly there      */
void     lx_unpages(uint64_t addr, uint64_t bytes);
int      lx_pages_used(uint64_t addr, uint64_t bytes); /* all of it is the program's */
uint64_t lx_mem_free(void);
uint64_t lx_mem_total(void);
/* A pointer the program handed in: inside its memory, for len bytes. */
int      lx_uok(uint64_t p, uint64_t len);
int      lx_ustr(uint64_t p, char *out, int cap);      /* a path: 0, or -errno     */
/* Small things that live as long as the program (paths, tables). */
void    *lx_kalloc(uint64_t n);
/* Pages for Mini's own use inside the windows (the cache, files in memory,
 * the framebuffer): a program's munmap can't give these away. */
void    *lx_kpages(uint64_t bytes);
void     lx_kunpages(uint64_t addr, uint64_t bytes);
int      lx_pages_force(uint64_t addr, uint64_t bytes);  /* MAP_FIXED: the program's now */
void     lx_sync_code(uint64_t addr, uint64_t len);     /* new code: caches          */

/* ---- the boot core does what needs the card or the network --------------------- */
enum { RQ_NONE, RQ_STORE, RQ_FSREAD, RQ_FSWRITE, RQ_MKDIR, RQ_REMOVE, RQ_RENAME, RQ_NET };
typedef struct {
    int      kind;
    int      pack;                    /* RQ_STORE: which pack, offset, length, where    */
    uint64_t off;
    uint32_t len;
    void    *buf;
    char     path[256], path2[256];   /* RQ_FS*: card paths                            */
    int      result;                  /* answer: >= 0, or -errno                        */
    uint8_t  net[64];                 /* RQ_NET: lx_net.c's own                         */
} lx_rq_t;
int  lx_request(lx_rq_t *rq);         /* on the program's core: waits for the answer    */
void lx_net_serve(lx_rq_t *rq);       /* on the boot core (lx_net.c)                    */
void lx_net_poll(void);               /* on the boot core, every turn                   */
void lx_net_reset(void);              /* on the boot core, at start and end             */

/* ---- notes and output ------------------------------------------------------------ */
void lx_out(const char *s, uint64_t n);            /* what fd 1 and 2 get              */
void lx_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* "[lx] ..." */
int  lx_stopping(void);
void lx_exit(int code) __attribute__((noreturn));   /* the program is over             */
uint64_t lx_now_ns(void);
uint64_t lx_real_ns(void);
void lx_wait_until(uint64_t ns);                    /* sleeping; returns early on stop  */

/* ---- packs and files (lx_fs.c) --------------------------------------------------- */
typedef struct {
    uint32_t name, parent, mode, mtime;
    uint64_t a, b;                    /* file: offset, size / folder: first child, count */
} lxpak_ent_t;

typedef struct memf memf_t;
enum { FD_NONE, FD_PAK, FD_MEM, FD_DIR, FD_FB, FD_EVDEV, FD_CON, FD_TTY, FD_NULL, FD_ZERO,
       FD_RANDOM, FD_SOCK, FD_SYN };
typedef struct {
    uint8_t  kind, cloexec;
    uint32_t flags;                   /* O_* the program opened it with             */
    uint64_t pos;
    int      pack;                    /* FD_PAK: which, its entry, where, how big    */
    uint32_t ent;
    uint64_t off, size;
    memf_t  *mf;                      /* FD_MEM                                     */
    int      sock;                    /* FD_SOCK: lx_net.c's slot                   */
    const char *syn;                  /* FD_SYN: the text (lx_kalloc'd)             */
    uint64_t ino;
    char     path[256];
} lxfd_t;
#define LX_MAXFD 256
extern lxfd_t *lx_fd;                 /* [LX_MAXFD]                                  */

int  lxfs_init(const lx_config_t *c, char *why, int cap);   /* boot core, before start */
void lxfs_free(void);
int  lxfs_path(int dirfd, const char *in, char *out);        /* absolute, tidy: 0 / -errno */
int  lxfs_open(const char *path, int flags, int mode, lxfd_t *fd);  /* 0 / -errno     */
int64_t lxfs_read(lxfd_t *f, void *buf, uint64_t n, uint64_t at);
int64_t lxfs_write(lxfd_t *f, const void *buf, uint64_t n, uint64_t at);
int  lxfs_close(lxfd_t *f);
int  lxfs_sync(lxfd_t *f);
int  lxfs_stat(const char *path, uint8_t *st);              /* struct stat, 128 bytes  */
int  lxfs_fstat(lxfd_t *f, uint8_t *st);
int  lxfs_getdents(lxfd_t *f, uint8_t *buf, uint64_t cap);
int  lxfs_mkdir(const char *path);
int  lxfs_unlink(const char *path, int dir);
int  lxfs_rename(const char *from, const char *to);
int  lxfs_truncate(lxfd_t *f, uint64_t size);
int  lxfs_access(const char *path);
int  lxfs_readlink(const char *path, char *out, int cap);
const char *lxfs_where(uint64_t pc, uint64_t *off);         /* the mapped file holding pc */
void lxfs_note_map(const char *path, uint64_t base, uint64_t len);
int  lxfs_debug(char *out, int cap);
int  lxfs_flush_all(void);                                   /* every dirty file to the card */
uint64_t lxfs_size(lxfd_t *f);
void lxfs_dup(const lxfd_t *from, lxfd_t *to);
int  lxfs_chdir(const char *path);
const char *lxfs_cwd(void);

/* ---- devices (lx_dev.c) ---------------------------------------------------------- */
int     lxdev_init(int w, int h);                            /* boot core: the framebuffer */
int     lxdev_open(const char *path, lxfd_t *fd);            /* 1 if it was a device */
int64_t lxdev_read(lxfd_t *f, void *buf, uint64_t n);
int64_t lxdev_write(lxfd_t *f, const void *buf, uint64_t n);
int     lxdev_ioctl(lxfd_t *f, uint64_t cmd, uint64_t arg);
int     lxdev_ready(lxfd_t *f);                              /* poll: readable */
uint64_t lxdev_fb_addr(uint64_t *len);
int     lxdev_list(const char *dir, void (*fn)(void *ctx, const char *name, int isdir), void *ctx);

/* ---- sockets (lx_net.c) ---------------------------------------------------------- */
int     lxnet_socket(int domain, int type, int proto);       /* slot, or -errno */
int     lxnet_close(int s);
int     lxnet_bind(int s, const uint8_t *sa, int len);
int     lxnet_connect(int s, const uint8_t *sa, int len);
int64_t lxnet_sendto(int s, const void *buf, uint64_t n, const uint8_t *sa, int salen, int nonblock);
int64_t lxnet_recvfrom(int s, void *buf, uint64_t n, uint8_t *sa, uint32_t *salen, int nonblock, int peek);
int     lxnet_getsockname(int s, uint8_t *sa, uint32_t *len);
int     lxnet_getpeername(int s, uint8_t *sa, uint32_t *len);
int     lxnet_setsockopt(int s, int level, int opt, const void *v, int len);
int     lxnet_getsockopt(int s, int level, int opt, void *v, uint32_t *len);
int     lxnet_ready(int s);
int     lxnet_writable(int s);
void    lxnet_set_nonblock(int s, int on);

/* ---- the syscalls (lx_sys.c) ----------------------------------------------------- */
int64_t lx_syscall(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
void    lxsys_reset(void);
extern uint64_t lx_brk_lo, lx_brk_cur, lx_brk_hi;
extern char lx_exe[256];
