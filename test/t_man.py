#!/usr/bin/env python3
"""`man`: every command says what it takes.

`help` lists 120 commands with a line each, which is enough to remember a name
and never enough to use one. A command whose arguments you discover by getting
them wrong is a command you avoid.

The assertion that matters here is coverage: `man -c` reports any command with
no page, and this test fails if there is one. That is what keeps the manual
honest as commands are added -- a page is part of adding a command, not a thing
to be done later.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM


def reach_shell(vm, timeout=180):
    for _ in range(16):
        if "boot mode" in vm.serial():
            break
        vm.key("2")
        time.sleep(0.4)
    vm.expect("BoltShell ready", timeout=timeout)
    vm.wait_prompt()


def main():
    with BoltVM(name="man", extra=["-vga", "none"]) as vm:
        reach_shell(vm)

        out = vm.run("man -c", timeout=90)
        assert "0 missing" in out, "some commands have no manual page:\n" + out
        print("  ok: every command in the table has a page")

        out = vm.run("man build")
        assert "usage: build" in out, "the page has no synopsis:\n" + out
        assert "build.txt" in out, "the page does not say what a target is:\n" + out
        print("  ok: a page has a synopsis and says what the command is for")
        out = vm.run("help build")
        assert "usage: build" in out and "build.txt" in out, out

        # -k is how you find a command whose name you do not know.
        out = vm.run("man -k backtrace")
        assert "core" in out, "-k did not find the command that shows a backtrace:\n" + out
        print("  ok: `man -k` searches the pages, not just the names")

        out = vm.run("man nosuchcommand")
        assert "no such command" in out, "an unknown name was not reported:\n" + out

        vm.expect_absent("KERNEL PANIC")

    print("PASS man: the manual covers every command")
    return 0


if __name__ == "__main__":
    sys.exit(main())
