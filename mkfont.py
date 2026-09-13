#!/usr/bin/env python3
"""Generate kernel/font_latin1.c -- the glyphs a non-US keyboard produces.

A layout that types 'ä' onto a machine whose font stops at 0x7F has not added
a language, it has added a blank square. So before any layout work there has to
be something to draw, and this makes it.

Almost all of it is composition rather than drawing. An accented letter is the
letter with a mark over it, and both fonts already have the letters, so the
mark is the only new information: put it in the rows above the glyph and OR the
two together. That is also why it comes out consistent -- every acute in the
file is the same three pixels, because there is one acute.

Two details the composition has to get right:

  * Lowercase already leaves its top rows empty (a 'a' occupies rows 2..6 of 8),
    so a mark drops straight in. Uppercase does not -- 'A' starts at row 0 --
    so the letter is shifted down by one and the mark goes where it was. That
    costs the descender row, which uppercase does not use, and it is what every
    8x8 codepage font has always done.
  * The two faces pack their bits in opposite directions. font8x8_basic has
    bit 0 at the left; font_8x16 has bit 7 at the left. Getting that backwards
    produces mirrored accents on correct letters, which looks like a font bug
    and is arithmetic.

The symbols -- the currency signs, the inverted punctuation, the guillemets --
are drawn by hand below, because there is nothing to compose them from.

Everything outside Latin-1 is deliberately absent, and the layouts are chosen
to match: this OS's text path is bytes, not code points, so Polish, Czech,
Greek and Cyrillic want UTF-8 through the console, the editors, the clipboard
and the filesystem first. That is a real piece of work and it is not this one.

Usage: python mkfont.py   (writes kernel/font_latin1.c)
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "kernel", "font8x8.c")
OUT = os.path.join(HERE, "kernel", "font_latin1.c")

# 0x80..0xFF rather than 0xA0..0xFF, for one character: the euro sign is not
# in Latin-1 at all (it was standardised after it), and every layout that wants
# one puts it at 0x80 the way CP1252 does. The gap between is control codes
# nothing types, and it costs 32 blank rows to keep one honest euro.
LO, HI = 0x80, 0xFF          # 128 glyphs


# --------------------------------------------------------------------------
#  Reading the fonts we already have
# --------------------------------------------------------------------------
def parse_fonts(path):
    text = open(path, encoding="utf-8", errors="replace").read()

    # font8x8_basic is a designated-initialiser table: [0x41] = { ... }
    basic = {}
    for m in re.finditer(r"\[0x([0-9A-Fa-f]{2})\]\s*=\s*\{([^}]*)\}", text):
        code = int(m.group(1), 16)
        rows = [int(v, 0) for v in m.group(2).split(",") if v.strip()]
        if len(rows) == 8:
            basic[code] = rows

    # font_8x16 is a plain list starting at space (32).
    start = text.index("font_8x16")
    body = text[start:]
    arial = {}
    for i, m in enumerate(re.finditer(r"\{([0-9a-fx,A-F ]{60,})\}", body)):
        rows = [int(v, 0) for v in m.group(1).split(",") if v.strip()]
        if len(rows) != 16:
            continue
        ch = 32 + i
        if ch > 126:
            break
        arial[ch] = rows
    return basic, arial


# --------------------------------------------------------------------------
#  Accents
#
#  Each is given left-to-right as a list of pixel columns per row, which is
#  readable as a picture in the source and is converted to bits below. The
#  8x8 marks are two rows; the 8x16 marks are three, because there is room and
#  a two-pixel acute on a sixteen-row face looks like dirt.
# --------------------------------------------------------------------------
ACC8 = {
    "acute":  ["..xx....", ".xx....."],
    "grave":  [".xx.....", "..xx...."],
    "circ":   ["..xx....", ".x..x..."],
    "tilde":  [".xx..x..", "x..xx..."],
    "uml":    [".x..x...", "........"],
    "ring":   ["..xx....", ".x..x..."],
}
ACC16 = {
    "acute":  ["...xx...", "..xx....", ".xx....."],
    "grave":  ["..xx....", "...xx...", "....xx.."],
    "circ":   ["...xx...", "..x..x..", ".x....x."],
    "tilde":  ["..xx.x..", ".x..xx..", "........"],
    "uml":    ["..x..x..", "..x..x..", "........"],
    "ring":   ["...xx...", "..x..x..", "...xx..."],
}


def bits_lsb(pat):
    """'..xx....' -> a byte with bit 0 as the leftmost pixel (font8x8_basic)."""
    v = 0
    for i, c in enumerate(pat):
        if c == "x":
            v |= 1 << i
    return v


def bits_msb(pat):
    """'..xx....' -> a byte with bit 7 as the leftmost pixel (font_8x16)."""
    v = 0
    for i, c in enumerate(pat):
        if c == "x":
            v |= 0x80 >> i
    return v


def compose8(base_rows, accent, upper):
    """8x8: drop the mark in above the letter, shifting uppercase down one."""
    rows = list(base_rows)
    if upper:
        rows = [0] + rows[:7]
    mark = [bits_lsb(p) for p in ACC8[accent]]
    out = []
    for i in range(8):
        v = rows[i]
        if i < len(mark):
            v |= mark[i]
        out.append(v & 0xFF)
    return out


def compose16(base_rows, accent, upper):
    """8x16: the same, with three rows of mark and two rows of shift."""
    rows = list(base_rows)
    if upper:
        rows = [0, 0] + rows[:14]
    mark = [bits_msb(p) for p in ACC16[accent]]
    out = []
    for i in range(16):
        v = rows[i]
        if 1 <= i <= 3:
            v |= mark[i - 1]
        out.append(v & 0xFF)
    return out


# --------------------------------------------------------------------------
#  What is composed from what.
# --------------------------------------------------------------------------
COMPOSED = {
    0xC0: ("A", "grave"), 0xC1: ("A", "acute"), 0xC2: ("A", "circ"),
    0xC3: ("A", "tilde"), 0xC4: ("A", "uml"),   0xC5: ("A", "ring"),
    0xC8: ("E", "grave"), 0xC9: ("E", "acute"), 0xCA: ("E", "circ"),
    0xCB: ("E", "uml"),
    0xCC: ("I", "grave"), 0xCD: ("I", "acute"), 0xCE: ("I", "circ"),
    0xCF: ("I", "uml"),
    0xD1: ("N", "tilde"),
    0xD2: ("O", "grave"), 0xD3: ("O", "acute"), 0xD4: ("O", "circ"),
    0xD5: ("O", "tilde"), 0xD6: ("O", "uml"),
    0xD9: ("U", "grave"), 0xDA: ("U", "acute"), 0xDB: ("U", "circ"),
    0xDC: ("U", "uml"),
    0xDD: ("Y", "acute"),
    0xE0: ("a", "grave"), 0xE1: ("a", "acute"), 0xE2: ("a", "circ"),
    0xE3: ("a", "tilde"), 0xE4: ("a", "uml"),   0xE5: ("a", "ring"),
    0xE8: ("e", "grave"), 0xE9: ("e", "acute"), 0xEA: ("e", "circ"),
    0xEB: ("e", "uml"),
    0xEC: ("i", "grave"), 0xED: ("i", "acute"), 0xEE: ("i", "circ"),
    0xEF: ("i", "uml"),
    0xF1: ("n", "tilde"),
    0xF2: ("o", "grave"), 0xF3: ("o", "acute"), 0xF4: ("o", "circ"),
    0xF5: ("o", "tilde"), 0xF6: ("o", "uml"),
    0xF9: ("u", "grave"), 0xFA: ("u", "acute"), 0xFB: ("u", "circ"),
    0xFC: ("u", "uml"),
    0xFD: ("y", "acute"), 0xFF: ("y", "uml"),
}

# The dotted i loses its dot under a mark: 'í' is not 'i' with two things on
# top. Base glyphs for those are the dotless body, drawn here.
DOTLESS = {
    8: ["........", "........", "..xx....", "..xx....", "..xx....", "..xx....",
        ".xxxx...", "........"],
    16: ["........", "........", "........", "........", "........", "..xxx...",
         "...xx...", "...xx...", "...xx...", "...xx...", "...xx...", "..xxxx..",
         "........", "........", "........", "........"],
}

# --------------------------------------------------------------------------
#  Drawn by hand: everything with nothing to compose from.
#  8x8 first; the 16-row versions are the same picture stretched, which is
#  honest for these shapes and saves drawing each one twice.
# --------------------------------------------------------------------------
DRAWN = {
    0x80: ["..xxxx..", ".xx...x.", "xxxx....", ".xx.....",    # euro
           "xxxx....", ".xx...x.", "..xxxx..", "........"],
    0xA0: ["........"] * 8,                                   # nbsp
    0xA1: ["...xx...", "........", "...xx...", "...xx...",    # inverted !
           "...xx...", "...xx...", "...xx...", "........"],
    0xA2: ["...x....", "..xxxx..", ".x.x....", ".x.x....",    # cent
           ".x.x....", "..xxxx..", "...x....", "........"],
    0xA3: ["..xxx...", ".xx..x..", ".xx.....", "xxxxx...",    # pound
           ".xx.....", ".xx..x..", "xxx.xx..", "........"],
    0xA4: ["........", "x.....x.", ".xxxxx..", ".x...x..",    # currency
           ".xxxxx..", "x.....x.", "........", "........"],
    0xA5: ["xx...xx.", ".xx.xx..", "..xxx...", ".xxxxx..",    # yen
           "..xxx...", ".xxxxx..", "..xxx...", "........"],
    0xA6: ["...xx...", "...xx...", "...xx...", "........",    # broken bar
           "...xx...", "...xx...", "...xx...", "........"],
    0xA7: ["..xxx...", ".xx..x..", "..xx....", ".xxxx...",    # section
           "..xx.x..", ".x..xx..", "..xxx...", "........"],
    0xA8: [".x..x...", "........", "........", "........",    # diaeresis
           "........", "........", "........", "........"],
    0xA9: ["..xxx...", ".x...x..", "x.xxx.x.", "x.x...x.",    # copyright
           "x.xxx.x.", ".x...x..", "..xxx...", "........"],
    0xAA: ["..xxx...", "....xx..", "..xxxx..", ".xx.xx..",    # feminine ordinal
           "..xxxx..", "........", ".xxxxx..", "........"],
    0xAB: ["........", "..x..x..", ".x..x...", "x..x....",    # <<
           ".x..x...", "..x..x..", "........", "........"],
    0xAC: ["........", "........", "xxxxxx..", "....xx..",    # not sign
           "....xx..", "........", "........", "........"],
    0xAD: ["........", "........", "........", ".xxxx...",    # soft hyphen
           "........", "........", "........", "........"],
    0xAE: ["..xxx...", ".x...x..", "x.xxx.x.", "x.xx..x.",    # registered
           "x.x.x.x.", ".x...x..", "..xxx...", "........"],
    0xAF: ["xxxxxx..", "........", "........", "........",    # macron
           "........", "........", "........", "........"],
    0xB0: ["..xxx...", ".xx.xx..", ".xx.xx..", "..xxx...",    # degree
           "........", "........", "........", "........"],
    0xB1: ["...xx...", "...xx...", ".xxxxxx.", "...xx...",    # plus-minus
           "...xx...", "........", ".xxxxxx.", "........"],
    0xB2: [".xxx....", "x...x...", "...xx...", "..x.....",    # superscript 2
           "xxxxx...", "........", "........", "........"],
    0xB3: ["xxxx....", "...xx...", "..xx....", "...xx...",    # superscript 3
           "xxxx....", "........", "........", "........"],
    0xB4: ["..xx....", ".xx.....", "........", "........",    # acute accent
           "........", "........", "........", "........"],
    0xB5: ["........", "........", ".x...x..", ".x...x..",    # micro
           ".x...x..", ".xxxxx..", ".x......", ".x......"],
    0xB6: [".xxxxx..", "xx.xx...", "xx.xx...", ".xxxx...",    # pilcrow
           "..xxx...", "..xxx...", "..xxx...", "........"],
    0xB7: ["........", "........", "........", "..xx....",    # middle dot
           "..xx....", "........", "........", "........"],
    0xB8: ["........", "........", "........", "........",    # cedilla
           "........", "........", "..xx....", ".xx....."],
    0xB9: ["..xx....", ".xxx....", "..xx....", "..xx....",    # superscript 1
           ".xxxx...", "........", "........", "........"],
    0xBA: ["..xxx...", ".xx.xx..", ".xx.xx..", "..xxx...",    # masculine ordinal
           "........", ".xxxxx..", "........", "........"],
    0xBB: ["........", ".x..x...", "..x..x..", "...x..x.",    # >>
           "..x..x..", ".x..x...", "........", "........"],
    0xBC: [".x...x..", ".x..x...", ".x.x....", "..x.xx..",    # 1/4
           ".x.x..x.", "x.x.xxx.", "..x...x.", "........"],
    0xBD: [".x...x..", ".x..x...", ".x.x....", "..x.xxx.",    # 1/2
           ".x.x...x", "x.x..xx.", "..x..xxx", "........"],
    0xBE: ["xx...x..", "..x.x...", ".x.x....", "xx.x.xx.",    # 3/4
           ".x.x..x.", "x.x.xxx.", "..x...x.", "........"],
    0xBF: ["...xx...", "........", "...xx...", "..xx....",    # inverted ?
           ".xx.....", ".xx..x..", "..xxx...", "........"],
    0xC6: ["xxxxxx..", "x.xx....", "x.xx....", "xxxxx...",    # AE
           "x.xx....", "x.xx....", "x.xxxx..", "........"],
    0xC7: ["..xxx...", ".xx..x..", ".xx.....", ".xx.....",    # C cedilla
           ".xx..x..", "..xxx...", "...xx...", "..xx...."],
    0xD0: [".xxxx...", ".xx.xx..", "xxx..xx.", "xxx..xx.",    # Eth
           ".xx..xx.", ".xx.xx..", ".xxxx...", "........"],
    0xD7: ["........", "x.....x.", ".xx.xx..", "..xxx...",    # multiply
           ".xx.xx..", "x.....x.", "........", "........"],
    0xD8: ["..xxx.x.", ".xx.xxx.", "xx.xxxx.", "xx.x.xx.",    # O slash
           "xxx..xx.", ".xxxxx..", "x.xxx...", "........"],
    0xDE: [".xx.....", ".xxxx...", ".xx..xx.", ".xx..xx.",    # Thorn
           ".xxxx...", ".xx.....", ".xx.....", "........"],
    0xDF: ["..xxx...", ".xx.xx..", ".xx.xx..", ".xxxx...",    # sharp s
           ".xx.xx..", ".xx..xx.", ".xxxxx..", "........"],
    0xE6: ["........", "........", "xxxxx...", "..x.xx..",    # ae
           "xxxxxx..", "x.x.....", "xxxxxx..", "........"],
    0xE7: ["........", "........", "..xxxx..", ".xx.....",    # c cedilla
           ".xx.....", "..xxxx..", "...xx...", "..xx...."],
    0xF0: [".xx.x...", "..xxx...", ".x..xx..", "xxxx.x..",    # eth
           "x...xx..", "xx..xx..", ".xxxx...", "........"],
    0xF7: ["........", "...xx...", "........", ".xxxxxx.",    # divide
           "........", "...xx...", "........", "........"],
    0xF8: ["........", "........", "..xxxx.x", ".xx.xxx.",    # o slash
           ".xxx.xx.", "x.xxxx..", "........", "........"],
    0xFE: [".xx.....", ".xx.....", ".xxxx...", ".xx..xx.",    # thorn
           ".xxxx...", ".xx.....", ".xx.....", "........"],
}


def stretch(rows8):
    """An 8-row picture as a 16-row one: two rows each, centred vertically."""
    out = ["........"] * 16
    for i, r in enumerate(rows8):
        out[i * 2] = r
        out[i * 2 + 1] = r if r.strip(".") else "........"
    return out


def build(basic, arial):
    """Return {code: (rows8, rows16)} for every code in LO..HI."""
    out = {}
    for code in range(LO, HI + 1):
        if code in DRAWN:
            pic = DRAWN[code]
            out[code] = ([bits_lsb(r) for r in pic],
                         [bits_msb(r) for r in stretch(pic)])
            continue
        if code in COMPOSED:
            ch, accent = COMPOSED[code]
            upper = ch.isupper()
            if ch in "iI":
                b8  = [bits_lsb(r) for r in DOTLESS[8]]
                b16 = [bits_msb(r) for r in DOTLESS[16]]
                if upper:
                    b8  = basic.get(ord("I"), [0] * 8)
                    b16 = arial.get(ord("I"), [0] * 16)
            else:
                b8  = basic.get(ord(ch), [0] * 8)
                b16 = arial.get(ord(ch), [0] * 16)
            out[code] = (compose8(b8, accent, upper), compose16(b16, accent, upper))
            continue
        out[code] = ([0] * 8, [0] * 16)     # blank: nothing types it
    return out


NAMES = {
    0xA1: "inverted !", 0xA3: "pound", 0xA7: "section", 0xB0: "degree",
    0xB5: "micro", 0xBF: "inverted ?", 0xDF: "sharp s", 0xE4: "a diaeresis",
    0xE7: "c cedilla", 0xF1: "n tilde", 0xF6: "o diaeresis", 0xFC: "u diaeresis",
}


def emit(table, path):
    L = []
    L.append("/* Generated by mkfont.py -- do not edit by hand.")
    L.append(" *")
    L.append(" * The Latin-1 supplement (0xA0..0xFF) for both faces, so a keyboard")
    L.append(" * layout that types 'a' with an umlaut over it has something to draw.")
    L.append(" * Almost all of it is the ASCII letter with a mark composed over it;")
    L.append(" * the symbols with nothing to compose from are drawn in mkfont.py.")
    L.append(" *")
    L.append(" * Same packing conventions as kernel/font8x8.c, which means two")
    L.append(" * different ones: bit 0 is the leftmost pixel in the 8x8 face and")
    L.append(" * bit 7 is the leftmost in the 8x16 face.")
    L.append(" */")
    L.append("")
    L.append("/* 0x80..0xFF, indexed as [c - 0x80]. */")
    L.append("const unsigned char font8x8_latin1[128][8] = {")
    for code in range(LO, HI + 1):
        rows, _ = table[code]
        note = NAMES.get(code, "")
        L.append("    { %s },%s" % (",".join("0x%02X" % v for v in rows),
                                    ("   /* %s */" % note) if note else ""))
    L.append("};")
    L.append("")
    L.append("const unsigned char font_8x16_latin1[128][16] = {")
    for code in range(LO, HI + 1):
        _, rows = table[code]
        L.append("    { %s }," % ",".join("0x%02X" % v for v in rows))
    L.append("};")
    L.append("")
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(L))


def main():
    basic, arial = parse_fonts(SRC)
    if len(basic) < 90 or len(arial) < 90:
        print("mkfont: only parsed %d/%d glyphs out of %s -- the table format "
              "must have changed" % (len(basic), len(arial), SRC), file=sys.stderr)
        return 1
    table = build(basic, arial)
    emit(table, OUT)
    drawn = sum(1 for c in range(LO, HI + 1) if c in DRAWN)
    comp = sum(1 for c in range(LO, HI + 1) if c in COMPOSED)
    blank = 128 - drawn - comp
    print("mkfont: %s -- %d composed, %d drawn, %d blank" % (OUT, comp, drawn, blank))
    return 0


if __name__ == "__main__":
    sys.exit(main())
