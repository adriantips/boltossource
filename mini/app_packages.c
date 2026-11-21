/* ===========================================================================
 *  BoltOS Mini  -  mini/app_packages.c
 *
 *  Packages: the apps boltos.dev offers for BoltOS Mini, and the ones
 *  installed. Up and down choose one; Return installs it (or opens it once
 *  it's in), Backspace twice removes it and what it downloaded, R asks
 *  boltos.dev for the list again. Installing goes on while you're elsewhere;
 *  the app shows up in the bar when it's done. The work is pkg.c's.
 * ===========================================================================*/
#include <stdint.h>
#include "ui.h"
#include "pkg.h"
#include "store.h"
#include "string.h"
#include "stdio.h"

uint32_t inet_dns_server(void);

typedef struct {
    char id[32], name[48], version[16];
    const pkg_offer_t *offer;             /* NULL: installed, but not on the list */
} row_t;
#define MAX_ROWS 16
static row_t rows[MAX_ROWS];
static int nrows, sel, confirm;
static uint64_t opened_at;

static void human(uint64_t b, char *out, int cap) {
    if (b >= (1ull << 30)) snprintf(out, (size_t)cap, "%lu.%lu GB", (unsigned long)(b >> 30), (unsigned long)((b * 10 >> 30) % 10));
    else if (b >= (1ull << 20)) snprintf(out, (size_t)cap, "%lu.%lu MB", (unsigned long)(b >> 20), (unsigned long)((b * 10 >> 20) % 10));
    else snprintf(out, (size_t)cap, "%lu KB", (unsigned long)((b + 1023) >> 10));
}

/* What's offered, then what's installed that isn't. */
static void build_rows(void) {
    const pkg_offer_t *o;
    int n = pkg_catalog(&o);
    nrows = 0;
    for (int i = 0; i < n && nrows < MAX_ROWS; i++) {
        row_t *r = &rows[nrows++];
        snprintf(r->id, sizeof r->id, "%s", o[i].id);
        snprintf(r->name, sizeof r->name, "%s", o[i].name);
        snprintf(r->version, sizeof r->version, "%s", o[i].version);
        r->offer = &o[i];
    }
    for (int i = 0; i < STORE_MAX && nrows < MAX_ROWS; i++) {
        const store_entry_t *e = store_at(i);
        if (!e || strncmp(e->name, "pkg/", 4) || !(e->flags & STORE_DONE)) continue;
        char id[32];
        snprintf(id, sizeof id, "%.*s", (int)(strlen(e->name) - 8), e->name + 4);
        int have = 0;
        for (int k = 0; k < nrows; k++) if (!strcmp(rows[k].id, id)) have = 1;
        if (have) continue;
        row_t *r = &rows[nrows++];
        snprintf(r->id, sizeof r->id, "%s", id);
        const char *bar = strchr(e->meta, '|');
        snprintf(r->name, sizeof r->name, "%.*s", bar ? (int)(bar - e->meta) : (int)strlen(e->meta), e->meta);
        if (!r->name[0]) snprintf(r->name, sizeof r->name, "%s", id);
        snprintf(r->version, sizeof r->version, "%s", bar ? bar + 1 : "");
        r->offer = 0;
    }
    if (sel >= nrows) sel = nrows ? nrows - 1 : 0;
}

static int busy_with(const row_t *r) {
    const pkg_job_t *j = pkg_job();
    return (j->state == PK_FETCHING || j->state == PK_INSTALLING) && !strcmp(j->id, r->id);
}

static void activate(void) {
    if (sel >= nrows) return;
    row_t *r = &rows[sel];
    const app_t *app;
    char ver[16];
    int inst = pkg_installed(r->id, &app, ver, sizeof ver);
    int update = inst && r->offer && strcmp(ver, r->offer->version);
    if (busy_with(r)) return;
    if (inst && !update && app) { shell_open(app); return; }
    if (r->offer) pkg_install(r->offer);
}

static void p_open(void) {
    confirm = 0;
    opened_at = now_ms();
    store_open(0);
    if (pkg_catalog(0) == -2 || !opened_at || pkg_catalog(0) == -1) pkg_refresh();
    build_rows();
}

static void p_key(const input_ev_t *e) {
    switch (e->key) {
    case KEY_UP:    if (sel > 0) sel--; confirm = 0; break;
    case KEY_DOWN:  if (sel < nrows - 1) sel++; confirm = 0; break;
    case KEY_ENTER: confirm = 0; activate(); break;
    case KEY_BACKSPACE: case KEY_DELETE:
        if (sel >= nrows || !pkg_installed(rows[sel].id, 0, 0, 0)) break;
        if (confirm) { pkg_remove(rows[sel].id); confirm = 0; build_rows(); }
        else confirm = 1;
        break;
    case KEY_CHAR:
        if (e->ch == 'r' || e->ch == 'R') { pkg_refresh(); confirm = 0; }
        break;
    }
}

static int list_y, row_h, list_x, list_w;

static void p_pointer(const input_ev_t *e, int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
    static int was;
    int click = (e->buttons & 1) && !(was & 1);
    was = e->buttons;
    if (!click) return;
    for (int i = 0; i < nrows; i++)
        if (ui_hit(e->x, e->y, list_x, list_y + i * row_h, list_w, row_h - S(6))) {
            if (sel == i) activate(); else { sel = i; confirm = 0; }
            return;
        }
}

static int last_sig;
static int p_tick(void) {
    const pkg_job_t *j = pkg_job();
    const pkg_offer_t *o;
    int sig = pkg_catalog(&o) * 7 + j->state * 131 + (int)(j->got >> 14) + store_count() * 17 + (inet_dns_server() != 0);
    if (sig != last_sig) { last_sig = sig; build_rows(); return 1; }
    return 0;
}

static void draw_status(int x, int y, const row_t *r, int inst, const char *ver, const app_t *app) {
    const pkg_job_t *j = pkg_job();
    char b[128], sz[24];
    if (busy_with(r)) {
        int pct = j->total ? (int)(j->got * 100 / j->total) : 0;
        snprintf(b, sizeof b, j->state == PK_INSTALLING ? "Installing" : "Downloading %d%%", pct);
        text_r(x, y, b, FONT_MEDIUM, S(13), th.accent);
        return;
    }
    if (inst && r->offer && strcmp(ver, r->offer->version)) snprintf(b, sizeof b, "Update to %s", r->offer->version);
    else if (inst && app) snprintf(b, sizeof b, "Installed  " TXT_RARROW " Open");
    else if (inst) snprintf(b, sizeof b, "Installed (didn't start)");
    else if (r->offer) { human(r->offer->size, sz, sizeof sz); snprintf(b, sizeof b, "Install  %s", sz); }
    else b[0] = 0;
    text_r(x, y, b, FONT_MEDIUM, S(13), inst && !(r->offer && strcmp(ver, r->offer->version)) ? th.sub : th.accent);
}

static void p_draw(int x, int y, int w, int h) {
    int px = x + S(40), pw = w - S(80);
    int hy = y + S(36);
    text_l(px, hy, "Packages", FONT_SEMIBOLD, S(22), th.text);
    text_l(px, hy + S(28), "Apps for BoltOS Mini from boltos.dev. They come over Ethernet and stay on the SD card.",
           FONT_REGULAR, S(13), th.sub);

    const pkg_offer_t *o;
    int cat = pkg_catalog(&o);
    list_x = px; list_w = pw; list_y = hy + S(64); row_h = S(62);
    int ly = list_y;
    if (cat == -1 && !nrows) text_l(px, ly + S(16), "Asking boltos.dev what there is\x85", FONT_REGULAR, S(14), th.sub);
    for (int i = 0; i < nrows; i++) {
        row_t *r = &rows[i];
        int ry = ly + i * row_h, rh = row_h - S(6), on = i == sel;
        gfx_rrect(px, ry, pw, rh, S(4), on ? th.accent : th.line, 255);
        gfx_rrect(px + 1, ry + 1, pw - 2, rh - 2, S(3), on ? th.sel : th.surface, 255);
        const app_t *app;
        char ver[16] = "";
        int inst = pkg_installed(r->id, &app, ver, sizeof ver);
        ui_glyph(app ? app->glyph : G_PACKAGE, px + S(30), ry + rh / 2, S(26), on ? th.accent : th.sub);
        char t[96];
        snprintf(t, sizeof t, "%s", r->name);
        int tw = text_l(px + S(58), ry + S(19), t, FONT_SEMIBOLD, S(15), th.text);
        snprintf(t, sizeof t, "%s", inst ? ver : r->version);
        text_l(px + S(58) + tw + S(8), ry + S(19), t, FONT_MONO, S(11), th.faint);
        text_fit_l(px + S(58), ry + S(39), r->offer ? r->offer->summary : "Installed. boltos.dev's list isn't here right now.",
                   pw - S(320), FONT_REGULAR, S(13), th.sub);
        draw_status(px + pw - S(18), ry + rh / 2, r, inst, ver, app);
        if (busy_with(r)) {
            const pkg_job_t *j = pkg_job();
            int bw = pw - S(2), fill = j->total ? (int)((uint64_t)bw * j->got / j->total) : 0;
            gfx_fill(px + 1, ry + rh - S(3), fill, S(2), th.accent);
        }
    }

    /* the chosen one, in full */
    int dy = ly + (nrows ? nrows : 1) * row_h + S(18);
    if (sel < nrows) {
        row_t *r = &rows[sel];
        if (r->offer && r->offer->about[0]) {
            dy += text_wrap(px, dy, pw, r->offer->about, FONT_REGULAR, S(13), S(19), 6, th.sub) * S(19) + S(4);
        }
        char b[200];
        if (r->offer && r->offer->needs[0]) {
            snprintf(b, sizeof b, "Needs: %s", r->offer->needs);
            text_l(px, dy + S(8), b, FONT_REGULAR, S(13), th.faint);
            dy += S(24);
        }
        const pkg_job_t *j = pkg_job();
        const char *err = pkg_load_error(r->id);
        if (j->state == PK_FAILED && !strcmp(j->id, r->id)) {
            snprintf(b, sizeof b, "It didn't install: %s", j->msg);
            text_l(px, dy + S(8), b, FONT_MEDIUM, S(13), 0xE5534B);
        } else if (err) {
            snprintf(b, sizeof b, "It's installed but didn't start: %s", err);
            text_l(px, dy + S(8), b, FONT_MEDIUM, S(13), 0xE5534B);
        } else if (confirm) {
            text_l(px, dy + S(8), "Press Backspace again to remove it, with everything it downloaded.", FONT_MEDIUM, S(13), 0xE0A030);
        }
    }

    /* the bottom line: what's wrong, where things are */
    char b[200], fr[24];
    int by = y + h - S(24);
    if (cat == -2) {
        snprintf(b, sizeof b, "boltos.dev can't be reached (%s). Join a network in Settings, then press R.", pkg_catalog_error());
        text_l(px, by - S(22), b, FONT_REGULAR, S(12), 0xE0A030);
    }
    {
        if (store_is_open()) { human(store_free(), fr, sizeof fr); snprintf(b, sizeof b, "SD card: %s free", fr); }
        else snprintf(b, sizeof b, "SD card: %s", store_why()[0] ? store_why() : "not looked at yet");
        text_l(px, by, b, FONT_REGULAR, S(12), th.faint);
    }
    text_r(px + pw, by, "Up/Down  choose     Return  install / open     Backspace  remove     R  refresh",
           FONT_REGULAR, S(11), th.faint);
}

const app_t app_packages = { "Packages", 0x6B7280, G_PACKAGE, p_open, p_key, p_pointer, p_draw, p_tick, 0 };
