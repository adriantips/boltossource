/* BoltOS Mini - mini/update.h: new versions of BoltOS Mini from boltos.dev
 * (mini/update.c). Everything here runs on the boot core, from the main loop. */
#pragma once
#include <stdint.h>

enum {
    UPD_WAITING,        /* not asked yet: no network, or the clock isn't set     */
    UPD_CHECKING,
    UPD_CURRENT,        /* this is the newest                                     */
    UPD_AVAILABLE,      /* a newer one, waiting for Update (or the next start)    */
    UPD_DOWNLOADING,
    UPD_INSTALLING,     /* onto the card                                          */
    UPD_RESTART,        /* on the card; the Pi restarts into it in a moment       */
    UPD_FAILED,         /* the last check or install didn't work: msg says why    */
};

typedef struct {
    int      state;
    int      have;                /* an offer below is newer than this           */
    char     version[16];         /* the newest on boltos.dev                    */
    char     date[16];
    char     notes[480];          /* what's new, a line each                     */
    uint32_t size;                /* bytes to download                           */
    int      big;                 /* waits for Update, even when updates are automatic */
    uint64_t got;                 /* downloaded so far                           */
    uint64_t checked;             /* now_ms() of the last answer, 0 never         */
    char     msg[128];
} update_t;

void            update_init(void);
int             update_poll(void);      /* main loop: 1 if the screen should change  */
const update_t *update_state(void);
const char     *update_version(void);   /* this kernel's, "1.1.0"                     */
int             update_auto(void);      /* quick updates install by themselves        */
void            update_set_auto(int on);
void            update_check(void);     /* ask boltos.dev now                          */
int             update_install(void);   /* the newer one, now; restarts when it's in  */
/* The bar's note ("update 1.2.0", "updating 40%"); 0 if there's nothing to say. */
int             update_bar(char *out, int cap);
int             update_dev(const char *cmd, char *out, int cap);
