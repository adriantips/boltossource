/* ===========================================================================
 *  BoltOS  -  kernel/sleep.c
 *  Suspend to RAM, and the considerably harder half: coming back.
 *
 *  See include/sleep.h for the shape of it. What is worth saying here is the
 *  two things that are easy to get wrong and give you a machine that sleeps
 *  beautifully and never wakes.
 *
 *  THE WAKING VECTOR IS A REAL-MODE ADDRESS. It goes in the FACS, it is 32
 *  bits wide, and the CPU arrives there in 16-bit real mode -- so it has to be
 *  below 1 MiB and it has to be code that expects no paging, no long mode and
 *  no GDT. The trampoline lives at physical 0x9000 and is copied there before
 *  sleeping rather than at boot, because nothing else needs it and low memory
 *  is scarce enough to leave alone until it is wanted.
 *
 *  DMA DOES NOT STOP BECAUSE THE CPU DID. A sound card halfway through a
 *  buffer, or a NIC with a descriptor ring armed, keeps its bus-master bit set
 *  across the sleep and comes back to a controller that has forgotten it ever
 *  existed. Quiescing before sleeping is not politeness; it is the difference
 *  between waking up and waking up with a device writing into memory it no
 *  longer owns.
 * ========================================================================= */
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include "sleep.h"
#include "acpi.h"
#include "apic.h"
#include "gdt.h"
#include "interrupts.h"
#include "pic.h"
#include "pit.h"
#include "hpet.h"
#include "io.h"
#include "mm.h"
#include "vmm.h"
#include "kheap.h"
#include "keyboard.h"
#include "mouse.h"
#include "framebuffer.h"
#include "gpu.h"
#include "gui.h"
#include "settings.h"
#include "audio.h"
#include "blk.h"
#include "hw.h"
#include "smp.h"
#include "syscall.h"
#include "string.h"
#include "kprintf.h"

/* Must match the ORG and PARAM in kernel/s3_wake.asm. */
#define WAKE_PHYS   0x9000
#define WAKE_PARAM  0x9F00

extern const uint8_t _binary_s3_wake_bin_start[];
extern const uint8_t _binary_s3_wake_bin_end[];

/* Where the C world comes back to. A jmp_buf holds the callee-saved registers,
 * the stack pointer and the return address, which is precisely the state a
 * function needs to be resumed -- everything else in the CPU is either
 * re-initialised below or was never the caller's to begin with. */
static jmp_buf   resume_to;
static uint32_t  suspends;
static const char *why = "";

/* The resume path runs before the kernel stack it came from is trustworthy --
 * the trampoline has no idea where that was -- so it gets its own. One page is
 * plenty for the handful of frames between the trampoline and the longjmp. */
static uint8_t resume_stack[8192] __attribute__((aligned(16)));

int         sleep_s3_ok(void);
uint32_t    sleep_count(void) { return suspends; }
const char *sleep_why(void)   { sleep_s3_ok(); return why; }

int sleep_s3_ok(void) {
    uint16_t a, b, pa, pb;
    (void)b; (void)pb;
    if (!acpi_sleep_type(3, &a, &b)) {
        why = "this firmware's DSDT declares no S3 state";
        return 0;
    }
    acpi_pm1_ports(&pa, &pb);
    if (!pa) {
        why = "the FADT gives no PM1a control register";
        return 0;
    }
    if (acpi_set_waking_vector(0) != 0) {
        why = "there is no FACS, so there is nowhere to leave a waking vector";
        return 0;
    }
    why = "";
    return 1;
}

/* One byte at the serial port, the same way the trampoline does it and for the
 * same reason: everything below runs before there is an IDT, so a fault here is
 * a triple fault and a reset, and the only evidence of where it happened is
 * what has already left the machine. kprintf is not available -- it goes
 * through the console, which goes through the framebuffer, which has state. */
static void mark(char c) { outb(0x3F8, (uint8_t)c); }

/* ------------------------------------------------------------------ back --
 *  The trampoline's target. Long mode is live and CR3 is the kernel's, and
 *  that is the entire extent of what is true: this runs on its own stack with
 *  the trampoline's throwaway GDT, no IDT, no local APIC, no GS base, and
 *  interrupts off. Everything below puts one more thing back, in the order
 *  kmain does it, because that order was chosen for the same reasons.
 * ------------------------------------------------------------------------*/
void s3_resume64(void);
void s3_resume64(void) {
    mark('g');
    gdt_init();          /* our own descriptors, replacing the trampoline's */
    mark('G');

    /* Immediately after, and this one cost an afternoon: loading the GS
     * *selector* in long mode -- which gdt_init() does along with every other
     * segment register -- sets GS_BASE from the descriptor, which is zero. So
     * gdt_init() has just thrown away the per-CPU pointer, and the next thing
     * to read gs:0 triple-faults into a reboot that looks exactly like a
     * resume that never happened. */
    smp_reload_gs_base();

    mark('i');
    idt_init();          /* somewhere for a fault to go, at last */

    /* Before any driver looks at a register. Every PCI device came back at
     * address zero with its decoders off, and a driver that reads a status
     * word off an unassigned BAR gets all-ones -- which NVMe reported as
     * CSTS.CFS, "controller fatal status", for a controller nobody had
     * touched yet. */
    mark('c');
    pci_restore_state();
    mark('p');
    pic_init();          /* the 8259s come back unmasked and unremapped */
    pit_init(1000);
    mark('a');
    apic_init();         /* masks the PICs again, re-routes the IO-APIC */
    mark('h');
    hpet_init();
    mark('s');
    syscall_init();      /* LSTAR, STAR, SFMASK, EFER.SCE -- all MSRs, all gone */

    /* The PS/2 controller comes back with a byte in its output buffer often
     * enough that keyboard_init()'s drain is the reason it is called here
     * rather than left alone. Without it the first keypress after a resume
     * goes nowhere, and so does every one after that. */
    mark('k');
    keyboard_init();
    mouse_init((int)fb_width(), (int)fb_height());

    /* And the disks, before anything tries to use one.
     *
     * This is the step that turns S3 from a demonstration into something you
     * would leave a machine in. The controllers lost their PCI configuration
     * and every register that pointed at a queue or a command list, while the
     * queues themselves sat in RAM untouched -- so the first write after a
     * resume went into a controller that had forgotten it existed, and the
     * filesystem commit on the *next* suspend waited for a completion that was
     * never coming. Which is to say the machine suspended beautifully once and
     * then hung. */
    mark('d');
    blk_resume_all();

    mark('R');
    suspends++;
    longjmp(resume_to, 1);
}

/* ----------------------------------------------------------------- there -- */

/* Put the picture back.
 *
 * This is settings_apply(), which is what `display res` calls, and not a
 * hand-rolled modeset -- because the hand-rolled one did not work and that one
 * does. Restoring the adapter's BAR puts the framebuffer back at the address
 * the compositor has been writing to all along, and re-programming DISPI puts
 * the mode back, and after both of those the screen was still black while the
 * guest's own `screenshot` came back with a complete picture of a desktop
 * nobody could see. Typing `display res 1024x768` fixed it every time.
 *
 * So rather than keep guessing at the difference, this takes the same route:
 * settings_apply() re-programs the mode, recomputes the logical desktop and
 * the letterbox, re-renders the wallpaper and repaints. Some part of that is
 * what the adapter needs, and the honest thing is to use the sequence that is
 * already proven rather than a shorter one that is nearly it.
 */
static void video_restore(void) {
    if (!gpu_present()) return;
    settings_apply();
    gui_request_redraw();
}

/* Stop everything that might still be moving bytes on its own. */
static void quiesce(void) {
    audio_capture_close();
    audio_stop();
}

int sleep_suspend(void) {
    if (!sleep_s3_ok()) return -1;

    uint16_t slp_a, slp_b, pm1a, pm1b;
    acpi_sleep_type(3, &slp_a, &slp_b);
    acpi_pm1_ports(&pm1a, &pm1b);

    /* Copy the trampoline down and fill in its parameter block. Done here and
     * not at boot because low memory is worth leaving alone until it is
     * wanted, and because the AP trampoline may still be sitting at 0x8000. */
    uint64_t len = (uint64_t)(_binary_s3_wake_bin_end - _binary_s3_wake_bin_start);
    if (len > 0x0F00) {                       /* would run into the param block */
        why = "the resume trampoline does not fit below its parameter block";
        return -1;
    }
    memcpy(P2V(WAKE_PHYS), _binary_s3_wake_bin_start, len);
    volatile uint64_t *param = (volatile uint64_t *)P2V(WAKE_PARAM);
    param[0] = vmm_kernel_pml4();
    param[1] = (uint64_t)(uintptr_t)(resume_stack + sizeof resume_stack) & ~0xFull;
    param[2] = (uint64_t)(uintptr_t)&s3_resume64;

    if (acpi_set_waking_vector(WAKE_PHYS) != 0) {
        why = "the FACS would not take a waking vector";
        return -1;
    }

    kprintf("[s3] suspending: SLP_TYP %u on PM1a 0x%x, waking vector 0x%x\n",
            (unsigned)(slp_a >> 10), pm1a, WAKE_PHYS);

    quiesce();
    pci_save_state();

    if (setjmp(resume_to) == 0) {
        cli();
        /* CMOS shutdown status = 0xFE.
         *
         * This is the PC's oldest and least documented handshake. Waking from
         * S3 is, from the hardware's point of view, a reset: the firmware runs
         * from the reset vector with no memory of anything. The byte at CMOS
         * index 0x0F is how it is told that this particular reset is a resume
         * and that it should read the FACS and jump, rather than do a full
         * POST and boot something. Without it the machine wakes up, forgets,
         * and boots the OS again from disk -- which looks exactly like a
         * suspend that never came back. */
        outb(0x70, 0x0F);
        outb(0x71, 0xFE);
        /* Write it twice if there are two registers -- the b register is a
         * second half of the same command on a chipset that has one, and
         * skipping it on a machine that needs it is a sleep that does not. */
        outw(pm1a, (uint16_t)(slp_a | (1 << 13)));      /* SLP_EN */
        if (pm1b) outw(pm1b, (uint16_t)(slp_b | (1 << 13)));

        /* The write is posted; the machine stops somewhere in the next few
         * instructions. If it is still here after that, S3 did not take -- and
         * the honest thing is to come back rather than hlt forever in a
         * function whose caller is waiting. */
        for (volatile int i = 0; i < 1000000; i++) __asm__ volatile("pause");
        sti();
        why = "the firmware accepted SLP_EN and stayed awake";
        kprintf("[s3] the machine did not sleep (%s)\n", why);
        return -1;
    }

    /* ---- resumed ---- */
    sti();

    /* The picture, last, and deliberately out here rather than in
     * s3_resume64() with the rest.
     *
     * Restoring the display's BAR puts the framebuffer back at the address the
     * compositor has been writing to all along, but the adapter's *mode*
     * registers were reset too: it comes back in text mode and every one of
     * those writes lands in VRAM nobody is scanning out. The serial console
     * looks perfect throughout, which is a particularly unhelpful kind of
     * half-working -- the machine is fine and the screen is black.
     *
     * Doing it in the bare resume path did not stick reliably. The adapter had
     * only just had its configuration space written back and a modeset issued
     * microseconds later took about half the time; up here there is a working
     * clock to wait on and a readback to check, so it can simply be retried. */
    video_restore();
    kprintf("[s3] resumed (suspend #%u). The application processors did not\n"
            "     come back -- they need a fresh INIT-SIPI-SIPI and nothing\n"
            "     here sends one, so this is the boot processor alone.\n",
            (unsigned)suspends);
    return 0;
}
