#include <stdint.h>
#include <stddef.h>
#include "audio.h"
#include "mixer.h"
#include "kheap.h"
#include "pcspk.h"
#include "pit.h"
#include "kprintf.h"

/* ===========================================================================
 *  Audio dispatch. Picks a codec driver once at boot and forwards to it.
 *
 *  The order is deliberate: Intel HDA first, because a machine that has both
 *  (QEMU will happily give you one of each) is a machine whose HDA codec is
 *  wired to the speakers. With neither, every call still has to do something
 *  reasonable -- audio_tone() drops to the PC speaker, and the streaming calls
 *  become no-ops that report "not busy" so a caller's push/poll loop finishes
 *  instead of spinning forever.
 * ===========================================================================*/

static const audio_dev_t *dev;
static void (*preempt)(void);          /* stop whatever is streaming */

void audio_set_preempt(void (*stop_streams)(void)) { preempt = stop_streams; }

int         audio_present(void) { return 1; }         /* the speaker is always there */
const char *audio_name(void)    { return dev ? dev->name : "PC speaker"; }
int         audio_pcm_ok(void)  { return dev != 0; }
int         audio_volume(void)  { return dev ? mixer_volume(MIX_MASTER) : 100; }

/* Hand the codec master*channel for whoever is playing. One gain register is
 * all the hardware has, and only one stream reaches it at a time, so the
 * product is the whole mix -- see the note at the top of include/mixer.h. */
static void apply_gain(int mix_ch) {
    if (!dev) return;
    mixer_set_active(mix_ch);
    dev->volume(mixer_effective(mix_ch));
}

void audio_gain_changed(void) { apply_gain(mixer_active()); }

void audio_set_volume(int percent) {
    mixer_set_volume(MIX_MASTER, percent);
    audio_gain_changed();
}

void audio_play_async_ch(int mix_ch, const int16_t *interleaved, uint32_t frames) {
    if (!dev || !interleaved || !frames) return;
    uint32_t cap = dev->capacity();
    if (frames > cap) frames = cap;
    apply_gain(mix_ch);
    mixer_note_peak(mix_ch, interleaved, frames);
    dev->submit(interleaved, frames);
}

void audio_play_async(const int16_t *interleaved, uint32_t frames) {
    audio_play_async_ch(MIX_APP, interleaved, frames);
}

int  audio_busy(void) { return dev ? dev->busy() : 0; }
void audio_stop(void) { if (dev) dev->stop(); }

void audio_play_pcm_ch(int mix_ch, const int16_t *interleaved, uint32_t frames) {
    if (!dev || !interleaved || !frames) return;
    if (preempt) preempt();               /* this buffer, not the one already going */
    uint32_t cap = dev->capacity();
    if (frames > cap) frames = cap;
    audio_play_async_ch(mix_ch, interleaved, frames);

    /* Wait on the clock, not on a spin count. The buffer is `frames`/48000 of a
     * second long, so a codec that is working reports itself drained inside
     * that plus a little slack; one that has wedged -- a stream engine that
     * never starts reads as permanently busy -- gives the caller its thread
     * back at the same moment instead of holding it for as long as an
     * iteration count happens to take on that machine. */
    uint64_t deadline = pit_ticks() + frames / (AUDIO_RATE / 1000u) + 500u;
    while (dev->busy() && pit_ticks() < deadline) __asm__ volatile("pause");
    dev->stop();
}

void audio_play_pcm(const int16_t *interleaved, uint32_t frames) {
    audio_play_pcm_ch(MIX_SYSTEM, interleaved, frames);
}

void audio_tone_ch(int mix_ch, uint32_t freq, uint32_t ms) {
    /* No preempt here: the fallback path is the PC speaker, which shares
     * nothing with the codec, and the codec path below goes out through
     * audio_play_pcm(), which does it. */
    if (!dev || freq == 0) {
        pcspk_tone(freq);
        uint64_t end = pit_ticks() + ms;
        while (pit_ticks() < end) __asm__ volatile("hlt");
        pcspk_off();
        return;
    }
    uint32_t frames = (AUDIO_RATE * ms) / 1000u;
    uint32_t cap = dev->capacity();
    if (frames > cap) frames = cap;
    if (!frames) return;

    int16_t *buf = (int16_t *)kmalloc((uint64_t)frames * 4);
    if (!buf) return;

    /* Integer square wave at ~25% of full scale: loud enough to hear, quiet
     * enough that the codec's own gain does not clip it into a rasp. */
    uint32_t half = AUDIO_RATE / (freq * 2);
    if (half == 0) half = 1;
    int16_t lvl = 8000;
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < frames; i++) {
        if (++cnt >= half) { cnt = 0; lvl = (int16_t)-lvl; }
        buf[i * 2] = lvl;
        buf[i * 2 + 1] = lvl;
    }
    audio_play_pcm_ch(mix_ch, buf, frames);
    kfree(buf);
}

void audio_tone(uint32_t freq, uint32_t ms) { audio_tone_ch(MIX_APP, freq, ms); }

/* --- capture --------------------------------------------------------------
 *  Opened on demand and closed again, unlike the output route. Holding it open
 *  costs a running DMA engine, a stream descriptor and -- on real hardware --
 *  a bias supply on the microphone ring, none of which a machine that is not
 *  recording should be paying for.
 * ------------------------------------------------------------------------*/
static int cap_open;

int audio_capture_ok(void)     { return dev && dev->cap_open != 0; }
int audio_capture_active(void) { return cap_open; }

const char *audio_capture_name(void) {
    return audio_capture_ok() ? dev->cap_name() : "";
}

int audio_capture_open(void) {
    if (!audio_capture_ok()) return -1;
    if (cap_open) return 0;
    if (dev->cap_open() != 0) return -1;
    dev->cap_gain(mixer_effective(MIX_CAPTURE));
    cap_open = 1;
    return 0;
}

void audio_capture_close(void) {
    if (!cap_open) return;
    dev->cap_close();
    cap_open = 0;
}

uint32_t audio_capture_read(int16_t *out, uint32_t frames) {
    if (!cap_open || !out || !frames) return 0;
    uint32_t got = dev->cap_read(out, frames);
    if (got) mixer_note_peak(MIX_CAPTURE, out, got);
    return got;
}

/* Drain the ring into a small scratch just to measure it. Static rather than
 * on the stack: kernel stacks here are not big enough to hold a tenth of a
 * second of stereo PCM, and this is called from a window tick. */
void audio_capture_pump(void) {
    static int16_t scratch[2048 * 2];
    if (!cap_open) return;
    /* Loop, because a tick that arrives late can have more waiting than the
     * scratch holds, and a meter reading the oldest 40 ms of a 600 ms backlog
     * is a meter showing the past. */
    for (int i = 0; i < 16; i++)
        if (audio_capture_read(scratch, 2048) == 0) break;
}

void audio_init(void) {
    mixer_init();                    /* levels from /etc/mixer.conf, if it exists */
    dev = hda_probe();
    if (!dev) dev = ac97_probe();
    if (!dev) { kprintf("[audio] no PCI codec; PC speaker only\n"); return; }
    apply_gain(MIX_APP);
}
