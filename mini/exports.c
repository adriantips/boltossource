/* ===========================================================================
 *  BoltOS Mini  -  mini/exports.c
 *
 *  What a package may use of the kernel, by name (pkg.h). The loader fills
 *  a package's references to these with their addresses; anything not on
 *  this list stops the load, and mkpkg.py refuses to make a package that
 *  needs something missing. Adding a name is safe; removing one or
 *  changing what it means breaks the packages that use it.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "ui.h"
#include "pkg.h"
#include "store.h"
#include "dl.h"
#include "web.h"
#include "tls.h"
#include "crypto.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "math.h"

int  bt_dns_resolve_addr(const char *host, netaddr *out, uint32_t timeout_ms);
void inet_wait_clock(uint32_t ms);
void mini_pump(void);
int  wifi_state(void);

#define X(n) { #n, (const void *)&n },
const kexport_t mini_exports[] = {
    /* libc */
    X(memcpy) X(memset) X(memmove) X(memcmp) X(strlen) X(strcmp) X(strncmp) X(strcpy) X(strncpy)
    X(strcat) X(strchr) X(strrchr) X(strstr) X(strcasestr) X(strcasecmp) X(strncasecmp)
    X(snprintf) X(vsnprintf) X(strtol) X(strtoul) X(strtoll) X(strtoull) X(strtod) X(strtof) X(atoi) X(qsort)
    X(expf) X(logf) X(sqrtf) X(powf) X(fabsf) X(floorf) X(ceilf) X(roundf) X(sinf) X(cosf) X(tanf)
    X(exp) X(log) X(sqrt) X(pow) X(floor) X(ceil) X(fabs) X(sin) X(cos)
    /* memory, the log */
    X(kmalloc) X(kfree) X(kheap_usage) X(kheap_get_stats) X(kprintf)
    X(bigmem_claim) X(bigmem_free) X(bigmem_owner)
    /* the machine */
    X(now_ms) X(now_us) X(delay_ms) X(delay_us) X(clock_known) X(clock_unix) X(clock_local)
    X(board_cpu_mhz) X(board_temp_mc) X(mini_version) X(g_ram_size) X(g_plat) X(g_board_name)
    X(core_pooled) X(core_busy) X(core_wanted) X(core_run) X(mini_rand) X(mini_pump)
    /* drawing and text */
    X(scr_w) X(scr_h) X(th)
    X(gfx_fill) X(gfx_fill_a) X(gfx_rrect) X(gfx_rrect4) X(gfx_rrect_stroke) X(gfx_circle) X(gfx_ring)
    X(gfx_line) X(gfx_arc) X(gfx_clip_push) X(gfx_clip_pop) X(gfx_lerp) X(gfx_shade)
    X(font_draw) X(font_width) X(font_fit) X(font_ascent) X(font_descent)
    X(text_l) X(text_c) X(text_r) X(text_fit_l) X(text_wrap)
    /* the shell */
    X(ui_card) X(ui_glyph) X(ui_row) X(ui_keycap) X(ui_hit)
    X(shell_go_home) X(shell_open) X(shell_request_frame) X(shell_toast) X(shell_current) X(shell_cpu_load)
    /* the network */
    X(dl_start) X(dl_poll) X(dl_total) X(dl_got) X(dl_rate) X(dl_error) X(dl_free)
    X(tls_connect_addr) X(tls_send) X(tls_recv) X(tls_close) X(bt_dns_resolve_addr) X(inet_wait_clock)
    X(inet_dns_server) X(wifi_state) X(web_request)
    /* the SD card */
    X(store_open) X(store_is_open) X(store_why) X(store_at) X(store_find) X(store_create) X(store_write)
    X(store_read) X(store_progress) X(store_set_meta) X(store_remove) X(store_free) X(store_total)
    /* JSON, hashes */
    X(json_parse) X(json_free) X(json_get) X(json_at) X(json_path) X(json_s) X(json_i) X(utf8_to_cp1252)
    X(sha256_init) X(sha256_update) X(sha256_final) X(sha256)
};
const int mini_nexports = (int)(sizeof mini_exports / sizeof mini_exports[0]);
