/* ===========================================================================
 *  BoltOS Mini  -  mini/h264.c
 *
 *  The decoder's outer layer (see h264.h): NAL units out of an MP4 sample,
 *  the parameter sets, slice headers, picture order counts, which decoded
 *  pictures stay as references and which come out next for display.
 *  Macroblocks are h264mb.c's, prediction h264pred.c's, the loop filter
 *  h264dbk.c's.
 * ===========================================================================*/
#include <stdint.h>
#include "h264int.h"
#include "kheap.h"
#include "string.h"
#include "stdio.h"

void h264_fail(h264 *d, const char *what) {
    if (!d->err[0]) snprintf(d->err, sizeof d->err, "%s", what);
}
const char *h264_error(h264 *d) { return d->err; }

/* The RBSP: emulation-prevention bytes out, eight zero bytes after (bs_peek32
 * reads ahead). */
static int unescape(h264 *d, const uint8_t *s, int n) {
    if (n + 16 > d->nal_cap) {
        if (d->nal) kfree(d->nal);
        d->nal_cap = n + 4096;
        d->nal = kmalloc((uint64_t)d->nal_cap);
        if (!d->nal) { d->nal_cap = 0; return -1; }
    }
    int o = 0, zeros = 0;
    for (int i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (zeros >= 2 && c == 3) { zeros = 0; continue; }
        d->nal[o++] = c;
        zeros = c ? 0 : zeros + 1;
    }
    memset(d->nal + o, 0, 16);
    /* the bits that count: up to the stop bit */
    int last = o - 1;
    while (last >= 0 && !d->nal[last]) last--;
    int bits = 0;
    if (last >= 0) { int k = 0; while (!((d->nal[last] >> k) & 1)) k++; bits = last * 8 + 8 - k - 1; }
    d->bs.p = d->nal; d->bs.pos = 0; d->bs.bits = bits;
    return o;
}

/* ---- parameter sets ------------------------------------------------------------------- */
static void skip_hrd(bs_t *b) {
    int n = (int)bs_ue(b) + 1;
    bs_u(b, 8);
    for (int i = 0; i < n && i < 32; i++) { bs_ue(b); bs_ue(b); bs_u1(b); }
    bs_u(b, 20);
}

static int parse_sps(h264 *d) {
    bs_t *b = &d->bs;
    sps_t t;
    memset(&t, 0, sizeof t);
    t.profile = (int)bs_u(b, 8);
    bs_u(b, 8);
    t.level = (int)bs_u(b, 8);
    t.id = (int)bs_ue(b);
    if (t.id > 3) { h264_fail(d, "SPS id out of range"); return -1; }
    t.chroma_format = 1;
    if (t.profile == 100 || t.profile == 110 || t.profile == 122 || t.profile == 244 || t.profile == 44 ||
        t.profile == 83 || t.profile == 86 || t.profile == 118 || t.profile == 128 || t.profile == 138 ||
        t.profile == 139 || t.profile == 134 || t.profile == 135) {
        t.chroma_format = (int)bs_ue(b);
        if (t.chroma_format == 3) bs_u1(b);
        if (bs_ue(b) || bs_ue(b)) { h264_fail(d, "not 8-bit video"); return -1; }
        bs_u1(b);
        if (bs_u1(b)) { h264_fail(d, "scaling matrices"); return -1; }
    }
    if (t.chroma_format != 1) { h264_fail(d, "not 4:2:0"); return -1; }
    t.log2_max_frame_num = (int)bs_ue(b) + 4;
    t.poc_type = (int)bs_ue(b);
    if (t.poc_type == 0) t.log2_max_poc_lsb = (int)bs_ue(b) + 4;
    else if (t.poc_type == 1) {
        t.delta_pic_order_always_zero = bs_u1(b);
        t.offset_for_non_ref_pic = bs_se(b);
        t.offset_for_top_to_bottom = bs_se(b);
        t.num_ref_frames_in_poc_cycle = (int)bs_ue(b);
        if (t.num_ref_frames_in_poc_cycle > 255) { h264_fail(d, "POC cycle too long"); return -1; }
        for (int i = 0; i < t.num_ref_frames_in_poc_cycle; i++) t.offset_for_ref_frame[i] = bs_se(b);
    }
    t.max_num_ref_frames = (int)bs_ue(b);
    t.gaps_allowed = bs_u1(b);
    t.mb_w = (int)bs_ue(b) + 1;
    t.mb_h = (int)bs_ue(b) + 1;
    t.frame_mbs_only = bs_u1(b);
    if (!t.frame_mbs_only) { h264_fail(d, "interlaced video"); return -1; }
    t.direct_8x8 = bs_u1(b);
    if (bs_u1(b)) {
        t.crop_l = (int)bs_ue(b) * 2; t.crop_r = (int)bs_ue(b) * 2;
        t.crop_t = (int)bs_ue(b) * 2; t.crop_b = (int)bs_ue(b) * 2;
    }
    t.reorder = t.profile == 66 ? 0 : (t.max_num_ref_frames > 0 ? t.max_num_ref_frames : 1);
    if (bs_u1(b)) {                                             /* VUI          */
        if (bs_u1(b)) { if (bs_u(b, 8) == 255) bs_u(b, 32); }
        if (bs_u1(b)) bs_u1(b);
        if (bs_u1(b)) { bs_u(b, 4); if (bs_u1(b)) bs_u(b, 24); }
        if (bs_u1(b)) { bs_ue(b); bs_ue(b); }
        if (bs_u1(b)) { bs_u(b, 32); bs_u(b, 32); bs_u1(b); }
        int nal_hrd = bs_u1(b);
        if (nal_hrd) skip_hrd(b);
        int vcl_hrd = bs_u1(b);
        if (vcl_hrd) skip_hrd(b);
        if (nal_hrd || vcl_hrd) bs_u1(b);
        bs_u1(b);
        if (bs_left(b) > 0 && bs_u1(b)) {
            bs_u1(b); bs_ue(b); bs_ue(b); bs_ue(b); bs_ue(b);
            t.reorder = (int)bs_ue(b);
            bs_ue(b);
        }
    }
    if (t.mb_w > 256 || t.mb_h > 256) { h264_fail(d, "picture too large"); return -1; }
    t.valid = 1;
    d->sps[t.id] = t;
    return 0;
}

static int parse_pps(h264 *d) {
    bs_t *b = &d->bs;
    pps_t t;
    memset(&t, 0, sizeof t);
    t.id = (int)bs_ue(b);
    t.sps_id = (int)bs_ue(b);
    if (t.id > 15 || t.sps_id > 3) { h264_fail(d, "PPS id out of range"); return -1; }
    t.cabac = bs_u1(b);
    t.bottom_field_poc = bs_u1(b);
    if (bs_ue(b)) { h264_fail(d, "slice groups (FMO)"); return -1; }
    t.num_ref_idx[0] = (int)bs_ue(b) + 1;
    t.num_ref_idx[1] = (int)bs_ue(b) + 1;
    t.weighted_pred = bs_u1(b);
    t.weighted_bipred = (int)bs_u(b, 2);
    t.init_qp = 26 + bs_se(b);
    bs_se(b);
    t.chroma_qp_offset = bs_se(b);
    t.deblock_ctrl = bs_u1(b);
    t.constrained_intra = bs_u1(b);
    t.redundant_pic_cnt = bs_u1(b);
    t.second_chroma_qp_offset = t.chroma_qp_offset;
    if (bs_left(b) > 0) {
        t.transform_8x8 = bs_u1(b);
        if (bs_u1(b)) { h264_fail(d, "scaling matrices"); return -1; }
        t.second_chroma_qp_offset = bs_se(b);
    }
    if (t.transform_8x8) { h264_fail(d, "8x8 transforms (High profile)"); return -1; }
    t.valid = 1;
    d->pps[t.id] = t;
    return 0;
}

/* ---- pictures ------------------------------------------------------------------------- */
static int alloc_pic(h264 *d, picture *p) {
    if (p->mem) return 0;
    int ys = d->w, cs = d->w / 2;
    uint64_t n = (uint64_t)ys * d->h + 2ull * cs * (d->h / 2);
    p->mem = kmalloc(n + 64);
    p->mb = kmalloc((uint64_t)d->mb_w * d->mb_h * sizeof(mbinfo));
    if (!p->mem || !p->mb) { h264_fail(d, "out of memory"); return -1; }
    p->ystride = ys; p->cstride = cs;
    p->y = p->mem;
    p->u = p->y + ys * d->h;
    p->v = p->u + cs * (d->h / 2);
    return 0;
}

static void free_pics(h264 *d) {
    for (int i = 0; i < DPB_N; i++) {
        if (d->dpb[i].mem) kfree(d->dpb[i].mem);
        if (d->dpb[i].mb) kfree(d->dpb[i].mb);
        memset(&d->dpb[i], 0, sizeof d->dpb[i]);
    }
}

/* Display order: the waiting picture with the lowest POC. */
static int lowest_waiting(h264 *d) {
    int best = -1;
    for (int i = 0; i < DPB_N; i++)
        if (d->dpb[i].wait_output && (best < 0 || d->dpb[i].poc < d->dpb[best].poc)) best = i;
    return best;
}
static int count_waiting(h264 *d) {
    int n = 0;
    for (int i = 0; i < DPB_N; i++) n += d->dpb[i].wait_output != 0;
    return n;
}

/* The output queue: slots in display order, taken by h264_output. */
static int outq[DPB_N], outq_n;
static void bump(h264 *d) {
    int i = lowest_waiting(d);
    if (i < 0) return;
    d->dpb[i].wait_output = 0;
    if (outq_n < DPB_N) outq[outq_n++] = i;
}

static int free_slot(h264 *d) {
    for (;;) {
        for (int i = 0; i < DPB_N; i++) {
            picture *p = &d->dpb[i];
            if (p == d->cur || p->ref || p->wait_output) continue;
            int queued = 0;
            for (int k = 0; k < outq_n; k++) if (outq[k] == i) queued = 1;
            if (!queued) return i;
        }
        if (count_waiting(d) == 0) return -1;
        bump(d);
    }
}

/* 8.2.5: what the picture just decoded does to the set of references. */
static void mark_refs(h264 *d) {
    picture *c = d->cur;
    slice_t *s = &d->sl;
    int max_frame_num = 1 << d->s->log2_max_frame_num;
    if (!s->nal_ref_idc) { c->ref = 0; return; }
    if (s->idr) {
        for (int i = 0; i < DPB_N; i++) if (&d->dpb[i] != c) d->dpb[i].ref = 0;
        if (s->long_term_ref) { c->ref = 2; c->long_idx = 0; d->max_long_idx = 0; }
        else { c->ref = 1; d->max_long_idx = -1; }
        return;
    }
    int marked_long = 0;
    if (s->adaptive) {
        for (int k = 0; k < s->mmco_count; k++) {
            int op = s->mmco[k][0], a = s->mmco[k][1], b = s->mmco[k][2];
            if (op == 1 || op == 3) {
                int pn = c->frame_num - (a + 1);                 /* picNumX */
                for (int i = 0; i < DPB_N; i++) {
                    picture *p = &d->dpb[i];
                    if (p == c || p->ref != 1) continue;
                    int wrap = p->frame_num > c->frame_num ? p->frame_num - max_frame_num : p->frame_num;
                    if (wrap == pn) {
                        if (op == 1) p->ref = 0;
                        else {
                            for (int j = 0; j < DPB_N; j++)
                                if (d->dpb[j].ref == 2 && d->dpb[j].long_idx == b) d->dpb[j].ref = 0;
                            p->ref = 2; p->long_idx = b;
                        }
                    }
                }
            } else if (op == 2) {
                for (int i = 0; i < DPB_N; i++) if (d->dpb[i].ref == 2 && d->dpb[i].long_idx == a) d->dpb[i].ref = 0;
            } else if (op == 4) {
                d->max_long_idx = a - 1;
                for (int i = 0; i < DPB_N; i++) if (d->dpb[i].ref == 2 && d->dpb[i].long_idx > d->max_long_idx) d->dpb[i].ref = 0;
            } else if (op == 5) {
                for (int i = 0; i < DPB_N; i++) if (&d->dpb[i] != c) d->dpb[i].ref = 0;
                d->max_long_idx = -1;
                c->mmco5 = 1;
            } else if (op == 6) {
                for (int i = 0; i < DPB_N; i++) if (d->dpb[i].ref == 2 && d->dpb[i].long_idx == a && &d->dpb[i] != c) d->dpb[i].ref = 0;
                c->ref = 2; c->long_idx = a; marked_long = 1;
            }
        }
    } else {
        /* sliding window */
        int nshort = 0, nlong = 0, oldest = -1;
        for (int i = 0; i < DPB_N; i++) {
            picture *p = &d->dpb[i];
            if (p == c) continue;
            if (p->ref == 1) {
                nshort++;
                int wrap = p->frame_num > c->frame_num ? p->frame_num - max_frame_num : p->frame_num;
                p->frame_num_wrap = wrap;
                if (oldest < 0 || wrap < d->dpb[oldest].frame_num_wrap) oldest = i;
            } else if (p->ref == 2) nlong++;
        }
        int maxr = d->s->max_num_ref_frames > 0 ? d->s->max_num_ref_frames : 1;
        if (nshort + nlong >= maxr && nshort > 0 && oldest >= 0) d->dpb[oldest].ref = 0;
    }
    if (!marked_long) c->ref = 1;
}

/* The picture is decoded: filter it, keep it if it is a reference, queue it for display. */
static void finish_picture(h264 *d) {
    picture *c = d->cur;
    if (!c) return;
    h264_deblock(d);
    mark_refs(d);
    if (d->s->poc_type == 0 && d->sl.nal_ref_idc) {
        if (c->mmco5) { d->prev_poc_msb = 0; d->prev_poc_lsb = 0; }
        else { d->prev_poc_msb = d->prev_poc_msb_cur; d->prev_poc_lsb = d->sl.poc_lsb; }
    }
    d->prev_frame_num_offset = c->mmco5 ? 0 : d->frame_num_offset;
    d->prev_frame_num = c->mmco5 ? 0 : d->sl.frame_num;
    if (c->mmco5) c->poc = 0;
    c->wait_output = 1;
    while (count_waiting(d) > d->s->reorder) bump(d);
    d->cur = 0;
    d->frames++;
}

/* 8.2.1 */
static int compute_poc(h264 *d) {
    const sps_t *s = d->s;
    slice_t *sl = &d->sl;
    int max_frame_num = 1 << s->log2_max_frame_num;
    if (s->poc_type == 0) {
        int max_lsb = 1 << s->log2_max_poc_lsb, msb;
        if (sl->idr) { d->prev_poc_msb = 0; d->prev_poc_lsb = 0; }
        if (sl->poc_lsb < d->prev_poc_lsb && d->prev_poc_lsb - sl->poc_lsb >= max_lsb / 2) msb = d->prev_poc_msb + max_lsb;
        else if (sl->poc_lsb > d->prev_poc_lsb && sl->poc_lsb - d->prev_poc_lsb > max_lsb / 2) msb = d->prev_poc_msb - max_lsb;
        else msb = d->prev_poc_msb;
        d->prev_poc_msb_cur = msb;
        int top = msb + sl->poc_lsb, bot = top + sl->delta_poc_bottom;
        return imin(top, bot);
    }
    if (sl->idr) d->frame_num_offset = 0;
    else if (d->prev_frame_num > sl->frame_num) d->frame_num_offset = d->prev_frame_num_offset + max_frame_num;
    else d->frame_num_offset = d->prev_frame_num_offset;
    if (s->poc_type == 2) {
        if (sl->idr) return 0;
        return 2 * (d->frame_num_offset + sl->frame_num) - (sl->nal_ref_idc ? 0 : 1);
    }
    int n = s->num_ref_frames_in_poc_cycle, abs_fn = n ? d->frame_num_offset + sl->frame_num : 0;
    if (!sl->nal_ref_idc && abs_fn > 0) abs_fn--;
    int expected = 0;
    if (abs_fn > 0) {
        int delta = 0;
        for (int i = 0; i < n; i++) delta += s->offset_for_ref_frame[i];
        int cyc = (abs_fn - 1) / n, in = (abs_fn - 1) % n;
        expected = cyc * delta;
        for (int i = 0; i <= in; i++) expected += s->offset_for_ref_frame[i];
    }
    if (!sl->nal_ref_idc) expected += s->offset_for_non_ref_pic;
    int top = expected + sl->delta_poc[0];
    int bot = top + s->offset_for_top_to_bottom + sl->delta_poc[1];
    return imin(top, bot);
}

static int start_picture(h264 *d) {
    slice_t *sl = &d->sl;
    const sps_t *s = d->s;
    if (d->mb_w != s->mb_w || d->mb_h != s->mb_h) {               /* a new size */
        h264_flush(d);
        free_pics(d);
        d->mb_w = s->mb_w; d->mb_h = s->mb_h;
        d->w = s->mb_w * 16; d->h = s->mb_h * 16;
    }
    if (sl->idr) {
        if (!sl->no_output_prior) while (count_waiting(d)) bump(d);
        else for (int i = 0; i < DPB_N; i++) d->dpb[i].wait_output = 0;
        for (int i = 0; i < DPB_N; i++) d->dpb[i].ref = 0;
    }
    int slot = free_slot(d);
    if (slot < 0) { h264_fail(d, "no free picture buffer"); return -1; }
    picture *p = &d->dpb[slot];
    if (alloc_pic(d, p) < 0) return -1;
    p->frame_num = sl->frame_num;
    p->poc = compute_poc(d);
    p->ref = 0; p->long_term = 0; p->wait_output = 0; p->idr = sl->idr; p->mmco5 = 0;
    p->pts = d->pts;
    memset(p->mb, 0, (size_t)d->mb_w * d->mb_h * sizeof(mbinfo));
    for (int i = 0; i < d->mb_w * d->mb_h; i++) p->mb[i].slice = -1;
    d->cur = p; d->cur_slot = slot;
    d->slice_num = 0;
    return 0;
}

/* ---- reference lists (8.2.4) ----------------------------------------------------------------- */
static void sort_by(picture **a, int n, int key, int desc) {      /* key 0 pic_num, 1 poc, 2 long_idx */
    for (int i = 1; i < n; i++) {
        picture *x = a[i];
        int kx = key == 0 ? x->pic_num : key == 1 ? x->poc : x->long_idx;
        int j = i - 1;
        while (j >= 0) {
            int kj = key == 0 ? a[j]->pic_num : key == 1 ? a[j]->poc : a[j]->long_idx;
            if (desc ? kj >= kx : kj <= kx) break;
            a[j + 1] = a[j]; j--;
        }
        a[j + 1] = x;
    }
}

static int build_lists(h264 *d) {
    slice_t *sl = &d->sl;
    picture *c = d->cur;
    int max_frame_num = 1 << d->s->log2_max_frame_num;
    picture *st[DPB_N], *lt[DPB_N];
    int ns = 0, nl = 0;
    for (int i = 0; i < DPB_N; i++) {
        picture *p = &d->dpb[i];
        if (p == c) continue;
        if (p->ref == 1) {
            p->frame_num_wrap = p->frame_num > sl->frame_num ? p->frame_num - max_frame_num : p->frame_num;
            p->pic_num = p->frame_num_wrap;
            st[ns++] = p;
        } else if (p->ref == 2) lt[nl++] = p;
    }
    sort_by(lt, nl, 2, 0);
    int nlists = sl->type == 1 ? 2 : sl->type == 0 ? 1 : 0;
    for (int l = 0; l < nlists; l++) {
        picture *tmp[2 * DPB_N];
        int n = 0;
        if (sl->type == 0) {
            sort_by(st, ns, 0, 1);
            for (int i = 0; i < ns; i++) tmp[n++] = st[i];
        } else {
            picture *before[DPB_N], *after[DPB_N];
            int nb = 0, na = 0;
            for (int i = 0; i < ns; i++) { if (st[i]->poc < c->poc) before[nb++] = st[i]; else after[na++] = st[i]; }
            sort_by(before, nb, 1, 1);
            sort_by(after, na, 1, 0);
            if (l == 0) { for (int i = 0; i < nb; i++) tmp[n++] = before[i]; for (int i = 0; i < na; i++) tmp[n++] = after[i]; }
            else { for (int i = 0; i < na; i++) tmp[n++] = after[i]; for (int i = 0; i < nb; i++) tmp[n++] = before[i]; }
        }
        for (int i = 0; i < nl; i++) tmp[n++] = lt[i];
        if (l == 1 && n > 1) {
            int same = 1;
            for (int i = 0; i < n; i++) if (tmp[i] != sl->list[0][i]) { same = 0; break; }
            if (same) { picture *x = tmp[0]; tmp[0] = tmp[1]; tmp[1] = x; }
        }
        int want = sl->num_ref_idx[l];
        for (int i = 0; i < want; i++) sl->list[l][i] = i < n ? tmp[i] : (n ? tmp[n - 1] : c);
    }
    return 0;
}

/* The modifications were parsed into these, then applied once the lists exist. */
static int mod_n[2], mod_op[2][33][2];

static void apply_mods(h264 *d) {
    slice_t *sl = &d->sl;
    int max_frame_num = 1 << d->s->log2_max_frame_num;
    for (int l = 0; l < 2; l++) {
        int pred = sl->frame_num, ref_idx = 0, want = sl->num_ref_idx[l];
        for (int k = 0; k < mod_n[l]; k++) {
            int idc = mod_op[l][k][0], v = mod_op[l][k][1];
            picture *pic = 0;
            if (idc == 0 || idc == 1) {
                int nowrap = idc == 0 ? pred - (v + 1) : pred + (v + 1);
                if (nowrap < 0) nowrap += max_frame_num;
                if (nowrap >= max_frame_num) nowrap -= max_frame_num;
                pred = nowrap;
                int pn = nowrap > sl->frame_num ? nowrap - max_frame_num : nowrap;
                for (int i = 0; i < DPB_N; i++)
                    if (d->dpb[i].ref == 1 && &d->dpb[i] != d->cur && d->dpb[i].pic_num == pn) pic = &d->dpb[i];
            } else if (idc == 2) {
                for (int i = 0; i < DPB_N; i++)
                    if (d->dpb[i].ref == 2 && d->dpb[i].long_idx == v) pic = &d->dpb[i];
            }
            if (!pic) continue;
            picture *tmp[MAX_REFS + 1];
            for (int i = 0; i < want; i++) tmp[i] = sl->list[l][i];
            for (int c = want; c > ref_idx; c--) tmp[c] = tmp[c - 1];
            tmp[ref_idx++] = pic;
            int n2 = ref_idx;
            for (int c = ref_idx; c <= want; c++) if (tmp[c] != pic) tmp[n2++] = tmp[c];
            for (int i = 0; i < want; i++) sl->list[l][i] = tmp[i];
        }
    }
}

static void implicit_weights(h264 *d) {
    slice_t *sl = &d->sl;
    for (int i = 0; i < sl->num_ref_idx[0]; i++)
        for (int j = 0; j < sl->num_ref_idx[1]; j++) {
            picture *p0 = sl->list[0][i], *p1 = sl->list[1][j];
            int w1 = 32;
            int tb = clip3(-128, 127, d->cur->poc - p0->poc), td = clip3(-128, 127, p1->poc - p0->poc);
            if (td && p0->ref != 2 && p1->ref != 2) {
                int tx = (16384 + iabs(td / 2)) / td;
                int dsf = clip3(-1024, 1023, (tb * tx + 32) >> 6);
                if ((dsf >> 2) >= -64 && (dsf >> 2) <= 128) w1 = dsf >> 2;
            }
            sl->implicit_w[i][j] = (int16_t)w1;
        }
}

/* ---- the slice header (7.3.3) ------------------------------------------------------------------- */
static int slice(h264 *d, int nal_type, int nal_ref_idc) {
    bs_t *b = &d->bs;
    slice_t *sl = &d->sl;
    int first_mb = (int)bs_ue(b);
    int type = (int)bs_ue(b) % 5;
    if (type > 2) { h264_fail(d, "SP/SI slices"); return -1; }
    static const int map[3] = { 0, 1, 2 };          /* P, B, I as coded: 0, 1, 2 */
    type = map[type];
    int pps_id = (int)bs_ue(b);
    if (pps_id > 15 || !d->pps[pps_id].valid || !d->sps[d->pps[pps_id].sps_id].valid) { h264_fail(d, "missing parameter set"); return -1; }
    d->pp = &d->pps[pps_id];
    d->s = &d->sps[d->pp->sps_id];
    const sps_t *s = d->s;
    const pps_t *pp = d->pp;
    int frame_num = (int)bs_u(b, s->log2_max_frame_num);
    int idr = nal_type == 5;
    if (first_mb == 0 && d->cur) finish_picture(d);              /* a new picture */
    if (!d->cur && first_mb != 0) { h264_fail(d, "slice without its picture's start"); return -1; }
    if (first_mb == 0) {
        memset(sl, 0, sizeof *sl);
    }
    sl->type = type; sl->first_mb = first_mb; sl->frame_num = frame_num; sl->idr = idr; sl->nal_ref_idc = nal_ref_idc;
    if (idr) sl->idr_id = (int)bs_ue(b);
    if (s->poc_type == 0) {
        sl->poc_lsb = (int)bs_u(b, s->log2_max_poc_lsb);
        if (pp->bottom_field_poc) sl->delta_poc_bottom = bs_se(b);
    } else if (s->poc_type == 1 && !s->delta_pic_order_always_zero) {
        sl->delta_poc[0] = bs_se(b);
        if (pp->bottom_field_poc) sl->delta_poc[1] = bs_se(b);
    }
    if (pp->redundant_pic_cnt && bs_ue(b)) return 0;           /* a redundant copy: skip */
    if (type == 1) sl->direct_spatial = bs_u1(b);
    sl->num_ref_idx[0] = pp->num_ref_idx[0];
    sl->num_ref_idx[1] = type == 1 ? pp->num_ref_idx[1] : 0;
    if (type == 2) sl->num_ref_idx[0] = 0;
    if (type != 2 && bs_u1(b)) {
        sl->num_ref_idx[0] = (int)bs_ue(b) + 1;
        if (type == 1) sl->num_ref_idx[1] = (int)bs_ue(b) + 1;
    }
    if (sl->num_ref_idx[0] > MAX_REFS || sl->num_ref_idx[1] > MAX_REFS) { h264_fail(d, "too many references"); return -1; }
    mod_n[0] = mod_n[1] = 0;
    for (int l = 0; l < (type == 1 ? 2 : type == 0 ? 1 : 0); l++) {
        if (bs_u1(b)) {
            for (;;) {
                int idc = (int)bs_ue(b);
                if (idc == 3 || bs_left(b) < 0) break;
                int v = (int)bs_ue(b);
                if (mod_n[l] < 33) { mod_op[l][mod_n[l]][0] = idc; mod_op[l][mod_n[l]][1] = v; mod_n[l]++; }
            }
        }
    }
    int explicit_w = (pp->weighted_pred && type == 0) || (pp->weighted_bipred == 1 && type == 1);
    sl->luma_log2_wd = 0; sl->chroma_log2_wd = 0;
    if (explicit_w) {
        sl->luma_log2_wd = (int)bs_ue(b);
        sl->chroma_log2_wd = (int)bs_ue(b);
        for (int l = 0; l < (type == 1 ? 2 : 1); l++)
            for (int i = 0; i < sl->num_ref_idx[l]; i++) {
                sl->lwf[l][i] = (int8_t)bs_u1(b);
                if (sl->lwf[l][i]) { sl->lw[l][i] = (int16_t)bs_se(b); sl->lo[l][i] = (int16_t)bs_se(b); }
                else { sl->lw[l][i] = (int16_t)(1 << sl->luma_log2_wd); sl->lo[l][i] = 0; }
                sl->cwf[l][i] = (int8_t)bs_u1(b);
                for (int c = 0; c < 2; c++) {
                    if (sl->cwf[l][i]) { sl->cw[l][i][c] = (int16_t)bs_se(b); sl->co[l][i][c] = (int16_t)bs_se(b); }
                    else { sl->cw[l][i][c] = (int16_t)(1 << sl->chroma_log2_wd); sl->co[l][i][c] = 0; }
                }
            }
    }
    sl->adaptive = 0; sl->mmco_count = 0;
    if (nal_ref_idc) {
        if (idr) { sl->no_output_prior = bs_u1(b); sl->long_term_ref = bs_u1(b); }
        else if ((sl->adaptive = bs_u1(b))) {
            for (;;) {
                int op = (int)bs_ue(b);
                if (op == 0 || bs_left(b) < 0) break;
                int a = 0, c = 0;
                if (op == 1 || op == 3) a = (int)bs_ue(b);
                if (op == 2) a = (int)bs_ue(b);
                if (op == 3 || op == 6) c = (int)bs_ue(b);
                if (op == 4) a = (int)bs_ue(b);
                if (op == 6) a = c;
                if (sl->mmco_count < 66) { sl->mmco[sl->mmco_count][0] = op; sl->mmco[sl->mmco_count][1] = a; sl->mmco[sl->mmco_count][2] = c; sl->mmco_count++; }
            }
        }
    }
    sl->cabac_init_idc = (pp->cabac && type != 2) ? (int)bs_ue(b) : 0;
    sl->qp = pp->init_qp + bs_se(b);
    sl->disable_deblock = 0; sl->alpha_off = sl->beta_off = 0;
    if (pp->deblock_ctrl) {
        sl->disable_deblock = (int)bs_ue(b);
        if (sl->disable_deblock != 1) { sl->alpha_off = bs_se(b) * 2; sl->beta_off = bs_se(b) * 2; }
    }
    if (sl->qp < 0 || sl->qp > 51 || sl->cabac_init_idc > 2) { h264_fail(d, "bad slice header"); return -1; }
    if (first_mb == 0 && start_picture(d) < 0) return -1;
    if (first_mb >= d->mb_w * d->mb_h) { h264_fail(d, "slice starts past the picture"); return -1; }
    build_lists(d);
    apply_mods(d);
    for (int l = 0; l < 2; l++)
        for (int i = 0; i < sl->num_ref_idx[l]; i++) sl->slot[l][i] = (int8_t)(sl->list[l][i] - d->dpb);
    if (type == 1 && pp->weighted_bipred == 2) implicit_weights(d);
    return h264_slice_data(d);
}

/* ---- the API ---------------------------------------------------------------------------------- */
h264 *h264_open(const uint8_t *avcc, int len) {
    if (len < 7 || avcc[0] != 1) return 0;
    h264 *d = kmalloc(sizeof *d);
    if (!d) return 0;
    memset(d, 0, sizeof *d);
    d->nal_len = (avcc[4] & 3) + 1;
    d->max_long_idx = -1;
    h264_mc_init();
    int n = avcc[5] & 31, o = 6;
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < n && o + 2 <= len; i++) {
            int l = avcc[o] << 8 | avcc[o + 1];
            o += 2;
            if (o + l > len) break;
            if (unescape(d, avcc + o + 1, l - 1) >= 0) {
                if ((avcc[o] & 31) == 7) parse_sps(d); else if ((avcc[o] & 31) == 8) parse_pps(d);
            }
            o += l;
        }
        if (k == 0) { if (o >= len) break; n = avcc[o++]; }
    }
    return d;
}

/* What decoding would allocate as it went, allocated now: a NAL buffer for
 * nal_bytes and the first `pics` pictures, sized by the stream's SPS. The
 * heap is the boot core's alone, so a decoder handed to another core
 * (mini/engine.c) must not grow there. */
int h264_prealloc(h264 *d, int nal_bytes, int pics) {
    const sps_t *s = 0;
    for (int i = 0; i < 4 && !s; i++) if (d->sps[i].valid) s = &d->sps[i];
    if (!s) return -1;
    if (nal_bytes + 16 > d->nal_cap) {
        if (d->nal) kfree(d->nal);
        d->nal_cap = nal_bytes + 4096;
        d->nal = kmalloc((uint64_t)d->nal_cap);
        if (!d->nal) { d->nal_cap = 0; return -1; }
    }
    if (d->mb_w != s->mb_w || d->mb_h != s->mb_h) {
        free_pics(d);
        d->mb_w = s->mb_w; d->mb_h = s->mb_h;
        d->w = s->mb_w * 16; d->h = s->mb_h * 16;
    }
    for (int i = 0; i < pics && i < DPB_N; i++) if (alloc_pic(d, &d->dpb[i]) < 0) return -1;
    return 0;
}

void h264_close(h264 *d) {
    if (!d) return;
    free_pics(d);
    if (d->nal) kfree(d->nal);
    kfree(d);
}

int h264_decode(h264 *d, const uint8_t *au, int len, int64_t pts) {
    d->pts = pts;
    int o = 0;
    while (o + d->nal_len <= len) {
        int n = 0;
        for (int i = 0; i < d->nal_len; i++) n = n << 8 | au[o + i];
        o += d->nal_len;
        if (n <= 0 || o + n > len) break;
        const uint8_t *nal = au + o;
        o += n;
        int type = nal[0] & 31, ref = (nal[0] >> 5) & 3;
        if (unescape(d, nal + 1, n - 1) < 0) return -1;
        int r = 0;
        switch (type) {
        case 1: case 5: r = slice(d, type, ref); break;
        case 7: r = parse_sps(d); break;
        case 8: r = parse_pps(d); break;
        default: break;                                  /* SEI, AUD, filler: nothing to do */
        }
        if (r < 0) return -1;
    }
    /* An access unit is a picture: finish it now, so its output needn't wait for the next one. */
    if (d->cur) finish_picture(d);
    return 0;
}

int h264_output(h264 *d, h264_pic *p) {
    if (!outq_n) return 0;
    picture *f = &d->dpb[outq[0]];
    memmove(outq, outq + 1, (size_t)(--outq_n) * sizeof outq[0]);
    const sps_t *s = d->s;
    p->w = d->w - s->crop_l - s->crop_r;
    p->h = d->h - s->crop_t - s->crop_b;
    p->stride = f->ystride; p->cstride = f->cstride;
    p->y = f->y + s->crop_t * f->ystride + s->crop_l;
    p->u = f->u + s->crop_t / 2 * f->cstride + s->crop_l / 2;
    p->v = f->v + s->crop_t / 2 * f->cstride + s->crop_l / 2;
    p->pts = f->pts;
    return 1;
}

void h264_flush(h264 *d) {
    if (d->cur) finish_picture(d);
    while (count_waiting(d)) bump(d);
}
