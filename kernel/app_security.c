/* ===========================================================================
 *  BoltOS  -  kernel/app_security.c
 *  The Security window: accounts, session locking, and the firewall.
 *
 *  Every control here drives the same code the shell commands drive
 *  (kernel/users.c, net/firewall.c), so the GUI cannot end up with a different
 *  idea of who may do what than the terminal has. Nothing is enforced in this
 *  file -- it only shows and edits.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "users.h"
#include "firewall.h"
#include "net.h"
#include "commands.h"     /* sh_utoa */
#include "string.h"
#include "kprintf.h"

enum { K_LOCK = 1, K_IDLE, K_FW_ON, K_FW_DEF, K_FW_DEL, K_FW_PRESET, K_TAB };
enum { TAB_SESSION = 0, TAB_FIREWALL, TAB_ACCOUNTS, NTABS };

typedef struct { int x, y, w, h; uint8_t kind, val; } hot_t;
#define MAXHOT 48
static hot_t hots[MAXHOT];
static int   nhot, org_x, org_y;
static int   tab;
static char  note[72];

/* Idle-lock choices, in minutes. 0 = never. */
static const int IDLE_CHOICES[] = { 0, 1, 5, 10, 30 };
#define NIDLE ((int)(sizeof(IDLE_CHOICES) / sizeof(IDLE_CHOICES[0])))

/* One-click rules that cover what a desktop actually wants. Each is the exact
 * token list `firewall add` parses, so the GUI and the shell cannot diverge. */
static const struct { const char *label; const char *rule; } PRESETS[] = {
    { "Block inbound",      "block in any" },
    { "Allow outbound web", "allow out tcp port 443" },
    { "Block ping",         "block in icmp" },
    { "Allow DNS",          "allow out udp port 53" },
};
#define NPRESET ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

static void hot_add(int x, int y, int w, int h, int kind, int val) {
    if (nhot >= MAXHOT) return;
    hots[nhot].x = x - org_x; hots[nhot].y = y - org_y;
    hots[nhot].w = w; hots[nhot].h = h;
    hots[nhot].kind = (uint8_t)kind; hots[nhot].val = (uint8_t)val;
    nhot++;
}

static int chip(int x, int y, const char *label, int on, int kind, int val) {
    int w = g_text_width(label, 1) + 22, h = 28;
    g_round(x, y, w, h, 8, on ? COL_ACCENT : COL_PANEL_3, 255);
    g_text(x + 11, y + 10, label, on ? 0xFFFFFF : COL_TEXT, 1);
    hot_add(x, y, w, h, kind, val);
    return w;
}

static int section(int x, int y, const char *label) {
    g_fill(x, y, 3, 12, COL_ACCENT);
    g_text(x + 10, y + 2, label, COL_TEXT, 1);
    return y + 22;
}

/* ---- pages -------------------------------------------------------------- */
static int page_session(int x, int y, int right) {
    y = section(x, y, "Signed in");
    g_text(x, y, users_current()[0] ? users_current() : "(nobody)", COL_TEXT, 1);
    g_text(x + 140, y, users_is_root() ? "root" :
                       users_is_admin() ? "admin" : "standard user", COL_TEXT_DIM, 1);
    y += 20;
    g_text(x, y, "Home", COL_TEXT_DIM, 1);
    g_text(x + 140, y, users_home(), COL_TEXT, 1);
    y += 30;

    y = section(x, y, "Lock the screen");
    g_text(x, y, "Ctrl+Alt+L locks immediately.", COL_TEXT_DIM, 1);
    y += 22;
    chip(x, y, "Lock now", 0, K_LOCK, 0);
    y += 40;

    y = section(x, y, "Lock automatically after");
    int bx = x;
    for (int i = 0; i < NIDLE; i++) {
        char lab[16];
        if (IDLE_CHOICES[i] == 0) { lab[0] = 0; kstrlcat(lab, "Never", sizeof lab); }
        else { sh_utoa((uint64_t)IDLE_CHOICES[i], lab); kstrlcat(lab, " min", sizeof lab); }
        if (bx + g_text_width(lab, 1) + 30 > right) { bx = x; y += 34; }
        bx += chip(bx, y, lab, gui_idle_lock() == IDLE_CHOICES[i], K_IDLE, i) + 8;
    }
    return y + 40;
}

static int page_firewall(int x, int y, int right) {
    uint32_t passed = 0, blocked = 0;
    fw_stats(&passed, &blocked);

    y = section(x, y, "Packet filter");
    int bx = x;
    bx += chip(bx, y, fw_enabled() ? "Enabled" : "Disabled", fw_enabled(), K_FW_ON, 0) + 8;
    chip(bx, y, fw_get_default() ? "Default: allow" : "Default: block",
         !fw_get_default(), K_FW_DEF, 0);
    y += 38;

    {
        char pn[16], bn[16];
        sh_utoa(passed, pn); sh_utoa(blocked, bn);
        g_text(x, y, "Passed", COL_TEXT_DIM, 1);  g_text(x + 70, y, pn, COL_GOOD, 1);
        g_text(x + 150, y, "Blocked", COL_TEXT_DIM, 1); g_text(x + 220, y, bn, COL_BAD, 1);
    }
    y += 28;

    y = section(x, y, "Add a rule");
    bx = x;
    for (int i = 0; i < NPRESET; i++) {
        if (bx + g_text_width(PRESETS[i].label, 1) + 30 > right) { bx = x; y += 34; }
        bx += chip(bx, y, PRESETS[i].label, 0, K_FW_PRESET, i) + 8;
    }
    y += 42;

    y = section(x, y, "Rules (first match wins)");
    int n = fw_count();
    if (!n) {
        g_text(x, y, "No rules. The default policy applies to everything.", COL_TEXT_DIM, 1);
        return y + 24;
    }
    for (int i = 0; i < n && y < org_y + 2000; i++) {
        char num[8]; sh_utoa((uint64_t)(i + 1), num);
        g_round(x, y, right - x - 60, 22, 5, COL_PANEL_2, 255);
        g_text(x + 8, y + 5, num, COL_TEXT_DIM, 1);
        g_text(x + 34, y + 5, fw_rule_text(i), COL_TEXT, 1);
        chip(right - 54, y - 3, "x", 0, K_FW_DEL, i);
        y += 26;
    }
    return y + 12;
}

static int page_accounts(int x, int y, int right) {
    (void)right;
    y = section(x, y, "Accounts");
    g_text(x, y, "NAME", COL_TEXT_DIM, 1);
    g_text(x + 140, y, "UID", COL_TEXT_DIM, 1);
    g_text(x + 200, y, "GROUP", COL_TEXT_DIM, 1);
    g_text(x + 280, y, "HOME", COL_TEXT_DIM, 1);
    y += 20;
    for (int i = 0; i < users_count(); i++) {
        const user_t *u = users_get(i);
        char num[12]; sh_utoa((uint64_t)u->uid, num);
        int me = strcmp(u->name, users_current()) == 0;
        g_text(x, y, u->name, me ? COL_ACCENT : COL_TEXT, 1);
        g_text(x + 140, y, num, COL_TEXT_DIM, 1);
        g_text(x + 200, y, u->gid == GID_ADMIN ? "admin" : "users", COL_TEXT_DIM, 1);
        g_text(x + 280, y, u->home, COL_TEXT_DIM, 1);
        y += 18;
    }
    y += 16;
    y = section(x, y, "Managing accounts");
    g_text(x, y, "Accounts are created and removed from the terminal, where", COL_TEXT_DIM, 1);
    y += 16;
    g_text(x, y, "a password can be typed without it appearing on screen:", COL_TEXT_DIM, 1);
    y += 22;
    g_text(x, y, "useradd NAME PASSWORD      passwd [NAME]", COL_TEXT, 1);
    y += 18;
    g_text(x, y, "userdel NAME               sudo COMMAND", COL_TEXT, 1);
    return y + 24;
}

/* ---- window ------------------------------------------------------------- */
static void sec_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    nhot = 0; org_x = cx; org_y = cy;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    int x = cx + 20, y = cy + 16, right = cx + cw - 20;

    static const char *names[NTABS] = { "Session", "Firewall", "Accounts" };
    int bx = x;
    for (int i = 0; i < NTABS; i++) bx += chip(bx, y, names[i], tab == i, K_TAB, i) + 8;
    y += 38;
    g_hline(x, y, right - x, COL_PANEL_3);
    y += 16;

    switch (tab) {
    case TAB_FIREWALL: y = page_firewall(x, y, right); break;
    case TAB_ACCOUNTS: y = page_accounts(x, y, right); break;
    default:           y = page_session(x, y, right);  break;
    }

    if (note[0]) g_text(x, cy + ch - 26, note, COL_WARN, 1);
}

static void sec_click(window_t *w, int lx, int ly) {
    (void)w;
    for (int i = 0; i < nhot; i++) {
        hot_t *h = &hots[i];
        if (lx < h->x || lx >= h->x + h->w || ly < h->y || ly >= h->y + h->h) continue;
        note[0] = 0;
        switch (h->kind) {
        case K_TAB:   tab = h->val; break;
        case K_LOCK:  gui_lock(); break;
        case K_IDLE:  gui_set_idle_lock(IDLE_CHOICES[h->val]); break;
        case K_FW_ON: fw_set_enabled(!fw_enabled()); break;
        case K_FW_DEF:fw_set_default(!fw_get_default()); break;
        case K_FW_DEL:
            /* No per-rule delete in the filter, so rebuild the list without
             * the removed one -- the rule text round-trips through fw_add. */
            {
                char keep[16][64];
                int n = fw_count(), k = 0;
                for (int r = 0; r < n && k < 16; r++) {
                    if (r == h->val) continue;
                    keep[k][0] = 0;
                    kstrlcat(keep[k], fw_rule_text(r), sizeof keep[k]);
                    k++;
                }
                fw_clear();
                for (int r = 0; r < k; r++) {
                    char buf[64]; buf[0] = 0; kstrlcat(buf, keep[r], sizeof buf);
                    char *tok[8]; int nt = 0;
                    for (char *p = buf; *p && nt < 8; ) {
                        while (*p == ' ') p++;
                        if (!*p) break;
                        tok[nt++] = p;
                        while (*p && *p != ' ') p++;
                        if (*p) *p++ = 0;
                    }
                    if (nt) fw_add(nt, tok, keep[r]);
                }
            }
            break;
        case K_FW_PRESET: {
            char buf[64]; buf[0] = 0;
            kstrlcat(buf, PRESETS[h->val].rule, sizeof buf);
            char *tok[8]; int nt = 0;
            for (char *p = buf; *p && nt < 8; ) {
                while (*p == ' ') p++;
                if (!*p) break;
                tok[nt++] = p;
                while (*p && *p != ' ') p++;
                if (*p) *p++ = 0;
            }
            int rc = nt ? fw_add(nt, tok, PRESETS[h->val].rule) : -1;
            if (rc == -2) { note[0] = 0; kstrlcat(note, "Rule table is full", sizeof note); }
            else if (rc != 0) { note[0] = 0; kstrlcat(note, "Could not add that rule", sizeof note); }
            else if (!fw_enabled()) {
                note[0] = 0;
                kstrlcat(note, "Rule added - the filter is still disabled", sizeof note);
            }
            break;
        }
        }
        gui_request_redraw();
        return;
    }
}

void security_app_init(void) {
    window_t *win = gui_add_window("Security", 560, 520, 0xE8506B, ICON_SECURITY);
    if (!win) return;
    win->draw  = sec_draw;
    win->click = sec_click;
    win->min_w = 460; win->min_h = 360;
    win->x = 210; win->y = 70;
}
