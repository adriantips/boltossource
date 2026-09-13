#!/usr/bin/env python3
"""Screen recording: an animated PNG the OS wrote itself.

Two layers, because they fail differently:

  * The APNG encoder, compiled on the host and rebuilt here frame by frame with
    Python's own zlib. Every frame after the first is a sub-rectangle, and a
    rectangle written at the wrong offset is invisible in a still image and
    ruins the animation -- so this reconstructs the canvas the way a player
    would and compares pixels.
  * The OS in QEMU: `rec start`, make the screen change, `rec stop`, and a file
    in ~/Videos with more than one frame in it.

Run just the encoder half with `python test/t_rec.py --host-only`.
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
LOGIN_USER, LOGIN_PASS = "user", "bolt"


# ------------------------------------------------------------- APNG reader --
def read_chunks(path):
    d = Path(path).read_bytes()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG: %r" % d[:8]
    pos, out = 8, []
    while pos < len(d):
        (ln,) = struct.unpack(">I", d[pos:pos + 4])
        typ = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + ln]
        (crc,) = struct.unpack(">I", d[pos + 8 + ln:pos + 12 + ln])
        assert crc == binascii.crc32(typ + body) & 0xFFFFFFFF, \
            "bad CRC on %r (a chunk patched in place forgot its checksum)" % typ
        out.append((typ, body))
        pos += 12 + ln
    return out


def unfilter(raw, w, h):
    """PNG row filters 0 (None) and 1 (Sub) over 8-bit truecolour."""
    px, stride, at = [], w * 3, 0
    for _ in range(h):
        f = raw[at]; at += 1
        row = bytearray(raw[at:at + stride]); at += stride
        if f == 1:
            for i in range(3, stride):
                row[i] = (row[i] + row[i - 3]) & 0xFF
        else:
            assert f == 0, "unexpected filter byte %d" % f
        px.append([tuple(row[i * 3:i * 3 + 3]) for i in range(w)])
    return px


def play(path):
    """Return (canvas_after_each_frame, [delay_ms...]) the way a player would."""
    chunks = read_chunks(path)
    types = [t for t, _ in chunks]
    assert types[0] == b"IHDR", types[:3]
    assert b"acTL" in types, "no acTL: this is not an animation"
    assert types.index(b"acTL") < types.index(b"IDAT"), \
        "acTL must come before the image data"
    assert types[-1] == b"IEND", types[-3:]

    w = h = None
    frames, nframes, delays = [], None, []
    canvas, pending, seq = None, None, 0
    for typ, body in chunks:
        if typ == b"IHDR":
            w, h, depth, ctype, comp, filt, ilace = struct.unpack(">IIBBBBB", body)
            assert (depth, ctype, comp, filt, ilace) == (8, 2, 0, 0, 0)
            canvas = [[(0, 0, 0)] * w for _ in range(h)]
        elif typ == b"acTL":
            nframes, plays = struct.unpack(">II", body)
            assert plays == 0, "a recording should loop forever, got %d" % plays
        elif typ == b"fcTL":
            s, fw, fh, fx, fy, dn, dd, dispose, blend = struct.unpack(">IIIIIHHBB", body)
            assert s == seq, "fcTL sequence %d out of order (wanted %d)" % (s, seq)
            seq += 1
            assert dispose == 0 and blend == 0, "unexpected dispose/blend %d/%d" % (dispose, blend)
            assert fx + fw <= w and fy + fh <= h, "frame rectangle leaves the canvas"
            pending = (fw, fh, fx, fy)
            delays.append(round(dn * 1000.0 / dd))
        elif typ in (b"IDAT", b"fdAT"):
            data = body
            if typ == b"fdAT":
                (s,) = struct.unpack(">I", body[:4])
                assert s == seq, "fdAT sequence %d out of order (wanted %d)" % (s, seq)
                seq += 1
                data = body[4:]
            fw, fh, fx, fy = pending
            rows = unfilter(zlib.decompress(data), fw, fh)
            for r in range(fh):
                canvas[fy + r][fx:fx + fw] = rows[r]
            frames.append([row[:] for row in canvas])
    assert nframes == len(frames), \
        "acTL says %d frames, the file has %d" % (nframes, len(frames))
    return frames, delays


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
    r = subprocess.run([str(exe), str(OUT)], capture_output=True, text=True)
    assert r.returncode == 0 and "host-ok" in r.stdout, \
        "the host encoder reported a failure:\n%s\n%s" % (r.stdout, r.stderr)

    frames, delays = play(OUT / "anim.png")
    assert len(frames) == 4, "expected 4 frames, got %d" % len(frames)
    print("  ok: four frames, sequence numbers in order, every CRC right")

    # The same recipe test/host/png.c drew, written out independently.
    W, H = 64, 48
    want = [[(((x * 4) & 0xFF), ((y * 5) & 0xFF), 0x40) for x in range(W)]
            for y in range(H)]
    assert frames[0] == want, "frame 0 is not the base image"

    for y in range(8, 20):
        for x in range(10, 30):
            want[y][x] = (0xCC, 0x22, 0x11)
    assert frames[1] == want, "frame 1's rectangle landed in the wrong place"

    for y in range(30, 40):
        for x in range(40, 56):
            want[y][x] = (0x22, 0xCC, 0x33)
    assert frames[2] == want, "frame 2's rectangle landed in the wrong place"

    want[H - 1][W - 1] = (0xFF, 0xFF, 0xFF)
    assert frames[3] == want, "a 1x1 frame in the far corner is not exact"
    print("  ok: every frame reconstructs pixel for pixel, offsets included")

    # delay_den is 250, so durations round to 4 ms.
    assert delays == [120, 40, 252, 8], "durations came back as %r" % delays
    print("  ok: each frame kept the duration it was held for")

    # A recording is mostly unchanged screen. The four frames here total 3072
    # pixels of base plus 440 of rectangles; a whole-frame encoder would write
    # four times the base.
    size = (OUT / "anim.png").stat().st_size
    assert size < W * H * 3, "%d bytes for four 64x48 frames is not compressed" % size
    print("  ok: %d bytes for four frames (raw would be %d)" % (size, W * H * 3 * 4))


# ---------------------------------------------------------------- the guest --
def gui_run(vm, line, expect, timeout=60):
    vm.clear()
    vm.type(line + "\n")
    vm.expect(expect, timeout=timeout)
    return vm.serial()


def guest_tests():
    with BoltVM(name="rec", net=False) as vm:
        vm.sign_in(LOGIN_USER, LOGIN_PASS)
        vm.open_app("Terminal")

        out = gui_run(vm, "rec status", "not recording")
        out = gui_run(vm, "rec start 8", "rec: recording")
        assert "/Videos/screencast-1.png" in out, "unexpected path:\n" + out[-600:]
        print("  ok: recording started")

        # Give it something to record. Each command repaints the terminal, so
        # every sample finds a different screen.
        for i in range(6):
            vm.type("echo recording-frame-%d\n" % i)
            time.sleep(0.5)

        out = gui_run(vm, "rec status", "frames")
        frames = int(out.split("     ")[-1].split(" frames")[0])
        assert frames >= 3, "only %d frames after three seconds of typing" % frames
        print("  ok: it is sampling -- %d frames so far" % frames)

        out = gui_run(vm, "rec stop", "rec: /", timeout=120)
        line = [ln for ln in out.splitlines() if ln.startswith("rec: /")][0]
        path, size = line.split()[1], int(line.split()[2])
        assert 2000 < size < 8 * 1024 * 1024, "implausible recording size %d" % size
        print("  ok: stopped and wrote %s (%d bytes)" % (path, size))

        out = gui_run(vm, "ls -l " + path.rsplit("/", 1)[0], "screencast")
        assert path.rsplit("/", 1)[1] in out, "the file is not on disk:\n" + out[-600:]

        out = gui_run(vm, "rec status", "not recording")
        out = gui_run(vm, "rec stop", "not recording")
        print("  ok: stopping twice is refused rather than crashing")

        # And a recording that is thrown away leaves nothing behind.
        gui_run(vm, "rec start 4 /home/user/Videos/thrown.png", "rec: recording")
        time.sleep(1.0)
        gui_run(vm, "rec cancel", "thrown away")
        out = gui_run(vm, "ls /home/user/Videos", "screencast")
        assert "thrown.png" not in out, "a cancelled recording still wrote a file:\n" + out
        print("  ok: `rec cancel` writes nothing")

        out = gui_run(vm, "notify list -a", "Screen recording")
        print("  ok: finishing one says where it went")

        vm.expect_absent("KERNEL PANIC")


def main():
    host_only = "--host-only" in sys.argv
    host_tests()
    if host_only:
        print("PASS rec-host: the APNG encoder reconstructs frame for frame")
        return 0
    guest_tests()
    print("PASS rec: BoltOS records its own screen to an animated PNG")
    return 0


if __name__ == "__main__":
    sys.exit(main())
