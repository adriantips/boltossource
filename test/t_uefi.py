#!/usr/bin/env python3
"""UEFI boot: BOOTX64.EFI finds the kernel on the ESP and reaches the desktop.

Skips itself (exit 0) when no OVMF firmware is installed, so the suite still
runs usefully on a machine with only a legacy-BIOS QEMU.

The memory check at the end is not decoration. Reaching the desktop says very
little about the memory map the loader handed over: with the E820 entries
misaligned the kernel booted anyway and ran on 62 free frames, and every
assertion above passed while it did.

The device check is not decoration either, and it is the one this file was
missing. UEFI assigns 64-bit BARs out of a window far above 4 GiB -- OVMF uses
768 GiB -- and the kernel direct-maps only the low 4 GiB, so pci_map_bar() used
to refuse every one of them and return. Nothing reported that: the machine
booted, reached the desktop, passed every assertion in this file, and had no
USB controller, no NVMe and no keyboard. A boot test that only asks whether the
desktop appeared cannot see a machine with no devices in it.

The timeouts are large because this path is genuinely slow, not because it is
flaky. Measured 2026-09-14 on the same image and the same device set: 13s to
the desktop over legacy BIOS, 162s over OVMF, with 148 of those 148-odd extra
seconds spent writing five binaries into ramfs. Nothing in this tree programs
MTRRs or PAT, so the kernel runs on whatever memory type the firmware left
behind -- SeaBIOS leaves it write-back and OVMF does not. Worth fixing; until
it is, a 90s ceiling here just reports the clock.
"""
import os, re, sys
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
        # No marker between here and the desktop is safe to wait on: once the
        # scheduler starts, /bin/hello prints from ring 3 while the kernel is
        # still printing, and nothing serialises COM1, so a line gets cut in
        # half ("[ok] preemptihello from ring3 (pid=1)"). Wait for the point
        # where ring 3 has exited and the kernel is the only writer again.
        vm.expect("BoltOS is up. Launching desktop.", timeout=300)
        vm.expect_absent("KERNEL PANIC")
        log = vm._buf

    m = re.search(r"\[pmm\] (\d+) frames managed \((\d+) MiB\), "
                  r"(\d+) free \((\d+) MiB\)", log)
    assert m, "no [pmm] line on a UEFI boot:\n" + log[-1500:]
    managed, free = int(m.group(2)), int(m.group(4))
    assert free > managed // 2, (
        "booted with %d MiB free of %d MiB managed -- the E820 map built in "
        "boot/uefi_boot.c is being misread. Check that struct e820 there is "
        "still the same 24 bytes wide as the one in mm/pmm.c." % (free, managed))
    # Every BAR this firmware put above the direct map has to have been mapped
    # into the MMIO window instead of refused. On OVMF there is always at least
    # one, so an empty list here means the window stopped being exercised and
    # this assertion has quietly stopped testing anything.
    high = re.findall(r"\[pci\] BAR (0x[0-9a-f]+) is above the direct map; "
                      r"mapped at (0x[0-9a-f]+)", log)
    assert high, ("no BAR was mapped above the direct map. OVMF puts its "
                  "64-bit BARs at 768 GiB, so either the firmware changed or "
                  "pci_map_bar() is refusing them again -- which costs this "
                  "machine its USB, its NVMe and its keyboard:\n" + log[-2000:])
    for bad in ("outside the low 4 GiB", "could not map MMIO"):
        assert bad not in log, "a BAR was refused: %r\n%s" % (bad, log[-2000:])
    print("  ok: %d BAR%s above 4 GiB mapped into the MMIO window (%s)"
          % (len(high), "" if len(high) == 1 else "s",
             ", ".join(a for a, _ in high)))

    # Whether a USB controller then works on this path is t_usbhid.py's UEFI
    # pass; this machine has no USB controller in its device set at all.

    print("PASS uefi: BOOTX64.EFI booted the kernel to the desktop "
          "(%d MiB free of %d)" % (free, managed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
