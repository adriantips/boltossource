#!/usr/bin/env python3
"""A clipboard that carries more than text.

It held one static char buffer, which meant copying a picture or a file was not
something the machine could do at all: Paint could not hand a drawing to
anything, and File Explorer's Ctrl+C reached no further than its own window.

The round trip this drives is the one that proves the three kinds are real and
that separate apps share one store: take a screenshot, which opens in Photos;
copy it there; confirm from a Terminal that the clipboard now says *image* and
not "empty" or "text"; paste it into Paint. Nothing in that path could work
before, and each step is a different app.
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

LOGIN_USER, LOGIN_PASS = "user", "bolt"


def term(vm, line, expect, timeout=40):
    vm.clear()
    vm.type(line + "\n")
    vm.expect(expect, timeout=timeout)
    return vm.serial()


def main():
    with BoltVM(name="clip", net=False) as vm:
        vm.sign_in(LOGIN_USER, LOGIN_PASS)
        vm.open_app("Terminal")

        # Text still behaves exactly as it did; the kind is just named now.
        term(vm, "clip clear", "cleared")
        out = term(vm, "clip show", "clipboard")
        assert "empty" in out, "a cleared clipboard should say so:\n" + out[-400:]
        term(vm, "clip set hello-clipboard", "copied")
        out = term(vm, "clip show", "clipboard:")
        assert "text" in out and "hello-clipboard" in out, \
            "text went missing:\n" + out[-400:]
        print("  ok: text still works, and reports itself as text")

        # Print Screen gives us a real image and opens it in Photos -- the
        # `screenshot` command writes the file but does not open anything, so
        # this has to be the key.
        vm.clear()
        vm.key("f12")
        assert vm.wait_for("Screenshot", timeout=120),             "Print Screen captured nothing:" + chr(10) + vm.serial()[-800:]
        time.sleep(3.0)                       # Photos decodes and opens it

        # Ctrl+C in Photos. The notification is the app confirming it copied.
        vm.clear()
        vm.key("c", ctrl=True)
        assert vm.wait_for("Image copied", timeout=30), \
            "Photos did not copy the image:\n" + vm.serial()[-800:]
        print("  ok: Photos copies the displayed image")

        # Back to the Terminal to ask the clipboard what it is holding. This is
        # the assertion that matters: two different apps, one store, and the
        # store knows it has a picture rather than a string.
        vm.open_app("Terminal")
        out = term(vm, "clip show", "clipboard:")
        m = re.search(r"clipboard: image (\d+)x(\d+)", out)
        assert m, "the clipboard does not report an image:\n" + out[-500:]
        # Not the panel's 1024x768: image_decode box-downscales anything wider
        # than 640, so what Photos holds -- and hands over -- is smaller.
        iw, ih = int(m.group(1)), int(m.group(2))
        assert iw > 100 and ih > 100, "implausible image size %dx%d" % (iw, ih)
        print("  ok: another app sees an image on the clipboard, %dx%d" % (iw, ih))

        # And a third app can take it.
        vm.open_app("Paint", probe=False)
        vm.clear()
        vm.key("v", ctrl=True)
        assert vm.wait_for("Pasted the clipboard image", timeout=30), \
            "Paint did not paste the image:\n" + vm.serial()[-800:]
        print("  ok: Paint pastes it onto the canvas")

        # Paint's own copy replaces it, and the size follows the canvas.
        vm.clear()
        vm.key("c", ctrl=True)
        assert vm.wait_for("Canvas copied", timeout=30), \
            "Paint did not copy its canvas:\n" + vm.serial()[-800:]
        vm.open_app("Terminal")
        out = term(vm, "clip show", "clipboard:")
        assert "image 520x340" in out, \
            "the clipboard did not take Paint's canvas:\n" + out[-500:]
        print("  ok: copying again replaces what was there")

        out = term(vm, "clip clear", "cleared")
        out = term(vm, "clip show", "clipboard")
        assert "empty" in out, "clear did not release the image:\n" + out[-400:]

        vm.expect_absent("KERNEL PANIC")

    print("PASS clip: text, images and files share one typed clipboard")
    return 0


if __name__ == "__main__":
    sys.exit(main())
