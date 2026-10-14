#!/usr/bin/env python3
"""Turn a TrueType font into the outline format the kernel's text renderer eats.

    mkuifont.py SRC.ttf OUT.bfnt [wght=600] [opsz=14]

The desktop draws its text from outlines, not bitmaps: one .bfnt per weight,
rasterised at whatever size the UI asks for and cached. The kernel could parse
a .ttf itself (kernel/ttf.c always has), but that path has three gaps that all
show up the moment the UI font is Inter:

  * Inter ships as one *variable* font. Bold is not a second file, it is a set
    of deltas in `gvar` applied to the regular outlines. Somebody has to apply
    them, and doing it at build time means the kernel never sees a variation.
  * Accented letters are *composite* glyphs -- e-acute is an `e` plus an acute,
    placed by reference. They get flattened here into plain contours.
  * Kerning lives in GPOS, in class-based pair tables. "To", "Ty" and "AV" look
    like typos without it. The pairs between the 256 characters the kernel can
    actually type (it speaks Windows-1252) are resolved here into a flat table.

No fontTools: this machine does not have it, and the parts of the format that
are needed fit in one file. What is implemented is exactly what Inter and
DejaVu use -- fvar/avar/gvar (with IUP), HVAR, glyf (simple + composite),
cmap 4/12, GPOS PairPos 1/2 with VariationIndex devices, Extension lookups.

Output layout (little-endian), version 1:

    0   "BFNT"
    4   u16 version, u16 upm
    8   i16 ascender, descender, line gap, cap height, x height
    18  u16 glyph count, u16 kern count, u16 flags (1 = monospace)
    24  u32 glyph table, u32 point blob, u32 kern table, u32 file size
    40  glyph table: per glyph 20 bytes
            u8 code, u8 pad, u16 advance, i16 xmin, ymin, xmax, ymax,
            u16 contours, u16 points, u32 offset into the point blob
        point blob, per glyph: u16 contour ends[], i16 (x, y)[], on-curve
            bits (bit i of byte i/8), padded to 2
        kern table: u8 left, u8 right, i16 value (font units), sorted
"""
import struct
import sys

# ---------------------------------------------------------------------------
#  Byte readers
# ---------------------------------------------------------------------------
def u8(d, o):  return d[o]
def u16(d, o): return (d[o] << 8) | d[o + 1]
def i16(d, o):
    v = u16(d, o)
    return v - 0x10000 if v & 0x8000 else v
def u32(d, o): return struct.unpack_from(">I", d, o)[0]
def i32(d, o): return struct.unpack_from(">i", d, o)[0]
def f2dot14(d, o): return i16(d, o) / 16384.0


# ---------------------------------------------------------------------------
#  Windows-1252: what the kernel's strings are. 0x80 is the euro, which is
#  where the keyboard driver already puts it. The five holes in the C1 range
#  are borrowed for a few symbols the UI wants and cp1252 does not have.
# ---------------------------------------------------------------------------
EXTRA = {0x81: 0x2192, 0x8D: 0x2190, 0x8F: 0x2191, 0x90: 0x2193, 0x9D: 0x2713}


def codepage():
    out = {}
    for b in range(0x20, 0x100):
        if b == 0x7F:
            continue
        if b in EXTRA:
            out[b] = EXTRA[b]
            continue
        try:
            out[b] = ord(bytes([b]).decode("cp1252"))
        except UnicodeDecodeError:
            pass
    return out


class Font:
    def __init__(self, data):
        self.d = d = data
        self.tab = {}
        for i in range(u16(d, 4)):
            r = 12 + 16 * i
            self.tab[d[r:r + 4].decode("latin1")] = (u32(d, r + 8), u32(d, r + 12))
        head = self.tab["head"][0]
        self.upm = u16(d, head + 18)
        self.long_loca = i16(d, head + 50)
        self.nglyphs = u16(d, self.tab["maxp"][0] + 4)
        hhea = self.tab["hhea"][0]
        self.asc, self.desc, self.gap = i16(d, hhea + 4), i16(d, hhea + 6), i16(d, hhea + 8)
        self.nhm = u16(d, hhea + 34)
        os2 = self.tab["OS/2"][0]
        self.cap = i16(d, os2 + 88) if u16(d, os2) >= 2 else int(self.upm * 0.7)
        self.xh = i16(d, os2 + 86) if u16(d, os2) >= 2 else int(self.upm * 0.5)
        self.cmap = self._cmap()
        self.coords = []           # normalised axis coordinates, one per fvar axis
        self.axes = self._fvar()
        self._gvar_init()
        self._cache = {}

    def has(self, t): return t in self.tab

    # ---- cmap ------------------------------------------------------------
    def _cmap(self):
        d, base = self.d, self.tab["cmap"][0]
        best = None
        for i in range(u16(d, base + 2)):
            r = base + 4 + 8 * i
            plat, enc, off = u16(d, r), u16(d, r + 2), u32(d, r + 4)
            fmt = u16(d, base + off)
            score = {(3, 10): 4, (0, 4): 3, (3, 1): 2, (0, 3): 1}.get((plat, enc), 0)
            if fmt in (4, 12) and (best is None or score > best[0]):
                best = (score, base + off, fmt)
        if not best:
            raise SystemExit("mkuifont: no usable cmap subtable")
        _, s, fmt = best
        m = {}
        if fmt == 12:
            for g in range(u32(d, s + 12)):
                r = s + 16 + 12 * g
                a, b, gid = u32(d, r), u32(d, r + 4), u32(d, r + 8)
                for c in range(a, b + 1):
                    m[c] = gid + c - a
        else:
            n = u16(d, s + 6) // 2
            ends, starts = s + 14, s + 16 + 2 * n
            deltas, ranges = starts + 2 * n, starts + 4 * n
            for i in range(n):
                e, st = u16(d, ends + 2 * i), u16(d, starts + 2 * i)
                dl, ro = i16(d, deltas + 2 * i), u16(d, ranges + 2 * i)
                for c in range(st, e + 1):
                    if c == 0xFFFF:
                        continue
                    if ro == 0:
                        g = (c + dl) & 0xFFFF
                    else:
                        a = ranges + 2 * i + ro + 2 * (c - st)
                        g = u16(d, a)
                        if g:
                            g = (g + dl) & 0xFFFF
                    if g:
                        m[c] = g
        return m

    # ---- metrics ---------------------------------------------------------
    def hmtx(self, g):
        h = self.tab["hmtx"][0]
        if g < self.nhm:
            return u16(self.d, h + 4 * g), i16(self.d, h + 4 * g + 2)
        adv = u16(self.d, h + 4 * (self.nhm - 1))
        return adv, i16(self.d, h + 4 * self.nhm + 2 * (g - self.nhm))

    # ---- variations: fvar / avar ------------------------------------------
    def _fvar(self):
        if not self.has("fvar"):
            return []
        d, o = self.d, self.tab["fvar"][0]
        axo, axc, axs = u16(d, o + 4), u16(d, o + 8), u16(d, o + 10)
        axes = []
        for a in range(axc):
            r = o + axo + a * axs
            axes.append((d[r:r + 4].decode(), i32(d, r + 4) / 65536.0,
                         i32(d, r + 8) / 65536.0, i32(d, r + 12) / 65536.0))
        self.coords = [0.0] * axc
        return axes

    def set_axes(self, user):
        """user: {"wght": 600, ...}. Normalise, then run avar's maps."""
        if not self.axes:
            return
        nc = []
        for tag, mn, df, mx in self.axes:
            v = min(max(float(user.get(tag, df)), mn), mx)
            if v < df:
                n = (v - df) / (df - mn) if df != mn else 0.0
            elif v > df:
                n = (v - df) / (mx - df) if mx != df else 0.0
            else:
                n = 0.0
            nc.append(n)
        if self.has("avar"):
            d, o = self.d, self.tab["avar"][0]
            p = o + 8
            for a in range(u16(d, o + 6)):
                cnt = u16(d, p); p += 2
                pts = [(f2dot14(d, p + 4 * k), f2dot14(d, p + 4 * k + 2)) for k in range(cnt)]
                p += 4 * cnt
                v = nc[a]
                for k in range(1, len(pts)):
                    (x0, y0), (x1, y1) = pts[k - 1], pts[k]
                    if x0 <= v <= x1:
                        nc[a] = y0 if x1 == x0 else y0 + (v - x0) * (y1 - y0) / (x1 - x0)
                        break
        self.coords = nc
        self._cache = {}

    @staticmethod
    def _scalar(coords, peak, start=None, end=None):
        s = 1.0
        for i, pk in enumerate(peak):
            v = coords[i]
            if pk == 0.0:
                continue
            st = start[i] if start else min(pk, 0.0)
            en = end[i] if end else max(pk, 0.0)
            if st > pk or pk > en or (st < 0.0 < en):
                continue      # malformed region: the spec says ignore the axis
            if v == 0.0:
                return 0.0
            if v < st or v > en:
                return 0.0
            if v == pk:
                continue
            if v < pk:
                s *= (v - st) / (pk - st) if pk != st else 1.0
            else:
                s *= (en - v) / (en - pk) if en != pk else 1.0
        return s

    # ---- gvar --------------------------------------------------------------
    def _gvar_init(self):
        self.gvar = None
        if not self.has("gvar"):
            return
        d, o = self.d, self.tab["gvar"][0]
        axc, shc, sho = u16(d, o + 4), u16(d, o + 6), u32(d, o + 8)
        gc, flags, dao = u16(d, o + 12), u16(d, o + 14), u32(d, o + 16)
        shared = [[f2dot14(d, o + sho + (t * axc + a) * 2) for a in range(axc)] for t in range(shc)]
        if flags & 1:
            offs = [u32(d, o + 20 + 4 * i) for i in range(gc + 1)]
        else:
            offs = [u16(d, o + 20 + 2 * i) * 2 for i in range(gc + 1)]
        self.gvar = (o + dao, offs, shared, axc)

    @staticmethod
    def _points(d, p):
        """Packed point numbers -> (list or None for 'all', new p)."""
        n = d[p]; p += 1
        if n & 0x80:
            n = ((n & 0x7F) << 8) | d[p]; p += 1
        if n == 0:
            return None, p
        pts, last = [], 0
        while len(pts) < n:
            c = d[p]; p += 1
            run = (c & 0x7F) + 1
            for _ in range(run):
                if c & 0x80:
                    v = u16(d, p); p += 2
                else:
                    v = d[p]; p += 1
                last += v
                pts.append(last)
        return pts[:n], p

    @staticmethod
    def _deltas(d, p, n):
        out = []
        while len(out) < n:
            c = d[p]; p += 1
            run = (c & 0x3F) + 1
            if c & 0x80:
                out.extend([0] * run)
            elif c & 0x40:
                for _ in range(run):
                    out.append(i16(d, p)); p += 2
            else:
                for _ in range(run):
                    v = d[p]; p += 1
                    out.append(v - 256 if v > 127 else v)
        return out[:n], p

    def glyph_deltas(self, g, npts, ends, orig):
        """Summed (dx, dy) per point for glyph g at the current coordinates.
        `ends` are the contour ends for IUP (None for composites, where an
        untouched component simply does not move); `orig` the default
        coordinates IUP interpolates against."""
        dx = [0.0] * npts
        dy = [0.0] * npts
        if not self.gvar or not any(self.coords):
            return dx, dy
        base, offs, shared, axc = self.gvar
        if offs[g] == offs[g + 1]:
            return dx, dy
        d = self.d
        o = base + offs[g]
        tcount, doff = u16(d, o), u16(d, o + 2)
        p = o + 4
        data = o + doff
        shared_pts = None
        if tcount & 0x8000:
            shared_pts, data = self._points(d, data)
        for _ in range(tcount & 0x0FFF):
            size, ti = u16(d, p), u16(d, p + 2)
            p += 4
            if ti & 0x8000:
                peak = [f2dot14(d, p + 2 * a) for a in range(axc)]; p += 2 * axc
            else:
                peak = shared[ti & 0x0FFF]
            start = end = None
            if ti & 0x4000:
                start = [f2dot14(d, p + 2 * a) for a in range(axc)]; p += 2 * axc
                end = [f2dot14(d, p + 2 * a) for a in range(axc)]; p += 2 * axc
            sc = self._scalar(self.coords, peak, start, end)
            q = data
            data += size
            if sc == 0.0:
                continue
            if ti & 0x2000:
                pts, q = self._points(d, q)
            else:
                pts = shared_pts
            cnt = npts if pts is None else len(pts)
            xs, q = self._deltas(d, q, cnt)
            ys, q = self._deltas(d, q, cnt)
            if pts is None:
                for i in range(npts):
                    dx[i] += xs[i] * sc
                    dy[i] += ys[i] * sc
                continue
            tx = [None] * npts
            ty = [None] * npts
            for k, pi in enumerate(pts):
                if pi < npts:
                    tx[pi] = xs[k]
                    ty[pi] = ys[k]
            if ends is not None:
                self._iup(tx, ty, ends, orig, npts)
            for i in range(npts):
                if tx[i] is not None:
                    dx[i] += tx[i] * sc
                    dy[i] += ty[i] * sc
        return dx, dy

    @staticmethod
    def _iup(tx, ty, ends, orig, npts):
        """Infer the deltas a tuple left out, contour by contour (gvar spec,
        'Inferred deltas for un-referenced point numbers'). Phantom points are
        outside every contour and are left as given."""
        start = 0
        for end in ends:
            idx = list(range(start, end + 1))
            touched = [i for i in idx if tx[i] is not None]
            if not touched:
                start = end + 1
                continue
            if len(touched) == 1:
                t = touched[0]
                for i in idx:
                    if tx[i] is None:
                        tx[i], ty[i] = tx[t], ty[t]
                start = end + 1
                continue
            n = len(idx)
            pos = {i: k for k, i in enumerate(idx)}
            for i in idx:
                if tx[i] is not None:
                    continue
                k = pos[i]
                a = b = None
                for s in range(1, n):          # previous touched
                    j = idx[(k - s) % n]
                    if tx[j] is not None:
                        a = j
                        break
                for s in range(1, n):          # next touched
                    j = idx[(k + s) % n]
                    if tx[j] is not None:
                        b = j
                        break
                for axis, arr in ((0, tx), (1, ty)):
                    c = orig[i][axis]
                    c1, c2 = orig[a][axis], orig[b][axis]
                    d1, d2 = arr[a], arr[b]
                    if c1 == c2:
                        v = d1 if d1 == d2 else 0
                    else:
                        if c1 > c2:
                            c1, c2, d1, d2 = c2, c1, d2, d1
                        if c <= c1:
                            v = d1
                        elif c >= c2:
                            v = d2
                        else:
                            v = d1 + (c - c1) * (d2 - d1) / (c2 - c1)
                    if axis == 0:
                        tx_i = v
                    else:
                        ty_i = v
                tx[i], ty[i] = tx_i, ty_i
            start = end + 1

    # ---- HVAR ----------------------------------------------------------------
    def _ivs_delta(self, store, outer, inner):
        """One delta out of an ItemVariationStore at the current coordinates."""
        d = self.d
        rlo = store + u32(d, store + 2)
        axc, rc = u16(d, rlo), u16(d, rlo + 2)
        cnt = u16(d, store + 6)
        if outer >= cnt:
            return 0.0
        ivd = store + u32(d, store + 8 + 4 * outer)
        items, wdc, ric = u16(d, ivd), u16(d, ivd + 2), u16(d, ivd + 4)
        if inner >= items:
            return 0.0
        regions = [u16(d, ivd + 6 + 2 * k) for k in range(ric)]
        longw = bool(wdc & 0x8000)
        words = wdc & 0x7FFF
        wsz, ssz = (4, 2) if longw else (2, 1)
        rowsz = words * wsz + (ric - words) * ssz
        row = ivd + 6 + 2 * ric + inner * rowsz
        total, q = 0.0, row
        for k, r in enumerate(regions):
            if k < words:
                v = i32(d, q) if longw else i16(d, q); q += wsz
            else:
                if longw:
                    v = i16(d, q)
                else:
                    v = d[q]; v = v - 256 if v > 127 else v
                q += ssz
            ro = rlo + 4 + r * axc * 6
            start = [f2dot14(d, ro + 6 * a) for a in range(axc)]
            peak = [f2dot14(d, ro + 6 * a + 2) for a in range(axc)]
            end = [f2dot14(d, ro + 6 * a + 4) for a in range(axc)]
            total += v * self._scalar(self.coords, peak, start, end)
        return total

    def hvar_advance_delta(self, g):
        if not self.has("HVAR") or not any(self.coords):
            return None
        d, o = self.d, self.tab["HVAR"][0]
        store, amap = o + u32(d, o + 4), u32(d, o + 8)
        outer, inner = 0, g
        if amap:
            m = o + amap
            fmt, ef = d[m], d[m + 1]
            if fmt == 0:
                cnt, p = u16(d, m + 2), m + 4
            else:
                cnt, p = u32(d, m + 2), m + 6
            size = ((ef >> 4) & 3) + 1
            ibits = (ef & 0xF) + 1
            k = min(g, cnt - 1)
            e = int.from_bytes(d[p + k * size: p + (k + 1) * size], "big")
            outer, inner = e >> ibits, e & ((1 << ibits) - 1)
        return self._ivs_delta(store, outer, inner)

    # ---- glyf ------------------------------------------------------------
    def _loca(self, g):
        d, o = self.d, self.tab["loca"][0]
        if self.long_loca:
            return u32(d, o + 4 * g), u32(d, o + 4 * g + 4)
        return u16(d, o + 2 * g) * 2, u16(d, o + 2 * g + 2) * 2

    def outline(self, g, depth=0):
        """(contours, advance) for glyph g at the current coordinates.
        contours: list of lists of (x, y, on) in font units, origin at the
        glyph's left side-bearing point; advance in font units (float)."""
        key = g
        if key in self._cache:
            return self._cache[key]
        d = self.d
        a, b = self._loca(g)
        adv0, lsb = self.hmtx(g)
        glyf = self.tab["glyf"][0]
        if b <= a:                                   # empty: space and friends
            n = 0
            phantom0 = [(0 - lsb, 0), (0 - lsb + adv0, 0), (0, 0), (0, 0)]
            dx, dy = self.glyph_deltas(g, 4, None, phantom0)
            advance = adv0 + dx[1] - dx[0]
            h = self.hvar_advance_delta(g)
            if h is not None:
                advance = adv0 + h
            res = ([], advance)
            self._cache[key] = res
            return res
        o = glyf + a
        nc = i16(d, o)
        xmin = i16(d, o + 2)
        if nc >= 0:
            ends = [u16(d, o + 10 + 2 * i) for i in range(nc)]
            npts = ends[-1] + 1 if nc else 0
            p = o + 10 + 2 * nc
            p += 2 + u16(d, p)                      # skip instructions
            flags = []
            while len(flags) < npts:
                f = d[p]; p += 1
                flags.append(f)
                if f & 8:
                    r = d[p]; p += 1
                    flags.extend([f] * r)
            flags = flags[:npts]
            xs, x = [], 0
            for f in flags:
                if f & 2:
                    v = d[p]; p += 1
                    x += v if f & 16 else -v
                elif not f & 16:
                    x += i16(d, p); p += 2
                xs.append(x)
            ys, y = [], 0
            for f in flags:
                if f & 4:
                    v = d[p]; p += 1
                    y += v if f & 32 else -v
                elif not f & 32:
                    y += i16(d, p); p += 2
                ys.append(y)
            orig = list(zip(xs, ys))
            pp1x = xmin - lsb
            orig_all = orig + [(pp1x, 0), (pp1x + adv0, 0), (0, 0), (0, 0)]
            dx, dy = self.glyph_deltas(g, npts + 4, ends, orig_all)
            org = pp1x + dx[npts]
            advance = (pp1x + adv0 + dx[npts + 1]) - org
            h = self.hvar_advance_delta(g)
            if h is not None:
                advance = adv0 + h
            contours, s = [], 0
            for e in ends:
                contours.append([(xs[i] + dx[i] - org, ys[i] + dy[i], flags[i] & 1)
                                 for i in range(s, e + 1)])
                s = e + 1
            res = (contours, advance)
            self._cache[key] = res
            return res

        # composite
        if depth > 8:
            return ([], adv0)
        comps = []
        p = o + 10
        while True:
            fl, gi = u16(d, p), u16(d, p + 2)
            p += 4
            if fl & 1:
                a1, a2 = i16(d, p), i16(d, p + 2); p += 4
            else:
                a1, a2 = d[p], d[p + 1]; p += 2
                if fl & 2:
                    a1 = a1 - 256 if a1 > 127 else a1
                    a2 = a2 - 256 if a2 > 127 else a2
            m = (1.0, 0.0, 0.0, 1.0)
            if fl & 8:
                s_ = f2dot14(d, p); p += 2
                m = (s_, 0.0, 0.0, s_)
            elif fl & 0x40:
                m = (f2dot14(d, p), 0.0, 0.0, f2dot14(d, p + 2)); p += 4
            elif fl & 0x80:
                m = (f2dot14(d, p), f2dot14(d, p + 2), f2dot14(d, p + 4), f2dot14(d, p + 6)); p += 8
            comps.append((fl, gi, a1, a2, m))
            if not fl & 0x20:
                break
        n = len(comps)
        pp1x = xmin - lsb
        orig_all = [(c[2], c[3]) for c in comps] + [(pp1x, 0), (pp1x + adv0, 0), (0, 0), (0, 0)]
        dx, dy = self.glyph_deltas(g, n + 4, None, orig_all)
        org = pp1x + dx[n]
        advance = (pp1x + adv0 + dx[n + 1]) - org
        h = self.hvar_advance_delta(g)
        if h is not None:
            advance = adv0 + h
        out = []
        for k, (fl, gi, a1, a2, m) in enumerate(comps):
            sub, sadv = self.outline(gi, depth + 1)
            if fl & 0x200:                           # USE_MY_METRICS
                advance = sadv
            if not fl & 2:                           # point matching: never seen in these fonts
                ox = oy = 0.0
            else:
                ox, oy = a1 + dx[k], a2 + dy[k]
            for c in sub:
                out.append([(m[0] * x + m[2] * y + ox - org, m[1] * x + m[3] * y + oy, on)
                            for (x, y, on) in c])
        res = (out, advance)
        self._cache[key] = res
        return res

    # ---- GPOS kerning ------------------------------------------------------
    def _coverage(self, o):
        d = self.d
        fmt = u16(d, o)
        cov = {}
        if fmt == 1:
            for i in range(u16(d, o + 2)):
                cov[u16(d, o + 4 + 2 * i)] = i
        else:
            for r in range(u16(d, o + 2)):
                q = o + 4 + 6 * r
                st, en, si = u16(d, q), u16(d, q + 2), u16(d, q + 4)
                for g in range(st, en + 1):
                    cov[g] = si + g - st
        return cov

    def _classdef(self, o):
        d = self.d
        fmt = u16(d, o)
        cl = {}
        if fmt == 1:
            st = u16(d, o + 2)
            for i in range(u16(d, o + 4)):
                cl[st + i] = u16(d, o + 6 + 2 * i)
        else:
            for r in range(u16(d, o + 2)):
                q = o + 4 + 6 * r
                for g in range(u16(d, q), u16(d, q + 2) + 1):
                    cl[g] = u16(d, q + 4)
        return cl

    def _gdef_store(self):
        if not self.has("GDEF"):
            return None
        d, o = self.d, self.tab["GDEF"][0]
        if u16(d, o) == 1 and u16(d, o + 2) >= 3:
            off = u32(d, o + 14)
            return o + off if off else None
        return None

    def _value(self, base, rec, fmt, store):
        """Read a ValueRecord at rec (formats from the subtable); return
        (x advance incl. variation delta, size in bytes)."""
        d = self.d
        q, xa, xdev = rec, 0, 0
        for bit in range(8):
            if not fmt & (1 << bit):
                continue
            if bit == 2:
                xa = i16(d, q)
            elif bit == 6:
                xdev = u16(d, q)
            q += 2
        if xdev and store is not None:
            dv = base + xdev
            if u16(d, dv + 4) == 0x8000:
                xa += self._ivs_delta(store, u16(d, dv), u16(d, dv + 2))
        return xa, q - rec

    @staticmethod
    def _vsize(fmt):
        return 2 * bin(fmt & 0xFF).count("1")

    def kerning(self, glyphs):
        """{(gl, gr): units} for every ordered pair in `glyphs` with a kern."""
        if not self.has("GPOS"):
            return {}
        d, o = self.d, self.tab["GPOS"][0]
        store = self._gdef_store()
        fl, ll = o + u16(d, o + 6), o + u16(d, o + 8)
        lookups = set()
        for i in range(u16(d, fl)):
            r = fl + 2 + 6 * i
            if d[r:r + 4] == b"kern":
                f = fl + u16(d, r + 4)
                for k in range(u16(d, f + 2)):
                    lookups.add(u16(d, f + 4 + 2 * k))
        gset = set(glyphs)
        result = {}
        for li in sorted(lookups):
            lk = ll + u16(d, ll + 2 + 2 * li)
            ltype, nsub = u16(d, lk), u16(d, lk + 4)
            subs = []
            for s in range(nsub):
                st = lk + u16(d, lk + 6 + 2 * s)
                if ltype == 9:
                    if u16(d, st + 2) != 2:
                        continue
                    st = st + u32(d, st + 4)
                elif ltype != 2:
                    continue
                subs.append(st)
            done = set()
            for st in subs:
                fmt = u16(d, st)
                cov = self._coverage(st + u16(d, st + 2))
                vf1, vf2 = u16(d, st + 4), u16(d, st + 6)
                s1, s2 = self._vsize(vf1), self._vsize(vf2)
                firsts = [g for g in gset if g in cov]
                if fmt == 1:
                    for g1 in firsts:
                        ps = st + u16(d, st + 10 + 2 * cov[g1])
                        cnt = u16(d, ps)
                        for k in range(cnt):
                            rec = ps + 2 + k * (2 + s1 + s2)
                            g2 = u16(d, rec)
                            if g2 not in gset or (g1, g2) in done:
                                continue
                            v, _ = self._value(ps, rec + 2, vf1, store)
                            done.add((g1, g2))      # found: this subtable applied
                            if v:
                                result[(g1, g2)] = result.get((g1, g2), 0) + v
                elif fmt == 2:
                    cd1 = self._classdef(st + u16(d, st + 8))
                    cd2 = self._classdef(st + u16(d, st + 10))
                    c1n, c2n = u16(d, st + 12), u16(d, st + 14)
                    for g1 in firsts:
                        c1 = cd1.get(g1, 0)
                        if c1 >= c1n:
                            continue
                        for g2 in gset:
                            if (g1, g2) in done:
                                continue
                            c2 = cd2.get(g2, 0)
                            if c2 >= c2n:
                                continue
                            done.add((g1, g2))      # covered first glyph: applies, even at 0
                            rec = st + 16 + (c1 * c2n + c2) * (s1 + s2)
                            v, _ = self._value(st, rec, vf1, store)
                            if v:
                                result[(g1, g2)] = result.get((g1, g2), 0) + v
        return result


# ---------------------------------------------------------------------------
#  Writer
# ---------------------------------------------------------------------------
def build(src, wght=None, opsz=None):
    f = Font(open(src, "rb").read())
    user = {}
    if wght is not None:
        user["wght"] = wght
    if opsz is not None:
        user["opsz"] = opsz
    f.set_axes(user)
    cp = codepage()
    codes = sorted(b for b, u in cp.items() if u in f.cmap)
    gids = {b: f.cmap[cp[b]] for b in codes}

    glyphs = []
    for b in codes:
        contours, adv = f.outline(gids[b])
        contours = [[(int(round(x)), int(round(y)), on) for (x, y, on) in c] for c in contours if len(c) >= 2]
        glyphs.append((b, int(round(adv)), contours))

    mono = len({g[1] for g in glyphs if g[0] < 0x7F}) == 1
    kern = {}
    if not mono:
        kp = f.kerning(list(set(gids.values())))
        inv = {}
        for b, g in gids.items():
            inv.setdefault(g, []).append(b)
        for (g1, g2), v in kp.items():
            v = int(round(v))
            if not v:
                continue
            for b1 in inv.get(g1, []):
                for b2 in inv.get(g2, []):
                    kern[(b1, b2)] = v

    # point blob
    blob = bytearray()
    table = bytearray()
    for b, adv, contours in glyphs:
        off = len(blob)
        pts = [p for c in contours for p in c]
        if pts:
            xmin = min(p[0] for p in pts); xmax = max(p[0] for p in pts)
            ymin = min(p[1] for p in pts); ymax = max(p[1] for p in pts)
        else:
            xmin = ymin = xmax = ymax = 0
        e = -1
        for c in contours:
            e += len(c)
            blob += struct.pack("<H", e)
        for (x, y, on) in pts:
            blob += struct.pack("<hh", x, y)
        bits = bytearray((len(pts) + 7) // 8)
        for i, p in enumerate(pts):
            if p[2]:
                bits[i >> 3] |= 1 << (i & 7)
        blob += bits
        if len(blob) & 1:
            blob += b"\0"
        table += struct.pack("<BBHhhhhHHI", b, 0, max(0, adv), xmin, ymin, xmax, ymax,
                             len(contours), len(pts), off)
    ktab = bytearray()
    for (b1, b2) in sorted(kern):
        ktab += struct.pack("<BBh", b1, b2, kern[(b1, b2)])

    hdr_size = 40
    gt = hdr_size
    pb = gt + len(table)
    kt = pb + len(blob)
    total = kt + len(ktab)
    hdr = b"BFNT" + struct.pack("<HHhhhhhHHHIIII", 1, f.upm, f.asc, f.desc, f.gap, f.cap, f.xh,
                                len(glyphs), len(kern), 1 if mono else 0, gt, pb, kt, total)
    assert len(hdr) == hdr_size, len(hdr)
    return hdr + table + blob + ktab, f, glyphs, kern


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    src, out = argv[1], argv[2]
    opts = dict(a.split("=", 1) for a in argv[3:])
    data, f, glyphs, kern = build(src, float(opts["wght"]) if "wght" in opts else None,
                                  float(opts["opsz"]) if "opsz" in opts else None)
    open(out, "wb").write(data)
    print("      %s: %d glyphs, %d kern pairs, %d bytes"
          % (out.replace("\\", "/").split("/")[-1], len(glyphs), len(kern), len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
