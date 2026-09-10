#!/usr/bin/env python3
"""Scheduler: processes are reaped, memory comes back, and kill works.

Running and killing processes used to leak a 64 KiB kernel stack plus an entire
page-table tree and every data page, every time -- so a machine that launched
programs all day slowly ran out of memory. This runs a program twenty times and
checks free memory is essentially where it started.
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

RUNS = 20


def reach_shell(vm, timeout=180):
    return vm.reach_shell(timeout, "2")


def shell(vm, line, expect=None, timeout=60):
    vm.type(line + "\n")
    if expect:
        vm.expect(expect, timeout=timeout)


def free_kib(vm):
    """Read PhysFree out of a fresh `meminfo`."""
    vm.clear()
    shell(vm, "cat /proc/meminfo", "PhysFree:", timeout=40)
    time.sleep(0.6)
    m = re.search(r"PhysFree:\s+(\d+) kB", vm.serial())
    assert m, "could not read PhysFree from /proc/meminfo:\n" + vm.serial()[-800:]
    return int(m.group(1))


def main():
    with BoltVM(name="sched", net=False) as vm:
        reach_shell(vm)

        # `ps` reports the real scheduler threads, including the watchdog.
        vm.clear()
        shell(vm, "ps", "scheduler threads", timeout=40)
        vm.expect("watchdog", timeout=30)      # the table streams a row at a time
        log = vm.serial()
        assert "demoA" in log and "demoB" in log, "demo threads missing from ps"
        print("  ok: ps lists the real scheduler threads, watchdog included")

        before = free_kib(vm)

        # Run a short-lived ring-3 program repeatedly. Each run allocates a
        # kernel stack, an address space and its pages; each exit must give
        # them all back.
        for i in range(RUNS):
            vm.clear()
            shell(vm, "crash ring3 null", "[fault] killed pid=", timeout=60)

        after = free_kib(vm)
        lost = before - after
        # Each leaked process would cost 64 KiB of stack plus its page tables
        # and pages -- well over 100 KiB. Twenty of them would be megabytes.
        print("  free memory: %d kB before, %d kB after %d processes (delta %d kB)"
              % (before, after, RUNS, lost))
        assert lost < 512, \
            "leaked %d kB over %d processes -- reclamation is not working" % (lost, RUNS)
        print("  ok: %d processes ran and were fully reclaimed" % RUNS)

        vm.expect_absent("KERNEL PANIC")
    print("PASS sched: processes are reaped and their memory returned")
    return 0


if __name__ == "__main__":
    sys.exit(main())
