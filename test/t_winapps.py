#!/usr/bin/env python3
"""The five accessory apps: Device Manager, Character Map, Snipping Tool,
Solitaire and Clipboard history.

A window says nothing on serial, so "it drew" can only ever be asserted as
"the frame is not one flat colour". Three of the five leave evidence somewhere
a shell can reach, though, and those are the parts actually worth testing:

  * the clipboard ring, through `clip history` -- newest first, a repeated copy
    moved rather than duplicated, and a pinned entry that outlives Clear;
  * the Snipping Tool, through the PNG it writes into ~/Pictures and the
    notification it posts -- that is a real fb_capture and a real encode;
  * Character Map, through the single byte it leaves on the clipboard.

Those run first, while the Terminal is the only other window. Everything else
is a smoke test with a screenshot in test/out/, plus `desk` to confirm all five
windows really opened -- a stack of five windows and a stack of four look the
same in a frame.
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

LOGIN_USER, LOGIN_PASS = "user", "bolt"

APPS = [
    ("Device Manager", "devmgr.ppm"),
    ("Character Map",  "charmap.ppm"),
    ("Snipping Tool",  "snip.ppm"),
    ("Solitaire",      "solitaire.ppm"),
    ("Clipboard",      "clipboard.ppm"),
]


def frame_colors(path, sample_every=97):
    """How many distinct colours are in the frame. A window that opened but
    never painted gives back the desktop's handful; a real one gives dozens."""
    with open(path, "rb") as fh:
        data = fh.read()
    parts = data.split(b"\n", 3)
    if len(parts) < 4 or not parts[0].startswith(b"P6"):
        return 0
    px = parts[3]
    seen = set()
    for i in range(0, len(px) - 3, 3 * sample_every):
        seen.add(px[i:i + 3])
    return len(seen)


def green_fraction(path, sample_every=31):
    """Solitaire's baize is the only large green area anything here draws, so
    this is how the test tells 'Solitaire opened' from 'a window opened'."""
    with open(path, "rb") as fh:
        data = fh.read()
    parts = data.split(b"\n", 3)
    if len(parts) < 4:
        return 0.0
    px, green, total = parts[3], 0, 0
    for i in range(0, len(px) - 3, 3 * sample_every):
        r, g, b = px[i], px[i + 1], px[i + 2]
        total += 1
        if g > 70 and g > r + 30 and g > b + 30:
            green += 1
    return green / total if total else 0.0


def list_missing(vm):
    """Which of the five the compositor does not have open, per `desk`.

    Call it with a Terminal focused; it leaves the Terminal focused.
    """
    vm.clear()
    vm.type("desk\n")
    vm.expect("window", timeout=30)
    time.sleep(0.6)
    listing = vm.serial()
    return [t for t, _ in APPS if t not in listing]


def history_tests(vm):
    """The clipboard ring, driven through `clip` in the GUI Terminal.

    Everything here is a property of kernel/clipboard.c, not of the window:
    newest first, no duplicate for a repeated copy, and a pinned entry that
    survives a clear. These are the parts that can actually regress silently.
    """
    def sh(line, expect, timeout=30):
        vm.clear()
        vm.type(line + "\n")
        vm.expect(expect, timeout=timeout)
        return vm.serial()

    vm.open_app("Terminal")

    sh("clip history clear", "cleared")
    sh("clip set alpha-one", "copied")
    sh("clip set beta-two", "copied")
    sh("clip set gamma-three", "copied")

    out = sh("clip history", "clipboard history:")
    for word in ("alpha-one", "beta-two", "gamma-three"):
        assert word in out, "%r never reached the history:\n%s" % (word, out[-800:])
    # Newest first: gamma has to be listed before alpha.
    assert out.index("gamma-three") < out.index("alpha-one"), \
        "the history is not newest-first:\n%s" % out[-800:]
    print("  ok: three copies are in the ring, newest first")

    # A repeat moves the entry to the front instead of adding a fourth.
    sh("clip set alpha-one", "copied")
    out = sh("clip history", "clipboard history:")
    assert out.count("alpha-one") == 1, "a repeated copy duplicated the entry:\n%s" % out[-800:]
    assert out.index("alpha-one") < out.index("gamma-three"), \
        "a repeated copy did not come to the front:\n%s" % out[-800:]
    print("  ok: copying the same text twice moves it rather than duplicating it")

    # Restore reaches back past the newest entry.
    sh("clip restore 2", "clipboard:")
    out = sh("clip show", "clipboard:")
    assert "beta-two" in out, "restore did not put the entry back:\n%s" % out[-600:]
    print("  ok: an older entry can be put back on the clipboard")

    # A pin survives the clear that takes everything else.
    sh("clip set keep-me", "copied")
    sh("clip pin 0", "pinned")
    sh("clip history clear", "cleared")
    out = sh("clip history", "clipboard history:")
    assert "keep-me" in out, "the pinned entry was cleared:\n%s" % out[-600:]
    assert "gamma-three" not in out, "clear left an unpinned entry behind:\n%s" % out[-600:]
    print("  ok: pinned entries survive Clear and unpinned ones do not")

    vm.key("esc")
    time.sleep(0.3)


def snip_tests(vm):
    """The Snipping Tool, end to end, driven by its keyboard shortcuts so the
    test needs no window coordinates: Enter captures the panel and Ctrl+S
    writes the crop into ~/Pictures. Both halves are real work -- fb_capture
    off VRAM, then the PNG encoder -- and neither says anything on serial, so
    the file is the only evidence.
    """
    for _ in range(3):
        vm.open_app("Snipping Tool", probe=False)
        time.sleep(1.2)
        vm.key("ret")                   # capture
        time.sleep(2.5)                 # reading VRAM back is not instant
        vm.key("s", ctrl=True)          # save
        time.sleep(3.0)                 # 1024x768 through the PNG encoder
        vm.open_app("Terminal")
        vm.clear()
        vm.type("ls /home/user/Pictures\n")
        time.sleep(1.5)
        if "snip-1.png" in vm.serial():
            break
    vm.expect("snip-1.png", timeout=30)

    # A file of the right name and no bytes would pass the line above, and that
    # is exactly what a failed capture or a failed encode looks like. A flat
    # 1024x768 desktop encodes to tens of kilobytes: far below the 2.3 MB raw,
    # far above nothing.
    vm.clear()
    vm.type("meta /home/user/Pictures/snip-1.png\n")
    vm.expect("size :", timeout=30)
    time.sleep(0.4)
    m = re.search(r"size\s*:\s*(\d+) bytes", vm.serial())
    assert m, "meta printed no size:\n%s" % vm.serial()[-800:]
    size = int(m.group(1))
    assert 2000 < size < 900000, "implausible snip: %d bytes" % size
    print("  ok: the Snipping Tool captured the panel and wrote a %d-byte PNG" % size)

    vm.clear()
    vm.type("notify list -a\n")
    vm.expect("Snipping Tool", timeout=30)
    print("  ok: and said where it went")


def charmap_tests(vm):
    """Enter adds the selected glyph to the string being built, Ctrl+C copies
    it. The window opens on the degree sign, so one byte of text on the
    clipboard is the expected result -- and it has to be one byte, because a
    Character Map that copied the *name* of a character would be useless.
    """
    for _ in range(3):
        vm.open_app("Character Map", probe=False)
        time.sleep(1.2)
        vm.key("ret")
        time.sleep(0.5)
        vm.key("c", ctrl=True)
        time.sleep(0.7)
        vm.open_app("Terminal")
        vm.clear()
        vm.type("clip show\n")
        time.sleep(1.5)
        if "clipboard: text, 1 bytes" in vm.serial():
            break
    vm.expect("clipboard: text, 1 bytes", timeout=30)
    print("  ok: Character Map put the picked glyph on the clipboard")


def main():
    with BoltVM(name="winapps", net=False) as vm:
        vm.sign_in(LOGIN_USER, LOGIN_PASS)
        print("  ok: signed in")

        history_tests(vm)

        # The two apps that do something a shell can see run first, while the
        # only other window is the Terminal. That is not tidiness: open_app
        # types a name into a Start menu it cannot see, and the menu takes
        # longer to come up the more windows the compositor has to paint. With
        # seven windows on screen it reliably missed, the keystrokes went to
        # whatever had focus, and Ctrl+C in the Terminal *clears* the clipboard
        # with nothing selected -- so the failure looked like Character Map
        # emptying the clipboard rather than like a test that missed.
        snip_tests(vm)
        charmap_tests(vm)

        base = vm.screenshot("winapps-desktop.ppm")
        base_colors = frame_colors(base)
        assert base_colors >= 8, "the desktop never painted (%d colours)" % base_colors

        for title, shot_name in APPS:
            vm.open_app(title, probe=False)
            time.sleep(1.2)
            shot = vm.screenshot(shot_name)
            n = frame_colors(shot)
            assert n >= 12, "%s: the frame has only %d colours" % (title, n)
            print("  ok: %s opened and painted (%d colours)" % (title, n))
            vm.key("esc")
            time.sleep(0.3)

        # The real assertion, and the only one that can tell "five windows
        # opened" from "four opened and one name was typed at whatever had
        # focus" -- a screenshot of a stack of windows looks identical either
        # way. `desk` asks the compositor what it actually has. Expect a retry
        # or two here for the reason given above.
        vm.open_app("Terminal")
        missing = list_missing(vm)
        for _ in range(4):
            if not missing:
                break
            print("  .. retrying: %s" % ", ".join(missing))
            for title in missing:
                vm.open_app(title, probe=False)
                time.sleep(1.0)
            vm.open_app("Terminal")
            missing = list_missing(vm)
        assert not missing, "never opened: %s\n%s" % (", ".join(missing), vm.serial()[-1200:])
        print("  ok: all five are open on the desktop")

        # Solitaire's baize is the one large flat colour any of these draw, so
        # it is the only one whose frame can be checked for more than "not
        # blank".
        gf = 0.0
        for _ in range(3):
            vm.open_app("Solitaire", probe=False)
            time.sleep(1.4)
            shot = vm.screenshot("solitaire.ppm")
            gf = green_fraction(shot)
            if gf > 0.10:
                break
        assert gf > 0.10, "Solitaire's table is not on screen (green %.3f)" % gf
        print("  ok: the card table is drawn (green %.1f%% of the frame)" % (gf * 100))

        # Its own keys, now that it is certainly in front: three deals, draw-3,
        # and a turn of the stock. Nothing here can be read back from a shell,
        # so the assertion is only that it kept painting -- but an out-of-range
        # index in the deal or the turn would fault the kernel, and that is
        # what this is really watching for.
        for _ in range(3):
            vm.type("n")
            time.sleep(0.4)
        vm.type("d")            # draw-3
        time.sleep(0.6)
        vm.key("spc")           # turn the stock over
        time.sleep(1.5)         # a busy desktop under TCG is ~10 fps
        shot = vm.screenshot("solitaire-dealt.ppm")
        assert frame_colors(shot) >= 12, "Solitaire stopped painting after a new deal"
        print("  ok: Solitaire redeals and turns the stock")

        vm.expect_absent("KERNEL PANIC")
        assert vm.alive(), "QEMU died during the accessory-app test"
        print("  ok: the compositor survived all five")

    print("PASS winapps: the accessory apps open and draw")
    return 0


if __name__ == "__main__":
    sys.exit(main())
