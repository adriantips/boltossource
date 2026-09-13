#include <stdint.h>
#include <stddef.h>
#include "wav.h"
#include "audio.h"
#include "vfs.h"
#include "kheap.h"
#include "string.h"

/* ===========================================================================
 *  RIFF/WAVE decoder + resampler. See wav.h for the contract.
 *
 *  The resampler is linear interpolation driven by a 16.16 fixed-point cursor:
 *  no floating point (this runs in the kernel, on the same thread that feeds
 *  the codec), and the error it leaves is far below what an 8-bit source
 *  carries anyway. Upsampling -- the common case, since most files are 22 or
 *  44.1 kHz and the codec wants 48 -- is what it is good at. Downsampling a
 *  96 kHz file decimates without a low-pass first, so anything above 24 kHz in
 *  the source will alias; that is a real limitation and not one worth a filter
 *  bank until something actually ships 96 kHz audio.
 * ===========================================================================*/

#define MAX_FILE  (64u * 1024 * 1024)   /* refuse to swallow the whole heap */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One source sample, sign- and width-normalised to signed 16-bit. */
static int32_t sample(const wav_t *w, uint32_t frame, int ch) {
    uint32_t bps = (uint32_t)w->bits / 8;
    uint64_t off = (uint64_t)frame * w->channels * bps + (uint64_t)ch * bps;
    if (off + bps > w->bytes) return 0;
    const uint8_t *p = w->data + off;
    /* 8-bit WAV samples are *unsigned*, centred on 128 -- read one as signed
     * and quiet passages come out as full-scale noise. */
    if (w->bits == 8) return ((int32_t)p[0] - 128) << 8;
    return (int16_t)le16(p);
}

int wav_open(const void *file, uint32_t len, wav_t *w, const char **err) {
    const char *why = "not a WAV file";
    const uint8_t *f = (const uint8_t *)file;
    memset(w, 0, sizeof *w);

    if (!f || len < 12 || memcmp(f, "RIFF", 4) != 0 || memcmp(f + 8, "WAVE", 4) != 0)
        goto fail;

    int have_fmt = 0;
    uint16_t format = 0;
    for (uint32_t off = 12; off + 8 <= len; ) {
        const uint8_t *id = f + off;
        uint32_t sz = le32(f + off + 4);
        uint32_t body = off + 8;
        /* A file truncated mid-chunk still plays what it has, which is what a
         * player should do with a partial download rather than refusing it. */
        if (sz > len - body) sz = len - body;

        if (memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            format       = le16(f + body);
            w->channels  = le16(f + body + 2);
            w->rate      = le32(f + body + 4);
            w->bits      = le16(f + body + 14);
            /* WAVE_FORMAT_EXTENSIBLE is how Windows writes anything above two
             * channels; the real format sits in the sub-format GUID. */
            if (format == 0xFFFE && sz >= 40) format = le16(f + body + 24);
            have_fmt = 1;
        } else if (memcmp(id, "data", 4) == 0) {
            w->data  = f + body;
            w->bytes = sz;
        }
        off = body + sz + (sz & 1);          /* chunks are word-aligned */
    }

    if (!have_fmt)                    { why = "no fmt chunk"; goto fail; }
    if (!w->data || !w->bytes)        { why = "no audio data"; goto fail; }
    if (format != 1)                  { why = "not PCM (compressed WAV)"; goto fail; }
    if (w->bits != 8 && w->bits != 16){ why = "only 8- and 16-bit samples"; goto fail; }
    if (w->channels < 1 || w->channels > 8) { why = "odd channel count"; goto fail; }
    if (w->rate < 4000 || w->rate > 192000) { why = "sample rate out of range"; goto fail; }

    w->frames = w->bytes / ((uint32_t)w->channels * (w->bits / 8));
    if (!w->frames)                   { why = "no complete frames"; goto fail; }
    w->step = ((uint64_t)w->rate << 16) / AUDIO_RATE;
    w->posf = 0;
    return 0;

fail:
    if (err) *err = why;
    return -1;
}

uint32_t wav_read(wav_t *w, int16_t *out, uint32_t frames) {
    uint32_t made = 0;
    while (made < frames) {
        uint64_t idx = w->posf >> 16;
        if (idx >= w->frames) break;
        uint32_t frac = (uint32_t)(w->posf & 0xFFFF);
        uint32_t next = (idx + 1 < w->frames) ? (uint32_t)idx + 1 : (uint32_t)idx;

        for (int ch = 0; ch < 2; ch++) {
            /* A mono source feeds both speakers; anything wider than stereo is
             * carried by its first two channels, which for every ordinary
             * layout are the front pair. */
            int src = (ch == 1 && w->channels < 2) ? 0 : ch;
            int32_t a = sample(w, (uint32_t)idx, src);
            int32_t b = sample(w, next, src);
            out[made * 2 + ch] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * frac) >> 16));
        }
        w->posf += w->step;
        made++;
    }
    return made;
}

int      wav_done(const wav_t *w)        { return (w->posf >> 16) >= w->frames; }
uint32_t wav_duration_ms(const wav_t *w) { return (uint32_t)((uint64_t)w->frames * 1000 / w->rate); }
uint32_t wav_position_ms(const wav_t *w) {
    uint64_t idx = w->posf >> 16;
    if (idx > w->frames) idx = w->frames;
    return (uint32_t)(idx * 1000 / w->rate);
}

void wav_seek_ms(wav_t *w, uint32_t ms) {
    uint64_t frame = (uint64_t)ms * w->rate / 1000;
    if (frame > w->frames) frame = w->frames;
    w->posf = frame << 16;
}

int wav_load(const char *path, wav_t *w, uint8_t **buf, const char **err) {
    *buf = 0;
    file *fh = vfs_open(path, O_RDONLY);
    if (!fh) { if (err) *err = "cannot open"; return -1; }

    int64_t size = vfs_lseek(fh, 0, SEEK_END);
    vfs_lseek(fh, 0, SEEK_SET);
    if (size <= 0 || (uint64_t)size > MAX_FILE) {
        vfs_close(fh);
        if (err) *err = size <= 0 ? "empty file" : "file too large";
        return -1;
    }

    uint8_t *mem = (uint8_t *)kmalloc((uint64_t)size);
    if (!mem) { vfs_close(fh); if (err) *err = "out of memory"; return -1; }

    /* Read in a loop: vfs_read is free to return a short count, and a WAV that
     * stops at the first partial read is a WAV that plays its first second. */
    int64_t got = 0;
    while (got < size) {
        int64_t n = vfs_read(fh, mem + got, (uint64_t)(size - got));
        if (n <= 0) break;
        got += n;
    }
    vfs_close(fh);

    if (wav_open(mem, (uint32_t)got, w, err) != 0) { kfree(mem); return -1; }
    *buf = mem;
    return 0;
}

/* ===========================================================================
 *  Writing one out.
 *
 *  A canonical 44-byte header and then the samples. No conversion: a capture
 *  arrives at 48 kHz signed 16-bit stereo, which is the one format every
 *  decoder on earth reads, and resampling it on the way to the disk would be
 *  throwing away the only copy that exists.
 * ===========================================================================*/

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

void wav_fix_length(uint8_t *h, uint16_t channels, uint32_t frames) {
    uint32_t bytes = frames * channels * 2u;
    put32(h + 4,  36u + bytes);        /* RIFF chunk size: everything after it */
    put32(h + 40, bytes);              /* data chunk size                       */
}

void wav_header(uint8_t *h, uint32_t rate, uint16_t channels, uint32_t frames) {
    memcpy(h + 0,  "RIFF", 4);
    memcpy(h + 8,  "WAVEfmt ", 8);
    put32(h + 16, 16);                             /* fmt chunk size, PCM      */
    put16(h + 20, 1);                              /* WAVE_FORMAT_PCM          */
    put16(h + 22, channels);
    put32(h + 24, rate);
    put32(h + 28, rate * channels * 2u);           /* byte rate                */
    put16(h + 32, (uint16_t)(channels * 2));       /* block align              */
    put16(h + 34, 16);                             /* bits per sample          */
    memcpy(h + 36, "data", 4);
    wav_fix_length(h, channels, frames);
}

int wav_save(const char *path, const int16_t *pcm, uint32_t frames,
             uint32_t rate, uint16_t channels, const char **err) {
    if (!path || !pcm || !frames || !channels) { if (err) *err = "nothing to write"; return -1; }

    file *fh = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!fh) { if (err) *err = "cannot create"; return -1; }

    uint8_t hdr[WAV_HDR_BYTES];
    wav_header(hdr, rate, channels, frames);
    if (vfs_write(fh, hdr, sizeof hdr) != (int64_t)sizeof hdr) {
        vfs_close(fh);
        if (err) *err = "short write on the header";
        return -1;
    }

    /* In chunks, because vfs_write is free to return a short count and a
     * recording that stops at the first one is a recording of its first
     * second. Same loop wav_load() reads with, for the same reason. */
    uint64_t left = (uint64_t)frames * channels * 2u;
    const uint8_t *p = (const uint8_t *)pcm;
    while (left) {
        int64_t n = vfs_write(fh, p, left > 65536u ? 65536u : left);
        if (n <= 0) { vfs_close(fh); if (err) *err = "short write"; return -1; }
        p += n; left -= (uint64_t)n;
    }
    vfs_close(fh);
    return 0;
}
