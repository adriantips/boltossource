#include <stdint.h>
#include "commands.h"
#include "gui.h"
#include "audio.h"
#include "boot.h"
#include "kprintf.h"
#include "string.h"
#include "stdlib.h"

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_media.c
 *  `media` -- the player's transport, from a prompt.
 *
 *  The buttons are how anybody will actually use it. The command exists for the
 *  same three reasons `desk` and `screenshot` do: a session over SSH has no
 *  seek bar to drag, a script may want to put a sound on, and a test that has
 *  to synthesise a click at the right pixel to check anything is a test that
 *  mostly checks the mouse driver.
 *
 *  It drives exactly the same state the window does -- there is one player, and
 *  this is a second set of controls on it, not a second copy of it.
 * ===========================================================================*/

static void fmt_time(uint32_t ms, char *out) {
    uint32_t s = ms / 1000, m = s / 60;
    int n = 0;
    s %= 60;
    if (m >= 10) out[n++] = (char)('0' + (m / 10) % 10);
    out[n++] = (char)('0' + m % 10);
    out[n++] = ':';
    out[n++] = (char)('0' + s / 10);
    out[n++] = (char)('0' + s % 10);
    out[n] = 0;
}

/* "90", "1:30" and "+15" / "-15" all mean something here; the last two are
 * relative to where the track is now. Returns milliseconds. */
static uint32_t parse_pos(const char *a) {
    int rel = (a[0] == '+' || a[0] == '-');
    int neg = (a[0] == '-');
    if (rel) a++;

    uint32_t v = 0, part = 0, seen = 0;
    for (const char *p = a; *p; p++) {
        if (*p == ':') { v = v * 60 + part; part = 0; seen = 1; continue; }
        if (*p < '0' || *p > '9') break;
        part = part * 10 + (uint32_t)(*p - '0');
        seen = 1;
    }
    (void)seen;
    v = v * 60 + part;

    uint32_t ms = v * 1000;
    if (!rel) return ms;
    uint32_t now = media_position_ms();
    if (neg) return now > ms ? now - ms : 0;
    return now + ms;
}

static void print_status(void) {
    const char *t = media_track();
    char pos[12], dur[12];
    fmt_time(media_position_ms(), pos);
    fmt_time(media_duration_ms(), dur);

    kprintf("Track   : %s\n", t[0] ? t : "(none)");
    kprintf("Folder  : %s\n", media_folder());
    kprintf("State   : %s\n", media_playing() ? "playing"
                            : (media_loaded() ? "paused" : "stopped"));
    kprintf("Position: %s / %s\n", pos, dur);
    kprintf("Device  : %s  volume %d%%  repeat %s\n",
            audio_name(), audio_volume(), media_repeat() ? "on" : "off");
    if (media_status()[0]) kprintf("Problem : %s\n", media_status());
}

static void print_list(void) {
    const char *nm = "";
    int n = media_list(0, 0), cur = media_index();
    if (n == 0) { kprintf("media: nothing playable in %s\n", media_folder()); return; }
    for (int i = 0; i < n; i++) {
        media_list(i, &nm);
        kprintf("%c %2d  %s\n", i == cur ? '*' : ' ', i + 1, nm);
    }
}

int cmd_media(int argc, char **argv) {
    if (boot_is_safe()) {
        kprintf("media: the desktop is not running, so nothing would feed the codec (safe mode)\n");
        return 1;
    }

    const char *sub = argc > 1 ? argv[1] : "status";

    if (strcmp(sub, "status") == 0) { print_status(); return 0; }
    if (strcmp(sub, "list") == 0)   { print_list(); return 0; }

    if (strcmp(sub, "open") == 0) {
        if (argc < 3) { kprintf("usage: media open FILE (.wav or .mp3)\n"); return 1; }
        media_open_path_bg(argv[2]);   /* the window, not the keyboard */
        if (!media_loaded()) { kprintf("media: %s: %s\n", argv[2], media_status()); return 1; }
        print_status();
        return media_status()[0] ? 1 : 0;
    }

    if (strcmp(sub, "play") == 0 || strcmp(sub, "pause") == 0 || strcmp(sub, "toggle") == 0) {
        int want_play = strcmp(sub, "pause") != 0;
        /* `play` on something already playing should not stop it, and `pause`
         * on something already paused should not start it: only `toggle` flips
         * regardless. A script that says play means play. */
        if (strcmp(sub, "toggle") == 0 || media_playing() != want_play) media_toggle();
        if (want_play && !media_playing() && media_status()[0]) {
            kprintf("media: %s\n", media_status());
            return 1;
        }
        print_status();
        return 0;
    }

    if (strcmp(sub, "stop") == 0) { media_stop(); print_status(); return 0; }

    if (strcmp(sub, "next") == 0 || strcmp(sub, "prev") == 0) {
        if (media_skip(sub[0] == 'n' ? 1 : -1) != 0) {
            kprintf("media: no playlist to skip through\n");
            return 1;
        }
        print_status();
        return 0;
    }

    if (strcmp(sub, "track") == 0) {
        if (argc < 3) { kprintf("usage: media track N   (see media list)\n"); return 1; }
        int i = atoi(argv[2]) - 1;
        if (media_select(i) != 0) { kprintf("media: no track %s\n", argv[2]); return 1; }
        print_status();
        return 0;
    }

    if (strcmp(sub, "seek") == 0) {
        if (argc < 3) { kprintf("usage: media seek M:SS | SECONDS | +SECONDS | -SECONDS\n"); return 1; }
        if (!media_loaded()) { kprintf("media: nothing is open\n"); return 1; }
        media_seek_ms(parse_pos(argv[2]));
        print_status();
        return 0;
    }

    if (strcmp(sub, "vol") == 0) {
        if (argc < 3) { kprintf("volume %d%%\n", audio_volume()); return 0; }
        int v = atoi(argv[2]);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        audio_set_volume(v);
        kprintf("volume %d%%\n", audio_volume());
        return 0;
    }

    if (strcmp(sub, "repeat") == 0) {
        if (argc > 2) media_set_repeat(strcmp(argv[2], "off") != 0);
        else          media_set_repeat(!media_repeat());
        kprintf("repeat %s\n", media_repeat() ? "on" : "off");
        return 0;
    }

    kprintf("usage: media [status|list|open FILE|play|pause|stop|next|prev|\n");
    kprintf("             track N|seek POS|vol N|repeat on|off]\n");
    return 1;
}
