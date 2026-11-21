/* BoltOS TV's AAC-LC decoder on the host, against ffmpeg.
 *
 * The same tv/aac.c and tv/aactab.c the TV runs, and mini/mp4.c to find the
 * audio track, with the heap stood in for. It decodes an MP4's sound into
 * raw s16le stereo; test/host/aaccmp.py holds that up against
 * `ffmpeg -i FILE -f s16le -ac 2 -`. AAC is decoded in floating point, so the
 * test is the signal-to-noise ratio, not equal bytes.
 *
 *   aactest FILE.mp4 OUT.raw
 *   aactest --imdct            (the transform against its definition)
 *
 * Build (see aaccmp.py, which does it):
 *   cc -O2 -DAAC_SELFTEST -I test/host/shim -o aactest test/host/aactest.c tv/aac.c tv/aactab.c mini/mp4.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "../../tv/aac.h"
#include "../../mini/mp4.h"

void *kmalloc(uint64_t n) { return malloc(n ? (size_t)n : 1); }
void  kfree(void *p) { free(p); }

int aac_selftest_imdct(void);

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--imdct")) return aac_selftest_imdct();
    if (argc < 3) { printf("usage: aactest FILE.mp4 OUT.raw\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) return 1;
    fclose(f);
    uint64_t off, size;
    if (!mp4_find_box(buf, (uint64_t)len, "moov", &off, &size)) { printf("no moov\n"); return 1; }
    mp4_t m;
    if (mp4_parse(buf + off, size, &m)) { printf("moov didn't parse\n"); return 1; }
    printf("audio: %d samples, %d Hz, %d ch, asc %d bytes\n", m.audio.n, m.rate, m.channels, m.asc_len);
    aac *d = aac_open(m.asc, m.asc_len);
    if (!d) { printf("AudioSpecificConfig rejected\n"); return 1; }
    printf("decoder: %d Hz, %d channels\n", aac_rate(d), aac_channels(d));
    FILE *o = fopen(argv[2], "wb");
    int16_t pcm[2048];
    int bad = 0;
    for (int i = 0; i < m.audio.n; i++) {
        mp4_sample *s = &m.audio.s[i];
        if (s->off + s->size > (uint64_t)len) break;
        if (aac_decode(d, buf + s->off, (int)s->size, pcm) < 0) {
            if (bad++ < 10) printf("unit %d (%u bytes, %02x %02x %02x %02x): %s\n", i, s->size, buf[s->off], buf[s->off + 1], buf[s->off + 2], buf[s->off + 3], aac_error(d));
        }
        fwrite(pcm, 2, 2048, o);
    }
    fclose(o);
    printf("%d units, %d failed\n", m.audio.n, bad);
    return bad ? 1 : 0;
}
