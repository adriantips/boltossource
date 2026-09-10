#!/usr/bin/env python3
"""Reading back what the guest actually played.

QEMU's `wav` audio backend writes everything a guest sends to its sound card
into a file, which is the only way from outside the machine to tell real audio
from a driver that merely accepted every register write. These helpers turn that
file into the two things worth asserting: how much audio there was, and what
frequency it carried.

The frequency measure counts sign changes rather than running an FFT. A sine or
a square wave crosses zero exactly twice per cycle, so the count is the answer,
and unlike a peak-picking measure it cannot be fooled by amplitude -- which
matters, because the whole point is to catch an amplifier left muted or a
converter programmed at the wrong rate.

Note that the backend records only while a stream is *running*. Silence between
two plays is not written, so two sounds played a minute apart land next to each
other in the file; use freq_windows() rather than looking for gaps.
"""
import os
import struct

QUIET = 1000          # |sample| below this is not signal


def read_wav(path):
    """Return (rate, channels, frames, samples) from a 16-bit PCM WAV."""
    assert os.path.exists(path), "QEMU wrote no audio file at " + path
    raw = open(path, "rb").read()
    assert raw[:4] == b"RIFF" and raw[8:12] == b"WAVE", "not a WAV file: %r" % raw[:16]
    rate = chans = bits = None
    data = b""
    i = 12
    while i + 8 <= len(raw):
        cid, clen = raw[i:i + 4], struct.unpack("<I", raw[i + 4:i + 8])[0]
        body = raw[i + 8:i + 8 + clen]
        if cid == b"fmt ":
            chans, rate = struct.unpack("<HI", body[2:8])
            bits = struct.unpack("<H", body[14:16])[0]
        elif cid == b"data":
            # QEMU fixes the length field up on exit, but a run that was killed
            # can leave it stale; trust the bytes actually on disk.
            data = body if clen and i + 8 + clen <= len(raw) else raw[i + 8:]
        i += 8 + clen + (clen & 1)
    assert rate and chans and bits == 16, \
        "unexpected WAV format: %s Hz, %s ch, %s bits" % (rate, chans, bits)
    frames = len(data) // (2 * chans)
    samples = struct.unpack("<%dh" % (frames * chans), data[:frames * chans * 2])
    return rate, chans, frames, samples


def seconds(frames, rate):
    return frames / float(rate)


def dominant_hz(samples, chans, rate):
    """The frequency of the whole capture, ignoring anything quiet."""
    loud = [s for s in samples[::chans] if abs(s) > QUIET]
    if len(loud) < rate // 10:
        return 0.0
    return _hz(loud, rate)


def freq_windows(samples, chans, rate, win_ms=100):
    """Frequency per window across the capture: [(seconds, hz), ...], hz 0 when
    the window is silent. Two different tones one after another show up as two
    plateaus, with one straddling window between them."""
    left = samples[::chans]
    win = max(1, rate * win_ms // 1000)
    out = []
    for i in range(0, max(0, len(left) - win), win):
        w = left[i:i + win]
        out.append((i / float(rate), 0.0 if max(abs(v) for v in w) <= QUIET else _hz(w, rate)))
    return out


def held_tone(windows, hz, tol=0.05, min_windows=2):
    """True if some run of windows sits within `tol` of `hz`. A single window
    can straddle two tones and read as their average, so one is never enough."""
    return sum(1 for _, f in windows if f and abs(f - hz) <= hz * tol) >= min_windows


def _hz(block, rate):
    crossings = sum(1 for a, b in zip(block, block[1:]) if (a >= 0) != (b >= 0))
    return crossings * rate / (2.0 * len(block))
