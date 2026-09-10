#!/usr/bin/env python3
"""A key pressed while the machine is starting must not cost you the keyboard.

This is a real bug that shipped: type anything between the boot menu and
keyboard_init() and the PS/2 controller was wedged for the rest of the session.
Not a lost keystroke -- a machine with no keyboard until the next reboot, on
hardware where the only way to reboot it is a keyboard.

The cause was the drain. A byte left in the 8042 by the firmware or the boot
menu raised its interrupt at a moment when no handler was installed, and the
controller will not raise another until that byte is read. keyboard_init() read
exactly one byte, after installing the handler -- enough for the single byte the
boot menu leaves behind, and not enough for somebody impatient.

Every other test now types during boot (BoltVM.reach_shell probes as soon as the
kernel names its boot mode), so they all depend on this. This one says so.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM


def main():
    with BoltVM(name="bootkbd", net=False, extra=["-vga", "none"]) as vm:
        # Get through the menu, then hammer the keyboard while the kernel is
        # still bringing itself up -- well before keyboard_init().
        while "boot mode" not in vm.serial():
            vm.key("2")
            time.sleep(0.3)
        for _ in range(4):
            vm.type("xyzzy")
        print("  ok: typed 20 characters during kernel bring-up")

        # The shell has to answer afterwards. This is the whole assertion: a
        # working keyboard after an impatient user.
        vm.expect("BoltShell ready", timeout=180)
        deadline = time.time() + 90
        while time.time() < deadline:
            vm.clear()
            vm.key("ret")
            vm.type("echo keyboardalive\n")
            time.sleep(1.2)
            if "keyboardalive" in vm.serial():
                break
        else:
            raise AssertionError(
                "the keyboard is dead after typing during boot\n--- serial ---\n"
                + vm.serial()[-1500:])
        print("  ok: the keyboard still works after being typed at during boot")

        # And the early keystrokes were buffered rather than thrown away: the
        # shell should have seen `xyzzy` and complained about it.
        log = vm.serial() + open(vm.serial_path, "rb").read().decode("utf-8", "replace")
        assert "xyzzy" in log, \
            "the keystrokes typed during boot vanished entirely\n" + log[-1500:]
        print("  ok: the keystrokes were queued, not dropped")

        vm.expect_absent("KERNEL PANIC")

    print("PASS bootkbd: typing during boot costs neither the keys nor the keyboard")
    return 0


if __name__ == "__main__":
    sys.exit(main())
