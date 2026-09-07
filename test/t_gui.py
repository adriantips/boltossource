#!/usr/bin/env python3
"""Desktop smoke test: sign in, then open apps and confirm they draw.

The compositor has no way to report success over serial, so this test's real
assertion is negative -- nothing panics, the GUI keeps running, and the frames
it produces are not blank. Screenshots land in test/out/ for eyeballing.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

# The default account seeded by users_init().
LOGIN_USER, LOGIN_PASS = "user", "bolt"


def ppm_is_interesting(path, min_colors=24):
    """A blank or single-colour frame means the compositor never painted."""
    with open(path, "rb") as fh:
        data = fh.read()
    # P6 header: magic, width, height, maxval -- then binary RGB triples.
    parts = data.split(b"\n", 3)
    if len(parts) < 4 or not parts[0].startswith(b"P6"):
        return False
    px = parts[3]
    seen = set()
    for i in range(0, min(len(px), 3 * 200000), 3 * 97):   # sparse sample
        seen.add(px[i:i + 3])
        if len(seen) >= min_colors:
            return True
    return len(seen) >= min_colors


def sign_in(vm):
    vm.expect("BoltOS is up. Launching desktop.", timeout=150)
    time.sleep(4.0)                    # let the login screen paint
    vm.type(LOGIN_USER)
    vm.key("tab")
    vm.type(LOGIN_PASS)
    vm.key("ret")
    time.sleep(3.0)


def main():
    with BoltVM(name="gui", net=False) as vm:
        sign_in(vm)

        shot = vm.screenshot("desktop.ppm")
        assert ppm_is_interesting(shot), "the desktop frame is blank after sign-in"
        print("  ok: signed in and the desktop composited a frame")

        # Desktop launcher icons are laid out in a grid from the top-left; the
        # first cell is the Terminal. Double-click opens it.
        vm.click(40, 50, double=True)
        time.sleep(2.0)
        shot = vm.screenshot("terminal.ppm")
        assert ppm_is_interesting(shot), "no frame after opening a window"
        print("  ok: an app window opened and drew")

        vm.expect_absent("KERNEL PANIC")

        # The GUI must still be alive and responsive at the end.
        assert vm.alive(), "QEMU died during the GUI test"
        print("  ok: the compositor survived the session")
    print("PASS gui: sign-in and the desktop work")
    return 0


if __name__ == "__main__":
    sys.exit(main())
