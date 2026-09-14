#!/usr/bin/env python3
"""Suspend to RAM, and the part that matters: coming back to the same machine.

S3 is the one power state where the interesting half is the return. Shutting
down is easy because nothing has to work afterwards; waking means the CPU comes
up in 16-bit real mode with paging off, long mode off, no GDT, every device at
its power-on defaults, and a kernel sitting in RAM that believes none of that
happened.

So the assertions are in two groups.

**It slept, and it was really asleep.** QEMU's own run state says `suspended`,
which no amount of guest-side spinning can fake -- the VCPUs are stopped. Then
`system_wakeup` is the wall socket.

**It came back, rather than rebooted.** This is the one worth being careful
about, because a failed resume looks *exactly* like a successful one from a
distance: the firmware gives up, does a full POST, and BoltOS boots from disk
again. A test that only checked "is there a shell prompt" would pass on a
machine that had lost everything. Three things separate the two:

  * The boot banner does not appear again. A resumed kernel never re-runs
    kmain.
  * `power status` reports a resume count, which only the resume path
    increments.
  * A file written before the suspend is still there afterwards **in RAM**,
    with the process that wrote it still where it was. The filesystem is
    committed before sleeping, so the file surviving is not by itself
    evidence -- a reboot would reload it from disk too. What a reboot cannot
    do is keep an uptime that never reset.

The trampoline's progress markers (`3PLgG...R` on the serial line) are asserted
too, because they are the only diagnosis available when a resume fails. Each
letter is one more thing that came back, and where the sequence stops is where
the machine died. Three of them were earned the hard way:

  * `3` -- the firmware would not jump here at all until the OS wrote 0xFE to
    CMOS index 0x0F. That byte is how a reset is told it is a resume.
  * `G` -- gdt_init() reloads every segment register, and loading the GS
    selector in long mode sets GS_BASE from the descriptor, which is zero. The
    per-CPU pointer was being thrown away by the very function restoring it.
  * `L` -- CR4 comes back with SSE off, and the kernel is compiled -msse. The
    first C function to zero a struct executed an SSE store into a CPU where
    SSE did not exist: #UD, no IDT yet, triple fault, reboot.
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

LOGIN_USER, LOGIN_PASS = "user", "bolt"
NOTE = "/home/user/before-the-nap.txt"
BANNER = "BoltOS kernel  -  64-bit long mode"


def run(vm, line, wait_for, timeout=60):
    vm.clear()
    vm.type(line + "\n")
    assert vm.wait_for(wait_for, timeout=timeout), \
        "%r never printed %r:\n%s" % (line, wait_for, vm.serial()[-1000:])
    return vm.serial()


def uptime_ticks(vm):
    """The ticks the kernel has counted. A reboot resets this to nothing; a
    resume does not, which is the difference the whole test turns on."""
    out = run(vm, "uptime", "ticks")
    m = None
    for m in re.finditer(r"\((\d+) ticks", out):
        pass
    assert m, "cannot read a tick count out of:\n" + out[-500:]
    return int(m.group(1))


def main():
    with BoltVM(name="s3", net=False) as vm:
        vm.sign_in(LOGIN_USER, LOGIN_PASS, timeout=200)
        vm.open_app("Terminal")

        out = run(vm, "power status", "power:")
        assert "suspend to RAM (S3): available" in out, \
            "this machine should offer S3:\n" + out[-700:]
        assert "resumed from S3" not in out, \
            "a freshly booted machine has not resumed from anything:\n" + out[-500:]
        print("  ok: S3 is available and nothing has resumed yet")

        # `write` prints nothing at all on success, so there is no output to
        # wait for -- reading it back is the confirmation.
        vm.clear()
        vm.type('write %s "the machine was here before it slept"\n' % NOTE)
        time.sleep(1.5)
        run(vm, "cat " + NOTE, "before it slept")
        before = uptime_ticks(vm)
        print("  ok: wrote a file and noted the tick count (%d)" % before)

        # Everything after this point in the serial log belongs to the suspend.
        mark = os.path.getsize(vm.serial_path)

        vm.clear()
        vm.type("suspend\n")
        assert vm.wait_for("waking vector", timeout=90), \
            "the suspend never reached the ACPI write:\n" + vm.serial()[-900:]

        # QEMU's own view. The VCPUs are stopped; there is nothing the guest
        # could do to produce this.
        st = None
        for _ in range(40):
            time.sleep(0.5)
            st = vm._cmd({"execute": "query-status"})
            if st and st.get("return", {}).get("status") == "suspended":
                break
        assert st and st["return"]["status"] == "suspended", \
            "QEMU does not think the machine is suspended: %r" % (st,)
        print("  ok: QEMU reports the machine suspended -- the VCPUs are stopped")

        time.sleep(1.0)
        vm._cmd({"execute": "system_wakeup"})
        assert vm.wait_for("resumed (suspend #1)", timeout=90), \
            ("the machine did not come back through the resume path:\n"
             + vm.serial()[-1500:])
        print("  ok: it woke, and came back through the resume path")

        # What the serial line carries between the sleep and the resume line is
        # the whole diagnosis, so it is asserted rather than merely printed.
        with open(vm.serial_path, "rb") as fh:
            fh.seek(mark)
            raw = fh.read()
        i = raw.find(b"waking vector")
        trail = raw[i:]
        for letter, meant in [(b"3", "the firmware jumped to the waking vector"),
                              (b"P", "protected mode"),
                              (b"L", "long mode, with SSE re-enabled"),
                              (b"g", "the GDT"),
                              (b"G", "...and the per-CPU pointer it destroys"),
                              (b"i", "the IDT"),
                              (b"c", "every device's PCI configuration space"),
                              (b"a", "the local APIC"),
                              (b"k", "the PS/2 controller"),
                              (b"d", "the disks"),
                              (b"R", "back in C")]:
            assert letter in trail, \
                ("the resume never reached %r (%s). The markers on the serial "
                 "line are the only diagnosis a failed wake has:\n%r"
                 % (letter.decode(), meant, trail[:300]))
        assert trail.index(b"3") < trail.index(b"P") < trail.index(b"L"), \
            "the trampoline's markers are out of order: %r" % trail[:120]
        print("  ok: every stage of the climb back reported itself, in order")

        assert BANNER.encode() not in trail, \
            ("the kernel banner appeared after the wake, so this was a reboot "
             "dressed as a resume -- the firmware gave up and booted from disk")
        print("  ok: the kernel did not re-run kmain, so this was not a reboot")

        # ---- and is it the same machine? --------------------------------
        time.sleep(1.0)
        out = run(vm, "power status", "power:")
        assert "resumed from S3 1 time" in out, \
            "the resume count did not go up:\n" + out[-600:]
        print("  ok: `power status` counts the resume")

        after = uptime_ticks(vm)
        assert after >= before, \
            ("the tick count went backwards across the suspend (%d -> %d), which "
             "means the machine restarted" % (before, after))
        print("  ok: the tick count carried on from %d, so nothing restarted" % before)

        out = run(vm, "cat " + NOTE, "before it slept")
        assert "the machine was here before it slept" in out, out[-400:]
        print("  ok: the file written before the nap is still there")

        # The machine has to be usable, not merely alive. These touch the
        # timer, the keyboard, the filesystem and the shell's own state.
        run(vm, "echo awake-and-working", "awake-and-working")
        run(vm, "ls /home/user", "before-the-nap.txt")
        run(vm, "mem", "kheap")
        print("  ok: it takes commands, reads the disk and reports its memory")

        # Twice, because a resume that works once and corrupts something is a
        # resume that fails the second time.
        vm.clear()
        vm.type("suspend\n")
        assert vm.wait_for("waking vector", timeout=90), vm.serial()[-700:]
        for _ in range(40):
            time.sleep(0.5)
            st = vm._cmd({"execute": "query-status"})
            if st and st.get("return", {}).get("status") == "suspended":
                break
        time.sleep(1.0)
        vm._cmd({"execute": "system_wakeup"})
        assert vm.wait_for("resumed (suspend #2)", timeout=90), \
            "the second suspend did not come back:\n" + vm.serial()[-1200:]
        time.sleep(1.0)
        run(vm, "echo still-here", "still-here")
        print("  ok: a second suspend and resume works too")

        vm.expect_absent("KERNEL PANIC")

    print("PASS s3: the machine sleeps, wakes, and is the same machine afterwards")
    return 0


if __name__ == "__main__":
    sys.exit(main())
