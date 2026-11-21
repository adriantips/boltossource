/* ===========================================================================
 *  BoltOS Mini  -  mini/mp4.c
 *
 *  ISO BMFF (MP4) sample tables: for each track, stsd for what it is, and
 *  stts, ctts, stss, stsc, stsz and stco/co64 turned into one array of
 *  samples -- file offset, size, decode and presentation time, keyframe.
 * ===========================================================================*/
#include <stdint.h>
#include "mp4.h"
#include "kheap.h"
#include "string.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* Walk the boxes in [p, p + len): for each, its type, payload and payload size. */
typedef struct { const uint8_t *p, *end; } walk_t;
static int next_box(walk_t *w, char type[5], const uint8_t **body, uint64_t *blen) {
    if (w->end - w->p < 8) return 0;
    uint64_t size = be32(w->p);
    int hdr = 8;
    if (size == 1) { if (w->end - w->p < 16) return 0; size = be64(w->p + 8); hdr = 16; }
    else if (size == 0) size = (uint64_t)(w->end - w->p);
    if (size < (uint64_t)hdr || size > (uint64_t)(w->end - w->p)) return 0;
    memcpy(type, w->p + 4, 4); type[4] = 0;
    *body = w->p + hdr; *blen = size - (uint64_t)hdr;
    w->p += size;
    return 1;
}

static const uint8_t *child(const uint8_t *p, uint64_t len, const char *type, uint64_t *clen) {
    walk_t w = { p, p + len };
    char t[5]; const uint8_t *b; uint64_t bl;
    while (next_box(&w, t, &b, &bl)) if (!memcmp(t, type, 4)) { *clen = bl; return b; }
    return 0;
}

int mp4_find_box(const uint8_t *buf, uint64_t len, const char *type, uint64_t *off, uint64_t *size) {
    uint64_t o = 0;
    while (o + 8 <= len) {
        uint64_t s = be32(buf + o);
        if (s == 1 && o + 16 <= len) s = be64(buf + o + 8);
        if (!memcmp(buf + o + 4, type, 4)) { *off = o; *size = s; return 1; }
        if (s < 8) return 0;
        o += s;
    }
    return 0;
}

static int build(mp4_track *t, const uint8_t *stbl, uint64_t len) {
    uint64_t l_stts, l_stsc, l_stsz, l_stco, l_ctts = 0, l_stss = 0;
    const uint8_t *stts = child(stbl, len, "stts", &l_stts), *stsc = child(stbl, len, "stsc", &l_stsc);
    const uint8_t *stsz = child(stbl, len, "stsz", &l_stsz), *stco = child(stbl, len, "stco", &l_stco);
    const uint8_t *ctts = child(stbl, len, "ctts", &l_ctts), *stss = child(stbl, len, "stss", &l_stss);
    int co64 = 0;
    if (!stco) { stco = child(stbl, len, "co64", &l_stco); co64 = 1; }
    if (!stts || !stsc || !stsz || !stco || l_stsz < 12) return -1;
    uint32_t fixed = be32(stsz + 4), n = be32(stsz + 8);
    if (n == 0 || n > 4000000 || (!fixed && l_stsz < 12 + 4ull * n)) return -1;
    t->s = kmalloc((uint64_t)n * sizeof(mp4_sample));
    if (!t->s) return -1;
    memset(t->s, 0, (size_t)n * sizeof(mp4_sample));
    t->n = (int)n;
    for (uint32_t i = 0; i < n; i++) t->s[i].size = fixed ? fixed : be32(stsz + 12 + 4 * i);
    /* times */
    uint32_t ne = be32(stts + 4), k = 0;
    int64_t dts = 0;
    for (uint32_t e = 0; e < ne && 8 + 8ull * e + 8 <= l_stts; e++) {
        uint32_t cnt = be32(stts + 8 + 8 * e), delta = be32(stts + 12 + 8 * e);
        for (uint32_t i = 0; i < cnt && k < n; i++, k++) { t->s[k].dts = dts; dts += delta; }
    }
    for (; k < n; k++) t->s[k].dts = dts;
    for (uint32_t i = 0; i < n; i++) t->s[i].pts = t->s[i].dts;
    if (ctts) {
        uint32_t nc = be32(ctts + 4); k = 0;
        for (uint32_t e = 0; e < nc && 8 + 8ull * e + 8 <= l_ctts; e++) {
            uint32_t cnt = be32(ctts + 8 + 8 * e);
            int32_t off = (int32_t)be32(ctts + 12 + 8 * e);
            for (uint32_t i = 0; i < cnt && k < n; i++, k++) t->s[k].pts = t->s[k].dts + off;
        }
    }
    if (stss) {
        uint32_t ns = be32(stss + 4);
        for (uint32_t i = 0; i < ns && 8 + 4ull * i + 4 <= l_stss; i++) { uint32_t s = be32(stss + 8 + 4 * i); if (s >= 1 && s <= n) t->s[s - 1].key = 1; }
    } else for (uint32_t i = 0; i < n; i++) t->s[i].key = 1;
    /* places: chunks, and how many samples each holds */
    uint32_t nchunks = be32(stco + 4), nsc = be32(stsc + 4), si = 0;
    for (uint32_t c = 0; c < nchunks && si < n; c++) {
        uint64_t off = co64 ? be64(stco + 8 + 8ull * c) : be32(stco + 8 + 4ull * c);
        uint32_t per = 0;
        for (uint32_t e = 0; e < nsc && 8 + 12ull * e + 12 <= l_stsc; e++)
            if (be32(stsc + 8 + 12 * e) <= c + 1) per = be32(stsc + 12 + 12 * e);
        for (uint32_t i = 0; i < per && si < n; i++, si++) { t->s[si].off = off; off += t->s[si].size; }
    }
    return 0;
}

int mp4_parse(const uint8_t *moov, uint64_t len, mp4_t *m) {
    memset(m, 0, sizeof *m);
    if (len < 8 || memcmp(moov + 4, "moov", 4)) return -1;
    walk_t w = { moov + 8, moov + len };
    char t[5]; const uint8_t *trak; uint64_t tl;
    while (next_box(&w, t, &trak, &tl)) {
        if (memcmp(t, "trak", 4)) continue;
        uint64_t l_mdia, l_hdlr, l_mdhd, l_minf, l_stbl, l_stsd;
        const uint8_t *mdia = child(trak, tl, "mdia", &l_mdia);
        if (!mdia) continue;
        const uint8_t *hdlr = child(mdia, l_mdia, "hdlr", &l_hdlr), *mdhd = child(mdia, l_mdia, "mdhd", &l_mdhd);
        const uint8_t *minf = child(mdia, l_mdia, "minf", &l_minf);
        const uint8_t *stbl = minf ? child(minf, l_minf, "stbl", &l_stbl) : 0;
        const uint8_t *stsd = stbl ? child(stbl, l_stbl, "stsd", &l_stsd) : 0;
        if (!hdlr || !mdhd || !stsd || l_hdlr < 12 || l_stsd < 16) continue;
        int video = !memcmp(hdlr + 8, "vide", 4), audio = !memcmp(hdlr + 8, "soun", 4);
        mp4_track *tr = video ? &m->video : audio ? &m->audio : 0;
        if (!tr || tr->s) continue;
        int v1 = mdhd[0] == 1;
        tr->timescale = (int)be32(mdhd + (v1 ? 20 : 12));
        tr->duration = v1 ? (int64_t)be64(mdhd + 24) : be32(mdhd + 16);
        const uint8_t *e = stsd + 8;                               /* the first sample entry */
        uint32_t esz = be32(e);
        if (esz > l_stsd - 8) continue;
        if (video && !memcmp(e + 4, "avc1", 4) && esz >= 86) {
            m->w = be16(e + 32); m->h = be16(e + 34);
            uint64_t cl;
            const uint8_t *avcc = child(e + 86, esz - 86, "avcC", &cl);
            if (avcc && cl <= sizeof m->avcc) { memcpy(m->avcc, avcc, cl); m->avcc_len = (int)cl; }
        } else if (audio && !memcmp(e + 4, "mp4a", 4) && esz >= 36) {
            m->channels = be16(e + 24);
            m->rate = (int)(be32(e + 32) >> 16);
            uint64_t cl;
            const uint8_t *esds = child(e + 36, esz - 36, "esds", &cl);
            /* ES_Descriptor (3) > DecoderConfigDescriptor (4) > DecoderSpecificInfo (5) */
            for (uint64_t i = 4; esds && i + 2 < cl; i++)
                if (esds[i] == 5) {
                    uint64_t j = i + 1; uint32_t sz = 0;
                    for (int k = 0; k < 4 && j < cl; k++) { uint8_t b = esds[j++]; sz = sz << 7 | (b & 0x7F); if (!(b & 0x80)) break; }
                    if (sz && j + sz <= cl && sz <= sizeof m->asc) { memcpy(m->asc, esds + j, sz); m->asc_len = (int)sz; }
                    break;
                }
        } else continue;
        if (build(tr, stbl, l_stbl) < 0) return -1;
    }
    return (m->video.n && m->avcc_len) || (m->audio.n && m->asc_len) ? 0 : -1;
}

void mp4_free(mp4_t *m) {
    if (m->video.s) kfree(m->video.s);
    if (m->audio.s) kfree(m->audio.s);
    m->video.s = m->audio.s = 0;
}
