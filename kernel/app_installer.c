/* ===========================================================================
 *  BoltOS  -  kernel/app_installer.c
 *  The graphical installer: pick a disk, choose the account you will sign in
 *  with, confirm, watch the progress bar. Front-end only -- every byte written
 *  goes through kernel/install.c, the same code the `install` shell command
 *  drives, so the two can never drift apart.
 *
 *  The install itself is a single long synchronous call. It runs from the
 *  window's tick callback, one stage per tick, so the compositor gets a frame
 *  in between and the progress bar actually moves instead of the desktop
 *  freezing for the duration.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "install.h"
#include "blk.h"
#include "users.h"
#include "commands.h"     /* sh_human, sh_utoa */
#include "string.h"
#include "kprintf.h"

/* Wizard pages. */
enum { PG_DISK = 0, PG_ACCOUNT, PG_CONFIRM, PG_RUNNING, PG_DONE };

/* Clickable regions, rebuilt every draw (same pattern as the Settings app). */
enum { K_DISK = 1, K_NEXT, K_BACK, K_FIELD, K_ESP };

typedef struct { int x, y, w, h; uint8_t kind, val; } hot_t;
#define MAXHOT 24
static hot_t hots[MAXHOT];
static int   nhot, org_x, org_y;

static int   page;
static int   sel_disk;
static int   field;                 /* 0 = username, 1 = password */
static char  user[USER_NAME_MAX];
static char  pass[40];
static int   ulen, plen;
static int   esp_choice = 1;        /* index into ESP_MB */
static char  msg[INSTALL_ERR_LEN];
static install_req req;
static int   run_started;

static const uint32_t ESP_MB[] = { 64, 128, 256 };
#define NESP ((int)(sizeof(ESP_MB) / sizeof(ESP_MB[0])))

static void hot_add(int x, int y, int w, int h, int kind, int val) {
    if (nhot >= MAXHOT) return;
    hots[nhot].x = x - org_x; hots[nhot].y = y - org_y;
    hots[nhot].w = w; hots[nhot].h = h;
    hots[nhot].kind = (uint8_t)kind; hots[nhot].val = (uint8_t)val;
    nhot++;
}

/* ---- widgets ------------------------------------------------------------ */
static int button(int x, int y, const char *label, int enabled) {
    int w = g_text_width(label, 1) + 32, h = 32;
    g_round(x, y, w, h, 8, enabled ? COL_ACCENT : COL_PANEL_3, 255);
    g_text(x + 16, y + 12, label, enabled ? 0xFFFFFF : COL_TEXT_DIM, 1);
    return w;
}

static void field_box(int x, int y, int w, const char *text, int active, int mask) {
    g_round(x, y, w, 30, 6, active ? COL_PANEL_3 : COL_PANEL_2, 255);
    g_rect(x, y, w, 30, active ? COL_ACCENT : COL_PANEL_3);
    if (mask) {
        char stars[40]; int i = 0;
        while (text[i] && i < 39) { stars[i] = '*'; i++; }
        stars[i] = 0;
        g_text(x + 10, y + 9, stars, COL_TEXT, 1);
    } else {
        g_text(x + 10, y + 9, text, COL_TEXT, 1);
    }
}

static void progress_bar(int x, int y, int w, int pct) {
    g_round(x, y, w, 18, 9, COL_PANEL_3, 255);
    int fill = (w * (pct < 0 ? 0 : pct > 100 ? 100 : pct)) / 100;
    if (fill > 0) g_round(x, y, fill < 18 ? 18 : fill, 18, 9, COL_ACCENT, 255);
    char n[8]; sh_utoa((uint64_t)pct, n);
    g_text(x + w + 10, y + 3, n, COL_TEXT_DIM, 1);
    g_text(x + w + 10 + g_text_width(n, 1), y + 3, "%", COL_TEXT_DIM, 1);
}

/* ---- pages -------------------------------------------------------------- */
static int draw_disk_page(int x, int y, int right) {
    g_text(x, y, "Choose the disk to install BoltOS onto.", COL_TEXT, 1);
    y += 20;
    g_text(x, y, "Everything on it will be erased.", COL_BAD, 1);
    y += 28;

    int n = install_target_count();
    if (n == 0) {
        g_text(x, y, "No installable disks were found.", COL_WARN, 1); y += 18;
        g_text(x, y, "The disk this system booted from is excluded;", COL_TEXT_DIM, 1); y += 16;
        g_text(x, y, "attach another disk and reopen this window.", COL_TEXT_DIM, 1);
        return y + 24;
    }
    if (sel_disk >= n) sel_disk = 0;

    for (int i = 0; i < n; i++) {
        blkdev_t *d = install_target_get(i);
        int h = 44, w = right - x;
        int on = (i == sel_disk);
        g_round(x, y, w, h, 8, on ? COL_PANEL_3 : COL_PANEL_2, 255);
        if (on) g_rect(x, y, w, h, COL_ACCENT);
        char cap[16]; sh_human(d->sectors * d->sector_size, cap);
        g_text(x + 12, y + 8,  d->name, on ? COL_TEXT : COL_TEXT_DIM, 1);
        g_text(x + 80, y + 8,  blk_media(d), COL_TEXT_DIM, 1);
        g_text(x + 140, y + 8, cap, COL_TEXT_DIM, 1);
        g_text(x + 12, y + 25, d->model[0] ? d->model : "(disk)", COL_TEXT_DIM, 1);
        hot_add(x, y, w, h, K_DISK, i);
        y += h + 8;
    }
    return y + 12;
}

static int draw_account_page(int x, int y, int right) {
    g_text(x, y, "Create the account you will sign in with.", COL_TEXT, 1);
    y += 28;
    int fw = right - x - 90;
    if (fw > 260) fw = 260;

    g_text(x, y + 9, "Username", COL_TEXT_DIM, 1);
    field_box(x + 90, y, fw, user, field == 0, 0);
    hot_add(x + 90, y, fw, 30, K_FIELD, 0);
    y += 42;

    g_text(x, y + 9, "Password", COL_TEXT_DIM, 1);
    field_box(x + 90, y, fw, pass, field == 1, 1);
    hot_add(x + 90, y, fw, 30, K_FIELD, 1);
    y += 46;

    g_text(x, y, "Tab switches fields; type to edit.", COL_TEXT_DIM, 1);
    y += 30;

    g_text(x, y, "EFI partition size", COL_TEXT_DIM, 1);
    y += 22;
    int bx = x;
    for (int i = 0; i < NESP; i++) {
        char lab[16]; sh_utoa(ESP_MB[i], lab); kstrlcat(lab, " MB", sizeof lab);
        int w = g_text_width(lab, 1) + 24;
        g_round(bx, y, w, 28, 8, i == esp_choice ? COL_ACCENT : COL_PANEL_3, 255);
        g_text(bx + 12, y + 10, lab, i == esp_choice ? 0xFFFFFF : COL_TEXT, 1);
        hot_add(bx, y, w, 28, K_ESP, i);
        bx += w + 8;
    }
    return y + 44;
}

static int draw_confirm_page(int x, int y, int right) {
    blkdev_t *d = install_target_get(sel_disk);
    (void)right;
    g_text(x, y, "Ready to install.", COL_TEXT, 1);
    y += 26;
    if (d) {
        char cap[16]; sh_human(d->sectors * d->sector_size, cap);
        g_text(x, y, "Target", COL_TEXT_DIM, 1);
        g_text(x + 90, y, d->name, COL_TEXT, 1);
        g_text(x + 150, y, cap, COL_TEXT_DIM, 1);
        g_text(x + 220, y, d->model[0] ? d->model : "(disk)", COL_TEXT_DIM, 1);
        y += 20;

        char plan[128];
        install_describe(d->name, plan, sizeof plan);
        g_text(x, y, "Layout", COL_TEXT_DIM, 1);
        g_text(x + 90, y, plan, COL_TEXT, 1);
        y += 20;
    }
    g_text(x, y, "Account", COL_TEXT_DIM, 1);
    g_text(x + 90, y, user, COL_TEXT, 1);
    y += 28;

    g_text(x, y, "Everything currently on that disk will be destroyed.", COL_BAD, 1);
    y += 18;
    g_text(x, y, "Both boot paths are written: legacy MBR and an EFI", COL_TEXT_DIM, 1);
    y += 16;
    g_text(x, y, "system partition with BOOTX64.EFI.", COL_TEXT_DIM, 1);
    return y + 30;
}

static int draw_running_page(int x, int y, int right) {
    g_text(x, y, page == PG_DONE ? "Installation complete." : "Installing...", COL_TEXT, 1);
    y += 28;
    progress_bar(x, y, right - x - 60, req.percent);
    y += 32;
    g_text(x, y, req.step[0] ? req.step : "starting", COL_TEXT_DIM, 1);
    y += 28;
    if (req.failed) {
        g_text(x, y, "Failed:", COL_BAD, 1);
        g_text(x + 60, y, req.error, COL_BAD, 1);
        y += 24;
    } else if (page == PG_DONE) {
        g_text(x, y, "Shut down, remove the install medium, and boot the", COL_TEXT, 1);
        y += 16;
        g_text(x, y, "new disk. Sign in as:", COL_TEXT, 1);
        g_text(x + 160, y, user, COL_ACCENT, 1);
        y += 24;
    }
    return y;
}

/* ---- window ------------------------------------------------------------- */
static void inst_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    nhot = 0; org_x = cx; org_y = cy;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    int x = cx + 22, y = cy + 18, right = cx + cw - 22;

    /* Step strip, so it is always obvious where in the wizard you are. */
    static const char *names[] = { "Disk", "Account", "Confirm", "Install" };
    int sx = x;
    for (int i = 0; i < 4; i++) {
        int active = (page == PG_DONE ? 3 : page) >= i;
        g_round(sx, y, 10, 10, 5, active ? COL_ACCENT : COL_PANEL_3, 255);
        g_text(sx + 16, y, names[i], active ? COL_TEXT : COL_TEXT_DIM, 1);
        sx += 26 + g_text_width(names[i], 1);
    }
    y += 30;
    g_hline(x, y, right - x, COL_PANEL_3);
    y += 18;

    switch (page) {
    case PG_DISK:    y = draw_disk_page(x, y, right); break;
    case PG_ACCOUNT: y = draw_account_page(x, y, right); break;
    case PG_CONFIRM: y = draw_confirm_page(x, y, right); break;
    default:         y = draw_running_page(x, y, right); break;
    }

    if (msg[0]) { g_text(x, y, msg, COL_WARN, 1); y += 22; }

    /* Buttons pinned to the bottom of the client area. */
    int by = cy + ch - 46;
    if (page > PG_DISK && page < PG_RUNNING) {
        int bw = button(x, by, "Back", 1);
        hot_add(x, by, bw, 32, K_BACK, 0);
    }
    if (page < PG_RUNNING) {
        const char *lab = (page == PG_CONFIRM) ? "Erase disk and install" : "Next";
        int enabled = (page != PG_DISK) || install_target_count() > 0;
        int bw = g_text_width(lab, 1) + 32;
        button(right - bw, by, lab, enabled);
        if (enabled) hot_add(right - bw, by, bw, 32, K_NEXT, 0);
    }
}

static void inst_key(window_t *w, char c) {
    (void)w;
    if (page != PG_ACCOUNT) return;
    msg[0] = 0;
    if (c == '\t') { field ^= 1; return; }
    if (c == '\n') { if (field == 0) field = 1; return; }
    if (c == '\b') {
        if (field == 0) { if (ulen) user[--ulen] = 0; }
        else            { if (plen) pass[--plen] = 0; }
        return;
    }
    if ((unsigned char)c < 32) return;
    if (field == 0) { if (ulen < USER_NAME_MAX - 1) { user[ulen++] = c; user[ulen] = 0; } }
    else            { if (plen < 39)                { pass[plen++] = c; pass[plen] = 0; } }
}

static void advance(void) {
    msg[0] = 0;
    switch (page) {
    case PG_DISK:
        if (install_target_count() == 0) { strncpy(msg, "No installable disk.", sizeof msg - 1); return; }
        if (!install_kernel_available()) {
            strncpy(msg, "Kernel image not found on the boot medium.", sizeof msg - 1);
            return;
        }
        page = PG_ACCOUNT;
        break;
    case PG_ACCOUNT:
        if (!user[0]) { strncpy(msg, "A username is required.", sizeof msg - 1); return; }
        page = PG_CONFIRM;
        break;
    case PG_CONFIRM: {
        blkdev_t *d = install_target_get(sel_disk);
        if (!d) { strncpy(msg, "That disk went away.", sizeof msg - 1); return; }
        memset(&req, 0, sizeof req);
        strncpy(req.disk, d->name, sizeof req.disk - 1);
        strncpy(req.username, user, sizeof req.username - 1);
        strncpy(req.password, pass, sizeof req.password - 1);
        req.esp_mb = ESP_MB[esp_choice];
        strncpy(req.step, "starting", INSTALL_STEP_LEN - 1);
        page = PG_RUNNING;
        run_started = 0;
        break;
    }
    default: break;
    }
}

/* The install runs from the tick, one call, on the tick AFTER the running page
 * has been painted once -- so the user sees "Installing..." rather than a
 * desktop that appears to hang while the disk is written. */
static void inst_tick(window_t *w) {
    (void)w;
    if (page != PG_RUNNING) return;
    if (!run_started) { run_started = 1; gui_request_redraw(); return; }
    if (run_started == 1) {
        run_started = 2;
        install_run(&req);
        page = PG_DONE;
        if (req.failed) gui_toast("Install failed");
        else            gui_toast("BoltOS installed");
        gui_request_redraw();
    }
}

static void inst_click(window_t *w, int lx, int ly) {
    (void)w;
    for (int i = 0; i < nhot; i++) {
        hot_t *h = &hots[i];
        if (lx < h->x || lx >= h->x + h->w || ly < h->y || ly >= h->y + h->h) continue;
        switch (h->kind) {
        case K_DISK:  sel_disk = h->val; break;
        case K_FIELD: field = h->val; break;
        case K_ESP:   esp_choice = h->val; break;
        case K_BACK:  if (page > PG_DISK) page--; msg[0] = 0; break;
        case K_NEXT:  advance(); break;
        }
        return;
    }
}

void installer_app_init(void) {
    window_t *win = gui_add_window("Install BoltOS", 560, 460, 0x46D17B, ICON_INSTALL);
    if (!win) return;
    win->draw  = inst_draw;
    win->click = inst_click;
    win->key   = inst_key;
    win->tick  = inst_tick;
    win->min_w = 460; win->min_h = 380;
    win->x = 180; win->y = 90;
    strncpy(user, "user", sizeof user - 1);
    ulen = (int)strlen(user);
}
