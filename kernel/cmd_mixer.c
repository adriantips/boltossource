#include <stdint.h>
#include <stdio.h>
#include "commands.h"
#include "mixer.h"
#include "audio.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_mixer.c
 *  `mixer` -- the channel levels from a prompt.
 *
 *  The same model the Mixer window drives, which is the point: a machine being
 *  administered over SSH has no window, and `mixer media 30` is the whole
 *  answer there.
 * ===========================================================================*/

static void usage(void) {
    kprintf("usage: mixer                      show every channel\n"
            "       mixer CHANNEL PERCENT      set one (e.g. mixer media 40)\n"
            "       mixer CHANNEL mute|unmute  silence one, keeping its level\n"
            "       mixer CHANNEL toggle       mute or unmute it\n"
            "       mixer reset                back to the defaults\n"
            "       mixer save                 write /etc/mixer.conf now\n");
}

/* A bar wide enough to compare two channels at a glance and narrow enough to
 * fit an 80-column terminal with the numbers beside it. */
static void bar(int pct, int peak) {
    char b[23];
    int fill = pct * 20 / 100;
    int mark = peak * 20 / 100;
    b[0] = '[';
    for (int i = 0; i < 20; i++)
        b[i + 1] = (i < fill) ? '=' : (i == mark && peak > 0 ? '|' : ' ');
    b[21] = ']';
    b[22] = 0;
    kprintf("%s", b);
}

static void show(void) {
    kprintf("  %-11s %-5s %-22s %s\n", "channel", "level", "", "what it is");
    for (int c = 0; c < mixer_count(); c++) {
        char lv[8];
        snprintf(lv, sizeof lv, "%d%%", mixer_volume(c));
        kprintf("%c %-11s %-5s ", c == mixer_active() ? '>' : ' ', mixer_name(c), lv);
        bar(mixer_volume(c), mixer_peak(c));
        kprintf(" %s%s\n", mixer_muted(c) ? "MUTED -- " : "", mixer_describe(c));
    }
    kprintf("  device: %s%s. '>' is the channel the codec is playing now.\n",
            audio_name(), audio_pcm_ok() ? "" : " (tones only)");
}

int cmd_mixer(int argc, char **argv) {
    if (argc < 2) { show(); return 0; }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) { usage(); return 0; }
    if (strcmp(argv[1], "list") == 0) { show(); return 0; }

    if (strcmp(argv[1], "save") == 0) {
        if (mixer_save() != 0) { kprintf("mixer: could not write /etc/mixer.conf\n"); return 1; }
        kprintf("mixer: saved to /etc/mixer.conf\n");
        return 0;
    }
    if (strcmp(argv[1], "reset") == 0) {
        mixer_reset();
        audio_gain_changed();
        mixer_save();
        show();
        return 0;
    }

    int c = mixer_channel_by_name(argv[1]);
    if (c < 0) {
        kprintf("mixer: no channel '%s' (run 'mixer' to list them)\n", argv[1]);
        return 1;
    }
    if (argc < 3) {
        kprintf("%s = %d%%%s\n", mixer_name(c), mixer_volume(c),
                mixer_muted(c) ? " (muted)" : "");
        return 0;
    }

    const char *v = argv[2];
    if (strcmp(v, "mute") == 0)        mixer_set_mute(c, 1);
    else if (strcmp(v, "unmute") == 0) mixer_set_mute(c, 0);
    else if (strcmp(v, "toggle") == 0) mixer_toggle_mute(c);
    else {
        int n = 0, digits = 0;
        for (const char *p = v; *p >= '0' && *p <= '9'; p++) { n = n * 10 + (*p - '0'); digits++; }
        if (!digits || n > 100) { kprintf("mixer: '%s' is not 0..100\n", v); return 1; }
        mixer_set_volume(c, n);
    }

    audio_gain_changed();
    if (mixer_save() != 0) kprintf("mixer: level set, but /etc/mixer.conf could not be written\n");
    kprintf("%s = %d%%%s", mixer_name(c), mixer_volume(c), mixer_muted(c) ? " (muted)" : "");
    if (c != MIX_MASTER && !mixer_is_input(c))
        kprintf(", %d%% after master", mixer_effective(c));
    kprintf("\n");
    return 0;
}
