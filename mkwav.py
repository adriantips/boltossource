#!/usr/bin/env python3
"""Generate the system sounds BoltOS ships in /usr/share/sounds.

They are plain tones with a short fade at each end -- honest about what they
are, and deliberately in two different shapes. A sound file on a real disk is
almost never in the format the codec wants, so between them these two exercise
every conversion kernel/wav.c has to do:

    chime.wav   22050 Hz, mono, 8-bit unsigned   -> resample up, mono to
                                                    stereo, unsigned to signed
    blip.wav    44100 Hz, stereo, 16-bit signed  -> resample up only

The fade matters: a tone that starts and stops at full amplitude puts a step
into the waveform, and a step is a click. Twenty milliseconds is enough to
remove it and short enough that the tone still reads as instant.

    mkwav.py OUTDIR
"""
import math
import os
import struct
import sys

FADE_MS = 20


def tone(path, rate, channels, bits, hz, ms, level=0.7):
    frames = rate * ms // 1000
    fade = max(1, rate * FADE_MS // 1000)
    peak = (1 << (bits - 1)) - 1

    body = bytearray()
    for i in range(frames):
        env = min(1.0, i / fade, (frames - 1 - i) / float(fade))
        v = math.sin(2.0 * math.pi * hz * i / rate) * level * env
        s = int(round(v * peak))
        s = max(-peak, min(peak, s))
        for _ in range(channels):
            if bits == 8:
                body.append(s + 128)          # 8-bit WAV samples are unsigned
            else:
                body += struct.pack("<h", s)

    block = channels * bits // 8
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * block, block, bits)
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(body)) + bytes(body)
    riff = b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks

    with open(path, "wb") as f:
        f.write(riff)
    print("      %s: %d Hz %s %d-bit, %d ms, %d bytes"
          % (os.path.basename(path), rate,
             "mono" if channels == 1 else "stereo", bits, ms, len(riff)))


def main(argv):
    outdir = argv[1] if len(argv) > 1 else "assets"
    os.makedirs(outdir, exist_ok=True)
    tone(os.path.join(outdir, "chime.wav"), 22050, 1, 8, 880, 1000)
    tone(os.path.join(outdir, "blip.wav"), 44100, 2, 16, 1320, 500)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
