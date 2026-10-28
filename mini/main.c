/* ===========================================================================
 *  BoltOS Mini  -  mini/main.c   (kmain)
 *
 *  Bring the board up in the order its pieces depend on each other, then
 *  run the one loop that everything lives in:
 *
 *      poll the USB-C device port      (the Mac's network link)
 *      poll the USB host ports         (the keyboard on the hub)
 *      poll QEMU's virtio devices      (when there is no Pi)
 *      run the network                 (DHCP, the web page, VNC)
 *      let the shell draw a frame      (only if something changed)
 *      feed the watchdog, sleep until the next tick
 *
 *  A frame that takes longer than a tick just means the next poll is late;
 *  nothing in here waits for anything else.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kprintf.h"
#include "kheap.h"
#include "buildid.h"
#include "font.h"
#include "stdio.h"
#include "wcn.h"
#include "wlan.h"
#include "bt.h"

const char *mini_version(void) { return "1.0 (" BOLT_BUILD_ID ")"; }

int mini_uptime_str(char *out, int cap) {
    uint64_t s = now_ms() / 1000;
    int d = (int)(s / 86400), h = (int)(s / 3600 % 24), m = (int)(s / 60 % 60);
    int n;
    if (d)      n = snprintf(out, (size_t)cap, "%dd %dh %dm", d, h, m);
    else if (h) n = snprintf(out, (size_t)cap, "%dh %dm", h, m);
    else        n = snprintf(out, (size_t)cap, "%dm %ds", m, (int)(s % 60));
    return n;
}

extern uint64_t g_idle_us;

/* Everything but the shell: what a blocking wait (a DNS lookup, a page
 * load) keeps turning so the links, the keyboard and VNC carry on. */
void mini_pump(void) {
    static int depth;
    if (depth > 2) return;
    depth++;
    musb_poll();
    usbh_poll();
    eth_poll();
    wcn_poll();
    if (g_plat == PLAT_QEMU) virtio_poll();
    net_poll();
    inet_poll();
    netsvc_poll();
    board_poll();
    depth--;
}          /* shell.c: time spent asleep, for the load */
static uint64_t g_dtb;
uint64_t mini_dtb(void) { return g_dtb; }

void kmain(uint64_t dtb) {
    uint64_t t0 = now_ms();             /* the counter runs from power-on: U-Boot's share is not ours */
    g_dtb = dtb;
    board_early(dtb);
    klog_init();
    kprintf("\n==================================================\n");
    kprintf("   BoltOS Mini %s\n", mini_version());
    kprintf("   %s, %lu MiB RAM, boot #%u\n", g_board_name,
            (unsigned long)(g_ram_size >> 20), klog_boot_count());
    kprintf("==================================================\n");
    uint64_t el; __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    kprintf("[ok] EL%lu, MMU and caches on, dtb at %lx\n", el >> 2, dtb);

    kheap_init();
    board_init();
    irq_on();
    kprintf("[ok] GIC and timer (1 kHz); CPU %u MHz\n", board_cpu_mhz());

    font_init();
    kprintf("[%4lu ms] fonts\n", now_ms() - t0);
    screen_init(1024, 600);

    int link = musb_init();
    if (link == 0) kprintf("[ok] USB-C port is a network adapter to the Mac\n");
    kprintf("[%4lu ms] USB-C\n", now_ms() - t0);
    usbh_init();
    kprintf("[%4lu ms] USB host\n", now_ms() - t0);
    if (g_plat == PLAT_QEMU) virtio_init();
    if (eth_init() == 0) kprintf("[%4lu ms] Ethernet\n", now_ms() - t0);
    net_init();
    netsvc_init();
    inet_init();
    shell_init();
    kprintf("[%4lu ms] shell (wallpaper)\n", now_ms() - t0);
    board_led(1, 1);
    kprintf("[ok] up in %lu ms (%lu since power-on)\n", now_ms() - t0, now_ms());

    /* Runs from a battery as well as from the Mac: with no USB host it simply
     * carries on (the old 90 s "restart into U-Boot's DFU window" rule is gone,
     * it would have rebooted a Pi that nobody is plugged into). To send a new
     * kernel, POST /reboot (pi.py does) while the Mac is connected. */
    extern volatile uint64_t g_loop_ms;
    for (;;) {
        g_loop_ms = now_ms();
        mini_pump();
        shell_frame();
        netsvc_jobs();
        wifi_poll();
        bt_poll();
        if (musb_busy()) continue;
        uint64_t t = now_us();
        wfi();
        g_idle_us += now_us() - t;
    }
}
