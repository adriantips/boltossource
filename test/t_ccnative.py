#!/usr/bin/env python3
"""`cc -N`: a C program written on BoltOS, compiled to machine code on BoltOS.

Until now everything BoltCC produced was bytecode with its VM in front of it.
The same front end now has a second backend (kernel/ccnative.c) that emits
x86-64, `as` assembles it, and kernel/elf.c loads the result as an ordinary
ring-3 program. Nothing leaves the machine at any point.

The claim worth testing is not that a native program runs -- it is that the two
backends agree. One source file, compiled both ways, has to print the same
bytes and finish with the same status; if the native one is wrong about a
backward jump, a frame slot or the order printf reads its arguments, it differs
from an interpreter that has been right about those since before this existed.
So the VM is the oracle, and the test compares against it rather than against a
string someone typed into the test.

The other half is the refusals. Native values are plain 64-bit integers with
nowhere to put the VM's type tag, so arrays, structs, the builtin library and
strings-as-values cannot be compiled -- and each has to be named, because
"cannot compile this" tells you to give up and "arrays" tells you what to take
out.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

# Everything the native backend claims: recursion, a loop with a backward jump,
# nested calls, arithmetic that has to use idiv, comparisons, and each printf
# conversion the emitted runtime implements. The values are chosen so a wrong
# answer is a different answer rather than a coincidentally equal one.
PROG = [
    "int fact(int n) { if (n < 2) return 1; return n * fact(n - 1); }",
    "int euclid(int a, int b) { while (b != 0) { int t = a % b; a = b; b = t; } return a; }",
    "int main() {",
    "    int total = 0;",
    "    for (int i = 1; i <= 10; i = i + 1) total = total + i;",
    '    printf(\\"sum=%d fact=%d gcd=%d\\\\n\\", total, fact(6), euclid(48, 18));',
    '    printf(\\"neg=%d hex=%x chr=%c pct=%%\\\\n\\", 0 - 1234, 48879, 66);',
    '    printf(\\"str=%s div=%d mod=%d\\\\n\\", \\"ok\\", 100 / 7, 100 % 7);',
    '    printf(\\"shift=%d band=%d cmp=%d\\\\n\\", 1 << 10, 12 & 10, 5 > 3);',
    '    puts(\\"done\\");',
    "    return total;",
    "}",
]

ARRAYS = [
    "int main() {",
    "    int a[4];",
    "    a[0] = 1;",
    "    return a[0];",
    "}",
]

STRVAL = [
    "int main() {",
    '    string s = \\"hello\\";',
    '    puts(s);',
    "    return 0;",
    "}",
]


def write_file(vm, path, lines):
    vm.run('write %s "%s"' % (path, lines[0]))
    for l in lines[1:]:
        vm.run('write -a %s "%s"' % (path, l))


def program_output(text, cmd):
    """The lines a program printed, with the shell's own noise taken out.

    Each path echoes the command that started it and wraps the real output in a
    banner, a loader line and an exit line. Comparing those would compare the
    shell and the loader rather than the two compilers.
    """
    out = []
    for line in text.splitlines():
        s = line.strip()
        if not s or s == cmd or s.startswith("bolt:") or s.startswith("[exec]") or \
           s.startswith("[exit]") or s.startswith("cc:") or s.startswith("run:") or \
           s.startswith("    run it with"):
            continue
        out.append(s)
    return out


def main():
    with BoltVM(name="ccnative", extra=["-vga", "none"]) as vm:
        vm.reach_shell(180, "2")
        vm.run("mkdir /n")
        write_file(vm, "/n/p.c", PROG)

        # The oracle: the same source on the backend that has been right for
        # every other test in this directory.
        vm_out = vm.run("cc -q /n/p.c", timeout=120)
        want = program_output(vm_out, "cc -q /n/p.c")
        assert any("sum=55" in l for l in want), \
            "the VM did not run the program at all:\n" + vm_out[-800:]
        print("  ok: the VM runs it   -> %s" % " | ".join(want))

        out = vm.run("cc -S /n/p.c", timeout=120)
        assert "_start" in out and "syscall" in out and "__printf" in out, \
            "-S did not print a program:\n" + out[-600:]
        print("  ok: -S prints the assembly, entry point and runtime included")

        out = vm.run("cc -N /n/p.c -o /n/p", timeout=120)
        assert "native" in out and "-> /n/p" in out, \
            "-N did not report writing a program:\n" + out[-600:]

        run_out = vm.run("run /n/p", timeout=120)
        got = program_output(run_out, "run /n/p")
        assert got == want, (
            "the two backends disagree about the same source\n"
            "  VM     : %s\n  native : %s" % (" | ".join(want), " | ".join(got)))
        print("  ok: native prints -> %s" % " | ".join(got))
        print("  ok: both backends agree, line for line")

        assert "exited 55" in run_out, \
            "main's return value did not become the exit status:\n" + run_out[-500:]
        print("  ok: main's return value is the program's exit status")

        # A refusal has to name what it cannot take.
        write_file(vm, "/n/arr.c", ARRAYS)
        out = vm.run("cc -N /n/arr.c -o /n/arr", timeout=120)
        assert "arrays" in out, "arrays were not named as the reason:\n" + out[-500:]
        assert "-> /n/arr" not in out, "a refused compile still wrote a program:\n" + out[-500:]
        print("  ok: arrays are refused by name, and nothing is written")

        write_file(vm, "/n/sv.c", STRVAL)
        out = vm.run("cc -N /n/sv.c -o /n/sv", timeout=120)
        assert "string" in out, \
            "a string used as a value was not named as the reason:\n" + out[-500:]
        print("  ok: a string used as a value is refused by name")

        # And the program it refused still runs, because -N is an option and
        # not a replacement.
        out = vm.run("cc -q /n/sv.c", timeout=120)
        assert "hello" in out, \
            "the VM should still run what -N would not take:\n" + out[-500:]
        print("  ok: what -N refuses, cc still runs on the VM")

        vm.expect_absent("KERNEL PANIC")

    print("PASS ccnative: C compiled to x86-64 on BoltOS agrees with the VM")
    return 0


if __name__ == "__main__":
    sys.exit(main())
