#!/usr/bin/env python3
"""Quick settings: brightness that really dims, and one panel with all of it.

Brightness here is a software dimmer -- there is no backlight on a machine with
no panel -- so it is one of the few desktop features that can be measured
exactly from outside: ask for 40% and the average pixel on the screen has to be
about 40% of what it was.

The awkward interaction it creates is the one worth testing twice. Captures are
taken from VRAM, so a screenshot at 40% would be a dark picture of a screen
nobody remembers being dark. fb_capture undoes the dim; this proves it by
taking a screenshot while dimmed, turning the brightness back up, and looking
at that screenshot through the OS's own image viewer.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

LOGIN_USER, LOGIN_PASS = "user", "bolt"

SCREEN_W, SCREEN_H, TASKBAR_H = 1024, 768, 48


def gui_run(vm, line, expect, timeout=45):
    vm.clear()
    vm.type(line + "\n")
    vm.expect(expect, timeout=timeout)
    return vm.serial()


def settle(vm, name, pause=1.2):
    """Screendump after letting the compositor finish a frame.

    QEMU dumps the panel while the guest is still running, and a brightness
    change repaints all 786,000 pixels -- the first version of this test caught
    a frame that was bright down to row 290 and dim below it, and read the
    average of the two.
    """
    time.sleep(pause)
    return ppm_mean(vm.screenshot(name))


def ppm_mean(path, step=37):
    """Average channel value over a sparse sample of the frame."""
    with open(path, "rb") as fh:
        data = fh.read()
    parts = data.split(b"\n", 3)
    assert parts[0].startswith(b"P6"), "not a PPM: " + path
    px = parts[3]
    total = n = 0
    for i in range(0, len(px) - 3, 3 * step):
        total += px[i] + px[i + 1] + px[i + 2]
        n += 3
    return total / max(n, 1)


def field(out, name):
    for ln in out.splitlines():
        if ln.startswith(name):
            return ln[len(name):].strip()
    raise AssertionError("no %r line in:\n%s" % (name, out[-800:]))


def main():
    with BoltVM(name="quick", net=False) as vm:
        vm.sign_in(LOGIN_USER, LOGIN_PASS)
        vm.open_app("Terminal")

        out = gui_run(vm, "display", "bright")
        assert "100%" in field(out, "bright"), out[-600:]
        assert "software dimmer" in field(out, "bright"), \
            "with no backlight driver it should say what is dimming:\n" + out[-600:]
        full = settle(vm, "bright-100.ppm")
        print("  ok: brightness starts at 100%% (mean pixel %.1f)" % full)

        gui_run(vm, "display brightness 40", "bright")
        dim = settle(vm, "bright-40.ppm")
        ratio = dim / full
        assert 0.30 < ratio < 0.55, \
            "40%% brightness made the screen %.0f%% as bright" % (ratio * 100)
        print("  ok: 40%% really is about 40%% of the light (measured %.0f%%)"
              % (ratio * 100))

        # A screenshot taken while dimmed, to look at later.
        gui_run(vm, "screenshot /home/user/dimshot.png", "screenshot: /")

        gui_run(vm, "display brightness 100", "bright")
        back = settle(vm, "bright-back.ppm")
        assert abs(back - full) / full < 0.06, \
            "turning it back up did not restore the screen (%.1f vs %.1f)" % (back, full)
        print("  ok: turning it back up restores the screen exactly")

        out = gui_run(vm, "display brightness 5", "brightness is 10 to 100")
        print("  ok: a brightness nobody could read the way out of is refused")

        out = gui_run(vm, "power status", "power:")
        assert "On AC power" in out or "Battery" in out, out[-500:]
        assert "software dimmer" in out, "power status should name the dimmer:\n" + out[-500:]
        print("  ok: `power status` says where the power is coming from")

        # The undim: that dimmed screenshot, viewed now that the screen is not
        # dim. If fb_capture had kept the dim, this picture would be dark.
        vm.key("esc"); time.sleep(0.3)
        vm.key("meta_l"); time.sleep(0.9)
        vm.type("dimshot"); time.sleep(0.9)
        vm.key("ret"); time.sleep(2.5)
        vm.key("up", meta=True); time.sleep(1.0)      # maximise Photos
        shown = settle(vm, "bright-undim.ppm")
        assert shown > full * 0.75, \
            ("a screenshot taken at 40%% came back dark (%.1f vs a %.1f screen): "
             "fb_capture is not undoing the dim" % (shown, full))
        print("  ok: a screenshot taken while dimmed is not a dark picture")

        vm.open_app("Terminal")

        # The panel itself. The tray speaker opens it; the second slider row is
        # brightness, and clicking halfway along it has to mean 50%.
        px = min(SCREEN_W - 96 + 14 - 126, SCREEN_W - 6 - 252)
        py = SCREEN_H - TASKBAR_H - 186 - 8
        vm.click(SCREEN_W - 96 + 14, SCREEN_H - TASKBAR_H + 14)
        time.sleep(0.6)
        shot = vm.screenshot("quick-open.ppm")
        assert os.path.getsize(shot) > 1000, "no frame with the panel open"

        vm.click(px + 44 + 81, py + 34 + 34 + 12)     # halfway along row 1
        time.sleep(0.5)
        vm.click(SCREEN_W - 96 + 14, SCREEN_H - TASKBAR_H + 14)   # close it
        time.sleep(0.4)

        out = gui_run(vm, "display", "bright")
        got = int(field(out, "bright").split("%")[0])
        assert 44 <= got <= 56, \
            "clicking the middle of the brightness slider gave %d%%" % got
        print("  ok: the panel's brightness slider sets what it points at (%d%%)" % got)

        gui_run(vm, "display brightness 100", "bright")
        vm.expect_absent("KERNEL PANIC")

    print("PASS quick: brightness, and a tray panel with more than a volume slider")
    return 0


if __name__ == "__main__":
    sys.exit(main())
