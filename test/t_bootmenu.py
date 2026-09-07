#!/usr/bin/env python3
"""Boot menu: the default entry, and safe mode picked by pressing 2.

The menu itself renders on the BIOS/firmware console, not on COM1, so what is
asserted here is the *effect* of the choice: the kernel prints its boot mode and
safe mode leaves the network, audio and desktop down.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM


def test_default():
    with BoltVM(name="menu-default") as vm:
        vm.expect("[ok] boot mode: normal", timeout=90)
        vm.expect("BoltOS is up. Launching desktop.", timeout=90)
        vm.expect_absent("KERNEL PANIC")
    print("PASS bootmenu-default: timeout selects the normal entry")


def test_safe_mode():
    with BoltVM(name="menu-safe") as vm:
        # stage2 shows the menu right after A20; press 2 during the countdown.
        # The keystrokes are sent repeatedly because the exact moment the menu
        # starts polling depends on how fast the firmware hands over.
        for _ in range(12):
            if "[ok] boot mode" in vm.serial():
                break
            vm.key("2")
            time.sleep(0.4)

        vm.expect("[ok] boot mode: safe", timeout=90)
        vm.expect("safe mode: skipping network + audio bring-up", timeout=90)
        vm.expect("BoltOS is up (safe mode). Starting BoltShell.", timeout=90)

        # The desktop must NOT have been started.
        vm.expect_absent("Launching desktop")
        vm.expect_absent("KERNEL PANIC")

        # The text shell is live and usable -- that is the whole point of the
        # mode, so prove a command actually runs.
        vm.type("version\n")
        vm.expect("BoltOS", timeout=30)
    print("PASS bootmenu-safe: '2' boots to a working shell with net/GUI down")


def main():
    test_default()
    test_safe_mode()
    return 0


if __name__ == "__main__":
    sys.exit(main())
