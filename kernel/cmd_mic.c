#include <stdint.h>
#include <stdio.h>
#include "commands.h"
#include "audio.h"
#include "mixer.h"
#include "wav.h"
#include "kheap.h"
#include "pit.h"
#include "hpet.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_mic.c
 *  `mic` -- the input side of the sound card, from a prompt.
 *
 *  A thing worth knowing before you read the assertions in test/t_mic.py: no
 *  emulator on this development machine can put a sound *into* a guest. QEMU's
 *  wav backend is output-only and its `none` backend feeds a capture stream
 *  silence. So everything below can be proved to work except the one step
 *  where a microphone would be involved, and what comes out of QEMU is a
 *  correctly-sized, correctly-formatted recording of nothing at all.
 *
 *  That is still worth having: a running DMA engine whose position counter
 *  advances at 48000 frames a second is the whole of the driver, and it is
 *  exactly what a stream that was never started does not do.
 * ===========================================================================*/

#define MAX_SECS   30
#define CHUNK      1024          /* frames per read: 4 KB of a 64 KB kernel stack */

/* THE CLOCK HERE IS NOT THE WALL CLOCK, and a recorder that believes it is
 * records the wrong length.
 *
 * The drain loop below polls hard with interrupts on, and under that load a
 * machine does not receive every timer interrupt -- so pit_ticks() runs slow,
 * and a deadline built on it holds the loop open longer than it was asked for.
 * The HPET would not have this problem and the kernel prefers it, but plenty
 * of machines do not have one (including, as it turns out, every QEMU built
 * without CONFIG_HPET, which is what this is developed against), and there the
 * PIT is all there is. Measured: a three-"second" capture produced 200310
 * frames, which at 48 kHz is 4.17 real seconds.
 *
 * The fix is not a better clock. It is to stop asking: a stream running at
 * 48 kHz *is* a clock, and an exact one. So a length in seconds is a length in
 * frames, the loop counts frames, and now_ms() is left doing the one job it is
 * still fit for -- putting a ceiling on a stream that has stopped producing,
 * so a wedged engine gives the shell back instead of keeping it. */
static uint64_t now_ms(void) {
    return hpet_present() ? hpet_us() / 1000u : pit_ticks();
}

static void usage(void) {
    kprintf("usage: mic                    what the input is and whether it is open\n"
            "       mic test [SECONDS]     capture and report level, writing nothing\n"
            "       mic rec FILE [SECONDS] capture to a WAV (48 kHz stereo)\n"
            "       mic gain PERCENT       the capture level (same as `mixer capture`)\n"
            "       mic mute | unmute      silence the input\n");
}

static void status(void) {
    if (!audio_capture_ok()) {
        kprintf("mic: %s has no input route; nothing to record from\n", audio_name());
        return;
    }
    kprintf("mic: %s, input %s, gain %d%%%s, %s\n",
            audio_name(), audio_capture_name(), mixer_volume(MIX_CAPTURE),
            mixer_muted(MIX_CAPTURE) ? " (muted)" : "",
            audio_capture_active() ? "open" : "closed");
}

/* Drain the ring until `wanted` frames have come out of it, into `sink` when
 * there is one and over a scratch buffer when there is not. Returns the frames
 * taken -- short only if the engine stopped producing and the guard expired.
 *
 * The drain has to keep up: the ring is 0.68 s long and wraps whether anybody
 * is reading or not, so a loop that slept between reads would record a track
 * with holes in it and no error anywhere. */
static uint32_t drain(int16_t *sink, uint32_t wanted, int *peak) {
    int16_t chunk[CHUNK * 2];
    uint32_t total = 0, idle = 0;
    int hi = 0;
    /* The guard: four times the length asked for, on a clock that under-counts
     * rather than over-counts, so it only ever fires on an engine that has
     * genuinely stopped. */
    uint64_t deadline = now_ms() + (uint64_t)(wanted / (AUDIO_RATE / 1000u)) * 4u + 2000u;

    while (total < wanted) {
        int16_t *dst = sink ? sink + (uint64_t)total * 2 : chunk;
        uint32_t want = wanted - total;
        if (want > CHUNK) want = CHUNK;

        uint32_t got = audio_capture_read(dst, want);
        if (!got) {
            /* Check the clock rarely: reading it is an MMIO or port access per
             * iteration, and this loop runs a million times a second. */
            if (++idle >= 4096) { idle = 0; if (now_ms() > deadline) break; }
            __asm__ volatile("pause");
            continue;
        }
        idle = 0;
        for (uint32_t i = 0; i < got; i++) {
            int v = dst[i * 2];
            if (v < 0) v = -v;
            if (v > hi) hi = v;
        }
        total += got;
    }
    if (peak) *peak = hi * 100 / 32768;
    return total;
}

static int parse_secs(const char *s, int dflt) {
    if (!s) return dflt;
    int n = 0, digits = 0;
    for (; *s >= '0' && *s <= '9'; s++) { n = n * 10 + (*s - '0'); digits++; }
    if (!digits || n < 1) return dflt;
    return n > MAX_SECS ? MAX_SECS : n;
}

int cmd_mic(int argc, char **argv) {
    if (argc < 2) { status(); return 0; }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) { usage(); return 0; }

    if (strcmp(argv[1], "gain") == 0) {
        if (argc < 3) { kprintf("gain %d%%\n", mixer_volume(MIX_CAPTURE)); return 0; }
        int n = 0, digits = 0;
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) { n = n * 10 + (*p - '0'); digits++; }
        if (!digits || n > 100) { kprintf("mic: '%s' is not 0..100\n", argv[2]); return 1; }
        mixer_set_volume(MIX_CAPTURE, n);
        mixer_save();
        kprintf("gain %d%%\n", mixer_volume(MIX_CAPTURE));
        return 0;
    }
    if (strcmp(argv[1], "mute") == 0 || strcmp(argv[1], "unmute") == 0) {
        mixer_set_mute(MIX_CAPTURE, argv[1][0] == 'm');
        mixer_save();
        status();
        return 0;
    }

    if (!audio_capture_ok()) {
        kprintf("mic: %s has no input route; nothing to record from\n", audio_name());
        return 1;
    }

    if (strcmp(argv[1], "test") == 0) {
        uint32_t secs = (uint32_t)parse_secs(argc > 2 ? argv[2] : 0, 1);
        uint32_t want = AUDIO_RATE * secs;
        if (audio_capture_open() != 0) { kprintf("mic: could not open the input\n"); return 1; }
        int peak = 0;
        uint32_t frames = drain(0, want, &peak);
        audio_capture_close();
        kprintf("mic: %u of %u frames (%u ms of audio), peak %d%% on %s\n",
                frames, want, frames / (AUDIO_RATE / 1000u), peak, audio_capture_name());
        if (!frames)
            kprintf("mic: the stream engine produced nothing -- it is not running\n");
        else if (frames < want)
            kprintf("mic: it stopped early. A cyclic buffer that is not cyclic\n"
                    "     looks exactly like this -- it fills once and halts.\n");
        else if (!peak)
            kprintf("mic: silence. On a real machine that is a muted or unplugged\n"
                    "     input; under QEMU it is the only thing capture can be,\n"
                    "     since no emulated backend here can feed a guest sound.\n");
        return frames == want ? 0 : 1;
    }

    if (strcmp(argv[1], "rec") == 0) {
        if (argc < 3) { usage(); return 1; }
        uint32_t secs = (uint32_t)parse_secs(argc > 3 ? argv[3] : 0, 3);
        uint32_t cap = AUDIO_RATE * secs;
        int16_t *buf = (int16_t *)kmalloc((uint64_t)cap * 4);
        if (!buf) { kprintf("mic: %u seconds does not fit in memory\n", secs); return 1; }

        if (audio_capture_open() != 0) {
            kfree(buf);
            kprintf("mic: could not open the input\n");
            return 1;
        }
        kprintf("mic: recording %u s from %s...\n", secs, audio_capture_name());
        int peak = 0;
        uint32_t frames = drain(buf, cap, &peak);
        audio_capture_close();

        if (!frames) { kfree(buf); kprintf("mic: nothing was captured\n"); return 1; }
        const char *err = 0;
        int rc = wav_save(argv[2], buf, frames, AUDIO_RATE, 2, &err);
        kfree(buf);
        if (rc != 0) { kprintf("mic: %s: %s\n", argv[2], err ? err : "write failed"); return 1; }
        kprintf("mic: %s -- %u frames, %u ms, peak %d%%\n",
                argv[2], frames, frames / (AUDIO_RATE / 1000u), peak);
        return 0;
    }

    usage();
    return 1;
}
