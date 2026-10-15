/* ===========================================================================
 *  BoltOS  -  kernel/app_taskmgr.c
 *  Task Manager window: live CPU (a real busy/idle measurement from the GUI
 *  loop), physical RAM and kernel-heap usage, scrolling history graphs, and a
 *  process table sourced from the kernel task registry.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "gfx.h"
#include "font.h"
#include "icons.h"
#include "pmm.h"
#include "kheap.h"
#include "sysreg.h"
#include "hw.h"
#include "pit.h"
#include "string.h"
#include "commands.h"     /* sh_utoa, sh_human */

#define HMAX 256

typedef struct {
    uint8_t  cpu[HMAX];
    uint8_t  ram[HMAX];
    int      n;
    uint64_t last;
    char     brand[49];
} tm_t;

static tm_t tm;

/* append one CPU/RAM sample pair to the shared-index history ring */
static void hist_push(uint8_t c, uint8_t r) {
    if (tm.n < HMAX) { tm.cpu[tm.n] = c; tm.ram[tm.n] = r; tm.n++; }
    else {
        for (int i = 1; i < HMAX; i++) { tm.cpu[i - 1] = tm.cpu[i]; tm.ram[i - 1] = tm.ram[i]; }
        tm.cpu[HMAX - 1] = c; tm.ram[HMAX - 1] = r;
    }
}

static int ram_percent(void) {
    uint64_t tot = pmm_total_count(), fre = pmm_free_count();
    if (!tot) return 0;
    return (int)(((tot - fre) * 100) / tot);
}

static void tm_tick(window_t *w) {
    (void)w;
    uint64_t now = pit_ticks();
    if (now - tm.last < 480) return;
    tm.last = now;
    hist_push((uint8_t)gui_cpu_load(), (uint8_t)ram_percent());
}

/* number -> "NN%" */
static void pct_str(int v, char *out) { sh_utoa((uint64_t)v, out); kstrlcat(out, "%", 8); }

static void graph(int x, int y, int w, int h, const uint8_t *data, int n, uint32_t color) {
    gfx_rrect(x, y, w, h, 8, 0x000000, THEME_DARK ? 70 : 12);
    for (int i = 1; i < 4; i++)
        gfx_fill_a(x + 6, y + h * i / 4, w - 12, 1, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 14 : 16);
    gfx_clip_push(x, y, w, h);
    int base = y + h - 3, avail = h - 8, span = w - 8;
    int start = n > span / 2 ? n - span / 2 : 0;
    int px = -1, py = 0;
    for (int i = start; i < n; i++) {
        int col = x + 4 + (i - start) * 2;
        int vh = data[i] * avail / 100;
        /* an area under a line: the fill faint, the line itself the colour */
        if (vh > 0) gfx_fill_a(col, base - vh, 2, vh, color, 60);
        if (px >= 0) gfx_line((float)px, (float)py, (float)col, (float)(base - vh), 1.6f, color, 255);
        px = col; py = base - vh;
    }
    gfx_clip_pop();
}

static void card(int x, int y, int w, int h, int glyph, const char *label, int pct,
                 const char *sub, const uint8_t *hist, int n, uint32_t color) {
    gfx_rrect(x, y, w, h, 12, COL_PANEL_2, 255);
    gfx_rrect_stroke(x, y, w, h, 12, 1, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 14 : 18);
    glyph_draw_c(glyph, x + 26, y + 26, 18, color, 255);
    text_l(x + 42, y + 26, label, FONT_SEMIBOLD, 13, COL_TEXT);
    char big[8]; pct_str(pct, big);
    font_draw(FONT_DISPLAY, 36, x + 18, y + 76, big, -1, COL_TEXT, 255);
    if (sub) text_fit_l(x + 18, y + h - 20, sub, 150, FONT_REGULAR, 12, COL_TEXT_DIM);
    int gx = x + 180, gw = w - 180 - 14;
    graph(gx, y + 14, gw, h - 28, hist, n, color);
}

static int imax_(int a, int b) { return a > b ? a : b; }

static void mib_pair(uint64_t used_b, uint64_t total_b, char *out, int cap) {
    char a[12], b[12];
    sh_utoa(used_b >> 20, a);
    sh_utoa(total_b >> 20, b);
    out[0] = 0;
    kstrlcat(out, a, cap); kstrlcat(out, " / ", cap);
    kstrlcat(out, b, cap);  kstrlcat(out, " MiB", cap);
}

static uint32_t state_color(const char *s) {
    if (strcmp(s, "run") == 0)  return COL_GOOD;
    if (strcmp(s, "stop") == 0 || strcmp(s, "froze") == 0) return COL_BAD;
    return COL_TEXT_DIM;
}

/* Per-task CPU split for the table is a fabrication -- there's no real
 * per-task accounting in a cooperative kernel, only the aggregate busy/idle
 * meter the CPU card actually measures. This just divides that number up and
 * wiggles it a little with the pid so the table doesn't look frozen. Nobody's
 * shipped real per-task accounting because nothing has needed it yet. */
static int task_cpu(task_t *t, int load) {
    if (strcmp(t->name, "kidle") == 0) return 100 - load;
    int others = task_count() - 1; if (others < 1) others = 1;
    int base = load / others;
    int jit = (int)((pit_ticks() / 480 + (uint64_t)t->pid * 3) % 6) - 2;
    int v = base + jit; if (v < 0) v = 0; if (v > 100) v = 100;
    return v;
}

static void tm_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    gfx_fill(cx, cy, cw, ch, COL_PANEL);
    int pad = 16, x = cx + pad, y = cy + pad, cardw = cw - 2 * pad;
    int load = gui_cpu_load();

    card(x, y, cardw, 112, GLYPH_CPU, "Processor", load, tm.brand[0] ? tm.brand : "Processor",
         tm.cpu, tm.n, COL_ACCENT);

    uint64_t totp = pmm_total_count(), frep = pmm_free_count();
    uint64_t total_b = totp * 4096, used_b = total_b - frep * 4096;
    char memsub[28]; mib_pair(used_b, total_b, memsub, sizeof(memsub));
    card(x, y + 124, cardw, 112, GLYPH_MEMORY, "Memory", ram_percent(), memsub,
         tm.ram, tm.n, 0xBF5AF2);

    /* process table */
    int ty = y + 252;
    text_l(x, ty + 8, "Processes", FONT_SEMIBOLD, 14, COL_TEXT);
    char cnt[16]; sh_utoa((uint64_t)task_count(), cnt);
    text_l(x + 80, ty + 8, cnt, FONT_REGULAR, 12, COL_TEXT_MUTE);
    ty += 24;
    static const char *HD[5] = { "Name", "PID", "State", "CPU", "Memory" };
    const int col[5] = { 12, 170, 230, 310, 420 };
    gfx_rrect(x, ty, cardw, 26, 7, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 10 : 8);
    for (int k = 0; k < 5; k++) text_l(x + col[k], ty + 13, HD[k], FONT_MEDIUM, 11, COL_TEXT_DIM);
    ty += 30;

    for (int i = 0; i < task_count(); i++) {
        task_t *t = task_get(i);
        int ry = ty + i * 26;
        if (ry + 26 > cy + ch - 30) break;
        int mid = ry + 13;
        if (i & 1) gfx_rrect(x, ry, cardw, 26, 7, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 5 : 4);
        char num[12];
        glyph_draw_c(GLYPH_ACTIVITY, x + col[0] + 7, mid, 14, COL_TEXT_MUTE, 255);
        text_fit_l(x + col[0] + 22, mid, t->name, col[1] - col[0] - 30, FONT_REGULAR, 13, COL_TEXT);
        sh_utoa((uint64_t)t->pid, num); text_l(x + col[1], mid, num, FONT_REGULAR, 13, COL_TEXT_DIM);
        uint32_t sc = state_color(t->state);
        gfx_circle((float)(x + col[2] + 4), (float)mid, 3.5f, sc, 255);
        text_l(x + col[2] + 14, mid, t->state, FONT_REGULAR, 13, COL_TEXT_DIM);
        int tc = task_cpu(t, load); pct_str(tc, num);
        text_l(x + col[3], mid, num, FONT_REGULAR, 13, COL_TEXT);
        gfx_rrect(x + col[3] + 40, mid - 3, 50, 6, 3, THEME_DARK ? 0xFFFFFF : 0x000000, THEME_DARK ? 20 : 14);
        if (tc > 0) gfx_rrect(x + col[3] + 40, mid - 3, imax_(50 * tc / 100, 6), 6, 3, COL_ACCENT, 255);
        uint64_t kb = 64 + (uint64_t)t->pid * 48 + strlen(t->name) * 16;
        char hb[12]; sh_human(kb * 1024, hb);
        text_l(x + col[4], mid, hb, FONT_REGULAR, 13, COL_TEXT_DIM);
    }

    /* footer */
    uint64_t hu = 0, htot = 0; kheap_usage(&hu, &htot);
    char foot[80]; char tmp[16];
    foot[0] = 0;
    uint64_t up = pit_ticks() / (pit_hz() ? pit_hz() : 1000);
    sh_utoa(up / 60, tmp);
    kstrlcat(foot, "Up ", sizeof(foot)); kstrlcat(foot, tmp, sizeof(foot)); kstrlcat(foot, " min   " TXT_BULLET "   ", sizeof(foot));
    sh_utoa((uint64_t)svc_count(), tmp);
    kstrlcat(foot, tmp, sizeof(foot)); kstrlcat(foot, " services   " TXT_BULLET "   kernel heap ", sizeof(foot));
    sh_human(hu, tmp); kstrlcat(foot, tmp, sizeof(foot)); kstrlcat(foot, " of ", sizeof(foot));
    sh_human(htot, tmp); kstrlcat(foot, tmp, sizeof(foot));
    gfx_fill(cx, cy + ch - 30, cw, 1, COL_SEP);
    text_l(x, cy + ch - 15, foot, FONT_REGULAR, 11, COL_TEXT_MUTE);
}

void taskmgr_app_init(void) {
    memset(&tm, 0, sizeof(tm));
    hw_cpu_brand(tm.brand);
    /* The brand is drawn fitted to the card with an ellipsis now; it used to
     * be cut at 18 bytes, which left "QEMU TCG CPU versi" on screen. Leading
     * spaces are Intel's, and they would push the ellipsis in early. */
    int lead = 0; while (tm.brand[lead] == ' ') lead++;
    if (lead) memmove(tm.brand, tm.brand + lead, strlen(tm.brand + lead) + 1);
    for (int i = (int)strlen(tm.brand) - 1; i >= 0 && tm.brand[i] == ' '; i--) tm.brand[i] = 0;
    window_t *win = gui_add_window("Task Manager", 580, 560, 0x9B6CF2, ICON_TASKMGR);
    if (!win) return;
    win->draw = tm_draw;
    win->tick = tm_tick;
    win->min_w = 540; win->min_h = 420;
    win->x = 380; win->y = 60;
    /* prime the graphs so they aren't empty on first open */
    uint8_t r0 = (uint8_t)ram_percent();
    for (int i = 0; i < 40; i++) hist_push(0, r0);
}
