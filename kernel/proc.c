#include <stdint.h>
#include "proc.h"
#include "elf.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "mm.h"
#include "sched.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"
#include "klog.h"
#include "framebuffer.h"

static int next_pid = 1;

proc_t *proc_current(void) { return sched_current_proc(); }
int     proc_pid_of(proc_t *p) { return p ? p->pid : -1; }

void proc_init(void) { /* nothing global to set up yet */ }

void proc_release(proc_t *p) {
    if (!p) return;
    g_fb_exclusive = 0;              /* it may have owned the screen */
    for (int i = 0; i < PROC_MAX_FD; i++)
        if (p->fds[i]) { vfs_close(p->fds[i]); p->fds[i] = 0; }
}

/* -------------------------------------------------------------- fd table ---*/
int proc_fd_alloc(proc_t *p, file *f) {
    for (int i = 0; i < PROC_MAX_FD; i++)
        if (!p->fds[i]) { p->fds[i] = f; return i; }
    return -1;
}
file *proc_fd_get(proc_t *p, int fd) {
    if (fd < 0 || fd >= PROC_MAX_FD) return 0;
    return p->fds[fd];
}
int proc_fd_close(proc_t *p, int fd) {
    if (fd < 0 || fd >= PROC_MAX_FD || !p->fds[fd]) return -1;
    vfs_close(p->fds[fd]);
    p->fds[fd] = 0;
    return 0;
}

/* ---------------------------------------------------------- stack builder --*/
static inline void poke64(uint8_t *page_kv, uint64_t frame_off, uint64_t v) {
    *(uint64_t *)(page_kv + frame_off) = v;
}

/* Lay out the SysV entry stack: argc, argv[], NULL, envp NULL, AT_NULL.
 *
 * `argv` is the full vector including argv[0]. It used to be argv[0] plus at
 * most one more, which meant no program could take an option and an operand at
 * the same time -- `netget HOST PATH PORT` silently became `netget HOST`, using
 * defaults for the rest, which is the kind of bug that looks like the network
 * being broken. Returns the initial user RSP, or 0 on failure. */
static uint64_t setup_user_stack(uint64_t cr3, int argc, const char **argv) {
    if (argc < 1 || argc > PROC_MAX_ARGV) return 0;
    uint64_t base = USER_STACK_TOP - (uint64_t)USER_STACK_PGS * PAGE_SIZE;
    uint64_t top_frame = 0;
    for (int i = 0; i < USER_STACK_PGS; i++) {
        uint64_t fr = pmm_alloc_frame();
        if (!fr) return 0;
        uint8_t *kv = (uint8_t *)P2V(fr);
        for (int j = 0; j < (int)PAGE_SIZE; j++) kv[j] = 0;
        uint64_t va = base + (uint64_t)i * PAGE_SIZE;
        if (vmm_map(cr3, va, fr, PTE_PRESENT | PTE_WRITE | PTE_USER) != 0) return 0;
        if (i == USER_STACK_PGS - 1) top_frame = fr;
    }
    /* everything we write lives in the top page: [TOP-PAGE_SIZE, TOP) */
    uint8_t *tk = (uint8_t *)P2V(top_frame);
    uint64_t page_va = USER_STACK_TOP - PAGE_SIZE;     /* user VA of top page base */
    #define OFF(uva) ((uva) - page_va)

    /* Copy the strings down from the very top, remembering where each landed.
     * They must all fit in the top page along with the vector below them. */
    uint64_t uva[PROC_MAX_ARGV];
    uint64_t cur = USER_STACK_TOP;
    for (int i = 0; i < argc; i++) {
        uint64_t slen = strlen(argv[i]) + 1;
        cur = (cur - slen) & ~0xFull;
        if (cur <= page_va + (uint64_t)(argc + 5) * 8 + 64) return 0;   /* no room */
        for (uint64_t j = 0; j < slen; j++) tk[OFF(cur) + j] = (uint8_t)argv[i][j];
        uva[i] = cur;
    }

    /* vector: argc, argv[0..argc-1], NULL, envp NULL, AT_NULL(key,val) */
    uint64_t nq = (uint64_t)argc + 5;
    uint64_t sp = (cur - nq * 8) & ~0xFull;             /* 16-aligned argc slot */
    uint64_t o  = OFF(sp);
    poke64(tk, o, (uint64_t)argc);                       /* argc            */
    uint64_t k = o + 8;
    for (int i = 0; i < argc; i++) { poke64(tk, k, uva[i]); k += 8; }
    poke64(tk, k,      0); k += 8;                        /* argv terminator */
    poke64(tk, k,      0); k += 8;                        /* envp terminator */
    poke64(tk, k,      0); k += 8;                        /* auxv AT_NULL k  */
    poke64(tk, k,      0);                                /* auxv AT_NULL v  */
    #undef OFF
    return sp;
}

/* --------------------------------------------------------------- exec ------*/
int proc_exec(const char *path) { return proc_exec_argv(path, 0); }

int proc_exec_argv(const char *path, const char *arg1) {
    const char *av[2] = { path, arg1 };
    return proc_exec_vec(path, arg1 ? 2 : 1, av);
}

int proc_exec_vec(const char *path, int argc, const char **argv) {
    file *f = vfs_open(path, O_RDONLY);
    if (!f) { kprintf("[exec] %s: not found\n", path); return -1; }

    vstat st;
    if (vfs_fstat(f, &st) != 0 || st.size == 0) { vfs_close(f); return -1; }
    uint8_t *image = (uint8_t *)kmalloc(st.size);
    if (!image) { vfs_close(f); return -1; }
    int64_t got = vfs_read(f, image, st.size);
    vfs_close(f);
    if (got < 0 || (uint64_t)got != st.size) { kfree(image); return -1; }

    uint64_t cr3 = vmm_new_address_space();
    if (!cr3) { kfree(image); return -1; }

    uint64_t entry = 0, brk_end = 0;
    if (elf_load(cr3, image, st.size, &entry, &brk_end) != 0) {
        kprintf("[exec] %s: bad ELF\n", path);
        kfree(image);
        return -1;
    }
    kfree(image);

    uint64_t ustack = setup_user_stack(cr3, argc, argv);
    if (!ustack) return -1;

    proc_t *p = (proc_t *)kmalloc(sizeof(proc_t));
    if (!p) return -1;
    memset(p, 0, sizeof(*p));
    p->pid       = next_pid++;
    p->cr3       = cr3;
    p->brk_start = p->brk_cur = brk_end;
    p->mmap_cur  = USER_MMAP_BASE;
    p->alive     = 1;
    strncpy(p->name, path, sizeof p->name);

    /* stdin/stdout/stderr -> /dev/console */
    p->fds[0] = vfs_open("/dev/console", O_RDONLY);
    p->fds[1] = vfs_open("/dev/console", O_WRONLY);
    p->fds[2] = vfs_open("/dev/console", O_WRONLY);

    if (sched_add_user(entry, ustack, cr3, p, p->name) < 0) { kfree(p); return -1; }
    kprintf("[exec] %s pid=%d entry=0x%lx brk=0x%lx\n", path, p->pid, entry, brk_end);
    return p->pid;
}

/* ---------------------------------------------------------- fault kill -----*/
/* Tear down the running ring-3 process after it faulted, and hand the
 * scheduler's choice of the next thread back to the ISR epilogue.
 *
 * This runs on the faulting thread's own kernel stack, which is why the
 * ordering matters: everything that touches this process is finished before
 * sched_exit_current() retires the slot, and schedule() then returns a frame on
 * some *other* thread's stack, so the epilogue's `mov rsp, rax` steps off this
 * stack and never comes back to it. */
struct registers *proc_fault_kill(struct registers *r) {
    proc_t *p = proc_current();
    if (!p) return 0;

    crash_record("ring-3 fault", p->name, r);

    kprintf("[fault] killed pid=%d (%s): %s at RIP 0x%lx\n",
            p->pid, p->name,
            r->int_no == 14 ? "page fault" :
            r->int_no == 13 ? "general protection fault" :
            r->int_no == 6  ? "invalid opcode" :
            r->int_no == 0  ? "divide by zero" : "fault",
            r->rip);

    p->exit_code = 128 + (int)r->int_no;
    p->alive = 0;
    proc_release(p);

    sched_exit_current();
    return schedule(r);              /* pick someone else; never returns here */
}

/* --------------------------------------------------------------- exit ------*/
void proc_exit(int code) {
    proc_t *p = proc_current();
    if (p) {
        p->exit_code = code;
        p->alive = 0;
        proc_release(p);
        kprintf("[exit] pid=%d code=%d\n", p->pid, code);
    } else {
        g_fb_exclusive = 0;      /* no proc to release, but free the panel anyway */
    }
    sched_exit_current();                 /* scheduler will never resume us */
    __asm__ volatile("sti");
    for (;;) __asm__ volatile("hlt");     /* park until the next tick reaps */
}
