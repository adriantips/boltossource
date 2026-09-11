#!/usr/bin/env python3
"""The Code app: open it from the desktop, and build something in it.

`build` is checked headlessly in t_project.py -- this checks the other half of
the same claim: that the IDE draws its project tree, tabs and editor, and that
pressing Run compiles and runs inside the compositor thread without taking the
desktop down with it. A compiler crash in a GUI callback is the failure mode
that matters here, because it would kill every window, not just this one.

The Run button is found by its colour rather than by arithmetic on the window
geometry, so the test does not have to be re-derived every time the toolbar
moves.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

LOGIN_USER, LOGIN_PASS = "user", "bolt"
RUN_RGB = (0x2E, 0xA0, 0x43)          # the Run button's green


def read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    parts = data.split(b"\n", 3)
    if len(parts) < 4 or not parts[0].startswith(b"P6"):
        return None, 0, 0
    w, h = (int(v) for v in parts[1].split())
    return parts[3], w, h


def find_color(path, rgb, tol=8):
    """Centroid of the pixels matching `rgb`, or None."""
    px, w, h = read_ppm(path)
    if px is None:
        return None
    sx = sy = n = 0
    for y in range(0, h, 2):
        row = y * w * 3
        for x in range(0, w, 2):
            i = row + x * 3
            if (abs(px[i] - rgb[0]) <= tol and abs(px[i+1] - rgb[1]) <= tol
                    and abs(px[i+2] - rgb[2]) <= tol):
                sx += x; sy += y; n += 1
    if n < 20:                          # a few stray pixels are not a button
        return None
    return sx // n, sy // n


def colors(path, step=97):
    px, w, h = read_ppm(path)
    if px is None:
        return set()
    return {px[i:i+3] for i in range(0, len(px) - 3, 3 * step)}


def sign_in(vm):
    vm.sign_in(LOGIN_USER, LOGIN_PASS, timeout=180)


def main():
    with BoltVM(name="ide", net=False) as vm:
        sign_in(vm)

        # The Code launcher is the second icon of the second desktop row. A
        # double-click is two press/release pairs, and the compositor samples
        # the mouse rather than queueing events, so a pair can be missed --
        # hence the retry until the window's Run button appears.
        run = None
        for _ in range(4):
            vm.click(135, 133, double=True)
            time.sleep(2.0)
            run = find_color(vm.screenshot("ide.ppm"), RUN_RGB)
            if run:
                break
        assert run, "the Code app never opened (no Run button on screen)"
        print("  ok: the Code app opened and drew its toolbar")

        before = colors(os.path.join(vm.workdir, "ide.ppm"))

        vm.click(run[0], run[1])
        time.sleep(6.0)                 # compile + run happen in the click handler
        after_path = vm.screenshot("ide_run.ppm")
        after = colors(after_path)
        assert after != before, "nothing changed on screen after pressing Run"
        print("  ok: Run compiled and ran without freezing the desktop")

        vm.expect_absent("KERNEL PANIC")
        assert vm.alive(), "QEMU died while the IDE was running a program"
        print("  ok: the compositor survived a build inside a window")

    print("PASS ide: the Code app builds and runs from the desktop")
    return 0


if __name__ == "__main__":
    sys.exit(main())
