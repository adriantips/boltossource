/* ===========================================================================
 *  BoltOS TV  -  tv/main.c   (kmain)
 *
 *  The desktop's kmain brings up the filesystem, user accounts, the network
 *  and audio, prints a page of boot log and then starts a login screen. A
 *  television does none of that in front of anybody: the hardware that the
 *  picture depends on comes up here, silently, and the shell takes the panel
 *  within a second or so. The network, which can take several seconds to get
 *  a lease and a clock, comes up behind the home screen on a thread of its
 *  own (tv/netsvc.c).
 * ===========================================================================*/
#include <stdint.h>
#include "boot.h"
#include "io.h"
#include "kprintf.h"
#include "buildid.h"
#include "serial.h"
#include "framebuffer.h"
#include "gdt.h"
#include "interrupts.h"
#include "pic.h"
#include "pit.h"
#include "acpi.h"
#include "apic.h"
#include "hpet.h"
#include "timeout.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "keyboard.h"
#include "mouse.h"
#include "xhci.h"
#include "ata.h"
#include "ahci.h"
#include "nvme.h"
#include "sched.h"
#include "audio.h"
#include "hw.h"
#include "keymap.h"
#include "kbdlayout.h"
#include "tv.h"

_Static_assert(sizeof(struct bootinfo) == 40, "boot.asm copies BOOTINFO_SZ bytes");

void stackguard_seed(void);
void tv_run(void);

/* boot_flags() and friends are asked by shared code; the TV build has one
 * mode, but the loader's word is kept so a UEFI boot still reads as one. */
static uint32_t g_boot_flags;
uint32_t boot_flags(void)       { return g_boot_flags; }
int      boot_is_safe(void)     { return 0; }
int      boot_is_recovery(void) { return 0; }
const char *boot_mode_name(void) { return "tv"; }

static void cpu_harden(void) {
    uint32_t a, b, c, d;
    cpuidx(7, 0, &a, &b, &c, &d);
    uint64_t cr4;
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    if ((b >> 7) & 1) cr4 |= (1ull << 20);       /* SMEP */
    if ((c >> 2) & 1) cr4 |= (1ull << 11);       /* UMIP */
    __asm__ volatile("mov %0,%%cr4" :: "r"(cr4) : "memory");
}

void kmain(struct bootinfo *bi) {
    stackguard_seed();
    g_boot_flags = bi->boot_flags;

    serial_init();
    fb_init(bi);
    /* Whatever the loader left on the panel goes, now: a black screen while
     * the hardware comes up reads as "switching on"; a text console does not. */
    if (fb_present()) fb_fill(0x000000);

    kprintf("==================================================\n");
    kprintf("   BoltOS TV %s  -  build %s\n", BOLT_VERSION, BOLT_BUILD_ID);
    kprintf("==================================================\n");
    if (fb_present())
        kprintf("[ok] panel %ux%u @0x%lx (%s)\n", fb_width(), fb_height(), bi->fb_addr,
                (g_boot_flags & BOOT_UEFI) ? "UEFI GOP" : "VBE");
    else
        kprintf("[--] no framebuffer: BoltOS TV has nothing to show a picture on\n");

    gdt_init();
    idt_init();
    pic_init();
    pmm_init(bi);
    vmm_init();
    kheap_init();
    { uint64_t hb, hs; kheap_window(&hb, &hs);
      kprintf("[ok] memory: kernel heap %lu MiB\n", hs >> 20); }
    cpu_harden();

    acpi_init();
    pit_init(1000);
    timeout_init();
    int ps2k = keyboard_init();
    int ps2m = fb_present() ? mouse_init((int)fb_width(), (int)fb_height()) : 0;
    apic_init();
    hpet_init();
    sti();

    /* The disks, for updates only: the stick BoltOS TV started from is where
     * a new kernel goes (kernel/sysupdate.c). Nothing else touches them. */
    ata_init();
    ahci_init();
    nvme_init();
    xhci_init();
    {
        int uk = 0, um = 0;
        xhci_hid_counts(&uk, &um);
        kprintf("[%s] remote/keyboard: %d PS/2 + %d USB; pointer: %d PS/2 + %d USB\n",
                (ps2k + uk) ? "ok" : "--", ps2k, uk, ps2m, um);
    }
    keymap_init();
    kbd_layout_init();
    audio_init();
    kprintf("[%s] sound: %s\n", audio_present() ? "ok" : "--",
            audio_present() ? audio_name() : "no codec");

    sched_init();

    if (!fb_present()) {
        kprintf("[--] halting: no display\n");
        for (;;) __asm__ volatile("hlt");
    }
    tv_run();
}
