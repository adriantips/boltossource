#!/usr/bin/env python3
"""The media player: audio that keeps playing while the machine does other things.

`play FILE` already proved the decoder and the codec work, but it plays a track
the way a test does -- it holds the calling thread until the last sample is
gone. Nothing about that makes a machine you can put music on. This test is
about the difference.

Three claims, and none of them can be checked from inside the guest:

  1. Opening a file returns immediately. The command prints the track's state
     and gives the prompt back while the track is still playing.
  2. Nobody has to drive it. Nothing is typed for six seconds, and audio keeps
     arriving at the sound card the whole time -- across a dozen handovers from
     one half-second buffer to the next, and across the end of a track, because
     the folder is the playlist and the player moves on by itself.
  3. Pause stops the samples, and nothing quietly restarts them.

The evidence for 2 is QEMU's audio backend, which writes what the guest sends to
its sound card into a file. It records only while a stream is running, so the
length of that file is the length of the audio that actually left the machine:
four seconds of it, when the two system sounds together hold one and a half, can
only have come from something feeding the codec continuously while the shell sat
idle. Both sounds have to appear in it at their own pitches, which is what shows
the player advanced from one track to the next rather than looping the first.

Skips are checked with the player paused (`media next ; media pause`, one line,
so nothing happens between them). A half-second track playing while a test types
its next command would otherwise advance on its own mid-assertion -- which is
the feature working, and would look like the skip failing.
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM
from wavcheck import read_wav, freq_windows, held_tone, seconds

LOGIN_USER, LOGIN_PASS = "user", "bolt"
SOUNDS = "/usr/share/sounds"
CHIME = SOUNDS + "/chime.wav"
CHIME_HZ, BLIP_HZ = 880, 1320
FREE_RUN_S = 6.0            # how long nothing is typed while it plays
MIN_AUDIO_S = 4.0           # what must reach the sound card over the whole run


def sign_in(vm):
    vm.sign_in(LOGIN_USER, LOGIN_PASS, timeout=180)
    vm.open_app("Terminal")


def run(vm, line, wait_for, timeout=60, settle=0.0):
    vm.clear()
    vm.type(line + "\n")
    assert vm.wait_for(wait_for, timeout=timeout), \
        "%r never printed %r:\n%s" % (line, wait_for, vm.serial()[-900:])
    if settle:                       # a second command on the same line to land
        time.sleep(settle)
    return vm.serial()


def field(out, key):
    """The last value of a `media status` field, e.g. field(out, 'State')."""
    m = re.findall(r"^%s\s*: (.*)$" % key, out, re.M)
    assert m, "no %s line in:\n%s" % (key, out[-700:])
    return m[-1].strip()


def status(vm):
    return run(vm, "media status", "Device  :")


def main():
    with BoltVM(name="media", net=False, audio="hda", wav="media-capture.wav") as vm:
        sign_in(vm)

        # ---- 1. it starts, and hands the prompt straight back ---------------
        t0 = time.time()
        out = run(vm, "media open " + CHIME, "Device  :")
        elapsed = time.time() - t0
        assert field(out, "Track") == "chime.wav", \
            "the wrong track is open: %s" % field(out, "Track")
        assert field(out, "State") == "playing", \
            "opening a file did not start it: %s" % field(out, "State")
        assert "Problem" not in out, "the player reported a problem:\n" + out[-600:]
        print("  ok: `media open` started chime.wav and returned in %.1fs" % elapsed)

        # The folder it came out of is the playlist, in name order.
        out = run(vm, "media list", "blip.wav")
        assert re.search(r"^\*\s+\d+\s+chime\.wav", out, re.M), \
            "the playing track is not marked in the list:\n" + out[-400:]
        print("  ok: the folder is the playlist, with the playing track marked")

        # ---- 2. nobody drives it for six seconds ----------------------------
        # A 1.0 s track and a 0.5 s one: to still be playing after six seconds
        # the player has to have crossed a dozen buffer handovers and the end of
        # a track several times, with nothing but the desktop's main loop
        # feeding it.
        time.sleep(FREE_RUN_S)
        out = status(vm)
        assert field(out, "State") == "playing", \
            "it stopped while nothing was touching it: %s" % field(out, "State")
        playing_now = field(out, "Track")
        assert playing_now in ("chime.wav", "blip.wav"), "unexpected track %r" % playing_now
        print("  ok: still playing (%s) after %.0fs of nobody driving it"
              % (playing_now, FREE_RUN_S))

        # ---- 3. pause stops it, and it stays stopped -------------------------
        out = run(vm, "media pause", "Device  :")
        assert field(out, "State") == "paused", "pause did not pause: " + field(out, "State")
        paused_at = field(out, "Position")
        time.sleep(2.5)
        out = status(vm)
        assert field(out, "State") == "paused", \
            "something restarted a paused track: " + field(out, "State")
        assert field(out, "Position") == paused_at, \
            "a paused track's position moved on its own: %s -> %s" \
            % (paused_at, field(out, "Position"))
        print("  ok: paused at %s, and still there 2.5s later" % paused_at)

        # ---- resume, and `play` that means play ------------------------------
        out = run(vm, "media play", "Device  :")
        assert field(out, "State") == "playing", "resume did not resume"
        out = run(vm, "media play", "Device  :")
        assert field(out, "State") == "playing", \
            "`media play` on a playing track stopped it -- that is toggle, not play"
        print("  ok: play resumes, and `play` twice does not mean stop")

        # ---- skipping, measured with the player held still -------------------
        out = run(vm, "media next ; media pause", "State   : paused", settle=0.5)
        first = field(out, "Track")
        out = run(vm, "media next ; media pause", "State   : paused", settle=0.5)
        second = field(out, "Track")
        assert second != first, "next did not move off %s" % first
        print("  ok: next walks the playlist (%s -> %s)" % (first, second))

        out = run(vm, "media seek 0:00", "Device  :")
        assert field(out, "Position").startswith("0:00"), \
            "seek to the start reported %s" % field(out, "Position")
        out = run(vm, "media vol 55", "volume")
        assert "volume 55%" in out, "volume was not set:\n" + out[-300:]
        print("  ok: seek and volume are the same controls the window has")

        # ---- one codec, one stream -------------------------------------------
        # `play` blocks until its samples are gone, so it cannot share the
        # device with a track that is streaming through it. It takes it, and
        # the player is left paused rather than fighting for the DMA engine.
        run(vm, "media play", "State   : playing")
        run(vm, "play 660 200", "ms elapsed")
        out = status(vm)
        assert field(out, "State") == "paused",             "a blocking `play` did not take the codec off the player: %s"             % field(out, "State")
        print("  ok: `play` preempts the player instead of talking over it")

        # ---- what it refuses --------------------------------------------------
        out = run(vm, "media open " + SOUNDS + "/nope.wav", "media:")
        assert "cannot open" in out, "a missing file was not reported:\n" + out[-400:]
        out = run(vm, "media open /bin/hello", "media:")
        assert "not a WAV file" in out, \
            "an ELF was not refused as a non-WAV:\n" + out[-400:]
        print("  ok: a missing file and a non-WAV are refused, and differently")

        # A last look at the window itself. It has been behind the Terminal all
        # along -- opening the player from a prompt deliberately does not take
        # the keyboard, and in this compositor the topmost window is the one
        # with the keyboard -- so bring it to the front to photograph it, which
        # is the point at which the Terminal stops receiving anything.
        run(vm, "media open " + CHIME, "Device  :")
        vm.open_app("Media", probe=False)
        vm.screenshot("media.ppm")
        print("  ok: the window is in test/out/media.ppm")

        vm.expect_absent("KERNEL PANIC")
        wav_path = vm.wav_path

    # QEMU has exited, so the capture is complete and its header fixed up.
    rate, chans, frames, samples = read_wav(wav_path)
    total = seconds(frames, rate)
    print("  ok: %.2f s of audio reached the sound card" % total)

    assert total >= MIN_AUDIO_S, (
        "only %.2f s of audio arrived, out of at least %.0f s of playing. The "
        "player stopped feeding the codec as soon as nobody was typing at it, "
        "which is the one thing it exists to do." % (total, MIN_AUDIO_S))

    wins = freq_windows(samples, chans, rate)
    for hz, what in ((CHIME_HZ, "chime.wav"), (BLIP_HZ, "blip.wav")):
        assert held_tone(wins, hz), (
            "no sustained %d Hz in the capture, so %s never played. Windows "
            "measured: %s" % (hz, what, ", ".join("%.0f" % f for _, f in wins if f)))
    print("  ok: both tracks are in the capture, so it advanced without being asked")

    print("PASS media: audio that plays on while the machine gets on with something else")
    return 0


if __name__ == "__main__":
    sys.exit(main())
