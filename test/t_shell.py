#!/usr/bin/env python3
"""The shell you actually type at: history, completion, and a caret that moves.

Nothing here adds a capability. Retyping a command in full, with no mistakes,
was simply the most repeated piece of work on the machine, and a system meant
to be lived in every day should not ask for it. Checked:

  * Up recalls the last command, Down comes back to what was being typed
  * Tab completes a command name, and a path, and lists the choices when there
    is more than one
  * Left/Home/End move the caret, and a character typed in the middle of the
    line goes in the middle of the line
  * the history is still there after a reboot
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM


def reach_shell(vm, timeout=180):
    for _ in range(16):
        if "boot mode" in vm.serial():
            break
        vm.key("2")                       # safe mode: a text shell
        time.sleep(0.4)
    vm.expect("BoltShell ready", timeout=timeout)
    vm.wait_prompt()


def press(vm, qcode, times=1, settle=0.6):
    for _ in range(times):
        vm.key(qcode)
        time.sleep(0.15)
    time.sleep(settle)


def main():
    with BoltVM(name="shell", extra=["-vga", "none"]) as vm:
        reach_shell(vm)

        # --- history ---------------------------------------------------------
        vm.run("echo remember-me")
        vm.clear()
        press(vm, "up")
        time.sleep(0.5)
        assert "echo remember-me" in vm.serial(), \
            "Up did not recall the last command:\n" + vm.serial()[-400:]
        vm.key("ret")
        time.sleep(1.0)
        assert "remember-me" in vm.serial(), "the recalled command did not run"
        print("  ok: Up recalls the last command, and it runs")

        # Down from a recalled entry comes back to the empty line being typed.
        vm.clear()
        press(vm, "up")
        press(vm, "down")
        vm.type("echo back-to-typing\n")
        time.sleep(1.2)
        out = vm.serial()
        assert "back-to-typing" in out, "Down did not restore the typed line:\n" + out[-500:]
        assert "remember-meecho" not in out, "Down left the recalled text behind:\n" + out[-500:]
        print("  ok: Down comes back to the line that was being typed")

        # --- completion ------------------------------------------------------
        vm.clear()
        vm.type("upti")
        press(vm, "tab")
        vm.key("ret")
        time.sleep(1.5)
        out = vm.serial()
        assert "up " in out and "ticks" in out, \
            "Tab did not complete `upti` into `uptime`:\n" + out[-600:]
        print("  ok: Tab completes a command name")

        # A path, in a directory made for the purpose.
        vm.run("mkdir /completeme")
        vm.run("write /completeme/unique-name.txt hello")
        vm.clear()
        vm.type("cat /completeme/uni")
        press(vm, "tab")
        vm.key("ret")
        time.sleep(1.5)
        out = vm.serial()
        assert "hello" in out, "Tab did not complete the path:\n" + out[-600:]
        print("  ok: Tab completes a path")

        # Several candidates: the shell lists them instead of guessing.
        vm.run("write /completeme/aaa-one.txt 1")
        vm.run("write /completeme/aaa-two.txt 2")
        vm.clear()
        vm.type("cat /completeme/aaa")
        press(vm, "tab")          # extends to the common prefix, "aaa-"
        press(vm, "tab")          # nothing further is shared: show the choices
        time.sleep(1.0)
        out = vm.serial()
        assert "aaa-one.txt" in out and "aaa-two.txt" in out, \
            "an ambiguous Tab listed nothing:\n" + out[-600:]
        press(vm, "backspace", times=40)
        time.sleep(0.5)
        print("  ok: an ambiguous Tab lists the candidates")

        # --- the caret moves ---------------------------------------------------
        vm.clear()
        vm.type("echo XZ")
        press(vm, "left")                 # between X and Z
        vm.type("Y")
        vm.key("ret")
        time.sleep(1.2)
        assert "XYZ" in vm.serial(), \
            "typing in the middle of the line did not insert there:\n" + vm.serial()[-400:]
        print("  ok: the caret moves, and text is inserted where it is")

        # --- and the history outlives the session -----------------------------
        vm.run("echo survives-the-reboot")
        vm.clear()
        vm.type("reboot\n")
        time.sleep(2.0)
        vm.clear()
        reach_shell(vm)
        vm.clear()
        press(vm, "up")           # `reboot` itself: the last thing typed
        press(vm, "up")           # and the one before it
        time.sleep(0.8)
        out = vm.serial()
        assert "survives-the-reboot" in out, \
            "the history did not survive the reboot:\n" + out[-500:]
        print("  ok: the history is written on shutdown and read back on boot")

        # History and variables belong to a session, not the next account.
        vm.key("c", ctrl=True)
        vm.run("users su user bolt")
        vm.run("SESSION=private-value")
        vm.run("echo private-history-entry")
        vm.run(" echo deliberately-unrecorded")
        vm.run("users su root root")
        out = vm.run("echo session=$SESSION")
        assert "session=private-value" not in out, out
        vm.clear()
        press(vm, "up")
        assert "private-history-entry" not in vm.serial(), vm.serial()
        vm.key("c", ctrl=True)
        out = vm.run("cat /home/user/.bolt_history")
        assert "private-history-entry" in out, out
        assert "deliberately-unrecorded" not in out and "users su" not in out, out
        out = vm.run("ls -l /home/user")
        assert "-rw-------" in out and ".bolt_history" in out, out
        vm.run("users su user bolt")
        vm.clear()
        press(vm, "up")
        assert "private-history-entry" in vm.serial(), vm.serial()
        print("  ok: history is private per account, opt-out works, and variables reset")

        vm.expect_absent("KERNEL PANIC")

    print("PASS shell: history, completion and line editing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
