/* Fuzzing the decoders, which is the third place a stranger picks the bytes.
 *
 * t_fuzz.py's own comment says the network stack and the browser are the two
 * subsystems that parse what someone else wrote. That was never true: GUIDE.md
 * tells people to `download` a track and then `play` it, so kernel/mp3.c is the
 * same trust boundary with none of the same scrutiny -- 1500 lines of bit-level
 * parsing in ring 0, over a file we did not make. Roadmap item 99 says so in
 * its own "known gap" paragraph. This is that paragraph paid off.
 *
 * Three things about Layer III make it worse than the line count suggests, and
 * all three are why the targets below decode rather than merely open:
 *
 *   the bit reservoir    main_data_begin says "my granule starts N bytes
 *                        before this frame", and N comes out of the file
 *   part2_3_length       says how many bits the Huffman data occupies, and
 *                        nothing in the format makes it agree with the frame
 *   codebooks 16..31     read up to 13 linbits per value, and the result
 *                        indexes a power table
 *
 * WAV is in here too. It is a much smaller parser, but it is reached by the
 * same `download` and the same `play`, and snd_open() picks between the two by
 * sniffing -- so the dispatch is fuzzed as well, through a VFS that serves the
 * input as though it were a file on the disk.
 *
 * The corpus is test/seeds/mp3/, which exists for exactly this and says so in
 * its README: six files that between them reach short blocks, mono side
 * information, a Xing table of contents, MPEG-2 LSF scalefactor partitions and
 * 8 kHz band tables. A mutator handed random bytes reaches none of that. The
 * WAV seeds are generated here by wav_header(), the shipping writer, because a
 * format whose own encoder lives in the tree may as well seed itself. */
#include <math.h>
#include "fuzz.h"
#include "vfs.h"
#include "wav.h"
#include "sound.h"

/* ---- a filesystem holding exactly one file ------------------------------ */

/* kernel/sound.c reaches the bytes through the VFS, and what it does with the
 * length and the short reads is part of what is under test, so this is a real
 * (if small) backend rather than a hole punched through snd_open(). The handle
 * is malloc'd, not kmalloc'd: the guarded heap belongs to the code being
 * fuzzed, and a leak report should not be able to mean "the harness". */
static const uint8_t *vfile;
static uint32_t       vfile_len;

typedef struct { uint32_t pos; } vhandle;

file *vfs_open(const char *path, int flags) {
    (void)path; (void)flags;
    if (!vfile) return 0;
    file *f = (file *)calloc(1, sizeof *f);
    if (!f) return 0;
    vhandle *h = (vhandle *)calloc(1, sizeof *h);
    if (!h) { free(f); return 0; }
    f->priv = h;
    return f;
}

int64_t vfs_read(file *f, void *buf, uint64_t len) {
    vhandle *h = (vhandle *)f->priv;
    if (h->pos >= vfile_len) return 0;
    uint64_t left = vfile_len - h->pos;
    if (len > left) len = left;
    /* Short reads on purpose, every other call: slurp() loops for them and a
     * loop that is never made to go round twice is a loop nobody has tested. */
    if (len > 1 && (h->pos & 1)) len /= 2;
    memcpy(buf, vfile + h->pos, (size_t)len);
    h->pos += (uint32_t)len;
    return (int64_t)len;
}

int64_t vfs_write(file *f, const void *buf, uint64_t len) {
    (void)f; (void)buf;
    return (int64_t)len;                 /* nothing here writes; wav_save links */
}

int64_t vfs_lseek(file *f, int64_t off, int whence) {
    vhandle *h = (vhandle *)f->priv;
    int64_t at = whence == SEEK_END ? (int64_t)vfile_len + off
               : whence == SEEK_CUR ? (int64_t)h->pos + off : off;
    if (at < 0) at = 0;
    if (at > (int64_t)vfile_len) at = (int64_t)vfile_len;
    h->pos = (uint32_t)at;
    return at;
}

void vfs_close(file *f) { if (f) { free(f->priv); free(f); } }

/* The decoder is included rather than linked so the selftest can reach the
 * tables it builds -- same arrangement as test/host/mp3.c, and for the same
 * reason. wav.c and sound.c stay separate objects. */
#include "../../kernel/mp3.c"

/* ---- corpus ------------------------------------------------------------- */

#define MAXSEED (1u << 17)

static fz_seed mp3_seeds[8];
static int     n_mp3_seeds;
static fz_seed wav_seeds[4];
static int     n_wav_seeds;
static fz_seed any_seeds[12];
static int     n_any_seeds;

static int load_seed(const char *path, fz_seed *into) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t *buf = (uint8_t *)malloc(MAXSEED);
    size_t n = fread(buf, 1, MAXSEED, f);
    fclose(f);
    if (!n) { free(buf); return -1; }
    into->d = buf;
    into->n = (uint32_t)n;
    return 0;
}

/* A WAV, written by the writer that ships. Eight bits is not decoration: an
 * 8-bit sample is unsigned and centred on 128, which is its own branch in
 * sample(), and it is the one wav.c's comments say was got wrong once. */
static void make_wav(fz_seed *into, uint32_t rate, uint16_t chans, int bits) {
    uint32_t frames = 400;
    uint32_t bytes  = frames * chans * (uint32_t)(bits / 8);
    uint8_t *buf = (uint8_t *)malloc(WAV_HDR_BYTES + bytes);
    wav_header(buf, rate, chans, frames);
    if (bits == 8) {                     /* fix the two fields wav_header fixes at 16 */
        buf[34] = 8; buf[35] = 0;
        buf[32] = (uint8_t)chans; buf[33] = 0;
        buf[28] = (uint8_t)(rate * chans); buf[29] = (uint8_t)((rate * chans) >> 8);
        buf[30] = (uint8_t)((rate * chans) >> 16); buf[31] = (uint8_t)((rate * chans) >> 24);
        buf[40] = (uint8_t)bytes; buf[41] = (uint8_t)(bytes >> 8);
        buf[42] = (uint8_t)(bytes >> 16); buf[43] = (uint8_t)(bytes >> 24);
    }
    uint8_t *d = buf + WAV_HDR_BYTES;
    for (uint32_t i = 0; i < frames; i++)
        for (uint16_t c = 0; c < chans; c++) {
            double v = sin(i * 0.07 + c);
            if (bits == 8) *d++ = (uint8_t)(128 + (int)(v * 100));
            else { int16_t s = (int16_t)(v * 20000); *d++ = (uint8_t)s; *d++ = (uint8_t)(s >> 8); }
        }
    into->d = buf;
    into->n = WAV_HDR_BYTES + bytes;
}

static void load_corpus(void) {
    static const char *names[] = {
        "test/seeds/mp3/mpeg1-cbr.mp3",
        "test/seeds/mp3/mpeg1-short.mp3",
        "test/seeds/mp3/mpeg1-mono.mp3",
        "test/seeds/mp3/mpeg1-vbr-toc.mp3",
        "test/seeds/mp3/mpeg2-lsf.mp3",
        "test/seeds/mp3/mpeg25-8khz.mp3",
    };
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
        if (load_seed(names[i], &mp3_seeds[n_mp3_seeds]) == 0) n_mp3_seeds++;

    make_wav(&wav_seeds[n_wav_seeds++], 44100, 2, 16);
    make_wav(&wav_seeds[n_wav_seeds++], 8000,  1, 8);

    for (int i = 0; i < n_mp3_seeds; i++) any_seeds[n_any_seeds++] = mp3_seeds[i];
    for (int i = 0; i < n_wav_seeds; i++) any_seeds[n_any_seeds++] = wav_seeds[i];
}

/* ---- targets ------------------------------------------------------------ */

static int16_t pcm[4096 * 2];

static void t_sniff(const uint8_t *d, uint32_t n) {
    mp3_sniff(d, n);
}

/* Open and ask it everything a player asks before the first sample. */
static void t_open(const uint8_t *d, uint32_t n) {
    const char *err = 0;
    mp3_t *m = mp3_open(d, n, &err);
    if (!m) return;
    (void)mp3_describe(m);
    (void)mp3_rate(m);
    (void)mp3_channels(m);
    (void)mp3_bitrate(m);
    (void)mp3_duration_ms(m);
    (void)mp3_position_ms(m);
    (void)mp3_done(m);
    mp3_close(m);
}

static void t_decode(const uint8_t *d, uint32_t n) {
    const char *err = 0;
    mp3_t *m = mp3_open(d, n, &err);
    if (!m) return;
    for (int i = 0; i < 96; i++)
        if (!mp3_read(m, pcm, 1024)) break;
    mp3_close(m);
}

/* mp3_step is a different path through pull(): one frame of work per call, and
 * the budget arithmetic that bounds it is its own thing to get wrong. */
static void t_step(const uint8_t *d, uint32_t n) {
    const char *err = 0;
    mp3_t *m = mp3_open(d, n, &err);
    if (!m) return;
    int idle = 0;
    for (int i = 0; i < 512 && idle < 8; i++)
        if (!mp3_step(m, pcm, 512)) idle++;
    mp3_close(m);
}

/* Seeking is where a file gets to choose an offset directly: the Xing table of
 * contents is 100 bytes out of the stream, and every one of them is a position
 * to start decoding from. The targets are drawn from the input so a seed
 * reproduces the whole run. */
static void t_seek(const uint8_t *d, uint32_t n) {
    const char *err = 0;
    mp3_t *m = mp3_open(d, n, &err);
    if (!m) return;
    uint32_t dur = mp3_duration_ms(m);
    for (int k = 0; k < 8; k++) {
        uint32_t ms = ((uint32_t)d[(k * 37u) % n] << 8) | d[(k * 91u + 5u) % n];
        mp3_seek_ms(m, k & 1 ? ms : (dur ? ms % (dur + 1) : ms));
        for (int i = 0; i < 4; i++)
            if (!mp3_read(m, pcm, 1024)) break;
    }
    mp3_close(m);
}

static void t_wav(const uint8_t *d, uint32_t n) {
    wav_t w;
    const char *err = 0;
    if (!n || wav_open(d, n, &w, &err) != 0) return;
    for (int i = 0; i < 64; i++)
        if (!wav_read(&w, pcm, 1024)) break;
    (void)wav_duration_ms(&w);
    (void)wav_position_ms(&w);
    wav_seek_ms(&w, ((uint32_t)d[0] << 8) | d[n - 1]);
    wav_read(&w, pcm, 512);
    (void)wav_done(&w);
}

/* The whole shipping path a track takes: read through the VFS, sniffed, handed
 * to whichever decoder, played, sought, closed. snd_open() is the only code
 * here with an opinion -- which decoder gets the file -- and getting that wrong
 * is how a WAV plays as static. */
static void t_snd(const uint8_t *d, uint32_t n) {
    snd_t s;
    const char *err = 0;
    vfile = d;
    vfile_len = n;
    if (snd_open("fuzz.bin", &s, &err) != 0) return;
    (void)snd_describe(&s);
    (void)snd_duration_ms(&s);
    for (int i = 0; i < 48; i++)
        if (!snd_read(&s, pcm, 1024)) break;
    snd_seek_ms(&s, n ? (uint32_t)d[n / 2] * 41u : 0);
    for (int i = 0; i < 8; i++)
        if (!snd_step(&s, pcm, 512)) break;
    (void)snd_position_ms(&s);
    (void)snd_done(&s);
    snd_close(&s);
}

/* ---- selftest ----------------------------------------------------------- */

/* A CRC-protected VBR file, built here because nothing on this machine encodes
 * one. The protection bit costs two bytes between the frame header and the side
 * information, and the Xing tag moves along with them; the decoder used to look
 * at the unprotected offset, find nothing, and throw away both the frame count
 * and the seek table. Two bytes go in after the header and two come off the
 * tail of the same frame -- which in a Xing frame is padding, the tag needing
 * about 190 of its 400-odd bytes -- so every later frame stays exactly where it
 * was and only the thing under test moves.
 *
 * Returns 0 if the file it built is indistinguishable, to the decoder, from the
 * one it started with. */
static int crc_xing_survives(void) {
    for (int i = 0; i < n_mp3_seeds; i++) {
        const uint8_t *d = mp3_seeds[i].d;
        uint32_t n = mp3_seeds[i].n;
        const char *err = "?";

        mp3_t *m = mp3_open(d, n, &err);
        if (!m) continue;
        if (!m->have_toc) { mp3_close(m); continue; }   /* not the VBR seed */
        uint32_t want_frames = m->nframes;
        uint8_t  want_toc[100];
        memcpy(want_toc, m->toc, sizeof want_toc);
        mp3_close(m);

        /* Where mp3_open would have found the first frame. */
        hdr_t h;
        uint32_t at = id3v2_len(d, n), found = 0;
        for (; at + 4 <= n; at++)
            if (parse_header(d + at, n - at, &h) == 0) { found = 1; break; }
        if (!found || h.crc || at + h.bytes > n || h.bytes < 8) return -1;

        uint8_t *copy = (uint8_t *)malloc(n);
        memcpy(copy, d, n);
        memmove(copy + at + 6, copy + at + 4, h.bytes - 6);
        copy[at + 4] = copy[at + 5] = 0;        /* the CRC value is never checked */
        copy[at + 1] &= (uint8_t)~1u;           /* protection bit clear = CRC present */

        mp3_t *m2 = mp3_open(copy, n, &err);
        int ok = m2 && m2->have_toc && m2->nframes == want_frames &&
                 memcmp(m2->toc, want_toc, sizeof want_toc) == 0;
        if (m2) mp3_close(m2);
        free(copy);
        return ok ? 0 : -1;
    }
    return -1;                                  /* no seed with a TOC: say so */
}

/* Without this a harness that reaches nothing reports a clean run at a million
 * inputs a second, which is what a harness that finds nothing also reports.
 * Every check here is "this input produces that answer", and the seeds being
 * absent is a failure rather than a quiet skip -- a fuzzer with no corpus is a
 * random byte generator, and none of these parsers get past their first length
 * check on random bytes. */
int fz_selftest(void) {
    fz_check_failures = 0;
    build_tables();

    FZ_CHECK(n_mp3_seeds == 6);          /* test/seeds/mp3 is the corpus */
    FZ_CHECK(n_wav_seeds == 2);

    /* Every codeword back through the tree it was built from. test/host/mp3.c
     * does this too; it costs a millisecond and it is the one check that says
     * the tables in this binary are the tables the decoder will use. */
    int symbols = 0, bad = 0;
    for (int t = 0; t < 34 && !bad; t++) {
        if (!htab[t].n) continue;
        for (int s = 0; s < htab[t].n; s++) {
            uint8_t bytes[4] = { 0, 0, 0, 0 };
            int len = htab[t].len[s];
            uint32_t code = htab[t].cod[s];
            for (int b = 0; b < len; b++)
                if ((code >> (len - 1 - b)) & 1) bytes[b >> 3] |= (uint8_t)(0x80u >> (b & 7));
            br_t br;
            br_init(&br, bytes, sizeof bytes);
            if (huff_sym(&br, root[t]) != s || (int)br.bit != len) { bad = 1; break; }
            symbols++;
        }
    }
    FZ_CHECK(!bad && symbols > 1000);

    if (n_mp3_seeds) {
        const fz_seed *sd = &mp3_seeds[0];
        FZ_CHECK(mp3_sniff(sd->d, sd->n) == 1);
        const char *err = "?";
        mp3_t *m = mp3_open(sd->d, sd->n, &err);
        FZ_CHECK(m != NULL);
        if (m) {
            FZ_CHECK(mp3_rate(m) == 44100);
            FZ_CHECK(mp3_channels(m) >= 1 && mp3_channels(m) <= 2);
            uint32_t got = mp3_read(m, pcm, 2048);
            FZ_CHECK(got == 2048);
            int loud = 0;
            for (uint32_t i = 0; i < got * 2; i++) if (pcm[i]) loud = 1;
            FZ_CHECK(loud);              /* silence means the decode never happened */
            mp3_close(m);
        }
        FZ_CHECK(crc_xing_survives() == 0);
    }

    if (n_wav_seeds) {
        wav_t w;
        const char *err = "?";
        FZ_CHECK(wav_open(wav_seeds[0].d, wav_seeds[0].n, &w, &err) == 0);
        FZ_CHECK(w.rate == 44100 && w.channels == 2 && w.bits == 16);
        FZ_CHECK(wav_read(&w, pcm, 256) == 256);
        FZ_CHECK(mp3_sniff(wav_seeds[0].d, wav_seeds[0].n) == 0);

        /* And that the dispatch sends each file to the decoder that fits: this
         * is the check that would have caught two of five real WAVs opening as
         * MP3s, which is a bug this project has actually had. */
        snd_t s;
        vfile = wav_seeds[0].d; vfile_len = wav_seeds[0].n;
        FZ_CHECK(snd_open("x.wav", &s, &err) == 0);
        FZ_CHECK(s.kind == SND_WAV);
        snd_close(&s);
        if (n_mp3_seeds) {
            vfile = mp3_seeds[0].d; vfile_len = mp3_seeds[0].n;
            FZ_CHECK(snd_open("x.mp3", &s, &err) == 0);
            FZ_CHECK(s.kind == SND_MP3);
            snd_close(&s);
        }
    }

    return fz_check_failures;
}

/* ---- the table --------------------------------------------------------- */

/* ncorpus is filled in by main(); the seeds are read off the disk, and a
 * static initialiser cannot know how many of them turned up. */
static fz_target targets[] = {
    { "mp3sniff",  t_sniff,  mp3_seeds, 0, 1 },
    { "mp3open",   t_open,   mp3_seeds, 0, 1 },
    { "mp3decode", t_decode, mp3_seeds, 0, 1 },
    { "mp3step",   t_step,   mp3_seeds, 0, 1 },
    { "mp3seek",   t_seek,   mp3_seeds, 0, 1 },
    { "wav",       t_wav,    wav_seeds, 0, 1 },
    { "snd",       t_snd,    any_seeds, 0, 1 },
};

int main(int argc, char **argv) {
    load_corpus();
    for (unsigned i = 0; i < sizeof targets / sizeof *targets; i++) {
        if (targets[i].corpus == mp3_seeds) targets[i].ncorpus = n_mp3_seeds;
        else if (targets[i].corpus == wav_seeds) targets[i].ncorpus = n_wav_seeds;
        else targets[i].ncorpus = n_any_seeds;
    }
    return fz_main(argc, argv, targets, (int)(sizeof targets / sizeof *targets));
}
