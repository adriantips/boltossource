#!/usr/bin/env python3
"""The volume mixer: named channels, a mute that remembers, and levels that persist.

The regression this exists for is small and irritating and was written out four
separate times in the desktop: mute spelled `set_volume(vol > 0 ? 0 : 80)`. Turn
a track down to 15, mute it, unmute it, and you are at 80 -- which on a laptop at
one in the morning is not a rounding error. So the first thing asserted here is
that a mute is a mute: the level goes to nothing and comes back exactly where it
was, and it does that at 15 rather than only at the default.

The second is that the channels actually compose. `media` at 40 under a `master`
at 50 has to reach the codec at 20, because that product is the entire mix on a
machine with one gain register.

The third is that the levels outlive the boot, since /etc/mixer.conf is the whole
reason the mixer is not just four variables.

And the fourth is the one that cannot be faked from inside the guest: a mute in
the mixer reaches the codec's amplifier. QEMU's audio backend writes what the
guest sends to its sound card into a file, so a muted tone and an unmuted one
can be told apart from out here by amplitude.

It is mute rather than level for a reason worth writing down, because the
obvious test does not work. QEMU applies a codec's *mute* to what it records
unconditionally, but its gain scaling sits behind CONFIG_MIXEMU, which is off in
every build you will meet -- measured here: master 100 and master 25 both come
out at peak 8000, and master 0 comes out silent. So an amplitude comparison
between two non-zero levels asserts nothing about the guest and everything about
how QEMU was compiled. Mute is the part of this path that an emulator will
actually show you, and it is also the part that was broken before: what used to
pass for a mute was `set_volume(0)`, which reaches the same amplifier by a route
that forgets your level on the way back.
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM
from wavcheck import read_wav

LOGIN_USER, LOGIN_PASS = "user", "bolt"
TONE_HZ = 440
TONE_MS = 700


def sign_in(vm):
    vm.sign_in(LOGIN_USER, LOGIN_PASS, timeout=180)
    vm.open_app("Terminal")


def run(vm, line, wait_for, timeout=60):
    vm.clear()
    vm.type(line + "\n")
    assert vm.wait_for(wait_for, timeout=timeout), \
        "%r never printed %r:\n%s" % (line, wait_for, vm.serial()[-900:])
    return vm.serial()


def level(vm, chan):
    """(percent, muted) for one channel, read back from `mixer CHANNEL`."""
    out = run(vm, "mixer " + chan, chan + " = ")
    m = re.search(re.escape(chan) + r" = (\d+)%( \(muted\))?", out)
    assert m, "no level for %s in:\n%s" % (chan, out[-600:])
    return int(m.group(1)), bool(m.group(2))


def model():
    """Everything that can be checked by reading the numbers back."""
    # snapshot=True (the default) still survives a warm reset -- the overlay
    # belongs to the QEMU process, not to the boot -- so the persistence half
    # of this runs without writing /etc/mixer.conf into the shared os.img and
    # leaving it for whatever test runs next.
    with BoltVM(name="mixer", net=False) as vm:
        sign_in(vm)

        out = run(vm, "mixer", "capture")
        for ch in ("master", "media", "system", "app", "capture"):
            assert re.search(r"\b" + ch + r"\b", out), \
                "`mixer` does not list %s:\n%s" % (ch, out[-800:])
        print("  ok: five channels, listed with their levels and meters")

        # A mute is a mute. Set an unusual level so a default cannot pass by
        # accident -- 80 would have passed against the old code.
        run(vm, "mixer media 15", "media = 15%")
        run(vm, "mixer media mute", "muted")
        pct, muted = level(vm, "media")
        assert muted, "media did not mute"
        assert pct == 15, "muting moved the level to %d%%" % pct
        run(vm, "mixer media unmute", "media = 15%")
        pct, muted = level(vm, "media")
        assert not muted and pct == 15, \
            "unmute came back at %d%% (muted=%s), not the 15%% it was" % (pct, muted)
        print("  ok: mute keeps the level; unmute puts it back at 15%, not 80%")

        # Setting a muted channel off zero is the gesture people make instead
        # of finding the mute button again.
        run(vm, "mixer system mute", "muted")
        run(vm, "mixer system 55", "system = 55%")
        _, muted = level(vm, "system")
        assert not muted, "dragging a muted channel up left it muted"
        print("  ok: setting a level on a muted channel unmutes it")

        # Composition: master scales the channels under it.
        run(vm, "mixer master 50", "master = 50%")
        out = run(vm, "mixer media 40", "media = 40%")
        assert "20% after master" in out, \
            "media 40 under master 50 should reach the codec at 20%:\n" + out[-600:]
        print("  ok: media 40 under master 50 is 20% at the codec")

        # Capture is deliberately not under master: turning the speakers down
        # must not turn the microphone down with them.
        out = run(vm, "mixer capture 90", "capture = 90%")
        assert "after master" not in out, \
            "capture should not be scaled by master:\n" + out[-600:]
        print("  ok: master does not scale the microphone's input gain")

        run(vm, "mixer master mute", "muted")
        out = run(vm, "mixer media 40", "media = 40%")
        assert "0% after master" in out, \
            "a muted master should silence the channels under it:\n" + out[-600:]
        run(vm, "mixer master unmute", "master")
        print("  ok: a muted master silences everything under it")

        # The file.
        out = run(vm, "cat /etc/mixer.conf", "master")
        assert "media = 40" in out, "/etc/mixer.conf did not keep media:\n" + out[-600:]
        print("  ok: /etc/mixer.conf holds what was set, without being asked to")

        vm.reset()
        sign_in(vm)
        pct, _ = level(vm, "media")
        assert pct == 40, "media came back at %d%% after a reboot, not 40%%" % pct
        pct, _ = level(vm, "capture")
        assert pct == 90, "capture came back at %d%% after a reboot, not 90%%" % pct
        print("  ok: the levels are still there after a reboot")

        out = run(vm, "mixer reset", "master")
        pct, _ = level(vm, "media")
        assert pct == 100, "`mixer reset` left media at %d%%" % pct
        print("  ok: `mixer reset` restores the defaults")

        vm.expect_absent("KERNEL PANIC")


def peak(samples, chans):
    return max((abs(s) for s in samples[::chans]), default=0)


def reaches_the_hardware():
    """A mute in the mixer is a mute in the codec.

    The long tone is played muted and the short one unmuted, in that order, so
    the silence is at the front of the capture where a tone bleeding across a
    block boundary cannot reach it. Two to one on the durations leaves the first
    30% of the file comfortably inside the muted tone even though the recorded
    lengths wobble by a third -- the DMA engine reports itself drained a little
    before the samples are all gone.

    HDA is the device under test, and that is not a free choice: QEMU's AC'97
    model ships with its mixer compiled out entirely, so it accepts every volume
    write and records at full scale regardless -- including a mute. Pointing
    this at the AC'97 would be a green test on a machine where the knob does
    nothing.
    """
    wav = "mixer-gain.wav"
    with BoltVM(name="mixer-gain", net=False, audio="hda", wav=wav) as vm:
        sign_in(vm)
        run(vm, "mixer master mute", "muted")
        run(vm, "play %d %d" % (TONE_HZ, 1200), "ms elapsed", timeout=90)
        time.sleep(0.5)
        run(vm, "mixer master unmute", "master = ")
        run(vm, "play %d %d" % (TONE_HZ, 600), "ms elapsed", timeout=90)
        vm.expect_absent("KERNEL PANIC")
        path = vm.wav_path

    rate, chans, frames, samples = read_wav(path)
    head = int(frames * 0.30) * chans
    quiet, whole = peak(samples[:head], chans), peak(samples, chans)
    assert whole > 2000, \
        "no tone reached the sound card at all (peak %d over %d frames)" % (whole, frames)
    assert quiet < 500, (
        "a tone played with master muted still reached the sound card at peak "
        "%d -- the mute is not reaching the codec's amplifier" % quiet)
    print("  ok: muted plays at peak %d, unmuted at %d" % (quiet, whole))


def main():
    model()
    reaches_the_hardware()
    print("PASS mixer: channels that compose, a mute that remembers, and gain that lands")
    return 0


if __name__ == "__main__":
    sys.exit(main())
