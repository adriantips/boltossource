/* ===========================================================================
 *  BoltOS  -  kernel/png_enc.c
 *  Writing a PNG: row filters, LZ77, fixed-Huffman deflate, zlib, chunks.
 *
 *  The decoder in kernel/image.c reads five formats. This writes one, properly:
 *  a screenshot has to be a file somebody can open on another machine, and an
 *  uncompressed dump would be 2.3 MB a frame on a 64 MiB filesystem.
 *
 *  Fixed Huffman rather than dynamic. The gap between them on a desktop
 *  screenshot -- flat panels, repeated glyphs, long runs -- is small next to
 *  what LZ77 already takes out, and a dynamic block means building and
 *  emitting two code trees for a few per cent. Fixed codes are a table.
 * ========================================================================= */
#include "png_enc.h"
#include "kheap.h"
#include "string.h"

/* ------------------------------------------------------------ checksums --- */
static uint32_t crc_table[256];
static int      crc_ready;

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
        crc_table[n] = c;
    }
    crc_ready = 1;
}

uint32_t png_crc32(uint32_t crc, const uint8_t *d, uint32_t n) {
    if (!crc_ready) crc_init();
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) c = crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t png_adler32(const uint8_t *d, uint32_t n) {
    uint32_t a = 1, b = 0;
    /* 5552 is the most bytes that can be summed before b can overflow 32 bits */
    while (n) {
        uint32_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { a += *d++; b += a; }
        a %= 65521; b %= 65521;
    }
    return (b << 16) | a;
}

/* ---------------------------------------------------------- bit output ---- */
/* Deflate packs its bit stream LSB-first within each byte, except that Huffman
 * codes are written starting from their most significant bit. Getting those
 * two conventions the wrong way round produces a file that looks plausible and
 * decodes to nothing, so they are separate functions here. */
typedef struct {
    uint8_t *buf;
    uint32_t cap, len;
    uint32_t bit;         /* bit accumulator                     */
    int      nbits;
    int      oom;
} bitout;

static int bo_reserve(bitout *b, uint32_t extra) {
    if (b->len + extra <= b->cap) return 0;
    uint32_t cap = b->cap ? b->cap * 2 : 8192;
    while (cap < b->len + extra) cap *= 2;
    uint8_t *n = (uint8_t *)kmalloc(cap);
    if (!n) { b->oom = 1; return -1; }
    if (b->buf) { memcpy(n, b->buf, b->len); kfree(b->buf); }
    b->buf = n; b->cap = cap;
    return 0;
}

static void bo_byte(bitout *b, uint8_t v) {
    if (b->oom || bo_reserve(b, 1)) return;
    b->buf[b->len++] = v;
}

static void bo_bits(bitout *b, uint32_t value, int n) {      /* LSB-first */
    b->bit |= (value & ((1u << n) - 1)) << b->nbits;
    b->nbits += n;
    while (b->nbits >= 8) {
        bo_byte(b, (uint8_t)(b->bit & 0xFF));
        b->bit >>= 8;
        b->nbits -= 8;
    }
}

static void bo_code(bitout *b, uint32_t code, int n) {       /* MSB-first */
    for (int i = n - 1; i >= 0; i--) bo_bits(b, (code >> i) & 1, 1);
}

static void bo_flush(bitout *b) {
    if (b->nbits) { bo_byte(b, (uint8_t)(b->bit & 0xFF)); b->bit = 0; b->nbits = 0; }
}

/* --------------------------------------------------- fixed Huffman codes -- */
/* RFC 1951 3.2.6. Literals 0-143 are 8 bits from 0x30, 144-255 are 9 from
 * 0x190, end-of-block and the short lengths are 7 bits from 0, and 280-287 are
 * 8 bits from 0xC0. */
static void emit_litlen(bitout *b, int sym) {
    if (sym < 144)      bo_code(b, 0x30 + sym, 8);
    else if (sym < 256) bo_code(b, 0x190 + sym - 144, 9);
    else if (sym < 280) bo_code(b, sym - 256, 7);
    else                bo_code(b, 0xC0 + sym - 280, 8);
}

static const uint16_t LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t  DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static void emit_match(bitout *b, int len, int dist) {
    int lc = 28;
    while (lc > 0 && len < LEN_BASE[lc]) lc--;
    emit_litlen(b, 257 + lc);
    if (LEN_EXTRA[lc]) bo_bits(b, (uint32_t)(len - LEN_BASE[lc]), LEN_EXTRA[lc]);

    int dc = 29;
    while (dc > 0 && dist < DIST_BASE[dc]) dc--;
    bo_code(b, (uint32_t)dc, 5);                       /* fixed 5-bit distance */
    if (DIST_EXTRA[dc]) bo_bits(b, (uint32_t)(dist - DIST_BASE[dc]), DIST_EXTRA[dc]);
}

/* ----------------------------------------------------------------- LZ77 --- */
#define WBITS      15
#define WSIZE      (1 << WBITS)          /* 32 KiB, the deflate maximum       */
#define HBITS      15
#define HSIZE      (1 << HBITS)
#define MIN_MATCH  3
#define MAX_MATCH  258
#define MAX_CHAIN  32                    /* how hard to look before settling  */
#define GOOD_MATCH 96                    /* long enough; stop looking          */

typedef struct { int32_t *head, *prev; } lz_t;

static uint32_t hash3(const uint8_t *p) {
    return (((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ (uint32_t)p[2])
           & (HSIZE - 1);
}

/* Longest match for `src[at..]` within the window. Returns its length (>=
 * MIN_MATCH) and sets *dist, or 0 if there is nothing worth encoding. */
static int lz_match(lz_t *z, const uint8_t *src, uint32_t at, uint32_t n, int *dist) {
    if (at + MIN_MATCH > n) return 0;
    uint32_t limit = at > WSIZE ? at - WSIZE : 0;
    uint32_t maxlen = n - at; if (maxlen > MAX_MATCH) maxlen = MAX_MATCH;

    int32_t cur = z->head[hash3(src + at)];
    int best = 0, bestd = 0, chain = MAX_CHAIN;
    while (cur >= 0 && (uint32_t)cur >= limit && chain--) {
        const uint8_t *a = src + cur, *b = src + at;
        uint32_t l = 0;
        while (l < maxlen && a[l] == b[l]) l++;
        if ((int)l > best) {
            best = (int)l; bestd = (int)(at - (uint32_t)cur);
            /* A screenshot is mostly long flat runs. Searching the rest of the
             * chain for a slightly better one costs more than it saves, and
             * this loop runs once per byte of a 2 MB image. */
            if (l >= maxlen || best >= GOOD_MATCH) break;
        }
        cur = z->prev[(uint32_t)cur & (WSIZE - 1)];
    }
    if (best < MIN_MATCH) return 0;
    *dist = bestd;
    return best;
}

static void lz_insert(lz_t *z, const uint8_t *src, uint32_t at) {
    uint32_t h = hash3(src + at);
    z->prev[at & (WSIZE - 1)] = z->head[h];
    z->head[h] = (int32_t)at;
}

/* One fixed-Huffman block over the whole input. */
static int deflate_fixed(bitout *b, const uint8_t *src, uint32_t n) {
    lz_t z;
    z.head = (int32_t *)kmalloc(sizeof(int32_t) * HSIZE);
    z.prev = (int32_t *)kmalloc(sizeof(int32_t) * WSIZE);
    if (!z.head || !z.prev) { kfree(z.head); kfree(z.prev); return -1; }
    for (int i = 0; i < HSIZE; i++) z.head[i] = -1;
    for (int i = 0; i < WSIZE; i++) z.prev[i] = -1;

    bo_bits(b, 1, 1);          /* BFINAL: one block for the whole image */
    bo_bits(b, 1, 2);          /* BTYPE 01: fixed Huffman               */

    uint32_t at = 0;
    while (at < n) {
        int dist = 0;
        int len = lz_match(&z, src, at, n, &dist);
        if (len >= MIN_MATCH) {
            emit_match(b, len, dist);
            /* Every position inside the match still has to enter the hash, or
             * later matches cannot reach back past it. */
            for (int i = 0; i < len; i++) {
                if (at + MIN_MATCH <= n) lz_insert(&z, src, at);
                at++;
            }
        } else {
            emit_litlen(b, src[at]);
            if (at + MIN_MATCH <= n) lz_insert(&z, src, at);
            at++;
        }
        if (b->oom) break;
    }
    emit_litlen(b, 256);       /* end of block */
    bo_flush(b);

    kfree(z.head); kfree(z.prev);
    return b->oom ? -1 : 0;
}

/* ------------------------------------------------------------ PNG rows ---- */
/* Filter 0 (None) or 1 (Sub), chosen per row by the minimum-sum-of-absolute-
 * differences heuristic every real encoder uses. Sub wins on gradients and
 * loses on flat runs, and a screenshot has both. */
static void filter_rows(const uint32_t *px, int w, int h, uint8_t *raw) {
    uint32_t stride = (uint32_t)w * 3;
    for (int y = 0; y < h; y++) {
        uint8_t *dst = raw + (uint64_t)y * (stride + 1);
        const uint32_t *s = px + (uint64_t)y * w;

        /* the unfiltered row, written past its own filter byte */
        uint8_t *cur = dst + 1;
        for (int x = 0; x < w; x++) {
            uint32_t p = s[x];
            cur[x * 3 + 0] = (uint8_t)(p >> 16);
            cur[x * 3 + 1] = (uint8_t)(p >> 8);
            cur[x * 3 + 2] = (uint8_t)p;
        }

        uint32_t sum_none = 0, sum_sub = 0;
        for (uint32_t i = 0; i < stride; i++) {
            int8_t v = (int8_t)cur[i];
            sum_none += (uint32_t)(v < 0 ? -v : v);
            int8_t d = (int8_t)(cur[i] - (i >= 3 ? cur[i - 3] : 0));
            sum_sub += (uint32_t)(d < 0 ? -d : d);
        }
        if (sum_sub < sum_none) {
            /* backwards, so each byte is subtracted from the original left */
            for (uint32_t i = stride; i-- > 3; ) cur[i] = (uint8_t)(cur[i] - cur[i - 3]);
            dst[0] = 1;
        } else {
            dst[0] = 0;
        }
    }
}

/* ------------------------------------------------------------- chunks ----- */
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void chunk(bitout *o, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t hdr[8];
    put_be32(hdr, n);
    hdr[4] = (uint8_t)type[0]; hdr[5] = (uint8_t)type[1];
    hdr[6] = (uint8_t)type[2]; hdr[7] = (uint8_t)type[3];
    for (int i = 0; i < 8; i++) bo_byte(o, hdr[i]);
    for (uint32_t i = 0; i < n; i++) bo_byte(o, data[i]);

    uint32_t crc = png_crc32(0, hdr + 4, 4);
    crc = png_crc32(crc, data, n);
    uint8_t c[4]; put_be32(c, crc);
    for (int i = 0; i < 4; i++) bo_byte(o, c[i]);
}

int png_encode(const uint32_t *px, int w, int h, uint8_t **out, uint32_t *outlen) {
    if (!px || !out || !outlen || w <= 0 || h <= 0) return -1;
    /* The only real limit is that the filtered stream is addressed with 32-bit
     * lengths. Compute it in 64 and refuse what will not fit, rather than
     * picking a round number and rejecting legitimate images -- a panoramic
     * screenshot is wider than any dimension cap worth writing down. */
    if (w > 65535 || h > 65535) return -1;
    uint64_t need = ((uint64_t)w * 3 + 1) * (uint64_t)h;
    if (need > 512u * 1024 * 1024) return -1;
    *out = 0; *outlen = 0;

    uint32_t rawlen = (uint32_t)need;
    uint8_t *raw = (uint8_t *)kmalloc(rawlen);
    if (!raw) return -1;
    filter_rows(px, w, h, raw);

    /* zlib stream: header, one deflate block, adler32 of the *unfiltered-
     * stream* bytes (which for PNG means the filtered rows -- the adler covers
     * exactly what deflate compressed). */
    bitout z = { 0, 0, 0, 0, 0, 0 };
    bo_byte(&z, 0x78);                       /* CM=8, CINFO=7 (32 KiB window) */
    bo_byte(&z, 0x01);                       /* 0x7801 % 31 == 0, no dictionary */
    if (deflate_fixed(&z, raw, rawlen) != 0) { kfree(raw); kfree(z.buf); return -1; }
    uint8_t ad[4]; put_be32(ad, png_adler32(raw, rawlen));
    for (int i = 0; i < 4; i++) bo_byte(&z, ad[i]);
    kfree(raw);
    if (z.oom) { kfree(z.buf); return -1; }

    bitout o = { 0, 0, 0, 0, 0, 0 };
    static const uint8_t SIG[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    for (int i = 0; i < 8; i++) bo_byte(&o, SIG[i]);

    uint8_t ihdr[13];
    put_be32(ihdr + 0, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8]  = 8;      /* bit depth                */
    ihdr[9]  = 2;      /* colour type 2: truecolour */
    ihdr[10] = 0;      /* deflate                  */
    ihdr[11] = 0;      /* adaptive filtering       */
    ihdr[12] = 0;      /* no interlace             */
    chunk(&o, "IHDR", ihdr, sizeof ihdr);
    chunk(&o, "IDAT", z.buf, z.len);
    chunk(&o, "IEND", 0, 0);
    kfree(z.buf);

    if (o.oom) { kfree(o.buf); return -1; }
    *out = o.buf; *outlen = o.len;
    return 0;
}
