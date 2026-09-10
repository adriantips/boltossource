#!/usr/bin/env python3
"""Notifications: something that happens while you are looking away is still
there when you look back.

`gui_toast` showed one line for two and a half seconds and then it was gone.
That is fine for "saved" and useless for "the disk stopped accepting writes" --
the messages most worth reading are the ones you are least likely to be staring
at the corner of the screen for.

So notifications are stored, and this test is about the store rather than the
pixels: that a post survives being read back, that levels and sources come
through, that read/dismiss/clear do what they say, that the ring drops the
oldest instead of the newest when it fills, and -- the point of the whole
exercise -- that the kernel itself posts one when a ring-3 program crashes.
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM


def reach_shell(vm, timeout=180):
    return vm.reach_shell(timeout, "2")


def main():
    with BoltVM(name="notify", extra=["-vga", "none"]) as vm:
        reach_shell(vm)

        # A clean slate: earlier boot messages may have posted already.
        vm.run("notify clear")

        out = vm.run('notify "the build finished"')
        assert "the build finished" in out, "posting printed nothing:\n" + out
        print("  ok: a notification is posted")

        out = vm.run("notify list")
        assert "the build finished" in out, "the posted notification is not listed:\n" + out
        assert "1 unread" in out, "it should be unread until read:\n" + out
        print("  ok: it is stored, and unread")

        # Levels and sources are what let a reader triage a list of them.
        vm.run('notify -l error -s Disk "the disk is full"')
        vm.run('notify -l warn -s Battery "battery low"')
        out = vm.run("notify list")
        assert "error" in out and "Disk" in out, "level/source lost:\n" + out
        assert "warn" in out and "Battery" in out, "level/source lost:\n" + out
        assert "3 unread" in out, "expected three unread:\n" + out
        print("  ok: levels and sources are kept")

        out = vm.run("notify -l nonsense hello")
        assert "unknown level" in out, "a bad level was accepted:\n" + out

        # Reading is not deleting: a read notification stays, and -a shows it.
        vm.run("notify read")
        out = vm.run("notify list")
        assert "nothing unread" in out, "read did not clear the unread count:\n" + out
        out = vm.run("notify list -a")
        assert "the disk is full" in out, "read deleted it instead of marking it:\n" + out
        print("  ok: read marks, it does not delete")

        # Dismiss takes one; the others stay.
        # The unread marker is a column of its own, blank once read, so
        # splitting on whitespace shifts by one. Match the shape instead.
        ids = [m.group(1) for ln in out.splitlines() if "the disk is full" in ln
               for m in [re.match(r"\s*\*?\s*(\d+)\s+\d\d:\d\d", ln)] if m]
        assert ids, "could not find the id column:\n" + out
        out = vm.run("notify dismiss %s" % ids[0])
        assert "no notification" not in out, "dismiss did not find it:\n" + out
        out = vm.run("notify list -a")
        assert "the disk is full" not in out, "dismiss left it behind:\n" + out
        assert "battery low" in out, "dismiss took the wrong one:\n" + out
        print("  ok: dismiss removes exactly one")

        out = vm.run("notify dismiss 99999")
        assert "no notification" in out, "dismissing a missing id should say so:\n" + out

        # The ring is 64 deep and drops the oldest, not the newest -- the newest
        # is the one somebody is about to read.
        vm.run("notify clear")
        vm.run('for i in 1 2 3 4 5 6 7 8; do notify "line $i"; done', timeout=60)
        out = vm.run("notify list -a")
        assert "line 8" in out, "the newest was lost:\n" + out
        assert "line 1" in out, "eight should fit in a ring of 64:\n" + out
        print("  ok: a run of notifications is kept in order")

        out = vm.run("notify clear")
        out = vm.run("notify list -a")
        assert "nothing stored" in out, "clear left something behind:\n" + out
        print("  ok: clear empties the store")

        # The real point: the kernel posts these itself. A ring-3 crash is the
        # case where the program cannot tell anybody anything.
        vm.run("crash ring3", timeout=60)
        time.sleep(1.0)
        out = vm.run("notify list -a")
        assert "Crash" in out, "a ring-3 crash posted no notification:\n" + out
        assert "core" in out.lower(), "the notification does not point at the core dump:\n" + out
        print("  ok: a ring-3 crash notifies, and says where the evidence is")

        # Mirrored into the kernel log, so a headless run sees them too.
        out = vm.run("dmesg | grep notify", timeout=60)
        assert "[notify:" in out, "notifications are not in the kernel log:\n" + out
        print("  ok: notifications reach dmesg")

        vm.expect_absent("KERNEL PANIC")

    print("PASS notify: notifications are stored, triaged and posted by the kernel")
    return 0


if __name__ == "__main__":
    sys.exit(main())
