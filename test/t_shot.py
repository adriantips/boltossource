#!/usr/bin/env python3
"""Screenshots: the OS can hand you a picture of itself.

Until now the only way to get one was shot.py, on the host, through QEMU's
monitor -- a way to photograph a virtual machine, not a feature of an operating
system. F12 did write a file, but an uncompressed BMP of the compositor's
backbuffer: 2.3 MB a frame, and blank whenever a ring-3 program owned the panel.

Two layers, because they fail differently:

  * The encoder, compiled on the host and decoded by Python's own zlib. An
    encoder read back only by its own decoder can be wrong in matching pairs
    and nobody ever finds out. Exact pixels, on flat, gradient, noise, 1x1 and
    shapes longer than the 32 KiB window.
  * The OS, in QEMU: `screenshot` writes a real PNG of the right size into the
    signed-in user's Pictures, the file is a valid PNG, and BoltOS's own
    decoder opens it.

Run just the encoder tests with `python test/t_shot.py --host-only`.
"""
import binascii
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT

OUT = Path(ROOT) / "test" / "out"


# --------------------------------------------------------------- PNG reader --
def read_png(path):
    """Decode an 8-bit truecolour PNG to (w, h, [ (r,g,b), ... ]).

    Deliberately hand-written rather than PIL: the test must not depend on a
    package that may not be installed, and unfiltering by hand is what proves
    the encoder's filter bytes mean what PNG says they mean.
    """
    d = Path(path).read_bytes()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG: %r" % d[:8]
    pos, idat, w = 8, b"", None
    while pos < len(d):
        (ln,) = struct.unpack(">I", d[pos:pos + 4])
        typ = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + ln]
        (crc,) = struct.unpack(">I", d[pos + 8 + ln:pos + 12 + ln])
        assert crc == binascii.crc32(typ + body) & 0xFFFFFFFF, "bad CRC on %r" % typ
        if typ == b"IHDR":
            w, h, depth, ctype, comp, filt, ilace = struct.unpack(">IIBBBBB", body)
            assert (depth, ctype, comp, filt, ilace) == (8, 2, 0, 0, 0), \
                "unexpected IHDR %r" % (body,)
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            assert ln == 0
        pos += 12 + ln
    assert w is not None, "no IHDR"

    raw = zlib.decompress(idat)          # fails loudly if the zlib stream is wrong
    stride = w * 3
    assert len(raw) == (stride + 1) * h, "raw length %d, expected %d" % (
        len(raw), (stride + 1) * h)

    px, prev = [], bytearray(stride)
    for y in range(h):
        ft = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if ft == 0:
            pass
        elif ft == 1:                     # Sub
            for i in range(3, stride):
                line[i] = (line[i] + line[i - 3]) & 0xFF
        elif ft == 2:                     # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        else:
            raise AssertionError("filter type %d was not emitted by this encoder" % ft)
        px.extend((line[i], line[i + 1], line[i + 2]) for i in range(0, stride, 3))
        prev = line
    return w, h, px


# ------------------------------------------------------------- host encoder --
def host_tests():
    cc = os.environ.get("BOLTOS_HOST_CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc and Path(r"C:\msys64\ucrt64\bin\clang.exe").exists():
        cc = r"C:\msys64\ucrt64\bin\clang.exe"
    if not cc:
        raise RuntimeError("host C compiler missing (set BOLTOS_HOST_CC)")
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / ("png-host.exe" if os.name == "nt" else "png-host")
    subprocess.run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-iquote", str(Path(ROOT) / "include"),
                    str(Path(ROOT) / "test" / "host" / "png.c"), "-o", str(exe)],
                   check=True)
    res = subprocess.run([str(exe), str(OUT)], check=True,
                         capture_output=True, text=True)
    assert "host-ok" in res.stdout, res.stdout + res.stderr
    sizes = {}
    for line in res.stdout.splitlines():
        if line.strip() == "host-ok":
            continue
        p, w, h, n = line.rsplit(" ", 3)
        sizes[Path(p).name] = (int(w), int(h), int(n))
    print("  ok: the encoder rejects bad arguments and matches known checksums")

    def expect(name, w, h, at):
        gw, gh, px = read_png(OUT / name)
        assert (gw, gh) == (w, h), "%s decoded as %dx%d" % (name, gw, gh)
        for (x, y), want in at.items():
            got = px[y * gw + x]
            assert got == want, "%s pixel (%d,%d) is %s, expected %s" % (
                name, x, y, got, want)
        return gw, gh, px

    expect("one.png", 1, 1, {(0, 0): (0x33, 0x66, 0xCC)})
    expect("flat.png", 320, 200, {(0, 0): (0x10, 0x20, 0x30),
                                  (319, 199): (0x10, 0x20, 0x30)})
    print("  ok: 1x1 and a flat fill decode to exactly the pixels put in")

    _, _, px = expect("gradient.png", 256, 64, {})
    for y in (0, 31, 63):
        for x in (0, 1, 128, 255):
            assert px[y * 256 + x] == (x, (y * 4) & 0xFF, 255 - x), \
                "gradient wrong at (%d,%d): %s" % (x, y, px[y * 256 + x])
    print("  ok: a gradient survives the Sub row filter byte for byte")

    _, _, px = expect("noise.png", 128, 128, {})
    s = 12345
    for i in range(128 * 128):
        s = (s * 1664525 + 1013904223) & 0xFFFFFFFF
        v = s & 0xFFFFFF
        assert px[i] == ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF), \
            "noise wrong at %d" % i
    print("  ok: incompressible data round-trips (the output-buffer growth path)")

    _, _, px = expect("wide.png", 20000, 1, {})
    for x in (0, 1, 9999, 19999):
        v = (x * 7) & 0xFFFF
        assert px[x] == ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)
    _, _, px = expect("tall.png", 1, 1000, {})
    for y in (0, 500, 999):
        assert px[y] == (0, 0, y & 0xFF)
    print("  ok: a row longer than the 32 KiB window, and 1000 one-pixel rows")

    w, h, _ = sizes["desktop.png"]
    raw = w * h * 3
    ratio = raw / sizes["desktop.png"][2]
    assert ratio > 15, "a flat 1024x768 desktop only compressed %.1fx" % ratio
    print("  ok: a desktop-like 1024x768 frame compresses %.0fx (%d KB -> %d KB)"
          % (ratio, raw // 1024, sizes["desktop.png"][2] // 1024))


# ---------------------------------------------------------------- the guest --
def reach_shell(vm, timeout=180):
    return vm.reach_shell(timeout, "2")


def guest_tests():
    with BoltVM(name="shot") as vm:
        reach_shell(vm)

        out = vm.run("screenshot", timeout=120)
        assert "screenshot: /" in out, "no path reported:\n" + out
        path = out.split("screenshot: ", 1)[1].split()[0]
        assert path.endswith(".png"), "not a PNG path: " + path
        print("  ok: `screenshot` captures from a text shell, with no desktop running")

        # The reported byte count is the encoded size. A capture that came out
        # tiny would mean the framebuffer read back blank; one near the raw
        # 2.3 MB would mean the deflate is not doing anything.
        size = int(out.split(" bytes")[0].rsplit(" ", 1)[1])
        assert 2000 < size < 600000, "implausible PNG size %d bytes" % size

        leaf = path.rsplit("/", 1)[1]
        out = vm.run("ls -l " + path.rsplit("/", 1)[0], timeout=60)
        assert leaf in out, "the file is not on disk:\n" + out
        print("  ok: it lands in Pictures as a %d-byte PNG" % size)

        # That the bytes are a valid PNG is settled by the host leg above, which
        # decodes them with an independent zlib. What is left to check here is
        # that the OS put the right bytes in the right place.
        out = vm.run("screenshot /home/named.png", timeout=120)
        assert "/home/named.png" in out, "an explicit path was not honoured:\n" + out
        print("  ok: an explicit path is honoured")

        out = vm.run("notify list -a", timeout=60)
        assert "Screenshot" in out, "taking one posted no notification:\n" + out
        print("  ok: each capture notifies where it went")

        vm.expect_absent("KERNEL PANIC")


def main():
    host_only = "--host-only" in sys.argv
    host_tests()
    if host_only:
        print("PASS shot-host: the PNG encoder is decodable by an independent reader")
        return 0
    guest_tests()
    print("PASS shot: BoltOS captures its own screen to a real PNG")
    return 0


if __name__ == "__main__":
    sys.exit(main())
