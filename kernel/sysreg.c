#include <stdint.h>
#include "sysreg.h"
#include "string.h"
#include "pit.h"

/* ---- services ----------------------------------------------------------- */
static service_t svcs[SVC_MAX];
static int       nsvc;

int svc_register(const char *name, const char *kind, int running) {
    if (nsvc >= SVC_MAX) return -1;
    service_t *s = &svcs[nsvc];
    strncpy(s->name, name, sizeof(s->name));
    strncpy(s->kind, kind, sizeof(s->kind));
    s->running = running;
    s->since   = pit_ticks();
    return nsvc++;
}
int        svc_count(void)       { return nsvc; }
service_t *svc_get(int i)        { return (i >= 0 && i < nsvc) ? &svcs[i] : 0; }
service_t *svc_find(const char *name) {
    for (int i = 0; i < nsvc; i++)
        if (strcmp(svcs[i].name, name) == 0) return &svcs[i];
    return 0;
}

/* ---- tasks -------------------------------------------------------------- */
static task_t tasks[TASK_MAX];
static int    ntask;
static int    next_pid = 1;

int task_add(const char *name, const char *state, int prio, int protect) {
    if (ntask >= TASK_MAX) return -1;
    task_t *t = &tasks[ntask];
    t->pid = next_pid++;
    strncpy(t->name, name, sizeof(t->name));
    strncpy(t->state, state, sizeof(t->state));
    t->prio    = prio;
    t->protect = protect;
    t->start   = pit_ticks();
    return tasks[ntask++].pid;
}
int     task_count(void)        { return ntask; }
task_t *task_get(int i)         { return (i >= 0 && i < ntask) ? &tasks[i] : 0; }
task_t *task_find_pid(int pid) {
    for (int i = 0; i < ntask; i++) if (tasks[i].pid == pid) return &tasks[i];
    return 0;
}
task_t *task_find_name(const char *name) {
    for (int i = 0; i < ntask; i++)
        if (strcmp(tasks[i].name, name) == 0) return &tasks[i];
    return 0;
}

/* ---- event log (ring) --------------------------------------------------- */
static char     ev_msg[EV_MAX][EV_LEN];
static uint64_t ev_tick[EV_MAX];
static int      ev_total;        /* total ever logged */

void evlog(const char *msg) {
    int slot = ev_total % EV_MAX;
    strncpy(ev_msg[slot], msg, EV_LEN);
    ev_tick[slot] = pit_ticks();
    ev_total++;
}
int evlog_count(void) { return ev_total < EV_MAX ? ev_total : EV_MAX; }

const char *evlog_get(int i, uint64_t *ticks_out) {
    int cnt = evlog_count();
    if (i < 0 || i >= cnt) return 0;
    int start = ev_total - cnt;          /* oldest retained */
    int slot  = (start + i) % EV_MAX;
    if (ticks_out) *ticks_out = ev_tick[slot];
    return ev_msg[slot];
}

/* ---- bootstrap ---------------------------------------------------------- */
void sysreg_init(void) {
    svc_register("serial",   "driver", 1);
    svc_register("console",  "driver", 1);
    svc_register("keyboard", "driver", 1);
    svc_register("pit",      "timer",  1);
    svc_register("pmm",      "memory", 1);
    svc_register("kheap",    "memory", 1);
    svc_register("ramfs",    "fs",     1);
    svc_register("bshell",   "shell",  1);

    task_add("kinit",  "run",   0,  1);
    task_add("kidle",  "sleep", 19, 1);
    task_add("bshell", "run",   0,  1);
}
