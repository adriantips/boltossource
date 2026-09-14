#include <stdint.h>
#include <stddef.h>
#include "sound.h"
#include "audio.h"
#include "vfs.h"
#include "kheap.h"
#include "string.h"

/* ===========================================================================
 *  Dispatch between the decoders. See sound.h for why this exists.
 *
 *  Nearly every function here is three lines and a switch. The one with an
 *  opinion is snd_open(), which decides what a file is: MP3 first, because
 *  mp3_sniff() insists on either an ID3 tag or two frame headers that agree
 *  with each other, and WAV second, because "RIFF....WAVE" is four bytes of
 *  evidence and would happily claim a file that merely started that way.
 * ===========================================================================*/

#define MAX_FILE (64u * 1024 * 1024)

static char about[96];

static char *put_str(char *at, char *end, const char *s) {
    while (*s && at < end - 1) *at++ = *s++;
    *at = 0;
    return at;
}
static char *put_num(char *at, char *end, uint32_t v) {
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n-- > 0 && at < end - 1) *at++ = tmp[n];
    *at = 0;
    return at;
}

/* Read the whole file. Both decoders work from the bytes in place, so this is
 * the only copy that exists -- which is also why the buffer belongs to snd_t
 * and not to the caller. */
static int slurp(const char *path, uint8_t **buf, uint32_t *len, const char **err) {
    *buf = 0;
    *len = 0;
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

    int64_t got = 0;
    while (got < size) {
        int64_t n = vfs_read(fh, mem + got, (uint64_t)(size - got));
        if (n <= 0) break;          /* a partial file still plays what it has */
        got += n;
    }
    vfs_close(fh);

    *buf = mem;
    *len = (uint32_t)got;
    return 0;
}

int snd_open(const char *path, snd_t *s, const char **err) {
    memset(s, 0, sizeof *s);
    const char *why = "not a sound file";
    if (slurp(path, &s->bytes, &s->len, err) != 0) return -1;

    if (mp3_sniff(s->bytes, s->len)) {
        s->mp3 = mp3_open(s->bytes, s->len, &why);
        if (s->mp3) { s->kind = SND_MP3; return 0; }
        /* Sniffed as one and would not open: say what the decoder said rather
         * than falling through and reporting it as a bad WAV. */
        kfree(s->bytes);
        s->bytes = 0;
        if (err) *err = why;
        return -1;
    }

    if (wav_open(s->bytes, s->len, &s->wav, &why) == 0) { s->kind = SND_WAV; return 0; }

    kfree(s->bytes);
    s->bytes = 0;
    if (err) *err = why;
    return -1;
}

void snd_close(snd_t *s) {
    if (!s) return;
    if (s->mp3) mp3_close(s->mp3);
    if (s->bytes) kfree(s->bytes);
    memset(s, 0, sizeof *s);
}

uint32_t snd_read(snd_t *s, int16_t *out, uint32_t frames) {
    if (!s) return 0;
    if (s->kind == SND_MP3) return mp3_read(s->mp3, out, frames);
    if (s->kind == SND_WAV) return wav_read(&s->wav, out, frames);
    return 0;
}

uint32_t snd_step(snd_t *s, int16_t *out, uint32_t frames) {
    if (!s) return 0;
    if (s->kind == SND_MP3) return mp3_step(s->mp3, out, frames);
    /* Resampling a WAV is three table-free multiplies a sample; there is no
     * work here worth spreading out. */
    if (s->kind == SND_WAV) return wav_read(&s->wav, out, frames);
    return 0;
}

uint32_t snd_duration_ms(const snd_t *s) {
    if (!s) return 0;
    if (s->kind == SND_MP3) return mp3_duration_ms(s->mp3);
    if (s->kind == SND_WAV) return wav_duration_ms(&s->wav);
    return 0;
}

uint32_t snd_position_ms(const snd_t *s) {
    if (!s) return 0;
    if (s->kind == SND_MP3) return mp3_position_ms(s->mp3);
    if (s->kind == SND_WAV) return wav_position_ms(&s->wav);
    return 0;
}

void snd_seek_ms(snd_t *s, uint32_t ms) {
    if (!s) return;
    if (s->kind == SND_MP3) mp3_seek_ms(s->mp3, ms);
    else if (s->kind == SND_WAV) wav_seek_ms(&s->wav, ms);
}

int snd_done(const snd_t *s) {
    if (!s) return 1;
    if (s->kind == SND_MP3) return mp3_done(s->mp3);
    if (s->kind == SND_WAV) return wav_done(&s->wav);
    return 1;
}

const char *snd_describe(const snd_t *s) {
    if (!s) return "";
    if (s->kind == SND_MP3) return mp3_describe(s->mp3);
    if (s->kind != SND_WAV) return "";

    char *at = about, *end = about + sizeof about;
    at = put_str(at, end, "WAV, ");
    at = put_num(at, end, s->wav.rate / 1000);
    at = put_str(at, end, ".");
    at = put_num(at, end, (s->wav.rate % 1000) / 100);
    at = put_str(at, end, " kHz ");
    at = put_str(at, end, s->wav.channels == 1 ? "mono" :
                          s->wav.channels == 2 ? "stereo" : "multichannel");
    at = put_str(at, end, ", ");
    at = put_num(at, end, s->wav.bits);
    put_str(at, end, "-bit");
    return about;
}

int snd_is_audio_name(const char *n) {
    if (!n) return 0;
    int ln = (int)strlen(n);
    if (ln < 5) return 0;
    char e[4];
    const char *p = n + ln - 4;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        e[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    if (e[0] != '.') return 0;
    return (e[1] == 'w' && e[2] == 'a' && e[3] == 'v') ||
           (e[1] == 'm' && e[2] == 'p' && e[3] == '3');
}
