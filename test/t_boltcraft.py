#!/usr/bin/env python3
"""boltcraft: that it is on the machine at all, and that it starts.

The game was written, it worked, and nobody could play it. Three separate
things saw to that, and this test is one assertion for each of them:

  * `sdk/build.sh` did not link kernel/font8x8.c, so the build the program's
    own README tells you to run failed on an undefined `font8x8_basic`. The
    PowerShell half of the SDK had the file and the bash half did not.
  * Nothing in build.sh compiled boltcraft into the image and nothing seeded
    /bin/boltcraft, so the only way onto a machine was to serve it over HTTP
    from another one and type a `download` line.
  * Once both of those were fixed it still had no way in from the desktop --
    and the desktop icon array was one slot short of holding it.

So: build it the documented way, boot a machine that has never seen a network,
and check the binary is there, the launcher is there, and `run boltcraft` gets
a ring-3 process onto the panel.

    python test/t_boltcraft.py --host-only   # just the SDK build, ~10 seconds
"""
import os, shutil, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT

OUT = os.path.join(ROOT, "test", "out")
SRC = ["main.c", "world.c", "tex.c", "render.c", "hud.c"]


def build_with_sdk():
    """The bash SDK has to link it, exactly as boltcraft/README.md says."""
    os.makedirs(OUT, exist_ok=True)
    out = os.path.join(OUT, "boltcraft-sdk.elf")
    bash = shutil.which("bash") or r"C:\msys64\usr\bin\bash.exe"
    env = dict(os.environ)
    if os.name == "nt":
        env["PATH"] = r"C:\msys64\usr\bin;C:\msys64\ucrt64\bin;" + env.get("PATH", "")
    args = [bash, os.path.join(ROOT, "sdk", "build.sh"), "-o", out]
    args += [os.path.join("boltcraft", s) for s in SRC]
    r = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        raise AssertionError("sdk/build.sh could not link boltcraft:\n"
                             + r.stdout[-2000:] + r.stderr[-2000:])
    with open(out, "rb") as fh:
        assert fh.read(4) == b"\x7fELF", "sdk/build.sh did not produce an ELF"
    size = os.path.getsize(out)
    assert size > 40000, "boltcraft came out %d bytes, which is not all of it" % size
    print("  ok: sdk/build.sh linked boltcraft (%d bytes)" % size)


def guest_tests():
    # net=False on purpose: the whole point is that nothing has to be fetched.
    with BoltVM("boltcraft", net=False) as vm:
        vm.reach_shell(180, "2")

        out = vm.run("ls /bin")
        assert "boltcraft" in out, "/bin/boltcraft was not seeded:\n" + out
        print("  ok: /bin/boltcraft is on a machine that has never seen a network")

        # It grabs the panel and runs until escape. In safe mode there is no
        # framebuffer, so SYS_FBGRAB fails and it exits saying so -- which is
        # still the ELF loading and running, which is what is being checked.
        out = vm.run("run boltcraft -seed 1234", timeout=40)
        for bad in ("no such file", "not found", "cannot exec", "bad elf"):
            assert bad not in out.lower(), \
                "`run boltcraft` did not start it:\n" + out

        log = vm.serial()
        for bad in ("PANIC", "page fault", "general protection"):
            assert bad.lower() not in log.lower(), \
                "boltcraft put '%s' in the log:\n%s" % (bad, log[-2000:])
        print("  ok: `run boltcraft` loaded and ran a ring-3 process")


def changed_fraction(before, after, box):
    """How much of `box` differs between two screendumps, 0..1."""
    from PIL import Image
    a = Image.open(before).convert("RGB").crop(box).tobytes()
    b = Image.open(after).convert("RGB").crop(box).tobytes()
    if len(a) != len(b):
        raise AssertionError("the two screendumps are different sizes")
    return sum(1 for p, q in zip(a, b) if p != q) / float(len(a))


def desktop_test():
    """And that there is a way in for somebody who never opens a terminal."""
    with BoltVM("boltcraft-gui", net=False) as vm:
        vm.sign_in("user", "bolt")
        time.sleep(2)
        before = vm.screenshot("boltcraft-desktop")

        vm.open_app("boltcraft", probe=False)
        time.sleep(2)
        after = vm.screenshot("boltcraft-launcher")

        # A taskbar button and no window is exactly what this looked like when
        # the window was being placed at x=2192 on a 1024-wide screen, and
        # "nothing in the log" was perfectly happy with it. So look at the
        # pixels: the window is 470x400 at 150,90 and must have covered them.
        frac = changed_fraction(before, after, (150, 90, 150 + 470, 90 + 400))
        assert frac > 0.5, \
            "the launcher window never painted -- only %.0f%% of where it " \
            "should be changed" % (frac * 100)

        log = vm.serial()
        for bad in ("PANIC", "page fault", "kmalloc failed"):
            assert bad.lower() not in log.lower(), \
                "the launcher put '%s' in the log:\n%s" % (bad, log[-2000:])
        print("  ok: the desktop launcher opens and paints (%.0f%% of its rect)"
              % (frac * 100))


def main():
    print("boltcraft: sdk build")
    build_with_sdk()
    if "--host-only" in sys.argv:
        print("t_boltcraft: ok (host only)")
        return
    print("boltcraft: on the machine")
    guest_tests()
    print("boltcraft: on the desktop")
    desktop_test()
    print("t_boltcraft: ok")


if __name__ == "__main__":
    main()
