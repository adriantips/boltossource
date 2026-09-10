#include <stdint.h>
#include <stddef.h>
#include "audio.h"
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
static int volume = 80;

int         audio_present(void) { return 1; }         /* the speaker is always there */
const char *audio_name(void)    { return dev ? dev->name : "PC speaker"; }
int         audio_pcm_ok(void)  { return dev != 0; }
int         audio_volume(void)  { return dev ? volume : 100; }

void audio_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    volume = percent;
    if (dev) dev->volume(percent);
}

void audio_play_async(const int16_t *interleaved, uint32_t frames) {
    if (!dev || !interleaved || !frames) return;
    uint32_t cap = dev->capacity();
    dev->submit(interleaved, frames > cap ? cap : frames);
}

int  audio_busy(void) { return dev ? dev->busy() : 0; }
void audio_stop(void) { if (dev) dev->stop(); }

void audio_play_pcm(const int16_t *interleaved, uint32_t frames) {
    if (!dev || !interleaved || !frames) return;
    uint32_t cap = dev->capacity();
    if (frames > cap) frames = cap;
    audio_play_async(interleaved, frames);

    /* Wait on the clock, not on a spin count. The buffer is `frames`/48000 of a
     * second long, so a codec that is working reports itself drained inside
     * that plus a little slack; one that has wedged -- a stream engine that
     * never starts reads as permanently busy -- gives the caller its thread
     * back at the same moment instead of holding it for as long as an
     * iteration count happens to take on that machine. */
    uint64_t deadline = pit_ticks() + frames / 48u + 500u;
    while (dev->busy() && pit_ticks() < deadline) __asm__ volatile("pause");
    dev->stop();
}

void audio_tone(uint32_t freq, uint32_t ms) {
    if (!dev || freq == 0) {
        pcspk_tone(freq);
        uint64_t end = pit_ticks() + ms;
        while (pit_ticks() < end) __asm__ volatile("hlt");
        pcspk_off();
        return;
    }
    uint32_t frames = (48000u * ms) / 1000u;
    uint32_t cap = dev->capacity();
    if (frames > cap) frames = cap;
    if (!frames) return;

    int16_t *buf = (int16_t *)kmalloc((uint64_t)frames * 4);
    if (!buf) return;

    /* Integer square wave at ~25% of full scale: loud enough to hear, quiet
     * enough that the codec's own gain does not clip it into a rasp. */
    uint32_t half = 48000u / (freq * 2);
    if (half == 0) half = 1;
    int16_t lvl = 8000;
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < frames; i++) {
        if (++cnt >= half) { cnt = 0; lvl = (int16_t)-lvl; }
        buf[i * 2] = lvl;
        buf[i * 2 + 1] = lvl;
    }
    audio_play_pcm(buf, frames);
    kfree(buf);
}

void audio_init(void) {
    dev = hda_probe();
    if (!dev) dev = ac97_probe();
    if (!dev) { kprintf("[audio] no PCI codec; PC speaker only\n"); return; }
    dev->volume(volume);
}
