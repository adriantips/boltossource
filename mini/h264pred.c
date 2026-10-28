/* ===========================================================================
 *  BoltOS Mini  -  mini/h264pred.c
 *
 *  Prediction and the inverse transform: intra 4x4, 16x16 and chroma
 *  (8.3), the 4x4 inverse transform (8.5.12), and inter prediction --
 *  quarter-sample luma with the six-tap filter, eighth-sample chroma,
 *  default, explicit and implicit weighting (8.4.2).
 * ===========================================================================*/
#include <stdint.h>
#include "h264int.h"
#include "string.h"

void h264_mc_init(void) { }

/* ---- the inverse transform --------------------------------------------------------------- */
void h264_idct4_add(uint8_t *dst, int stride, int16_t *c) {
    int t[16];
    for (int i = 0; i < 4; i++) {
        const int16_t *r = c + i * 4;
        int e0 = r[0] + r[2], e1 = r[0] - r[2], e2 = (r[1] >> 1) - r[3], e3 = r[1] + (r[3] >> 1);
        t[i * 4] = e0 + e3; t[i * 4 + 1] = e1 + e2; t[i * 4 + 2] = e1 - e2; t[i * 4 + 3] = e0 - e3;
    }
    for (int j = 0; j < 4; j++) {
        int e0 = t[j] + t[8 + j], e1 = t[j] - t[8 + j], e2 = (t[4 + j] >> 1) - t[12 + j], e3 = t[4 + j] + (t[12 + j] >> 1);
        int r[4] = { e0 + e3, e1 + e2, e1 - e2, e0 - e3 };
        for (int i = 0; i < 4; i++) dst[i * stride + j] = clip1(dst[i * stride + j] + ((r[i] + 32) >> 6));
    }
}

void h264_dc_add(uint8_t *dst, int stride, int dc) {
    int v = (dc + 32) >> 6;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) dst[y * stride + x] = clip1(dst[y * stride + x] + v);
}

/* ---- intra (8.3) ------------------------------------------------------------------------------- */
/* Is the MB holding (relative 4x4) position (x, y) usable for intra prediction? */
static int intra_ok(h264 *d, mbinfo *cur, int x, int y, int b) {
    int mx = d->mbx, my = d->mby;
    if (x >= 0 && x < 4 && y >= 0 && y < 4) return blk_at[y][x] < b;
    if (x > 3 && y >= 0) return 0;
    if (x < 0) mx--; else if (x > 3) mx++;
    if (y < 0) my--;
    if (!h264_avail(d, mx, my)) return 0;
    mbinfo *n = &d->cur->mb[my * d->mb_w + mx];
    (void)cur;
    return !(d->pp->constrained_intra && !n->intra);
}

void h264_intra4x4(h264 *d, mbinfo *m, int b, int mode, uint8_t *dst, int s) {
    int bx = blk_x[b], by = blk_y[b];
    int ht = intra_ok(d, m, bx, by - 1, b), hl = intra_ok(d, m, bx - 1, by, b);
    int htl = intra_ok(d, m, bx - 1, by - 1, b), htr = intra_ok(d, m, bx + 1, by - 1, b);
    int T[8], L[4], Q = 0;
    if (ht) {
        for (int i = 0; i < 4; i++) T[i] = dst[-s + i];
        if (htr) for (int i = 4; i < 8; i++) T[i] = dst[-s + i];
        else for (int i = 4; i < 8; i++) T[i] = T[3];
    }
    if (hl) for (int i = 0; i < 4; i++) L[i] = dst[i * s - 1];
    if (htl) Q = dst[-s - 1];
#define P(x, y) dst[(y) * s + (x)]
#define TP(i) ((i) < 0 ? Q : T[i])
#define LP(i) ((i) < 0 ? Q : L[i])
    switch (mode) {
    case 0: for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) P(x, y) = (uint8_t)T[x]; break;
    case 1: for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) P(x, y) = (uint8_t)L[y]; break;
    case 2: {
        int v = 128;
        if (ht && hl) v = (T[0] + T[1] + T[2] + T[3] + L[0] + L[1] + L[2] + L[3] + 4) >> 3;
        else if (hl) v = (L[0] + L[1] + L[2] + L[3] + 2) >> 2;
        else if (ht) v = (T[0] + T[1] + T[2] + T[3] + 2) >> 2;
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) P(x, y) = (uint8_t)v;
        break;
    }
    case 3:
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
            P(x, y) = (uint8_t)(x == 3 && y == 3 ? (T[6] + 3 * T[7] + 2) >> 2 : (T[x + y] + 2 * T[x + y + 1] + T[x + y + 2] + 2) >> 2);
        break;
    case 4:
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            int v;
            if (x > y) v = (TP(x - y - 2) + 2 * TP(x - y - 1) + TP(x - y) + 2) >> 2;
            else if (x < y) v = (LP(y - x - 2) + 2 * LP(y - x - 1) + LP(y - x) + 2) >> 2;
            else v = (T[0] + 2 * Q + L[0] + 2) >> 2;
            P(x, y) = (uint8_t)v;
        }
        break;
    case 5:
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            int z = 2 * x - y, v;
            if (z >= 0 && !(z & 1)) v = (TP(x - (y >> 1) - 1) + TP(x - (y >> 1)) + 1) >> 1;
            else if (z > 0) v = (TP(x - (y >> 1) - 2) + 2 * TP(x - (y >> 1) - 1) + TP(x - (y >> 1)) + 2) >> 2;
            else if (z == -1) v = (L[0] + 2 * Q + T[0] + 2) >> 2;
            else v = (LP(y - 1) + 2 * LP(y - 2) + LP(y - 3) + 2) >> 2;
            P(x, y) = (uint8_t)v;
        }
        break;
    case 6:
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            int z = 2 * y - x, v;
            if (z >= 0 && !(z & 1)) v = (LP(y - (x >> 1) - 1) + LP(y - (x >> 1)) + 1) >> 1;
            else if (z > 0) v = (LP(y - (x >> 1) - 2) + 2 * LP(y - (x >> 1) - 1) + LP(y - (x >> 1)) + 2) >> 2;
            else if (z == -1) v = (L[0] + 2 * Q + T[0] + 2) >> 2;
            else v = (TP(x - 1) + 2 * TP(x - 2) + TP(x - 3) + 2) >> 2;
            P(x, y) = (uint8_t)v;
        }
        break;
    case 7:
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            int i = x + (y >> 1);
            P(x, y) = (uint8_t)(!(y & 1) ? (T[i] + T[i + 1] + 1) >> 1 : (T[i] + 2 * T[i + 1] + T[i + 2] + 2) >> 2);
        }
        break;
    case 8:
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            int z = x + 2 * y, i = y + (x >> 1), v;
            if (z > 5) v = L[3];
            else if (z == 5) v = (L[2] + 3 * L[3] + 2) >> 2;
            else if (!(z & 1)) v = (L[i] + L[i + 1] + 1) >> 1;
            else v = (L[i] + 2 * L[i + 1] + L[i + 2] + 2) >> 2;
            P(x, y) = (uint8_t)v;
        }
        break;
    }
#undef P
#undef TP
#undef LP
}

static int nb_intra(h264 *d, int dx, int dy) {
    int mx = d->mbx + dx, my = d->mby + dy;
    if (!h264_avail(d, mx, my)) return 0;
    return !(d->pp->constrained_intra && !d->cur->mb[my * d->mb_w + mx].intra);
}

void h264_intra16(h264 *d, mbinfo *m, int mode, uint8_t *dst, int s) {
    (void)m;
    int ht = nb_intra(d, 0, -1), hl = nb_intra(d, -1, 0), htl = nb_intra(d, -1, -1);
    switch (mode) {
    case 0: for (int y = 0; y < 16; y++) memcpy(dst + y * s, dst - s, 16); break;
    case 1: for (int y = 0; y < 16; y++) memset(dst + y * s, dst[y * s - 1], 16); break;
    case 2: {
        int sum = 0, v = 128;
        if (ht && hl) { for (int i = 0; i < 16; i++) sum += dst[-s + i] + dst[i * s - 1]; v = (sum + 16) >> 5; }
        else if (hl) { for (int i = 0; i < 16; i++) sum += dst[i * s - 1]; v = (sum + 8) >> 4; }
        else if (ht) { for (int i = 0; i < 16; i++) sum += dst[-s + i]; v = (sum + 8) >> 4; }
        for (int y = 0; y < 16; y++) memset(dst + y * s, v, 16);
        break;
    }
    case 3: {
        (void)htl;
        int H = 0, V = 0;
        for (int i = 0; i < 8; i++) {
            H += (i + 1) * (dst[-s + 8 + i] - dst[-s + 6 - i]);
            V += (i + 1) * (dst[(8 + i) * s - 1] - dst[(6 - i) * s - 1]);
        }
        int a = 16 * (dst[15 * s - 1] + dst[-s + 15]), b = (5 * H + 32) >> 6, c = (5 * V + 32) >> 6;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) dst[y * s + x] = clip1((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
        break;
    }
    }
}

static void chroma_pred(h264 *d, int mode, uint8_t *dst, int s) {
    int ht = nb_intra(d, 0, -1), hl = nb_intra(d, -1, 0);
    switch (mode) {
    case 0:
        for (int b = 0; b < 4; b++) {
            int xo = (b & 1) * 4, yo = (b >> 1) * 4, st = 0, sl = 0, v = 128;
            if (ht) for (int i = 0; i < 4; i++) st += dst[-s + xo + i];
            if (hl) for (int i = 0; i < 4; i++) sl += dst[(yo + i) * s - 1];
            if (b == 0 || b == 3) { if (ht && hl) v = (st + sl + 4) >> 3; else if (hl) v = (sl + 2) >> 2; else if (ht) v = (st + 2) >> 2; }
            else if (b == 1) { if (ht) v = (st + 2) >> 2; else if (hl) v = (sl + 2) >> 2; }
            else { if (hl) v = (sl + 2) >> 2; else if (ht) v = (st + 2) >> 2; }
            for (int y = 0; y < 4; y++) memset(dst + (yo + y) * s + xo, v, 4);
        }
        break;
    case 1: for (int y = 0; y < 8; y++) memset(dst + y * s, dst[y * s - 1], 8); break;
    case 2: for (int y = 0; y < 8; y++) memcpy(dst + y * s, dst - s, 8); break;
    case 3: {
        int H = 0, V = 0;
        for (int i = 0; i < 4; i++) {
            H += (i + 1) * (dst[-s + 4 + i] - dst[-s + 2 - i]);
            V += (i + 1) * (dst[(4 + i) * s - 1] - dst[(2 - i) * s - 1]);
        }
        int a = 16 * (dst[7 * s - 1] + dst[-s + 7]), b = (34 * H + 32) >> 6, c = (34 * V + 32) >> 6;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) dst[y * s + x] = clip1((a + b * (x - 3) + c * (y - 3) + 16) >> 5);
        break;
    }
    }
}

void h264_intra_chroma(h264 *d, mbinfo *m, int mode, uint8_t *u, uint8_t *v, int s) {
    (void)m;
    chroma_pred(d, mode, u, s);
    chroma_pred(d, mode, v, s);
}

/* ---- inter (8.4.2) ------------------------------------------------------------------------------ */
/* The (w+5) x (h+5) window around (x, y), edges repeated (8.4.2.2.1's clipping). */
static void fetch(const uint8_t *pl, int stride, int pw, int ph, int x, int y, int w, int h, uint8_t *out, int os) {
    if (x >= 0 && y >= 0 && x + w <= pw && y + h <= ph) {
        for (int j = 0; j < h; j++) memcpy(out + j * os, pl + (y + j) * stride + x, (size_t)w);
        return;
    }
    for (int j = 0; j < h; j++) {
        const uint8_t *row = pl + clip3(0, ph - 1, y + j) * stride;
        for (int i = 0; i < w; i++) out[j * os + i] = row[clip3(0, pw - 1, x + i)];
    }
}

#define TAP(a, b, c, d, e, f) ((a) - 5 * (b) + 20 * (c) + 20 * (d) - 5 * (e) + (f))

/* Luma at quarter-sample position (qx, qy) (picture coordinates, 1/4 units), w x h into out. */
static void luma_mc(const picture *ref, int pw, int ph, int qx, int qy, int w, int h, uint8_t *out) {
    uint8_t win[21 * 21];
    int fx = qx & 3, fy = qy & 3, x = qx >> 2, y = qy >> 2;
    fetch(ref->y, ref->ystride, pw, ph, x - 2, y - 2, w + 5, h + 5, win, 21);
    const uint8_t *W = win + 2 * 21 + 2;                        /* W[j*21+i] = sample (x+i, y+j) */
    if (!fx && !fy) { for (int j = 0; j < h; j++) memcpy(out + j * 16, W + j * 21, (size_t)w); return; }
    uint8_t hb[2][16 * 16], vb[2][16 * 16], jb[16 * 16];
    /* which half-sample planes this position averages (bit 0/1 horizontal halves on rows
     * y, y+1; 2/3 vertical halves on columns x, x+1; 4 the centre) */
    static const uint8_t needs[16] = { 0, 1, 1, 1, 4, 5, 17, 9, 4, 20, 16, 24, 4, 6, 18, 10 };
    int nd = needs[fy * 4 + fx];
    int need_h0 = nd & 1, need_h1 = nd >> 1 & 1, need_v0 = nd >> 2 & 1, need_v1 = nd >> 3 & 1, need_j = nd >> 4 & 1;
    for (int r = 0; r < 2; r++) {
        if (r == 0 ? !need_h0 : !need_h1) continue;
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
            const uint8_t *p = W + (j + r) * 21 + i;
            hb[r][j * 16 + i] = clip1((TAP(p[-2], p[-1], p[0], p[1], p[2], p[3]) + 16) >> 5);
        }
    }
    for (int c = 0; c < 2; c++) {
        if (c == 0 ? !need_v0 : !need_v1) continue;
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
            const uint8_t *p = W + j * 21 + i + c;
            vb[c][j * 16 + i] = clip1((TAP(p[-42], p[-21], p[0], p[21], p[42], p[63]) + 16) >> 5);
        }
    }
    if (need_j) {
        int mid[21 * 16];                                          /* unrounded horizontal halves, rows -2..h+2 */
        for (int j = -2; j < h + 3; j++) for (int i = 0; i < w; i++) {
            const uint8_t *p = W + j * 21 + i;
            mid[(j + 2) * 16 + i] = TAP(p[-2], p[-1], p[0], p[1], p[2], p[3]);
        }
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
            const int *q = mid + (j + 2) * 16 + i;
            jb[j * 16 + i] = clip1((TAP(q[-32], q[-16], q[0], q[16], q[32], q[48]) + 512) >> 10);
        }
    }
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        int G = W[j * 21 + i], o = j * 16 + i, v;
        switch (fy * 4 + fx) {
        case 1:  v = (G + hb[0][o] + 1) >> 1; break;                    /* a */
        case 2:  v = hb[0][o]; break;                                    /* b */
        case 3:  v = (W[j * 21 + i + 1] + hb[0][o] + 1) >> 1; break;     /* c */
        case 4:  v = (G + vb[0][o] + 1) >> 1; break;                     /* d */
        case 5:  v = (hb[0][o] + vb[0][o] + 1) >> 1; break;              /* e */
        case 6:  v = (hb[0][o] + jb[o] + 1) >> 1; break;                 /* f */
        case 7:  v = (hb[0][o] + vb[1][o] + 1) >> 1; break;              /* g */
        case 8:  v = vb[0][o]; break;                                    /* h */
        case 9:  v = (vb[0][o] + jb[o] + 1) >> 1; break;                 /* i */
        case 10: v = jb[o]; break;                                       /* j */
        case 11: v = (jb[o] + vb[1][o] + 1) >> 1; break;                 /* k */
        case 12: v = (W[(j + 1) * 21 + i] + vb[0][o] + 1) >> 1; break;   /* n */
        case 13: v = (vb[0][o] + hb[1][o] + 1) >> 1; break;              /* p */
        case 14: v = (jb[o] + hb[1][o] + 1) >> 1; break;                 /* q */
        default: v = (vb[1][o] + hb[1][o] + 1) >> 1; break;              /* r */
        }
        out[o] = (uint8_t)v;
    }
}

static void chroma_mc(const uint8_t *pl, int stride, int pw, int ph, int ex, int ey, int w, int h, uint8_t *out) {
    uint8_t win[9 * 9];
    int fx = ex & 7, fy = ey & 7, x = ex >> 3, y = ey >> 3;
    fetch(pl, stride, pw, ph, x, y, w + 1, h + 1, win, 9);
    int a = (8 - fx) * (8 - fy), b = fx * (8 - fy), c = (8 - fx) * fy, dd = fx * fy;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        const uint8_t *p = win + j * 9 + i;
        out[j * 8 + i] = (uint8_t)((a * p[0] + b * p[1] + c * p[9] + dd * p[10] + 32) >> 6);
    }
}

/* One block of w x h luma (4x4 units at bx, by): both lists, weighted, written in. */
static void predict(h264 *d, mbinfo *m, int bx, int by, int w4, int h4) {
    slice_t *sl = &d->sl;
    picture *c = d->cur;
    int b = blk_at[by][bx], b8 = b >> 2;
    int r[2] = { m->ref[0][b8], m->ref[1][b8] };
    int w = w4 * 4, h = h4 * 4, px = d->mbx * 16 + bx * 4, py = d->mby * 16 + by * 4;
    uint8_t L[2][16 * 16], U[2][8 * 8], V[2][8 * 8];
    for (int l = 0; l < 2; l++) {
        if (r[l] < 0) continue;
        const picture *ref = sl->list[l][r[l]];
        int mvx = m->mv[l][b][0], mvy = m->mv[l][b][1];
        luma_mc(ref, d->w, d->h, px * 4 + mvx, py * 4 + mvy, w, h, L[l]);
        int cx = px * 4 + mvx, cy = py * 4 + mvy;                 /* chroma: same numbers, eighths of a chroma sample */
        chroma_mc(ref->u, ref->cstride, d->w / 2, d->h / 2, cx, cy, w / 2, h / 2, U[l]);
        chroma_mc(ref->v, ref->cstride, d->w / 2, d->h / 2, cx, cy, w / 2, h / 2, V[l]);
    }
    uint8_t *dy = c->y + py * c->ystride + px;
    uint8_t *du = c->u + (py / 2) * c->cstride + px / 2, *dv = c->v + (py / 2) * c->cstride + px / 2;
    int explicit_w = (d->pp->weighted_pred && sl->type == 0) || (d->pp->weighted_bipred == 1 && sl->type == 1);
    int implicit_w = d->pp->weighted_bipred == 2 && sl->type == 1 && r[0] >= 0 && r[1] >= 0;
    if (r[0] >= 0 && r[1] >= 0) {
        int lw0, lw1, lo, cw0[2], cw1[2], co[2], ld, cd;
        if (implicit_w) {
            lw1 = sl->implicit_w[r[0]][r[1]]; lw0 = 64 - lw1; lo = 0; ld = 5;
            cw0[0] = cw0[1] = lw0; cw1[0] = cw1[1] = lw1; co[0] = co[1] = 0; cd = 5;
        } else if (explicit_w) {
            lw0 = sl->lw[0][r[0]]; lw1 = sl->lw[1][r[1]]; lo = (sl->lo[0][r[0]] + sl->lo[1][r[1]] + 1) >> 1; ld = sl->luma_log2_wd;
            for (int k = 0; k < 2; k++) { cw0[k] = sl->cw[0][r[0]][k]; cw1[k] = sl->cw[1][r[1]][k]; co[k] = (sl->co[0][r[0]][k] + sl->co[1][r[1]][k] + 1) >> 1; }
            cd = sl->chroma_log2_wd;
        } else {
            for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) dy[j * c->ystride + i] = (uint8_t)((L[0][j * 16 + i] + L[1][j * 16 + i] + 1) >> 1);
            for (int j = 0; j < h / 2; j++) for (int i = 0; i < w / 2; i++) {
                du[j * c->cstride + i] = (uint8_t)((U[0][j * 8 + i] + U[1][j * 8 + i] + 1) >> 1);
                dv[j * c->cstride + i] = (uint8_t)((V[0][j * 8 + i] + V[1][j * 8 + i] + 1) >> 1);
            }
            return;
        }
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++)
            dy[j * c->ystride + i] = clip1(((L[0][j * 16 + i] * lw0 + L[1][j * 16 + i] * lw1 + (1 << ld)) >> (ld + 1)) + lo);
        for (int j = 0; j < h / 2; j++) for (int i = 0; i < w / 2; i++) {
            du[j * c->cstride + i] = clip1(((U[0][j * 8 + i] * cw0[0] + U[1][j * 8 + i] * cw1[0] + (1 << cd)) >> (cd + 1)) + co[0]);
            dv[j * c->cstride + i] = clip1(((V[0][j * 8 + i] * cw0[1] + V[1][j * 8 + i] * cw1[1] + (1 << cd)) >> (cd + 1)) + co[1]);
        }
        return;
    }
    int l = r[0] >= 0 ? 0 : 1;
    if (explicit_w) {
        int lw = sl->lw[l][r[l]], lo = sl->lo[l][r[l]], ld = sl->luma_log2_wd, cd = sl->chroma_log2_wd;
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
            int p = L[l][j * 16 + i];
            dy[j * c->ystride + i] = clip1(ld >= 1 ? ((p * lw + (1 << (ld - 1))) >> ld) + lo : p * lw + lo);
        }
        for (int k = 0; k < 2; k++) {
            int cw = sl->cw[l][r[l]][k], co = sl->co[l][r[l]][k];
            uint8_t *dst = k ? dv : du, *src = k ? V[l] : U[l];
            for (int j = 0; j < h / 2; j++) for (int i = 0; i < w / 2; i++) {
                int p = src[j * 8 + i];
                dst[j * c->cstride + i] = clip1(cd >= 1 ? ((p * cw + (1 << (cd - 1))) >> cd) + co : p * cw + co);
            }
        }
        return;
    }
    for (int j = 0; j < h; j++) memcpy(dy + j * c->ystride, L[l] + j * 16, (size_t)w);
    for (int j = 0; j < h / 2; j++) { memcpy(du + j * c->cstride, U[l] + j * 8, (size_t)(w / 2)); memcpy(dv + j * c->cstride, V[l] + j * 8, (size_t)(w / 2)); }
}

static int same_motion(mbinfo *m, int a, int b) {
    for (int l = 0; l < 2; l++) {
        if (m->ref[l][a >> 2] != m->ref[l][b >> 2]) return 0;
        if (m->ref[l][a >> 2] >= 0 && (m->mv[l][a][0] != m->mv[l][b][0] || m->mv[l][a][1] != m->mv[l][b][1])) return 0;
    }
    return 1;
}

void h264_inter(h264 *d, mbinfo *m) {
    int all = 1;
    for (int b = 1; b < 16 && all; b++) all = same_motion(m, 0, b);
    if (all) { predict(d, m, 0, 0, 4, 4); return; }
    for (int k = 0; k < 4; k++) {
        int b0 = k * 4, x = (k & 1) * 2, y = (k >> 1) * 2;
        if (same_motion(m, b0, b0 + 1) && same_motion(m, b0, b0 + 2) && same_motion(m, b0, b0 + 3)) predict(d, m, x, y, 2, 2);
        else for (int s = 0; s < 4; s++) predict(d, m, blk_x[b0 + s], blk_y[b0 + s], 1, 1);
    }
}
