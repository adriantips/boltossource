#pragma once
#include <stdint.h>
#include "interrupts.h"

/* Minimal round-robin preemptive scheduler over kernel threads. The timer
 * IRQ0 calls schedule() (installed as the tick hook); it saves the outgoing
 * thread's register frame and returns the incoming thread's frame so the ISR
 * epilogue restores it. All threads run in ring 0 sharing the kernel CR3. */
struct proc;                                        /* defined in proc.h */

void sched_init(void);                              /* adopt current context as thread 0 */
int  sched_add(void (*entry)(void), const char *name);
/* Ring-3 thread: starts at user rip on user stack in address space cr3. The
 * proc owns the per-process kernel stack used for syscall/trap entry; the
 * scheduler loads it into tss.rsp0 + the SYSCALL stack on every switch in. */
int  sched_add_user(uint64_t rip, uint64_t ustack_top, uint64_t cr3,
                    struct proc *p, const char *name);
struct registers *schedule(struct registers *r);   /* tick hook */

/* Retire the running thread: free its slot so schedule() never resumes it.
 * Caller must not return to the abandoned context (spin in hlt). */
void         sched_exit_current(void);
struct proc *sched_current_proc(void);              /* proc of running thread, or 0 */

int          sched_count(void);                     /* live threads */
int          sched_current(void);
const char  *sched_name(int i);
uint64_t     sched_switches(void);

/* Heartbeat counters of the two demo threads (kernel/main.c): they wake once a
 * second, so a rising count is direct evidence the scheduler is preempting. */
uint64_t     demo_thread_beats(int which);

/* ---------------------------------------------------------------------------
 *  Killing threads, and watching for stuck ones.
 * ------------------------------------------------------------------------- */

/* Terminate the thread in `slot`. A ring-3 thread dies immediately and its
 * stack, address space and proc are queued for reaping on the next context
 * switch; 0 is returned. A kernel thread is only flagged (1 returned) because
 * unwinding arbitrary kernel state from the outside is not safe. -1 = no such
 * thread, -2 = that is the calling thread (use proc_exit instead). */
int      sched_kill(int slot);
int      sched_slot_of_pid(int pid);      /* -1 if no live thread has that pid  */
int      sched_is_user(int slot);         /* 1 if it is a ring-3 thread         */

uint64_t sched_ticks(void);               /* scheduler ticks since boot         */
uint64_t sched_thread_ticks(int slot);    /* ticks this thread has been current */
uint64_t sched_idle_ticks(int slot);      /* ticks since it last ran            */

/* Slot of a runnable thread that has not been scheduled for `limit` ticks, or
 * -1. Round-robin should never starve anyone, so a hit means a thread is wedged
 * with interrupts disabled -- worth reporting even though the kernel cannot
 * preempt its way out of that. */
int      sched_starved(uint64_t limit);
