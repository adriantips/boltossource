/* ===========================================================================
 *  BoltOS Mini  -  mini/stubs.c
 *
 *  Where the shared code reaches for something Mini does not have, and the
 *  answer is "nothing" -- the same job tv/stubs.c does for the TV kernel.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "stdio.h"

/* The heap asks the physical allocator for one contiguous run; Mini's RAM
 * map gives it a fixed one (mini.h). */
uint64_t pmm_alloc_contig(uint64_t n) {
    static int given;
    if (given || n * 0x1000ull > MINI_HEAP_SIZE) return 0;
    given = 1;
    return MINI_HEAP_BASE;
}

/* font.c asks for a repaint when a face finishes loading; the shell draws
 * from the fonts after they are loaded, so there is nothing to repaint. */
void gui_request_redraw(void) { }

/* libc's scanf.c also has the stdin flavours; there is no stdin here, only
 * sscanf, which never reaches these. */
FILE *stdin;
int fgetc(FILE *f) { (void)f; return -1; }
int ungetc(int c, FILE *f) { (void)c; (void)f; return -1; }

/* ---- not every build has every part ---------------------------------------------
 *  Weak, so a kernel without one of these still links and boots, and says
 *  plainly that the part is missing. The real ones replace them.            */
__attribute__((weak)) int  musb_init(void) { return -1; }
__attribute__((weak)) void musb_poll(void) { }
__attribute__((weak)) int  musb_configured(void) { return 0; }
__attribute__((weak)) int  musb_busy(void) { return 0; }
__attribute__((weak)) int  usbh_init(void) { return 0; }
__attribute__((weak)) void usbh_poll(void) { }
__attribute__((weak)) int  usbh_device_list(char *out, int cap) { if (cap) out[0] = 0; return 0; }
__attribute__((weak)) int  usbh_keyboards(void) { return 0; }
__attribute__((weak)) int  usbh_debug(char *out, int cap) { if (cap) out[0] = 0; return 0; }
__attribute__((weak)) int  usbh_mice(void) { return 0; }
__attribute__((weak)) int  eth_init(void) { return -1; }
__attribute__((weak)) void eth_poll(void) { }
__attribute__((weak)) int  eth_link(void) { return 0; }
__attribute__((weak)) struct mini_netif *eth_netif(void) { return 0; }
__attribute__((weak)) int  eth_debug(char *out, int cap) { if (cap) out[0] = 0; return 0; }
__attribute__((weak)) int  virtio_init(void) { return 0; }
__attribute__((weak)) void virtio_poll(void) { }
__attribute__((weak)) void net_init(void) { }
__attribute__((weak)) void net_poll(void) { }
__attribute__((weak)) void netsvc_init(void) { }
__attribute__((weak)) void netsvc_poll(void) { }
__attribute__((weak)) void netsvc_jobs(void) { }
__attribute__((weak)) int  vnc_clients(void) { return 0; }
__attribute__((weak)) int  vnc_streaming(void) { return 0; }
__attribute__((weak)) int  vnc_perf(char *out, int cap, uint64_t w) { (void)w; if (cap) out[0] = 0; return 0; }
__attribute__((weak)) char g_vnc_password[9] = "boltos";
__attribute__((weak)) void shell_request_frame(void) { }
__attribute__((weak)) void shell_toast(const char *t, const char *d) { (void)t; (void)d; }
__attribute__((weak)) int  shell_frame(void) { return 0; }
__attribute__((weak)) void shell_init(void) {
    gfx_bind(&g_back, 0, 0, 0, 0, scr_w, scr_h);
    gfx_vgrad(0, 0, scr_w, scr_h, 0x1B2440, 0x3A2C5A, 255);
    gfx_rrect(scr_w / 2 - 200, scr_h / 2 - 60, 400, 120, 24, 0xFFFFFF, 40);
    screen_present(0, 0, scr_w, scr_h);
}
/* The H.264 decoder's per-macroblock trace goes to the host test's stdout; on Mini, nowhere. */
__attribute__((weak)) void h264_trace(const char *fmt, ...) { (void)fmt; }
