#include <stdint.h>
#include <stddef.h>
#include "audio.h"
#include "hw.h"
#include "mmio.h"
#include "dma.h"
#include "hpet.h"
#include "string.h"
#include "kprintf.h"

/* ===========================================================================
 *  Intel High Definition Audio (HDA) controller + codec driver.
 *
 *  This is what a machine made after about 2005 actually has; AC'97 is the
 *  fallback for older boards and for QEMU's -device AC97. HDA splits the job
 *  in two, and both halves have to be brought up before a sample reaches a
 *  speaker:
 *
 *    the controller  -- a PCI device with MMIO registers, a set of DMA stream
 *                       engines, and a command/response ring pair (CORB/RIRB)
 *                       for talking to the codecs on its serial link.
 *    the codec       -- a graph of widgets (converters, mixers, selectors, pin
 *                       complexes). Nothing is wired up by default: you have to
 *                       walk the graph, find a route from an output pin back to
 *                       a digital-to-analogue converter, select that route at
 *                       every fork, unmute every amplifier along it, and switch
 *                       the pin's output driver on.
 *
 *  That walk is the reason this file is not just "write a BDL, set RUN". A pin
 *  on one machine reaches its DAC directly; on the next there is a selector and
 *  two mixers in between, and picking the wrong branch gives you silence with
 *  every register looking correct.
 *
 *  Commands go through CORB/RIRB, with the immediate-command registers as a
 *  fallback -- some controllers (and some emulations) implement only one of the
 *  two well, and a codec we cannot talk to is a codec we must not claim.
 *
 *  Playback is polled, not interrupt-driven, to match the AC'97 path and the
 *  push/poll contract in audio.h. The one wrinkle is that an HDA stream buffer
 *  is *cyclic*: it does not stop at the end, it wraps and plays again. So every
 *  buffer is submitted as two descriptors -- the caller's PCM, then a permanent
 *  block of silence -- and the engine is stopped while it is grinding through
 *  the silence. Without that pad, the gap between the buffer draining and the
 *  caller noticing would be filled with a replay of the chunk just played.
 * ===========================================================================*/

/* --- controller registers (BAR0) ---------------------------------------- */
#define REG_GCAP        0x00   /* 16: stream counts, 64-bit capability       */
#define REG_GCTL        0x08   /* 32: bit0 CRST                              */
#define REG_WAKEEN      0x0C
#define REG_STATESTS    0x0E   /* 16: one bit per codec address, RW1C        */
#define REG_INTCTL      0x20
#define REG_CORBLBASE   0x40
#define REG_CORBUBASE   0x44
#define REG_CORBWP      0x48   /* 16                                         */
#define REG_CORBRP      0x4A   /* 16: bit15 = reset                          */
#define REG_CORBCTL     0x4C   /* 8:  bit1 = DMA run                         */
#define REG_CORBSIZE    0x4E   /* 8                                          */
#define REG_RIRBLBASE   0x50
#define REG_RIRBUBASE   0x54
#define REG_RIRBWP      0x58   /* 16: bit15 = reset                          */
#define REG_RINTCNT     0x5A   /* 16                                         */
#define REG_RIRBCTL     0x5C   /* 8:  bit1 = DMA run                         */
#define REG_RIRBSTS     0x5D   /* 8:  bit0 response interrupt, bit2 overrun  */
#define REG_RIRBSIZE    0x5E   /* 8                                          */
#define REG_ICOI        0x60   /* 32: immediate command out                  */
#define REG_ICII        0x64   /* 32: immediate response in                  */
#define REG_ICIS        0x68   /* 16: bit0 busy, bit1 result valid           */

/* --- stream descriptor registers (offsets from the descriptor base) ------ */
#define SD_CTL          0x00   /* 24 bits: byte 0 = SRST/RUN, byte 2 = tag   */
#define SD_STS          0x03   /* 8:  bit2 BCIS, bit3 FIFOE, bit4 DESE       */
#define SD_LPIB         0x04   /* 32: link position in buffer                */
#define SD_CBL          0x08   /* 32: cyclic buffer length in bytes          */
#define SD_LVI          0x0C   /* 16: last valid BDL index                   */
#define SD_FMT          0x12   /* 16: stream format                          */
#define SD_BDPL         0x18   /* 32                                         */
#define SD_BDPU         0x1C   /* 32                                         */

#define SD_BASE         0x80
#define SD_STRIDE       0x20

#define CTL_SRST        0x01
#define CTL_RUN         0x02
#define STS_BCIS        0x04

/* 48 kHz base, no multiply or divide, 16-bit samples, 2 channels. */
#define FMT_48K_S16_2   0x0011
#define STREAM_TAG      1

/* --- codec verbs and parameters ----------------------------------------- */
#define V_GET_PARAM      0xF00
#define V_GET_CONN_LIST  0xF02
#define V_GET_CFG_DEF    0xF1C
#define V_SET_STREAM_FMT 0x200
#define V_SET_AMP        0x300
#define V_SET_CONN_SEL   0x701
#define V_SET_POWER      0x705
#define V_SET_CONV_STRM  0x706
#define V_SET_PIN_CTL    0x707
#define V_SET_EAPD       0x70C

#define P_VENDOR_ID     0x00
#define P_NODE_COUNT    0x04
#define P_FUNC_TYPE     0x05
#define P_AUDIO_CAP     0x09
#define P_PIN_CAP       0x0C
#define P_IN_AMP_CAP    0x0D
#define P_CONN_LEN      0x0E
#define P_OUT_AMP_CAP   0x12

#define FUNC_AUDIO      0x01

#define WT_DAC          0x0    /* audio output converter */
#define WT_ADC          0x1
#define WT_MIXER        0x2
#define WT_SELECTOR     0x3
#define WT_PIN          0x4

#define WCAP_IN_AMP     (1u << 1)
#define WCAP_OUT_AMP    (1u << 2)
#define WCAP_AMP_OVR    (1u << 3)
#define WCAP_DIGITAL    (1u << 9)

#define PINCAP_HP_DRV   (1u << 3)
#define PINCAP_OUT      (1u << 4)
#define PINCAP_EAPD     (1u << 16)

#define PIN_CTL_OUT_EN  0x40
#define PIN_CTL_HP_EN   0x80

/* Amp payload selectors: which amp, and which channels, a SET_AMP applies to. */
#define AMP_SET_OUT     0x8000
#define AMP_SET_IN      0x4000
#define AMP_SET_LEFT    0x2000
#define AMP_SET_RIGHT   0x1000
#define AMP_MUTE        0x0080

/* Sample buffer: the caller's PCM, then a fixed block of silence the cyclic
 * engine falls into when the data runs out (see the header comment). */
#define CAP_BYTES       (256u * 1024)   /* ~1.36 s at 48 kHz stereo */
#define PAD_BYTES       (32u * 1024)    /* ~170 ms of silence       */

#define RIRB_OFFSET     2048            /* both rings share one page, 128-aligned */

struct bdl_entry { uint64_t addr; uint32_t len; uint32_t flags; } __attribute__((packed));

/* One hop of the route from the output pin back to the DAC. */
struct hop { uint16_t nid; uint8_t idx; uint8_t type; };

static struct {
    int present;
    volatile void *mm;             /* controller MMIO                         */
    uint32_t sd;                   /* output stream descriptor base offset    */
    struct dma_buf ring;           /* CORB + RIRB                             */
    struct dma_buf bdl;
    struct dma_buf pcm;
    volatile uint32_t *corb;
    volatile uint64_t *rirb;
    uint8_t  rirb_rp;
    int      use_corb;             /* 0 = immediate command registers         */
    uint8_t  cad;                  /* codec address                           */
    uint16_t afg, dac, pin, vol_nid;
    struct hop path[8];
    int      path_n;
    uint32_t data_bytes;           /* real PCM in the buffer now playing      */
} h;

static void udelay(uint32_t us) {
    if (hpet_present()) { hpet_delay_us(us); return; }
    for (uint32_t i = 0; i < us * 200; i++) __asm__ volatile("pause");
}

/* --- talking to a codec -------------------------------------------------- */

/* Both command paths take the same 32-bit word: codec address, node, then the
 * verb shifted up by 8 -- which puts a 12-bit verb at bits 19:8 with an 8-bit
 * payload below it, and a 4-bit verb at 19:16 with a 16-bit payload. The verb
 * constants above are written the way the spec numbers them, so one expression
 * covers both encodings. */
static uint32_t encode(uint16_t nid, uint16_t verb, uint16_t payload) {
    return ((uint32_t)h.cad << 28) | ((uint32_t)nid << 20) |
           ((uint32_t)verb << 8) | payload;
}

static int corb_send(uint32_t word, uint32_t *resp) {
    uint16_t wp = (uint16_t)((mmio_read16(h.mm, REG_CORBWP) + 1) & 0xFF);
    h.corb[wp] = word;
    mmio_write16(h.mm, REG_CORBWP, wp);
    for (int spin = 0; spin < 100000; spin++) {
        if ((mmio_read16(h.mm, REG_RIRBWP) & 0xFF) != h.rirb_rp) {
            h.rirb_rp = (uint8_t)((h.rirb_rp + 1) & 0xFF);
            if (resp) *resp = (uint32_t)h.rirb[h.rirb_rp];
            /* Acknowledge the response before the next command goes out. A
             * controller stops fetching from the CORB once RINTCNT responses
             * are sitting unacknowledged, and it will not start again until
             * this status bit is cleared -- so a driver that never writes here
             * gets exactly RINTCNT commands answered and then silence. Which
             * looks precisely like a codec that has stopped responding. */
            mmio_write8(h.mm, REG_RIRBSTS, 0x05);   /* RINTFL | overrun, RW1C */
            return 0;
        }
        udelay(1);
    }
    return -1;
}

static int imm_send(uint32_t word, uint32_t *resp) {
    for (int spin = 0; spin < 10000 && (mmio_read16(h.mm, REG_ICIS) & 0x1); spin++)
        udelay(1);
    if (mmio_read16(h.mm, REG_ICIS) & 0x1) return -1;
    mmio_write16(h.mm, REG_ICIS, 0x2);            /* clear a stale result */
    mmio_write32(h.mm, REG_ICOI, word);
    mmio_write16(h.mm, REG_ICIS, 0x1);            /* go */
    for (int spin = 0; spin < 100000; spin++) {
        if (mmio_read16(h.mm, REG_ICIS) & 0x2) {
            if (resp) *resp = mmio_read32(h.mm, REG_ICII);
            return 0;
        }
        udelay(1);
    }
    return -1;
}

static int try_cmd(uint16_t nid, uint16_t verb, uint16_t payload, uint32_t *resp) {
    uint32_t word = encode(nid, verb, payload);
    return h.use_corb ? corb_send(word, resp) : imm_send(word, resp);
}

/* Fire and forget; a codec that stops answering is caught during the probe. */
static uint32_t cmd(uint16_t nid, uint16_t verb, uint16_t payload) {
    uint32_t r = 0;
    try_cmd(nid, verb, payload, &r);
    return r;
}

static uint32_t param(uint16_t nid, uint8_t p) { return cmd(nid, V_GET_PARAM, p); }
static uint8_t  wtype(uint16_t nid) { return (uint8_t)((param(nid, P_AUDIO_CAP) >> 20) & 0xF); }

/* --- the widget graph ---------------------------------------------------- */

/* A widget's connection list, flattened: short (8-bit) and long (16-bit)
 * entries, with range entries expanded into the nodes they stand for. */
static int conn_list(uint16_t nid, uint16_t *out, int max) {
    uint32_t cl = param(nid, P_CONN_LEN);
    int len = (int)(cl & 0x7F);
    int longf = (int)((cl >> 7) & 1);
    int per = longf ? 2 : 4;
    int n = 0;
    for (int i = 0; i < len && n < max; ) {
        uint32_t r = cmd(nid, V_GET_CONN_LIST, (uint16_t)i);
        for (int k = 0; k < per && i < len && n < max; k++, i++) {
            uint16_t e = longf ? (uint16_t)((r >> (k * 16)) & 0xFFFF)
                               : (uint16_t)((r >> (k * 8)) & 0xFF);
            int range = longf ? (e & 0x8000) : (e & 0x80);
            e = (uint16_t)(e & (longf ? 0x7FFF : 0x7F));
            if (range && n > 0)
                for (uint16_t x = (uint16_t)(out[n - 1] + 1); x <= e && n < max; x++)
                    out[n++] = x;
            else
                out[n++] = e;
        }
    }
    return n;
}

/* Amp capabilities, honouring the widget's "override" bit: without it the
 * widget inherits the function group's defaults, and reading its own parameter
 * gives zero -- which would look exactly like "this widget has no amplifier". */
static uint32_t amp_cap(uint16_t nid, int input) {
    uint32_t wcap = param(nid, P_AUDIO_CAP);
    if (!(wcap & (uint32_t)(input ? WCAP_IN_AMP : WCAP_OUT_AMP))) return 0;
    uint16_t from = (wcap & WCAP_AMP_OVR) ? nid : h.afg;
    return param(from, (uint8_t)(input ? P_IN_AMP_CAP : P_OUT_AMP_CAP));
}

/* Set a widget's output amplifier to `pct` of its range, both channels. */
static void set_out_gain(uint16_t nid, int pct) {
    uint32_t cap = amp_cap(nid, 0);
    if (!cap) return;
    int steps = (int)((cap >> 8) & 0x7F);
    uint16_t p = AMP_SET_OUT | AMP_SET_LEFT | AMP_SET_RIGHT;
    if (pct <= 0 && (cap & 0x80000000u)) p |= AMP_MUTE;
    else p |= (uint16_t)(steps * pct / 100);
    cmd(nid, V_SET_AMP, p);
}

/* Unmute one input of a mixer at full gain. A mixer sums its inputs, so the
 * route through it is chosen by unmuting the one we want rather than by
 * selecting it. */
static void unmute_in(uint16_t nid, int idx) {
    uint32_t cap = amp_cap(nid, 1);
    if (!cap) return;
    int steps = (int)((cap >> 8) & 0x7F);
    cmd(nid, V_SET_AMP, (uint16_t)(AMP_SET_IN | AMP_SET_LEFT | AMP_SET_RIGHT |
                                   ((idx & 0xF) << 8) | steps));
}

/* Walk backwards from `nid` looking for a DAC, recording the branch that leads
 * to one at every fork. Depth-limited: a codec graph with a cycle in its
 * connection lists must not take the kernel down with it. */
static uint16_t trace_dac(uint16_t nid, int depth) {
    if (depth > 4) return 0;
    uint16_t conn[16];
    int n = conn_list(nid, conn, 16);
    for (int i = 0; i < n; i++) {
        uint8_t t = wtype(conn[i]);
        uint16_t found = 0;
        if (t == WT_DAC)
            found = conn[i];
        else if (t == WT_MIXER || t == WT_SELECTOR)
            found = trace_dac(conn[i], depth + 1);
        if (!found) continue;
        if (h.path_n < (int)(sizeof(h.path) / sizeof(h.path[0]))) {
            h.path[h.path_n].nid  = nid;
            h.path[h.path_n].idx  = (uint8_t)i;
            h.path[h.path_n].type = wtype(nid);
            h.path_n++;
        }
        return found;
    }
    return 0;
}

/* How much we want a given output pin. Line-out beats the built-in speaker
 * beats headphones; anything the codec says has no physical connection behind
 * it is not a candidate at all. */
static int pin_score(uint16_t nid) {
    if (param(nid, P_AUDIO_CAP) & WCAP_DIGITAL) return 0;   /* we send analogue */
    if (!(param(nid, P_PIN_CAP) & PINCAP_OUT)) return 0;
    uint32_t cfg = cmd(nid, V_GET_CFG_DEF, 0);
    if (((cfg >> 30) & 3) == 1) return 0;                   /* nothing behind it */
    switch ((cfg >> 20) & 0xF) {
        case 0:  return 30;                                 /* line out  */
        case 1:  return 25;                                 /* speaker   */
        case 2:  return 20;                                 /* headphone */
        default: return 1;
    }
}

/* Find the audio function group on codec `cad`, then the best output pin on it
 * and a route from that pin to a DAC. Returns 0 if this codec cannot play. */
static int claim_codec(uint8_t cad) {
    h.cad = cad;
    h.afg = h.dac = h.pin = h.vol_nid = 0;
    h.path_n = 0;

    uint32_t nc = 0;
    if (try_cmd(0, V_GET_PARAM, P_NODE_COUNT, &nc) != 0 || nc == 0) return 0;
    uint16_t first = (uint16_t)((nc >> 16) & 0xFF), count = (uint16_t)(nc & 0xFF);

    for (uint16_t fg = first; fg < first + count; fg++) {
        if ((param(fg, P_FUNC_TYPE) & 0x7F) != FUNC_AUDIO) continue;
        h.afg = fg;
        break;
    }
    if (!h.afg) return 0;

    cmd(h.afg, V_SET_POWER, 0);                    /* D0 */
    udelay(1000);

    uint32_t wn = param(h.afg, P_NODE_COUNT);
    uint16_t w0 = (uint16_t)((wn >> 16) & 0xFF), wc = (uint16_t)(wn & 0xFF);
    if (!wc) return 0;

    int best = 0;
    for (uint16_t w = w0; w < w0 + wc; w++) {
        if (wtype(w) != WT_PIN) continue;
        int s = pin_score(w);
        if (s <= best) continue;
        h.path_n = 0;
        uint16_t d = trace_dac(w, 0);
        if (!d) continue;
        best = s; h.pin = w; h.dac = d;
    }
    if (!h.pin) return 0;

    /* Re-run the walk for the pin we settled on: the loop above left h.path
     * holding whichever candidate it happened to examine last. */
    h.path_n = 0;
    trace_dac(h.pin, 0);

    /* Power and unmute everything on the route, then open the pin's driver. */
    cmd(h.dac, V_SET_POWER, 0);
    cmd(h.pin, V_SET_POWER, 0);
    set_out_gain(h.dac, 100);
    for (int i = 0; i < h.path_n; i++) {
        struct hop *p = &h.path[i];
        cmd(p->nid, V_SET_POWER, 0);
        if (p->type == WT_MIXER) unmute_in(p->nid, p->idx);
        else                     cmd(p->nid, V_SET_CONN_SEL, p->idx);
        set_out_gain(p->nid, 100);
    }

    uint32_t pincap = param(h.pin, P_PIN_CAP);
    uint8_t ctl = PIN_CTL_OUT_EN;
    if (pincap & PINCAP_HP_DRV) ctl |= PIN_CTL_HP_EN;
    cmd(h.pin, V_SET_PIN_CTL, ctl);
    if (pincap & PINCAP_EAPD) cmd(h.pin, V_SET_EAPD, 0x02);

    /* Bind the converter to our stream tag and tell it the sample format. It
     * has to agree with the stream descriptor exactly or the link stalls. */
    cmd(h.dac, V_SET_CONV_STRM, (uint16_t)(STREAM_TAG << 4));
    cmd(h.dac, V_SET_STREAM_FMT, FMT_48K_S16_2);

    /* Whichever end of the route owns a gain control becomes the volume knob;
     * everything else stays wide open so the two do not stack up. */
    h.vol_nid = amp_cap(h.dac, 0) ? h.dac : (amp_cap(h.pin, 0) ? h.pin : 0);
    return 1;
}

/* --- the stream engine --------------------------------------------------- */

static void stream_reset(void) {
    mmio_write8(h.mm, h.sd + SD_CTL, 0);
    for (int i = 0; i < 1000 && (mmio_read8(h.mm, h.sd + SD_CTL) & CTL_RUN); i++) udelay(10);
    mmio_write8(h.mm, h.sd + SD_CTL, CTL_SRST);
    for (int i = 0; i < 1000 && !(mmio_read8(h.mm, h.sd + SD_CTL) & CTL_SRST); i++) udelay(10);
    mmio_write8(h.mm, h.sd + SD_CTL, 0);
    for (int i = 0; i < 1000 && (mmio_read8(h.mm, h.sd + SD_CTL) & CTL_SRST); i++) udelay(10);
}

static uint32_t hda_capacity(void) { return h.present ? CAP_BYTES / 4 : 0; }

static void hda_submit(const int16_t *pcm, uint32_t frames) {
    if (!h.present) return;
    frames &= ~31u;                       /* keep every descriptor 128-byte sized */
    if (!frames) return;
    uint32_t data = frames * 4;
    if (data > CAP_BYTES) data = CAP_BYTES;
    memcpy(h.pcm.virt, pcm, data);

    struct bdl_entry *b = (struct bdl_entry *)h.bdl.virt;
    b[0].addr = h.pcm.phys;              b[0].len = data;      b[0].flags = 1;  /* IOC */
    b[1].addr = h.pcm.phys + CAP_BYTES;  b[1].len = PAD_BYTES; b[1].flags = 0;

    stream_reset();
    mmio_write32(h.mm, h.sd + SD_CBL, data + PAD_BYTES);
    mmio_write16(h.mm, h.sd + SD_LVI, 1);
    mmio_write16(h.mm, h.sd + SD_FMT, FMT_48K_S16_2);
    mmio_write32(h.mm, h.sd + SD_BDPL, (uint32_t)h.bdl.phys);
    mmio_write32(h.mm, h.sd + SD_BDPU, (uint32_t)(h.bdl.phys >> 32));
    mmio_write8 (h.mm, h.sd + SD_STS, 0x1C);            /* clear BCIS/FIFOE/DESE */
    mmio_write8 (h.mm, h.sd + SD_CTL + 2, (uint8_t)(STREAM_TAG << 4));
    h.data_bytes = data;
    mmio_write8 (h.mm, h.sd + SD_CTL, CTL_RUN);
}

static int hda_busy(void) {
    if (!h.present || !h.data_bytes) return 0;
    if (mmio_read8(h.mm, h.sd + SD_STS) & STS_BCIS) return 0;
    return mmio_read32(h.mm, h.sd + SD_LPIB) < h.data_bytes;
}

static void hda_stop(void) {
    if (!h.present) return;
    mmio_write8(h.mm, h.sd + SD_CTL, 0);
    h.data_bytes = 0;
}

static void hda_volume(int percent) {
    if (!h.present || !h.vol_nid) return;
    set_out_gain(h.vol_nid, percent);
}

static const audio_dev_t hda_dev = {
    .name     = "Intel HDA",
    .capacity = hda_capacity,
    .submit   = hda_submit,
    .busy     = hda_busy,
    .stop     = hda_stop,
    .volume   = hda_volume,
};

/* --- bring-up ------------------------------------------------------------ */

static void rings_init(void) {
    /* CORB at the top of the page, RIRB half way down: both need 128-byte
     * alignment and together they are well under one page. */
    h.corb = (volatile uint32_t *)h.ring.virt;
    h.rirb = (volatile uint64_t *)((uint8_t *)h.ring.virt + RIRB_OFFSET);

    mmio_write8 (h.mm, REG_CORBCTL, 0);
    mmio_write8 (h.mm, REG_RIRBCTL, 0);
    mmio_write8 (h.mm, REG_CORBSIZE, 0x02);            /* 256 entries */
    mmio_write8 (h.mm, REG_RIRBSIZE, 0x02);
    mmio_write32(h.mm, REG_CORBLBASE, (uint32_t)h.ring.phys);
    mmio_write32(h.mm, REG_CORBUBASE, (uint32_t)(h.ring.phys >> 32));
    mmio_write32(h.mm, REG_RIRBLBASE, (uint32_t)(h.ring.phys + RIRB_OFFSET));
    mmio_write32(h.mm, REG_RIRBUBASE, (uint32_t)((h.ring.phys + RIRB_OFFSET) >> 32));

    mmio_write16(h.mm, REG_CORBRP, 0x8000);            /* reset the read pointer */
    for (int i = 0; i < 1000 && !(mmio_read16(h.mm, REG_CORBRP) & 0x8000); i++) udelay(10);
    mmio_write16(h.mm, REG_CORBRP, 0);
    for (int i = 0; i < 1000 && (mmio_read16(h.mm, REG_CORBRP) & 0x8000); i++) udelay(10);
    mmio_write16(h.mm, REG_CORBWP, 0);

    mmio_write16(h.mm, REG_RIRBWP, 0x8000);            /* reset the write pointer */
    /* How many responses may accumulate before the controller stops fetching.
     * Interrupts are off here, so this is not about interrupt coalescing: it is
     * the ceiling on how far ahead the command ring may run. Set it high and
     * acknowledge every response anyway (see corb_send), so neither a missed
     * acknowledgement nor a long burst of verbs can stall the ring. */
    mmio_write16(h.mm, REG_RINTCNT, 0xFF);
    mmio_write8 (h.mm, REG_RIRBSTS, 0x05);             /* start from a clean status */
    h.rirb_rp = 0;

    mmio_write8(h.mm, REG_CORBCTL, 0x02);              /* DMA run */
    mmio_write8(h.mm, REG_RIRBCTL, 0x02);
}

const audio_dev_t *hda_probe(void) {
    struct pci_dev pd;
    if (!pci_find_by_class(0x04, 0x03, &pd)) return 0;

    struct pci_bar bar;
    if (pci_bar(&pd, 0, &bar) != 0 || !bar.is_mmio) {
        kprintf("[audio] HDA %x:%x has no MMIO BAR0\n", pd.vendor, pd.device);
        return 0;
    }
    h.mm = pci_map_bar(&bar);
    if (!h.mm) return 0;
    pci_enable_bus_master(&pd);

    /* Controller reset: drop CRST, wait for it to read back low, raise it, wait
     * for it to read back high. Codecs need about 521 us after that before they
     * show up in STATESTS, so the wait afterwards is not optional. */
    mmio_write32(h.mm, REG_GCTL, mmio_read32(h.mm, REG_GCTL) & ~1u);
    for (int i = 0; i < 1000 && (mmio_read32(h.mm, REG_GCTL) & 1); i++) udelay(10);
    udelay(500);
    mmio_write32(h.mm, REG_GCTL, mmio_read32(h.mm, REG_GCTL) | 1u);
    for (int i = 0; i < 1000 && !(mmio_read32(h.mm, REG_GCTL) & 1); i++) udelay(10);
    if (!(mmio_read32(h.mm, REG_GCTL) & 1)) {
        kprintf("[audio] HDA controller did not leave reset\n");
        return 0;
    }
    udelay(1000);

    mmio_write32(h.mm, REG_INTCTL, 0);                 /* we poll */
    mmio_write16(h.mm, REG_WAKEEN, 0);
    uint16_t statests = mmio_read16(h.mm, REG_STATESTS);
    mmio_write16(h.mm, REG_STATESTS, statests);        /* RW1C */
    if (!statests) {
        kprintf("[audio] HDA %x:%x has no codec attached\n", pd.vendor, pd.device);
        return 0;
    }

    uint16_t gcap = mmio_read16(h.mm, REG_GCAP);
    uint32_t iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    if (!oss) { kprintf("[audio] HDA reports no output streams\n"); return 0; }
    /* Input stream descriptors come first, then the output ones. */
    h.sd = SD_BASE + iss * SD_STRIDE;

    if (dma_alloc(4096, &h.ring) != 0) return 0;
    rings_init();

    /* Prefer CORB/RIRB, fall back to the immediate-command registers. Whichever
     * one answers a harmless read is the one used for everything after. */
    uint32_t probe_resp = 0;
    h.cad = 0;
    h.use_corb = 1;
    if (try_cmd(0, V_GET_PARAM, P_VENDOR_ID, &probe_resp) != 0 || probe_resp == 0) {
        mmio_write8(h.mm, REG_CORBCTL, 0);
        mmio_write8(h.mm, REG_RIRBCTL, 0);
        h.use_corb = 0;
        if (try_cmd(0, V_GET_PARAM, P_VENDOR_ID, &probe_resp) != 0 || probe_resp == 0) {
            kprintf("[audio] HDA codec answers on neither command path\n");
            dma_free(&h.ring);
            return 0;
        }
    }

    /* Walk the codecs STATESTS reported until one gives us a playable route. */
    int claimed = 0;
    for (uint8_t cad = 0; cad < 15 && !claimed; cad++) {
        if (!(statests & (1u << cad))) continue;
        claimed = claim_codec(cad);
    }
    if (!claimed) {
        kprintf("[audio] HDA: no codec offered a route to an output\n");
        dma_free(&h.ring);
        return 0;
    }

    if (dma_alloc(2 * sizeof(struct bdl_entry), &h.bdl) != 0) { dma_free(&h.ring); return 0; }
    if (dma_alloc(CAP_BYTES + PAD_BYTES, &h.pcm) != 0) {
        dma_free(&h.bdl); dma_free(&h.ring);
        return 0;
    }

    h.present = 1;
    kprintf("[ok] Intel HDA %x:%x (codec %d, pin %d -> dac %d, %s), 48kHz stereo\n",
            pd.vendor, pd.device, h.cad, h.pin, h.dac,
            h.use_corb ? "CORB/RIRB" : "immediate");
    return &hda_dev;
}
