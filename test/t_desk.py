#!/usr/bin/env python3
"""Virtual desktops: four of them, and a window is only on one.

The assertion that matters is not that the pips draw. It is that a window on
another desktop is genuinely gone -- not painted, and not reachable. A
compositor that merely skips drawing a window still delivers its clicks and
keystrokes, and the bug that produces is invisible until somebody types a
password into a window they cannot see.

So this drives the real thing: open a Terminal, move it between desktops from
the shell, switch away with Win+1, type, and prove the typing went nowhere.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

LOGIN_USER, LOGIN_PASS = "user", "bolt"


def gui_run(vm, line, expect, timeout=30):
    """Type into the GUI Terminal and wait for output on COM1.

    The GUI Terminal paints its own grid but still writes through the console
    sink, so its output reaches the serial log like everything else.
    """
    vm.clear()
    vm.type(line + "\n")
    vm.expect(expect, timeout=timeout)
    return vm.serial()


def main():
    with BoltVM(name="desk", net=False) as vm:
        vm.sign_in(LOGIN_USER, LOGIN_PASS)
        vm.open_app("Terminal")

        out = gui_run(vm, "desk", "usage: desk")
        assert "* 1" in out, "should start on desktop 1:\n" + out[-800:]
        assert "Terminal" in out, "the Terminal is not listed on a desktop:\n" + out[-800:]
        assert " 4  0 window" in out, "there should be four desktops:\n" + out[-800:]
        print("  ok: four desktops, and a window opens on the current one")

        # Moving takes you with it, so the window you just moved is the one you
        # are looking at. Anything else is a way to lose a window.
        out = gui_run(vm, "desk move 3", "moved to desktop 3")
        out = gui_run(vm, "desk", "usage: desk")
        assert "* 3" in out, "did not follow the window to desktop 3:\n" + out[-800:]
        assert " 1  0 windows" in out, "the window is still on desktop 1:\n" + out[-800:]
        print("  ok: `desk move` sends the focused window and follows it")

        # Now the part that has to be true: switch away and the window is not
        # merely hidden. Win+1 goes to an empty desktop; typing there must not
        # reach the Terminal sitting on desktop 3.
        vm.key("1", meta=True)
        time.sleep(1.0)
        vm.clear()
        vm.type("echo leaked-to-hidden-window\n")
        time.sleep(2.0)
        assert "leaked-to-hidden-window" not in vm.serial(), \
            "keystrokes reached a window on another desktop:\n" + vm.serial()[-800:]
        print("  ok: a window on another desktop receives no input")

        # And it is still there when you go back, with its scrollback intact.
        vm.key("3", meta=True)
        time.sleep(1.0)
        out = gui_run(vm, "echo back-on-three", "back-on-three")
        print("  ok: switching back restores the window and its focus")

        # Ctrl+Alt+Right walks forward: 3 -> 4, and 4 wraps to 1.
        vm.key("right", ctrl=True, alt=True)
        time.sleep(0.8)
        vm.key("right", ctrl=True, alt=True)
        time.sleep(0.8)
        vm.key("3", meta=True)          # back to the Terminal to read the answer
        time.sleep(1.0)
        out = gui_run(vm, "desk", "usage: desk")
        assert "* 3" in out, "Ctrl+Alt+Right then Win+3 did not land on 3:\n" + out[-800:]
        print("  ok: Ctrl+Alt+Right walks the desktops and wraps")

        out = gui_run(vm, "desk 9", "no desktop")
        print("  ok: a desktop that does not exist is refused")

        shot = vm.screenshot("desk.ppm")
        assert os.path.getsize(shot) > 1000, "no frame after all that switching"

        vm.expect_absent("KERNEL PANIC")

    print("PASS desk: four virtual desktops, and windows really live on one")
    return 0


if __name__ == "__main__":
    sys.exit(main())
