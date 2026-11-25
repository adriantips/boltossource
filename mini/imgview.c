/* ===========================================================================
 *  BoltOS Mini  -  mini/imgview.c
 *
 *  A saved picture, full screen: `img --name` in the Terminal. Pictures are
 *  the files the Browser saves into /Pictures on the card (right-click an
 *  image, Save image), kept as they came: PNG, JPEG, GIF or BMP.
 *
 *  While one is up Mini does as little as it can: the picture is drawn once
 *  and then nothing is (a frame only when the hint line comes or goes), the
 *  shell is quiet -- no package or update checks, no Code or Notes work --
 *  Firefox's machine is stopped, and the screen doesn't go to sleep. The
 *  server machine (developer mode) carries on as it was, ahead of all this.
 *
 *  Ctrl+C closes it and goes back to the Terminal; Esc does too, and so does
 *  a click on the line at the bottom that says so. That line shows for the
 *  first few seconds and comes back with any other key or click, so nobody
 *  is left looking at a picture with no idea how to get out.
 * ===========================================================================*/
#include <stdint.h>
#include "ui.h"
#include "fs.h"
#include "image.h"
#include "vm.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

#define PICTURES "/Pictures"

static struct {
    image_t *im;
    char name[FS_NAME];
    int drawn_w, drawn_h, drawn_hint;  /* what's on screen now                    */
    uint64_t hint_until;               /* the "how to close" line until then       */
    int bar_y, bar_h;                  /* where that line is, for a click         */
} V;

const app_t app_imgview;

/* ---- the folder --------------------------------------------------------------------------- */
static int ext_of(const char *n) {                        /* the dot before the extension, or -1 */
    const char *d = strrchr(n, '.');
    return d && d != n ? (int)(d - n) : -1;
}

/* /Pictures/NAME.ext for a name without its extension (any case). 0 if found. */
int pictures_find(const char *name, char *path, int cap) {
    if (!fs_ready() || !name || !*name) return -1;
    fs_ent_t *e = kmalloc(sizeof(fs_ent_t) * 256);
    if (!e) return -1;
    int n = fs_list(PICTURES, e, 256), r = -1;
    size_t nl = strlen(name);
    for (int i = 0; i < n && r; i++) {
        if (e[i].dir) continue;
        int d = ext_of(e[i].name);
        size_t bl = d >= 0 ? (size_t)d : strlen(e[i].name);
        if ((bl == nl && !strncasecmp(e[i].name, name, nl)) || !strcasecmp(e[i].name, name)) {
            snprintf(path, (size_t)cap, PICTURES "/%s", e[i].name);
            r = 0;
        }
    }
    kfree(e);
    return r;
}

/* The saved pictures' names, without extensions, two spaces apart. */
int pictures_list(char *out, int cap) {
    out[0] = 0;
    if (!fs_ready()) return 0;
    fs_ent_t *e = kmalloc(sizeof(fs_ent_t) * 256);
    if (!e) return 0;
    int n = fs_list(PICTURES, e, 256), k = 0, o = 0;
    for (int i = 0; i < n; i++) {
        if (e[i].dir) continue;
        int d = ext_of(e[i].name);
        int bl = d >= 0 ? d : (int)strlen(e[i].name);
        if (o < cap - 1) o += snprintf(out + o, (size_t)(cap - o), "%s%.*s", k ? "  " : "", bl, e[i].name);
        k++;
    }
    kfree(e);
    return k;
}

/* ---- drawing ------------------------------------------------------------------------------- */
static int hint_px(void) { int p = S(17); return p < 13 ? 13 : p; }     /* readable on a 3.5" screen */

static void draw(int hint) {
    gfx_bind(&g_back, 0, 0, 0, 0, scr_w, scr_h);
    gfx_fill(0, 0, scr_w, scr_h, 0x000000);
    if (V.im && V.im->w > 0 && V.im->h > 0) {
        int dw = scr_w, dh = (int)((int64_t)scr_w * V.im->h / V.im->w);
        if (dh > scr_h) { dh = scr_h; dw = (int)((int64_t)scr_h * V.im->w / V.im->h); }
        surface_t s = { V.im->px, V.im->w, V.im->h, V.im->w };
        gfx_surface_scaled((scr_w - dw) / 2, (scr_h - dh) / 2, dw, dh, &s, 0, 255);
    }
    if (hint) {
        int px = hint_px();
        V.bar_h = px * 3;
        V.bar_y = scr_h - V.bar_h;
        gfx_fill(0, V.bar_y, scr_w, V.bar_h, 0x111214);
        gfx_fill(0, V.bar_y, scr_w, 1, 0x2C2E33);
        char l[FS_NAME + 96];
        snprintf(l, sizeof l, "Press Ctrl+C to close %s  " TXT_BULLET "  Esc, or a click here, works too", V.name);
        if (font_width(FONT_MEDIUM, px, l, -1) > scr_w - 2 * px)
            snprintf(l, sizeof l, "Press Ctrl+C to close  " TXT_BULLET "  Esc or a click here too");
        text_c(scr_w / 2, V.bar_y + V.bar_h / 2, l, FONT_MEDIUM, px, 0xFFFFFF);
    }
    screen_present(0, 0, scr_w, scr_h);                  /* only what changed goes out */
    V.drawn_w = scr_w; V.drawn_h = scr_h; V.drawn_hint = hint;
}

static void hint_again(void) { V.hint_until = now_ms() + 4000; }

/* ---- the app -------------------------------------------------------------------------------- */
static void close_view(void) {
    if (V.im) { image_free(V.im); V.im = 0; }
    shell_open(&app_terminal);                            /* also ends quiet and keep-awake */
}

static void iv_key(const input_ev_t *e) {
    int ctrl_c = e->key == KEY_CHAR && (e->mods & MOD_CTRL) && (e->ch == 'c' || e->ch == 'C');
    if (ctrl_c || e->key == KEY_ESC) { close_view(); return; }
    hint_again();                                         /* any other key: say how again */
}

static void iv_pointer(const input_ev_t *e, int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
    static int was;
    int click = (e->buttons & 1) && !(was & 1);
    was = e->buttons;
    if (!click) return;
    if (V.drawn_hint && e->y >= V.bar_y) { close_view(); return; }
    hint_again();
}

static int iv_tick(void) {
    if (!shell_is_fullscreen()) { shell_fullscreen(1); shell_quiet_mode(1, 1); V.drawn_w = 0; }
    int hint = now_ms() < V.hint_until;
    if (hint != V.drawn_hint || V.drawn_w != scr_w || V.drawn_h != scr_h) draw(hint);
    return 0;
}

static void iv_draw(int x, int y, int w, int h) { gfx_fill(x, y, w, h, 0x000000); }   /* full screen draws itself */
static int iv_esc(void) { close_view(); return 1; }

const app_t app_imgview = { "Picture", 0x202124, G_DOT, 0, iv_key, iv_pointer, iv_draw, iv_tick, iv_esc };

/* The Terminal's `img NAME`: 0 once it's showing, else why not in msg. */
int imgview_show(const char *name, char *msg, int cap) {
    char path[FS_PATH];
    if (!fs_ready()) { snprintf(msg, (size_t)cap, "The SD card's files aren't there, so there are no saved pictures."); return -1; }
    if (pictures_find(name, path, sizeof path)) {
        char list[600];
        int n = pictures_list(list, sizeof list);
        if (n) snprintf(msg, (size_t)cap, "No picture called \"%s\". Saved ones: %s", name, list);
        else snprintf(msg, (size_t)cap, "No picture called \"%s\". Save one in the Browser: right-click an image, Save image.", name);
        return -1;
    }
    char *buf = 0; uint32_t len = 0;
    if (fs_read(path, &buf, &len) || !buf) { snprintf(msg, (size_t)cap, "Couldn't read %s: %s", path, fs_why()); return -1; }
    int fit = scr_w > scr_h ? scr_w : scr_h;
    image_t *im = image_decode_upto((const uint8_t *)buf, len, fit);
    kfree(buf);
    if (!im) { snprintf(msg, (size_t)cap, "%s isn't a picture Mini can show (PNG, JPEG, GIF or BMP).", path); return -1; }
    if (V.im) image_free(V.im);
    V.im = im;
    snprintf(V.name, sizeof V.name, "%s", fs_basename(path));
    /* Nothing else while it's up: Firefox's machine is the big one. */
    if (vm_state() == VM_RUNNING || vm_state() == VM_LOADING) { kprintf("[img] stopping Firefox's machine for the picture\n"); vm_stop(); }
    shell_open(&app_imgview);
    shell_fullscreen(1);
    shell_quiet_mode(1, 1);
    V.hint_until = now_ms() + 6000;
    V.drawn_w = 0;
    draw(1);
    kprintf("[img] showing %s, %d x %d\n", path, im->w, im->h);
    return 0;
}
