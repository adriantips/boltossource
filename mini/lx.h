/* ===========================================================================
 *  BoltOS Mini  -  mini/lx.h
 *
 *  Running an unmodified Linux program -- an aarch64 ELF built against musl,
 *  static or dynamic -- on one of the spare cores, without a Linux kernel.
 *
 *  The program runs at EL0 under a page table of its own that opens the two
 *  big RAM windows to it and nothing else. Its `svc #0` lands in Mini's
 *  vectors on that core, and lx_sys.c answers it as Linux would: memory
 *  (mmap/brk out of the windows), files, time, a framebuffer, an input
 *  device and UDP sockets. Anything it does wrong -- a bad pointer, an
 *  instruction it shouldn't run -- ends the program, never Mini.
 *
 *  Files: read-only "lx packs" in the card's store (mini/tools/mklxpak.py),
 *  unioned at "/", with every write landing in a folder on the card's FAT32
 *  partition (lx_config.rw) that shadows the packs. /dev/fb0 is a
 *  framebuffer the app shows; /dev/input/event0 is the keyboard and mouse
 *  as Linux evdev events (input.c's raw ring).
 *
 *  The two sides, as in rt.h: the boot core calls lx_start, then lx_poll
 *  every loop -- it does the card reads, the FAT writes and the network the
 *  program asks for, and drains what it printed -- and lx_stop to end it.
 * ===========================================================================*/
#pragma once
#include <stdint.h>

enum { LX_IDLE, LX_STARTING, LX_RUNNING, LX_DONE };

#define LX_MAXARG  24
#define LX_MAXPACK 4

typedef struct {
    const char *argv[LX_MAXARG];     /* argv[0] is the program's path in the union      */
    const char *envp[LX_MAXARG];     /* "HOME=/opt/demo" ...; NULL-terminated            */
    const char *packs[LX_MAXPACK];   /* store names, the first one winning a path       */
    const char *rw;                  /* FAT32 folder every write goes to: "/Games/demo/rw" */
    const char *cwd;                 /* "/" if NULL                                      */
    int  fb_w, fb_h;                 /* /dev/fb0's size, 0 for none                      */
    int  cache_mb;                   /* pack block cache; 0: 64                          */
} lx_config_t;

int  lx_start(const lx_config_t *c); /* 0, or -1 with lx_message() saying why          */
void lx_stop(void);                  /* asks it to end; it does at its next syscall    */
int  lx_poll(void);                  /* every main-loop turn; 1 when there is news     */
int  lx_state(void);                 /* LX_*                                           */
const char *lx_message(void);        /* "Running", "Exited with 0", "Crashed: ..."     */
int  lx_exit_code(void);
int  lx_crashed(void);
int  lx_cpu(void);                   /* the core it runs on, 0 if none                 */
uint64_t lx_elapsed_ms(void);
/* The last lines it printed (stdout and stderr, and Mini's own notes about
 * it), oldest first, NUL-terminated. */
int  lx_console(char *out, int cap);

/* /dev/fb0: the half the program last finished (FBIOPAN_DISPLAY), or the
 * only one. *seq counts finished frames; 0 until the first. The app copies
 * it out and says so with lx_fb_done, which lets the program draw into
 * that half again. */
const uint32_t *lx_fb_frame(int *w, int *h, uint32_t *seq);
void lx_fb_done(uint32_t seq);

/* GET /lx?... for testing from the Mac: run=/path[+arg...], stop, status,
 * con (its console). */
int  lx_dev(const char *q, char *out, int cap);
