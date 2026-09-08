#include <stdint.h>
#include "sched.h"
#include "vmm.h"
#include "kheap.h"
#include "string.h"
#include "gdt.h"
#include "proc.h"
#include "vmm.h"

#define MAXT    16
#define KSTACK  65536          /* per-thread kernel stack: deep enough for the
                                * TLS/X.509/RSA chain a SYS_HTTPGET runs in ring 0 */

typedef struct {
    uint64_t     rsp;          /* saved register-frame pointer when not running */
    uint64_t     cr3;          /* address space (PML4 physical)                 */
    uint64_t     kstack_top;   /* ring0 stack for syscall/trap entry (user thr) */
    void        *kstack;       /* base of that allocation, for freeing it       */
    struct proc *proc;         /* owning process, 0 for kernel threads          */
    int          used;
    /* A retired thread cannot free its own kernel stack or address space --
     * it is still running on the one and inside the other. It becomes a zombie
     * instead, and the next context switch (which happens on a different stack,
     * in a different address space) reaps it. */
    int          zombie;
    int          kill_req;     /* asked to die at its next scheduling point     */
    uint64_t     ticks;        /* scheduler ticks this thread has been current  */
    uint64_t     last_run;     /* tick count when it last ran (starvation check)*/
    char         name[16];
    /* FPU/SSE state. Saved/restored by the ISR common path (isr.asm) around
     * every interrupt so a handler's XMM use never corrupts the interrupted
     * thread, and so context switches preserve float state. 16-byte aligned as
     * FXSAVE/FXRSTOR require. */
    _Alignas(16) uint8_t fxarea[512];
} kthread_t;

/* Lay down a valid initial FXSAVE image: x87 control word 0x037F (all
 * exceptions masked, 64-bit precision) and MXCSR 0x1F80 (all SSE exceptions
 * masked). Everything else zero. */
static void fpu_init_area(uint8_t *a) {
    memset(a, 0, 512);
    a[0] = 0x7F; a[1] = 0x03;          /* FCW  @ off 0  */
    a[24] = 0x80; a[25] = 0x1F;        /* MXCSR @ off 24 */
}

static kthread_t th[MAXT];
static int       cur      = 0;
static int       enabled  = 0;
static uint64_t  nswitch  = 0;
static uint64_t  ticks    = 0;      /* scheduler ticks since boot */

/* set by kernel/syscall.c; the SYSCALL prologue reads it to find its stack.
 * The scheduler points it at the running user thread's per-process stack. */
extern uint64_t  syscall_kstack_top;

/* Called from isr_common (kernel/isr.asm) bracketing the C handler. Built with
 * SSE codegen disabled so the functions themselves never touch XMM — they only
 * issue the raw fxsave/fxrstor against the current thread's save area, which
 * isr_handler() may have re-pointed by switching `cur`. */
__attribute__((target("no-sse")))
void fpu_save_current(void) {
    __asm__ volatile("fxsave (%0)" :: "r"(th[cur].fxarea) : "memory");
}
__attribute__((target("no-sse")))
void fpu_restore_current(void) {
    __asm__ volatile("fxrstor (%0)" :: "r"(th[cur].fxarea) : "memory");
}

static int slot_alloc(void) {
    for (int i = 0; i < MAXT; i++) if (!th[i].used) return i;
    return -1;
}

void sched_init(void) {
    th[0].used = 1;
    th[0].cr3  = vmm_kernel_pml4();
    th[0].proc = 0;
    strncpy(th[0].name, "kmain", sizeof th[0].name);   /* current context */
    fpu_init_area(th[0].fxarea);
    cur = 0;
    irq_set_tick_hook(schedule);
    enabled = 1;
}

int sched_add(void (*entry)(void), const char *name) {
    int i = slot_alloc();
    if (i < 0) return -1;
    uint8_t *stack = (uint8_t *)kmalloc(KSTACK);
    if (!stack) return -1;

    /* Hand-craft an initial register frame at the top of the new stack so the
     * ISR epilogue (pop GPRs; iretq) starts the thread at entry() in ring 0. */
    uint64_t top = ((uint64_t)stack + KSTACK) & ~0xFull;
    struct registers *f = (struct registers *)(top - sizeof(struct registers));
    memset(f, 0, sizeof *f);
    f->rip    = (uint64_t)entry;
    f->cs     = 0x08;          /* kernel code */
    f->rflags = 0x202;         /* IF set */
    f->rsp    = top;           /* thread's own stack when running */
    f->ss     = 0x10;          /* kernel data */

    th[i].rsp        = (uint64_t)f;
    th[i].cr3        = vmm_kernel_pml4();
    th[i].kstack_top = top;
    th[i].kstack     = stack;
    th[i].proc       = 0;
    th[i].used       = 1;
    th[i].zombie = th[i].kill_req = 0;
    th[i].ticks = 0; th[i].last_run = ticks;
    fpu_init_area(th[i].fxarea);
    strncpy(th[i].name, name, sizeof th[i].name);
    return i;
}

int sched_add_user(uint64_t rip, uint64_t ustack_top, uint64_t cr3,
                   struct proc *p, const char *name) {
    int i = slot_alloc();
    if (i < 0) return -1;
    uint8_t *stack = (uint8_t *)kmalloc(KSTACK);    /* kernel stack for traps/syscalls */
    if (!stack) return -1;

    /* initial frame is already a ring-3 frame: the ISR epilogue's iretq performs
     * the ring0->ring3 transition straight into the user program. */
    uint64_t top = ((uint64_t)stack + KSTACK) & ~0xFull;
    struct registers *f = (struct registers *)(top - sizeof(struct registers));
    memset(f, 0, sizeof *f);
    f->rip    = rip;
    f->cs     = 0x23;          /* user code, RPL 3 */
    f->rflags = 0x202;         /* IF set */
    f->rsp    = ustack_top;
    f->ss     = 0x1B;          /* user data, RPL 3 */

    th[i].rsp        = (uint64_t)f;
    th[i].cr3        = cr3;
    th[i].kstack_top = top;
    th[i].kstack     = stack;
    th[i].proc       = p;
    th[i].used       = 1;
    th[i].zombie = th[i].kill_req = 0;
    th[i].ticks = 0; th[i].last_run = ticks;
    fpu_init_area(th[i].fxarea);
    strncpy(th[i].name, name, sizeof th[i].name);
    return i;
}

/* Release a retired thread's resources. Only ever called from schedule(), and
 * only for a slot that is not the one we are running on -- by then we are on a
 * different kernel stack and in a different address space, so both are safe to
 * hand back. Without this, every process that ever ran leaked its 64 KiB kernel
 * stack plus its entire page-table tree and data pages. */
static void reap(kthread_t *t) {
    if (t->kstack) { kfree(t->kstack); t->kstack = 0; }
    if (t->cr3 && t->cr3 != vmm_kernel_pml4()) vmm_free_address_space(t->cr3);
    if (t->proc) { kfree(t->proc); t->proc = 0; }
    t->cr3 = 0;
    t->zombie = 0;
}

struct registers *schedule(struct registers *r) {
    if (!enabled) return r;
    ticks++;
    if (th[cur].used) {
        th[cur].rsp = (uint64_t)r;                 /* save outgoing (if still alive) */
        th[cur].ticks++;
        th[cur].last_run = ticks;
    }

    /* round-robin to the next live slot (slots may have holes from exit) */
    int n = cur;
    for (int k = 0; k < MAXT; k++) {
        n = (n + 1) % MAXT;
        if (th[n].used) break;
    }
    if (n == cur && th[cur].used) return r;        /* sole runnable thread */

    int prev = cur;
    cur = n;
    nswitch++;
    th[cur].last_run = ticks;
    vmm_switch(th[cur].cr3);                        /* activate incoming address space */
    if (th[cur].proc) {                             /* ring-3: arm its kernel stack */
        tss_set_rsp0(th[cur].kstack_top);
        syscall_kstack_top = th[cur].kstack_top;
    }
    /* Reap zombies -- but never `prev`. We are still standing on prev's kernel
     * stack right now: this very ISR frame lives there, and the epilogue only
     * steps off it when it loads the frame we are about to return. So a zombie
     * is freed on the *second* switch after it died, by which point nothing is
     * using its stack at all. One extra scheduling round is a cheap price for
     * not freeing memory out from under the running frame. */
    for (int i = 0; i < MAXT; i++)
        if (th[i].zombie && i != cur && i != prev) reap(&th[i]);

    return (struct registers *)th[cur].rsp;         /* restore incoming context */
}

void sched_exit_current(void) {
    if (cur == 0) return;                          /* never retire kmain (slot 0) */
    th[cur].used   = 0;
    th[cur].zombie = 1;                            /* schedule() frees it shortly */
}

/* ---------------------------------------------------------------------------
 *  Killing and watching threads.
 * ------------------------------------------------------------------------- */

/* Ask a thread to die. A ring-3 thread is terminated immediately: its slot is
 * retired and its resources queued for reaping, which is safe because it is not
 * the running thread (a thread cannot ask itself to die through this path).
 * Kernel threads are only flagged -- unwinding arbitrary kernel state from the
 * outside is not something this kernel can do safely. */
int sched_kill(int slot) {
    if (slot <= 0 || slot >= MAXT || !th[slot].used) return -1;
    if (slot == cur) return -2;                    /* caller must use proc_exit  */
    if (!th[slot].proc) { th[slot].kill_req = 1; return 1; }  /* advisory only   */

    /* Hand back its files and the screen now, while the proc is still intact;
     * reap() frees the proc itself once we have switched off its stack. */
    proc_release(th[slot].proc);
    th[slot].used   = 0;
    th[slot].zombie = 1;
    return 0;
}

int sched_slot_of_pid(int pid) {
    for (int i = 0; i < MAXT; i++)
        if (th[i].used && th[i].proc && proc_pid_of(th[i].proc) == pid) return i;
    return -1;
}

int      sched_is_user(int i)  { return (i >= 0 && i < MAXT && th[i].used && th[i].proc) ? 1 : 0; }
uint64_t sched_thread_ticks(int i) { return (i >= 0 && i < MAXT) ? th[i].ticks : 0; }
uint64_t sched_idle_ticks(int i) {
    if (i < 0 || i >= MAXT || !th[i].used) return 0;
    return ticks - th[i].last_run;
}
uint64_t sched_ticks(void) { return ticks; }

/* Starvation watchdog: the slot of a runnable thread that has not been given
 * the CPU for `limit` scheduler ticks, or -1 if everything is being served.
 * Round-robin over a handful of threads should never starve anyone, so a hit
 * here means a thread is stuck inside an interrupt-disabled region -- worth
 * reporting even though the kernel cannot preempt its way out of it. */
int sched_starved(uint64_t limit) {
    for (int i = 0; i < MAXT; i++) {
        if (!th[i].used || i == cur) continue;
        if (ticks - th[i].last_run > limit) return i;
    }
    return -1;
}

struct proc *sched_current_proc(void) { return th[cur].proc; }

int sched_count(void) {
    int n = 0;
    for (int i = 0; i < MAXT; i++) if (th[i].used) n++;
    return n;
}
int         sched_current(void)   { return cur; }
const char *sched_name(int i)     { return (i >= 0 && i < MAXT && th[i].used) ? th[i].name : ""; }
uint64_t    sched_switches(void)  { return nswitch; }
