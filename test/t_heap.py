#!/usr/bin/env python3
"""The kernel heap: coalescing and exhaustion on the host, then a live one.

The host half compiles mm/kheap.c and drives it through the allocation orders
real code uses, checking that the heap comes back whole each time. The QEMU
half asks a running BoltOS what its heap looks like -- including whether the
allocator's own structure still holds together -- before and after two
hundred and forty file operations have been through it.

    python test/t_heap.py --host-only     # no QEMU, ~2 seconds
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT

OUT = Path(ROOT) / "test" / "out"


def host_tests():
    cc = os.environ.get("BOLTOS_HOST_CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc and Path(r"C:\msys64\ucrt64\bin\clang.exe").exists():
        cc = r"C:\msys64\ucrt64\bin\clang.exe"
    if not cc:
        raise RuntimeError("host C compiler missing (set BOLTOS_HOST_CC)")
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / ("heap-host.exe" if os.name == "nt" else "heap-host")
    subprocess.run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-iquote", str(Path(ROOT) / "include"),
                    str(Path(ROOT) / "test" / "host" / "heap.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def shell(vm, line, expect=None, timeout=60):
    vm.type(line + "\n")
    if expect:
        vm.expect(expect, timeout=timeout)


def largest_free_kib(log):
    for line in reversed(log.splitlines()):
        if "largest free" in line:
            return int(line.split("largest free")[1].split("KiB")[0].strip())
    raise AssertionError("meminfo did not report a largest free block:\n" + log[-2000:])


def qemu_tests():
    with BoltVM(name="heap", net=False) as vm:
        vm.reach_shell(entry="2")

        shell(vm, "meminfo", "structure ok")
        before = largest_free_kib(vm.serial())
        assert before > 1024, "a freshly booted heap should have megabytes in one piece"
        print("  ok: the heap is sound after boot, largest free block %d KiB" % before)

        # Churn it the way using the machine does: files made, filled and
        # removed, over and over. Created in order and removed in order is
        # exactly the pattern the old allocator could not merge -- when each
        # block was released its successor was still in use, and its
        # predecessor was behind it, where nothing looked.
        names = " ".join(str(i) for i in range(1, 41))
        body = "a" * 200
        for r in range(3):
            vm.type("for n in %s; do write /tmp/h$n %s; done\n" % (names, body))
            # A marker, not the prompt: the prompt is already on screen and
            # would match before the loop had run a single iteration.
            shell(vm, "echo churn-wrote-%d" % r, "churn-wrote-%d" % r, timeout=180)
            vm.type("for n in %s; do rm /tmp/h$n; done\n" % names)
            shell(vm, "echo churn-freed-%d" % r, "churn-freed-%d" % r, timeout=180)

        vm.clear()
        shell(vm, "meminfo", "structure ok")
        after = largest_free_kib(vm.serial())
        log = vm.serial()
        assert "bad free" not in log, "the kernel freed something it never allocated"
        # Not an equality: the shell keeps history, the log grew, and files were
        # created. What must not happen is the big block breaking up.
        assert after > before * 3 // 4, (
            "largest free block fell from %d to %d KiB over 240 file operations"
            % (before, after))
        print("  ok: 240 file operations later it is still sound, "
              "%d KiB in one piece" % after)

        shell(vm, "cat /proc/meminfo", "KHeapLargest")
        assert "KHeapUsed:" in vm.serial()
        print("  ok: /proc/meminfo reports what is free and what is free in one piece")

        vm.expect_absent("KERNEL PANIC")
        vm.expect_absent("structure INCONSISTENT")
    return 0


def main(argv):
    host_tests()
    if "--host-only" in argv:
        print("PASS heap: allocator tests only (--host-only)")
        return 0
    rc = qemu_tests()
    print("PASS heap: coalescing, exhaustion, and a live heap that stays whole")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
