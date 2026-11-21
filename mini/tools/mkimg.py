#!/usr/bin/env python3
"""Turn a PNG into an image BoltOS Mini can draw straight from the kernel.

    mkimg.py IN.png OUT.img [--key RRGGBB] [--fade N]

OUT.img is "BIMG", the width and the height (little-endian u32 each), then
width * height pixels of premultiplied 0xAARRGGBB, the form gfx_image()
takes. It goes into the kernel with llvm-objcopy, the way the fonts do.

--key takes a picture drawn on a flat background of that colour and makes
the background transparent: each pixel gets the least alpha that explains
it as some colour over the key colour, so anti-aliased edges and soft glows
keep their shape over whatever the image is drawn on.

--fade fades the outer N pixels to transparent, so lines that run off the
picture's edge trail away instead of stopping at a straight cut.

Only the standard library: 8-bit greyscale, RGB, grey+alpha and RGBA PNGs,
not interlaced, which is what every editor saves.
"""
import argparse, struct, sys, zlib


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("%s isn't a PNG" % path)
    pos, idat, w = 8, [], None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (0, 2, 4, 6) or inter:
                raise SystemExit("%s: only 8-bit, non-interlaced grey/RGB/RGBA PNGs" % path)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if w is None:
        raise SystemExit("%s has no IHDR" % path)
    bpp = {0: 1, 2: 3, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    rows, prev, p = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    px = []
    for line in rows:
        for x in range(w):
            q = line[x * bpp:(x + 1) * bpp]
            if ctype == 0:
                px.append((q[0], q[0], q[0], 255))
            elif ctype == 2:
                px.append((q[0], q[1], q[2], 255))
            elif ctype == 4:
                px.append((q[0], q[0], q[0], q[1]))
            else:
                px.append((q[0], q[1], q[2], q[3]))
    return w, h, px


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--key", help="background colour to make transparent, RRGGBB")
    ap.add_argument("--fade", type=int, default=0, help="fade the outer N pixels out")
    a = ap.parse_args(argv)
    w, h, px = read_png(a.src)
    key = None
    if a.key:
        k = int(a.key, 16)
        key = ((k >> 16) & 255, (k >> 8) & 255, k & 255)
    out = bytearray(b"BIMG" + struct.pack("<II", w, h))
    for i, (r, g, b, al) in enumerate(px):
        c = (r, g, b)
        if a.fade > 0:
            x, y = i % w, i // w
            e = min(x + 0.5, y + 0.5, w - x - 0.5, h - y - 0.5) / a.fade
            if e < 1:
                al = al * e * e * (3 - 2 * e)          # smoothstep to 0 at the edge
        if key:
            # The least alpha with which c is some colour over the key.
            t = 0.0
            for ch, kc in zip(c, key):
                if ch > kc:
                    t = max(t, (ch - kc) / (255 - kc))
                elif ch < kc:
                    t = max(t, (kc - ch) / kc)
            pre = [min(max(ch - kc * (1 - t), 0.0), 255 * t) for ch, kc in zip(c, key)]
            t *= al / 255
            pre = [v * al / 255 for v in pre]
        else:
            t = al / 255
            pre = [ch * t for ch in c]
        A = int(round(t * 255))
        R, G, B = (min(int(round(v)), A) for v in pre)
        out += struct.pack("<I", (A << 24) | (R << 16) | (G << 8) | B)
    open(a.out, "wb").write(out)


if __name__ == "__main__":
    main(sys.argv[1:])
