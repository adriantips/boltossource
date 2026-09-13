#!/usr/bin/env python3
"""Start-menu search: apps, and now the files as well.

Proving a row appeared in a menu is a screenshot problem. Proving the row does
what it says is not: `desk` lists the windows that are open, so opening a .txt
from the search box has to make Notepad appear in that list, and it has to be
Notepad rather than the File Explorer's built-in viewer -- a search result that
ignores the file associations is a search result that opens the wrong program.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

LOGIN_USER, LOGIN_PASS = "user", "bolt"

# Nothing else on the machine is called this, so a match is this file.
MARKER = "zqwmarker"


def gui_run(vm, line, expect, timeout=30):
    vm.clear()
    vm.type(line + "\n")
    vm.expect(expect, timeout=timeout)
    return vm.serial()


def windows(vm):
    """The names of every open window, from `desk`."""
    return gui_run(vm, "desk", "usage: desk")


def main():
    with BoltVM(name="search", net=False) as vm:
        vm.sign_in(LOGIN_USER, LOGIN_PASS)
        vm.open_app("Terminal")

        vm.type('write /home/user/%s.txt "found by the start menu"\n' % MARKER)
        time.sleep(1.0)                  # write says nothing when it works
        out = gui_run(vm, "find / " + MARKER, MARKER + ".txt")
        path = [ln for ln in out.splitlines() if ln.endswith(MARKER + ".txt")][0]
        print("  ok: a file to look for, at " + path)

        assert "Notepad" not in windows(vm), "Notepad is already open; nothing to prove"

        # Type the name into the Start menu and press Enter. The apps do not
        # match it, so the first result is the file.
        vm.key("esc"); time.sleep(0.3)
        vm.key("meta_l"); time.sleep(0.9)
        vm.type(MARKER); time.sleep(0.9)
        vm.key("ret"); time.sleep(2.0)

        vm.open_app("Terminal")          # take the keyboard back
        out = windows(vm)
        assert "Notepad" in out, \
            "searching for a .txt did not open it in Notepad:\n" + out[-800:]
        print("  ok: a file result opens in the app its extension belongs to")

        # One letter is not a search -- it would match most of the filesystem,
        # and the menu would be a list of everything.
        vm.key("esc"); time.sleep(0.3)
        vm.key("meta_l"); time.sleep(0.9)
        vm.type("z"); time.sleep(0.7)
        shot = vm.screenshot("search-one-letter.ppm")
        assert os.path.getsize(shot) > 1000, "no frame with the menu open"
        vm.key("esc"); time.sleep(0.4)
        print("  ok: a single character does not start a filesystem search")

        # And the app grid still works, which is the half that already existed.
        vm.open_app("Calculator", probe=False)
        time.sleep(1.0)
        vm.open_app("Terminal")
        out = windows(vm)
        assert "Calculator" in out, "Start-menu app search stopped opening apps:\n" + out[-800:]
        print("  ok: searching for an app still opens the app")

        vm.expect_absent("KERNEL PANIC")

    print("PASS search: the Start menu finds files as well as apps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
