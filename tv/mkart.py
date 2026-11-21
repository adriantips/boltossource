#!/usr/bin/env python3
"""Draw BoltOS TV's app tiles and UI glyphs.

    mkart.py [OUTDIR]          (default tv/art)

The desktop's mkicons.py has Chrome rasterise SVG; this has no such need.
Glyphs are Lucide's (ISC, see assets/icons/LICENSE-lucide.txt), read out of
the copy the website already depends on (website/node_modules/lucide-react),
flattened to polylines here and stroked with round caps and joins at eight
times the final size, then filtered down. Tiles are drawn the same way: a
squircle, a gradient, a sheen along the top, and a white glyph.

Outputs, all committed so a build needs neither Node nor this script:

    tiles-96.png  tiles-192.png    ART_* in tv/tv.h, left to right
    glyphs-32.png glyphs-64.png    GL_* in tv/tv.h, left to right, white
"""
import math
import os
import re
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LUCIDE = os.path.join(ROOT, "website", "node_modules", "lucide-react", "dist", "esm", "icons")
SS = 8

# ---------------------------------------------------------------- Lucide ----
def lucide_nodes(name):
    path = os.path.join(LUCIDE, name + ".mjs")
    src = open(path, encoding="utf-8").read()
    alias = re.search(r"export \{ default \} from '\./([\w-]+)\.mjs'", src)
    if alias:
        return lucide_nodes(alias.group(1))
    nodes = []
    for tag, attrs in re.findall(r'\[\s*"(\w+)",\s*\{([^}]*)\}\s*\]', src):
        nodes.append((tag, dict(re.findall(r'(\w+):\s*"([^"]*)"', attrs))))
    if not nodes:
        raise SystemExit("no icon nodes in " + path)
    return nodes


def _num_tokens(s):
    return [float(t) for t in re.findall(r"-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?", s)]


def _arc(p0, rx, ry, phi, large, sweep, p1):
    """SVG elliptical arc -> points (endpoint parameterisation, F.6.5)."""
    x1, y1 = p0
    x2, y2 = p1
    if rx == 0 or ry == 0:
        return [p1]
    phi = math.radians(phi)
    cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    rx, ry = abs(rx), abs(ry)
    lam = (x1p ** 2) / (rx ** 2) + (y1p ** 2) / (ry ** 2)
    if lam > 1:
        rx *= math.sqrt(lam)
        ry *= math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    co = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        co = -co
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x1 + x2) / 2
    cy = sp * cxp + cp * cyp + (y1 + y2) / 2

    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a
    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    n = max(4, int(abs(dt) / (math.pi / 32)))
    pts = []
    for i in range(1, n + 1):
        t = t1 + dt * i / n
        pts.append((cx + rx * math.cos(t) * cp - ry * math.sin(t) * sp,
                    cy + rx * math.cos(t) * sp + ry * math.sin(t) * cp))
    return pts


def _bez(p0, c1, c2, p3, n=24):
    out = []
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        out.append((u ** 3 * p0[0] + 3 * u * u * t * c1[0] + 3 * u * t * t * c2[0] + t ** 3 * p3[0],
                    u ** 3 * p0[1] + 3 * u * u * t * c1[1] + 3 * u * t * t * c2[1] + t ** 3 * p3[1]))
    return out


NUM = re.compile(r"[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?")


def parse_path(d):
    """SVG path data -> list of (points, closed).

    A scanner rather than a tokenizer, because arc flags are single
    characters that minified paths run straight into the next number:
    "a2 2 0 011.66.9" is flags 0 and 1 followed by 1.66 and .9."""
    pos = [0]
    n = len(d)

    def skip():
        while pos[0] < n and d[pos[0]] in " ,\t\n\r":
            pos[0] += 1

    def number():
        skip()
        m = NUM.match(d, pos[0])
        if not m:
            raise SystemExit("bad number in path at %d: %r" % (pos[0], d))
        pos[0] = m.end()
        return float(m.group(0))

    def flag():
        skip()
        c = d[pos[0]]
        pos[0] += 1
        return 1 if c == "1" else 0

    def more():
        skip()
        return pos[0] < n and (d[pos[0]].isdigit() or d[pos[0]] in "+-.")

    cmd = None
    cur = start = (0.0, 0.0)
    last_c = None
    subs, pts = [], []

    def flush(closed=False):
        nonlocal pts
        if len(pts) > 1:
            subs.append((pts, closed))
        pts = []

    while True:
        skip()
        if pos[0] >= n:
            break
        if d[pos[0]].isalpha():
            cmd = d[pos[0]]
            pos[0] += 1
            if cmd in "Zz":
                if pts:
                    pts.append(start)
                flush(True)
                cur = start
                continue
        elif cmd is None:
            raise SystemExit("path starts without a command: %r" % d)
        rel = cmd.islower()
        c = cmd.upper()
        ox, oy = cur if rel else (0.0, 0.0)
        if c == "M":
            x, y = number(), number()
            flush()
            cur = start = (ox + x, oy + y)
            pts = [cur]
            cmd = "l" if rel else "L"
            last_c = None
        elif c == "L":
            x, y = number(), number()
            cur = (ox + x, oy + y); pts.append(cur); last_c = None
        elif c == "H":
            x = number()
            cur = ((cur[0] + x) if rel else x, cur[1]); pts.append(cur); last_c = None
        elif c == "V":
            y = number()
            cur = (cur[0], (cur[1] + y) if rel else y); pts.append(cur); last_c = None
        elif c == "C":
            a = [number() for _ in range(6)]
            c1 = (ox + a[0], oy + a[1]); c2 = (ox + a[2], oy + a[3]); p = (ox + a[4], oy + a[5])
            pts += _bez(cur, c1, c2, p); cur = p; last_c = c2
        elif c == "S":
            a = [number() for _ in range(4)]
            c1 = (2 * cur[0] - last_c[0], 2 * cur[1] - last_c[1]) if last_c else cur
            c2 = (ox + a[0], oy + a[1]); p = (ox + a[2], oy + a[3])
            pts += _bez(cur, c1, c2, p); cur = p; last_c = c2
        elif c == "Q":
            a = [number() for _ in range(4)]
            q = (ox + a[0], oy + a[1]); p = (ox + a[2], oy + a[3])
            c1 = (cur[0] + 2 / 3 * (q[0] - cur[0]), cur[1] + 2 / 3 * (q[1] - cur[1]))
            c2 = (p[0] + 2 / 3 * (q[0] - p[0]), p[1] + 2 / 3 * (q[1] - p[1]))
            pts += _bez(cur, c1, c2, p); cur = p; last_c = q
        elif c == "T":
            a = [number() for _ in range(2)]
            q = (2 * cur[0] - last_c[0], 2 * cur[1] - last_c[1]) if last_c else cur
            p = (ox + a[0], oy + a[1])
            c1 = (cur[0] + 2 / 3 * (q[0] - cur[0]), cur[1] + 2 / 3 * (q[1] - cur[1]))
            c2 = (p[0] + 2 / 3 * (q[0] - p[0]), p[1] + 2 / 3 * (q[1] - p[1]))
            pts += _bez(cur, c1, c2, p); cur = p; last_c = q
        elif c == "A":
            rx, ry, rot = number(), number(), number()
            large, sweep = flag(), flag()
            x, y = number(), number()
            p = (ox + x, oy + y)
            pts += _arc(cur, rx, ry, rot, large, sweep, p)
            cur = p; last_c = None
        else:
            raise SystemExit("path command %r" % cmd)
    flush()
    return subs


def shapes(name):
    out = []
    for tag, a in lucide_nodes(name):
        f = lambda k, d=0.0: float(a.get(k, d))
        if tag == "path":
            out += parse_path(a["d"])
        elif tag == "circle":
            cx, cy, r = f("cx"), f("cy"), f("r")
            out.append(([(cx + r * math.cos(2 * math.pi * k / 96), cy + r * math.sin(2 * math.pi * k / 96))
                         for k in range(97)], True))
        elif tag == "ellipse":
            cx, cy, rx, ry = f("cx"), f("cy"), f("rx"), f("ry")
            out.append(([(cx + rx * math.cos(2 * math.pi * k / 96), cy + ry * math.sin(2 * math.pi * k / 96))
                         for k in range(97)], True))
        elif tag == "line":
            out.append(([(f("x1"), f("y1")), (f("x2"), f("y2"))], False))
        elif tag in ("polyline", "polygon"):
            v = _num_tokens(a["points"])
            p = list(zip(v[0::2], v[1::2]))
            if tag == "polygon":
                p.append(p[0])
            out.append((p, tag == "polygon"))
        elif tag == "rect":
            x, y, w, h = f("x"), f("y"), f("width"), f("height")
            r = min(f("rx", f("ry")), w / 2, h / 2)
            if r <= 0:
                out.append(([(x, y), (x + w, y), (x + w, y + h), (x, y + h), (x, y)], True))
            else:
                p = []
                for (ccx, ccy, a0) in ((x + w - r, y + r, -90), (x + w - r, y + h - r, 0),
                                       (x + r, y + h - r, 90), (x + r, y + r, 180)):
                    for k in range(13):
                        t = math.radians(a0 + 90 * k / 12)
                        p.append((ccx + r * math.cos(t), ccy + r * math.sin(t)))
                p.append(p[0])
                out.append((p, True))
        else:
            raise SystemExit("lucide element %r in %s" % (tag, name))
    return out


def stroke_glyph(name, size, sw=2.0, fill=False, inset=0.0):
    """White glyph on transparent, `size` px. Lucide's 24-unit box, inset by
    `inset` units on every side (tiles want the glyph smaller than the box)."""
    big = size * SS
    m = Image.new("L", (big, big), 0)
    d = ImageDraw.Draw(m)
    span = 24.0 + 2 * inset
    k = big / span
    w = max(1, int(round(sw * k)))
    for pts, closed in shapes(name):
        P = [((x + inset) * k, (y + inset) * k) for x, y in pts]
        if fill and closed and len(P) > 2:
            d.polygon(P, fill=255)
        d.line(P, fill=255, width=w, joint="curve")
        r = w / 2
        for (x, y) in (P if not closed else P[:1]) + [P[-1]]:
            d.ellipse((x - r, y - r, x + r, y + r), fill=255)
        for (x, y) in P:
            d.ellipse((x - r, y - r, x + r, y + r), fill=255)
    m = m.resize((size, size), Image.LANCZOS)
    out = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    out.putalpha(m)
    return out


# -------------------------------------------------------------- the bolt ----
def bolt_glyph(size):
    """BoltOS's own mark: a filled lightning bolt, a little italic."""
    big = size * SS
    m = Image.new("L", (big, big), 0)
    d = ImageDraw.Draw(m)
    pts = [(13.6, 1.5), (4.4, 13.4), (11.0, 13.4), (9.6, 22.5), (19.6, 9.8), (12.9, 9.8), (13.6, 1.5)]
    k = big / 24.0
    d.polygon([(x * k, y * k) for x, y in pts], fill=255)
    m = m.filter(ImageFilter.GaussianBlur(k * 0.12)).resize((size, size), Image.LANCZOS)
    out = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    out.putalpha(m)
    return out


# ----------------------------------------------------------------- tiles ----
def squircle_mask(n, exp=5.0):
    big = n * SS
    m = Image.new("L", (big, big), 0)
    px = m.load()
    half = big / 2
    # Row spans of |x|^e + |y|^e <= 1, filled as lines: quick and exact enough.
    d = ImageDraw.Draw(m)
    for yy in range(big):
        y = abs((yy + 0.5 - half) / half)
        if y > 1:
            continue
        x = (1 - y ** exp) ** (1 / exp)
        d.line([(half - x * half, yy), (half + x * half, yy)], fill=255)
    return m.resize((n, n), Image.LANCZOS)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def hexrgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def tile(size, top, bottom, glyph, glyph_scale=0.52, sw=1.9, fill=False, custom=None):
    top, bottom = hexrgb(top), hexrgb(bottom)
    grad = Image.new("RGB", (1, size))
    for y in range(size):
        grad.putpixel((0, y), lerp(top, bottom, y / (size - 1)))
    body = grad.resize((size, size)).convert("RGBA")
    # A sheen across the top third and a darker foot, which is what makes the
    # flat gradient read as a lit object rather than a swatch.
    sheen = Image.new("L", (size, size), 0)
    sd = ImageDraw.Draw(sheen)
    for y in range(size):
        t = y / size
        a = int(max(0.0, 0.32 - t * 0.9) * 255)
        sd.line([(0, y), (size, y)], fill=a)
    white = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    white.putalpha(sheen)
    body = Image.alpha_composite(body, white)

    gsize = int(size * glyph_scale)
    g = custom(gsize) if custom else stroke_glyph(glyph, gsize, sw=sw, fill=fill)
    # soft drop shadow under the glyph
    sh = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ga = g.split()[3]
    shadow = Image.new("RGBA", g.size, (0, 0, 0, 0))
    shadow.putalpha(ga.point(lambda v: v * 0.35))
    off = (size - gsize) // 2
    sh.paste(shadow, (off, off + max(1, size // 64)), shadow)
    sh = sh.filter(ImageFilter.GaussianBlur(max(1, size / 80)))
    body = Image.alpha_composite(body, sh)
    body.alpha_composite(g, (off, off))

    mask = squircle_mask(size)
    # A fine light rim just inside the edge.
    rim = ImageChops.subtract(mask, mask.filter(ImageFilter.MinFilter(3 if size >= 128 else 3)))
    rimc = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    rimc.putalpha(rim.point(lambda v: int(v * 0.28)))
    body = Image.alpha_composite(body, rimc)
    body.putalpha(ImageChops.multiply(body.split()[3], mask))
    return body


def appletv_box(gsize):
    """A set-top box with a remote beside it: generic, and nobody's logo."""
    big = gsize * SS
    m = Image.new("L", (big, big), 0)
    d = ImageDraw.Draw(m)
    k = big / 24.0
    w = int(1.9 * k)
    d.rounded_rectangle((2.5 * k, 8.0 * k, 16.5 * k, 16.5 * k), radius=2.2 * k, outline=255, width=w)
    d.line((5.5 * k, 19.5 * k, 13.5 * k, 19.5 * k), fill=255, width=w)
    for x, y in ((5.5, 19.5), (13.5, 19.5)):
        r = w / 2
        d.ellipse((x * k - r, y * k - r, x * k + r, y * k + r), fill=255)
    d.rounded_rectangle((18.4 * k, 5.0 * k, 22.1 * k, 19.5 * k), radius=1.85 * k, fill=255)
    r = 0.95 * k
    d.ellipse((20.25 * k - r, 8.4 * k - r, 20.25 * k + r, 8.4 * k + r), fill=0)
    d.rounded_rectangle((19.6 * k, 12.2 * k, 20.9 * k, 15.6 * k), radius=0.6 * k, fill=0)
    m = m.resize((gsize, gsize), Image.LANCZOS)
    out = Image.new("RGBA", (gsize, gsize), (255, 255, 255, 0))
    out.putalpha(m)
    return out


# ART_* order in tv/tv.h
TILES = [
    ("live",     "#FF6A5B", "#E2366B", "tv",               0.52, 1.9, False, None),
    ("appletv",  "#4A4A50", "#17171A", None,               0.60, 1.9, False, appletv_box),
    ("hdmi",     "#3FA2FF", "#2257E6", "hdmi-port",        0.54, 1.9, False, None),
    ("web",      "#7C6CFF", "#4B2FD8", "globe",            0.54, 1.8, False, None),
    ("shield",   "#3DDC84", "#0E9F7E", "shield-check",     0.54, 1.9, False, None),
    ("settings", "#9A9AA2", "#5B5B63", "settings",         0.56, 1.8, False, None),
    # The streaming apps: each service's colour and a plain glyph -- nobody's logo.
    ("youtube",  "#FF4E45", "#D10F0F", "play",             0.46, 2.1, True,  None),
    ("netflix",  "#3A0A0D", "#0B0B0D", "clapperboard",     0.52, 1.9, False, None),
    ("prime",    "#2BB3FF", "#0F68D6", "monitor-play",     0.54, 1.8, False, None),
    ("disney",   "#3C4FC4", "#121A5E", "sparkles",         0.54, 1.8, False, None),
    ("hbomax",   "#8A3FFC", "#3B0FA8", "popcorn",          0.54, 1.8, False, None),
]

# GL_* order in tv/tv.h
GLYPHS = [
    ("bolt", None), ("chevron-right", None), ("chevron-left", None), ("chevron-up", None),
    ("chevron-down", None), ("volume-2", None), ("volume-x", None), ("wifi", None),
    ("ethernet-port", None), ("globe", None), ("shield-check", None), ("shield-off", None),
    ("search", None), ("arrow-left", None), ("rotate-cw", None), ("lock", None), ("x", None),
    ("power", None), ("moon", None), ("sun", None), ("image", None), ("info", None),
    ("tv", None), ("film", None), ("check", None), ("play", None), ("unplug", None),
    ("clock", None), ("house", None), ("keyboard", None), ("star", None), ("list", None),
    ("cpu", None), ("rotate-ccw", None),
]


def strip(images, size):
    out = Image.new("RGBA", (size * len(images), size), (0, 0, 0, 0))
    for i, im in enumerate(images):
        out.paste(im, (i * size, 0))
    return out


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tv", "art")
    os.makedirs(out_dir, exist_ok=True)
    for size in (96, 192):
        ims = [tile(size, t, b, g, sc, sw, fl, cu) for _, t, b, g, sc, sw, fl, cu in TILES]
        strip(ims, size).save(os.path.join(out_dir, "tiles-%d.png" % size), optimize=True)
    for size in (32, 64):
        ims = []
        for name, _ in GLYPHS:
            if name == "bolt":
                ims.append(bolt_glyph(size))
            else:
                ims.append(stroke_glyph(name, size, sw=2.0, inset=1.0))
        strip(ims, size).save(os.path.join(out_dir, "glyphs-%d.png" % size), optimize=True)
    print("OK -> %s (%d tiles, %d glyphs)" % (out_dir, len(TILES), len(GLYPHS)))


if __name__ == "__main__":
    main()
