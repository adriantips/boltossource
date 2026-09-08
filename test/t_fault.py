#!/usr/bin/env python3
"""Per-process fault isolation: a ring-3 program that faults must not take the
kernel with it, and the crash must be recorded where the next boot can read it.

Every mode of /bin/crashme raises a different CPU exception, because they take
different paths through the fault handler:
    null   #PF  a bad data pointer
    div    #DE  a fault with no error code
    ud     #UD  a bad instruction
    stack  #PF  recursion off the end of the user stack

After all four, the shell must still be answering -- that is the whole point.
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


def shell(vm, line, expect=None, timeout=60):
    vm.type(line + "\n")
    if expect:
        vm.expect(expect, timeout=timeout)


def main():
    with BoltVM(name="fault", net=False) as vm:
        reach_shell(vm)

        for mode, want in (("null",  "page fault"),
                           ("div",   "divide by zero"),
                           ("ud",    "invalid opcode"),
                           ("stack", "page fault")):
            vm.clear()                      # only look at THIS crash's output
            shell(vm, "crash ring3 " + mode, "[fault] killed pid=", timeout=90)
            log = vm.serial()
            assert want in log, "expected %r for mode %s, got:\n%s" % (want, mode, log[-1200:])
            assert "KERNEL PANIC" not in log, \
                "a ring-3 fault (%s) panicked the kernel" % mode
            # The machine is still ours: run something and see it answer.
            shell(vm, "echo survived-" + mode, "survived-" + mode, timeout=30)
            print("  ok: %-6s faulted, was reaped, machine still running" % mode)

        # The crash reports were persisted, not just printed.
        shell(vm, "crashlog", "ring-3 fault", timeout=60)
        log = vm.serial()
        assert "/var/log/crash.log" in log, "crashlog did not name the log file"
        assert "backtrace:" in log, "the crash record has no backtrace"
        print("  ok: crash reports persisted to /var/log/crash.log")

        # dmesg replays the boot log from the ring, long after the console that
        # printed it has scrolled away.
        vm.clear()
        shell(vm, "dmesg", "BoltOS kernel  -  64-bit long mode", timeout=60)
        vm.expect("[ok] preemptive scheduler", timeout=120)
        print("  ok: dmesg replays the kernel message ring")

        # And the reports survive a reboot -- the reason they are on disk.
        vm.clear()
        shell(vm, "reboot", "BoltFS committed and marked clean", timeout=60)
        reach_shell(vm)
        shell(vm, "crashlog", "ring-3 fault", timeout=60)
        print("  ok: crash reports survived the reboot")

        vm.expect_absent("KERNEL PANIC")
    print("PASS fault: ring-3 crashes are contained, reported and persisted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
