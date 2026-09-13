#!/usr/bin/env python3
"""Window tiling: halves, quarters, and the preview you get while dragging.

The zones are arithmetic, so they can be checked exactly -- `snap` prints the
focused window's rectangle and this test compares it against the numbers the
screen size implies. What cannot be checked that way is the drop preview, which
exists only while a button is held. So the last part really does hold the
button down, screenshots the frame mid-drag, and looks at a pixel inside the
landing zone that the dragged window is not covering.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

LOGIN_USER, LOGIN_PASS = "user", "bolt"

# The boot mode (1024x768) and TASKBAR_H in kernel/gui.c.
SCREEN_W, SCREEN_H, TASKBAR_H = 1024, 768, 48
AVAIL = SCREEN_H - TASKBAR_H


def rect(zone):
    """The same arithmetic snap_rect() does, written out independently."""
    hw, hh = SCREEN_W // 2, AVAIL // 2
    return {
        "left":         (hw,            AVAIL,      0,  0),
        "right":        (SCREEN_W - hw, AVAIL,      hw, 0),
        "top-left":     (hw,            hh,         0,  0),
        "top-right":    (SCREEN_W - hw, hh,         hw, 0),
        "bottom-left":  (hw,            AVAIL - hh, 0,  hh),
        "bottom-right": (SCREEN_W - hw, AVAIL - hh, hw, hh),
        "max":          (SCREEN_W,      AVAIL,      0,  0),
    }[zone]


def geom(zone):
    w, h, x, y = rect(zone)
    return "%dx%d+%d+%d" % (w, h, x, y)


def gui_run(vm, line, expect, timeout=30):
    vm.clear()
    vm.type(line + "\n")
    vm.expect(expect, timeout=timeout)
    return vm.serial()


def snap_line(vm, arg=""):
    """Run `snap [arg]` and return the line describing the focused window."""
    out = gui_run(vm, ("snap " + arg).strip(), "usage: snap" if not arg else "snap: ")
    for ln in out.splitlines():
        if arg and ln.startswith("snap: ") and "+" in ln:
            return ln
        if not arg and ": " in ln and "+" in ln and "usage" not in ln:
            return ln
    raise AssertionError("no rectangle in:\n" + out[-800:])


def ppm_pixel(path, x, y):
    with open(path, "rb") as fh:
        data = fh.read()
    parts = data.split(b"\n", 3)
    assert parts[0].startswith(b"P6"), "not a PPM: " + path
    w, h = (int(v) for v in parts[1].split())
    px = parts[3]
    i = (y * w + x) * 3
    return tuple(px[i:i + 3])


def main():
    with BoltVM(name="snap", net=False) as vm:
        vm.sign_in(LOGIN_USER, LOGIN_PASS)
        vm.open_app("Terminal")

        before = snap_line(vm)
        assert "floating" in before, "a freshly opened window should float:\n" + before
        float_rect = before.split()[-1]
        print("  ok: a new window is floating, at " + float_rect)

        # Every zone, from the command, checked against the arithmetic.
        for zone in ("left", "right", "top-left", "top-right",
                     "bottom-left", "bottom-right", "max"):
            ln = snap_line(vm, zone)
            assert geom(zone) in ln, "snap %s gave %r, wanted %s" % (zone, ln, geom(zone))
        print("  ok: halves, quarters and maximise land on the exact rectangles")

        ln = snap_line(vm, "restore")
        assert float_rect in ln, "restore did not put it back: %r wanted %s" % (ln, float_rect)
        print("  ok: restore returns the window to the size it had before tiling")

        # Win+arrow walks between them: left half, then up to the top-left
        # quarter, then down to the left half again.
        vm.key("left", meta=True);  time.sleep(0.6)
        ln = snap_line(vm)
        assert geom("left") in ln, "Win+Left did not give the left half:\n" + ln
        vm.key("up", meta=True);    time.sleep(0.6)
        ln = snap_line(vm)
        assert geom("top-left") in ln, "Win+Left then Win+Up is the top-left quarter:\n" + ln
        vm.key("down", meta=True);  time.sleep(0.6)
        ln = snap_line(vm)
        assert geom("left") in ln, "Win+Down from a quarter is the half below it:\n" + ln
        print("  ok: Win+arrow walks halves and quarters")

        # Park it somewhere floating and roomy for the drag.
        snap_line(vm, "restore")
        ln = snap_line(vm)
        w, h = (int(v) for v in ln.split()[-1].split("+")[0].split("x"))
        x, y = (int(v) for v in ln.split()[-1].split("+")[1:])

        # A point inside the top-left quarter that the dragged window will not
        # be covering: grabbing the title bar near its right end pushes the
        # window off the left edge, where clamp_window() stops it. Not too near
        # the right end, though -- the last 138px of a title bar are the close,
        # maximise and minimise buttons, and the first version of this test
        # grabbed the close button and spent a while looking like a focus bug.
        probe = (300, 200)
        grab_x = x + w - 160
        clean = vm.screenshot("snap-before.ppm")
        base = ppm_pixel(clean, *probe)

        seen = {}
        def look():
            time.sleep(0.4)
            shot = vm.screenshot("snap-dragging.ppm")
            seen["px"] = ppm_pixel(shot, *probe)

        vm.drag(grab_x, y + 14, 1, 40, hold=look)
        assert seen["px"] != base, \
            "no drop preview under the pointer at the top-left corner: %r == %r" % (seen["px"], base)
        print("  ok: dragging to a corner previews the zone it would land in")

        ln = snap_line(vm)
        assert geom("top-left") in ln, "the drop did not tile it top-left:\n" + ln
        print("  ok: releasing there tiles the window into that quarter")

        vm.expect_absent("KERNEL PANIC")

    print("PASS snap: halves, quarters, Win+arrow and a drop preview")
    return 0


if __name__ == "__main__":
    sys.exit(main())
