/* ===========================================================================
 *  BoltOS  -  kernel/main.c   (kmain)
 *  Brings up the framebuffer console first so the whole boot log renders on
 *  screen, then the core kernel subsystems, then the idle loop.
 *  All output also mirrors to COM1 serial.
 * ===========================================================================*/
#include <stdint.h>
#include "boot.h"
#include "io.h"
#include "kprintf.h"
#include "serial.h"
#include "console.h"
#include "framebuffer.h"
#include "gpu.h"
#include "gdt.h"
#include "interrupts.h"
#include "pic.h"
#include "pit.h"
#include "acpi.h"
#include "apic.h"
#include "hpet.h"
#include "smp.h"
#include "pmm.h"
#include "vmm.h"
#include "mm.h"
#include "syscall.h"
#include "kheap.h"
#include "keyboard.h"
#include "mouse.h"
#include "fs.h"
#include "ata.h"
#include "ahci.h"
#include "blk.h"
#include "nvme.h"
#include "xhci.h"
#include "sysreg.h"
#include "sched.h"
#include "shell.h"
#include "gui.h"
#include "netif.h"
#include "vfs.h"
#include "proc.h"
#include "fat32.h"
#include "ext2.h"
#include "audio.h"
#include "dhcp.h"
#include "netcfg.h"
#include "ntp.h"
#include "hw.h"
#include "users.h"
#include "keymap.h"

/* kernel/boot.asm copies bootinfo and its E820 table into .data before it
 * zeroes .bss, using these offsets spelled out as constants. stage2.asm writes
 * the same ones. A change to the struct should break the build here rather
 * than produce a machine that boots with no memory map. */
_Static_assert(sizeof(struct bootinfo) == 40, "boot.asm copies BOOTINFO_SZ bytes");
_Static_assert(__builtin_offsetof(struct bootinfo, e820_count) == 24, "BI_E820_COUNT");
_Static_assert(__builtin_offsetof(struct bootinfo, e820_addr)  == 28, "BI_E820_ADDR");
#include "ttf.h"
#include "string.h"
#include "kprintf.h"

/* Turn on the CPU's supervisor-mode protections where the silicon offers them:
 *   SMEP - faults if ring 0 ever executes from a user page (blocks a whole
 *          class of privilege-escalation exploits).
 *   UMIP - makes SGDT/SIDT/SLDT/SMSW/STR fault in user mode, hiding kernel
 *          descriptor addresses from ring 3.
 * SMAP is deliberately left off: the syscall path reads user buffers directly
 * and would need STAC/CLAC bracketing first. CPUID leaf 7 reports support. */
static void cpu_harden(void) {
    uint32_t a, b, c, d;
    cpuidx(7, 0, &a, &b, &c, &d);
    uint64_t cr4; __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    int smep = (b >> 7)  & 1;
    int umip = (c >> 2)  & 1;
    if (smep) cr4 |= (1ull << 20);
    if (umip) cr4 |= (1ull << 11);
    if (smep || umip) __asm__ volatile("mov %0,%%cr4" :: "r"(cr4) : "memory");
    kprintf("[ok] CPU hardening: SMEP=%s UMIP=%s\n", smep ? "on" : "n/a", umip ? "on" : "n/a");
}

/* Demo kernel threads: proof that the scheduler really preempts. Each one wakes
 * once a second and bumps its own counter; `ps` prints the counters next to the
 * kernel's context-switch total, so the evidence is on demand rather than
 * streamed. (They used to write 'A' and 'B' straight to the serial console once
 * a second, which interleaved with anything the user was typing at the text
 * shell -- a prompt would come back reading "wriAte foo".) hlt parks the CPU
 * until the next timer tick, which is also when the scheduler runs. */
static uint64_t demo_beats[2];

uint64_t demo_thread_beats(int which) {
    return (which >= 0 && which < 2) ? demo_beats[which] : 0;
}

static void demo_beat(int which) {
    uint64_t last = 0;
    for (;;) {
        if (pit_ticks() - last >= 1000) { last = pit_ticks(); demo_beats[which]++; }
        __asm__ volatile("hlt");
    }
}
static void demo_thread_a(void) { demo_beat(0); }
static void demo_thread_b(void) { demo_beat(1); }

/* ---------------------------------------------------------------------------
 *  Watchdog. Wakes once a second and looks for a runnable thread that has not
 *  been given the CPU for several seconds. Round-robin over a handful of
 *  threads should never starve anyone, so a hit means a thread is wedged inside
 *  an interrupt-disabled region -- a driver spinning on a register that will
 *  never change, say. The kernel cannot preempt its way out of that, but saying
 *  so out loud turns "the machine froze" into a diagnosable event, and the
 *  report lands in the crash log with everything else.
 *
 *  It also watches the heap: crossing 90% used is the last chance to say
 *  something before allocations start failing in places that cannot report it.
 * ------------------------------------------------------------------------- */
#define WATCHDOG_STARVE_TICKS  5000     /* ~5 s at 1000 Hz */

static void watchdog_thread(void) {
    uint64_t last = 0;
    int warned_slot = -1, warned_heap = 0;
    for (;;) {
        if (pit_ticks() - last >= 1000) {
            last = pit_ticks();

            int slot = sched_starved(WATCHDOG_STARVE_TICKS);
            if (slot >= 0 && slot != warned_slot) {
                warned_slot = slot;
                kprintf("[watchdog] thread %d (%s) has not run for %lu ticks; "
                        "it is probably wedged with interrupts off\n",
                        slot, sched_name(slot), (unsigned long)sched_idle_ticks(slot));
            } else if (slot < 0) {
                warned_slot = -1;
            }

            uint64_t used = 0, total = 0;
            kheap_usage(&used, &total);
            if (total && used * 100 / total >= 90) {
                if (!warned_heap) {
                    warned_heap = 1;
                    kprintf("[watchdog] kernel heap %lu%% full (%lu of %lu KiB)\n",
                            (unsigned long)(used * 100 / total),
                            (unsigned long)(used >> 10), (unsigned long)(total >> 10));
                }
            } else {
                warned_heap = 0;
            }
        }
        __asm__ volatile("hlt");
    }
}

/* Ring-3 user program. The build cross-compiles user/hello.c into a static
 * ET_EXEC ELF and embeds the raw bytes here. We seed it into the filesystem at
 * /bin/hello, then exec it through the ELF64 loader: a fresh address space is
 * built, the PT_LOAD segments are mapped, a SysV entry stack is laid out, and
 * the program is scheduled as a ring-3 thread with its own fd table + heap. */
extern const uint8_t _binary_hello_elf_start[];
extern const uint8_t _binary_hello_elf_end[];
extern const uint8_t _binary_js_elf_start[];
extern const uint8_t _binary_js_elf_end[];
extern const uint8_t _binary_browser_elf_start[];
extern const uint8_t _binary_browser_elf_end[];
extern const uint8_t _binary_crashme_elf_start[];
extern const uint8_t _binary_crashme_elf_end[];
extern const uint8_t _binary_netget_elf_start[];
extern const uint8_t _binary_netget_elf_end[];

/* Write an embedded ELF blob into the filesystem at `path` so proc_exec can load
 * it. /bin is created on first use. Returns 0 on success. */
static int seed_blob(const char *path, const uint8_t *start, const uint8_t *end) {
    uint64_t len = (uint64_t)(end - start);
    fs_node *n = fs_lookup(path);
    if (!n) { fs_create("/bin", 1); n = fs_create(path, 0); }
    if (!n || fs_write(n, start, (uint32_t)len) != 0) {
        kprintf("[--] could not seed %s\n", path);
        return -1;
    }
    kprintf("[ok] seeded %s (%lu bytes)\n", path, len);
    return 0;
}

static void launch_user_demo(void) {
    syscall_init();   kprintf("[ok] SYSCALL/SYSRET armed (LSTAR + EFER.SCE)\n");

    /* Ring-3 programs: the demo, the JavaScript interpreter, and the web browser
     * (DOM + layout + JS). The latter two are seeded for `js`/`webx` to exec on
     * demand -- all three run in ring 3 through the ELF loader + syscalls. */
    seed_blob("/bin/js",      _binary_js_elf_start,      _binary_js_elf_end);
    seed_blob("/bin/browser", _binary_browser_elf_start, _binary_browser_elf_end);
    seed_blob("/bin/crashme", _binary_crashme_elf_start, _binary_crashme_elf_end);
    seed_blob("/bin/netget",  _binary_netget_elf_start,  _binary_netget_elf_end);
    if (seed_blob("/bin/hello", _binary_hello_elf_start, _binary_hello_elf_end) != 0)
        return;

    if (proc_exec("/bin/hello") < 0)
        kprintf("[--] proc_exec(/bin/hello) failed\n");
}

void stackguard_seed(void);   /* kernel/stackguard.c: re-seed the stack canary */

/* ---------------------------------------------------------------------------
 *  Boot mode. The bootloader's menu (stage2.asm / uefi_boot.c) hands its choice
 *  down in bootinfo.boot_flags; the rest of the kernel asks for it here rather
 *  than keeping its own copy of the bootinfo pointer, which is only valid until
 *  the PMM reclaims low memory.
 * ------------------------------------------------------------------------- */
static uint32_t g_boot_flags;

uint32_t boot_flags(void)     { return g_boot_flags; }
int      boot_is_safe(void)   { return (g_boot_flags & (BOOT_SAFE | BOOT_RECOVERY)) != 0; }
int      boot_is_recovery(void) { return (g_boot_flags & BOOT_RECOVERY) != 0; }
const char *boot_mode_name(void) {
    if (g_boot_flags & BOOT_RECOVERY) return "recovery";
    if (g_boot_flags & BOOT_SAFE)     return "safe";
    return "normal";
}

void kmain(struct bootinfo *bi) {
    stackguard_seed();   /* before any other call, so all frames see one value */
    g_boot_flags = bi->boot_flags;
    serial_init();
    fb_init(bi);
    console_init();          /* paints background + taskbar, starts text console */

    kprintf("==================================================\n");
    kprintf("   BoltOS kernel  -  64-bit long mode\n");
    kprintf("==================================================\n");
    kprintf("[ok] boot mode: %s%s\n", boot_mode_name(),
            (g_boot_flags & BOOT_FSCK) ? " (+fsck)" : "");
    if (fb_present())
        kprintf("[ok] framebuffer %ux%u @0x%lx (32bpp)\n",
                fb_width(), fb_height(), bi->fb_addr);
    else
        kprintf("[--] no framebuffer; serial only\n");

    gpu_init();   /* identify the display adapter + VRAM behind the framebuffer */

    gdt_init();   kprintf("[ok] GDT + TSS loaded\n");
    idt_init();   kprintf("[ok] IDT (256 vectors)\n");
    pic_init();   kprintf("[ok] PIC remapped to 0x20-0x2F\n");

    pmm_init(bi);
    vmm_init();   kprintf("[ok] VMM online (kernel PML4 @0x%lx)\n", vmm_kernel_pml4());
    kheap_init(); kprintf("[ok] kernel heap online (96 MiB @ 16 MiB)\n");
    cpu_harden(); /* SMEP/UMIP supervisor protections where supported */

    /* VMM self-test: map a fresh user page in a scratch address space and read
     * the translation back, then tear it down. */
    {
        uint64_t as = vmm_new_address_space();
        uint64_t pg = pmm_alloc_frame();
        vmm_map(as, 0x400000, pg, PTE_PRESENT | PTE_WRITE | PTE_USER);
        kprintf("[vmm] map 0x400000 -> 0x%lx, lookup -> 0x%lx %s\n",
                pg, vmm_get_phys(as, 0x400000),
                vmm_get_phys(as, 0x400000) == pg ? "OK" : "FAIL");
        vmm_unmap(as, 0x400000);
        pmm_free_frame(pg);
    }

    acpi_init();     /* parse ACPI tables (RSDP/XSDT/FADT/MADT/HPET) */

    pit_init(1000);  kprintf("[ok] PIT @ 1000 Hz\n");
    keyboard_init(); kprintf("[ok] PS/2 keyboard\n");
    if (fb_present()) { mouse_init((int)fb_width(), (int)fb_height()); kprintf("[ok] PS/2 mouse\n"); }

    /* Hand interrupt delivery to the APIC: masks the 8259s, routes PS/2 lines
     * through the IO-APIC, and runs the scheduler tick off the local-APIC timer.
     * Falls back to the PIC/PIT path if no ACPI/APIC is present. */
    apic_init();
    hpet_init();     /* high-resolution monotonic clock for fine delays */

    sti();
    kprintf("[ok] interrupts enabled\n");

    smp_init();      /* bring application processors online (INIT-SIPI-SIPI) */

    ata_init();      /* probe ATA disks (HDD/SSD) before the FS attaches */
    ahci_init();     /* probe AHCI/SATA controller; registers disks into block layer */
    nvme_init();     /* probe NVMe; registers its namespace into the block layer */
    xhci_init();     /* probe xHCI USB controller, enumerate attached devices */
    fs_init();       kprintf("[ok] ramfs mounted (/)\n");
    fs_persist_init();/* back the tree with a real disk + load saved image */
    vfs_init();      kprintf("[ok] VFS (ramfs + /dev + /proc)\n");

    /* FAT32: a mount failure says nothing about whether a disk is empty.
     * Only initialise a verified-empty scratch disk, and never in recovery or
     * on the disk BoltFS has already attached. */
    for (int i = 0; i < blk_count(); i++) {
        blkdev_t *d = blk_get(i);
        if (d->is_boot || d == fs_persist_disk() ||
            d->name[0] != 's' || d->name[1] != 'd') continue;
        if (fat32_mount(d) != 0) {
            if (!boot_is_recovery() && blk_is_blank_scratch(d) &&
                fat32_format(d, "BOLTFAT") == 0) {
                const char *welcome =
                    "BoltOS FAT32 volume.\r\n"
                    "Read/written by the in-kernel FAT32 driver, and by any PC.\r\n";
                fat32_write("/README.TXT", welcome, (uint32_t)strlen(welcome));
                fat32_mkdir("/DOCS");
            }
        }
        if (fat32_mounted()) break;
    }
    /* ext2: mount the first non-boot disk that carries an ext2 superblock, so a
     * Linux-formatted volume (e.g. a USB stick) can be read. Skipped for the
     * BoltFS/FAT disks above. */
    for (int i = 0; i < blk_count(); i++) {
        blkdev_t *d = blk_get(i);
        if (d->is_boot) continue;
        if (ext2_mount(d) == 0) break;
    }

    proc_init();
    users_init();    kprintf("[ok] user accounts (%d) loaded\n", users_count());
    /* After the filesystem, because the bindings live in /etc/keys.conf. */
    keymap_init();   kprintf("[ok] %d keyboard shortcuts\n", keymap_count());
    ttf_init();      /* parse the embedded TrueType font for scalable text */
    sysreg_init();   kprintf("[ok] service + task registry\n");
    /* Safe mode exists for the boot that would otherwise never finish: a NIC
     * whose probe hangs, a codec that wedges, a desktop that faults on its first
     * composite. Everything skipped here can still be started by hand from the
     * shell once the user is in. */
    if (boot_is_safe()) {
        kprintf("[--] safe mode: skipping network + audio bring-up\n");
    } else {
        net_init();      /* netif core + NIC driver probe (e1000, virtio-net) */

        /* A saved configuration wins over DHCP, and is loaded first so a static
         * setup does not pay for a DHCP round trip that will be thrown away.
         * Without this every boot re-leased and forgot, so a fixed address or a
         * chosen DNS server had to be re-entered by hand every single time. */
        int saved = netcfg_load();
        if (saved && !g_netcfg.use_dhcp)
            kprintf("[ok] network: static configuration from %s\n", NETCFG_PATH);
        else
            dhcp_configure(2000);  /* lease IP/mask/gw/DNS; static on timeout */
        netcfg_apply();            /* static values, and a chosen DNS either way */

        /* The RTC is usually wrong on a VM, and a wrong clock fails TLS
         * certificate validity checks -- which looks like "HTTPS is broken"
         * rather than "the year is wrong". One UDP round trip, best effort. */
        if (g_netcfg.ntp_enabled && ntp_sync(g_netcfg.ntp_server, 3000) == 0) {
            struct rtc_time nt; ntp_now(&nt);
            kprintf("[ok] clock synced by NTP: %u-%02u-%02u %02u:%02u:%02u UTC\n",
                    nt.year, nt.mon, nt.day, nt.hour, nt.min, nt.sec);
        }

        audio_init();    /* AC97 / Intel HDA codec (PC speaker fallback) */
    }

    sched_init();                         /* adopt current context as thread 0 */
    sched_add(demo_thread_a, "demoA");
    sched_add(demo_thread_b, "demoB");
    sched_add(watchdog_thread, "watchdog");
    launch_user_demo();                   /* ring-3 user thread + syscalls */
    kprintf("[ok] preemptive scheduler (%d threads)\n", sched_count());

    evlog("framebuffer console came up");
    evlog("memory managers initialised");
    evlog("timer and keyboard online");
    evlog("ramfs mounted and shell started");

    if (fb_present() && !boot_is_safe()) {
        kprintf("\nBoltOS is up. Launching desktop.\n");
        gui_run();          /* enter the GUI desktop; never returns */
    }

    kprintf("\nBoltOS is up (%s mode). Starting BoltShell.\n", boot_mode_name());
    if (boot_is_safe())
        kprintf("The desktop is not running. 'help' lists commands; "
                "'reboot' returns to a normal boot.\n");
    shell_run();    /* serial-only fallback; never returns */
}
