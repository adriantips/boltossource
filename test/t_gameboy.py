#!/usr/bin/env python3
"""The Game Boy core on the host, then the app on the desktop.

The host half is where the real assertions are, and several of them are
checked against something other than the emulator itself: DAA is re-derived
from the hardware description and diffed against the core over all 4096
inputs, the flag tests assert results worked out by hand from the instruction
set, and the PPU tests build a tile by hand and assert the pixels that have to
come out of it. It runs in about a second.

The QEMU half is a smoke test and nothing more: it opens the app and checks
the machine survives it. It cannot check emulation, because there is no ROM on
the guest to run -- nothing is shipped with the emulator and getting a binary
cartridge into the image from here would mean either a network fetch or
baking one into the build. If you want to check a real game, put a .gb in
/home/user/roms on a machine and open the app.

    python test/t_gameboy.py --host-only     # no QEMU, ~2 seconds
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
    exe = OUT / ("gb-host.exe" if os.name == "nt" else "gb-host")
    subprocess.run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                    "-Wno-inconsistent-dllimport", "-Wno-macro-redefined",
                    "-iquote", str(Path(ROOT) / "include"),
                    str(Path(ROOT) / "test" / "host" / "gameboy.c"), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)


def gui_test():
    with BoltVM("gameboy", net=False) as vm:
        vm.sign_in("user", "bolt")
        vm.open_app("Game Boy", probe=False)
        time.sleep(2)
        vm.screenshot("gameboy")
        log = vm.serial()
        for bad in ("PANIC", "page fault", "kmalloc failed"):
            assert bad.lower() not in log.lower(), \
                "the Game Boy app put '%s' in the log:\n%s" % (bad, log[-2000:])
        print("  desktop: the Game Boy app opened")


def main():
    host_tests()
    if "--host-only" not in sys.argv:
        gui_test()
    print("t_gameboy: ok")


if __name__ == "__main__":
    main()
