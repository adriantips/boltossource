#!/usr/bin/env python3
"""Measure what BoltOS Mini's screen sharing delivers, and check it is right.

    vncbench.py [HOST[:PORT]] [--enc zrle|hextile|raw] [--seconds 5]
                [--anim 0|1|none] [--verify]

It logs in like Screen Sharing, asks for the given encoding (Screen Sharing
itself takes zlib, with --depth32), and keeps one
incremental update request outstanding, as viewers do. With --anim it asks
the Pi (GET /anim) to change its screen every frame for the run -- mode 0
flips between Home and each app, mode 1 moves Home's selection -- and then
prints the updates per second and bytes that came back, next to the Pi's
own /perf counters.

--verify decodes every update into a picture and, at the end, compares it
with /screen.bmp pixel by pixel.
"""
import argparse, os, socket, struct, sys, threading, time, urllib.request, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vncshot import vnc_response, recv  # noqa: E402

ENC = {"zrle": 16, "zlib": 6, "hextile": 5, "raw": 0}


def http(host, path, timeout=10):
    with urllib.request.urlopen("http://%s%s" % (host, path), timeout=timeout) as r:
        return r.read()


class Viewer:
    def __init__(self, host, port, password, enc, decode, depth32=False):
        self.s = socket.create_connection((host, port), timeout=10)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        assert recv(self.s, 12).startswith(b"RFB 003.")
        self.s.sendall(b"RFB 003.008\n")
        n = recv(self.s, 1)[0]
        assert 2 in recv(self.s, n)
        self.s.sendall(bytes([2]))
        self.s.sendall(vnc_response(password, recv(self.s, 16)))
        if struct.unpack(">I", recv(self.s, 4))[0] != 0:
            raise SystemExit("login refused")
        self.s.sendall(b"\x01")
        self.w, self.h = struct.unpack(">HH", recv(self.s, 4))
        recv(self.s, 16)
        recv(self.s, struct.unpack(">I", recv(self.s, 4))[0])
        if depth32:                                   # what Screen Sharing sends
            self.s.sendall(struct.pack(">BxxxBBBBHHHBBBxxx", 0, 32, 32, 0, 1, 255, 255, 255, 16, 8, 0))
        self.cp = 4 if depth32 else 3                 # RFC 6143: 3-byte CPIXELs only at depth <= 24
        encs = [ENC[enc], 0, -239, -223]
        self.s.sendall(struct.pack(">BxH", 2, len(encs)) + b"".join(struct.pack(">i", e) for e in encs))
        self.decode = decode
        self.fb = bytearray(self.w * self.h * 4)
        self.z = zlib.decompressobj()
        self.zl = zlib.decompressobj()
        self.bytes = 0

    def rd(self, n):
        self.bytes += n
        return recv(self.s, n)

    def request(self, incremental=1):
        self.s.sendall(struct.pack(">BBHHHH", 3, incremental, 0, 0, self.w, self.h))

    def put(self, x, y, w, h, data, bpp=4):
        """data: w*h pixels of bpp bytes (3: CPIXEL b,g,r)."""
        if not self.decode:
            return
        fb, W = self.fb, self.w
        if bpp == 4:
            data = bytearray(data)
            data[3::4] = bytes(len(data) // 4)              # only the colour counts
        for r in range(h):
            row = data[r * w * bpp:(r + 1) * w * bpp]
            if bpp == 3:
                px = bytearray(w * 4)
                px[0::4], px[1::4], px[2::4] = row[0::3], row[1::3], row[2::3]
                row = px
            o = ((y + r) * W + x) * 4
            fb[o:o + w * 4] = row

    def update(self):
        """Read one FramebufferUpdate; returns its number of rects."""
        mt = self.rd(1)[0]
        if mt != 0:
            raise SystemExit("unexpected message %d" % mt)
        self.rd(1)
        nr = struct.unpack(">H", self.rd(2))[0]
        for _ in range(nr):
            x, y, w, h, enc = struct.unpack(">HHHHi", self.rd(12))
            if enc == 0:
                self.put(x, y, w, h, self.rd(w * h * 4))
            elif enc == 5:
                self.hextile(x, y, w, h)
            elif enc == 6:
                data = self.rd(struct.unpack(">I", self.rd(4))[0])
                raw = self.zl.decompress(data)                  # every rect is checked, decoded or not
                if len(raw) != w * h * 4:
                    raise SystemExit("zlib rect: %d bytes, not %d" % (len(raw), w * h * 4))
                self.put(x, y, w, h, raw)
            elif enc == 16:
                data = self.rd(struct.unpack(">I", self.rd(4))[0])
                if self.decode:
                    self.zrle(x, y, w, h, self.z.decompress(data))
            elif enc == -239:
                self.rd(w * h * 4 + ((w + 7) // 8) * h)
            elif enc == -223:
                pass
            else:
                raise SystemExit("encoding %d not handled here" % enc)
        return nr

    def hextile(self, x, y, w, h):
        for ty in range(y, y + h, 16):
            for tx in range(x, x + w, 16):
                tw, th = min(16, x + w - tx), min(16, y + h - ty)
                sub = self.rd(1)[0]
                if sub & 1:
                    self.put(tx, ty, tw, th, self.rd(tw * th * 4))
                elif sub == 2:
                    self.put(tx, ty, tw, th, self.rd(4) * (tw * th))
                else:
                    raise SystemExit("hextile subencoding %d not handled here" % sub)

    def zrle(self, x, y, w, h, d):
        p = 0
        cp = self.cp

        def rl():
            nonlocal p
            n = 1
            while True:
                b = d[p]; p += 1
                n += b
                if b != 255:
                    return n
        for ty in range(y, y + h, 64):
            for tx in range(x, x + w, 64):
                tw, th = min(64, x + w - tx), min(64, y + h - ty)
                sub = d[p]; p += 1
                if sub == 0:
                    self.put(tx, ty, tw, th, d[p:p + tw * th * cp], cp); p += tw * th * cp
                elif sub == 1:
                    self.put(tx, ty, tw, th, d[p:p + cp] * (tw * th), cp); p += cp
                elif 2 <= sub <= 16:
                    pal = [d[p + cp * i:p + cp * i + cp] for i in range(sub)]; p += cp * sub
                    bits = 1 if sub == 2 else 2 if sub <= 4 else 4
                    out = bytearray()
                    for r in range(th):
                        nb = (tw * bits + 7) // 8
                        row = d[p:p + nb]; p += nb
                        for c in range(tw):
                            bit = c * bits
                            idx = (row[bit // 8] >> (8 - bits - bit % 8)) & ((1 << bits) - 1)
                            out += pal[idx]
                    self.put(tx, ty, tw, th, out, cp)
                elif sub == 128:
                    out = bytearray()
                    while len(out) < tw * th * cp:
                        c = d[p:p + cp]; p += cp
                        out += c * rl()
                    self.put(tx, ty, tw, th, out, cp)
                elif sub >= 130:
                    n = sub - 128
                    pal = [d[p + cp * i:p + cp * i + cp] for i in range(n)]; p += cp * n
                    out = bytearray()
                    while len(out) < tw * th * cp:
                        b = d[p]; p += 1
                        out += pal[b & 127] * (rl() if b & 128 else 1)
                    self.put(tx, ty, tw, th, out, cp)
                else:
                    raise SystemExit("ZRLE subencoding %d" % sub)
        if p != len(d):
            raise SystemExit("ZRLE rect: %d bytes left over" % (len(d) - p))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default="10.77.0.1")
    ap.add_argument("--password", default="boltos")
    ap.add_argument("--enc", default="zrle", choices=sorted(ENC))
    ap.add_argument("--seconds", type=float, default=5)
    ap.add_argument("--anim", default="0")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--depth32", action="store_true", help="ask for Screen Sharing's pixel format")
    a = ap.parse_args(argv)
    host, _, port = a.host.partition(":")
    v = Viewer(host, int(port or 5900), a.password, a.enc, a.verify, a.depth32)
    v.request(0)
    v.update()                                        # the first, whole screen
    first = v.bytes
    http(host, "/perf")
    if a.anim != "none":
        http(host, "/anim?mode=%s&ms=%d" % (a.anim, int(a.seconds * 1000)))
    t0 = time.time()
    n = rects = empty = 0
    v.bytes = 0
    v.s.settimeout(1.0)
    while time.time() - t0 < a.seconds:
        v.request(1)
        try:
            k = v.update()
        except socket.timeout:
            break
        n += 1
        rects += k
    v.s.settimeout(10)
    dt = time.time() - t0
    perf = http(host, "/perf").decode()
    print("%s: first frame %d KiB; then %d updates in %.2f s = %.1f/s, %d rects, %.0f KiB (%.0f KiB/update, %.1f MB/s)" %
          (a.enc, first // 1024, n, dt, n / dt, rects, v.bytes / 1024, v.bytes / 1024 / max(n, 1), v.bytes / dt / 1e6))
    print("  the Pi says: " + perf.strip().replace("\n", "\n  the Pi says: "))
    if a.verify:
        time.sleep(1.0)                               # let the screen settle
        while True:
            v.request(1)
            v.s.settimeout(0.5)
            try:
                v.update()
            except socket.timeout:
                break
            finally:
                v.s.settimeout(10)
        bmp = http(host, "/screen.bmp", timeout=20)
        off = struct.unpack("<I", bmp[10:14])[0]
        bad = 0
        rows = []
        for y in range(v.h):
            src = bmp[off + (v.h - 1 - y) * v.w * 3: off + (v.h - y) * v.w * 3]
            row = v.fb[y * v.w * 4:(y + 1) * v.w * 4]
            if src[0::3] != row[0::4] or src[1::3] != row[1::4] or src[2::3] != row[2::4]:
                bad += 1
                rows.append(y)
        print("  verify: %s" % ("every row matches /screen.bmp" if not bad else "%d rows differ (%s)" % (bad, rows[:12])))


if __name__ == "__main__":
    main(sys.argv[1:])
