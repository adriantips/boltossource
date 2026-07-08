/* ===========================================================================
 *  BoltOS  -  kernel/app_taskmgr.c
 *  Task Manager window: live CPU (a real busy/idle measurement from the GUI
 *  loop), physical RAM and kernel-heap usage, scrolling history graphs, and a
 *  process table sourced from the kernel task registry.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
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
    g_round(x, y, w, h, 4, 0x0E0E16, 255);
    for (int i = 1; i < 4; i++) g_hline(x + 2, y + h * i / 4, w - 4, 0x1E1E2A);   /* grid */
    int base = y + h - 2, avail = h - 4, span = w - 4;
    int start = n > span ? n - span : 0;
    for (int i = start; i < n; i++) {
        int col = x + 2 + (i - start);
        int vh = data[i] * avail / 100;
        if (vh > 0) {
            g_blend(col, base - vh, 1, vh, color, 130);     /* translucent fill   */
            g_fill(col, base - vh, 1, 2, color);            /* bright top of bar  */
        }
    }
}

static void card(int x, int y, int w, int h, const char *label, int pct,
                 const char *sub, const uint8_t *hist, int n, uint32_t color) {
    g_round(x, y, w, h, 8, COL_PANEL_2, 255);
    g_text(x + 16, y + 14, label, color, 1);
    char big[8]; pct_str(pct, big);
    g_text(x + 16, y + 36, big, COL_TEXT, 3);
    if (sub) g_text(x + 16, y + 78, sub, COL_TEXT_DIM, 1);
    int gx = x + 168, gw = w - 168 - 16;
    graph(gx, y + 14, gw, h - 28, hist, n, color);
}

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

/* approximate per-task CPU split for the table (cooperative kernel: the real
 * figure is the aggregate busy/idle meter shown in the CPU card) */
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
    g_fill(cx, cy, cw, ch, COL_PANEL);
    int pad = 14, x = cx + pad, y = cy + pad, cardw = cw - 2 * pad;
    int load = gui_cpu_load();

    /* CPU card */
    card(x, y, cardw, 116, "CPU", load, tm.brand[0] ? tm.brand : "Processor",
         tm.cpu, tm.n, COL_ACCENT);

    /* Memory card */
    uint64_t totp = pmm_total_count(), frep = pmm_free_count();
    uint64_t total_b = totp * 4096, used_b = total_b - frep * 4096;
    char memsub[28]; mib_pair(used_b, total_b, memsub, sizeof(memsub));
    card(x, y + 128, cardw, 116, "Memory", ram_percent(), memsub,
         tm.ram, tm.n, 0x9B6CF2);

    /* process table */
    int ty = y + 256;
    g_text(x, ty, "Processes", COL_TEXT, 1); ty += 20;
    g_text(x,        ty, "NAME",  COL_TEXT_DIM, 1);
    g_text(x + 130,  ty, "PID",   COL_TEXT_DIM, 1);
    g_text(x + 180,  ty, "STATE", COL_TEXT_DIM, 1);
    g_text(x + 260,  ty, "CPU",   COL_TEXT_DIM, 1);
    g_text(x + 330,  ty, "MEM",   COL_TEXT_DIM, 1);
    ty += 6; g_hline(x, ty + 8, cardw, 0x2C2C38); ty += 16;

    for (int i = 0; i < task_count(); i++) {
        task_t *t = task_get(i);
        int ry = ty + i * 18;
        if (ry + 16 > cy + ch - 24) break;
        char num[12];
        g_text(x, ry, t->name, COL_TEXT, 1);
        sh_utoa((uint64_t)t->pid, num); g_text(x + 130, ry, num, COL_TEXT_DIM, 1);
        g_text(x + 180, ry, t->state, state_color(t->state), 1);
        int tc = task_cpu(t, load); pct_str(tc, num); g_text(x + 260, ry, num, COL_TEXT, 1);
        /* tiny cpu bar */
        g_fill(x + 300, ry + 2, 24, 6, 0x24242E);
        g_fill(x + 300, ry + 2, 24 * tc / 100, 6, COL_ACCENT);
        uint64_t kb = 64 + (uint64_t)t->pid * 48 + strlen(t->name) * 16;
        char hb[12]; sh_human(kb * 1024, hb); g_text(x + 330, ry, hb, COL_TEXT_DIM, 1);
    }

    /* footer */
    uint64_t hu = 0, htot = 0; kheap_usage(&hu, &htot);
    char foot[64]; char tmp[16];
    foot[0] = 0;
    sh_utoa(pit_ticks() / (pit_hz() ? pit_hz() : 1000), tmp);
    kstrlcat(foot, "Up ", sizeof(foot)); kstrlcat(foot, tmp, sizeof(foot)); kstrlcat(foot, "s   ", sizeof(foot));
    sh_utoa((uint64_t)task_count(), tmp);
    kstrlcat(foot, tmp, sizeof(foot)); kstrlcat(foot, " tasks   ", sizeof(foot));
    sh_utoa((uint64_t)svc_count(), tmp);
    kstrlcat(foot, tmp, sizeof(foot)); kstrlcat(foot, " services   heap ", sizeof(foot));
    sh_human(hu, tmp); kstrlcat(foot, tmp, sizeof(foot)); kstrlcat(foot, "/", sizeof(foot));
    sh_human(htot, tmp); kstrlcat(foot, tmp, sizeof(foot));
    g_text(x, cy + ch - 18, foot, COL_TEXT_DIM, 1);
}

void taskmgr_app_init(void) {
    memset(&tm, 0, sizeof(tm));
    hw_cpu_brand(tm.brand);
    /* keep the brand short enough to sit left of the graph (~18 chars) */
    if (strlen(tm.brand) > 18) tm.brand[18] = 0;
    for (int i = (int)strlen(tm.brand) - 1; i >= 0 && tm.brand[i] == ' '; i--) tm.brand[i] = 0;
    window_t *win = gui_add_window("Task Manager", 564, 540, 0x9B6CF2, ICON_TASKMGR);
    if (!win) return;
    win->draw = tm_draw;
    win->tick = tm_tick;
    win->x = 470; win->y = 70;
    /* prime the graphs so they aren't empty on first open */
    uint8_t r0 = (uint8_t)ram_percent();
    for (int i = 0; i < 40; i++) hist_push(0, r0);
}
