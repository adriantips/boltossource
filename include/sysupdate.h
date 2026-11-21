#pragma once
#include <stdint.h>

/* ===========================================================================
 *  System updates for the x86 editions -- the desktop and BoltOS TV
 *  (kernel/sysupdate.c). BoltOS Mini has its own, on the same files:
 *
 *      https://boltos.dev/update/<channel>.json   the newest version
 *      https://boltos.dev/update/<channel>-<version>.bin   its kernel.bin
 *
 *  release.sh publishes both. The edition says how to reach the network and
 *  restart (sysupd_host_t); the rest -- when to ask, what to install by
 *  itself, writing the kernel onto the disk this machine started from -- is
 *  here, on a kernel thread of its own, so nothing on screen waits for it.
 * ===========================================================================*/

enum {
    SU_WAITING,         /* not asked yet: no network                              */
    SU_CHECKING,
    SU_CURRENT,         /* this is the newest                                     */
    SU_AVAILABLE,       /* a newer one, waiting for Update (or the next start)    */
    SU_DOWNLOADING,
    SU_INSTALLING,      /* onto the boot disk                                     */
    SU_RESTART,         /* on the disk; restarting into it in a moment            */
    SU_FAILED,          /* the last check or install didn't work: msg says why    */
};

typedef struct {
    const char *product;              /* "BoltOS TV"                               */
    const char *channel;              /* "tv": which .json to ask for              */
    const char *version;              /* this kernel's, "1.1.0"                    */
    int  (*online)(void);             /* the network is up                         */
    /* GET url (with extra header lines, or NULL) into buf, on the update
     * thread; the body's length or -1. NULL: net/http.c's http_request,
     * straight from the update thread. */
    int  (*fetch)(const char *url, const char *headers, char *buf, uint32_t cap, int *status);
    void (*toast)(const char *title, const char *detail);
    void (*reboot)(void);
    uint64_t (*idle_ms)(void);        /* since the last key; NULL: always idle     */
} sysupd_host_t;

typedef struct {
    volatile int state;
    int      have;                    /* an offer below is newer than this         */
    char     version[16];
    char     date[16];
    char     notes[480];              /* what's new, a line each                   */
    uint32_t size;
    int      big;                     /* waits for Update even when automatic      */
    volatile uint32_t got;            /* downloaded so far                         */
    uint64_t checked;                 /* ms of the last answer, 0 never             */
    char     msg[128];
    char     disk[64];                /* where an update goes: "ahci0, EFI partition" */
} sysupd_t;

void            sysupd_init(const sysupd_host_t *host);
int             sysupd_poll(void);        /* the shell, every frame: 1 = redraw     */
const sysupd_t *sysupd_state(void);
int             sysupd_auto(void);
void            sysupd_set_auto(int on);  /* kept on the boot disk                   */
/* A few lines of the product's own settings ("key=value\n"), kept beside
 * UPDATE.CFG as PREFS.CFG when the stick is there. Read once at start:
 * sysupd_prefs_loaded() says when that has happened. */
int             sysupd_prefs_loaded(void);
void            sysupd_prefs_get(char *out, int cap);
void            sysupd_prefs_set(const char *text);
void            sysupd_check(void);
int             sysupd_install(void);     /* the newer one, now                      */
void            sysupd_restart(void);     /* into what was installed                 */
