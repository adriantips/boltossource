#!/usr/bin/env python3
"""Intel HD Audio, alongside AC'97 rather than instead of it.

The old driver detected an HDA controller, printed its name, and then played
every sound through the PC speaker -- so on any machine built in the last twenty
years BoltOS had no real audio at all, while `hw` cheerfully reported "Intel
HDA". This asserts the three things that claim now has to mean.

*Sound comes out, and it is the right sound.* The hard part of HDA is not the
DMA engine, it is that a codec arrives with nothing connected: the driver has to
walk the widget graph, find a route from an output pin back to a converter,
select it at every fork and unmute every amplifier on the way. A test that only
read registers back would pass with all of that wrong. So QEMU's audio backend
is pointed at a file, and the assertions are made on the samples that actually
left the guest -- how many there were, and what frequency they carry. A wrong
sample rate, a wrong stream format or a muted amplifier each fail that, and none
of them are visible from inside the machine.

*HDA is preferred, and AC'97 still works.* Given both devices the machine picks
HDA and never opens the AC'97; given only an AC'97 it still plays, which is the
regression that matters, since both drivers were rewritten to sit behind one
interface.

*A controller it cannot drive is declined.* An HDA controller with no codec on
its link has nothing to play through. Claiming it anyway is how the old code
produced silence that looked like success.
"""
import os, re, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

LOGIN_USER, LOGIN_PASS = "user", "bolt"
TONE_HZ = 440


def sign_in(vm):
    vm.expect("BoltOS is up. Launching desktop.", timeout=180)
    time.sleep(4.0)
    vm.type(LOGIN_USER); vm.key("tab"); vm.type(LOGIN_PASS); vm.key("ret")
    time.sleep(3.0)


def open_terminal(vm):
    vm.key("meta_l")
    vm.type("Terminal")
    vm.key("ret")
    time.sleep(2.0)


def play(vm, ms, device):
    """Play a tone; return the elapsed milliseconds the kernel reported."""
    vm.clear()
    vm.type("play %d %d\n" % (TONE_HZ, ms))
    assert vm.wait_for("ms elapsed", timeout=90), \
        "play %d never finished:\n%s" % (ms, vm.serial()[-800:])
    m = re.search(r"play: (\d+) ms elapsed on (.+)", vm.serial())
    assert m, "play did not report its duration:\n" + vm.serial()[-600:]
    got, name = int(m.group(1)), m.group(2).strip()
    assert name == device, "expected %r to play it, got %r" % (device, name)
    return got


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
            # QEMU fixes the length field up on exit, but a truncated run can
            # leave it stale; trust the bytes actually on disk.
            data = raw[i + 8:i + 8 + clen] if clen and i + 8 + clen <= len(raw) \
                else raw[i + 8:]
        i += 8 + clen + (clen & 1)
    assert rate and chans and bits == 16, \
        "unexpected WAV format: %s Hz, %s ch, %s bits" % (rate, chans, bits)
    frames = len(data) // (2 * chans)
    samples = struct.unpack("<%dh" % (frames * chans), data[:frames * chans * 2])
    return rate, chans, frames, samples


def dominant_hz(samples, chans, rate):
    """Frequency of a square wave, by counting sign changes on one channel.

    A square wave crosses zero exactly twice per cycle, so this needs no FFT --
    and unlike a peak-picking measure it cannot be fooled by amplitude.
    """
    left = samples[::chans]
    # Ignore the silence pad the driver appends after every buffer.
    loud = [s for s in left if abs(s) > 1000]
    if len(loud) < rate // 10:
        return 0.0
    crossings = sum(1 for a, b in zip(loud, loud[1:]) if (a >= 0) != (b >= 0))
    return crossings * rate / (2.0 * len(loud))


def hda_plays_real_audio():
    """An AC'97 and an HDA in the same machine: HDA wins, and what it sends to
    the speakers is a 440 Hz tone of the length that was asked for."""
    wav = "audio-hda.wav"
    with BoltVM(name="audio-hda", net=False, audio="both", wav=wav) as vm:
        sign_in(vm)
        log = vm.serial()
        m = re.search(r"\[ok\] Intel HDA \S+ \(codec (\d+), pin (\d+) -> dac (\d+), "
                      r"(CORB/RIRB|immediate)\)", log)
        assert m, "the HDA driver did not claim the controller:\n" + log[-1500:]
        print("  ok: codec %s routed pin %s -> dac %s over %s"
              % (m.group(1), m.group(2), m.group(3), m.group(4)))
        assert "AC97 audio" not in log, \
            "the AC'97 was opened even though an HDA was available:\n" + log[-1200:]
        print("  ok: the AC'97 in the same machine was left alone")

        open_terminal(vm)
        # Two lengths. One would also be satisfied by a driver that waits a
        # fixed time and streams nothing; both together will not be.
        short_ms = play(vm, 300, "Intel HDA")
        long_ms = play(vm, 1200, "Intel HDA")
        assert long_ms > short_ms * 2, (
            "playback does not scale with the length asked for: 300 ms took "
            "%d ms and 1200 ms took %d ms. A fixed wait looks like this."
            % (short_ms, long_ms))
        print("  ok: 300 ms took %d ms, 1200 ms took %d ms" % (short_ms, long_ms))
        vm.expect_absent("KERNEL PANIC")
        wav_path = vm.wav_path

    # QEMU has exited, so the capture is complete and its header is fixed up.
    rate, chans, frames, samples = read_wav(wav_path)
    assert chans == 2, "the capture is %d-channel, not stereo" % chans
    print("  ok: QEMU received %d frames at %d Hz, stereo" % (frames, rate))

    # 1.5 s of tone went out; the pad and scheduling slack put the rest around
    # it. Well under a second means most of the samples never left the machine.
    secs = frames / float(rate)
    assert secs >= 1.2, \
        "only %.2f s of audio reached the sound card for 1.5 s of tone" % secs
    print("  ok: %.2f s of audio arrived at the sound card" % secs)

    hz = dominant_hz(samples, chans, rate)
    assert abs(hz - TONE_HZ) < TONE_HZ * 0.1, (
        "the tone came out at %.0f Hz, not %d Hz. The stream format or the "
        "sample rate the converter was programmed with is wrong."
        % (hz, TONE_HZ))
    print("  ok: it is a %.0f Hz tone (asked for %d Hz)" % (hz, TONE_HZ))


def ac97_still_plays():
    """No HDA at all: the older codec is still the one that plays."""
    wav = "audio-ac97.wav"
    with BoltVM(name="audio-ac97", net=False, audio="ac97", wav=wav) as vm:
        sign_in(vm)
        log = vm.serial()
        assert "AC97 audio" in log, "the AC'97 was not claimed:\n" + log[-1500:]
        assert "Intel HDA" not in log, \
            "an HDA was reported in a machine that has none:\n" + log[-1200:]
        print("  ok: AC'97 claimed, and no HDA invented")

        open_terminal(vm)
        play(vm, 1200, "AC97")
        vm.expect_absent("KERNEL PANIC")
        wav_path = vm.wav_path

    rate, chans, frames, samples = read_wav(wav_path)
    hz = dominant_hz(samples, chans, rate)
    assert abs(hz - TONE_HZ) < TONE_HZ * 0.1, \
        "the AC'97 path came out at %.0f Hz, not %d Hz" % (hz, TONE_HZ)
    print("  ok: AC'97 still delivers a %.0f Hz tone through the new interface" % hz)


def hda_without_codec():
    """A controller with an empty link is not an audio device."""
    with BoltVM(name="audio-bare", net=False, audio="hda-nocodec") as vm:
        vm.expect("no codec attached", timeout=180)
        vm.expect("PC speaker only", timeout=60)
        print("  ok: an HDA with no codec is declined, not claimed")
        vm.expect_absent("KERNEL PANIC")


def main():
    hda_plays_real_audio()
    ac97_still_plays()
    hda_without_codec()
    print("PASS audio: HDA drives real hardware, AC'97 still drives the rest")
    return 0


if __name__ == "__main__":
    sys.exit(main())
