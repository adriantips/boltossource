#!/usr/bin/env python3
"""Playing an audio file, which the machine could not do at all.

BoltOS had a working codec and no way to reach it with anything but a synthesised
square wave: the Music app held note tables and generated tones on the fly, and
nothing in the system could read a sound off a disk. "Listen to audio" was true
only if you counted beeps.

kernel/wav.c is the decoder, and it is also a converter, because the two things
cannot be separated here. Every codec BoltOS drives is fixed at 48 kHz stereo,
and essentially no WAV file is: the two the system ships are 22050 Hz mono 8-bit
and 44100 Hz stereo 16-bit, which between them need resampling, mono-to-stereo
expansion and the unsigned-to-signed shift that 8-bit WAV samples require. Get
the last one wrong and silence plays as full-scale noise.

So the assertions are made on the sound, not on the parse. QEMU's audio backend
writes what the guest sends to its sound card into a file, and the check is that
both tones are in there at the pitch they were written with -- 880 Hz for the
22 kHz mono file, 1320 Hz for the 44.1 kHz stereo one. A resampler that is
subtly wrong shifts the pitch and fails this; one that is badly wrong fails it
loudly. Neither is visible from inside the machine, which is why it is measured
from outside.

The header line `play` prints is asserted too, but only as the cheap half: it
proves the file was understood, not that anything came out of it.
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM
from wavcheck import read_wav, freq_windows, held_tone, seconds

LOGIN_USER, LOGIN_PASS = "user", "bolt"
SOUNDS = "/usr/share/sounds"

# What mkwav.py generates, and therefore what has to come back out.
CHIME = dict(path=SOUNDS + "/chime.wav", rate=22050, ch=1, bits=8, secs="1.00", hz=880)
BLIP = dict(path=SOUNDS + "/blip.wav", rate=44100, ch=2, bits=16, secs="0.50", hz=1320)


def sign_in(vm):
    vm.expect("BoltOS is up. Launching desktop.", timeout=180)
    time.sleep(4.0)
    vm.type(LOGIN_USER); vm.key("tab"); vm.type(LOGIN_PASS); vm.key("ret")
    time.sleep(3.0)
    vm.key("meta_l"); vm.type("Terminal"); vm.key("ret")
    time.sleep(2.0)


def run(vm, line, wait_for, timeout=90):
    vm.clear()
    vm.type(line + "\n")
    assert vm.wait_for(wait_for, timeout=timeout), \
        "%r never printed %r:\n%s" % (line, wait_for, vm.serial()[-900:])
    return vm.serial()


def play_file(vm, spec):
    """Play one sound and check the header `play` reports against the file."""
    out = run(vm, "play " + spec["path"], "ms elapsed")
    m = re.search(r"(\d+) Hz, (\d+) ch, (\d+)-bit, (\d+\.\d+) s -> 48000 Hz stereo", out)
    assert m, "play did not describe the file it opened:\n" + out[-700:]
    rate, ch, bits, secs = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)
    assert (rate, ch, bits, secs) == (spec["rate"], spec["ch"], spec["bits"], spec["secs"]), \
        ("the header was read as %d Hz / %d ch / %d-bit / %s s, but the file is "
         "%d / %d / %d / %s" % (rate, ch, bits, secs, spec["rate"], spec["ch"],
                                spec["bits"], spec["secs"]))
    print("  ok: %s read as %d Hz, %d ch, %d-bit, %s s"
          % (os.path.basename(spec["path"]), rate, ch, bits, secs))


def main():
    with BoltVM(name="wav", net=False, audio="hda", wav="wav-capture.wav") as vm:
        sign_in(vm)

        out = run(vm, "ls " + SOUNDS, "chime.wav")
        assert "blip.wav" in out, "the system sounds were not seeded:\n" + out[-500:]
        print("  ok: both system sounds are on the filesystem")

        play_file(vm, CHIME)
        play_file(vm, BLIP)

        # A path that is not there, and a file that is not a WAV. Both have to
        # say which of the two happened -- a decoder that reports "cannot open"
        # for a corrupt file sends you looking in the wrong place entirely.
        out = run(vm, "play " + SOUNDS + "/nope.wav", "play:")
        assert "cannot open" in out, "a missing file was not reported:\n" + out[-400:]
        out = run(vm, "play /bin/hello", "play:")
        assert "not a WAV file" in out, \
            "an ELF was not rejected as a non-WAV:\n" + out[-400:]
        print("  ok: a missing file and a non-WAV are refused, and differently")

        vm.expect_absent("KERNEL PANIC")
        wav_path = vm.wav_path

    # QEMU has exited, so the capture is complete and its header fixed up.
    rate, chans, frames, samples = read_wav(wav_path)
    total = seconds(frames, rate)
    print("  ok: %.2f s of audio reached the sound card" % total)
    assert total >= 1.2, (
        "only %.2f s arrived for 1.5 s of sound. Most of it never left the "
        "machine, whatever the decoder said it had read." % total)

    # The backend records only while a stream runs, so the two sounds sit next
    # to each other in the file with no gap to split on -- hence windows.
    wins = freq_windows(samples, chans, rate)
    for spec, name in ((CHIME, "chime"), (BLIP, "blip")):
        assert held_tone(wins, spec["hz"]), (
            "no sustained %d Hz anywhere in the capture, so %s.wav did not come "
            "out at the pitch it was written with. Windows measured: %s"
            % (spec["hz"], name,
               ", ".join("%.0f" % f for _, f in wins if f)))
        print("  ok: %s.wav plays at %d Hz after conversion from %d Hz %s %d-bit"
              % (name, spec["hz"], spec["rate"],
                 "mono" if spec["ch"] == 1 else "stereo", spec["bits"]))

    print("PASS wav: the machine plays audio files, converted to what the codec wants")
    return 0


if __name__ == "__main__":
    sys.exit(main())
