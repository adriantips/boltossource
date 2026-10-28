/* ===========================================================================
 *  BoltOS Mini  -  mini/h264dbk.c
 *
 *  The loop filter (8.7), run over the whole picture once its slices are
 *  decoded: per macroblock, the vertical edges left to right, then the
 *  horizontal ones top to bottom, each 4-sample segment as strong as its
 *  boundary strength says.
 * ===========================================================================*/
#include <stdint.h>
#include "h264int.h"

static const uint8_t alpha_t[52] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,5,6,7,8,9,10,12,13,15,17,20,22,25,28,
                                     32,36,40,45,50,56,63,71,80,90,101,113,127,144,162,182,203,226,255,255 };
static const uint8_t beta_t[52] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,3,3,3,3,4,4,4,6,6,7,7,8,8,
                                    9,9,10,10,11,11,12,12,13,13,14,14,15,15,16,16,17,17,18,18 };
static const uint8_t tc0_t[52][3] = {
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,1},{0,0,1},{0,0,1},{0,0,1},{0,1,1},{0,1,1},{1,1,1},{1,1,1},{1,1,1},
    {1,1,1},{1,1,2},{1,1,2},{1,1,2},{1,1,2},{1,2,3},{1,2,3},{2,2,3},{2,2,4},{2,3,4},{2,3,4},{3,3,5},{3,4,6},
    {3,4,6},{4,5,7},{4,5,8},{4,6,9},{5,7,10},{6,8,11},{6,8,13},{7,10,14},{8,11,16},{9,12,18},{10,13,20},
    {11,15,23},{13,17,25} };

static int mvdiff(const int16_t *a, const int16_t *b) { return iabs(a[0] - b[0]) >= 4 || iabs(a[1] - b[1]) >= 4; }

/* bS between 4x4 luma blocks bp (in p) and bq (in q); mb_edge if they are in different MBs. */
static int strength(mbinfo *p, int bp, mbinfo *q, int bq, int mb_edge) {
    if (p->intra || q->intra) return mb_edge ? 4 : 3;
    if (p->nz[bp] || q->nz[bq]) return 2;
    int pr0 = p->ref[0][bp >> 2] >= 0 ? p->refpic[0][bp >> 2] : -1, pr1 = p->ref[1][bp >> 2] >= 0 ? p->refpic[1][bp >> 2] : -1;
    int qr0 = q->ref[0][bq >> 2] >= 0 ? q->refpic[0][bq >> 2] : -1, qr1 = q->ref[1][bq >> 2] >= 0 ? q->refpic[1][bq >> 2] : -1;
    int np = (pr0 >= 0) + (pr1 >= 0), nq = (qr0 >= 0) + (qr1 >= 0);
    if (np != nq) return 1;
    if (np == 1) {
        int rp = pr0 >= 0 ? pr0 : pr1, rq = qr0 >= 0 ? qr0 : qr1;
        if (rp != rq) return 1;
        const int16_t *mp = pr0 >= 0 ? p->mv[0][bp] : p->mv[1][bp], *mq = qr0 >= 0 ? q->mv[0][bq] : q->mv[1][bq];
        return mvdiff(mp, mq);
    }
    /* two each: the same pair of pictures, matched up by picture */
    if (!((pr0 == qr0 && pr1 == qr1) || (pr0 == qr1 && pr1 == qr0))) return 1;
    const int16_t *p0 = p->mv[0][bp], *p1 = p->mv[1][bp], *q0 = q->mv[0][bq], *q1 = q->mv[1][bq];
    if (pr0 != pr1) {
        if (pr0 == qr0) return mvdiff(p0, q0) || mvdiff(p1, q1);
        return mvdiff(p0, q1) || mvdiff(p1, q0);
    }
    return (mvdiff(p0, q0) || mvdiff(p1, q1)) && (mvdiff(p0, q1) || mvdiff(p1, q0));
}

/* Filter n lines across an edge: q0 at s, the edge between s[-step] and s[0], lines `along` apart. */
static void edge(uint8_t *s, int step, int along, int n, const int *bs, int per, int indexA, int beta, int chroma) {
    int alpha = alpha_t[indexA];
    if (!alpha || !beta) return;
    for (int i = 0; i < n; i++, s += along) {
        int b = bs[i / per];
        if (!b) continue;
        int p0 = s[-step], p1 = s[-2 * step], q0 = s[0], q1 = s[step];
        if (iabs(p0 - q0) >= alpha || iabs(p1 - p0) >= beta || iabs(q1 - q0) >= beta) continue;
        if (b < 4) {
            int tc0 = tc0_t[indexA][b - 1], tc;
            if (chroma) tc = tc0 + 1;
            else {
                int p2 = s[-3 * step], q2 = s[2 * step];
                int ap = iabs(p2 - p0) < beta, aq = iabs(q2 - q0) < beta;
                tc = tc0 + ap + aq;
                if (ap) s[-2 * step] = (uint8_t)(p1 + clip3(-tc0, tc0, (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1));
                if (aq) s[step] = (uint8_t)(q1 + clip3(-tc0, tc0, (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1));
            }
            int delta = clip3(-tc, tc, (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
            s[-step] = clip1(p0 + delta);
            s[0] = clip1(q0 - delta);
        } else if (chroma) {
            s[-step] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
            s[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
        } else {
            int p2 = s[-3 * step], q2 = s[2 * step], p3 = s[-4 * step], q3 = s[3 * step];
            int small = iabs(p0 - q0) < ((alpha >> 2) + 2);
            if (iabs(p2 - p0) < beta && small) {
                s[-step] = (uint8_t)((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
                s[-2 * step] = (uint8_t)((p2 + p1 + p0 + q0 + 2) >> 2);
                s[-3 * step] = (uint8_t)((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
            } else s[-step] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
            if (iabs(q2 - q0) < beta && small) {
                s[0] = (uint8_t)((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
                s[step] = (uint8_t)((p0 + q0 + q1 + q2 + 2) >> 2);
                s[2 * step] = (uint8_t)((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
            } else s[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
        }
    }
}

int h264_skip_deblock;                  /* tests: compare against ffmpeg -skip_loop_filter all */

void h264_deblock(h264 *d) {
    if (h264_skip_deblock) return;
    picture *c = d->cur;
    int ys = c->ystride, cs = c->cstride;
    for (int my = 0; my < d->mb_h; my++)
        for (int mx = 0; mx < d->mb_w; mx++) {
            mbinfo *q = &c->mb[my * d->mb_w + mx];
            if (q->dbk == 1 || q->slice < 0) continue;
            int left = mx > 0, top = my > 0;
            if (q->dbk == 2) {
                if (left && c->mb[my * d->mb_w + mx - 1].slice != q->slice) left = 0;
                if (top && c->mb[(my - 1) * d->mb_w + mx].slice != q->slice) top = 0;
            }
            uint8_t *Y = c->y + my * 16 * ys + mx * 16;
            uint8_t *U = c->u + my * 8 * cs + mx * 8, *V = c->v + my * 8 * cs + mx * 8;
            for (int dir = 0; dir < 2; dir++) {                      /* 0 vertical edges, 1 horizontal */
                for (int e = 0; e < 4; e++) {
                    if (e == 0 && !(dir ? top : left)) continue;
                    mbinfo *p = e ? q : &c->mb[(my - dir) * d->mb_w + mx - (dir ^ 1)];
                    int bs[4];
                    for (int k = 0; k < 4; k++) {
                        int qx = dir ? k : e, qy = dir ? e : k;
                        int px = dir ? qx : (e ? qx - 1 : 3), py = dir ? (e ? qy - 1 : 3) : qy;
                        bs[k] = strength(p, blk_at[py][px], q, blk_at[qy][qx], e == 0);
                    }
                    if (!bs[0] && !bs[1] && !bs[2] && !bs[3]) continue;
                    int qpav = (p->qp + q->qp + 1) >> 1;
                    int ia = clip3(0, 51, qpav + q->aoff), ib = clip3(0, 51, qpav + q->boff);
                    if (dir == 0) edge(Y + e * 4, 1, ys, 16, bs, 4, ia, beta_t[ib], 0);
                    else edge(Y + e * 4 * ys, ys, 1, 16, bs, 4, ia, beta_t[ib], 0);
                    if (e & 1) continue;                              /* chroma: edges 0 and 2 only */
                    for (int k = 0; k < 2; k++) {
                        int qpc = (p->qpc[k] + q->qpc[k] + 1) >> 1;
                        int ca = clip3(0, 51, qpc + q->aoff), cb = clip3(0, 51, qpc + q->boff);
                        uint8_t *P = k ? V : U;
                        if (dir == 0) edge(P + e * 2, 1, cs, 8, bs, 2, ca, beta_t[cb], 1);
                        else edge(P + e * 2 * cs, cs, 1, 8, bs, 2, ca, beta_t[cb], 1);
                    }
                }
            }
        }
}
