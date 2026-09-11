#include <stdint.h>
#include "interrupts.h"
#include "kprintf.h"
#include "pic.h"
#include "apic.h"
#include "io.h"
#include "klog.h"
#include "sched.h"
#include "proc.h"

static irq_handler_t handlers[16];
static irq_handler_t msi_handlers[16];      /* vectors 0x70..0x7F */
static tick_hook_t   tick_hook;

void irq_install(int irq, irq_handler_t h) {
    if (irq >= 0 && irq < 16) handlers[irq] = h;
}

/* A PCI device's legacy interrupt. Installing the handler was never enough:
 * apic_init() masks every IO-APIC redirection entry and then unmasks exactly
 * four ISA lines, so a driver that registered an ISR for its PCI line got an
 * interrupt that could not be delivered. The e1000 had one for its receive
 * ring, which meant the machine only ever took packets off the card while
 * something was already polling -- so an idle BoltOS did not answer so much as
 * an ARP request, and nothing could open a connection to it. Registering the
 * handler and opening the line are the same decision, so they are one call. */
void irq_install_pci(int irq, irq_handler_t h) {
    if (irq < 0 || irq >= 16) return;
    handlers[irq] = h;
    if (apic_active()) ioapic_route_pci_irq((uint8_t)irq, (uint8_t)(32 + irq));
    /* Without an APIC the PIC is already unmasked across the board. */
}

void msi_install(uint8_t vector, irq_handler_t h) {
    if (vector >= MSI_VECTOR_BASE && vector < MSI_VECTOR_BASE + 16)
        msi_handlers[vector - MSI_VECTOR_BASE] = h;
}

void irq_set_tick_hook(tick_hook_t h) { tick_hook = h; }

/* ---------------------------------------------------------------------------
 *  Stuck interrupts.
 *
 *  A PCI INTx line is level-triggered and shared: several devices can pull the
 *  same one, and it stays asserted until whichever device pulled it is told to
 *  stop. Only the network drivers open a line at all, and there is one handler
 *  per line -- so if a device on the same line as the NIC asserts INTx, the
 *  NIC's handler runs, finds nothing of its own to acknowledge, and the line is
 *  still asserted when the interrupt is dismissed. It fires again immediately,
 *  and the machine does nothing else ever again.
 *
 *  The line is counted instead, and a line that fires absurdly often inside one
 *  millisecond is masked and reported. A device that stops working is a bad
 *  outcome; a machine that stops is a worse one, and "IRQ 11 is stuck" in the
 *  log is the difference between a diagnosis and a mystery.
 * ------------------------------------------------------------------------- */
#define IRQ_STORM_PER_TICK 10000

static uint32_t irq_count[16];
static int      irq_stuck[16];

static void irq_storm_check(void) {
    for (int i = 0; i < 16; i++) {
        if (irq_count[i] > IRQ_STORM_PER_TICK && !irq_stuck[i]) {
            irq_stuck[i] = 1;
            ioapic_mask_irq((uint8_t)i, 1);
            kprintf("[irq] line %d fired %u times in a millisecond and nothing "
                    "cleared it; masking it\n", i, (unsigned)irq_count[i]);
        }
        irq_count[i] = 0;
    }
}

static const char *exc[32] = {
    "Divide-by-zero", "Debug", "NMI", "Breakpoint", "Overflow",
    "BOUND range", "Invalid opcode", "Device not available", "Double fault",
    "Coproc segment", "Invalid TSS", "Segment not present", "Stack-segment",
    "General protection", "Page fault", "Reserved", "x87 FP",
    "Alignment check", "Machine check", "SIMD FP", "Virtualization",
    "Control protection", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Hypervisor", "VMM comm", "Security", "Reserved"
};

static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3,%0":"=r"(v)); return v; }

/* A frame pointer is only safe to dereference if it lands in mapped memory:
 * either identity-mapped low RAM or the higher-half kernel window (where this
 * kernel's stacks live). Anything else ends the walk rather than risking a
 * fault inside the dump that would triple-fault the machine. */
static int rbp_ok(uint64_t p) {
    if (p & 7) return 0;
    if (p >= 0x1000 && p < 0x100000000ull)   return 1;   /* low identity map */
    if (p >= 0xffffffff80000000ull)           return 1;   /* higher-half kernel */
    return 0;
}

static void backtrace(uint64_t rbp, uint64_t rip) {
    kprintf("    backtrace:\n      #0 0x%lx\n", rip);
    for (int i = 1; i <= 16 && rbp_ok(rbp); i++) {
        uint64_t next = ((uint64_t *)rbp)[0];
        uint64_t ret  = ((uint64_t *)rbp)[1];
        if (ret < 0x1000) break;
        kprintf("      #%d 0x%lx\n", i, ret);
        if (next <= rbp || next - rbp > 0x40000) break;   /* not a sane frame */
        rbp = next;
    }
}

/* Full crash dump: cause, every GPR, control/segment state, page-fault decode,
 * and a stack backtrace. Mirrors to screen and COM1, then halts. */
static void panic_dump(struct registers *r) {
    kprintf("\n\n================ KERNEL PANIC ================\n");
    kprintf("  CPU EXCEPTION #%lu", r->int_no);
    if (r->int_no < 32) kprintf(" (%s)", exc[r->int_no]);
    kprintf("   err=0x%lx\n", r->err_code);
    kprintf("  RIP=0x%016lx  CS=0x%lx  RFLAGS=0x%lx\n", r->rip, r->cs, r->rflags);
    kprintf("  RSP=0x%016lx  SS=0x%lx\n", r->rsp, r->ss);
    kprintf("  RAX=0x%016lx  RBX=0x%016lx\n", r->rax, r->rbx);
    kprintf("  RCX=0x%016lx  RDX=0x%016lx\n", r->rcx, r->rdx);
    kprintf("  RSI=0x%016lx  RDI=0x%016lx\n", r->rsi, r->rdi);
    kprintf("  RBP=0x%016lx  R8 =0x%016lx\n", r->rbp, r->r8);
    kprintf("  R9 =0x%016lx  R10=0x%016lx\n", r->r9,  r->r10);
    kprintf("  R11=0x%016lx  R12=0x%016lx\n", r->r11, r->r12);
    kprintf("  R13=0x%016lx  R14=0x%016lx  R15=0x%016lx\n", r->r13, r->r14, r->r15);
    kprintf("  CR2=0x%016lx  CR3=0x%016lx\n", read_cr2(), read_cr3());
    if (r->int_no == 14) {                                  /* page fault: decode err bits */
        uint64_t e = r->err_code;
        kprintf("  page fault: %s while %s in %s mode%s%s\n",
                (e & 1) ? "protection-violation" : "non-present page",
                (e & 2) ? "write" : "read",
                (e & 4) ? "user" : "kernel",
                (e & 8)  ? ", reserved-bit set" : "",
                (e & 16) ? ", instruction fetch" : "");
    }
    backtrace(r->rbp, r->rip);
    kprintf("  System halted.\n");
    kprintf("=============================================\n");
}

struct registers *isr_handler(struct registers *r) {
    if (r->int_no < 32) {
        /* A fault raised in ring 3 is the process's problem, not the machine's.
         * It used to take the whole kernel down with it, which meant one bad
         * pointer in the browser or a user program killed the desktop and
         * everything else running. Now the process is reported and reaped and
         * the machine carries on -- which is the difference between an OS you
         * can leave running and a demo.
         *
         * Ring 0 still panics: a kernel fault means kernel state is already
         * untrustworthy, and continuing would corrupt more than it saved. */
        if ((r->cs & 3) == 3 && sched_current_proc() && !crash_in_progress()) {
            struct registers *next = proc_fault_kill(r);
            if (next) return next;
        }
        if (!crash_in_progress())
            crash_record("kernel panic", 0, r);   /* persisted for the next boot */
        panic_dump(r);
        kprintf("  (crash log saved to %s)\n", CRASH_LOG_PATH);
        for (;;) __asm__ volatile("cli; hlt");
    }

    if (r->int_no >= 32 && r->int_no < 48) {
        int irq = (int)r->int_no - 32;
        irq_count[irq]++;
        if (handlers[irq]) handlers[irq](r);
        if (apic_active()) lapic_eoi(); else pic_eoi(irq);
        /* preemptive reschedule on the timer tick; may return a new frame */
        if (irq == 0) {
            irq_storm_check();
            if (tick_hook) r = tick_hook(r);
        }
    }

    /* Message-signalled interrupts (NVMe completions, xHCI events). Delivered
     * straight to the local APIC, so only it needs the EOI. */
    if (r->int_no >= MSI_VECTOR_BASE && r->int_no < MSI_VECTOR_BASE + 16) {
        irq_handler_t h = msi_handlers[r->int_no - MSI_VECTOR_BASE];
        if (h) h(r);
        if (apic_active()) lapic_eoi();
    }
    return r;
}
