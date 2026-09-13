#!/usr/bin/env python3
"""The microphone: a capture path, and an honest account of what QEMU can prove.

Start with the limit, because it decides every assertion below. **No emulated
audio backend available here can put a sound into a guest.** QEMU's `wav`
backend is output-only, and its `none` backend answers a capture stream with
silence. So the one step a real microphone would be involved in cannot be
exercised at all, and every recording this test makes is silent.

What is left is still most of the driver, and it is the half that breaks:

  * The codec graph walk *backwards*. An ADC arrives connected to nothing, the
    same way a DAC does, and an input pin has to be found, scored, selected at
    every fork and switched on -- including its bias supply, without which a
    real electret capsule produces nothing while every register reads back
    correctly. The driver says which pin and which ADC it settled on, and that
    line is checked.

  * The stream engine. A capture ring whose position counter advances at 48000
    frames a second is a running DMA engine. One that was never started, or
    was programmed with the wrong cyclic length, or whose BDL points somewhere
    the controller cannot reach, produces nothing -- and `mic test` reporting
    zero frames is exactly the shape of every one of those bugs.

  * The ring reader, which is the fiddly part: a position that wraps, a span
    that straddles the end of the buffer, and a reader that must not hand back
    samples it has already handed back.

**The rate is timed from out here, and that is not belt-and-braces.** The guest
cannot time itself: there is no HPET device in this QEMU build at all (it ships
without CONFIG_HPET, so `-machine hpet=on` is accepted and does nothing), which
leaves the PIT, and the PIT under-counts badly inside a loop that polls MMIO a
million times a second. A three-second capture measured from inside came back
claiming 66 kHz; from outside, the same capture takes the three seconds it
should. So the guest counts frames -- the sample clock is exact and is the only
good clock it has -- and the host holds the stopwatch. A stream running at
double rate, half rate, or not at all is caught by the wall clock here and by
nothing the machine could have said about itself.

  * That a capture becomes a file something else can read. `mic rec` writes a
    WAV, and the OS's own decoder is asked to open it -- so the header this
    machine writes and the header it reads agree, which is not a given when
    the two were written months apart.

Both codecs are exercised, because the input path was written twice: HDA walks
a widget graph, AC'97 programs a record-select register, and the two have
nothing in common below the interface.
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

LOGIN_USER, LOGIN_PASS = "user", "bolt"
REC = "/home/user/cap.wav"


def sign_in(vm):
    vm.sign_in(LOGIN_USER, LOGIN_PASS, timeout=180)


def run(vm, line, wait_for, timeout=90):
    vm.clear()
    vm.type(line + "\n")
    assert vm.wait_for(wait_for, timeout=timeout), \
        "%r never printed %r:\n%s" % (line, wait_for, vm.serial()[-1000:])
    return vm.serial()


def capture(vm, secs):
    """`mic test SECS`, timed from here. Returns (frames, wanted, peak, wall_s)."""
    vm.clear()
    t0 = time.time()
    vm.type("mic test %d\n" % secs)
    assert vm.wait_for("frames (", timeout=60 + secs * 6), \
        "mic test never finished:\n" + vm.serial()[-900:]
    wall = time.time() - t0
    out = vm.serial()
    m = re.search(r"mic: (\d+) of (\d+) frames \(\d+ ms of audio\), peak (\d+)%", out)
    assert m, "mic test did not report a frame count:\n" + out[-800:]
    return int(m.group(1)), int(m.group(2)), int(m.group(3)), wall


def one_codec(device, expect_route):
    with BoltVM(name="mic-" + device, net=False, audio=device) as vm:
        sign_in(vm)
        # Before opening the Terminal: open_app() clears the serial buffer, and
        # the driver's probe line is in the boot log, not after it.
        log = vm.serial()
        vm.open_app("Terminal")

        if expect_route:
            m = re.search(r"\[ok\] HDA capture: (.+?) \(pin (\d+) -> adc (\d+)\)", log)
            assert m, "the HDA driver found no input route:\n" + log[-1500:]
            print("  ok: %s traced %s back from adc %s to pin %s"
                  % (device, m.group(1), m.group(3), m.group(2)))

        out = run(vm, "mic", "mic:")
        assert "no input route" not in out, \
            "%s reports no input to record from:\n%s" % (device, out[-600:])
        print("  ok: %s names the jack it is listening on" % device)

        # Three seconds, because the ring is 0.68 s long: anything shorter
        # would not prove the reader survives a wrap, let alone four of them.
        frames, want, peak, wall = capture(vm, 3)
        assert frames > 0, \
            ("the capture engine produced no frames at all on %s -- it is not "
             "running" % device)
        assert frames == want, (
            "%s stopped after %d of %d frames. A cyclic buffer that is not "
            "cyclic looks exactly like this: it fills once and halts, with the "
            "run bit still set and no error bit anywhere." % (device, frames, want))
        # The overhead here is a constant, measured at 0.83-1.08 s: typing the
        # command, and the 0.25 s serial poll. The marginal cost is exactly one
        # second of wall time per second of audio, so the window only has to be
        # wide enough for that constant -- and it is narrow enough that a stream
        # running at double rate (3 s of frames in 1.5) or half (in 6) falls out
        # of it.
        assert 2.8 < wall < 5.5, (
            "%s produced 3 seconds of frames in %.2f s of real time -- the "
            "stream is not running at 48 kHz" % (device, wall))
        print("  ok: %s captured %d frames in %.2f s, across four ring wraps"
              % (device, frames, wall))
        assert peak == 0, (
            "%s captured audio at peak %d%% -- which would be a surprise, since "
            "no backend here can feed a guest sound. Check what it is really "
            "reading." % (device, peak))
        print("  ok: %s recorded silence, which is all QEMU can give it" % device)

        # A recording, and the OS's own decoder reading it back.
        out = run(vm, "mic rec %s 2" % REC, "frames,", timeout=90)
        m = re.search(r"mic: \S+ -- (\d+) frames, (\d+) ms", out)
        assert m, "mic rec did not report what it wrote:\n" + out[-800:]
        wrote_ms = int(m.group(2))
        assert 1500 < wrote_ms < 2600, \
            "asked for 2 s and got %d ms of samples" % wrote_ms
        print("  ok: %s wrote %d ms to a WAV" % (device, wrote_ms))

        # `ls -l` prints sizes in KiB, which is enough: a header that lied
        # about its length by a chunk or a frame count off by a channel would
        # both be visible at this resolution.
        out = run(vm, "ls -l /home/user", "cap.wav")
        m = re.search(r"cap\.wav", out) and re.search(r"([\d.]+)K\s+cap\.wav", out)
        assert m, "cap.wav is not in the listing, or has no size:\n" + out[-700:]
        kib = float(m.group(1))
        expect = (44 + int(48000 * wrote_ms / 1000) * 4) / 1024.0
        assert abs(kib - expect) < 4.0, \
            "cap.wav is %.1fK; %d ms of 48 kHz stereo plus a header is %.1fK" \
            % (kib, wrote_ms, expect)
        print("  ok: the file on disk is %.1fK, the size 48 kHz stereo implies" % kib)

        # The decoder this machine plays with, reading the header it just
        # wrote. `play` would hold the shell for two seconds; `media open`
        # parses it and returns -- and the duration it reports is the header
        # being believed, not merely accepted.
        out = run(vm, "media open " + REC, "cap.wav")
        assert "cannot" not in out.lower() and "not a" not in out.lower(), \
            "the OS could not read back its own recording:\n" + out[-700:]
        m = re.search(r"Position: \d+:\d\d / (\d+):(\d\d)", out)
        assert m, "media did not report a duration for it:\n" + out[-700:]
        secs = int(m.group(1)) * 60 + int(m.group(2))
        assert secs == 2, \
            "the decoder makes the 2 s recording %d s long -- the rate or the " \
            "channel count in the header it wrote disagrees with the one it " \
            "reads" % secs
        run(vm, "media stop", "State")
        print("  ok: %s's decoder reads its own file back as %d s" % (device, secs))

        vm.expect_absent("KERNEL PANIC")


def no_input_at_all():
    """A codec with no way in says so, instead of recording zeros forever."""
    with BoltVM(name="mic-none", net=False, audio="hda-nocodec") as vm:
        vm.reach_shell(180, "2")
        time.sleep(0.5)
        vm.key("ret")
        time.sleep(0.5)
        out = run(vm, "mic", "mic:")
        assert "no input route" in out, \
            "a machine with no codec should say it has nothing to record from:\n" \
            + out[-600:]
        print("  ok: no codec -> `mic` says there is nothing to record from")
        run(vm, "mic test", "no input route")
        print("  ok: and `mic test` refuses rather than reporting a silent success")
        vm.expect_absent("KERNEL PANIC")


def main():
    one_codec("hda", expect_route=True)
    one_codec("ac97", expect_route=False)
    no_input_at_all()
    print("PASS mic: a capture path on both codecs, and silence that is honest about why")
    return 0


if __name__ == "__main__":
    sys.exit(main())
