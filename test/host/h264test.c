/* BoltOS Mini's H.264 decoder on the host, against ffmpeg.
 *
 * The same mini/h264*.c and mini/mp4.c the Pi runs, with the heap stood in
 * for. It decodes an MP4's video into raw I420; test/host/h264cmp.py holds
 * that up against `ffmpeg -f rawvideo -pix_fmt yuv420p`, frame by frame --
 * H.264 decoding is exact, so every byte must match.
 *
 *   h264test FILE.mp4 OUT.yuv [FRAMES]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../mini/h264.h"
#include "../../mini/mp4.h"

void *kmalloc(uint64_t n) { return malloc(n ? (size_t)n : 1); }
void  kfree(void *p) { free(p); }
#include <stdarg.h>
void h264_trace(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: h264test FILE.mp4 OUT.yuv [FRAMES]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    fread(buf, 1, (size_t)len, f); fclose(f);
    int maxf = argc > 3 ? atoi(argv[3]) : 1 << 30;
    extern int h264_skip_deblock;
    h264_skip_deblock = getenv("NODBK") != 0;
    extern int h264_debug;
    h264_debug = getenv("TRACE") ? atoi(getenv("TRACE")) : 0;
    uint64_t off, size;
    if (!mp4_find_box(buf, (uint64_t)len, "moov", &off, &size)) { printf("no moov\n"); return 1; }
    mp4_t m;
    if (mp4_parse(buf + off, size, &m)) { printf("moov didn't parse\n"); return 1; }
    printf("%dx%d, %d video samples, timescale %d\n", m.w, m.h, m.video.n, m.video.timescale);
    h264 *d = h264_open(m.avcc, m.avcc_len);
    if (!d) { printf("avcC rejected\n"); return 1; }
    FILE *o = fopen(argv[2], "wb");
    int in = 0, out = 0;
    h264_pic p;
    for (int i = 0; i < m.video.n && out < maxf; i++) {
        mp4_sample *s = &m.video.s[i];
        if (s->off + s->size > (uint64_t)len) break;
        if (h264_decode(d, buf + s->off, (int)s->size, s->pts) < 0) { printf("sample %d: %s\n", i, h264_error(d)); break; }
        in++;
        while (out < maxf && h264_output(d, &p)) {
            for (int y = 0; y < p.h; y++) fwrite(p.y + y * p.stride, 1, (size_t)p.w, o);
            for (int y = 0; y < p.h / 2; y++) fwrite(p.u + y * p.cstride, 1, (size_t)(p.w / 2), o);
            for (int y = 0; y < p.h / 2; y++) fwrite(p.v + y * p.cstride, 1, (size_t)(p.w / 2), o);
            out++;
        }
    }
    h264_flush(d);
    while (out < maxf && h264_output(d, &p)) {
        for (int y = 0; y < p.h; y++) fwrite(p.y + y * p.stride, 1, (size_t)p.w, o);
        for (int y = 0; y < p.h / 2; y++) fwrite(p.u + y * p.cstride, 1, (size_t)(p.w / 2), o);
        for (int y = 0; y < p.h / 2; y++) fwrite(p.v + y * p.cstride, 1, (size_t)(p.w / 2), o);
        out++;
    }
    fclose(o);
    printf("decoded %d access units, wrote %d pictures\n", in, out);
    h264_close(d);
    return 0;
}
