#!/usr/bin/env python3
"""Display scaling: a smaller desktop blown back up to fill the panel.

Two things have to be true and only one of them is arithmetic. The desktop has
to become the size the scale implies -- `display` and `snap` both report it --
and the *pointer* has to follow, because the mouse reports physical panel pixels
and every hit test is in logical ones. A compositor that draws scaled but
hit-tests unscaled looks perfect in a screenshot and is unusable.

So the last part clicks a window's maximise button at the physical position its
logical rectangle maps to, and checks the window actually restored.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

LOGIN_USER, LOGIN_PASS = "user", "bolt"

PANEL_W, PANEL_H, SHELF_H = 1024, 768, 68


def gui_run(vm, line, expect, timeout=30):
    vm.clear()
    vm.type(line + "\n")
    vm.expect(expect, timeout=timeout)
    return vm.serial()


def field(out, name):
    for ln in out.splitlines():
        if ln.startswith(name):
            return ln[len(name):].strip()
    raise AssertionError("no %r line in:\n%s" % (name, out[-800:]))


def snap_line(vm, arg=""):
    out = gui_run(vm, ("snap " + arg).strip(), "usage: snap" if not arg else "snap: ")
    for ln in out.splitlines():
        if arg and ln.startswith("snap: ") and "+" in ln:
            return ln
        if not arg and ": " in ln and "+" in ln and "usage" not in ln:
            return ln
    raise AssertionError("no rectangle in:\n" + out[-800:])


def main():
    with BoltVM(name="scale", net=False) as vm:
        vm.sign_in(LOGIN_USER, LOGIN_PASS)
        vm.open_app("Terminal")

        out = gui_run(vm, "display", "scales")
        assert field(out, "panel") == "1024 x 768", out[-600:]
        assert field(out, "desktop") == "1024 x 768", out[-600:]
        assert "100%" in field(out, "scale"), "Auto on a 1024x768 panel is 100%:\n" + out[-600:]
        print("  ok: a small panel is not scaled, and Auto says so")

        out = gui_run(vm, "display scale 150%", "desktop")
        assert field(out, "desktop") == "682 x 512", \
            "150%% of 1024x768 is a 682x512 desktop, got %r" % field(out, "desktop")
        print("  ok: 150% gives the desktop the panel divided by 1.5")

        # The window manager works in the new coordinate space, not the old one.
        ln = snap_line(vm, "max")
        assert "682x%d+0+0" % (512 - SHELF_H) in ln, \
            "a maximised window should fill the scaled desktop:\n" + ln
        print("  ok: maximise fills the scaled desktop, not the panel")

        # Click the maximise button where the *panel* has it: the third traffic
        # light, 18 + 2 * 20 px in from the window's left edge (light_cx() in
        # kernel/gui_shell.c), on the middle of the 32px title bar.
        lx, ly = 18 + 2 * 20, 16
        vm.click(lx * PANEL_W // 682, ly * PANEL_H // 512)
        time.sleep(0.6)
        ln = snap_line(vm)
        assert "floating" in ln, \
            "a click at the scaled position missed the caption button:\n" + ln
        print("  ok: the pointer is mapped into the scaled desktop")

        # The rect it restored to was saved on a 1024-wide desktop. It has to
        # be cut down to this one, or the window comes back with its own close
        # button past the right edge -- which is how this was found. The bottom
        # is allowed to sit under the dock; any window you drag down does.
        w, h = (int(v) for v in ln.split()[-1].split("+")[0].split("x"))
        x, y = (int(v) for v in ln.split()[-1].split("+")[1:])
        assert 0 <= x and x + w <= 682 and 0 <= y < 512 - SHELF_H, \
            "restored to %dx%d+%d+%d: its title bar is off the 682x512 desktop" % (w, h, x, y)
        print("  ok: a window restored after the desktop shrank still fits on it")

        shot = vm.screenshot("scale-150.ppm")
        assert os.path.getsize(shot) > 1000, "no frame while scaled"

        # 200% of this panel would leave 512x384, which is not a desktop.
        out = gui_run(vm, "display scale 200%", "desktop")
        assert field(out, "desktop") == "1024 x 768", \
            "a scale that leaves too little should be declined:\n" + out[-600:]
        assert "declined" in field(out, "scale"), \
            "declining it quietly is worse than not offering it:\n" + out[-600:]
        print("  ok: a scale with no room left is declined, and says so")

        out = gui_run(vm, "display scale 100%", "desktop")
        assert field(out, "desktop") == "1024 x 768", out[-600:]
        out = gui_run(vm, "display scale 300%", "no scale called")
        print("  ok: back to native, and a scale that does not exist is refused")

        vm.expect_absent("KERNEL PANIC")

    print("PASS scale: the desktop scales and the pointer scales with it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
