#!/usr/bin/env python3
"""A stand-in for the BoltOS Web Engine (webengine/boltweb.py), for testing a
client without Firefox: it speaks the same protocol, sends a looping H.264
test picture (ffmpeg's testsrc2 through libx264, shaped as boltweb shapes it)
and, to a client that asks for sound, a 440 Hz tone; and it prints every
message the client sends back (navigation, pointer, keys, acknowledgements
counted).

    fakeengine.py [--port 7310] [--size 1280x720]

Under QEMU's user networking a guest reaches it at 10.0.2.2.
"""
import argparse, math, os, re, socket, struct, subprocess, sys, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "build", ".tools", "env", "bin")
FFMPEG = os.path.join(TOOLS, "ffmpeg") if os.path.exists(os.path.join(TOOLS, "ffmpeg")) else "ffmpeg"
NAMES = {0x01: "HELLO", 0x02: "NAV", 0x03: "BACK", 0x04: "FORWARD", 0x05: "RELOAD", 0x06: "STOP",
         0x10: "MOUSE", 0x11: "WHEEL", 0x12: "KEY", 0x20: "ACK", 0x21: "KEYFRAME"}


def encode(w, h):
    out = os.path.join(tempfile.mkdtemp(), "t.h264")
    subprocess.run([FFMPEG, "-v", "error", "-f", "lavfi", "-i", "testsrc2=s=%dx%d:r=25:d=8" % (w, h),
                    "-c:v", "libx264", "-profile:v", "baseline", "-pix_fmt", "yuv420p",
                    "-x264-params", "keyint=50:bframes=0:ref=1:no-deblock=1:repeat-headers=1", out], check=True)
    data = open(out, "rb").read()
    starts = [m.end() for m in re.finditer(b"\x00\x00\x01", data)]
    nals = []
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else len(data)
        n = data[s:e].rstrip(b"\x00")
        if n:
            nals.append(n)
    aus, sps, pps, cur, key = [], None, None, b"", False
    for n in nals:
        t = n[0] & 31
        if t == 7: sps = n
        elif t == 8: pps = n
        elif t in (1, 5):
            aus.append((t == 5, sps, pps, struct.pack(">I", len(n)) + n))
    return aus


def avcc(sps, pps):
    return (bytes([1, sps[1], sps[2], sps[3], 0xFF, 0xE1]) + struct.pack(">H", len(sps)) + sps +
            bytes([1]) + struct.pack(">H", len(pps)) + pps)


def serve(conn, aus):
    lock = threading.Lock()
    def send(kind, payload=b""):
        with lock:
            conn.sendall(struct.pack("<BI", kind, len(payload)) + payload)
    def recv(n):
        b = b""
        while len(b) < n:
            c = conn.recv(n - len(b))
            if not c: raise ConnectionError
            b += c
        return b
    kind, n = struct.unpack("<BI", recv(5))
    p = recv(n)
    ver, w, h, flags = struct.unpack("<HHHH", p[:8])
    print("HELLO: protocol %d, %dx%d, flags %d, key %r" % (ver, w, h, flags, p[8:].decode(errors="replace")), flush=True)
    send(0x81, struct.pack("<HH", w, h) + b"Firefox 0 (fake engine)")
    url = ["about:blank"]
    send(0x84, bytes([0, 0, 0, 0]) + url[0].encode() + b"\0Fake engine")
    stop = [False]
    acks = [0]
    acked = [0]                  # the last frame the client showed
    want_key = [False]

    def frames():
        # As boltweb does: never more than two frames unacknowledged, and a
        # keyframe as soon as the client asks for one.
        no, i = 0, 0
        while not stop[0]:
            if want_key[0]:
                want_key[0] = False
                while not aus[i][0]: i = (i + 1) % len(aus)
            key, sps, pps, au = aus[i]
            if not key and no - acked[0] >= 2:
                time.sleep(0.005); continue
            if key: send(0x82, avcc(sps, pps))
            no += 1
            send(0x83, struct.pack("<IB", no, 1 if key else 0) + au)
            i = (i + 1) % len(aus)
            time.sleep(0.04)
    def sound():
        ph, t0, sent = 0.0, time.time(), 0
        while not stop[0]:
            blk = bytearray()
            for _ in range(960):
                v = int(6000 * math.sin(ph)); ph += 2 * math.pi * 440 / 48000
                blk += struct.pack("<hh", v, v)
            send(0x87, bytes(blk))
            sent += 1
            time.sleep(max(0, t0 + sent * 0.02 - time.time()))
    threading.Thread(target=frames, daemon=True).start()
    if flags & 1:
        threading.Thread(target=sound, daemon=True).start()
    try:
        while True:
            kind, n = struct.unpack("<BI", recv(5))
            p = recv(n) if n else b""
            if kind == 0x21:
                want_key[0] = True
            if kind == 0x20:
                acked[0] = max(acked[0], struct.unpack("<I", p[:4])[0])
                acks[0] += 1
                if acks[0] % 100 == 0: print("ACK x%d" % acks[0], flush=True)
                continue
            if kind == 0x10: detail = "x=%d y=%d buttons=%d" % struct.unpack("<hhB", p[:5])
            elif kind == 0x12: detail = "keysym=0x%x mods=%d" % struct.unpack("<IB", p[:5])
            elif kind == 0x11: detail = "dy=%d dx=%d" % struct.unpack("<bb", p[:2])
            elif kind == 0x02:
                detail = p.decode(errors="replace"); url[0] = detail
                send(0x84, bytes([0, 1, 0, 0]) + detail.encode() + b"\0" + b"Fake: " + detail.encode())
            else: detail = p.hex()
            print("%s %s" % (NAMES.get(kind, hex(kind)), detail), flush=True)
    finally:
        stop[0] = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=7310)
    ap.add_argument("--size", default="1280x720")
    a = ap.parse_args()
    w, h = map(int, a.size.split("x"))
    aus = encode(w, h)
    print("fake engine on port %d, %d frames of %dx%d" % (a.port, len(aus), w, h), flush=True)
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", a.port)); s.listen(1)      # QEMU's user networking reaches the host here
    while True:
        c, addr = s.accept()
        print("client %s" % (addr,), flush=True)
        try: serve(c, aus)
        except (ConnectionError, OSError) as e: print("gone: %r" % e, flush=True)
        c.close()


if __name__ == "__main__":
    main()
