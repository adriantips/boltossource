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
 *      look after packages and updates (boltos.dev)
 *      let the shell draw a frame      (only if something changed)
 *      feed the watchdog, sleep until the next tick
 *
 *  A frame that takes longer than a tick just means the next poll is late;
 *  nothing in here waits for anything else.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "fs.h"
#include "kprintf.h"
#include "kheap.h"
#include "buildid.h"
#include "font.h"
#include "stdio.h"
#include "string.h"
#include "wcn.h"
#include "wlan.h"
#include "bt.h"
#include "update.h"

const char *mini_version(void) { return BOLT_VERSION " (" BOLT_BUILD_ID ")"; }

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

perf_t g_perf;
int perf_report(char *out, int cap) {
    perf_t p = g_perf;
    uint64_t t = now_us(), dt = p.t0 ? t - p.t0 : 1;
    int vnc_perf(char *out, int cap, uint64_t window_us);
    memset(&g_perf, 0, sizeof g_perf);
    g_perf.t0 = t;
    uint32_t f = p.frames ? p.frames : 1, u = p.vnc_updates ? p.vnc_updates : 1;
    int n = snprintf(out, (size_t)cap,
        "window %lu ms\n"
        "frames %u (%lu.%lu/s), draw avg %lu us max %lu us, present avg %lu us\n"
        "loops %u (%lu/s), longest turn %lu us\n"
        "vnc updates %u (%lu.%lu/s), %u rects, %lu KiB (%lu KiB/s), scan avg %lu us max %lu us, pump %lu us total\n",
        (unsigned long)(dt / 1000),
        p.frames, (unsigned long)(p.frames * 10000000ull / dt / 10), (unsigned long)(p.frames * 10000000ull / dt % 10),
        (unsigned long)(p.draw_us / f), (unsigned long)p.draw_max, (unsigned long)(p.present_us / f),
        p.loops, (unsigned long)(p.loops * 1000000ull / dt), (unsigned long)p.busy_max,
        p.vnc_updates, (unsigned long)(p.vnc_updates * 10000000ull / dt / 10), (unsigned long)(p.vnc_updates * 10000000ull / dt % 10),
        p.vnc_rects, (unsigned long)(p.vnc_bytes >> 10), (unsigned long)((p.vnc_bytes * 1000000ull / dt) >> 10),
        (unsigned long)(p.vnc_scan_us / u), (unsigned long)p.vnc_scan_max, (unsigned long)p.vnc_pump_us);
    if (n < cap) n += vnc_perf(out + n, cap - n, dt);
    return n;
}

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
    cores_init();

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
    /* The card's files: what a power cut took from the kept settings comes
     * back from there before Wi-Fi and Bluetooth look for it. */
    if (fs_mount() == 0) { fs_keep_restore(); wcn_keep_firmware(); }
    void shell_settings_restore(void);
    shell_settings_restore();
    update_init();
    kprintf("[%4lu ms] the card's files\n", now_ms() - t0);
    void pkg_init(void);
    pkg_init();
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
        uint64_t t_turn = now_us();
        mini_pump();
        int pkg_poll(void);
        if (pkg_poll()) shell_request_frame();
        if (update_poll()) shell_request_frame();
        shell_frame();
        netsvc_jobs();
        fs_poll();
        wifi_poll();
        bt_poll();
        uint64_t t = now_us();
        g_perf.loops++;
        if (t - t_turn > g_perf.busy_max) g_perf.busy_max = t - t_turn;
        if (musb_busy()) continue;
        wfi();
        g_idle_us += now_us() - t;
    }
}
