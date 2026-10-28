#!/usr/bin/env python3
"""Log in to BoltOS Mini's VNC server and save what it shows, as a PNG.

    vncshot.py HOST[:PORT] out.png [--password boltos] [--keys "text"] [--wait 0.5]

It speaks the same RFB 3.8 + VNC-password handshake macOS Screen Sharing
does, asks for Hextile, and decodes the first full update. --keys types text
(\\n for Return) before taking the picture, so a test can drive the shell.
Uses Pillow if it is there to write the PNG, and a PPM otherwise.
"""
import socket, struct, sys, time

IP = [58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6,64,56,48,40,32,24,16,8,
      57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7]
FP = [40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,38,6,46,14,54,22,62,30,37,5,45,13,53,21,61,29,
      36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,34,2,42,10,50,18,58,26,33,1,41,9,49,17,57,25]
E = [32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,12,13,14,15,16,17,16,17,18,19,20,21,20,21,22,23,
     24,25,24,25,26,27,28,29,28,29,30,31,32,1]
P = [16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,2,8,24,14,32,27,3,9,19,13,30,6,22,11,4,25]
PC1 = [57,49,41,33,25,17,9,1,58,50,42,34,26,18,10,2,59,51,43,35,27,19,11,3,60,52,44,36,63,55,
       47,39,31,23,15,7,62,54,46,38,30,22,14,6,61,53,45,37,29,21,13,5,28,20,12,4]
PC2 = [14,17,11,24,1,5,3,28,15,6,21,10,23,19,12,4,26,8,16,7,27,20,13,2,41,52,31,37,47,55,30,40,
       51,45,33,48,44,49,39,56,34,53,46,42,50,36,29,32]
SH = [1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1]
S = [
 "14 4 13 1 2 15 11 8 3 10 6 12 5 9 0 7 0 15 7 4 14 2 13 1 10 6 12 11 9 5 3 8 4 1 14 8 13 6 2 11 15 12 9 7 3 10 5 0 15 12 8 2 4 9 1 7 5 11 3 14 10 0 6 13",
 "15 1 8 14 6 11 3 4 9 7 2 13 12 0 5 10 3 13 4 7 15 2 8 14 12 0 1 10 6 9 11 5 0 14 7 11 10 4 13 1 5 8 12 6 9 3 2 15 13 8 10 1 3 15 4 2 11 6 7 12 0 5 14 9",
 "10 0 9 14 6 3 15 5 1 13 12 7 11 4 2 8 13 7 0 9 3 4 6 10 2 8 5 14 12 11 15 1 13 6 4 9 8 15 3 0 11 1 2 12 5 10 14 7 1 10 13 0 6 9 8 7 4 15 14 3 11 5 2 12",
 "7 13 14 3 0 6 9 10 1 2 8 5 11 12 4 15 13 8 11 5 6 15 0 3 4 7 2 12 1 10 14 9 10 6 9 0 12 11 7 13 15 1 3 14 5 2 8 4 3 15 0 6 10 1 13 8 9 4 5 11 12 7 2 14",
 "2 12 4 1 7 10 11 6 8 5 3 15 13 0 14 9 14 11 2 12 4 7 13 1 5 0 15 10 3 9 8 6 4 2 1 11 10 13 7 8 15 9 12 5 6 3 0 14 11 8 12 7 1 14 2 13 6 15 0 9 10 4 5 3",
 "12 1 10 15 9 2 6 8 0 13 3 4 14 7 5 11 10 15 4 2 7 12 9 5 6 1 13 14 0 11 3 8 9 14 15 5 2 8 12 3 7 0 4 10 1 13 11 6 4 3 2 12 9 5 15 10 11 14 1 7 6 0 8 13",
 "4 11 2 14 15 0 8 13 3 12 9 7 5 10 6 1 13 0 11 7 4 9 1 10 14 3 5 12 2 15 8 6 1 4 11 13 12 3 7 14 10 15 6 8 0 5 9 2 6 11 13 8 1 4 10 7 9 5 0 15 14 2 3 12",
 "13 2 8 4 6 15 11 1 10 9 3 14 5 0 12 7 1 15 13 8 10 3 7 4 12 5 6 11 0 14 9 2 7 11 4 1 9 12 14 2 0 6 10 13 15 3 5 8 2 1 14 7 4 10 8 13 15 12 9 0 3 5 6 11"]
S = [[int(x) for x in s.split()] for s in S]


def perm(v, t, n):
    o = 0
    for b in t:
        o = (o << 1) | ((v >> (n - b)) & 1)
    return o


def des(key, block):
    k = perm(key, PC1, 64)
    c, d = k >> 28, k & 0xFFFFFFF
    subs = []
    for s in SH:
        for _ in range(s):
            c = ((c << 1) | (c >> 27)) & 0xFFFFFFF
            d = ((d << 1) | (d >> 27)) & 0xFFFFFFF
        subs.append(perm((c << 28) | d, PC2, 56))
    x = perm(block, IP, 64)
    l, r = x >> 32, x & 0xFFFFFFFF
    for k in subs:
        e = perm(r, E, 32) ^ k
        o = 0
        for i in range(8):
            six = (e >> (42 - 6 * i)) & 63
            o = (o << 4) | S[i][((six >> 4) & 2 | six & 1) * 16 + ((six >> 1) & 15)]
        l, r = r, l ^ perm(o, P, 32)
    return perm((r << 32) | l, FP, 64)


def vnc_response(password, chal):
    key = 0
    pw = password.encode()[:8].ljust(8, b"\0")
    for b in pw:
        key = (key << 8) | int("{:08b}".format(b)[::-1], 2)
    out = b""
    for h in range(2):
        out += des(key, int.from_bytes(chal[8 * h:8 * h + 8], "big")).to_bytes(8, "big")
    return out


def recv(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise SystemExit("server closed the connection")
        b += c
    return b


KEYSYM = {"\n": 0xFF0D, "\b": 0xFF08, "\t": 0xFF09, "\x1b": 0xFF1B}


def main(argv):
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("out")
    ap.add_argument("--password", default="boltos")
    ap.add_argument("--keys", default="")
    ap.add_argument("--click", default="", help="x,y to click before the picture")
    ap.add_argument("--wait", type=float, default=0.6)
    a = ap.parse_args(argv)
    host, _, port = a.host.partition(":")
    s = socket.create_connection((host, int(port or 5900)), timeout=10)
    assert recv(s, 12).startswith(b"RFB 003.")
    s.sendall(b"RFB 003.008\n")
    n = recv(s, 1)[0]
    types = recv(s, n)
    assert 2 in types, types
    s.sendall(bytes([2]))
    s.sendall(vnc_response(a.password, recv(s, 16)))
    if struct.unpack(">I", recv(s, 4))[0] != 0:
        raise SystemExit("login refused: " + recv(s, struct.unpack(">I", recv(s, 4))[0]).decode())
    s.sendall(b"\x01")
    w, h = struct.unpack(">HH", recv(s, 4))
    recv(s, 16)
    name = recv(s, struct.unpack(">I", recv(s, 4))[0]).decode()
    s.sendall(struct.pack(">BxHi", 2, 1, 5))                    # Hextile
    t0 = time.time()
    for ch in a.keys.encode().decode("unicode_escape"):
        sym = KEYSYM.get(ch, ord(ch))
        s.sendall(struct.pack(">BBxxI", 4, 1, sym) + struct.pack(">BBxxI", 4, 0, sym))
        time.sleep(0.03)
    if a.click:
        x, y = (int(v) for v in a.click.split(","))
        s.sendall(struct.pack(">BBHH", 5, 0, x, y) + struct.pack(">BBHH", 5, 1, x, y) +
                  struct.pack(">BBHH", 5, 0, x, y))
    time.sleep(a.wait)
    s.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, w, h))
    fb = bytearray(w * h * 4)
    got = 0
    while got < w * h:
        mt = recv(s, 1)[0]
        if mt != 0:
            raise SystemExit("unexpected message %d" % mt)
        recv(s, 1)
        nr = struct.unpack(">H", recv(s, 2))[0]
        for _ in range(nr):
            x, y, rw, rh, enc = struct.unpack(">HHHHi", recv(s, 12))
            if enc == 0:
                data = recv(s, rw * rh * 4)
                for r in range(rh):
                    o = ((y + r) * w + x) * 4
                    fb[o:o + rw * 4] = data[r * rw * 4:(r + 1) * rw * 4]
                got += rw * rh
            elif enc == 5:
                for ty in range(y, y + rh, 16):
                    for tx in range(x, x + rw, 16):
                        tw, th = min(16, x + rw - tx), min(16, y + rh - ty)
                        sub = recv(s, 1)[0]
                        if sub & 1:
                            data = recv(s, tw * th * 4)
                            for r in range(th):
                                o = ((ty + r) * w + tx) * 4
                                fb[o:o + tw * 4] = data[r * tw * 4:(r + 1) * tw * 4]
                        elif sub == 2:
                            px = recv(s, 4)
                            for r in range(th):
                                o = ((ty + r) * w + tx) * 4
                                fb[o:o + tw * 4] = px * tw
                        else:
                            raise SystemExit("hextile subencoding %d not handled here" % sub)
                got += rw * rh
            elif enc == -239:
                recv(s, rw * rh * 4 + ((rw + 7) // 8) * rh)
            elif enc == -223:
                pass
            else:
                raise SystemExit("encoding %d not handled here" % enc)
    dt = time.time() - t0 - a.wait
    rgb = bytearray(w * h * 3)
    rgb[0::3], rgb[1::3], rgb[2::3] = fb[2::4], fb[1::4], fb[0::4]
    try:
        from PIL import Image
        Image.frombytes("RGB", (w, h), bytes(rgb)).save(a.out)
    except ImportError:
        with open(a.out, "wb") as f:
            f.write(b"P6 %d %d 255\n" % (w, h) + rgb)
    print("%s: %dx%d from %r, full frame in %.2f s" % (a.out, w, h, name, dt))


if __name__ == "__main__":
    main(sys.argv[1:])
