#include <stdint.h>
#include <stddef.h>
#include "audio.h"
#include "hw.h"
#include "io.h"
#include "dma.h"
#include "string.h"
#include "kprintf.h"
#include "timeout.h"

/* ===========================================================================
 *  AC'97 audio (Intel 82801 / QEMU AC97). Two I/O BARs: BAR0 = mixer (NAM),
 *  BAR1 = bus-master (NABM). We program the PCM-out engine with a buffer
 *  descriptor list and stream 48 kHz/16-bit/stereo samples by DMA.
 *
 *  Published to the rest of the system as an audio_dev through ac97_probe();
 *  drivers/audio.c owns the fallbacks and the tone generator.
 * ===========================================================================*/

/* NAM mixer registers (BAR0). */
#define NAM_RESET        0x00
#define NAM_MASTER_VOL   0x02
#define NAM_MIC_VOL      0x0E
#define NAM_REC_SEL      0x1A
#define NAM_REC_GAIN     0x1C
#define NAM_PCM_VOL      0x18
#define NAM_EXT_AUDIO    0x28
#define NAM_PCM_RATE     0x2C

/* NABM bus-master. The same six registers appear twice: the PCM-in box at
 * BAR1 + 0x00 and the PCM-out box at BAR1 + 0x10. */
#define PI_BDBAR         0x00
#define PI_CIV           0x04
#define PI_LVI           0x05
#define PI_SR            0x06
#define PI_PICB          0x08
#define PI_CR            0x0B

#define PO_BDBAR         0x10
#define PO_CIV           0x14
#define PO_LVI           0x15
#define PO_SR            0x16
#define PO_PICB          0x18
#define PO_CR            0x1B
#define GLOB_CNT         0x2C
#define GLOB_STA         0x30

#define CR_RPBM          0x01     /* run/pause bus master */
#define CR_RR            0x02     /* reset registers      */
#define SR_DCH           0x01     /* DMA controller halted */

#define BDL_ENTRIES      32
#define SAMPLE_RATE      48000

struct bdl_entry { uint32_t addr; uint16_t samples; uint16_t flags; } __attribute__((packed));

/* The capture ring, same size and for the same reason as the HDA one, spread
 * across every descriptor the BDL has -- see cap_advance() for why it wants as
 * many as it can get. */
#define CAPIN_BYTES      (128u * 1024)
#define CAP_ENTRIES      BDL_ENTRIES
#define CAP_PER          (CAPIN_BYTES / CAP_ENTRIES)

static struct {
    int      present;
    uint16_t nam, nabm;          /* I/O port bases */
    struct dma_buf bdl;          /* buffer descriptor list */
    struct dma_buf pcm;          /* sample buffer (up to ~1s) */
    struct dma_buf cap_bdl, cap_pcm;
    int      cap_ready, cap_on;
    uint32_t cap_rd;             /* byte offset consumed up to */
} a;

static void nam_w(uint8_t reg, uint16_t v) { outw((uint16_t)(a.nam + reg), v); }
static uint16_t glob_sta(void) { return inw((uint16_t)(a.nabm + GLOB_STA)); }

static void ac97_volume(int percent) {
    if (!a.present) return;
    /* AC97 attenuation: 0 = loudest, 0x3F = quietest, per channel; bit15 mute. */
    uint16_t att = (uint16_t)((100 - percent) * 0x1F / 100);
    uint16_t v = (uint16_t)((att << 8) | att);
    if (percent == 0) v |= 0x8000;
    nam_w(NAM_MASTER_VOL, v);
    nam_w(NAM_PCM_VOL, v);
}

static uint32_t ac97_capacity(void) { return a.present ? (uint32_t)(a.pcm.size / 4) : 0; }

/* Program the BDL for the first `frames` stereo frames in a.pcm and start the
 * DMA engine. Returns without waiting (the buffer plays in the background). */
static void ac97_start_buffer(uint32_t frames) {
    struct bdl_entry *bdl = (struct bdl_entry *)a.bdl.virt;
    uint32_t per = 0xFFFE / 2;                 /* max stereo frames per descriptor */
    uint32_t off = 0, n = 0;
    for (n = 0; n < BDL_ENTRIES && off < frames; n++) {
        uint32_t f = frames - off;
        if (f > per) f = per;
        bdl[n].addr    = (uint32_t)(a.pcm.phys + off * 4);
        bdl[n].samples = (uint16_t)(f * 2);    /* 16-bit sample count */
        bdl[n].flags   = 0;                    /* IOC off; we poll DCH */
        off += f;
    }
    if (n == 0) return;
    bdl[n - 1].flags = 0x4000;                 /* BUP on the final descriptor */

    outb((uint16_t)(a.nabm + PO_CR), CR_RR);   /* reset PCM-out engine */
    while (inb((uint16_t)(a.nabm + PO_CR)) & CR_RR) { }
    outl((uint16_t)(a.nabm + PO_BDBAR), (uint32_t)a.bdl.phys);
    outb((uint16_t)(a.nabm + PO_LVI), (uint8_t)(n - 1));
    outw((uint16_t)(a.nabm + PO_SR), 0x1C);    /* clear status bits */
    outb((uint16_t)(a.nabm + PO_CR), CR_RPBM); /* run */
}

/* Copy PCM into the device buffer, start DMA, return. */
static void ac97_submit(const int16_t *interleaved, uint32_t frames) {
    if (!a.present) return;
    memcpy(a.pcm.virt, interleaved, frames * 4);
    ac97_start_buffer(frames);
}

/* 1 while the PCM-out DMA engine is still playing a buffer. */
static int ac97_busy(void) {
    if (!a.present) return 0;
    return (inw((uint16_t)(a.nabm + PO_SR)) & SR_DCH) ? 0 : 1;
}

static void ac97_stop(void) {
    if (!a.present) return;
    outb((uint16_t)(a.nabm + PO_CR), 0);
}

/* --- capture --------------------------------------------------------------
 *  PCM-in is the same bus-master box as PCM-out at a different offset, so the
 *  engine half of this is short. What is not short is that AC'97 records from
 *  whichever source the record-select register names, and it comes out of reset
 *  naming none of them -- so a driver that only programs the DMA gets a running
 *  engine full of zeros and no way to tell that from a quiet room.
 * ------------------------------------------------------------------------*/

static const char *ac97_cap_name(void) { return "Mic"; }

static void ac97_cap_gain(int percent) {
    if (!a.present) return;
    /* Record gain is the other direction from the attenuators above: 0 is the
     * quietest and 0x0F the loudest, 1.5 dB a step, bit 15 mute. */
    uint16_t g = (uint16_t)(percent * 0x0F / 100);
    uint16_t v = (uint16_t)((g << 8) | g);
    if (percent <= 0) v |= 0x8000;
    nam_w(NAM_REC_GAIN, v);
    /* And the mic's own 20 dB boost stage, which is a separate mute. */
    nam_w(NAM_MIC_VOL, percent <= 0 ? 0x8000 : 0x0008);
}

static int ac97_cap_open(void) {
    if (!a.present) return -1;
    if (a.cap_on) return 0;
    if (!a.cap_ready) {
        if (dma_alloc(BDL_ENTRIES * sizeof(struct bdl_entry), &a.cap_bdl) != 0) return -1;
        if (dma_alloc(CAPIN_BYTES, &a.cap_pcm) != 0) { dma_free(&a.cap_bdl); return -1; }
        a.cap_ready = 1;
    }
    memset(a.cap_pcm.virt, 0, CAPIN_BYTES);

    /* Record from the microphone, both channels. Without this the engine runs
     * and records the nothing it is connected to. */
    nam_w(NAM_REC_SEL, 0x0000);
    ac97_cap_gain(60);

    struct bdl_entry *bdl = (struct bdl_entry *)a.cap_bdl.virt;
    for (uint32_t i = 0; i < CAP_ENTRIES; i++) {
        bdl[i].addr    = (uint32_t)(a.cap_pcm.phys + i * CAP_PER);
        bdl[i].samples = (uint16_t)(CAP_PER / 2);      /* 16-bit sample count */
        bdl[i].flags   = 0;
    }

    outb((uint16_t)(a.nabm + PI_CR), CR_RR);
    while (inb((uint16_t)(a.nabm + PI_CR)) & CR_RR) { }
    outl((uint16_t)(a.nabm + PI_BDBAR), (uint32_t)a.cap_bdl.phys);
    outb((uint16_t)(a.nabm + PI_LVI), (uint8_t)(CAP_ENTRIES - 2));
    outw((uint16_t)(a.nabm + PI_SR), 0x1C);
    a.cap_rd = 0;
    a.cap_on = 1;
    outb((uint16_t)(a.nabm + PI_CR), CR_RPBM);
    return 0;
}

/* Keep the last-valid index ahead of the current one.
 *
 * This is the difference between a ring and a buffer, and AC'97 will not do it
 * for you. The engine walks descriptors from CIV and halts the moment it
 * finishes the one LVI names -- it does not wrap past it. Program LVI once and
 * the capture runs exactly once around the buffer and stops: 32768 frames,
 * every time, however long you wait, with the run bit still set and no error
 * bit anywhere. (That is not a hypothetical. It is what this did.)
 *
 * So LVI is pushed to two behind CIV on every read, which leaves the engine
 * thirty descriptors of road ahead and never lets it arrive. The index wraps
 * inside the 32-entry list on its own, and the descriptors point back at the
 * same memory, so the buffer is cyclic. */
static void cap_advance(void) {
    uint8_t civ = (uint8_t)(inb((uint16_t)(a.nabm + PI_CIV)) & (CAP_ENTRIES - 1));
    outb((uint16_t)(a.nabm + PI_LVI), (uint8_t)((civ + CAP_ENTRIES - 2) & (CAP_ENTRIES - 1)));
}

static void ac97_cap_close(void) {
    if (!a.present || !a.cap_on) return;
    outb((uint16_t)(a.nabm + PI_CR), 0);
    a.cap_on = 0;
}

/* Where the engine has written up to, in bytes from the start of the ring.
 * AC'97 has no byte-accurate position register: CIV names the descriptor it is
 * in and PICB counts the samples *left* in it, so the two together give the
 * position. PICB is read after CIV and CIV again after PICB -- a wrap between
 * the two reads would otherwise put the position a whole descriptor out. */
static uint32_t cap_pos(void) {
    for (int t = 0; t < 4; t++) {
        uint8_t civ = (uint8_t)(inb((uint16_t)(a.nabm + PI_CIV)) & (CAP_ENTRIES - 1));
        uint16_t picb = inw((uint16_t)(a.nabm + PI_PICB));
        if ((inb((uint16_t)(a.nabm + PI_CIV)) & (CAP_ENTRIES - 1)) != civ) continue;
        uint32_t left = (uint32_t)picb * 2;
        if (left > CAP_PER) left = CAP_PER;
        return (civ * CAP_PER + (CAP_PER - left)) % CAPIN_BYTES;
    }
    return a.cap_rd;
}

static uint32_t ac97_cap_read(int16_t *out, uint32_t frames) {
    if (!a.cap_on || !out || !frames) return 0;
    cap_advance();
    uint32_t pos = cap_pos();
    uint32_t avail = (pos + CAPIN_BYTES - a.cap_rd) % CAPIN_BYTES;
    uint32_t want = frames * 4;
    if (avail > want) avail = want;
    if (!avail) return 0;

    const uint8_t *src = (const uint8_t *)a.cap_pcm.virt;
    uint32_t first = CAPIN_BYTES - a.cap_rd;
    if (first > avail) first = avail;
    memcpy(out, src + a.cap_rd, first);
    if (avail > first) memcpy((uint8_t *)out + first, src, avail - first);
    a.cap_rd = (a.cap_rd + avail) % CAPIN_BYTES;
    return avail / 4;
}

static const audio_dev_t ac97_dev = {
    .name = "AC97",
    .capacity = ac97_capacity,
    .submit = ac97_submit,
    .busy = ac97_busy,
    .stop = ac97_stop,
    .volume = ac97_volume,
    .cap_open = ac97_cap_open,
    .cap_read = ac97_cap_read,
    .cap_close = ac97_cap_close,
    .cap_gain = ac97_cap_gain,
    .cap_name = ac97_cap_name,
};

const audio_dev_t *ac97_probe(void) {
    struct pci_dev pd;

    /* AC'97 (class 0x04 subclass 0x01, or Intel 8086:2415). */
    if (!pci_find_by_class(0x04, 0x01, &pd) && !pci_find_by_id(0x8086, 0x2415, &pd))
        return 0;

    struct pci_bar nam, nabm;
    if (pci_bar(&pd, 0, &nam) != 0 || pci_bar(&pd, 1, &nabm) != 0) {
        kprintf("[audio] AC97 BAR decode failed\n");
        return 0;
    }
    a.nam  = nam.is_mmio  ? 0 : nam.port;
    a.nabm = nabm.is_mmio ? 0 : nabm.port;
    if (!a.nam || !a.nabm) { kprintf("[audio] AC97 expects I/O BARs\n"); return 0; }

    pci_enable_bus_master(&pd);

    if (dma_alloc(BDL_ENTRIES * sizeof(struct bdl_entry), &a.bdl) != 0) return 0;
    if (dma_alloc(256 * 1024, &a.pcm) != 0) {     /* ~1.3s @ 48k stereo */
        dma_free(&a.bdl);
        return 0;
    }

    /* cold reset + wait for primary codec ready */
    outl((uint16_t)(a.nabm + GLOB_CNT), 0x00000002);
    /* Each glob_sta() is a port read, about a microsecond on real silicon, so
     * the old million-iteration bound was a second of boot spent on a codec
     * that was never going to answer. AC'97 gives primary-codec-ready 600 ms. */
    wait_until(glob_sta() & 0x100, 600000ull);
    nam_w(NAM_RESET, 0xFFFF);

    a.present = 1;
    kprintf("[ok] AC97 audio %x:%x (NAM=0x%x NABM=0x%x), 48kHz stereo\n",
            pd.vendor, pd.device, a.nam, a.nabm);
    return &ac97_dev;
}
