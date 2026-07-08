#pragma once
#include <stdint.h>

/* Kernel-state registries that back the process/service/timeline commands.
 * BoltOS is a single-address-space cooperative kernel: these describe the real
 * subsystems and execution contexts that exist, not preemptive UNIX processes. */

#define SVC_MAX   24
#define TASK_MAX  16
#define EV_MAX    64
#define EV_LEN    72

typedef struct {
    char     name[16];
    char     kind[12];
    int      running;
    uint64_t since;     /* pit ticks when registered */
} service_t;

typedef struct {
    int      pid;
    char     name[16];
    char     state[10]; /* "run" "sleep" "stop" "froze" */
    int      prio;      /* -20..19, lower = higher priority */
    int      protect;   /* core context; cannot be killed */
    uint64_t start;
} task_t;

void       sysreg_init(void);

int        svc_register(const char *name, const char *kind, int running);
int        svc_count(void);
service_t *svc_get(int i);
service_t *svc_find(const char *name);

int     task_add(const char *name, const char *state, int prio, int protect);
int     task_count(void);
task_t *task_get(int i);
task_t *task_find_pid(int pid);
task_t *task_find_name(const char *name);

void        evlog(const char *msg);
int         evlog_count(void);                       /* retained entries */
const char *evlog_get(int i, uint64_t *ticks_out);   /* i = 0..count-1, oldest first */
