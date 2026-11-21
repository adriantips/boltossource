#!/usr/bin/env python3
"""Talk to the BoltOS Web Engine the way a Mini does, from this Mac.

    probe.py URL [--host localhost:7310] [--key K] [--size 1024x530] [--secs 8]
             [--out DIR] [--scroll N] [--click X,Y] [--type TEXT]

Connects, opens URL, acknowledges every frame at once (a decoder that is
never late), and saves what arrived: DIR/stream.h264 (Annex B, playable with
ffplay) and DIR/last.png, the page as it ended up (needs ffmpeg). Prints the
page's state as it changes, and how many frames and bytes it took.
"""
import argparse, os, socket, struct, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("url")
ap.add_argument("--host", default="localhost:7310")
ap.add_argument("--key", default=None)
ap.add_argument("--size", default="1024x530")
ap.add_argument("--secs", type=float, default=8)
ap.add_argument("--out", default=None)
ap.add_argument("--scroll", type=int, default=0, help="wheel notches down, after the page loads")
ap.add_argument("--click", default=None)
ap.add_argument("--type", default=None)
ap.add_argument("--ffmpeg", default="ffmpeg")
a = ap.parse_args()

root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
key = a.key
if key is None:
    try:
        key = open(os.path.join(root, "build/webengine/key")).read().strip()
    except OSError:
        key = ""
out = a.out or os.path.join(root, "build/webengine/probe")
os.makedirs(out, exist_ok=True)
w, h = (int(v) for v in a.size.split("x"))
host, port = a.host.rsplit(":", 1)
s = socket.create_connection((host, int(port)), timeout=30)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)


def send(t, p=b""):
    s.sendall(struct.pack("<BI", t, len(p)) + p)


def recv_exact(n):
    b = bytearray()
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return bytes(b)


send(0x01, struct.pack("<HHHH", 1, w, h, 0) + key.encode())
t0 = time.monotonic()
navigated = False
stream = open(os.path.join(out, "stream.h264"), "wb")
frames = keys = total = 0
sizes = []
last_frame_t = t0
did_scroll = did_click = False
s.settimeout(0.2)
while time.monotonic() - t0 < a.secs:
    try:
        t, n = struct.unpack("<BI", recv_exact(5))
        p = recv_exact(n) if n else b""
    except socket.timeout:
        t = None
    except EOFError:
        print("closed")
        break
    now = time.monotonic() - t0
    if t == 0x81:
        print(f"{now:6.2f} hello {struct.unpack('<HH', p[:4])} {p[4:].decode()}")
        if not navigated:
            send(0x02, a.url.encode())
            navigated = True
    elif t == 0x82:
        o = 6
        sps_len = struct.unpack(">H", p[o:o + 2])[0]
        sps = p[o + 2:o + 2 + sps_len]
        o += 2 + sps_len + 1
        pps_len = struct.unpack(">H", p[o:o + 2])[0]
        pps = p[o + 2:o + 2 + pps_len]
        stream.write(b"\0\0\0\1" + sps + b"\0\0\0\1" + pps)
    elif t == 0x83:
        no, key_ = struct.unpack("<IB", p[:5])
        au = p[5:]
        o = 0
        while o + 4 <= len(au):
            ln = struct.unpack(">I", au[o:o + 4])[0]
            stream.write(b"\0\0\0\1" + au[o + 4:o + 4 + ln])
            o += 4 + ln
        frames += 1
        keys += key_
        total += len(au)
        sizes.append(len(au))
        last_frame_t = now
        if os.environ.get("PROBE_V"):
            print(f"{now:6.2f} frame {no} {len(au)}B key={key_}")
        send(0x20, struct.pack("<I", no))
    elif t == 0x84:
        loading, back, fwd = p[0], p[1], p[2]
        url, _, title = p[4:].partition(b"\0")
        print(f"{now:6.2f} state loading={loading} back={back} fwd={fwd} {url.decode()!r} {title.decode()!r}")
    elif t == 0x85:
        cw, ch, hx, hy = struct.unpack("<HHHH", p[:8])
        print(f"{now:6.2f} cursor {cw}x{ch} hot {hx},{hy}")
    elif t == 0x86:
        print(f"{now:6.2f} ERROR {p.decode()}")
        break
    if navigated and now > a.secs / 2:
        if a.scroll and not did_scroll:
            send(0x10, struct.pack("<hhB", w // 2, h // 2, 0))
            for _ in range(abs(a.scroll)):
                send(0x11, struct.pack("<bb", -1 if a.scroll > 0 else 1, 0))
            did_scroll = True
        if a.click and not did_click:
            x, y = (int(v) for v in a.click.split(","))
            send(0x10, struct.pack("<hhB", x, y, 0))
            send(0x10, struct.pack("<hhB", x, y, 1))
            send(0x10, struct.pack("<hhB", x, y, 0))
            if a.type:
                time.sleep(0.3)
                for c in a.type:
                    send(0x12, struct.pack("<IB", 0xFF0D if c == "\n" else ord(c), 0))
            did_click = True
stream.close()
el = time.monotonic() - t0
print(f"{frames} frames ({keys} key), {total / 1024:.0f} KiB, biggest {max(sizes or [0]) / 1024:.0f} KiB, "
      f"median {sorted(sizes)[len(sizes) // 2] / 1024 if sizes else 0:.1f} KiB")
png = os.path.join(out, "last.png")
r = subprocess.run([a.ffmpeg, "-y", "-loglevel", "error", "-i", os.path.join(out, "stream.h264"), "-update", "1",
                    "-frames:v", "100000", png], capture_output=True, text=True)
print(png if r.returncode == 0 else r.stderr[-400:])
