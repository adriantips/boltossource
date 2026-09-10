#!/usr/bin/env python3
"""Global shortcuts are a table, not code.

Alt+Tab was a synthetic byte invented inside the keyboard driver, Win+D was an
`if` in the compositor, and Ctrl+Alt+L was a special case in the middle of the
Ctrl-key handler. Each worked and none could be changed.

The interesting assertions are the ones about a *rebound* chord, because they
are the ones that could not have passed before: that the new chord fires, that
the old one stops firing and goes back to being an ordinary keystroke, and that
the change is still there after a reboot. The rest is the guard rails -- a bare
letter refused, a duplicate chord refused, and a garbled config file ignored
rather than taken as a reason to boot without a keyboard.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

LOGIN_USER, LOGIN_PASS = "user", "bolt"


def reach_shell(vm, timeout=180):
    for _ in range(16):
        if "boot mode" in vm.serial():
            break
        vm.key("2")
        time.sleep(0.4)
    vm.expect("BoltShell ready", timeout=timeout)
    vm.wait_prompt()


def text_leg():
    """Everything that can be checked without a desktop, which is most of it."""
    with BoltVM(name="keys", snapshot=False, extra=["-vga", "none"]) as vm:
        reach_shell(vm)

        out = vm.run("keys")
        for name in ("start-menu", "cycle-window", "lock", "show-desktop",
                     "desk-next", "screenshot"):
            assert name in out, "%s is not in the table:\n%s" % (name, out)
        assert "Alt+Tab" in out, "the default chords are not shown:\n" + out
        assert "Ctrl+Alt+L" in out, out
        assert "Win+D" in out, out
        assert "F12" in out, out
        print("  ok: every action lists its chord, and the defaults are unchanged")

        # A bare letter would take that key away from every program.
        out = vm.run("keys lock k")
        assert "no modifier" in out, "a bare letter was accepted:\n" + out
        out = vm.run("keys lock Ctrl+Alt+L")     # unchanged, so still fine
        assert "lock = Ctrl+Alt+L" in out, out

        # Two actions cannot share one chord.
        out = vm.run("keys show-desktop Ctrl+Alt+L")
        assert "already" in out, "a duplicate chord was accepted:\n" + out
        print("  ok: a bare key and a duplicate chord are both refused")

        out = vm.run("keys lock Ctrl+Alt+K")
        assert "lock = Ctrl+Alt+K" in out, out
        out = vm.run("keys")
        assert "* lock" in out, "a changed binding is not marked:\n" + out
        assert "Ctrl+Alt+K" in out and "Ctrl+Alt+L" not in out.split("lock")[1][:40], out
        print("  ok: a shortcut can be rebound")

        out = vm.run("cat /etc/keys.conf")
        assert "lock = Ctrl+Alt+K" in out, "the change was not written:\n" + out
        assert "show-desktop = Win+D" in out, out
        print("  ok: it is written to /etc/keys.conf")

        # F-keys are the exception to the bare-key rule; nothing types one.
        out = vm.run("keys screenshot F9")
        assert "screenshot = F9" in out, out
        out = vm.run("keys start-menu none")
        assert "start-menu = none" in out, "unbinding is not allowed:\n" + out

        # It has to survive a reboot, or it is a preference nobody would set.
        vm.clear()
        vm.type("reboot\n")
        vm.expect("BoltFS committed and marked clean", timeout=60)
        reach_shell(vm)
        out = vm.run("keys")
        assert "Ctrl+Alt+K" in out, "the binding did not survive a reboot:\n" + out
        assert "F9" in out, out
        assert "* start-menu" in out and "none" in out, out
        print("  ok: bindings reload from the file at boot")

        # A config file with rubbish in it must not cost you your keyboard.
        vm.run("write /etc/keys.conf \"lock = Ctrl+Alt+J\"")
        vm.run("write -a /etc/keys.conf \"nonsense-action = Ctrl+P\"")
        vm.run("write -a /etc/keys.conf \"show-desktop = Ctrl+Alt+J\"")
        vm.run("write -a /etc/keys.conf \"cycle-window = Wibble+Q\"")
        vm.clear()
        vm.type("reboot\n")
        vm.expect("BoltFS committed and marked clean", timeout=60)
        reach_shell(vm)
        log = vm.serial()
        assert "keyboard shortcuts" in log, "the keymap did not initialise:\n" + log[-1500:]
        out = vm.run("keys")
        assert "lock" in out and "Ctrl+Alt+J" in out, "the good line was not applied:\n" + out
        assert "Alt+Tab" in out, "a bad line lost an unrelated default:\n" + out
        print("  ok: bad lines are reported and skipped, good ones still apply")

        vm.run("keys reset")
        out = vm.run("keys")
        assert "Ctrl+Alt+L" in out and "Alt+Tab" in out and "Win+D" in out, out
        assert "*" not in out.split("action")[1].split("(*")[0], \
            "reset left something marked as changed:\n" + out
        print("  ok: reset puts every default back")

        vm.expect_absent("KERNEL PANIC")


def desktop_leg():
    """The half that needs a compositor: a rebound chord actually fires, and
    the chord it replaced goes back to being an ordinary keystroke."""
    with BoltVM(name="keysgui", net=False) as vm:
        vm.expect("BoltOS is up. Launching desktop.", timeout=150)
        time.sleep(4.0)
        vm.type(LOGIN_USER); vm.key("tab"); vm.type(LOGIN_PASS); vm.key("ret")
        time.sleep(3.0)

        vm.key("meta_l")
        vm.type("Terminal")
        vm.key("ret")
        time.sleep(2.0)

        # Rebind the desktop switcher onto a chord nothing else uses, then use
        # it. desk-2 lives on Win+2 by default; move it to Ctrl+Alt+F2.
        vm.clear()
        vm.type("keys desk-2 Ctrl+Alt+F2\n")
        vm.expect("desk-2 = Ctrl+Alt+F2", timeout=30)

        # The new chord works: switch to desktop 2, where nothing is open, so
        # typing reaches no window at all.
        vm.key("f2", ctrl=True, alt=True)
        time.sleep(1.0)
        vm.clear()
        vm.type("echo should-not-appear\n")
        time.sleep(1.5)
        assert "should-not-appear" not in vm.serial(), \
            "the rebound chord did not switch desktops:\n" + vm.serial()[-600:]
        print("  ok: a rebound chord fires")

        # And the old chord is an ordinary keystroke again. Win+2 must not
        # switch desktops -- and the proof is stronger than "nothing happened":
        # the '2' should land in the Terminal as a character, so submitting the
        # line asks the shell to run a command called "2".
        vm.key("1", meta=True)
        time.sleep(1.0)
        vm.clear()
        vm.type("echo back-on-one\n")
        vm.expect("back-on-one", timeout=30)

        vm.key("2", meta=True)                  # no longer bound to anything
        time.sleep(0.5)
        vm.clear()
        vm.key("ret")
        vm.expect("unknown command: 2", timeout=30)
        print("  ok: the chord it replaced is a plain keystroke again")

        vm.clear()
        vm.type("keys reset\n")
        vm.expect("back to its default", timeout=30)
        vm.expect_absent("KERNEL PANIC")


def main():
    text_leg()
    desktop_leg()
    print("PASS keys: shortcuts are data, editable, persistent and enforced")
    return 0


if __name__ == "__main__":
    sys.exit(main())
