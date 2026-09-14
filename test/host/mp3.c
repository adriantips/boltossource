/* Run the shipping Layer III decoder on the host, so it can be held up against
 * a decoder nobody here wrote.
 *
 * Two jobs. `--selftest` checks the things that have an answer inside the file
 * itself: that every Huffman codeword decodes back to the symbol it was built
 * from, that the synthesis window unfolded from its 257-value prototype matches
 * rows of ISO 11172-3 Table B.3 that were checked by hand, and that a torn or
 * lying file is refused rather than followed. `--native FILE` decodes an MP3 and
 * writes raw signed 16-bit stereo at the file's own sample rate, which is what
 * t_mp3.py diffs against mpg123.
 *
 * Native rate, not 48 kHz, on purpose: comparing across this decoder's
 * resampler would be comparing two things at once, and the resampler is
 * wav.c's, already tested. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

/* kernel/mp3.c pulls in kheap.h and stdio.h for four calls between them. */
static void *kmalloc(unsigned long long n) { return malloc((size_t)n); }
static void kfree(void *p) { free(p); }
#define kheap_h_stub 1

#include "../../kernel/mp3.c"

/* ---- the checks that need no reference decoder --------------------------- */

static int fails;

static void check(int ok, const char *what) {
    if (!ok) { printf("  FAIL %s\n", what); fails++; }
}

/* Feed each codeword back through the tree it was built from. A tree that
 * dropped or misplaced a symbol shows up here rather than as noise in a file. */
static void selftest_huffman(void) {
    int symbols = 0;
    for (int t = 0; t < 34; t++) {
        if (!htab[t].n) continue;
        for (int s = 0; s < htab[t].n; s++) {
            uint8_t bytes[4] = { 0, 0, 0, 0 };
            int len = htab[t].len[s];
            uint32_t code = htab[t].cod[s];
            for (int b = 0; b < len; b++)
                if ((code >> (len - 1 - b)) & 1) bytes[b >> 3] |= (uint8_t)(0x80u >> (b & 7));
            br_t br;
            br_init(&br, bytes, sizeof bytes);
            int got = huff_sym(&br, root[t]);
            if (got != s || (int)br.bit != len) {
                printf("  FAIL table %d symbol %d decoded as %d in %u bits (want %d in %d)\n",
                       t, s, got, br.bit, s, len);
                fails++;
                return;
            }
            symbols++;
        }
    }
    printf("  ok: %d Huffman codewords round-trip through their trees\n", symbols);
}

/* Two rows of Table B.3, typed from the standard, against the window this
 * decoder unfolds from 257 integers. If the fold is wrong every file sounds
 * wrong, and nothing else in the decoder would tell you which half was at
 * fault -- so the table gets to be an independent witness.
 *
 * Note which way the table is read: printed entry (row r, column c) is
 * D[32c + r]. Reading it along the rows instead produces a window that passes
 * this check just as happily if the check is written the same wrong way, which
 * is how it survived a first pass -- so the indexing here is the assertion. */
static void selftest_window(void) {
    static const double row0[16] = {
         0.000000000, -0.000442505,  0.003250122, -0.007003784,
         0.031082153, -0.078628540,  0.100311279, -0.572036743,
         1.144989014,  0.572036743,  0.100311279,  0.078628540,
         0.031082153,  0.007003784,  0.003250122,  0.000442505
    };
    static const double row1[16] = {
        -0.000015259, -0.000473022,  0.003326416, -0.007919312,
         0.030517578, -0.084182739,  0.090927124, -0.600219727,
         1.144287109,  0.543823242,  0.108856201,  0.073059082,
         0.031478882,  0.006118774,  0.003173828,  0.000396729
    };
    for (int c = 0; c < 16; c++) {
        check(fabs(dwin[32 * c + 0] - row0[c]) < 1e-9, "synthesis window row 0");
        check(fabs(dwin[32 * c + 1] - row1[c]) < 1e-9, "synthesis window row 1");
    }
    if (!fails) printf("  ok: the synthesis window matches Table B.3 rows 0 and 1\n");
}

/* The synthesis window's squares sum to 64 -- that is how the standard
 * normalises it, and it is the one statement about all 512 values at once that
 * can be checked without a second filterbank to compare against. Together with
 * the two rows above it says the fold put every value somewhere sensible.
 *
 * Tempting and wrong, twice over, before this: "hold subband 0 and the output
 * should be flat" (band 0 is centred at fs/128, not DC) and then "it should be
 * that carrier" (summing the prototype's every-32nd shift folds it back to
 * period 32). Whether the filterbank is right is settled against mpg123 in
 * t_mp3.py, which is the only witness here that did not come out of this file. */
static void selftest_window_energy(void) {
    double s = 0;
    for (int i = 0; i < 512; i++) s += (double)dwin[i] * dwin[i];
    check(fabs(s - 64.0) < 1e-3, "the synthesis window's squares sum to 64");
    if (!fails) printf("  ok: the window is normalised (sum of squares %.4f)\n", s);
}

static void selftest_sniff(void) {
    uint8_t wav[256];
    memset(wav, 0, sizeof wav);
    memcpy(wav, "RIFF", 4);
    memcpy(wav + 8, "WAVEfmt ", 8);
    check(!mp3_sniff(wav, sizeof wav), "a WAV must not sniff as an MP3");

    uint8_t junk[4096];
    for (size_t i = 0; i < sizeof junk; i++) junk[i] = (uint8_t)(i * 37u);
    check(!mp3_sniff(junk, sizeof junk), "sawtooth bytes must not sniff as an MP3");

    uint8_t tag[512];
    memset(tag, 0, sizeof tag);
    memcpy(tag, "ID3", 3);
    check(mp3_sniff(tag, sizeof tag), "an ID3 tag is an MP3");
    if (!fails) printf("  ok: sniffing tells MP3 from not-MP3\n");
}

/* Every prefix of a real file, and every file with one byte corrupted, has to
 * either decode or be refused -- never wander off the end of the buffer. The
 * fuzzers in t_fuzz.py do this properly; this is the cheap version that runs
 * next to the decoder it guards. */
static void selftest_damage(const uint8_t *file, uint32_t len) {
    int16_t pcm[4096];
    for (uint32_t cut = 1; cut < len; cut += (len / 40) + 1) {
        const char *err;
        mp3_t *m = mp3_open(file, cut, &err);
        if (!m) continue;
        for (int i = 0; i < 12; i++) if (!mp3_read(m, pcm, 1024)) break;
        mp3_close(m);
    }
    unsigned seed = 12345;
    uint8_t *copy = (uint8_t *)malloc(len);
    for (int round = 0; round < 300; round++) {
        memcpy(copy, file, len);
        for (int k = 0; k < 8; k++) {
            seed = seed * 1103515245u + 12345u;
            copy[(seed >> 8) % len] ^= (uint8_t)(seed >> 20);
        }
        const char *err;
        mp3_t *m = mp3_open(copy, len, &err);
        if (!m) continue;
        for (int i = 0; i < 12; i++) if (!mp3_read(m, pcm, 1024)) break;
        mp3_close(m);
    }
    free(copy);
    printf("  ok: truncated and corrupted files decode or are refused, never crash\n");
}

/* ---- decoding a file ----------------------------------------------------- */

static uint8_t *slurp(const char *path, uint32_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read\n"); exit(2); }
    fclose(f);
    *len = (uint32_t)n;
    return buf;
}

/* Decode straight out of the frame buffer, before the resampler sees it. */
static int dump_native(const char *in, const char *out) {
    uint32_t len;
    uint8_t *file = slurp(in, &len);
    const char *err = "?";
    mp3_t *m = mp3_open(file, len, &err);
    if (!m) { fprintf(stderr, "mp3_open: %s\n", err); return 2; }

    FILE *o = fopen(out, "wb");
    if (!o) { fprintf(stderr, "cannot write %s\n", out); return 2; }
    fprintf(stderr, "%s | %u frames | %u ms\n",
            mp3_describe(m), m->nframes, mp3_duration_ms(m));

    uint32_t total = 0;
    while (!m->ended) {
        uint32_t n = decode_frame(m);
        if (!n) break;
        fwrite(m->nat + 2, sizeof(int16_t), (size_t)n * 2, o);
        total += n;
    }
    fclose(o);
    fprintf(stderr, "decoded %u frames at %u Hz\n", total, mp3_rate(m));
    mp3_close(m);
    free(file);
    return 0;
}

/* The whole shipping path: open, read at 48 kHz stereo, seek, read again. */
static int dump_48k(const char *in, const char *out) {
    uint32_t len;
    uint8_t *file = slurp(in, &len);
    const char *err = "?";
    mp3_t *m = mp3_open(file, len, &err);
    if (!m) { fprintf(stderr, "mp3_open: %s\n", err); return 2; }
    FILE *o = fopen(out, "wb");
    int16_t pcm[4096];
    for (;;) {
        uint32_t n = mp3_read(m, pcm, 2048);
        if (!n) break;
        fwrite(pcm, sizeof(int16_t), (size_t)n * 2, o);
    }
    fclose(o);
    mp3_close(m);
    free(file);
    return 0;
}

/* Seek, then decode from there. The point is where it lands: an MP3 has no
 * index, so seeking is arithmetic on byte offsets (or the Xing table of
 * contents), and landing in the wrong place is the failure mode. */
static int dump_seek(const char *in, uint32_t ms, const char *out) {
    uint32_t len;
    uint8_t *file = slurp(in, &len);
    const char *err = "?";
    mp3_t *m = mp3_open(file, len, &err);
    if (!m) { fprintf(stderr, "mp3_open: %s\n", err); return 2; }

    mp3_seek_ms(m, ms);
    FILE *o = fopen(out, "wb");
    int16_t pcm[4096];
    uint32_t made = 0;
    while (made < 48000u * 2) {                 /* two seconds is plenty */
        uint32_t n = mp3_read(m, pcm, 2048);
        if (!n) break;
        fwrite(pcm, sizeof(int16_t), (size_t)n * 2, o);
        made += n;
    }
    fclose(o);
    fprintf(stderr, "asked for %u ms, decoder reports %u ms, %u frames\n",
            ms, mp3_position_ms(m) - made * 1000 / 48000, made);
    mp3_close(m);
    free(file);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) {
        build_tables();
        selftest_huffman();
        selftest_window();
        selftest_window_energy();
        selftest_sniff();
        if (argc >= 3) {
            uint32_t len;
            uint8_t *f = slurp(argv[2], &len);
            selftest_damage(f, len);
            free(f);
        }
        if (fails) { printf("%d failure(s)\n", fails); return 1; }
        printf("host mp3: ok\n");
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "--native") == 0) return dump_native(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "--48k") == 0)    return dump_48k(argv[2], argv[3]);
    if (argc == 5 && strcmp(argv[1], "--seek") == 0)
        return dump_seek(argv[3], (uint32_t)atoi(argv[2]), argv[4]);

    fprintf(stderr, "usage: mp3-host --selftest [FILE.mp3]\n"
                    "       mp3-host --native FILE.mp3 OUT.pcm\n"
                    "       mp3-host --48k    FILE.mp3 OUT.pcm\n"
                    "       mp3-host --seek MS FILE.mp3 OUT.pcm\n");
    return 2;
}
