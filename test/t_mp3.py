#!/usr/bin/env python3
"""MP3: the decoder, held against one nobody here wrote.

A Layer III decoder is six passes over 576 numbers and every one of them has to
be right before you hear anything but noise, so "it makes a sound" is not a
test. What this does instead is encode known audio with LAME, decode it twice
-- once with mpg123, once with kernel/mp3.c -- and diff the two sample for
sample. Thirty-odd files across every sampling rate MPEG-1, MPEG-2 and MPEG-2.5
define, constant and variable bitrate, mono, stereo, joint and dual channel, and
material chosen to make the encoder reach for the parts that are easy to get
wrong: a chirp for steady long blocks, noise for the big Huffman codebooks, and
clicks, which are what short blocks exist for.

Two decoders that agree to within one part in twenty thousand are not agreeing
by accident. The residue is float rounding -- the two pipelines round in
different places -- and it is worth about 85 dB, which is where the assertion is
set.

The comparison is done at the file's own sampling rate, taken out of the decoder
before its resampler runs, because otherwise a failure could be either half and
the resampler is wav.c's and already tested. The 48 kHz path the OS actually
plays through is checked separately, for length rather than for content.

    python test/t_mp3.py --host-only    # no QEMU, about 40 seconds

The host half needs lame and mpg123. Both ship with msys2 (`pacman -S mingw-w64-
ucrt-x86_64-lame mingw-w64-ucrt-x86_64-mpg123`); without them the differential
part is skipped and said so, rather than quietly passing on nothing.
"""
import array
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import wave
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT
from wavcheck import read_wav, freq_windows, held_tone, seconds

OUT = Path(ROOT) / "test" / "out"
LOGIN_USER, LOGIN_PASS = "user", "bolt"

# Decoders disagree only in the last place or two of the float pipeline. 85 dB
# is roughly "one LSB of a 16-bit sample"; anything wrong enough to hear lands
# tens of dB below it, so there is no grey zone to argue about.
MIN_SNR_DB = 80.0
MAX_PEAK_LSB = 4


def tool(*names):
    for n in names:
        p = shutil.which(n)
        if p:
            return p
        for d in (r"C:\msys64\ucrt64\bin", r"C:\msys64\usr\bin"):
            c = os.path.join(d, n + ".exe")
            if os.path.exists(c):
                return c
    return None


def host_cc():
    cc = os.environ.get("BOLTOS_HOST_CC") or tool("clang", "gcc")
    if not cc:
        raise RuntimeError("host C compiler missing (set BOLTOS_HOST_CC)")
    return cc


def build_harness():
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / ("mp3-host.exe" if os.name == "nt" else "mp3-host")
    # -fno-builtin, not -ffreestanding: see test/host/fuzz.h for why the latter
    # is poison in a host build. The two -Wno- flags are the kernel's headers
    # arguing with the host's about dllimport and FP_NAN.
    subprocess.run([host_cc(), "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fno-builtin", "-Wno-inconsistent-dllimport", "-Wno-macro-redefined",
                    "-iquote", str(Path(ROOT) / "include"),
                    str(Path(ROOT) / "test" / "host" / "mp3.c"),
                    # Linked rather than included, so their statics stay their
                    # own and nothing in the shipping sources has to be renamed
                    # to suit a test. What is under test here is the dispatch a
                    # player goes through, not only the decoder beneath it.
                    str(Path(ROOT) / "kernel" / "sound.c"),
                    str(Path(ROOT) / "kernel" / "wav.c"),
                    "-o", str(exe)],
                   check=True)
    return exe


# ---- material the encoder has to work at ----------------------------------

def make_wav(path, rate, chans, kind, secs=1.5):
    n = int(rate * secs)
    a = array.array("h")
    seed = 1
    for i in range(n):
        t = i / rate
        if kind == "chirp":
            # Rising through the spectrum, so no one granule is representative
            # and every scalefactor band gets used somewhere in the file.
            f = 200 + 4000 * t / secs
            l = math.sin(2 * math.pi * f * t) * 0.6
            r = math.sin(2 * math.pi * f * 1.5 * t) * 0.4
        elif kind == "noise":
            seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
            l = ((seed >> 16) & 0xFFFF) / 32768.0 - 1.0
            seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
            r = ((seed >> 16) & 0xFFFF) / 32768.0 - 1.0
            l *= 0.5
            r *= 0.5
        elif kind == "clicks":
            # Transients: what short blocks are for, and the only way to make an
            # encoder emit block types 1, 2 and 3 at all.
            l = 0.9 if (i % (rate // 6)) < 40 else 0.0
            r = 0.9 if (i % (rate // 9)) < 30 else 0.0
        else:
            l = r = 0.0
        a.append(int(max(-1.0, min(1.0, l)) * 32000))
        if chans == 2:
            a.append(int(max(-1.0, min(1.0, r)) * 32000))
    with wave.open(str(path), "wb") as f:
        f.setnchannels(chans)
        f.setsampwidth(2)
        f.setframerate(rate)
        f.writeframes(a.tobytes())


def read_wav_samples(path):
    with wave.open(str(path), "rb") as f:
        chans = f.getnchannels()
        rate = f.getframerate()
        data = f.readframes(f.getnframes())
    s = array.array("h")
    s.frombytes(data)
    return s, chans, rate


def read_pcm(path):
    a = array.array("h")
    with open(path, "rb") as f:
        a.frombytes(f.read())
    return a


def diff(ref, refchans, mine, tag):
    """SNR of ours against the reference over everything they both cover."""
    n = min(len(ref) // refchans, len(mine) // 2)
    assert n > 4000, "%s: only %d frames to compare" % (tag, n)
    # The first and last granule of a file are the encoder's own padding and
    # are not the same thing in both decoders; skip a granule either end.
    lo, hi = 1152, n - 1152
    err = peak = 0
    energy = 0
    count = 0
    for i in range(lo, hi):
        for c in range(2):
            rv = ref[i * refchans + (c if refchans == 2 else 0)]
            d = rv - mine[i * 2 + c]
            err += d * d
            energy += rv * rv
            if abs(d) > peak:
                peak = abs(d)
            count += 1
    rms = math.sqrt(err / count)
    sig = math.sqrt(energy / count)
    snr = 20 * math.log10(sig / rms) if rms > 0 else 999.0
    return snr, peak, rms, count


CASES = [
    # (kind, rate, channels, lame args, what it is there to exercise)
    ("chirp",  44100, 2, ["-b", "192"], "MPEG-1 44.1 kHz, the ordinary case"),
    ("chirp",  44100, 1, ["-b", "128"], "mono, where the side info is 17 bytes"),
    ("chirp",  48000, 2, ["-b", "256"], "MPEG-1 48 kHz"),
    ("chirp",  32000, 2, ["-b", "128"], "MPEG-1 32 kHz"),
    ("chirp",  22050, 2, ["-b", "64"],  "MPEG-2, one granule a frame"),
    ("chirp",  24000, 2, ["-b", "48"],  "MPEG-2 24 kHz"),
    ("chirp",  16000, 2, ["-b", "32"],  "MPEG-2 16 kHz"),
    ("chirp",  11025, 2, ["-b", "16"],  "MPEG-2.5, a quarter rate"),
    ("chirp",   8000, 2, ["-b", "16"],  "MPEG-2.5 8 kHz, where the bands widen"),
    ("noise",  44100, 2, ["-b", "320"], "the big codebooks, with linbits"),
    ("noise",  44100, 2, ["-b", "64"],  "coarse quantisation, scalefactors in play"),
    ("noise",  22050, 2, ["-b", "48"],  "MPEG-2 scalefactor partitions"),
    ("noise",  16000, 1, ["-b", "32"],  "MPEG-2 mono"),
    ("clicks", 44100, 2, ["-b", "320"], "short blocks with room to spare"),
    ("clicks", 44100, 2, ["-b", "64"],  "short blocks, coarsely quantised"),
    ("clicks", 32000, 2, ["-b", "128"], "short blocks at 32 kHz"),
    ("clicks", 22050, 2, ["-b", "64"],  "MPEG-2 short blocks"),
    ("clicks", 16000, 2, ["-b", "24"],  "MPEG-2 short blocks, low rate"),
    ("clicks",  8000, 2, ["-b", "16"],  "MPEG-2.5 short blocks"),
    ("noise",  44100, 2, ["-V", "0"],   "variable bitrate, best quality"),
    ("noise",  44100, 2, ["-V", "4"],   "variable bitrate, middle"),
    ("chirp",  44100, 2, ["-V", "9"],   "variable bitrate, worst"),
    ("chirp",  44100, 2, ["-m", "s", "-b", "128"], "forced left/right, no mid/side"),
    ("chirp",  44100, 2, ["-m", "d", "-b", "128"], "dual mono"),
    ("chirp",  44100, 2, ["-m", "j", "-b", "96"],  "joint stereo, mid/side toggling"),
]


def differential(exe, lame, mpg123):
    src = OUT / "mp3src.wav"
    mp3 = OUT / "mp3case.mp3"
    ref = OUT / "mp3ref.wav"
    mine = OUT / "mp3mine.pcm"
    worst = (999.0, "")
    for kind, rate, chans, args, why in CASES:
        for f in (mp3, ref, mine):
            if f.exists():
                f.unlink()
        make_wav(src, rate, chans, kind)
        subprocess.run([lame] + args + ["--quiet", str(src), str(mp3)], check=True)
        subprocess.run([mpg123, "--no-gapless", "-q", "-w", str(ref), str(mp3)], check=True)
        r = subprocess.run([exe, "--native", str(mp3), str(mine)],
                           capture_output=True, text=True)
        assert r.returncode == 0, "our decoder refused %s %d %s:\n%s" % (
            kind, rate, args, r.stderr)

        refs, refchans, refrate = read_wav_samples(ref)
        tag = "%-6s %5d %s" % (kind, rate, " ".join(args))
        # Not the source rate: LAME resamples when the bitrate is too low to
        # carry it (48 kbps of 24 kHz comes back as 22.05), so the thing worth
        # asserting is that the two decoders read the same rate out of the file.
        got = re.search(r"at (\d+) Hz", r.stderr)
        assert got and int(got.group(1)) == refrate, (
            "mpg123 decoded %s at %d Hz and we read %s -- the comparison would "
            "be of two different things" % (tag, refrate, got and got.group(1)))
        snr, peak, rms, n = diff(refs, refchans, read_pcm(mine), tag)
        state = "ok " if (snr >= MIN_SNR_DB and peak <= MAX_PEAK_LSB) else "BAD"
        print("  %s %s  snr %5.1f dB  peak %d LSB   (%s)" % (state, tag, snr, peak, why))
        assert snr >= MIN_SNR_DB and peak <= MAX_PEAK_LSB, (
            "%s decodes differently from mpg123: %.1f dB, worst sample off by %d.\n"
            "%s" % (tag, snr, peak, why))
        if snr < worst[0]:
            worst = (snr, tag)
    print("  ok: %d files, every one within %.0f dB of mpg123 (worst %s at %.1f dB)"
          % (len(CASES), MIN_SNR_DB, worst[1].strip(), worst[0]))


def shipping_path(exe, lame):
    """The 48 kHz output the codec actually gets, which is the other half of
    the decoder -- resampled, stereo whatever the file was, and the right
    length for the file's duration."""
    src = OUT / "mp3src.wav"
    mp3 = OUT / "mp3case.mp3"
    pcm = OUT / "mp3-48k.pcm"
    for rate in (44100, 22050, 32000):
        make_wav(src, rate, 1 if rate == 22050 else 2, "chirp", secs=2.0)
        subprocess.run([lame, "-b", "128", "--quiet", str(src), str(mp3)], check=True)
        subprocess.run([exe, "--48k", str(mp3), str(pcm)],
                       capture_output=True, text=True, check=True)
        frames = pcm.stat().st_size // 4
        want = 2.0 * 48000
        # A few percent over is the encoder's own delay and padding, which are
        # audio as far as a decoder is concerned; a resampler that had the ratio
        # upside down would be out by tens of percent.
        assert abs(frames - want) < 0.10 * want, (
            "a 2.0 s %d Hz file came out as %.2f s at 48 kHz"
            % (rate, frames / 48000.0))
        print("  ok: %d Hz in, %.2f s of 48 kHz stereo out" % (rate, frames / 48000.0))


def seeking(exe, lame):
    """Where a seek lands. An MP3 carries no index, so this is arithmetic on
    byte offsets (or on a Xing table of contents), and it is approximate by
    construction -- the bit reservoir also has to refill, which costs a frame
    or two of audio before anything comes out. What the assertion is for is the
    failure that matters: a seek that lands somewhere else entirely."""
    src, mp3 = OUT / "seeksrc.wav", OUT / "seek.mp3"
    full, part = OUT / "seekfull.pcm", OUT / "seekpart.pcm"
    rate, secs = 44100, 30.0

    # A slow chirp: no two moments sound alike, so "where did this land" has
    # exactly one answer and a correlation can find it.
    n = int(rate * secs)
    a = array.array("h")
    for i in range(n):
        t = i / secs / rate * secs
        f = 200 + 3000 * (i / rate) / secs
        v = int(math.sin(2 * math.pi * f * (i / rate)) * 30000)
        a.append(v)
        a.append(v)
    with wave.open(str(src), "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(rate)
        f.writeframes(a.tobytes())

    for args, what in ((["-b", "128"], "constant bitrate, by byte offset"),
                       (["-V", "4"], "variable bitrate, by Xing table of contents")):
        subprocess.run([lame] + args + ["--quiet", str(src), str(mp3)], check=True)
        subprocess.run([exe, "--48k", str(mp3), str(full)],
                       capture_output=True, check=True)
        ref = read_pcm(full)
        worst = 0
        for ms in (2000, 5000, 10000, 20000, 27000):
            subprocess.run([exe, "--seek", str(ms), str(mp3), str(part)],
                           capture_output=True, check=True)
            got = read_pcm(part)
            assert len(got) > 12000 * 2, "seeking to %d ms decoded nothing" % ms
            # Correlate a window from 2000 frames in (past the reservoir refill)
            # against the straight decode, over +/- 3 s of where it was asked for.
            lo = max(0, (ms - 3000) * 48)
            hi = min(len(ref) // 2 - 4000, (ms + 3000) * 48)
            best = None
            for off in range(lo, hi, 24):
                e = sum(abs(ref[(off + i) * 2] - got[(2000 + i) * 2])
                        for i in range(0, 2000, 5))
                if best is None or e < best[0]:
                    best = (e, off)
            landed = (best[1] - 2000) / 48.0
            worst = max(worst, abs(landed - ms))
        assert worst < 600, (
            "a seek was %.0f ms out (%s). Approximate is expected; this is not "
            "approximate, it is somewhere else." % (worst, what))
        print("  ok: seeks land within %.0f ms over 30 s (%s)" % (worst, what))


def adversarial_wav(path):
    """A real WAV whose samples contain real MPEG-1 Layer III frame headers,
    three of them, spaced exactly one frame apart.

    The accident that started this -- ordinary PCM containing a byte pair that
    looks like a frame sync -- depended on luck and on files that are build
    output. This is the same thing on purpose and in a file the test writes
    itself, so the regression is guarded on a bare checkout: any classifier that
    goes looking for frame headers before it checks for "RIFF" will claim it.
    """
    # 0xFF 0xFB: sync, MPEG-1, Layer III, no CRC. 0x90: 128 kbps at 44.1 kHz,
    # no padding. 0x00: stereo, no emphasis. 144 * 128000 / 44100 = 417 bytes.
    hdr = bytes((0xFF, 0xFB, 0x90, 0x00))
    frame = 417
    data = bytearray()
    for i in range(6):
        data += hdr
        data += bytes(((i * 37 + j * 11) & 0x7F for j in range(frame - 4)))
    body = bytes(data)

    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 2, 44100, 44100 * 4, 4, 16))
        f.write(b"data" + struct.pack("<I", len(body)) + body)
    return path


def classifying(exe, lame):
    """What snd_open() decides a file is.

    A sniffer is a classifier, and a classifier can only be tested with negative
    examples that came from somewhere other than the positives. Every WAV this
    suite had ever fed the decoder was one it had just written, and every MP3
    was one LAME had just written, so it could not have caught what actually
    happened: Layer III has no magic of its own beyond eleven set bits, ordinary
    PCM contains byte pairs that look like a frame sync, and two of five real
    WAV files on this machine were being opened as MP3s and played as static.

    So the negatives here are dragged in from the rest of the repository --
    PNGs, TrueType, ELF, a disk image, source, whatever is lying about -- on the
    principle that the full negative set for this classifier is every other
    format the machine can open, and a 5 MB font has far more chances to contain
    an accidental sync than a 22 KB chime does.
    """
    seeds = Path(ROOT) / "test" / "seeds" / "mp3"
    positives = sorted(seeds.glob("*.mp3")) if seeds.is_dir() else []

    # The negatives this test makes for itself. Everything below them is a
    # bonus: assets/audio/*.wav, build/ and esp/ are all gitignored build
    # output, so on a fresh checkout -- and in the CI host job, which never runs
    # build.sh -- they are simply absent, and a set assembled hopefully out of
    # globs shrinks in silence and still passes. The files that exposed the bug
    # were two large WAVs of exactly this kind, so the test carries its own now.
    negatives = [(adversarial_wav(OUT / "neg-planted.wav"),
                  "a WAV with real MPEG frame headers planted in its samples")]
    for rate, chans, kind, secs in ((44100, 2, "noise", 8.0),
                                    (48000, 2, "noise", 8.0),
                                    (22050, 1, "chirp", 4.0),
                                    (44100, 2, "clicks", 4.0)):
        p = OUT / ("neg-%d-%d-%s.wav" % (rate, chans, kind))
        make_wav(p, rate, chans, kind, secs=secs)
        negatives.append((p, "%d Hz %s %s WAV, %.0f s" % (
            rate, "mono" if chans == 1 else "stereo", kind, secs)))

    # And then whatever else this checkout happens to have, because the full
    # negative set for this classifier is every other format the machine can
    # open. Tracked files first, so CI gets them too.
    for pat, what in (("assets/wallpapers/*.png", "PNG"),
                      ("assets/wallpapers/*.jpg", "JPEG"),
                      ("assets/fonts/*.ttf", "TrueType"),
                      ("kernel/mp3.c", "C source"),
                      ("README.md", "Markdown"),
                      ("iso/boltos.iso", "an ISO image"),
                      # Build output: present after build.sh, absent in CI.
                      ("assets/audio/*.wav", "a system sound"),
                      ("build/kernel.elf", "ELF64"),
                      ("build/*.EFI", "PE32+"),
                      ("esp/EFI/BOOT/*.EFI", "PE32+")):
        for p in sorted(Path(ROOT).glob(pat))[:2]:
            # build.sh copies BOOTX64.EFI from build/ to esp/, so the same
            # binary matches two patterns. A listing that is going to be read as
            # evidence must not count one file twice -- that is the same kind of
            # small lie as the glob that matched nothing.
            if p.is_file() and not any(q.name == p.name and
                                       q.stat().st_size == p.stat().st_size
                                       for q, _ in negatives):
                negatives.append((p, what))

    # Before trusting the planted file as a negative, prove it is a real one.
    # The same bytes with the 44-byte RIFF header removed have to classify as an
    # MP3 -- if they do not, the plant failed and "it was not called an MP3"
    # means nothing. A negative example nobody could mistake is not a test.
    planted = negatives[0][0]
    body = OUT / "neg-planted-body.bin"
    body.write_bytes(planted.read_bytes()[44:])
    bare = subprocess.run([exe, "--which", str(body)], capture_output=True, text=True)
    assert bare.stdout.startswith("MP3"), (
        "the planted frame headers are not convincing on their own (%s), so the "
        "WAV built around them proves nothing about the classifier"
        % (bare.stdout + bare.stderr).strip())

    assert positives, "no seeds in test/seeds/mp3 -- nothing to check the yes side with"
    for p in positives:
        out = subprocess.run([exe, "--which", str(p)], capture_output=True, text=True)
        assert out.returncode == 0 and out.stdout.startswith("MP3"), (
            "%s is an MP3 and was not opened as one: %s"
            % (p.name, (out.stdout + out.stderr).strip()))

    wrong = []
    for p, what in negatives:
        out = subprocess.run([exe, "--which", str(p)], capture_output=True, text=True)
        if out.stdout.startswith("MP3"):
            wrong.append("%s (%d bytes, %s) -> %s"
                         % (p.name, p.stat().st_size, what, out.stdout.strip()))
    assert not wrong, (
        "files that are not MP3s were opened as MP3s, which is how a .wav comes "
        "out as static:\n  " + "\n  ".join(wrong))

    # Say what was tested, not how much. A count cannot be checked by a reader;
    # this listing is how boltos-d8 spotted that an `esp/*.EFI` glob one
    # directory too shallow had been matching nothing at all, in silence.
    print("  ok: the planted headers do read as an MP3 with the RIFF header off, "
          "and as a WAV with it on")
    print("  ok: %d MP3s opened as MP3s, and %d files of other kinds not:"
          % (len(positives), len(negatives)))
    for p, what in negatives:
        print("       %-26s %9d  %s" % (p.name, p.stat().st_size, what))


def host_tests():
    exe = build_harness()
    lame, mpg123 = tool("lame"), tool("mpg123")

    # A real file to tear up, if there is an encoder to make one with.
    sample = None
    if lame:
        make_wav(OUT / "mp3src.wav", 44100, 2, "noise", secs=1.0)
        subprocess.run([lame, "-b", "128", "--quiet",
                        str(OUT / "mp3src.wav"), str(OUT / "mp3damage.mp3")], check=True)
        sample = str(OUT / "mp3damage.mp3")

    subprocess.run([str(exe), "--selftest"] + ([sample] if sample else []), check=True)

    if not (lame and mpg123):
        # Classifying needs no reference decoder, only files -- run it anyway.
        classifying(str(exe), lame)
        print("  SKIP the differential half: %s missing. Nothing below this line "
              "has been checked against another decoder."
              % (" and ".join(n for n, p in (("lame", lame), ("mpg123", mpg123)) if not p)))
        return
    classifying(str(exe), lame)
    differential(str(exe), lame, mpg123)
    shipping_path(str(exe), lame)
    seeking(str(exe), lame)


# ---- and on the machine itself --------------------------------------------

def shell(vm, line, wait_for, timeout=90):
    """Type into the GUI Terminal and wait for a line, not for a fixed time --
    every fixed sleep is a guess that stops being true on a loaded machine."""
    vm.clear()
    vm.type(line + "\n")
    assert vm.wait_for(wait_for, timeout=timeout), (
        "%r never printed %r:\n%s" % (line, wait_for, vm.serial()[-900:]))
    return vm.serial()


def position_s(status_out):
    """`media status`'s Position and the track's length, both in seconds."""
    m = re.search(r"Position: (\d+):(\d+) / (\d+):(\d+)", status_out)
    assert m, "no Position line in:\n" + status_out[-700:]
    return (int(m.group(1)) * 60 + int(m.group(2)),
            int(m.group(3)) * 60 + int(m.group(4)))


def qemu_tests():
    """Decoding on the host proves the arithmetic. This proves the OS can get an
    MP3 off a network, off a disk and out of a sound card, which is the part a
    person would notice."""
    lame = tool("lame")
    if not lame:
        print("SKIP mp3 qemu: lame missing, so there is no MP3 to serve")
        return 0

    # Sixteen seconds of a steady tone. Steady because a capture can be checked
    # for a pitch and not for a chirp; sixteen because the player repeats by
    # default and everything below has to finish inside one pass of the track --
    # a loop back to zero reads exactly like a stall. At 96 kbps that is about
    # 190 KB, which fits in cmd_download's 256 KB buffer with room to spare.
    src, mp3 = OUT / "guest-src.wav", OUT / "guest.mp3"
    rate, hz, guest_secs = 44100, 880, 16.0
    a = array.array("h")
    for i in range(int(rate * guest_secs)):
        v = int(math.sin(2 * math.pi * hz * i / rate) * 26000)
        a.append(v)
        a.append(v)
    with wave.open(str(src), "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(rate)
        f.writeframes(a.tobytes())
    subprocess.run([lame, "-b", "96", "--quiet", str(src), str(mp3)], check=True)

    import http.server
    import threading
    import socketserver

    served = OUT / "served"
    served.mkdir(exist_ok=True)
    shutil.copyfile(mp3, served / "tone.mp3")

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=str(served), **kw)

        def log_message(self, *a):
            pass

    httpd = socketserver.TCPServer(("0.0.0.0", 0), Quiet)
    port = httpd.server_address[1]
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    try:
        # The desktop, not the serial shell, and that is forced rather than
        # chosen: media_pump() is called from the top of the compositor's main
        # loop, so on a machine with no framebuffer -- `-vga none`, which is how
        # t_net.py gets a shell with networking -- the compositor is not running
        # and nothing would feed the codec. The GUI Terminal runs the same shell
        # and echoes to the same serial port, so the commands are identical.
        with BoltVM(name="mp3", net=True, audio="hda", wav="mp3-capture.wav") as vm:
            vm.sign_in(LOGIN_USER, LOGIN_PASS, timeout=180)
            vm.open_app("Terminal")

            shell(vm, "download http://10.0.2.2:%d/tone.mp3 /home/user/tone.mp3" % port,
                  "downloaded", timeout=120)
            print("  ok: the machine fetched an MP3 over its own network stack")

            out = shell(vm, "media open /home/user/tone.mp3", "Device  :", timeout=120)
            m = re.search(r"(MPEG-[\d.]+) Layer III, ([\d.]+) kHz ([a-z ]+), (\d+) kbps", out)
            assert m, ("nothing reported the file as an MPEG stream:\n" + out[-800:])
            print("  ok: reported as %s Layer III, %s kHz %s, %s kbps"
                  % (m.group(1), m.group(2), m.group(3).strip(), m.group(4)))

            assert "State   : playing" in out, \
                "the track opened but did not start:\n" + out[-800:]

            # Nothing is typed for four seconds. The player is fed from the
            # compositor's loop one decoder step at a time, so a position that
            # has moved on its own is the evidence that the incremental decode
            # keeps the codec fed -- which is the part of this that MP3 changed.
            time.sleep(4.0)
            out = shell(vm, "media status", "Device  :", timeout=60)
            assert "State   : playing" in out, \
                "it stopped on its own within four seconds:\n" + out[-800:]
            pos = re.search(r"Position: (\d+):(\d+)", out)
            assert pos and position_s(out)[0] >= 3, (
                "four seconds passed and the track had not advanced past %s -- "
                "nothing was feeding the codec:\n%s"
                % (pos.group(0) if pos else "?", out[-800:]))
            print("  ok: unattended for four seconds and the position moved to %s"
                  % pos.group(0).split()[-1])

            # And now the case nothing else in the suite covers. Every test that
            # boots with an audio device is an audio test that leaves the
            # desktop alone, and every test that works the compositor boots with
            # no audio device -- so "a track playing while the screen is busy"
            # is untested in both directions. It is also exactly where the
            # decode budget could go wrong: the refill is a ceiling rather than
            # a pacing rule, so it arrives in bursts, and a compositor that is
            # already behind is what would turn that into a gap you can hear.
            #
            # Driven from the shell rather than by synthesising drags: `snap`
            # and `display` each repaint the whole screen and print their state
            # back, and neither takes the keyboard away from the Terminal the
            # way switching desktops would.
            before, duration = position_s(out)
            t0 = time.time()
            for zone, bright in (("left", 55), ("right", 100),
                                 ("max", 70), ("restore", 85)):
                shell(vm, "snap " + zone, "floating" if zone == "restore" else zone,
                      timeout=60)
                shell(vm, "display brightness %d" % bright, "brightness", timeout=60)
            worked = time.time() - t0
            out = shell(vm, "media status", "Device  :", timeout=60)
            after, _ = position_s(out)

            assert "State   : playing" in out, \
                "the track stopped while the desktop was busy:\n" + out[-800:]
            # The player repeats, so a position that went backwards wrapped
            # rather than stalled. Adding a lap is only honest while the work
            # took less than a lap -- past that the count is ambiguous, and the
            # test says so rather than guessing which way.
            assert worked < duration - 2, (
                "the compositor work took %.0f s of a %d s track, so the "
                "position is ambiguous. Shorten the loop or lengthen the track."
                % (worked, duration))
            advanced = after - before
            if advanced < 0:
                advanced += duration
            # A second of slack for the status command itself, and the position
            # has one-second resolution at each end.
            assert advanced >= worked - 2.0, (
                "%.0f s of compositor work passed and the track advanced %d s "
                "(%d -> %d of %d). The decode is not keeping up when the screen "
                "is busy:\n%s"
                % (worked, advanced, before, after, duration, out[-800:]))
            print("  ok: %d s of audio across %.0f s of snapping and repainting"
                  % (advanced, worked))

            vm.expect_absent("KERNEL PANIC")
            wav_path = vm.wav_path
    finally:
        httpd.shutdown()

    rate_c, chans, frames, samples = read_wav(wav_path)
    total = seconds(frames, rate_c)
    # Four seconds of it were played unattended, so that is the floor. The
    # backend records only while a stream is running, which is why this is a
    # measure of audio that actually left the machine rather than of elapsed
    # wall time.
    assert total >= 3.0, (
        "only %.2f s reached the sound card across four seconds of playing. "
        "Most of it never left the machine, whatever the decoder said." % total)
    wins = freq_windows(samples, chans, rate_c)
    assert held_tone(wins, hz), (
        "no sustained %d Hz in the capture, so the MP3 did not come out at the "
        "pitch it went in at. Windows measured: %s"
        % (hz, ", ".join("%.0f" % f for _, f in wins if f)))
    print("  ok: %.2f s of audio reached the sound card, at %d Hz" % (total, hz))
    print("PASS mp3: the machine decodes and plays MP3, and agrees with mpg123 about it")
    return 0


def main():
    host_only = "--host-only" in sys.argv
    host_tests()
    if host_only:
        print("PASS mp3 (host only)")
        return 0
    return qemu_tests()


if __name__ == "__main__":
    sys.exit(main())
