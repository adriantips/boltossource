#!/usr/bin/env python3
"""The kernel log outliving the boot it was written in.

klog_persist() has always written the ring to /var/log/kernel.log on a clean
shutdown, and nothing has ever read it back -- so the log survived the reboot
and then had no way of being seen, which is indistinguishable from not
surviving it.

Two things are worth asserting. That a log written on one boot is readable on
the next, which is the whole feature. And that `logs save` works, because the
ring is otherwise only persisted at shutdown -- meaning the boot that goes
wrong is exactly the boot whose log is lost, which is the wrong way round.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM


def reach_shell(vm, timeout=180):
    return vm.reach_shell(timeout, "2")


def main():
    with BoltVM(name="logs", snapshot=False, extra=["-vga", "none"]) as vm:
        reach_shell(vm)

        vm.run("logs clear")
        out = vm.run("logs")
        assert "is empty" in out, "clear did not empty the file:\n" + out
        assert "dmesg" in out, "the empty case should say where this boot's log is:\n" + out
        print("  ok: an empty log says so, and says where to look instead")

        # `logs save` is the difference between a log that survives a clean
        # shutdown and one that survives anything.
        out = vm.run("logs save", timeout=90)
        assert "appended to" in out, "save reported nothing:\n" + out
        # -a, not the default: the banner is at the *top* of the log and the
        # default view is deliberately the tail.
        out = vm.run("logs -a", timeout=90)
        assert "BoltOS kernel" in out, \
            "the saved log does not contain this boot:\n" + out[-1200:]
        assert "1 boot" in out, "the boot count is wrong:\n" + out[:400]
        print("  ok: `logs save` writes this boot's ring to disk")

        out = vm.run("logs -n 3")
        body = out.split("boot)")[-1]
        assert body.count("\n") < 12, "-n 3 printed far more than three lines:\n" + out
        assert "last 3 lines" in out, "-n did not take effect:\n" + out
        print("  ok: it shows the tail, and -n says how much")

        # The real assertion: across a reboot.
        vm.clear()
        vm.type("reboot\n")
        vm.expect("BoltFS committed and marked clean", timeout=90)
        reach_shell(vm)

        out = vm.run("logs")
        assert "2 boots" in out or "3 boots" in out, \
            "the earlier boot's log did not survive:\n" + out
        print("  ok: the log from the previous boot is still there after a reboot")

        # A clean shutdown persists it without being asked, which is what makes
        # the boot count go up on its own.
        out = vm.run("logs -a")
        assert out.count("BoltOS kernel") >= 2, \
            "only one boot is in the file:\n" + out[-1500:]
        print("  ok: a clean shutdown appends without being asked")

        out = vm.run("logs nonsense")
        assert "unknown option" in out, "a bad option was accepted:\n" + out

        vm.expect_absent("KERNEL PANIC")

    print("PASS logs: the kernel log survives the boot and can be read back")
    return 0


if __name__ == "__main__":
    sys.exit(main())
