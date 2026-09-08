#include <stdint.h>
#include "commands.h"
#include "core.h"
#include "elf.h"
#include "fs.h"
#include "kprintf.h"
#include "string.h"
#include <stdio.h>

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_core.c
 *  `core` -- the post-mortem inspector.
 *
 *  A crash report of bare hex addresses is a puzzle. The same report with the
 *  function names in it is a bug you can go and fix, and every piece needed to
 *  turn one into the other is already on the machine: the trap frame and the
 *  stack are in the dump, and the program's own .symtab is in the file it was
 *  loaded from.
 *
 *  So `core show` walks the saved frame chain, resolves every return address
 *  through elf_symbolize(), and prints the stack next to it. Nothing here
 *  touches the live machine -- it reads two files.
 * ===========================================================================*/

static void usage(void) {
    kprintf("usage: core [list | show FILE | rm FILE|all]\n"
            "  list          the dumps in " CORE_DIR " (the default)\n"
            "  show FILE     registers, the fault, a named backtrace, the stack\n"
            "  rm FILE|all   delete one dump, or all of them\n"
            "\n"
            "A dump is written whenever a ring-3 program faults; the newest %d\n"
            "are kept. FILE may be given bare or as a full path.\n", CORE_KEEP);
}

/* Accept "hello.3.core" as well as "/var/crash/hello.3.core". */
static fs_node *open_dump(const char *name, char *path, uint32_t cap) {
    if (name[0] == '/') snprintf(path, cap, "%s", name);
    else                snprintf(path, cap, CORE_DIR "/%s", name);
    fs_node *f = fs_lookup(path);
    if (!f || f->is_dir) return 0;
    if (!fs_may(f, FS_PERM_READ)) return 0;
    return f;
}

static const core_hdr_t *header_of(fs_node *f) {
    if (!f->data || f->size < sizeof(core_hdr_t)) return 0;
    const core_hdr_t *h = (const core_hdr_t *)f->data;
    if (memcmp(h->magic, CORE_MAGIC, 8)) return 0;
    if (h->version != CORE_VERSION) return 0;
    if ((uint64_t)h->stack_len + sizeof(core_hdr_t) > f->size) return 0;
    return h;
}

static const char *exc_name(uint64_t v) {
    switch (v) {
    case 0:  return "divide by zero";
    case 3:  return "breakpoint";
    case 6:  return "invalid opcode";
    case 8:  return "double fault";
    case 13: return "general protection fault";
    case 14: return "page fault";
    default: return "fault";
    }
}

static int core_list(void) {
    fs_node *d = fs_lookup(CORE_DIR);
    int n = 0;
    if (d && d->is_dir) {
        for (fs_node *c = d->child; c; c = c->next) {
            if (c->is_dir) continue;
            const core_hdr_t *h = header_of(c);
            if (!h) { kprintf("  %-28s (not a core file)\n", c->name); n++; continue; }
            kprintf("  %-28s pid %-4u  %s  %s\n",
                    c->name, h->pid, h->when, exc_name(h->regs.int_no));
            n++;
        }
    }
    if (!n) kprintf("no core dumps in " CORE_DIR "\n");
    return 0;
}

/* The program's own file, so its symbols can be read. */
static fs_node *exe_of(const core_hdr_t *h) {
    fs_node *e = fs_lookup(h->exe);
    if (!e || e->is_dir || !e->data) return 0;
    return e;
}

static void print_addr(fs_node *exe, uint64_t addr) {
    char sym[96];
    if (exe && elf_symbolize(exe->data, exe->size, addr, sym, sizeof sym))
        kprintf("0x%016lx  %s\n", addr, sym);
    else
        kprintf("0x%016lx\n", addr);
}

/* Read 8 bytes out of the captured stack, if that address is in it. */
static int stack_read(const core_hdr_t *h, const uint8_t *stack, uint64_t addr,
                      uint64_t *out) {
    if (addr < h->stack_base) return 0;
    uint64_t off = addr - h->stack_base;
    if (off + 8 > h->stack_len) return 0;
    memcpy(out, stack + off, 8);
    return 1;
}

static int core_show(const char *name) {
    char path[160];
    fs_node *f = open_dump(name, path, sizeof path);
    if (!f) { kprintf("core: cannot read %s\n", name); return 1; }
    const core_hdr_t *h = header_of(f);
    if (!h) { kprintf("core: %s is not a BoltOS core dump\n", path); return 1; }
    const uint8_t *stack = f->data + sizeof(core_hdr_t);
    const struct registers *r = &h->regs;

    kprintf("core %s\n", path);
    kprintf("  program  %s   pid %u\n", h->exe, h->pid);
    kprintf("  when     %s\n", h->when);
    kprintf("  fault    vector %lu (%s)  err=0x%lx\n",
            r->int_no, exc_name(r->int_no), r->err_code);
    if (r->int_no == 14) {
        kprintf("           address 0x%lx: %s%s%s\n", h->cr2,
                (r->err_code & 1) ? "protection violation" : "non-present page",
                (r->err_code & 2) ? " on write" : " on read",
                (r->err_code & 16) ? ", instruction fetch" : "");
    }

    fs_node *exe = exe_of(h);
    if (!exe) kprintf("  (%s is gone, so frames cannot be named)\n", h->exe);

    kprintf("\n  registers\n");
    kprintf("    RIP 0x%016lx  RSP 0x%016lx\n", r->rip, r->rsp);
    kprintf("    RAX 0x%016lx  RBX 0x%016lx\n", r->rax, r->rbx);
    kprintf("    RCX 0x%016lx  RDX 0x%016lx\n", r->rcx, r->rdx);
    kprintf("    RSI 0x%016lx  RDI 0x%016lx\n", r->rsi, r->rdi);
    kprintf("    RBP 0x%016lx  R8  0x%016lx\n", r->rbp, r->r8);
    kprintf("    R9  0x%016lx  R10 0x%016lx\n", r->r9,  r->r10);
    kprintf("    R11 0x%016lx  R12 0x%016lx\n", r->r11, r->r12);
    kprintf("    R13 0x%016lx  R14 0x%016lx\n", r->r13, r->r14);
    kprintf("    R15 0x%016lx  CS  0x%lx  RFLAGS 0x%lx\n", r->r15, r->cs, r->rflags);

    /* The frame chain. Ring-3 programs are built with frame pointers kept
     * precisely so this walk is possible; each frame holds the caller's RBP and
     * the return address into the caller. */
    kprintf("\n  backtrace\n    #0  ");
    print_addr(exe, r->rip);
    uint64_t rbp = r->rbp;
    int depth = 1;
    for (; depth < 24; depth++) {
        uint64_t next = 0, ret = 0;
        if (!stack_read(h, stack, rbp, &next)) break;
        if (!stack_read(h, stack, rbp + 8, &ret)) break;
        if (ret < 0x1000) break;
        kprintf("    #%-2d ", depth);
        print_addr(exe, ret);
        if (next <= rbp) break;                  /* stacks grow one way only */
        rbp = next;
    }
    if (depth == 1)
        kprintf("    (no further frames: the frame pointer is outside the saved stack)\n");

    /* And the raw stack, which is where the arguments and locals are. */
    kprintf("\n  stack  %u bytes from 0x%lx\n", h->stack_len, h->stack_base);
    uint32_t show = h->stack_len < 256 ? h->stack_len : 256;
    for (uint32_t o = 0; o + 8 <= show; o += 8) {
        uint64_t v;
        memcpy(&v, stack + o, 8);
        kprintf("    0x%016lx  0x%016lx", h->stack_base + o, v);
        char sym[96];
        if (exe && v >= 0x1000 && elf_symbolize(exe->data, exe->size, v, sym, sizeof sym))
            kprintf("  %s", sym);
        kputc('\n');
    }
    if (show < h->stack_len)
        kprintf("    ... %u more bytes\n", h->stack_len - show);
    return 0;
}

static int core_rm(const char *name) {
    if (!strcmp(name, "all")) {
        fs_node *d = fs_lookup(CORE_DIR);
        int n = 0;
        if (d && d->is_dir) {
            for (;;) {
                fs_node *c = d->child;
                while (c && c->is_dir) c = c->next;
                if (!c) break;
                fs_remove_node(c);
                n++;
            }
        }
        fs_sync();
        kprintf("core: removed %d dump%s\n", n, n == 1 ? "" : "s");
        return 0;
    }
    char path[160];
    fs_node *f = open_dump(name, path, sizeof path);
    if (!f) { kprintf("core: no dump %s\n", name); return 1; }
    fs_remove_node(f);
    fs_sync();
    kprintf("core: removed %s\n", path);
    return 0;
}

int cmd_core(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "list")) return core_list();
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) { usage(); return 0; }
    if (!strcmp(argv[1], "show")) {
        if (argc < 3) { kprintf("usage: core show FILE\n"); return 1; }
        return core_show(argv[2]);
    }
    if (!strcmp(argv[1], "rm")) {
        if (argc < 3) { kprintf("usage: core rm FILE|all\n"); return 1; }
        return core_rm(argv[2]);
    }
    /* `core NAME` is what people type; treat it as `core show NAME`. */
    return core_show(argv[1]);
}
