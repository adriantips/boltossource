#!/usr/bin/env python3
"""UEFI boot: BOOTX64.EFI finds the kernel on the ESP and reaches the desktop.

Skips itself (exit 0) when no OVMF firmware is installed, so the suite still
runs usefully on a machine with only a legacy-BIOS QEMU.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, find_ovmf


def main():
    if not find_ovmf()[0]:
        print("SKIP uefi: no OVMF firmware found")
        return 0

    with BoltVM(name="uefi", uefi=True) as vm:
        vm.expect("BoltOS kernel", timeout=120)
        vm.expect("[ok] boot mode: normal", timeout=60)
        vm.expect("[ok] VMM online", timeout=60)
        vm.expect("[ok] preemptive scheduler", timeout=90)
        vm.expect("BoltOS is up. Launching desktop.", timeout=90)
        vm.expect_absent("KERNEL PANIC")
    print("PASS uefi: BOOTX64.EFI booted the kernel to the desktop")
    return 0


if __name__ == "__main__":
    sys.exit(main())
