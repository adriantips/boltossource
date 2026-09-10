#include <stdint.h>
#include <stddef.h>
#include "audio.h"
#include "hw.h"
#include "io.h"
#include "dma.h"
#include "string.h"
#include "kprintf.h"

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
#define NAM_PCM_VOL      0x18
#define NAM_EXT_AUDIO    0x28
#define NAM_PCM_RATE     0x2C

/* NABM bus-master, PCM-out box (BAR1 + 0x10). */
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

static struct {
    int      present;
    uint16_t nam, nabm;          /* I/O port bases */
    struct dma_buf bdl;          /* buffer descriptor list */
    struct dma_buf pcm;          /* sample buffer (up to ~1s) */
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

static const audio_dev_t ac97_dev = {
    .name = "AC97",
    .capacity = ac97_capacity,
    .submit = ac97_submit,
    .busy = ac97_busy,
    .stop = ac97_stop,
    .volume = ac97_volume,
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
    for (int t = 0; t < 1000000 && !(glob_sta() & 0x100); t++) __asm__ volatile("pause");
    nam_w(NAM_RESET, 0xFFFF);

    a.present = 1;
    kprintf("[ok] AC97 audio %x:%x (NAM=0x%x NABM=0x%x), 48kHz stereo\n",
            pd.vendor, pd.device, a.nam, a.nabm);
    return &ac97_dev;
}
