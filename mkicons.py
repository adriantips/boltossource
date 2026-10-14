#!/usr/bin/env python3
"""Draw BoltOS's icons and bake them into the atlases the kernel embeds.

    mkicons.py [OUTDIR]          (default assets/icons)

Two families come out of this:

  * App and system icons, full colour. Every one is drawn here as SVG --
    the tile shape, the gradients, the glyph -- so the whole set shares one
    light source, one corner shape and one sheen, which is what makes a set
    of icons look like a set. Chrome rasterises them (it is the best SVG
    renderer on this machine by a distance) into one atlas per size.
  * UI glyphs: the monochrome symbols in the tray, the menus and the window
    controls. These are Lucide's (ISC licence, see LICENSE-lucide.txt next to
    the atlases), lifted from the copy the website already depends on, drawn
    white so the kernel can tint the alpha to any colour.

The order of the app atlas is the ICON_* enum in include/gui.h -- parsed out of
it, the way mkwall.py reads settings.c -- so an icon added there without a
drawing here fails loudly instead of showing the wrong picture. The glyph
order is written to include/glyphs.h.

Run it again after changing a drawing; the atlases are committed, so a normal
build needs no Chrome.
"""
import base64
import math
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ROOT)
from mkuifont import Font  # noqa: E402

APP_SIZES = [16, 24, 32, 48, 64, 96]
GLYPH_SIZES = [16, 20, 24]
COLS = 8
CHROME_CANDIDATES = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]
LUCIDE = os.path.join(ROOT, "website", "node_modules", "lucide-react", "dist", "esm", "icons")

# ---------------------------------------------------------------------------
#  Shapes
# ---------------------------------------------------------------------------
def squircle(cx=64.0, cy=64.0, a=56.0, n=4.6, pts=144):
    out = []
    for i in range(pts):
        t = 2 * math.pi * i / pts
        c, s = math.cos(t), math.sin(t)
        x = cx + a * math.copysign(abs(c) ** (2 / n), c)
        y = cy + a * math.copysign(abs(s) ** (2 / n), s)
        out.append("%.2f %.2f" % (x, y))
    return "M" + " L".join(out) + " Z"


SQ = squircle()


def gear(cx, cy, R, r, teeth, hole, tw=0.42, vw=0.62):
    pts = []
    for i in range(teeth):
        a0 = 2 * math.pi * i / teeth
        step = 2 * math.pi / teeth
        for (rad, ang) in ((r, a0 - step * vw / 2), (R, a0 - step * tw / 2),
                           (R, a0 + step * tw / 2), (r, a0 + step * vw / 2)):
            pts.append((cx + rad * math.sin(ang), cy - rad * math.cos(ang)))
        # valley arc to the next tooth
        for k in range(1, 4):
            ang = a0 + step * vw / 2 + (step * (1 - vw)) * k / 4
            pts.append((cx + r * math.sin(ang), cy - r * math.cos(ang)))
    d = "M" + " L".join("%.2f %.2f" % p for p in pts) + " Z"
    if hole:
        d += " M%.2f %.2f a%.2f %.2f 0 1 0 %.2f 0 a%.2f %.2f 0 1 0 %.2f 0 Z" % (
            cx - hole, cy, hole, hole, 2 * hole, hole, hole, -2 * hole)
    return d


_fonts = {}


def text_path(s, size, x, y, weight=700, anchor="middle", opsz=None):
    """Inter outlines as an SVG path: no font has to exist where this is
    rendered, and the result matches the UI exactly."""
    key = (weight, opsz)
    if key not in _fonts:
        f = Font(open(os.path.join(ROOT, "assets", "fonts", "Inter.ttf"), "rb").read())
        user = {"wght": weight}
        if opsz:
            user["opsz"] = opsz
        f.set_axes(user)
        _fonts[key] = f
    f = _fonts[key]
    sc = size / f.upm
    glyphs, pen = [], 0.0
    for ch in s:
        g = f.cmap.get(ord(ch))
        if g is None:
            continue
        contours, adv = f.outline(g)
        glyphs.append((pen, contours))
        pen += adv
    width = pen * sc
    ox = x - (width / 2 if anchor == "middle" else (width if anchor == "end" else 0))
    d = []
    for (px, contours) in glyphs:
        for c in contours:
            pts = [((px + gx) * sc + ox, y - gy * sc, on) for (gx, gy, on) in c]
            n = len(pts)
            if n < 2:
                continue
            start = next((i for i in range(n) if pts[i][2]), None)
            if start is None:
                sx = (pts[0][0] + pts[1][0]) / 2; sy = (pts[0][1] + pts[1][1]) / 2
                seq = pts[1:] + pts[:1]
            else:
                sx, sy = pts[start][0], pts[start][1]
                seq = pts[start + 1:] + pts[:start + 1]
            d.append("M%.2f %.2f" % (sx, sy))
            ctrl = None
            for (px2, py2, on) in seq:
                if on:
                    if ctrl:
                        d.append("Q%.2f %.2f %.2f %.2f" % (ctrl[0], ctrl[1], px2, py2)); ctrl = None
                    else:
                        d.append("L%.2f %.2f" % (px2, py2))
                else:
                    if ctrl:
                        mx, my = (ctrl[0] + px2) / 2, (ctrl[1] + py2) / 2
                        d.append("Q%.2f %.2f %.2f %.2f" % (ctrl[0], ctrl[1], mx, my))
                    ctrl = (px2, py2)
            if ctrl:
                d.append("Q%.2f %.2f %.2f %.2f" % (ctrl[0], ctrl[1], sx, sy))
            d.append("Z")
    return " ".join(d)


# ---------------------------------------------------------------------------
#  The house style
# ---------------------------------------------------------------------------
DEFS = """
<filter id="ds" x="-25%%" y="-25%%" width="150%%" height="150%%">
  <feDropShadow dx="0" dy="2.6" stdDeviation="2.4" flood-color="#000" flood-opacity=".30"/></filter>
<filter id="gs" x="-25%%" y="-25%%" width="150%%" height="150%%">
  <feDropShadow dx="0" dy="1.6" stdDeviation="1.6" flood-color="#000" flood-opacity=".26"/></filter>
<filter id="soft" x="-25%%" y="-25%%" width="150%%" height="150%%">
  <feDropShadow dx="0" dy="1" stdDeviation="1.2" flood-color="#000" flood-opacity=".18"/></filter>
<linearGradient id="sheen" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#fff" stop-opacity=".30"/>
  <stop offset=".46" stop-color="#fff" stop-opacity=".05"/>
  <stop offset=".5" stop-color="#fff" stop-opacity="0"/></linearGradient>
<linearGradient id="rim" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#fff" stop-opacity=".62"/>
  <stop offset=".35" stop-color="#fff" stop-opacity=".10"/>
  <stop offset=".8" stop-color="#fff" stop-opacity=".06"/>
  <stop offset="1" stop-color="#fff" stop-opacity=".30"/></linearGradient>
<clipPath id="clip"><path d="%(sq)s"/></clipPath>
""" % {"sq": SQ}


def lg(id_, c0, c1, x1=0, y1=0, x2=0, y2=1, stops=None):
    if stops is None:
        stops = [(0, c0), (1, c1)]
    s = "".join('<stop offset="%s" stop-color="%s"/>' % (o, c) for o, c in stops)
    return '<linearGradient id="%s" x1="%s" y1="%s" x2="%s" y2="%s">%s</linearGradient>' % (id_, x1, y1, x2, y2, s)


def rg(id_, stops, cx=.5, cy=.5, r=.5, fx=None, fy=None):
    s = "".join('<stop offset="%s" stop-color="%s" stop-opacity="%s"/>' % st if len(st) == 3 else
                '<stop offset="%s" stop-color="%s"/>' % st for st in stops)
    f = ' fx="%s" fy="%s"' % (fx, fy) if fx is not None else ""
    return '<radialGradient id="%s" cx="%s" cy="%s" r="%s"%s>%s</radialGradient>' % (id_, cx, cy, r, f, s)


def svg(body, defs=""):
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">'
            '<defs>%s%s</defs>%s</svg>' % (DEFS, defs, body))


def tile(c0, c1, content, defs="", angle="v", sheen=True):
    """An app tile: squircle, gradient, sheen and the glass rim, content in
    between so the highlight sits on the glass and not the paint."""
    if angle == "d":
        grad = lg("bg", c0, c1, 0, 0, 1, 1)
    else:
        grad = lg("bg", c0, c1)
    body = ('<g filter="url(#ds)"><path d="%s" fill="url(#bg)"/></g>' % SQ +
            ('<path d="%s" fill="url(#sheen)"/>' % SQ if sheen else "") +
            '<g clip-path="url(#clip)">%s</g>' % content +
            '<path d="%s" fill="none" stroke="url(#rim)" stroke-width="2.2"/>' % SQ)
    return svg(body, grad + defs)


W = "#ffffff"
STROKE = 'fill="none" stroke-linecap="round" stroke-linejoin="round"'


def folder_shape(front0="#FFD969", front1="#F6B12F", back="#E9A322", emb=None, emb_col="#C98612",
                 x=10, y=20, w=108, h=88):
    """A Fluent-style folder: back panel with its tab, front flap with depth."""
    tab_w = w * 0.40
    defs = lg("ff", front0, front1) + lg("fb", "#F3B53B", back)
    body = ('<g filter="url(#ds)">'
            '<path d="M%(x)s %(y2)s q0 -7 7 -7 h%(tw)s q4 0 7 4 l6 7 h%(rw)s q7 0 7 7 v%(bh)s h-%(w)s Z" fill="url(#fb)"/>'
            '<path d="M%(x)s %(fy)s q0 -6 6 -6 h%(fw)s q6 0 6 6 v%(fh)s q0 6 -6 6 h-%(fw)s q-6 0 -6 -6 Z" fill="url(#ff)"/>'
            '</g>'
            '<path d="M%(x6)s %(fy1)s h%(fw)s" stroke="#fff" stroke-opacity=".55" stroke-width="2" stroke-linecap="round"/>'
            ) % {"x": x, "y2": y + 7, "tw": tab_w - 7, "rw": w - tab_w - 20, "bh": h * 0.55, "w": w,
                 "fy": y + h * 0.30, "fw": w - 12, "fh": h * 0.70 - 12, "x6": x + 6, "fy1": y + h * 0.30 - 4.5}
    if emb:
        sc = 2.1
        body += '<g transform="translate(%s %s) scale(%s)" opacity=".9">%s</g>' % (
            x + w / 2 - 12 * sc, y + h * 0.30 + (h * 0.70) / 2 - 12 * sc + 2, sc,
            emb.replace("currentColor", emb_col))
    return defs, body


def page_shape(fold=True, lines=True, badge=None, badge_col="#0A84FF", x=24, y=10, w=80, h=108):
    defs = lg("pg", "#FFFFFF", "#ECEEF3")
    f = 26
    body = ('<g filter="url(#ds)"><path d="M%s %s h%s l%s %s v%s q0 6 -6 6 h-%s q-6 0 -6 -6 v-%s q0 -6 6 -6 Z" '
            'fill="url(#pg)"/></g>') % (x + 6, y, w - f - 6, f, f, h - f - 6, w - 12, h - 12)
    body += ('<path d="M%s %s v%s q0 6 6 6 h%s Z" fill="#D5D9E2"/>' % (x + w - f, y, f - 6, f - 6)) if fold else ""
    body += '<path d="M%s %s h%s l%s %s v%s q0 6 -6 6 h-%s q-6 0 -6 -6 v-%s q0 -6 6 -6 Z" fill="none" stroke="#000" stroke-opacity=".10" stroke-width="1.2"/>' % (
        x + 6, y, w - f - 6, f, f, h - f - 6, w - 12, h - 12)
    if lines:
        for i in range(5):
            ly = y + 44 + i * 11
            body += '<rect x="%s" y="%s" width="%s" height="4" rx="2" fill="#B9C0CE"/>' % (x + 14, ly, (w - 28) * (0.62 if i == 4 else 1))
    if badge:
        body += ('<rect x="%s" y="%s" width="%s" height="26" rx="7" fill="%s"/>' % (x + 8, y + h - 40, w - 16, badge_col) +
                 '<path d="%s" fill="#fff"/>' % text_path(badge, 15, x + w / 2, y + h - 21.5, 700))
    return defs, body


def lucide_inner(name):
    """The element list of a Lucide icon, as SVG, in its own 24-unit space."""
    src = open(os.path.join(LUCIDE, name + ".mjs"), encoding="utf-8").read()
    out = []
    for tag, attrs in re.findall(r'\[\s*"(\w+)",\s*\{([^}]*)\}\s*\]', src):
        kv = dict(re.findall(r'(\w+):\s*"([^"]*)"', attrs))
        kv.pop("key", None)
        a = " ".join('%s="%s"' % (k.replace("_", "-"), v) for k, v in kv.items())
        out.append("<%s %s/>" % (tag, a))
    return "".join(out)


def emboss(name, sw=2.0):
    return ('<g fill="none" stroke="currentColor" stroke-width="%s" stroke-linecap="round" '
            'stroke-linejoin="round">%s</g>' % (sw, lucide_inner(name)))


def lucide_glyph(name, x, y, size, color="#fff", sw=2.2, extra=""):
    s = size / 24.0
    return ('<g transform="translate(%s %s) scale(%s)" fill="none" stroke="%s" stroke-width="%s" '
            'stroke-linecap="round" stroke-linejoin="round" %s>%s</g>' % (x, y, s, color, sw, extra, lucide_inner(name)))


# ---------------------------------------------------------------------------
#  The icons. Keys are ICON_* names without the prefix, lower case.
# ---------------------------------------------------------------------------
I = {}

I["start"] = svg(  # the BoltOS mark: a bolt, no tile
    '<g filter="url(#ds)"><path d="M74 8 L28 70 Q25 75 31 75 L58 75 L50 116 Q49 121 53 117 L100 54 '
    'Q103 49 97 49 L70 49 L78 12 Q79 6 74 8 Z" fill="url(#bolt)"/></g>'
    '<path d="M74 8 L28 70 Q25 75 31 75 L58 75 L50 116 Q49 121 53 117 L100 54 Q103 49 97 49 L70 49 L78 12 Q79 6 74 8 Z" '
    'fill="none" stroke="#fff" stroke-opacity=".45" stroke-width="2"/>',
    lg("bolt", "#6EE7FF", "#7B5CFF", 0, 0, 0.6, 1))

I["terminal"] = tile("#45464D", "#18181B",
    '<rect x="22" y="26" width="84" height="76" rx="10" fill="#0B0B0D" opacity=".55"/>'
    '<g filter="url(#gs)"><path d="M36 48 L54 64 L36 80" stroke="#fff" stroke-width="9" %s/>'
    '<rect x="60" y="76" width="30" height="9" rx="4.5" fill="#5BE37D"/></g>' % STROKE)

I["code"] = tile("#4DA3FF", "#1A56DB",
    '<g filter="url(#gs)" stroke="#fff" stroke-width="9" %s>'
    '<path d="M46 40 L24 64 L46 88"/><path d="M82 40 L104 64 L82 88"/><path d="M72 34 L56 94" stroke-opacity=".75"/></g>' % STROKE)

_fd, _fb = folder_shape(x=22, y=30, w=84, h=70)
I["files"] = tile("#7DC3FF", "#2E7CF6", _fb, _fd)

_fd, _fb = folder_shape()
I["folder"] = svg(_fb, _fd)

_pd, _pb = page_shape()
I["file"] = svg(_pb, _pd)

I["trash"] = svg(
    '<g filter="url(#ds)">'
    '<path d="M28 34 L36 112 Q37 120 45 120 L83 120 Q91 120 92 112 L100 34 Z" fill="url(#bin)"/>'
    '<rect x="20" y="22" width="88" height="14" rx="7" fill="url(#lid)"/>'
    '<path d="M50 22 v-6 q0 -5 5 -5 h18 q5 0 5 5 v6" fill="none" stroke="#8A97AD" stroke-width="5"/></g>'
    '<g stroke="#ffffff" stroke-opacity=".55" stroke-width="4" stroke-linecap="round">'
    '<path d="M48 50 L51 104"/><path d="M64 50 L64 104"/><path d="M80 50 L77 104"/></g>'
    '<path d="M28 34 L100 34" stroke="#fff" stroke-opacity=".7" stroke-width="2"/>',
    lg("bin", "#DCE3EE", "#9AA7BC") + lg("lid", "#F2F5FA", "#B9C4D6"))

I["trash_full"] = svg(
    '<g filter="url(#ds)">'
    '<path d="M28 34 L36 112 Q37 120 45 120 L83 120 Q91 120 92 112 L100 34 Z" fill="url(#bin)"/>'
    '<path d="M34 34 Q40 14 58 20 Q66 6 82 16 Q98 14 96 34 Z" fill="#F7F7F2"/>'
    '<path d="M44 26 L60 30 M70 22 L84 28" stroke="#C9CDD6" stroke-width="3" stroke-linecap="round"/>'
    '<rect x="20" y="30" width="88" height="12" rx="6" fill="url(#lid)"/></g>'
    '<g stroke="#ffffff" stroke-opacity=".55" stroke-width="4" stroke-linecap="round">'
    '<path d="M48 54 L51 104"/><path d="M64 54 L64 104"/><path d="M80 54 L77 104"/></g>',
    lg("bin", "#DCE3EE", "#9AA7BC") + lg("lid", "#F2F5FA", "#B9C4D6"))

I["settings"] = tile("#B4B4BB", "#62626A",
    '<g filter="url(#gs)"><path d="%s" fill="url(#gear)" fill-rule="evenodd"/>'
    '<circle cx="64" cy="64" r="15" fill="none" stroke="#5A5A62" stroke-width="5"/></g>' % gear(64, 64, 44, 35, 12, 15),
    lg("gear", "#FDFDFE", "#D5D5DC"))

I["browser"] = tile("#63D6FF", "#0A6CF0",
    '<g filter="url(#gs)" stroke="#fff" stroke-width="6" fill="none">'
    '<circle cx="64" cy="64" r="34"/><ellipse cx="64" cy="64" rx="14" ry="34"/>'
    '<path d="M31 54 H97 M31 74 H97" stroke-width="5"/></g>'
    '<path d="M18 86 Q64 40 112 34" stroke="#BFF3FF" stroke-width="5" fill="none" stroke-linecap="round" opacity=".9"/>'
    '<circle cx="104" cy="35" r="6" fill="#fff"/>')

I["oldbrowser"] = tile("#D8B07A", "#8C5A2B",
    '<g filter="url(#gs)">'
    '<circle cx="64" cy="58" r="32" fill="url(#brass)"/>'
    '<g stroke="#6B4520" stroke-width="3.5" fill="none" opacity=".85">'
    '<ellipse cx="64" cy="58" rx="13" ry="32"/><path d="M33 48 H95 M33 68 H95"/><path d="M64 26 V90"/></g>'
    '<path d="M36 58 A28 28 0 0 0 92 58" fill="none" stroke="#3E2A12" stroke-width="5" transform="rotate(-25 64 58)"/>'
    '<rect x="54" y="94" width="20" height="8" rx="3" fill="#4A3214"/><rect x="42" y="100" width="44" height="9" rx="4.5" fill="#3E2A12"/></g>',
    rg("brass", [(0, "#FFF0C9"), (0.55, "#E2B865"), (1, "#A8752C")], cx=.38, cy=.32, r=.75))

I["calc"] = tile("#3A3A3F", "#151517",
    '<rect x="26" y="22" width="76" height="22" rx="7" fill="#000" opacity=".45"/>'
    '<path d="%s" fill="#fff"/>' % text_path("42", 18, 96, 40, 500, anchor="end") +
    "".join('<circle cx="%s" cy="%s" r="8.5" fill="%s"/>' % (38 + c * 18, 60 + r * 18,
            "#FF9F0A" if c == 3 else ("#A5A5AA" if r == 0 else "#5A5A60"))
            for r in range(3) for c in range(4)))

I["clock"] = tile("#2A2A2E", "#050506",
    '<circle cx="64" cy="64" r="44" fill="#FBFBFD"/>' +
    "".join('<rect x="62.5" y="24" width="3" height="%s" rx="1.5" fill="#1C1C1E" transform="rotate(%s 64 64)"/>' %
            (9 if i % 3 == 0 else 5, i * 30) for i in range(12)) +
    '<path d="M64 64 L64 38" stroke="#1C1C1E" stroke-width="6" stroke-linecap="round" transform="rotate(-60 64 64)"/>'
    '<path d="M64 64 L64 30" stroke="#1C1C1E" stroke-width="4.5" stroke-linecap="round" transform="rotate(60 64 64)"/>'
    '<path d="M64 74 L64 28" stroke="#FF9F0A" stroke-width="2.5" stroke-linecap="round" transform="rotate(170 64 64)"/>'
    '<circle cx="64" cy="64" r="4" fill="#FF9F0A"/>')

I["notes"] = tile("#FFFFFF", "#ECECF1",
    '<rect x="8" y="8" width="112" height="34" fill="url(#ny)"/>'
    '<rect x="8" y="40" width="112" height="2" fill="#000" opacity=".08"/>' +
    "".join('<rect x="24" y="%s" width="%s" height="4.5" rx="2.25" fill="#C7C7CC"/>' % (56 + i * 14, 80 if i < 3 else 52)
            for i in range(4)) +
    "".join('<circle cx="%s" cy="25" r="2.2" fill="#000" opacity=".18"/>' % (24 + i * 9) for i in range(10)),
    lg("ny", "#FFE15A", "#FFC53D"))

# No date on it: kernel/icons.c writes today's month and day over the band
# and the page, so the icon in the dock is a calendar and not a picture of one.
I["calendar"] = tile("#FFFFFF", "#ECECF1",
    '<rect x="8" y="8" width="112" height="34" fill="url(#cr)"/>',
    lg("cr", "#FF6259", "#E8352D"))

I["piano"] = tile("#B58BFF", "#6A3FD6",
    '<g filter="url(#gs)"><rect x="20" y="30" width="88" height="68" rx="9" fill="#fff"/>' +
    "".join('<rect x="%s" y="30" width="1.6" height="68" fill="#B8B8C4"/>' % (20 + i * 14.67) for i in range(1, 6)) +
    "".join('<rect x="%s" y="30" width="9" height="40" rx="2.5" fill="#1C1C1E"/>' % (20 + i * 14.67 - 4.5)
            for i in (1, 2, 4, 5)) + '</g>')

I["paint"] = tile("#FFFFFF", "#E9EAF0",
    '<g filter="url(#gs)"><path d="M64 24 C34 24 18 44 22 66 C26 86 44 92 54 86 C62 81 56 72 64 70 C74 67 82 80 96 74 '
    'C112 66 108 24 64 24 Z" fill="#F1D9B0"/>'
    '<circle cx="44" cy="48" r="7" fill="#FF453A"/><circle cx="62" cy="38" r="7" fill="#FFD60A"/>'
    '<circle cx="82" cy="42" r="7" fill="#34C759"/><circle cx="92" cy="58" r="7" fill="#0A84FF"/>'
    '<circle cx="38" cy="66" r="7" fill="#BF5AF2"/></g>'
    '<path d="M66 102 L104 58" stroke="#8B5A2B" stroke-width="7" stroke-linecap="round"/>'
    '<path d="M60 108 Q58 98 66 96 Q72 98 70 104 Q66 110 60 108 Z" fill="#1C1C1E"/>')

I["mines"] = tile("#A3AEC2", "#5B6578",
    "".join('<rect x="%s" y="%s" width="26" height="26" rx="5" fill="%s"/>' % (
        22 + c * 29, 22 + r * 29, "#E8ECF3" if (r, c) in ((1, 1), (2, 1), (2, 2)) else "#C3CAD7")
        for r in range(3) for c in range(3)) +
    '<path d="%s" fill="#0A84FF"/>' % text_path("1", 18, 64, 71, 700) +
    '<path d="%s" fill="#34C759"/>' % text_path("2", 18, 64, 100, 700) +
    '<g><circle cx="93" cy="93" r="7.5" fill="#1C1C1E"/>' +
    "".join('<rect x="91.5" y="81" width="3" height="24" rx="1.5" fill="#1C1C1E" transform="rotate(%s 93 93)"/>' % a
            for a in (0, 45, 90, 135)) + '</g>'
    '<path d="M32 26 V48" stroke="#1C1C1E" stroke-width="2.5"/><path d="M33 27 L45 32 L33 37 Z" fill="#FF453A"/>')

I["snake"] = tile("#4CD964", "#1E9C45",
    '<path d="M28 92 Q28 72 46 72 L76 72 Q94 72 94 56 Q94 40 76 40 L60 40" fill="none" stroke="#0E5E26" '
    'stroke-width="17" stroke-linecap="round" stroke-linejoin="round" opacity=".35" transform="translate(0 3)"/>'
    '<path d="M28 92 Q28 72 46 72 L76 72 Q94 72 94 56 Q94 40 76 40 L60 40" fill="none" stroke="url(#sn)" '
    'stroke-width="16" stroke-linecap="round" stroke-linejoin="round"/>'
    '<circle cx="58" cy="37" r="2.6" fill="#0B3D18"/>'
    '<g filter="url(#gs)"><circle cx="38" cy="38" r="9" fill="#FF3B30"/><path d="M38 29 q2 -6 7 -6" stroke="#2B7A1C" stroke-width="3" fill="none"/></g>',
    lg("sn", "#E8FF8A", "#B5F25A", 0, 0, 1, 0))

I["2048"] = tile("#FFD15C", "#F2911B",
    '<g filter="url(#gs)"><path d="%s" fill="#fff"/></g>' % text_path("2048", 36, 64, 77, 800))

I["stopwatch"] = tile("#FFB340", "#FF6A00",
    '<g filter="url(#gs)"><rect x="56" y="16" width="16" height="10" rx="3" fill="#fff"/>'
    '<rect x="61" y="24" width="6" height="8" fill="#fff"/>'
    '<path d="M92 34 l8 -8" stroke="#fff" stroke-width="6" stroke-linecap="round"/>'
    '<circle cx="64" cy="70" r="36" fill="#fff"/></g>'
    '<path d="M64 70 L64 44" stroke="#FF6A00" stroke-width="5" stroke-linecap="round" transform="rotate(35 64 70)"/>'
    '<circle cx="64" cy="70" r="4.5" fill="#1C1C1E"/>' +
    "".join('<rect x="63" y="38" width="2" height="5" fill="#8E8E93" transform="rotate(%s 64 70)"/>' % (i * 30) for i in range(12)))

I["sysinfo"] = tile("#7DB4FF", "#2F63F0",
    '<g filter="url(#gs)"><circle cx="64" cy="64" r="38" fill="#fff"/></g>'
    '<circle cx="64" cy="44" r="6" fill="#2F63F0"/><rect x="58" y="56" width="12" height="34" rx="6" fill="#2F63F0"/>')

I["logs"] = tile("#C9A2FF", "#8452E8",
    '<g filter="url(#gs)"><rect x="30" y="20" width="68" height="88" rx="9" fill="#fff"/></g>' +
    "".join('<rect x="40" y="%s" width="%s" height="5" rx="2.5" fill="%s"/>' % (
        34 + i * 13, (48 if i % 2 else 36), "#FF9F0A" if i == 2 else "#B8B0CC") for i in range(5)) +
    '<circle cx="84" cy="88" r="13" fill="none" stroke="#6B3FD0" stroke-width="5"/>'
    '<path d="M93 97 L102 106" stroke="#6B3FD0" stroke-width="6" stroke-linecap="round"/>')

I["life"] = tile("#2C5A44", "#0E2419",
    "".join('<rect x="%s" y="%s" width="20" height="20" rx="5" fill="url(#cell)"/>' % (28 + c * 25, 28 + r * 25)
            for (r, c) in ((0, 1), (1, 2), (2, 0), (2, 1), (2, 2))) +
    "".join('<rect x="%s" y="%s" width="20" height="20" rx="5" fill="#fff" opacity=".06"/>' % (28 + c * 25, 28 + r * 25)
            for (r, c) in ((0, 0), (0, 2), (1, 0), (1, 1))),
    lg("cell", "#9DFFB0", "#30D158"))

I["ttt"] = tile("#FF8A80", "#E5394D",
    '<g stroke="#fff" stroke-opacity=".55" stroke-width="5" stroke-linecap="round">'
    '<path d="M52 24 V104 M76 24 V104 M24 52 H104 M24 76 H104"/></g>'
    '<g filter="url(#gs)"><path d="M30 30 L46 46 M46 30 L30 46 M82 82 L98 98 M98 82 L82 98" stroke="#fff" stroke-width="7" stroke-linecap="round"/>'
    '<circle cx="64" cy="64" r="8.5" fill="none" stroke="#FFE066" stroke-width="6"/>'
    '<circle cx="90" cy="38" r="8.5" fill="none" stroke="#FFE066" stroke-width="6"/></g>')

_wheel = "".join(
    '<path d="M64 64 L%.2f %.2f A36 36 0 0 1 %.2f %.2f Z" fill="hsl(%d,90%%,58%%)"/>' % (
        64 + 36 * math.sin(math.radians(i * 30)), 64 - 36 * math.cos(math.radians(i * 30)),
        64 + 36 * math.sin(math.radians(i * 30 + 30.5)), 64 - 36 * math.cos(math.radians(i * 30 + 30.5)), i * 30)
    for i in range(12))
I["color"] = tile("#FFFFFF", "#E7E8EE",
    '<g filter="url(#gs)">%s<circle cx="64" cy="64" r="12" fill="#fff"/></g>'
    '<path d="M82 96 L100 78" stroke="#3A3A3C" stroke-width="7" stroke-linecap="round"/>'
    '<path d="M98 66 L112 80 L104 88 L90 74 Z" fill="#3A3A3C"/>' % _wheel)

I["memory"] = tile("#7AD7FF", "#1E88F0",
    '<g filter="url(#gs)"><rect x="22" y="30" width="44" height="62" rx="8" fill="url(#cb)" transform="rotate(-10 44 61)"/>'
    '<rect x="60" y="34" width="44" height="62" rx="8" fill="#fff" transform="rotate(8 82 65)"/></g>'
    '<path d="M82 76 C70 66 74 56 82 62 C90 56 94 66 82 76 Z" fill="#FF375F" transform="rotate(8 82 65)"/>',
    lg("cb", "#FF6FB0", "#C2185B"))

I["matrix"] = tile("#06210F", "#000",
    "".join('<rect x="%s" y="%s" width="9" height="9" rx="2" fill="#30D158" opacity="%.2f"/>' % (
        22 + c * 14, 18 + r * 13, max(0.08, 1 - abs(r - (c * 3 % 7)) * 0.18))
        for c in range(7) for r in range(8)))

I["doom"] = tile("#7A1414", "#250404",
    '<g filter="url(#gs)"><path d="M30 30 Q36 54 48 58 L48 50 Q64 40 80 50 L80 58 Q92 54 98 30 Q104 60 88 72 '
    'Q90 92 76 100 L74 108 H54 L52 100 Q38 92 40 72 Q24 60 30 30 Z" fill="url(#sk)"/></g>'
    '<path d="M50 74 L60 80 L50 84 Z M78 74 L68 80 L78 84 Z" fill="#FFD60A"/>'
    '<path d="M56 96 h16" stroke="#3A0606" stroke-width="3"/>',
    lg("sk", "#FF8A3D", "#C2210F"))

I["sheets"] = tile("#3DDC84", "#14873F",
    '<g filter="url(#gs)"><rect x="22" y="26" width="84" height="76" rx="8" fill="#fff"/></g>'
    '<rect x="22" y="26" width="84" height="18" rx="8" fill="#D8F5E3"/><rect x="22" y="38" width="84" height="6" fill="#D8F5E3"/>'
    '<g stroke="#9ED9B4" stroke-width="2.5"><path d="M50 26 V102 M78 26 V102 M22 62 H106 M22 82 H106 M22 44 H106"/></g>'
    '<rect x="51" y="63" width="27" height="19" fill="#34C759" opacity=".35"/>')

I["image"] = tile("#FFFFFF", "#E9EAF0",
    '<g filter="url(#gs)"><rect x="18" y="24" width="92" height="80" rx="12" fill="url(#sky)"/></g>'
    '<circle cx="86" cy="48" r="10" fill="#FFE066"/>'
    '<path d="M18 92 L48 58 L66 78 L80 64 L110 96 L110 92 Q110 104 98 104 L30 104 Q18 104 18 92 Z" fill="url(#mt)"/>'
    '<path d="M48 58 L56 67 L50 66 L45 70 L42 65 Z" fill="#fff" opacity=".85"/>',
    lg("sky", "#5AC8FA", "#A0E4FF") + lg("mt", "#34C759", "#1C8C3C"))

I["music"] = tile("#FF6482", "#FA2D48",
    '<g filter="url(#gs)" fill="#fff"><path d="M52 34 L94 26 L94 82" fill="none" stroke="#fff" stroke-width="8" stroke-linejoin="round"/>'
    '<path d="M52 34 V90" stroke="#fff" stroke-width="8"/>'
    '<ellipse cx="42" cy="90" rx="13" ry="11"/><ellipse cx="84" cy="82" rx="13" ry="11"/></g>')

I["tasks"] = tile("#FFFFFF", "#ECECF1",
    "".join('<circle cx="34" cy="%s" r="9" fill="none" stroke="%s" stroke-width="4.5"/>'
            '<rect x="52" y="%s" width="%s" height="5" rx="2.5" fill="#C7C7CC"/>' % (y, c, y - 2.5, w)
            for (y, c, w) in ((38, "#0A84FF", 50), (64, "#FF453A", 42), (90, "#FF9F0A", 46))) +
    '<circle cx="34" cy="38" r="5" fill="#0A84FF"/>')

I["contacts"] = tile("#3ED6C7", "#138F84",
    '<g filter="url(#gs)"><circle cx="64" cy="50" r="18" fill="#fff"/>'
    '<path d="M28 102 Q30 72 64 72 Q98 72 100 102 Z" fill="#fff"/></g>')

I["install"] = tile("#5AE38C", "#16994D",
    '<g filter="url(#gs)"><rect x="24" y="72" width="80" height="30" rx="9" fill="#fff"/>'
    '<circle cx="88" cy="87" r="4" fill="#34C759"/><rect x="34" y="85" width="34" height="4" rx="2" fill="#C7C7CC"/>'
    '<path d="M64 20 V58 M48 44 L64 60 L80 44" stroke="#fff" stroke-width="9" %s/></g>' % STROKE)

I["security"] = tile("#7D7AFF", "#3634A3",
    '<g filter="url(#gs)"><path d="M64 18 L100 30 V60 Q100 92 64 110 Q28 92 28 60 V30 Z" fill="#fff"/></g>'
    '<path d="M48 64 L60 76 L82 52" stroke="#4F4CD8" stroke-width="8" %s/>' % STROKE)

I["software"] = tile("#4FC3FF", "#0A6CF0",
    '<g filter="url(#gs)"><path d="M26 46 L64 28 L102 46 L102 88 L64 106 L26 88 Z" fill="#fff"/></g>'
    '<path d="M26 46 L64 64 L102 46 M64 64 V106" fill="none" stroke="#B9D8FF" stroke-width="3.5"/>'
    '<path d="M45 37 L83 55 V70" fill="none" stroke="#0A84FF" stroke-width="6" stroke-linejoin="round"/>')

I["media"] = tile("#FFB04A", "#FF375F",
    '<g filter="url(#gs)"><path d="M48 34 Q48 26 55 30 L96 56 Q103 60 96 64 L55 90 Q48 94 48 86 Z" fill="#fff" '
    'transform="translate(-2 4)"/></g>', angle="d")

I["network"] = tile("#3D9BFF", "#0040DD",
    '<g filter="url(#gs)" fill="none" stroke="#fff" stroke-width="9" stroke-linecap="round">'
    '<path d="M24 54 Q64 18 104 54"/><path d="M38 70 Q64 46 90 70"/><path d="M52 86 Q64 76 76 86"/></g>'
    '<circle cx="64" cy="100" r="6" fill="#fff"/>')

I["mixer"] = tile("#C77DFF", "#7A2EC9",
    '<g stroke="#000" stroke-opacity=".28" stroke-width="5" stroke-linecap="round">'
    '<path d="M40 28 V100 M64 28 V100 M88 28 V100"/></g>'
    '<g filter="url(#gs)" fill="#fff"><rect x="30" y="68" width="20" height="13" rx="5"/>'
    '<rect x="54" y="38" width="20" height="13" rx="5"/><rect x="78" y="58" width="20" height="13" rx="5"/></g>')

I["device"] = tile("#4FD1E8", "#167C91",
    '<g filter="url(#gs)"><rect x="34" y="34" width="60" height="60" rx="9" fill="#fff"/></g>'
    '<rect x="48" y="48" width="32" height="32" rx="4" fill="#1D8EA5"/>' +
    "".join('<rect x="%s" y="22" width="5" height="12" rx="2" fill="#fff"/><rect x="%s" y="94" width="5" height="12" rx="2" fill="#fff"/>'
            '<rect x="22" y="%s" width="12" height="5" rx="2" fill="#fff"/><rect x="94" y="%s" width="12" height="5" rx="2" fill="#fff"/>'
            % (p, p, p, p) for p in (44, 61.5, 79)))

I["charmap"] = tile("#FFFFFF", "#E6E7ED",
    '<path d="%s" fill="#1C1C1E"/>' % text_path("Aa", 54, 64, 82, 600) +
    '<rect x="26" y="96" width="76" height="5" rx="2.5" fill="#FF9F0A"/>')

I["snip"] = tile("#FF7AA2", "#E0245E",
    '<rect x="24" y="26" width="80" height="62" rx="9" fill="none" stroke="#fff" stroke-width="5" stroke-dasharray="11 8"/>'
    '<g filter="url(#gs)" fill="none" stroke="#fff" stroke-width="5.5" stroke-linecap="round">'
    '<circle cx="48" cy="98" r="9"/><circle cx="80" cy="98" r="9"/><path d="M54 92 L82 50 M74 92 L46 50"/></g>')

I["cards"] = tile("#36B36A", "#0E6B33",
    '<g filter="url(#gs)"><rect x="24" y="30" width="48" height="68" rx="8" fill="#fff" transform="rotate(-12 48 64)"/>'
    '<rect x="56" y="30" width="48" height="68" rx="8" fill="#fff" transform="rotate(10 80 64)"/></g>'
    '<path d="M80 78 C64 64 70 50 80 58 C90 50 96 64 80 78 Z" fill="#FF3B30" transform="rotate(10 80 64)"/>'
    '<path d="M42 44 C36 52 38 58 44 56 L42 62 L48 62 L46 56 C52 58 54 52 42 44 Z" fill="#1C1C1E" transform="rotate(-12 48 64)"/>')

I["clipboard"] = tile("#FFC94D", "#F08C00",
    '<g filter="url(#gs)"><rect x="30" y="26" width="68" height="84" rx="10" fill="#fff"/>'
    '<rect x="48" y="18" width="32" height="16" rx="6" fill="#8E5A00"/></g>' +
    "".join('<rect x="42" y="%s" width="%s" height="5" rx="2.5" fill="#D9C7A0"/>' % (50 + i * 13, 44 if i < 3 else 28)
            for i in range(4)))

I["strike"] = tile("#5E6A2A", "#262D0E",
    '<g filter="url(#gs)" fill="none" stroke="#FFB340" stroke-width="5" stroke-linecap="round">'
    '<circle cx="64" cy="64" r="30"/><path d="M64 22 V44 M64 84 V106 M22 64 H44 M84 64 H106"/></g>'
    '<circle cx="64" cy="64" r="5" fill="#FF453A"/>')

I["gameboy"] = tile("#D1D1D6", "#8E8E93",
    '<g filter="url(#gs)"><rect x="34" y="16" width="60" height="96" rx="10" fill="#ECECEC"/></g>'
    '<rect x="42" y="24" width="44" height="36" rx="4" fill="#4B4F5C"/><rect x="47" y="29" width="34" height="26" fill="#9BBC0F"/>'
    '<path d="M50 78 h12 M56 72 v12" stroke="#3A3A3C" stroke-width="5" stroke-linecap="round"/>'
    '<circle cx="78" cy="80" r="4.5" fill="#B03060"/><circle cx="86" cy="73" r="4.5" fill="#B03060"/>')

I["craft"] = tile("#7FD0FF", "#3496E0",
    '<g filter="url(#gs)"><path d="M64 26 L100 44 L64 62 L28 44 Z" fill="#6CC04A"/>'
    '<path d="M28 44 L64 62 L64 104 L28 86 Z" fill="#8B5A33"/><path d="M100 44 L64 62 L64 104 L100 86 Z" fill="#6E4422"/>'
    '<path d="M28 44 L64 62 L64 70 L28 52 Z" fill="#5AA83B"/><path d="M100 44 L64 62 L64 70 L100 52 Z" fill="#4B8E30"/></g>')

I["taskmgr"] = tile("#2F4FA8", "#101C47",
    '<g stroke="#fff" stroke-opacity=".12" stroke-width="2"><path d="M22 44 H106 M22 64 H106 M22 84 H106"/></g>'
    '<path d="M22 84 L40 84 L50 56 L62 96 L74 40 L84 74 L106 74" fill="none" stroke="url(#pulse)" '
    'stroke-width="7" stroke-linecap="round" stroke-linejoin="round"/>',
    lg("pulse", "#5BE37D", "#64D2FF", 0, 0, 1, 0))

# ---- system icons, no tile ------------------------------------------------
I["pc"] = svg(
    '<g filter="url(#ds)"><rect x="12" y="18" width="104" height="72" rx="9" fill="#2B2D33"/></g>'
    '<rect x="18" y="24" width="92" height="60" rx="4" fill="url(#scr)"/>'
    '<path d="M18 70 Q60 40 110 52 L110 84 L18 84 Z" fill="#fff" opacity=".12"/>'
    '<path d="M52 90 L48 108 H80 L76 90 Z" fill="#B4B9C4"/><rect x="38" y="106" width="52" height="7" rx="3.5" fill="#D4D8E0"/>',
    lg("scr", "#4FA3FF", "#1B4FD8", 0, 0, 1, 1))

I["drive"] = svg(
    '<g filter="url(#ds)"><rect x="14" y="40" width="100" height="52" rx="10" fill="url(#dr)"/></g>'
    '<rect x="14" y="40" width="100" height="52" rx="10" fill="none" stroke="#fff" stroke-opacity=".6" stroke-width="1.5"/>'
    '<rect x="24" y="72" width="44" height="6" rx="3" fill="#8A93A3"/><circle cx="98" cy="75" r="4.5" fill="#34C759"/>',
    lg("dr", "#F2F4F8", "#B7BFCC"))

I["usb"] = svg(
    '<g filter="url(#ds)"><rect x="40" y="36" width="48" height="80" rx="10" fill="url(#dr)"/></g>'
    '<rect x="48" y="12" width="32" height="26" rx="3" fill="#C9CED8"/><rect x="55" y="20" width="6" height="6" fill="#6B7280"/>'
    '<rect x="67" y="20" width="6" height="6" fill="#6B7280"/><circle cx="64" cy="98" r="4" fill="#0A84FF"/>',
    lg("dr", "#3A3D46", "#1C1D22"))

for key, glyph in (("home", "house"), ("desktopf", "monitor"), ("docs", "file-text"),
                   ("downloads", "download"), ("pictures", "image"), ("musicf", "music"),
                   ("videos", "video")):
    _fd, _fb = folder_shape(emb=emboss(glyph, 2.6))
    I[key] = svg(_fb, _fd)

for key, badge, col in (("file_text", "TXT", "#8E8E93"), ("file_code", "</>", "#0A84FF"),
                        ("file_image", None, None), ("file_audio", "MP3", "#FF375F"),
                        ("file_video", "VID", "#BF5AF2"), ("file_archive", "ZIP", "#FF9F0A"),
                        ("file_exe", "EXE", "#3A3A3C"), ("file_web", "HTML", "#FF6A00"),
                        ("file_sheet", "CSV", "#34C759")):
    if key == "file_image":
        _pd, _pb = page_shape(lines=False)
        _pb += ('<rect x="34" y="44" width="60" height="50" rx="6" fill="url(#sky)"/>'
                '<circle cx="80" cy="58" r="6" fill="#FFE066"/>'
                '<path d="M34 86 L52 66 L64 78 L72 70 L94 90 Q94 94 88 94 L40 94 Q34 94 34 88 Z" fill="#34C759"/>')
        _pd += lg("sky", "#5AC8FA", "#A0E4FF")
    else:
        _pd, _pb = page_shape(lines=True, badge=badge, badge_col=col)
    I[key] = svg(_pb, _pd)

I["user"] = svg(
    '<circle cx="64" cy="64" r="58" fill="url(#av)"/>'
    '<circle cx="64" cy="52" r="20" fill="#fff" opacity=".95"/>'
    '<path d="M28 104 Q32 76 64 76 Q96 76 100 104 Q84 120 64 120 Q44 120 28 104 Z" fill="#fff" opacity=".95"/>',
    lg("av", "#9AA6B8", "#5D6878"))


# ---------------------------------------------------------------------------
#  Cursors: 32 px cells, drawn once at 1x. Hot spots live in kernel/gui.c.
# ---------------------------------------------------------------------------
CUR_DEFS = ('<filter id="cs" x="-30%" y="-30%" width="160%" height="160%">'
            '<feDropShadow dx="0" dy="1.2" stdDeviation="1.1" flood-color="#000" flood-opacity=".45"/></filter>')


def cursor_svg(body):
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32"><defs>%s</defs>'
            '<g filter="url(#cs)">%s</g></svg>' % (CUR_DEFS, body))


def dbl_arrow(rot):
    # a double-headed arrow along x, rotated; black with a white keyline
    d = "M4 16 L10 10 L10 14 L22 14 L22 10 L28 16 L22 22 L22 18 L10 18 L10 22 Z"
    return ('<path d="%s" transform="rotate(%s 16 16)" fill="#000" stroke="#fff" stroke-width="1.6" '
            'stroke-linejoin="round" paint-order="stroke"/>' % (d, rot))


_IBEAM = "M12 6 Q16 6 16 9 Q16 6 20 6 M16 9 V23 M12 26 Q16 26 16 23 Q16 26 20 26 M13 16 H19"
CURSORS = [
    ("ARROW", cursor_svg('<path d="M6 3 L6 25 L11.3 20 L14.8 28.2 L18.4 26.7 L14.9 18.6 L22 18.6 Z" '
                         'fill="#000" stroke="#fff" stroke-width="1.7" stroke-linejoin="round" paint-order="stroke"/>')),
    ("RESIZE_EW", cursor_svg(dbl_arrow(0))),
    ("RESIZE_NS", cursor_svg(dbl_arrow(90))),
    ("RESIZE_NWSE", cursor_svg(dbl_arrow(45))),
    ("RESIZE_NESW", cursor_svg(dbl_arrow(-45))),
    ("TEXT", cursor_svg('<path d="%s" fill="none" stroke="#fff" stroke-width="3.6" stroke-linecap="round"/>'
                        '<path d="%s" fill="none" stroke="#000" stroke-width="1.6" stroke-linecap="round"/>'
                        % (_IBEAM, _IBEAM))),
    ("HAND", cursor_svg('<path d="M12 15 V6.5 Q12 4.5 14 4.5 Q16 4.5 16 6.5 V13 V11.5 Q16 9.6 18 9.6 Q20 9.6 20 11.5 V13.5 '
                        'Q20 11.8 22 11.8 Q24 11.8 24 13.6 V15 Q24 13.4 25.8 13.4 Q27.6 13.4 27.6 15.4 V21 Q27.6 28 21 28 '
                        'H18 Q14 28 11.5 24.5 L7 18 Q5.8 16.2 7.4 15.2 Q9 14.4 10.2 16 Z" fill="#fff" stroke="#000" '
                        'stroke-width="1.5" stroke-linejoin="round"/>'
                        '<path d="M16 13 V18 M20 13.5 V18 M24 15 V18.5" stroke="#000" stroke-width="1.3" stroke-linecap="round"/>')),
]


# ---------------------------------------------------------------------------
#  Glyphs: name in the C enum -> Lucide icon
# ---------------------------------------------------------------------------
GLYPHS = [
    ("WIFI", "wifi"), ("WIFI_2", "wifi-high"), ("WIFI_1", "wifi-low"), ("WIFI_0", "wifi-zero"),
    ("WIFI_OFF", "wifi-off"), ("ETHERNET", "ethernet-port"),
    ("VOL_MUTE", "volume-x"), ("VOL_0", "volume"), ("VOL_1", "volume-1"), ("VOL_2", "volume-2"),
    ("BATTERY", "battery"), ("BATTERY_CHG", "battery-charging"),
    ("BELL", "bell"), ("BELL_DOT", "bell-dot"), ("BELL_OFF", "bell-off"),
    ("SEARCH", "search"), ("CLOSE", "x"), ("MINUS", "minus"), ("PLUS", "plus"),
    ("MAXIMIZE", "maximize-2"), ("RESTORE", "minimize-2"),
    ("CHEV_L", "chevron-left"), ("CHEV_R", "chevron-right"), ("CHEV_U", "chevron-up"), ("CHEV_D", "chevron-down"),
    ("ARROW_L", "arrow-left"), ("ARROW_R", "arrow-right"), ("ARROW_U", "arrow-up"),
    ("REFRESH", "rotate-cw"), ("HOME", "house"), ("SETTINGS", "settings"), ("POWER", "power"),
    ("LOCK", "lock"), ("LOGOUT", "log-out"), ("USER", "user"), ("MOON", "moon"), ("SUN", "sun"),
    ("THEME", "sun-moon"), ("RECORD", "circle-dot"), ("CAMERA", "camera"), ("CHECK", "check"),
    ("MORE", "ellipsis"), ("GRID", "layout-grid"), ("LIST", "list"), ("COPY", "copy"), ("CUT", "scissors"),
    ("PASTE", "clipboard-paste"), ("RENAME", "pencil"), ("TRASH", "trash-2"), ("NEW_FOLDER", "folder-plus"),
    ("SORT", "arrow-up-down"), ("INFO", "info"), ("PLAY", "play"), ("PAUSE", "pause"),
    ("NEXT", "skip-forward"), ("PREV", "skip-back"), ("STOP", "square"), ("SHUFFLE", "shuffle"),
    ("REPEAT", "repeat"), ("STAR", "star"), ("MONITOR", "monitor"), ("DOCUMENT", "file-text"),
    ("DOWNLOAD", "download"), ("PICTURE", "image"), ("MUSIC", "music"), ("VIDEO", "video"),
    ("FOLDER", "folder"), ("DRIVE", "hard-drive"), ("GLOBE", "globe"), ("SHIELD", "shield-check"),
    ("KEYBOARD", "keyboard"), ("MOUSE", "mouse"), ("SPEAKER", "speaker"), ("MIC", "mic"),
    ("WINDOW", "app-window"), ("PIN", "pin"), ("UNPIN", "pin-off"), ("SIDEBAR", "panel-left"),
    ("TERMINAL", "terminal"), ("SYNC", "refresh-cw"), ("BLUETOOTH", "bluetooth"), ("PLANE", "plane"),
    ("EYE", "eye"), ("PALETTE", "palette"), ("TYPE", "type"), ("CPU", "cpu"), ("MEMORY", "memory-stick"),
    ("NETWORK", "network"), ("ACTIVITY", "activity"), ("SLIDERS", "sliders-horizontal"), ("LAYERS", "layers"),
    ("SPARKLES", "sparkles"), ("BOLT", "zap"), ("EXTERNAL", "external-link"), ("CLOCK", "clock"),
    ("CALENDAR", "calendar"), ("HELP", "circle-help"), ("WARN", "triangle-alert"), ("ERROR", "circle-x"),
    ("OK", "circle-check"), ("GRIP", "grip-horizontal"), ("COMMAND", "command"), ("UNDO", "undo-2"),
    ("REDO", "redo-2"), ("SAVE", "save"), ("FILE_NEW", "file-plus"), ("FOLDER_OPEN", "folder-open"),
    ("TEXT_INPUT", "text-cursor-input"), ("USB", "usb"), ("SMILE", "smile"),
]


# ---------------------------------------------------------------------------
#  Rendering
# ---------------------------------------------------------------------------
def icon_order():
    """ICON_* from include/gui.h, in declaration order."""
    src = open(os.path.join(ROOT, "include", "gui.h"), encoding="utf-8").read()
    m = re.search(r"enum\s*\{\s*ICON_NONE\s*=\s*0\s*,(.*?)\}", src, re.S)
    if not m:
        raise SystemExit("mkicons: no ICON_* enum in include/gui.h")
    names = [n.strip() for n in m.group(1).replace("\n", " ").split(",") if n.strip()]
    names = [n for n in names if n.startswith("ICON_") and n != "ICON_COUNT"]
    return [n[5:].lower() for n in names]


def find_chrome():
    for c in CHROME_CANDIDATES:
        if os.path.exists(c):
            return c
    raise SystemExit("mkicons: no Chrome or Edge to render with")


def render_grid(chrome, files, size, cols, out_png, work):
    rows = (len(files) + cols - 1) // cols
    W, H = cols * size, rows * size
    imgs = []
    for i, f in enumerate(files):
        if f is None:
            continue
        x, y = (i % cols) * size, (i // cols) * size
        imgs.append('<img src="%s" style="left:%dpx;top:%dpx;width:%dpx;height:%dpx">' % (
            os.path.basename(f), x, y, size, size))
    html = ('<!doctype html><html><head><style>html,body{margin:0;padding:0;background:transparent;overflow:hidden}'
            'img{position:absolute;display:block}</style></head><body>%s</body></html>' % "".join(imgs))
    page = os.path.join(work, "grid_%d.html" % size)
    open(page, "w", encoding="utf-8").write(html)
    profile = os.path.join(work, "profile")
    cmd = [chrome, "--headless", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
           "--disable-extensions", "--user-data-dir=" + profile,
           "--force-device-scale-factor=1", "--default-background-color=00000000",
           "--window-size=%d,%d" % (W, H), "--screenshot=" + out_png,
           "file:///" + page.replace("\\", "/")]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120, check=False)
    if not os.path.exists(out_png):
        raise SystemExit("mkicons: Chrome produced no %s" % out_png)
    return W, H


def main(argv):
    outdir = argv[1] if len(argv) > 1 else os.path.join(ROOT, "assets", "icons")
    os.makedirs(outdir, exist_ok=True)
    order = icon_order()
    missing = [n for n in order if n not in I]
    if missing:
        raise SystemExit("mkicons: no drawing for ICON_%s" % ", ICON_".join(m.upper() for m in missing))
    chrome = find_chrome()
    work = tempfile.mkdtemp(prefix="mkicons_")
    try:
        from PIL import Image
    except ImportError:
        Image = None
    # app icons: index 0 is ICON_NONE and stays empty
    files = [None]
    for n in order:
        p = os.path.join(work, "app_%s.svg" % n)
        open(p, "w", encoding="utf-8").write(I[n])
        files.append(p)
    for s in APP_SIZES:
        out = os.path.join(outdir, "apps-%d.png" % s)
        W, H = render_grid(chrome, files, s, COLS, out, work)
        print("      apps-%d.png: %dx%d, %d icons" % (s, W, H, len(order)))
    # glyphs
    gfiles = []
    for cname, lname in GLYPHS:
        p = os.path.join(work, "g_%s.svg" % cname.lower())
        open(p, "w", encoding="utf-8").write(
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="#fff" '
            'stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">%s</svg>' % lucide_inner(lname))
        gfiles.append(p)
    for s in GLYPH_SIZES:
        out = os.path.join(outdir, "glyphs-%d.png" % s)
        W, H = render_grid(chrome, gfiles, s, 16, out, work)
        if Image:      # the glyph atlases only need their alpha: keep them small
            im = Image.open(out).convert("RGBA")
            a = im.getchannel("A")
            Image.merge("LA", (Image.new("L", im.size, 255), a)).save(out, optimize=True)
        print("      glyphs-%d.png: %dx%d, %d glyphs" % (s, W, H, len(GLYPHS)))
    if Image:
        for s in APP_SIZES:
            p = os.path.join(outdir, "apps-%d.png" % s)
            Image.open(p).convert("RGBA").save(p, optimize=True)
    # cursors
    cfiles = []
    for cname, body in CURSORS:
        p = os.path.join(work, "c_%s.svg" % cname.lower())
        open(p, "w", encoding="utf-8").write(body)
        cfiles.append(p)
    out = os.path.join(outdir, "cursors-32.png")
    W, H = render_grid(chrome, cfiles, 32, len(CURSORS), out, work)
    if Image:
        Image.open(out).convert("RGBA").save(out, optimize=True)
    print("      cursors-32.png: %dx%d, %d cursors" % (W, H, len(CURSORS)))
    # the glyph enum
    hdr = os.path.join(ROOT, "include", "glyphs.h")
    with open(hdr, "w", encoding="utf-8", newline="\n") as f:
        f.write("#pragma once\n/* Generated by mkicons.py -- the order of assets/icons/glyphs-*.png.\n"
                " * Lucide icons (ISC), see assets/icons/LICENSE-lucide.txt. */\n")
        f.write("enum {\n")
        for i, (cname, lname) in enumerate(GLYPHS):
            f.write("    GLYPH_%s,%s/* %s */\n" % (cname, " " * max(1, 14 - len(cname)), lname))
        f.write("    GLYPH_COUNT\n};\n#define GLYPH_COLS 16\n#define ICON_COLS %d\n\n" % COLS)
        f.write("/* cursors-32.png, one row */\nenum {\n")
        for cname, _ in CURSORS:
            f.write("    CURSOR_%s,\n" % cname)
        f.write("    CURSOR_COUNT\n};\n")
    print("      include/glyphs.h: %d glyphs" % len(GLYPHS))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
