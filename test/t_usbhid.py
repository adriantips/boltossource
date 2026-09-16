#!/usr/bin/env python3
"""A USB keyboard and a USB mouse, which on a modern machine is the only kind.

The machine this was written for has no 8042 at all. Its keyboard and its mouse
are USB devices on the root ports of an Intel xHCI controller, and until commit
b9184eb the firmware's legacy emulation was quietly making them look like PS/2
ones. Taking the controller away from the firmware -- which that commit had to
do, or nothing else about USB worked -- took the emulation with it, so from
then on this driver's own HID path was the only thing between a keypress and
the shell. It did not work, for four reasons an emulator cannot show you:

  * The configuration descriptor was read into a 64-byte buffer and clamped to
    it. QEMU's usb-kbd is 34 bytes. A real keyboard with a second interface for
    its media keys is 59, a third takes it past 84, and the clamp does not
    fail -- it truncates, so the endpoint descriptor simply is not there.

  * Only the first interface was looked at, and its protocol byte was applied
    to the first interrupt-IN endpoint found anywhere in the configuration. A
    receiver carrying a keyboard and a mouse as two interfaces of one device
    therefore had one of them decoded as the other.

  * The endpoint context's Interval field was left at zero, which is out of
    range for a full-speed interrupt endpoint. A real controller answers
    Configure Endpoint with a parameter error; QEMU does not check.

  * Max ESIT Payload was left at zero too, which is a request for no periodic
    bandwidth: the endpoint is configured, the doorbell is accepted, and the
    controller never schedules a transaction for it.

Two things are asserted, and the second is the one that cannot be faked:

  1. Both interfaces are armed, and the kernel says so in numbers -- one USB
     keyboard, one USB mouse, each with an endpoint and a report size.

  2. Input actually crosses the wire. There is no aiming a keystroke at one of
     several keyboards from the host -- `input-send-event` routes by display,
     not by device -- so instead the guest is asked. `usb` prints a per-endpoint
     count of the reports each HID endpoint has delivered, and those counters
     moving is the whole chain working: scheduled by the controller, completed
     on the event ring, decoded, and injected. A keystroke that arrived over
     the emulated PS/2 controller instead would leave them at zero.

There is no compositor in safe mode, so the mouse's cursor cannot be watched.
A decoded report is a report that crossed the wire, which is the part that was
broken.
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, find_ovmf


def counters(vm, timeout=60):
    """`usb`'s per-endpoint report counts, as {"keyboard": n, "mouse": n}."""
    vm.clear()
    vm.type("usb\n")
    assert vm.wait_for("input:", timeout=timeout), \
        "`usb` printed no input line:\n" + vm.serial()[-1200:]
    time.sleep(0.8)                     # let the rest of the listing arrive
    out = vm.serial()
    got = {}
    for m in re.finditer(r"slot\s+(\d+)\s+(keyboard|mouse)\s+DCI\s+(\d+)\s+"
                         r"(\d+)-byte reports, (\d+) delivered", out):
        got[m.group(2)] = int(m.group(5))
    return got, out


def run(uefi=False):
    # usb-kbd and usb-mouse are boot-protocol devices (subclass 1, protocol 1
    # and 2), which is exactly what this driver claims to drive.
    with BoltVM(name="usbhid-efi" if uefi else "usbhid", net=False, uefi=uefi,
                usb=["usb-kbd,id=ukbd", "usb-mouse,id=umouse"]) as vm:
        # Safe mode: a serial shell, and USB is probed either way -- xhci_init
        # runs well before the branch that skips the network and the audio.
        log = vm.reach_shell(420 if uefi else 240, "2")

        # The UEFI half is the whole reason this test has two passes. Firmware
        # puts 64-bit BARs above the 4 GiB the kernel direct-maps -- OVMF at
        # 768 GiB -- and pci_map_bar() used to refuse them and return, so the
        # controller below was never mapped and the machine had no USB at all.
        # It also has no fallback: on a UEFI boot the firmware's own USB driver
        # stops at ExitBootServices, and there is no legacy PS/2 emulation
        # behind it, so a keyboard that is USB is a keyboard or nothing.
        if uefi:
            for bad in ("outside the low 4 GiB", "could not map MMIO"):
                assert bad not in log, \
                    "a BAR was refused on the UEFI path: %r\n%s" % (bad, log[-2000:])

        kb = re.search(r"\[xhci\] slot (\d+) iface (\d+): boot keyboard armed on "
                       r"DCI (\d+) \((\d+)-byte reports\)", log)
        assert kb, ("no USB keyboard was armed. Either the device did not "
                    "enumerate or its endpoint was refused; the xhci lines "
                    "above say which:\n" + log[-2000:])
        ms = re.search(r"\[xhci\] slot (\d+) iface (\d+): boot mouse armed on "
                       r"DCI (\d+) \((\d+)-byte reports\)", log)
        assert ms, "no USB mouse was armed:\n" + log[-2000:]
        print("  ok: keyboard on slot %s DCI %s, mouse on slot %s DCI %s"
              % (kb.group(1), kb.group(3), ms.group(1), ms.group(3)))

        # The report size has to come from the endpoint descriptor rather than
        # from the protocol: ask for fewer bytes than the device sends and the
        # packet is a babble error, which halts the endpoint on the first key.
        assert int(kb.group(4)) >= 8, \
            "keyboard reports sized %s bytes; a boot report is 8" % kb.group(4)

        m = re.search(r"input: (\d+) keyboards? \((\d+) PS/2, (\d+) USB\), "
                      r"(\d+) pointers? \((\d+) PS/2, (\d+) USB\)", log)
        assert m, "the boot log printed no input summary:\n" + log[-1500:]
        assert int(m.group(3)) == 1 and int(m.group(6)) == 1, \
            "the summary counted %s USB keyboards and %s USB pointers, not 1 " \
            "and 1: %s" % (m.group(3), m.group(6), m.group(0))
        print("  ok: the boot log's input summary counts both of them")

        before, out = counters(vm)
        assert "keyboard" in before and "mouse" in before, \
            "`usb` did not list both HID endpoints:\n" + out[-1200:]

        marker = "usbhidmarker"
        vm.clear()
        vm.type("echo " + marker + "\n")
        assert vm.wait_for(marker, timeout=60), \
            "nothing typed reached the shell:\n" + vm.serial()[-1500:]

        for _ in range(12):
            vm.nudge(6, 4)
        time.sleep(0.6)

        after, out = counters(vm)
        assert after["keyboard"] > before["keyboard"], (
            "the USB keyboard delivered no reports (%d, was %d) although a line "
            "typed at the machine reached the shell -- so the keystrokes came "
            "in over the emulated PS/2 controller and the USB path is dead:\n%s"
            % (after["keyboard"], before["keyboard"], out[-1200:]))
        assert after["mouse"] > before["mouse"], (
            "the USB mouse delivered no reports after 12 movements (%d, was "
            "%d). Its endpoint is armed, so this is the endpoint never being "
            "scheduled:\n%s" % (after["mouse"], before["mouse"], out[-1200:]))
        print("  ok: %d keyboard and %d mouse reports crossed the wire"
              % (after["keyboard"] - before["keyboard"],
                 after["mouse"] - before["mouse"]))

    return 0


def main():
    print("-- legacy BIOS")
    run(uefi=False)
    if find_ovmf()[0]:
        print("-- UEFI")
        run(uefi=True)
    else:
        print("  SKIP: no OVMF firmware, so the UEFI half did not run")
    print("PASS usbhid: a USB keyboard and mouse enumerate, arm, and deliver")
    return 0


if __name__ == "__main__":
    sys.exit(main())
