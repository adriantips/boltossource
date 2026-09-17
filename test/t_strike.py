#!/usr/bin/env python3
"""BoltStrike: the rules on the host, then the app on a real desktop.

The host half compiles strike/st_map.c and strike/st_game.c with nothing under
them but malloc and plays several hundred rounds of bots against each other in
about a second. That is where the real assertions are -- every round ends by
itself, no bot ever finishes a tick inside a wall, both teams win sometimes,
line of sight is symmetric, the bomb can be planted and defused and explodes
when it is not. None of it needs a framebuffer, so none of it waits for one.

The QEMU half only checks the thing the host cannot: that the app opens on the
desktop, allocates its textures, and paints a frame without taking the machine
down with it.

    python test/t_strike.py --host-only     # no QEMU, ~2 seconds
"""
import os
import shutil
import time
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT

OUT = Path(ROOT) / "test" / "out"


def host_tests():
    cc = os.environ.get("BOLTOS_HOST_CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc and Path(r"C:\msys64\ucrt64\bin\clang.exe").exists():
        cc = r"C:\msys64\ucrt64\bin\clang.exe"
    if not cc:
        raise RuntimeError("host C compiler missing (set BOLTOS_HOST_CC)")
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / ("strike-host.exe" if os.name == "nt" else "strike-host")
    # include/math.h and include/stdlib.h redefine things the host's own
    # headers already have; the kernel build never sees both.
    subprocess.run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                    "-Wno-inconsistent-dllimport", "-Wno-macro-redefined",
                    "-iquote", str(Path(ROOT) / "include"),
                    str(Path(ROOT) / "test" / "host" / "strike.c"), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)


def gui_test():
    with BoltVM("strike", net=False) as vm:
        vm.sign_in("user", "bolt")
        # probe=False: the probe types a marker at a shell prompt, which only
        # the Terminal has.
        vm.open_app("BoltStrike", probe=False)

        # The app paints a "Loading" notice, then blocks the desktop loop
        # while it generates every texture and sprite. Give it that, then a
        # few frames of the game actually running.
        time.sleep(4)
        vm.screenshot("strike-freeze")

        # Freeze time is seven seconds; wait it out so the shot below is
        # of a live round with bots on their way to a site.
        time.sleep(9)
        for _ in range(6):
            vm.nudge(8, 0)
            time.sleep(0.5)
        vm.screenshot("strike")

        log = vm.serial()
        for bad in ("PANIC", "page fault", "kmalloc failed", "could not allocate"):
            assert bad.lower() not in log.lower(), \
                "BoltStrike put '%s' in the log:\n%s" % (bad, log[-2000:])
        print("  desktop: BoltStrike opened and painted")


def main():
    host_only = "--host-only" in sys.argv
    host_tests()
    if not host_only:
        gui_test()
    print("t_strike: ok")


if __name__ == "__main__":
    main()
