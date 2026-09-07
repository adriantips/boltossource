#!/usr/bin/env python3
"""Boot smoke test: the legacy BIOS chain reaches the desktop with the core
subsystems online, and the shell answers.

This is the test everything else depends on -- if it fails, no other result
means anything.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM


def main():
    with BoltVM(name="boot") as vm:
        vm.expect("BoltOS kernel", timeout=60)

        # Core bring-up, in the order kmain reports it.
        for marker in ("[ok] GDT + TSS loaded",
                       "[ok] IDT (256 vectors)",
                       "[ok] VMM online",
                       "[ok] kernel heap online",
                       "[ok] PIT @ 1000 Hz",
                       "[ok] interrupts enabled",
                       "[ok] ramfs mounted",
                       "[ok] VFS",
                       "[ok] user accounts",
                       "[ok] preemptive scheduler"):
            vm.expect(marker, timeout=90)

        # The ring-3 demo really ran through the ELF loader + syscalls.
        vm.expect("[exec] /bin/hello", timeout=60)

        # And the desktop came up rather than falling back to the text shell.
        vm.expect("BoltOS is up. Launching desktop.", timeout=60)

        # A panic anywhere in boot invalidates the run.
        vm.expect_absent("KERNEL PANIC")
        print("PASS boot: kernel reached the desktop with no panic")
    return 0


if __name__ == "__main__":
    sys.exit(main())
