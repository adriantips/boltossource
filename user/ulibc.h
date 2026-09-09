#pragma once
#include <stdint.h>
#include <stddef.h>

/* The ABI itself -- syscall numbers, the argument registers, PROT_*, and the
 * shape of struct user_fbinfo -- comes from the kernel's own header rather than
 * being copied here. Two lists that must agree are one list too many, and the
 * SDK exists precisely so a third-party program reads the same definitions the
 * kernel was built from. */
#include "syscall.h"

/* open flags (match include/vfs.h) */
#define O_RDONLY 0x000
#define O_WRONLY 0x001
#define O_RDWR   0x002
#define O_CREAT  0x040
#define O_TRUNC  0x200
#define O_APPEND 0x400

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct { uint64_t size; uint64_t is_dir; } stat_t;   /* matches SYS_FSTAT */

/* thin syscall wrappers */
long  sys_read (int fd, void *buf, unsigned long len);
long  sys_write(int fd, const void *buf, unsigned long len);
int   open (const char *path, int flags);
int   close(int fd);
long  read (int fd, void *buf, unsigned long len);
long  write(int fd, const void *buf, unsigned long len);
long  lseek(int fd, long off, int whence);
int   fstat(int fd, stat_t *st);
void *mmap (void *addr, unsigned long len, int prot, int flags, int fd);
void *sbrk_brk(void *addr);          /* raw SYS_BRK */
int   getpid(void);
int   bolt_abi(void);       /* the kernel's ABI version; see SDK.md */
void  yield(void);
void  exit(int code) __attribute__((noreturn));

/* ---- device access for the ring-3 browser (see include/syscall.h) -------- */
int   fb_map(struct user_fbinfo *fi);   /* claim the panel + pause compositor; 0/-1 */
void  fb_release(void);                  /* hand the panel back to the desktop    */
void  fb_present(const void *buf);       /* blit a packed w*h xRGB backbuffer     */
int   getkey(void);                      /* next key, or -1 if none (non-blocking) */
long  http_get_u(const char *url, char *buf, unsigned long cap, int *status);

/* ---- sockets (see include/socket.h) --------------------------------------
 * A socket is an ordinary fd: read()/write()/close() work on it directly.
 * Addresses are IPv4 in host order, as everywhere else in the stack. */
#define AF_INET     2
#define SOCK_STREAM 1
int   socket(int domain, int type);
int   connect_ip(int fd, uint32_t ip, uint16_t port, uint32_t timeout_ms);
int   listen_port(int fd, uint16_t port);      /* ports < 1024 need root */
int   accept_conn(int fd, uint32_t *peer_ip, uint16_t *peer_port, uint32_t timeout_ms);
int   resolve_host(const char *name, uint32_t *ip_out);   /* /etc/hosts, cache, DNS */

/* heap */
void *malloc(unsigned long n);
void *calloc(unsigned long nm, unsigned long sz);
void *realloc(void *p, unsigned long n);
void  free(void *p);

/* umbrella: pull in the fuller stdio / stdlib surface */
#include "stdio.h"
#include "stdlib.h"
