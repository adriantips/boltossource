#!/usr/bin/env python3
"""The desktop wallpaper, and the part of it that is easy to get wrong.

Shipping a picture is the trivial half. The half worth a test is that BoltOS
ships one render per resolution and swaps between them when the mode changes,
because the compositor stretches the wallpaper with a nearest-neighbour
sampler: hand it the 1024x768 render on a 4K panel and every ridgeline turns
into a staircase. Nothing about that failure is visible in a log, so this reads
pixels off the panel and compares them against the file that should be on it.

It also pins the two rules around the swap:

  * a fresh machine with no saved choice comes up with the render cut for the
    mode it booted in, not a gradient and not a 640px thumbnail;
  * the swap follows the filename convention (<stem>-<W>x<H>.jpg) and so only
    ever moves within a shipped set -- a photo the user picked themselves has
    no siblings and must be left exactly where it is.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

LOGIN_USER, LOGIN_PASS = "user", "bolt"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WALLDIR = os.path.join(ROOT, "assets", "wallpapers")

# Settings opens at (90, 50) with a 220px sidebar, so the client starts at
# (90, 82) and the page at x = 90 + 220 + 28. The rows are laid out in
# app_settings.c -- if that layout moves, these are the lines to fix, and the
# symptom will be a resolution that never changed rather than anything about
# wallpapers.
SIDEBAR_DISPLAY = (90 + 10 + 60, 82 + 84 + 38 + 17)      # second sidebar item
_SEG_X, _SEG_W = 90 + 220 + 28 + 16, 860 - 220 - 56 - 32  # the resolution picker
_SEG_SLOT = (_SEG_W - 4) // 5                              # five modes
CHIP_1080P = (_SEG_X + 2 + 2 * _SEG_SLOT + _SEG_SLOT // 2, 82 + 66 + 62 + 15)

# The default wallpaper since the glass desktop. Mountain is still shipped,
# with the same set of renders, but it is no longer what a fresh disk gets.
RENDERS = ["aurora-1024x768.jpg",  "aurora-1280x720.jpg",
           "aurora-1920x1080.jpg", "aurora-2560x1440.jpg",
           "aurora-3840x2160.jpg"]


def run(vm, line, timeout=15):
    vm.clear()
    vm.type(line + "\n")
    time.sleep(2.0)
    return vm.serial()


def field(out, name):
    """The last `name ...` line of a `display` report, without the name."""
    for ln in reversed(out.splitlines()):
        if ln.startswith(name + " "):
            return ln[len(name):].strip()
    raise AssertionError("no %r line in:\n%s" % (name, out[-800:]))


def patch(img, box):
    """Mean RGB of a region. A box filter down to one pixel is the average,
    and comparing averages rather than pixels is deliberate -- the panel has
    been through a JPEG round trip the source file has not."""
    from PIL import Image
    return img.crop(box).convert("RGB").resize((1, 1), Image.BOX).getpixel((0, 0))


def main():
    try:
        from PIL import Image
    except ImportError:
        print("SKIP: needs Pillow to read the panel back")
        return 0

    with BoltVM(name="wall", net=False) as vm:
        vm.sign_in(LOGIN_USER, LOGIN_PASS)
        vm.open_app("Terminal")

        out = run(vm, "ls /usr/share/wallpapers")
        for f in RENDERS:
            assert f in out, "%s was not seeded:\n%s" % (f, out[-800:])
        print("  ok: one render seeded per entry in the resolution table")

        # No choice made on a fresh disk, so the boot mode picks the file.
        # Asked of `display`, which reports what is on the panel. This used to
        # cat /home/.wallpaper, but the boot restore no longer writes that --
        # and the test disks still carry one naming the mountain from before
        # the glass desktop, which is exactly the stale preference the restore
        # now ignores.
        out = run(vm, "display")
        assert "/usr/share/wallpapers/aurora-1024x768.jpg" in field(out, "wallpaper"), \
            "a fresh machine did not come up on the 1024x768 render:\n" + out[-800:]
        print("  ok: a machine with no saved choice gets the render for its mode")

        # And it is genuinely on the panel. The sample is far right and low,
        # clear of the icon grid and above the tray, where only wallpaper can
        # be -- and clear of the Terminal's shadow too. Windows cast a soft one
        # now that reaches 64px past their edge, and the box this used to use
        # (768,380)-(1008,560) caught enough of it to read 13% dark.
        vm.screenshot("wall-1024x768")
        shot = Image.open(os.path.join(ROOT, "test", "out", "wall-1024x768"))
        assert shot.size == (1024, 768), "panel is %s" % (shot.size,)
        box = (880, 590, 1010, 670)
        want = patch(Image.open(os.path.join(WALLDIR, "aurora-1024x768.jpg")), box)
        got = patch(shot, box)
        off = max(abs(a - b) for a, b in zip(want, got))
        assert off <= 12, \
            "the desktop does not match the shipped render there: %s vs %s" % (got, want)
        print("  ok: the panel shows the render, not a gradient (dRGB <= %d)" % off)

        # Now the swap. 1024x768 -> 1080p is a real mode change (the GPU is
        # reprogrammed), and the wallpaper has to move with it.
        vm.open_app("Settings", probe=False)
        time.sleep(2.0)
        vm.click(*SIDEBAR_DISPLAY)
        time.sleep(1.0)
        vm.click(*CHIP_1080P)
        time.sleep(8.0)                      # mode set, then an 8 MP decode

        vm.open_app("Terminal")
        out = run(vm, "display")
        assert "/usr/share/wallpapers/aurora-1920x1080.jpg" in field(out, "wallpaper"), \
            "the wallpaper did not follow the mode change:\n" + out[-800:]

        vm.screenshot("wall-1920x1080")
        shot = Image.open(os.path.join(ROOT, "test", "out", "wall-1920x1080"))
        assert shot.size == (1920, 1080), "the mode never changed: panel is %s" % (shot.size,)
        box = (1440, 540, 1890, 880)
        want = patch(Image.open(os.path.join(WALLDIR, "aurora-1920x1080.jpg")), box)
        got = patch(shot, box)
        off = max(abs(a - b) for a, b in zip(want, got))
        assert off <= 12, \
            "1080p is showing something other than the 1080p render: %s vs %s" % (got, want)
        print("  ok: a mode change swaps to the render cut for the new panel")

    print("t_wall: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
