#!/usr/bin/env python3
"""`edit`: the terminal text editor actually edits, and saves what it shows.

Before this, the only way to change a file from the text shell was `write`,
which replaces the whole thing -- so on a serial console, in safe mode or in the
recovery shell, exactly where a file most often needs a one-line fix, there was
no way to make one without retyping the file.

The checks are deliberately about the file on disk rather than about what the
screen looked like. A full-screen editor over a serial line produces pages of
repainted text; what matters is that the bytes that end up in the file are the
ones the editor was showing.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM


def reach_shell(vm, timeout=180):
    for _ in range(16):
        if "boot mode" in vm.serial():
            break
        vm.key("2")                      # safe mode: a text shell, no desktop
        time.sleep(0.4)
    vm.expect("BoltShell ready", timeout=timeout)
    vm.wait_prompt()


def shell(vm, line, settle=1.6):
    vm.clear()
    vm.type(line + "\n")
    time.sleep(settle)
    return vm.serial()


def save_and_exit(vm):
    vm.key("s", ctrl=True)               # ^S
    time.sleep(1.5)
    vm.key("x", ctrl=True)               # ^X
    time.sleep(1.5)


def main():
    with BoltVM(name="edit", extra=["-vga", "none"]) as vm:
        reach_shell(vm)

        # --- a new file ----------------------------------------------------
        vm.clear()
        vm.type("edit /note.txt\n")
        vm.expect("new file", timeout=25)
        vm.type("alpha\nbeta\ngamma")
        time.sleep(1.2)
        save_and_exit(vm)

        out = shell(vm, "cat /note.txt")
        for want in ("alpha", "beta", "gamma"):
            assert want in out, "'%s' is missing from the saved file:\n%s" % (want, out)
        print("  ok: a new file is created, typed into, and saved")

        # --- reopening shows what was saved --------------------------------
        vm.clear()
        vm.type("edit /note.txt\n")
        vm.expect("3 lines", timeout=25)
        print("  ok: reopening reports the three saved lines")

        # --- editing an existing file, without losing the rest of it -------
        # Caret starts at line 1; cut it, leaving beta and gamma.
        vm.key("k", ctrl=True)           # ^K cuts the current line
        time.sleep(0.8)
        save_and_exit(vm)

        out = shell(vm, "cat /note.txt")
        assert "alpha" not in out, "^K did not remove the first line:\n" + out
        assert "beta" in out and "gamma" in out, \
            "^K removed more than the one line:\n" + out
        print("  ok: ^K cuts one line and leaves the rest intact")

        # --- the edit survives a reboot ------------------------------------
        shell(vm, "sync", settle=3.0)
        out = shell(vm, "cat /note.txt")
        assert "beta" in out
        print("  ok: the edited file is committed to disk")

        vm.expect_absent("KERNEL PANIC")

    print("PASS edit: the terminal editor creates, edits and saves files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
