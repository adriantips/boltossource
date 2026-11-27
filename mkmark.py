#!/usr/bin/env python3
"""Put the BoltOS mark -- the logo on boltos.dev -- into the icon atlases.

    mkmark.py

The mark is a bolt through a broken ring, from the website's own path data
(website/src/components/site/Logo.tsx). The kernels draw it themselves with
kernel/boltmark.c; this script draws the two places it lives as a picture:

  * the desktop's ICON_START, in every assets/icons/apps-*.png: the Start
    button, Settings > About, System Info. It is the website's favicon as a
    BoltOS tile -- the black squircle with the white mark -- in the house
    style of mkicons.py (its drop shadow, sheen and rim).
  * BoltOS TV's GL_BOLT glyph, cell 0 of tv/art/glyphs-32.png and -64.png.

Only those cells are replaced; every other icon stays byte for byte what
mkicons.py and tv/mkart.py drew. Both of those draw the same mark now too,
so a full re-run of either agrees with this.

The shape is rasterised the way kernel/boltmark.c does it -- signed
distances -- at four times the size and box-filtered down.
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

ROOT = os.path.dirname(os.path.abspath(__file__))

# ---- the mark, in the website's viewBox units (12..88) -------------------------
BOLT = [(63.2, 18.2), (36.5, 52.0), (48.2, 52.0), (37.8, 78.0), (63.6, 44.8), (51.4, 44.8)]
ARCS = [(49.948, 48.581, -59.855, 158.672), (49.906, 48.557, 118.361, 159.838)]   # cx, cy, start, sweep (deg)
RING_R = 23.8
RING_C = (49.93, 48.57)          # where the ring is centred: what icons centre on


def mark_coverage(n, scale, ox, oy, stroke):
    """Coverage of an n x n grid where mark unit (u, v) lands on pixel
    ((u - ox) * scale, (v - oy) * scale)."""
    pix = (np.arange(n) + 0.5) / scale
    u = pix[None, :] + ox
    v = pix[:, None] + oy
    u, v = np.broadcast_arrays(u, v)
    # the bolt: distance to its edges, sign from crossings
    d2 = np.full(u.shape, 1e18)
    inside = np.zeros(u.shape, bool)
    for i in range(6):
        ax, ay = BOLT[i]
        bx, by = BOLT[(i + 1) % 6]
        ex, ey = bx - ax, by - ay
        wx, wy = u - ax, v - ay
        t = np.clip((wx * ex + wy * ey) / (ex * ex + ey * ey), 0, 1)
        d2 = np.minimum(d2, (wx - ex * t) ** 2 + (wy - ey * t) ** 2)
        with np.errstate(divide="ignore", invalid="ignore"):
            crosses = ((ay > v) != (by > v)) & (u < ax + (v - ay) * ex / ey)
        inside ^= crosses
    d = np.where(inside, -np.sqrt(d2), np.sqrt(d2))
    # the arcs: the ring, cut to each arc's wedge
    hw = stroke / 2
    for cx, cy, a0, sweep in ARCS:
        dx, dy = u - cx, v - cy
        rad = np.abs(np.hypot(dx, dy) - RING_R) - hw
        t = (np.degrees(np.arctan2(dy, dx)) - a0) % 360.0
        m = np.where(t <= sweep, np.minimum(t, sweep - t), -np.minimum(t - sweep, 360.0 - t))
        da = -m * math.pi / 180.0 * RING_R
        d = np.minimum(d, np.maximum(rad, da))
    k = 1.0 / scale
    return np.clip(0.5 - d / k, 0, 1)


def mark_image(px, box, cx, cy, stroke, color=(255, 255, 255), ss=4):
    """An RGBA px x px image with the mark's ring centred at (cx, cy) (pixels),
    its 76-unit box `box` px across."""
    n = px * ss
    scale = box / 76.0 * ss
    ox = RING_C[0] - cx * ss / scale
    oy = RING_C[1] - cy * ss / scale
    cov = mark_coverage(n, scale, ox, oy, stroke)
    a = Image.fromarray((cov * 255 + 0.5).astype(np.uint8), "L").reduce(ss)
    out = Image.new("RGBA", (px, px), color + (0,))
    out.putalpha(a)
    return out


# ---- the desktop tile, in mkicons.py's house style -------------------------------
def squircle(cx=64.0, cy=64.0, a=56.0, n=4.6, pts=360):
    out = []
    for i in range(pts):
        t = 2 * math.pi * i / pts
        c, s = math.cos(t), math.sin(t)
        out.append((cx + a * math.copysign(abs(c) ** (2 / n), c), cy + a * math.copysign(abs(s) ** (2 / n), s)))
    return out


def vgrad(w, h, stops):
    """A w x h float array of alpha (or value) from (offset, value) stops, top to bottom."""
    ys = (np.arange(h) + 0.5) / h
    offs = [o for o, _ in stops]
    vals = [v for _, v in stops]
    col = np.interp(ys, offs, vals)
    return np.repeat(col[:, None], w, axis=1)


def start_tile(size):
    from PIL import ImageDraw
    ss = 8
    N = 128 * ss
    k = ss

    def mask(points, dy=0.0):
        m = Image.new("L", (N, N), 0)
        ImageDraw.Draw(m).polygon([(x * k, (y + dy) * k) for x, y in points], fill=255)
        return m

    sq = squircle()
    sqm = mask(sq)
    canvas = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    # drop shadow: dy 2.6, blur 2.4, 30% black
    sh = mask(sq, 2.6).filter(ImageFilter.GaussianBlur(2.4 * k))
    shadow = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    shadow.putalpha(sh.point(lambda a: int(a * 0.30)))
    canvas.alpha_composite(shadow)
    # the tile: near black, a touch lighter at the top, like the favicon
    top, bot = np.array([0x2C, 0x2D, 0x33], float), np.array([0x06, 0x06, 0x08], float)
    g = vgrad(1, N, [(0.0, 0.0), (1.0, 1.0)])[:, 0]
    rgb = (top[None, :] * (1 - g[:, None]) + bot[None, :] * g[:, None]).astype(np.uint8)
    body = Image.fromarray(np.repeat(rgb[:, None, :], N, axis=1), "RGB").convert("RGBA")
    body.putalpha(sqm)
    canvas.alpha_composite(body)
    # sheen over the top half (mkicons' #sheen over the squircle's box, y 8..120)
    sheen_a = np.zeros((N, N))
    y0, y1 = 8 * k, 120 * k
    sheen_a[y0:y1, :] = vgrad(N, y1 - y0, [(0, .30), (.46, .05), (.5, 0), (1, 0)])
    sheen = Image.new("RGBA", (N, N), (255, 255, 255, 0))
    sa = Image.fromarray((sheen_a * 255).astype(np.uint8), "L")
    sheen.putalpha(Image.fromarray((np.asarray(sa, float) * np.asarray(sqm, float) / 255).astype(np.uint8), "L"))
    # the mark: white, the ring centred on the tile, a soft shadow under it
    box_units = 98.0                        # the mark's 76-unit box, in tile units
    px_box = box_units / 128.0 * size       # what that is at this icon size
    stroke = max(3.6, 1.05 * 76.0 / px_box)
    mk = mark_image(N, box_units * k, 64 * k, 64.5 * k, stroke, ss=1)
    ma = mk.getchannel("A")
    gs = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    gs_a = ma.transform(ma.size, Image.AFFINE, (1, 0, 0, 0, 1, -1.6 * k)).filter(ImageFilter.GaussianBlur(1.6 * k))
    gs.putalpha(gs_a.point(lambda a: int(a * 0.30)))
    canvas.alpha_composite(gs)
    canvas.alpha_composite(sheen)
    canvas.alpha_composite(mk)
    # the glass rim
    from PIL import ImageDraw as D
    rim_m = Image.new("L", (N, N), 0)
    D.Draw(rim_m).line([(x * k, y * k) for x, y in sq + sq[:1]], fill=255, width=int(2.2 * k), joint="curve")
    rim_m = Image.fromarray((np.asarray(rim_m, float) * np.asarray(sqm, float) / 255).astype(np.uint8), "L")
    ra = np.zeros((N, N))
    ra[y0:y1, :] = vgrad(N, y1 - y0, [(0, .62), (.35, .10), (.8, .06), (1, .30)])
    ra[:y0, :] = .62
    ra[y1:, :] = .30
    rim = Image.new("RGBA", (N, N), (255, 255, 255, 0))
    rim.putalpha(Image.fromarray((ra * np.asarray(rim_m, float)).astype(np.uint8), "L"))
    canvas.alpha_composite(rim)
    return canvas.resize((size, size), Image.LANCZOS)


def icon_index(name):
    src = open(os.path.join(ROOT, "include", "gui.h"), encoding="utf-8").read()
    import re
    m = re.search(r"enum\s*\{\s*ICON_NONE\s*=\s*0\s*,(.*?)\}", src, re.S)
    names = [n.strip() for n in m.group(1).replace("\n", " ").split(",") if n.strip()]
    names = [n for n in names if n.startswith("ICON_") and n != "ICON_COUNT"]
    return 1 + names.index(name)          # index 0 is ICON_NONE


def main():
    cols = 8                               # mkicons.py COLS
    idx = icon_index("ICON_START")
    for s in (16, 20, 24, 32, 40, 48, 64, 96):
        p = os.path.join(ROOT, "assets", "icons", "apps-%d.png" % s)
        atlas = Image.open(p).convert("RGBA")
        x, y = (idx % cols) * s, (idx // cols) * s
        atlas.paste(Image.new("RGBA", (s, s), (0, 0, 0, 0)), (x, y))
        atlas.paste(start_tile(s), (x, y))
        atlas.save(p, optimize=True)
        print("      apps-%d.png: ICON_START" % s)
    for s in (32, 64):
        p = os.path.join(ROOT, "tv", "art", "glyphs-%d.png" % s)
        atlas = Image.open(p).convert("RGBA")
        stroke = max(3.6, 1.1 * 76.0 / s)
        g = mark_image(s, s * 76.0 / 70.0, s / 2, s / 2 + s * 0.02, stroke)
        atlas.paste(Image.new("RGBA", (s, s), (0, 0, 0, 0)), (0, 0))
        atlas.paste(g, (0, 0))
        atlas.save(p, optimize=True)
        print("      tv/art/glyphs-%d.png: GL_BOLT" % s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
