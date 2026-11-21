/* ===========================================================================
 *  BoltOS TV  -  tv/settings.c
 *
 *  Settings, laid out the way a television's are: categories down the left,
 *  the selected one's controls on the right, Right/OK to go in, Left/Back to
 *  come out. Every control is one of four kinds -- a slider, a choice you
 *  step through with Left and Right, a switch, an action -- plus read-only
 *  rows for the network and About pages.
 *
 *  Settings last until the set is switched off; the defaults below are what
 *  it comes up with every time. The exceptions are automatic updates and
 *  the streaming engine's address, which kernel/sysupdate.c keeps on the
 *  stick the TV started from.
 * ===========================================================================*/
#include <stdint.h>
#include "tv.h"
#include "string.h"
#include "stdio.h"
#include "audio.h"
#include "acpi.h"
#include "hw.h"
#include "kheap.h"
#include "pmm.h"
#include "framebuffer.h"
#include "xhci.h"
#include "keyboard.h"
#include "mouse.h"
#include "buildid.h"
#include "sysupdate.h"
#include "engine.h"

tvsettings_t tvs;

void settings_defaults(void) {
    memset(&tvs, 0, sizeof tvs);
    tvs.volume = 40;
    tvs.brightness = 100;
    tvs.picture = 0;
    tvs.film = -1;
    tvs.film_info = 1;
    tvs.clock24 = 1;
    tvs.utc_offset = 0;
    tvs.sounds = 1;
    tvs.adblock = 1;
}

/* ---- picture ------------------------------------------------------------- *
 * One 3x256 table, applied as the canvas is copied to the panel: brightness
 * and the picture mode folded together, so it is one lookup per channel per
 * pixel, and no pass at all when it would be the identity. */
static uint8_t lut[768];
static int lut_identity = 1;

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* Contrast about mid-grey, in 1/100ths. */
static int curve(int v, int contrast, int lift) {
    return clamp255(((v - 128) * contrast) / 100 + 128 + lift);
}

void settings_apply_picture(void) {
    int b = tvs.brightness;
    lut_identity = tvs.picture == 0 && b >= 100;
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++) {
            int o = v;
            switch (tvs.picture) {
            case 1: o = curve(v, 116, 2); if (c == 2) o = clamp255(o + 3); break;     /* Vivid  */
            case 2: o = curve(v, 92, -4);                                              /* Cinema */
                    if (c == 0) o = clamp255(o * 102 / 100);
                    if (c == 2) o = clamp255(o * 92 / 100);
                    break;
            case 3: if (c == 1) o = v * 95 / 100;                                      /* Warm   */
                    if (c == 2) o = v * 84 / 100;
                    break;
            }
            lut[c * 256 + v] = (uint8_t)clamp255(o * b / 100);
        }
}

const uint8_t *settings_picture_lut(void) { return lut_identity ? 0 : lut; }
void tv_set_brightness(int pct) { tvs.brightness = pct < 10 ? 10 : pct > 100 ? 100 : pct; settings_apply_picture(); }
int  tv_brightness(void) { return tvs.brightness; }

/* ---- the page model ------------------------------------------------------ */
enum { IT_SLIDER, IT_CHOICE, IT_SWITCH, IT_ACTION, IT_INFO, IT_HEAD };
typedef struct {
    int kind;
    const char *label;
    char value[96];            /* filled each frame for CHOICE and INFO */
    int *pv;                   /* SLIDER / SWITCH target                */
    int lo, hi, step;
    int id;                    /* what a CHOICE / ACTION does           */
} item_t;

enum { C_PICTURE, C_SOUND, C_BACKGROUND, C_NETWORK, C_STREAMING, C_SLEEP, C_CLOCK, C_UPDATE, C_ABOUT, C_POWER, C_COUNT };
static const struct { const char *name; int glyph; } CATS[C_COUNT] = {
    { "Picture", GL_SUN }, { "Sound", GL_VOL }, { "Background", GL_IMAGE }, { "Network", GL_ETHER },
    { "Streaming", GL_FILM },
    { "Sleep timer", GL_MOON }, { "Clock", GL_CLOCK }, { "Updates", GL_RELOAD }, { "About", GL_INFO },
    { "Power", GL_POWER },
};

enum { A_PICTURE_MODE = 1, A_FILM, A_SLEEP, A_TZ, A_RESTART, A_OFF, A_UPDATE, A_ENGINE };

/* ---- updates (kernel/sysupdate.c does the work) --------------------------- */
static int upd_auto = 1;                  /* the switch's own copy             */

static int tv_online(void) { return netsvc_state() == NET_UP; }
static const sysupd_host_t tv_host = {
    .product = "BoltOS TV", .version = BOLT_VERSION, .online = tv_online,
#ifdef UPDATE_CHANNEL
    .channel = UPDATE_CHANNEL,            /* release.sh --channel: a test one  */
#else
    .channel = "tv",
#endif
    .fetch = netsvc_get, .toast = tv_toast, .reboot = acpi_reboot, .idle_ms = tv_idle_ms,
};
void tv_update_init(void) { sysupd_init(&tv_host); }

static void mbytes(char *out, int cap, uint32_t b) {
    snprintf(out, (size_t)cap, "%u.%u MB", b / 1000000, b % 1000000 / 100000);
}

static item_t items[24];
static int nitems;
static int cat, col, row;        /* col 0 = categories, 1 = items */
static float cat_y = -1, row_y = -1, scroll_y;
static uint64_t last_tick;
static glasscache_t gc_left, gc_right;

static const char *const MODES[] = { "Standard", "Vivid", "Cinema", "Warm" };
static const int SLEEPS[] = { 0, 15, 30, 45, 60, 90, 120 };
#define NSLEEP ((int)(sizeof SLEEPS / sizeof SLEEPS[0]))

static item_t *add(int kind, const char *label) {
    if (nitems >= 24) return &items[23];
    item_t *it = &items[nitems++];
    memset(it, 0, sizeof *it);
    it->kind = kind;
    it->label = label;
    return it;
}
static void info(const char *label, const char *fmt, const char *v) {
    item_t *it = add(IT_INFO, label);
    snprintf(it->value, sizeof it->value, fmt, v);
}

static void build(void) {
    nitems = 0;
    item_t *it;
    char ip[16];
    switch (cat) {
    case C_PICTURE:
        it = add(IT_SLIDER, "Brightness"); it->pv = &tvs.brightness; it->lo = 10; it->hi = 100; it->step = 5;
        it = add(IT_CHOICE, "Picture mode"); it->id = A_PICTURE_MODE;
        snprintf(it->value, sizeof it->value, "%s", MODES[tvs.picture]);
        add(IT_HEAD, "Picture modes are applied to everything on screen, films and menus alike.");
        break;
    case C_SOUND:
        it = add(IT_SLIDER, "Volume"); it->pv = &tvs.volume; it->lo = 0; it->hi = 100; it->step = 5;
        it = add(IT_SWITCH, "Mute"); it->pv = &tvs.muted;
        it = add(IT_SWITCH, "Interface sounds"); it->pv = &tvs.sounds;
        info("Output", "%s", audio_present() ? audio_name() : "No sound device found");
        break;
    case C_BACKGROUND:
        it = add(IT_CHOICE, "Film"); it->id = A_FILM;
        snprintf(it->value, sizeof it->value, "%s",
                 tvs.film < 0 ? "Shuffle all six" : tv_films[tvs.film].title);
        add(IT_HEAD, "Films are from Pexels, filmed in 4K by the people credited on screen, "
                     "and play as seamless 1080p loops.");
        break;
    case C_NETWORK: {
        static const char *const ST[] = { "Not connected", "Looking for a network adapter",
                                          "Asking for an address", "Connected", "No network" };
        info("Status", "%s", ST[netsvc_state()]);
        info("Adapter", "%s", netsvc_nic()[0] ? netsvc_nic() : "None found");
        if (netsvc_state() == NET_UP) {
            fmt_ip(ip, netsvc_ip());  info("IP address", "%s", ip);
            fmt_ip(ip, netsvc_gw());  info("Router", "%s", ip);
            fmt_ip(ip, netsvc_dns()); info("DNS server", "%s", ip);
        }
        info("Clock", "%s", netsvc_clock_synced() ? "Set from the internet (NTP)" : "From the computer's clock");
        info("Ad blocking", "%s", tvs.adblock ? "On for every lookup" : "Off");
        add(IT_HEAD, "BoltOS TV uses wired Ethernet: Intel e1000-family and virtio adapters.");
        break;
    }
    case C_STREAMING: {
        static char addr[96];
        engine_get_address(addr, sizeof addr);
        it = add(IT_ACTION, addr[0] ? "Change the engine's address" : "Set the engine's address"); it->id = A_ENGINE;
        info("Firefox engine", "%s", addr[0] ? addr : "Not set");
        info("YouTube", "%s", "Plays on the TV itself: no engine needed");
        add(IT_HEAD, "Netflix, Prime Video, Disney+ and HBO Max need a real browser with Widevine. They play in "
                     "Firefox on a computer on your network running the BoltOS Web Engine (webengine/ in the source).");
        add(IT_HEAD, "Write host, host:port (7310 if left out) or key@host:port if the engine has a key.");
        break;
    }
    case C_SLEEP: {
        it = add(IT_CHOICE, "Switch off after"); it->id = A_SLEEP;
        if (tvs.sleep_min) snprintf(it->value, sizeof it->value, "%d minutes", tvs.sleep_min);
        else snprintf(it->value, sizeof it->value, "Off");
        if (tvs.sleep_at) {
            int h, m;
            tv_clock(&h, &m, 0, 0, 0, 0, 0);
            int left = (int)((tvs.sleep_at - tv_now_ms()) / 60000);
            int mm = m + left + 1;
            char t[16];
            fmt_time(t, sizeof t, (h + mm / 60) % 24, mm % 60);
            it = add(IT_INFO, "Turns off at");
            snprintf(it->value, sizeof it->value, "%s", t);
        }
        break;
    }
    case C_CLOCK:
        it = add(IT_SWITCH, "24-hour clock"); it->pv = &tvs.clock24;
        it = add(IT_CHOICE, "Time zone"); it->id = A_TZ;
        snprintf(it->value, sizeof it->value, "UTC%s%d", tvs.utc_offset >= 0 ? "+" : "", tvs.utc_offset);
        if (!tvs.utc_offset) snprintf(it->value, sizeof it->value, "UTC");
        info("Source", "%s", netsvc_clock_synced() ? "Internet time (NTP)" : "The computer's clock");
        break;
    case C_UPDATE: {
        const sysupd_t *u = sysupd_state();
        static char act_label[48], line[160], notes[480];
        upd_auto = sysupd_auto();
        it = add(IT_SWITCH, "Automatic updates"); it->pv = &upd_auto;
        int busy = u->state == SU_CHECKING || u->state == SU_DOWNLOADING || u->state == SU_INSTALLING;
        if (!busy) {
            if (u->state == SU_RESTART) snprintf(act_label, sizeof act_label, "Restart now");
            else if (u->have) snprintf(act_label, sizeof act_label, "%s %s", u->state == SU_FAILED ? "Try again:" : "Update to", u->version);
            else snprintf(act_label, sizeof act_label, "Check for updates");
            it = add(IT_ACTION, act_label); it->id = A_UPDATE;
        }
        info("Software", "%s", "BoltOS TV " BOLT_VERSION);
        if (u->state == SU_DOWNLOADING && u->size) {
            char g[16], t[16];
            mbytes(g, sizeof g, u->got > u->size ? u->size : u->got); mbytes(t, sizeof t, u->size);
            snprintf(line, sizeof line, "Downloading %s: %s of %s", u->version, g, t);
            info("Status", "%s", line);
        } else info("Status", "%s", u->msg);
        if (u->have) {
            char sz[16]; mbytes(sz, sizeof sz, u->size);
            snprintf(line, sizeof line, "%s  \x95  %s update  \x95  %s%s%s", u->version, u->big ? "big" : "quick", sz,
                     u->date[0] ? "  \x95  " : "", u->date);
            info("Newest", "%s", line);
            int n = 0;
            for (const char *p = u->notes; *p && n < (int)sizeof notes - 4; p++) {
                if (*p == '\n') { if (n) { notes[n++] = ' '; notes[n++] = '\x95'; notes[n++] = ' '; } }
                else notes[n++] = *p;
            }
            notes[n] = 0;
            if (n) add(IT_HEAD, notes);
        }
        if (u->disk[0]) info("Installs to", "%s", u->disk);
        add(IT_HEAD, upd_auto ? "Quick updates install by themselves when the TV starts. Big ones wait until you choose Update."
                              : "Updates wait for you: choose Update when it suits you.");
        break;
    }
    case C_ABOUT: {
        info("Software", "%s", "BoltOS TV " BOLT_VERSION);
        info("Build", "%s", BOLT_BUILD_ID);
        static char cpu[49];
        hw_cpu_brand(cpu);
        char *c = cpu; while (*c == ' ') c++;
        info("Processor", "%s", c[0] ? c : "x86-64");
        static char mem[48], disp[48], inp[48];
        uint64_t used, total;
        kheap_usage(&used, &total);
        snprintf(mem, sizeof mem, "%lu MiB kernel heap, %lu MiB in use",
                 (unsigned long)(total >> 20), (unsigned long)(used >> 20));
        info("Memory", "%s", mem);
        snprintf(disp, sizeof disp, "%ux%u, menus at %dx%d", fb_width(), fb_height(), tv_w, tv_h);
        info("Display", "%s", disp);
        int uk = 0, um = 0; xhci_hid_counts(&uk, &um);
        int pk = kbd_ps2_keyboard(), pm = mouse_ps2_present();
        snprintf(inp, sizeof inp, "%d keyboard%s or remote%s, %d pointer%s (USB and PS/2)", uk + pk,
                 uk + pk == 1 ? "" : "s", uk + pk == 1 ? "" : "s", um + pm, um + pm == 1 ? "" : "s");
        info("Remote", "%s", inp);
        add(IT_HEAD, "Made by Adrian Derakhshideh  \x95  boltos.dev  \x95  github.com/adriantips/boltossource");
        add(IT_HEAD, "Films from Pexels by Cl\xE9ment Proust, Kenan Turgu\xE7, Oskar Gross, T Honkamies, "
                     "Logan Voss and AP Vibes. MPEG-1 decoding by pl_mpeg (MIT). Type: Inter. Icons: Lucide (ISC).");
        add(IT_HEAD, "H.264 decoding by BoltOS Mini's decoder. AAC tables from ISO/IEC 14496-3, as typed in by FFmpeg (LGPL-2.1).");
        break;
    }
    case C_POWER:
        it = add(IT_ACTION, "Restart"); it->id = A_RESTART;
        it = add(IT_ACTION, "Switch off"); it->id = A_OFF;
        break;
    }
}

void settings_open_about(void) { cat = C_ABOUT; col = 0; row = 0; }
void settings_open_streaming(void) { cat = C_STREAMING; col = 0; row = 0; }

static osk_t kb;                 /* the engine's address, being typed */

static int focusable(int i) {
    return i >= 0 && i < nitems && items[i].kind != IT_INFO && items[i].kind != IT_HEAD;
}
static int first_focusable(void) {
    for (int i = 0; i < nitems; i++) if (focusable(i)) return i;
    return -1;
}

static void changed(item_t *it) {
    if (it->pv == &upd_auto) sysupd_set_auto(upd_auto);
    if (it->pv == &tvs.brightness) settings_apply_picture();
    if (it->pv == &tvs.volume || it->pv == &tvs.muted) tv_volume_changed();
}

static void choose(item_t *it, int dir) {
    switch (it->id) {
    case A_PICTURE_MODE: tvs.picture = (tvs.picture + 4 + dir) % 4; settings_apply_picture(); break;
    case A_FILM: {
        int f = tvs.film + dir;                       /* -1 .. n-1, wrapping */
        if (f < -1) f = tv_nfilms - 1;
        if (f >= tv_nfilms) f = -1;
        tvs.film = f;
        break;
    }
    case A_SLEEP: {
        int i = 0;
        while (i < NSLEEP && SLEEPS[i] != tvs.sleep_min) i++;
        i = (i + NSLEEP + dir) % NSLEEP;
        tvs.sleep_min = SLEEPS[i];
        tvs.sleep_at = tvs.sleep_min ? tv_now_ms() + (uint64_t)tvs.sleep_min * 60000 : 0;
        break;
    }
    case A_TZ:
        tvs.utc_offset += dir;
        if (tvs.utc_offset < -12) tvs.utc_offset = 14;
        if (tvs.utc_offset > 14) tvs.utc_offset = -12;
        break;
    }
}

static void act(item_t *it) {
    if (it->kind == IT_SWITCH) { *it->pv = !*it->pv; changed(it); sfx_select(); return; }
    if (it->kind == IT_CHOICE) { choose(it, 1); sfx_move(); return; }
    if (it->kind != IT_ACTION) return;
    sfx_select();
    if (it->id == A_RESTART) { tv_toast("Restarting", 0); acpi_reboot(); }
    if (it->id == A_ENGINE) {
        char a[96]; engine_get_address(a, sizeof a);
        osk_begin(&kb, a, "Engine address, like 192.168.1.20", "Save");
        return;
    }
    if (it->id == A_UPDATE) {
        const sysupd_t *u = sysupd_state();
        if (u->state == SU_RESTART) sysupd_restart();
        else if (u->have) sysupd_install();
        else sysupd_check();
    }
    if (it->id == A_OFF) {
        acpi_poweroff();
        tv_toast("Could not switch off", "This machine did not answer the ACPI power-off. Hold its power button.");
    }
}

static void settings_enter(void) {
    build();
    if (col == 1 && !focusable(row)) row = first_focusable();
}

static void settings_key(tvkey_t k) {
    if (kb.active) {
        if (osk_key(&kb, k) == 1) { engine_set_address(kb.text); tv_toast(kb.text[0] ? "Engine address saved" : "Engine address cleared", kb.text); }
        return;
    }
    build();
    if (col == 0) {
        int old = cat;
        switch (k.k) {
        case K_UP: if (cat > 0) cat--; break;
        case K_DOWN: if (cat < C_COUNT - 1) cat++; break;
        case K_RIGHT: case K_OK:
            build();
            row = first_focusable();
            if (row >= 0) { col = 1; sfx_select(); }
            return;
        case K_BACK: case K_LEFT: sfx_back(); tv_pop(); return;
        default: return;
        }
        if (cat != old) { sfx_move(); scroll_y = 0; }
        return;
    }
    item_t *it = &items[row];
    switch (k.k) {
    case K_UP:
        for (int i = row - 1; i >= 0; i--) if (focusable(i)) { row = i; sfx_move(); break; }
        break;
    case K_DOWN:
        for (int i = row + 1; i < nitems; i++) if (focusable(i)) { row = i; sfx_move(); break; }
        break;
    case K_LEFT: case K_RIGHT: {
        int dir = k.k == K_RIGHT ? 1 : -1;
        if (it->kind == IT_SLIDER) {
            int v = *it->pv + dir * it->step;
            *it->pv = v < it->lo ? it->lo : v > it->hi ? it->hi : v;
            changed(it);
            sfx_move();
        } else if (it->kind == IT_CHOICE) { choose(it, dir); sfx_move(); }
        else if (it->kind == IT_SWITCH) { *it->pv = dir > 0; changed(it); sfx_move(); }
        else if (k.k == K_LEFT) { col = 0; sfx_back(); }
        break;
    }
    case K_OK: act(it); break;
    case K_BACK: col = 0; sfx_back(); break;
    }
}

/* geometry */
#define LX 56
#define LW 520
#define RX (LX + LW + 32)
#define CROW 92
#define IROW 108
#define INFO_ROW 80
static int row_h(const item_t *it) { return it->kind == IT_HEAD ? S(80) : it->kind == IT_INFO ? S(INFO_ROW) : S(IROW); }

static void settings_pointer(int x, int y, int click, int wheel) {
    (void)wheel;
    if (kb.active) return;
    build();
    for (int i = 0; i < C_COUNT; i++)
        if (ui_hit(x, y, S(LX + 16), S(LX + 150 + i * CROW), S(LW - 32), S(CROW))) {
            if (cat != i || col != 0) { cat = i; col = 0; sfx_move(); }
            return;
        }
    int iy = S(LX + 150);
    for (int i = 0; i < nitems; i++) {
        int h = row_h(&items[i]);
        if (focusable(i) && ui_hit(x, y, S(RX + 16), iy, tv_w - S(RX) - S(56) - S(32), h)) {
            col = 1; row = i;
            if (click) act(&items[i]);
            return;
        }
        iy += h;
    }
}

static void settings_tick(uint64_t now) {
    uint64_t dt = last_tick ? now - last_tick : 16;
    last_tick = now;
    float tc = (float)cat, tr = (float)row;
    if (cat_y < 0) cat_y = tc;
    if (row_y < 0) row_y = tr;
    float nc = ui_ease(cat_y, tc, dt, 150), nr = ui_ease(row_y, tr, dt, 150);
    if (nc != cat_y || nr != row_y || tv_view_t() < 1.0f) tv_redraw();
    cat_y = nc; row_y = nr;
    if (kb.active && now % 530 < 17) tv_redraw();
    if (cat == C_NETWORK || cat == C_SLEEP || cat == C_ABOUT || cat == C_UPDATE) {
        static uint64_t last;
        if (now - last > 1000) { last = now; tv_redraw(); }
    }
}

static void settings_draw(uint64_t now) {
    build();
    float vt = tv_view_t();
    int a = (int)(vt * 255);
    int slide = (int)((1.0f - vt) * S(40));

    /* categories */
    int lx = S(LX) - slide, ly = S(56), lw = S(LW), lh = tv_h - S(112);
    ui_glass(&gc_left, lx, ly, lw, lh, S(40), 160, a);
    art_tile(ART_SETTINGS, lx + S(36), ly + S(34), S(56), a);
    ui_text(FONT_SEMIBOLD, S(40), lx + S(110), ly + S(62), "Settings", 0xFFFFFF, a, 0);
    int fy = S(LX + 150) + (int)(cat_y * S(CROW));
    ui_focus(lx + S(16), fy, lw - S(32), S(CROW) - S(8), S(26), col == 0 ? a : a * 90 / 255);
    for (int i = 0; i < C_COUNT; i++) {
        int cy = S(LX + 150) + i * S(CROW) + (S(CROW) - S(8)) / 2;
        int on = i == cat && col == 0;
        uint32_t ink = on ? 0x101318 : 0xFFFFFF;
        art_glyph(CATS[i].glyph, lx + S(60), cy, S(30), ink, a);
        ui_text(FONT_MEDIUM, S(29), lx + S(98), cy, CATS[i].name, ink, a, 0);
        if (i == cat && col == 1) art_glyph(GL_CHEV_R, lx + lw - S(50), cy, S(24), 0xFFFFFF, a * 150 / 255);
    }

    /* the page */
    int rx = S(RX) + slide, rw = tv_w - S(RX) - S(56), ry = S(56), rh = tv_h - S(112);
    ui_glass(&gc_right, rx, ry, rw, rh, S(40), 160, a);
    ui_text(FONT_SEMIBOLD, S(40), rx + S(44), ry + S(62), CATS[cat].name, 0xFFFFFF, a, 0);
    int iy = S(LX + 150);
    if (col == 1 && focusable(row)) {
        int fyy = iy;
        for (int i = 0; i < row; i++) fyy += row_h(&items[i]);
        ui_focus(rx + S(16), fyy, rw - S(32), S(IROW) - S(8), S(26), a);
    }
    for (int i = 0; i < nitems; i++) {
        item_t *it = &items[i];
        if (it->kind == IT_HEAD) {
            ui_wrap(FONT_REGULAR, S(21), rx + S(44), iy + S(14), rw - S(88), S(30), 2, it->label, 0x9AA2B0, a);
            iy += S(80);
            continue;
        }
        int on = col == 1 && i == row;
        int cy = iy + (row_h(it) - S(8)) / 2;
        uint32_t ink = on ? 0x101318 : 0xFFFFFF, dim = on ? 0x3A404C : 0xB9C0CC;
        ui_text(FONT_MEDIUM, S(29), rx + S(44), cy, it->label, ink, a, 0);
        int right = rx + rw - S(44);
        switch (it->kind) {
        case IT_SLIDER: {
            int sw = S(420);
            if (on) {
                int pct = (*it->pv - it->lo) * 100 / (it->hi - it->lo);
                char v[8]; snprintf(v, sizeof v, "%d", *it->pv);
                ui_text(FONT_SEMIBOLD, S(26), right, cy, v, ink, a, 2);
                gfx_rrect(right - S(64) - sw, cy - S(5), sw, S(10), S(5), 0x101318, 40);
                gfx_rrect(right - S(64) - sw, cy - S(5), sw * pct / 100 > S(10) ? sw * pct / 100 : S(10),
                          S(10), S(5), 0x101318, 230);
                gfx_circle((float)(right - S(64) - sw + sw * pct / 100), (float)cy, (float)S(14), 0x101318, 255);
            } else {
                char v[8]; snprintf(v, sizeof v, "%d", *it->pv);
                ui_text(FONT_SEMIBOLD, S(26), right, cy, v, dim, a, 2);
                int pct = (*it->pv - it->lo) * 100 / (it->hi - it->lo);
                ui_progress(right - S(64) - sw, cy - S(4), sw, S(8), pct, 0xFFFFFF);
            }
            break;
        }
        case IT_CHOICE: {
            int w = ui_text(FONT_MEDIUM, S(26), right - S(40), cy, it->value, ink, a, 2);
            art_glyph(GL_CHEV_R, right - S(12), cy, S(24), ink, on ? a : a * 120 / 255);
            if (on) art_glyph(GL_CHEV_L, right - S(40) - w - S(28), cy, S(24), ink, a);
            break;
        }
        case IT_SWITCH:
            if (on) {
                int w = S(76), h = S(44);
                gfx_rrect(right - w, cy - h / 2, w, h, h / 2, *it->pv ? 0x34C759 : 0x101318, *it->pv ? 255 : 50);
                int kx = *it->pv ? right - h / 2 : right - w + h / 2;
                gfx_circle((float)kx, (float)cy, (float)(h / 2 - S(4)), 0xFFFFFF, 255);
            } else {
                ui_switch(right - S(76), cy, *it->pv, 0);
            }
            break;
        case IT_ACTION:
            art_glyph(it->id == A_OFF ? GL_POWER : it->id == A_UPDATE || it->id == A_ENGINE ? GL_CHEV_R : GL_RESTART, right - S(16), cy, S(28), ink, a);
            break;
        case IT_INFO:
            font_draw_fit(FONT_REGULAR, S(25), rx + S(330), cy + font_capheight(FONT_REGULAR, S(25)) / 2,
                          it->value, rw - S(330) - S(44), dim, a);
            break;
        }
        iy += row_h(it);
    }
    if (kb.active) osk_draw(&kb, now, 255);
}

const view_t view_settings = {
    .name = "settings", .backdrop = BG_FILM_DIM,
    .enter = settings_enter, .key = settings_key, .pointer = settings_pointer,
    .draw = settings_draw, .tick = settings_tick,
};
