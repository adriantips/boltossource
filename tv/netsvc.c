/* ===========================================================================
 *  BoltOS TV  -  tv/netsvc.c
 *
 *  Everything that waits on the network happens on a kernel thread of its
 *  own: getting a lease, setting the clock, and every page and picture the
 *  browser asks for. The shell's thread never blocks on a packet, so a TLS
 *  handshake that takes two seconds is two seconds of a spinner over a film
 *  that keeps moving, not two seconds of a frozen picture.
 *
 *  The thread exists only while there is something to do. The scheduler is
 *  round-robin, so an idle thread parked in `hlt` would still be handed every
 *  other timer tick -- half the machine, gone, to wait for nothing. Instead
 *  the worker exits when its queue runs dry, and netsvc_fetch() starts a new
 *  one when there is work again.
 * ===========================================================================*/
#include <stdint.h>
#include "tv.h"
#include "io.h"
#include "net.h"
#include "netif.h"
#include "dhcp.h"
#include "ntp.h"
#include "net6.h"
#include "http.h"
#include "sched.h"
#include "kheap.h"
#include "string.h"
#include "stdio.h"
#include "kprintf.h"
#include "pit.h"

static volatile int state = NET_DOWN;
static volatile int booted;           /* lease + clock attempted            */
static volatile int synced;
static char nic[48];

#define QCAP 16
static fetch_t *queue[QCAP];
static volatile int qhead, qtail;
static volatile int worker_alive;

int netsvc_state(void)        { return state; }
const char *netsvc_nic(void)  { return nic; }
uint32_t netsvc_ip(void)      { return net_ip; }
uint32_t netsvc_gw(void)      { return net_gw; }
uint32_t netsvc_dns(void)     { return net_dns; }
int netsvc_clock_synced(void) { return synced; }

void fmt_ip(char *out, uint32_t ip) {
    snprintf(out, 16, "%u.%u.%u.%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

static void bring_up(void) {
    state = NET_PROBING;
    net_init();
    struct netif *nif = netif_default();
    if (!nif) {
        state = NET_NOLINK;
        kprintf("[tv] network: no adapter this kernel drives\n");
        return;
    }
    snprintf(nic, sizeof nic, "Ethernet (%s)", nif->name);
    state = NET_DHCP;
    if (dhcp_configure(4000) != 0 || !net_ip) {
        state = NET_NOLINK;
        kprintf("[tv] network: no DHCP answer\n");
        return;
    }
    state = NET_UP;
    nd6_autoconfigure(nif, 1500);
    if (ntp_sync(NTP_DEFAULT_SERVER, 3000) == 0) synced = 1;
    char ip[16]; fmt_ip(ip, net_ip);
    kprintf("[tv] network up: %s, clock %s\n", ip, synced ? "set by NTP" : "from the RTC");
}

static fetch_t *dequeue(void) {
    uint64_t f = irq_save();
    fetch_t *j = 0;
    while (qhead != qtail) {
        j = queue[qhead];
        qhead = (qhead + 1) % QCAP;
        if (j->state == FETCH_QUEUED) { j->state = FETCH_RUNNING; break; }
        if (j->state == FETCH_CANCELLED) { kfree(j->buf); kfree(j); }   /* released while waiting */
        j = 0;
    }
    if (!j) worker_alive = 0;          /* decided under the same lock netsvc_fetch takes */
    irq_restore(f);
    return j;
}

static void run_job(fetch_t *j) {
    if (tvs.adblock && adblock_url_blocked(j->url)) {
        j->status = -8;                 /* refused before a packet was sent */
        j->len = -1;
    } else if (state != NET_UP) {
        j->status = state == NET_NOLINK ? -6 : -7;
        j->len = -1;
    } else {
        j->location[0] = 0;
        j->len = http_get(j->url, j->buf, j->cap, &j->status, j->location, sizeof j->location);
    }
    uint64_t f = irq_save();
    if (j->state == FETCH_CANCELLED) { irq_restore(f); kfree(j->buf); kfree(j); return; }
    j->state = j->len >= 0 ? FETCH_DONE : FETCH_FAILED;
    irq_restore(f);
}

static void worker(void) {
    if (!booted) { bring_up(); booted = 1; }
    for (;;) {
        fetch_t *j = dequeue();
        if (!j) break;
        run_job(j);
    }
    sched_exit_current();
    for (;;) __asm__ volatile("hlt");   /* reaped at the next switch */
}

static void ensure_worker(void) {
    uint64_t f = irq_save();
    int start = !worker_alive;
    if (start) worker_alive = 1;
    irq_restore(f);
    if (start && sched_add(worker, "tv-net") < 0) {
        worker_alive = 0;
        kprintf("[tv] could not start the network thread\n");
    }
}

void netsvc_start(void) { ensure_worker(); }

int netsvc_fetch(fetch_t *j) {
    uint64_t f = irq_save();
    int n = (qtail + 1) % QCAP;
    if (n == qhead) { irq_restore(f); return -1; }
    j->state = FETCH_QUEUED;
    j->len = 0;
    j->status = 0;
    queue[qtail] = j;
    qtail = n;
    irq_restore(f);
    ensure_worker();
    return 0;
}

/* The caller is done with `j` (and its buffer, which must be kmalloc'd):
 * free them now if nothing is using them, or have the worker do it. */
void netsvc_cancel(fetch_t *j) {
    if (!j) return;
    uint64_t f = irq_save();
    int st = j->state;
    if (st == FETCH_QUEUED || st == FETCH_RUNNING) {
        j->state = FETCH_CANCELLED;
        irq_restore(f);
        return;
    }
    irq_restore(f);
    kfree(j->buf);
    kfree(j);
}
