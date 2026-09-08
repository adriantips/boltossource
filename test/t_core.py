#!/usr/bin/env python3
"""Core dumps: a ring-3 crash you can still investigate afterwards.

A crash already printed its registers and got a line in the crash log, which
says *that* a program died. What it was doing -- the call chain, the arguments,
the locals -- lives on the stack, and the stack went away with the address
space a moment later.

So the fault path saves it. Checked here:

  * a faulting program leaves a dump in /var/crash, and `core list` finds it
  * `core show` decodes the fault and prints the registers
  * the backtrace is *named*, resolved out of the program's own .symtab -- the
    difference between a column of hex and a bug report
  * the saved stack is there, and the dump survives a reboot
  * a program crashing over and over cannot fill the disk
"""
import os, re, sys, time
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


def main():
    with BoltVM(name="core", extra=["-vga", "none"]) as vm:
        reach_shell(vm)

        # --- a ring-3 fault writes a dump ------------------------------------
        out = vm.run("crash ring3 null", timeout=90)
        assert "[core]" in out, "no core dump was written for a ring-3 fault:\n" + out
        m = re.search(r"/var/crash/(\S+\.core)", out)
        assert m, "the core dump has no name:\n" + out
        dump = m.group(1)
        print("  ok: a faulting program leaves %s" % dump)

        out = vm.run("core list")
        assert dump in out, "`core list` does not show the dump:\n" + out
        assert "page fault" in out, "the listing does not say what killed it:\n" + out

        # --- and it can be read back -----------------------------------------
        out = vm.run("core show " + dump, timeout=90)
        assert "/bin/crashme" in out, "the dump does not name the program:\n" + out
        assert "page fault" in out, "the fault was not decoded:\n" + out
        assert "RIP 0x" in out and "RSP 0x" in out, "registers are missing:\n" + out
        assert "backtrace" in out, "there is no backtrace:\n" + out
        assert "main" in out, "the backtrace named no function -- symbols missing:\n" + out
        assert "stack " in out, "the saved stack is missing:\n" + out
        print("  ok: `core show` decodes the fault and names the frames")

        # --- a different fault, and the dumps do not pile up unboundedly -----
        out = vm.run("crash ring3 ud", timeout=90)
        assert "[core]" in out, "no dump for an invalid-opcode fault:\n" + out
        for _ in range(9):
            vm.run("crash ring3 div", timeout=90)
        out = vm.run("core list")
        assert out.count(".core") <= 9, "dumps are not being pruned:\n" + out
        print("  ok: the newest dumps are kept and the rest pruned")

        # --- it is still there after a reboot ---------------------------------
        vm.run("sync")
        vm.clear()
        vm.reset()
        reach_shell(vm)
        out = vm.run("core list")
        assert ".core" in out, "the dumps did not survive a reboot:\n" + out
        print("  ok: dumps survive the reboot that follows a crash")

        out = vm.run("core rm all")
        assert "removed" in out, "`core rm all` did nothing:\n" + out
        out = vm.run("core list")
        assert "no core dumps" in out, "the dumps were not removed:\n" + out
        print("  ok: `core rm all` clears them")

        vm.expect_absent("KERNEL PANIC")

    print("PASS core: a ring-3 crash leaves a dump that names its own frames")
    return 0


if __name__ == "__main__":
    sys.exit(main())
