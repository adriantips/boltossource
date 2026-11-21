/* ===========================================================================
 *  BoltOS Mini  -  mini/hid.c
 *
 *  A HID report descriptor read into a list of fields, and reports read
 *  with it: what a Bluetooth keyboard, mouse or game controller sends when
 *  it isn't in the simple "boot" format (bt.c gets the descriptor from SDP
 *  for classic devices and from the Report Map for LE ones).
 *
 *    keyboard   page 7: the keys held (an array) and the modifiers
 *    mouse      X, Y and the wheel when they're relative, page 9 buttons
 *    gamepad    the hat switch, or X and Y when they're absolute, as the
 *               arrow keys; button 1 is Return, 2 is Esc, 3 Backspace, 4 Tab
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "hid.h"
#include "string.h"

void hid_pointer_move(int dx, int dy, int btn, int wheel);

int hid_parse(hid_map_t *m, const uint8_t *d, int len) {
    memset(m, 0, sizeof *m);
    uint16_t page = 0, pos[256];
    int32_t lmin = 0, lmax = 0;
    int size = 0, count = 0, rid = 0;
    uint32_t usages[32]; int nus = 0;
    uint32_t umin = 0, umax = 0; int have_range = 0;
    memset(pos, 0, sizeof pos);
    for (int i = 0; i < len; ) {
        uint8_t b = d[i];
        if (b == 0xFE) { i += 3 + (i + 1 < len ? d[i + 1] : 0); continue; }   /* long item */
        int sz = b & 3; if (sz == 3) sz = 4;
        if (i + 1 + sz > len) break;
        uint32_t v = 0;
        for (int k = 0; k < sz; k++) v |= (uint32_t)d[i + 1 + k] << (8 * k);
        int32_t sv = sz == 1 ? (int8_t)v : sz == 2 ? (int16_t)v : (int32_t)v;
        int tag = b & 0xFC;
        switch (tag) {
        case 0x04: page = (uint16_t)v; break;
        case 0x14: lmin = sv; break;
        case 0x24: lmax = sz && lmin >= 0 ? (int32_t)v : sv; break;
        case 0x74: size = (int)v; break;
        case 0x84: rid = (int)v & 255; m->has_ids = 1; break;
        case 0x94: count = (int)v; break;
        case 0x08: if (nus < 32) usages[nus++] = sz == 4 ? v : (uint32_t)page << 16 | v; break;
        case 0x18: umin = sz == 4 ? v : (uint32_t)page << 16 | v; have_range = 1; break;
        case 0x28: umax = sz == 4 ? v : (uint32_t)page << 16 | v; have_range = 1; break;
        case 0x80:                                          /* Input */
            if (!(v & 1)) {                                 /* not padding */
                if (v & 2) {                                /* variables: one each */
                    for (int k = 0; k < count && m->nel < HID_MAXEL; k++) {
                        uint32_t u = nus ? usages[k < nus ? k : nus - 1] : have_range ? umin + (uint32_t)k : 0;
                        if (have_range && !nus && umin + (uint32_t)k > umax) break;
                        hid_el_t *e = &m->el[m->nel++];
                        e->rid = (uint8_t)rid; e->flags = (uint8_t)(1 | (v & 4 ? 2 : 0));
                        e->size = (uint8_t)size; e->count = 1;
                        e->page = (uint16_t)(u >> 16); e->usage = (uint16_t)u;
                        e->bit = (uint16_t)(pos[rid] + k * size);
                        e->lmin = lmin; e->lmax = lmax;
                    }
                } else if (m->nel < HID_MAXEL) {            /* an array of usages */
                    hid_el_t *e = &m->el[m->nel++];
                    uint32_t u0 = have_range ? umin : nus ? usages[0] : 0, u1 = have_range ? umax : u0;
                    e->rid = (uint8_t)rid; e->flags = 0;
                    e->size = (uint8_t)size; e->count = (uint8_t)(count > 255 ? 255 : count);
                    e->page = (uint16_t)(u0 >> 16); e->usage = (uint16_t)u0; e->umax = (uint16_t)u1;
                    e->bit = pos[rid];
                    e->lmin = lmin; e->lmax = lmax;
                }
            }
            pos[rid] = (uint16_t)(pos[rid] + size * count);
            nus = 0; have_range = 0;
            break;
        case 0x90: case 0xB0: case 0xA0: case 0xC0:          /* output, feature, collections */
            nus = 0; have_range = 0;
            break;
        }
        i += 1 + sz;
    }
    for (int k = 0; k < m->nel; k++) {
        hid_el_t *e = &m->el[k];
        if (e->page == 7) m->keyboard = 1;
        if (e->page == 1 && (e->usage == 0x30 || e->usage == 0x31) && (e->flags & 2)) m->mouse = 1;
        if (e->page == 1 && (e->usage == 0x39 || ((e->usage == 0x30 || e->usage == 0x31) && !(e->flags & 2)))) m->gamepad = 1;
    }
    return m->keyboard || m->mouse || m->gamepad ? 0 : -1;
}

static int32_t field(const uint8_t *r, int n, int bit, int size, int sign) {
    if (size <= 0 || size > 32 || (bit + size + 7) / 8 > n) return 0;
    uint32_t v = 0;
    for (int i = 0; i < size; i++) if (r[(bit + i) >> 3] >> ((bit + i) & 7) & 1) v |= 1u << i;
    if (sign && size < 32 && (v >> (size - 1) & 1)) v |= ~0u << size;
    return (int32_t)v;
}

static void press(hid_state_t *st, int key, int ch, int mods, int source) {
    input_key(key, ch, mods, source);
    st->rep_key = key; st->rep_ch = ch; st->rep_mods = mods;
    st->rep_at = now_ms() + 450;
}

static const int dir_keys[4] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT };
static const int btn_keys[4] = { KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_TAB };

void hid_report(const hid_map_t *m, hid_state_t *st, int rid, const uint8_t *r, int n, int source) {
    if (rid < 0 && m->has_ids) { if (n < 1) return; rid = r[0]; r++; n--; }
    if (rid < 0) rid = 0;
    uint8_t keys[16]; int nk = 0, mods = -1, dx = 0, dy = 0, wheel = 0, btn = 0, rel = 0;
    int dirs = -1, ax = 0x7FFF, ay = 0x7FFF;
    uint32_t buttons = 0; int has_buttons = 0;
    for (int k = 0; k < m->nel; k++) {
        const hid_el_t *e = &m->el[k];
        if (e->rid != rid) continue;
        if (!(e->flags & 1)) {                              /* array: the keys held */
            for (int c = 0; c < e->count; c++) {
                int32_t v = field(r, n, e->bit + c * e->size, e->size, 0);
                if (e->page == 7 && v >= 4 && v <= 0xE7 && nk < 16) keys[nk++] = (uint8_t)v;
            }
            continue;
        }
        int sign = e->lmin < 0;
        int32_t v = field(r, n, e->bit, e->size, sign);
        if (e->page == 7 && e->usage >= 0xE0 && e->usage <= 0xE7) { if (mods < 0) mods = 0; if (v) mods |= 1 << (e->usage - 0xE0); }
        else if (e->page == 7 && v && e->usage >= 4 && nk < 16) keys[nk++] = (uint8_t)e->usage;   /* a bitmap keyboard */
        else if (e->page == 9) { has_buttons = 1; if (v && e->usage >= 1 && e->usage <= 32) buttons |= 1u << (e->usage - 1); }
        else if (e->page == 1 && (e->flags & 2)) {
            if (e->usage == 0x30) { dx = v; rel = 1; }
            else if (e->usage == 0x31) { dy = v; rel = 1; }
            else if (e->usage == 0x38) { wheel = v; rel = 1; }
        } else if (e->page == 1 && e->usage == 0x39) {    /* hat: 0 up, clockwise, outside the range: none */
            int h = v - e->lmin, steps = e->lmax - e->lmin + 1;
            dirs = 0;
            if (v >= e->lmin && v <= e->lmax && steps >= 4) {
                int oct = steps == 8 ? h : h * 2;
                static const int map[8] = { 1, 1 | 8, 8, 2 | 8, 2, 2 | 4, 4, 1 | 4 };
                dirs = map[oct & 7];
            }
        } else if (e->page == 1 && (e->usage == 0x30 || e->usage == 0x31) && e->lmax > e->lmin) {
            int32_t mid = (e->lmin + e->lmax) / 2, span = (e->lmax - e->lmin) / 2;
            int norm = span ? (int)((v - mid) * 100 / span) : 0;
            if (e->usage == 0x30) ax = norm; else ay = norm;
        }
    }
    if (m->keyboard && (nk || mods >= 0)) {
        int md = mods < 0 ? 0 : mods, km = 0;
        if (md & 0x22) km |= MOD_SHIFT;
        if (md & 0x11) km |= MOD_CTRL;
        if (md & 0x44) km |= MOD_ALT;
        if (md & 0x88) km |= MOD_SUPER;
        int any = 0;
        for (int i = 0; i < nk; i++) {
            int was = 0;
            for (int j = 0; j < st->nkeys; j++) if (st->keys[j] == keys[i]) was = 1;
            any = 1;
            if (was) continue;
            int ch, key = input_from_hid(keys[i], km & MOD_SHIFT, &ch);
            press(st, key, ch, km, source);
        }
        if ((st->mods & 0x88) && !(md & 0x88) && !any) input_key(KEY_SUPER, 0, 0, source);
        if (!any) st->rep_key = 0;
        input_raw_keys(st->mods, st->keys, st->nkeys, (uint8_t)md, keys, nk);
        memcpy(st->keys, keys, (size_t)nk);
        st->nkeys = (uint8_t)nk;
        st->mods = (uint8_t)md;
    }
    if (rel) {
        for (int b = 0; b < 3; b++) if (buttons >> b & 1) btn |= 1 << b;
        hid_pointer_move(dx, dy, btn, wheel);
        return;
    }
    if (m->gamepad) {
        if (dirs < 0) {                                     /* no hat: the left stick, past halfway */
            dirs = 0;
            if (ay != 0x7FFF) { if (ay < -50) dirs |= 1; if (ay > 50) dirs |= 2; }
            if (ax != 0x7FFF) { if (ax < -50) dirs |= 4; if (ax > 50) dirs |= 8; }
        }
        int newly = dirs & ~st->dirs;
        for (int b = 0; b < 4; b++) if (newly >> b & 1) press(st, dir_keys[b], 0, 0, source);
        if (!dirs && st->dirs) st->rep_key = 0;
        st->dirs = dirs;
        if (has_buttons) {
            uint32_t down = buttons & ~st->buttons;
            for (int b = 0; b < 4; b++) if (down >> b & 1) input_key(btn_keys[b], 0, 0, source);
            st->buttons = buttons;
        }
    }
}

void hid_repeat(hid_state_t *st, int source) {
    if (st->rep_key && now_ms() >= st->rep_at) {
        input_key(st->rep_key, st->rep_ch, st->rep_mods, source);
        st->rep_at = now_ms() + (st->dirs ? 120 : 33);
    }
}
