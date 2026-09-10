#!/usr/bin/env python3
"""`cc`: a real multi-file C program is written, built and run on BoltOS itself.

This is the "development-ready" claim in its smallest honest form. Before this,
BoltCC could compile exactly one self-contained file: no #include meant no
headers, no headers meant no way to split a program across files, and a language
you cannot split across files is not one you write real software in.

The program built here uses the things that make multi-file C work at all:

    util.h    an include guard, a function-like macro, a function prototype
    util.c    the definition of that function
    main.c    includes the header, calls across to util.c, uses an array

and it is authored from the shell -- which needed quoting to be possible at all,
since every line of C worth writing contains a semicolon.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

HEADER = [
    '#ifndef UTIL_H',
    '#define UTIL_H',
    '#define TWICE(x) ((x) * 2)',
    'int square(int n);',
    'int cube(int n);',
    '#endif',
]
UTIL = [
    '#include \\"util.h\\"',
    'int square(int n) { return n * n; }',
    'int cube(int n) { return n * square(n); }',
]
MAIN = [
    '#include \\"util.h\\"',
    'int main() {',
    '  int a[5];',
    '  for (int i = 0; i < 5; i++) a[i] = square(i);',
    '  int sum = 0;',
    '  for (int i = 0; i < len(a); i++) sum += a[i];',
    '  printf(\\"sum=%d twice=%d cube=%d\\\\n\\", sum, TWICE(21), cube(3));',
    '  return 0;',
    '}',
]


def reach_shell(vm, timeout=180):
    return vm.reach_shell(timeout, "2")


def write_file(vm, path, lines):
    vm.run('write %s "%s"' % (path, lines[0]))
    for l in lines[1:]:
        vm.run('write -a %s "%s"' % (path, l))


def main():
    with BoltVM(name="cc", extra=["-vga", "none"]) as vm:
        reach_shell(vm)
        vm.run("mkdir /proj")

        write_file(vm, "/proj/util.h", HEADER)
        write_file(vm, "/proj/util.c", UTIL)
        write_file(vm, "/proj/main.c", MAIN)

        # Shell quoting has to survive the round trip, or none of the rest can.
        out = vm.run("cat /proj/main.c")
        assert "int a[5];" in out, "the semicolon was lost writing the file:\n" + out
        assert '\\n' in out or "%d" in out, "the printf line did not survive:\n" + out
        print("  ok: C source with semicolons and escapes is authored from the shell")

        # The build.
        out = vm.run("cc /proj/main.c /proj/util.c", timeout=120)
        assert "sum=30" in out, "the program did not produce the right answer:\n" + out
        assert "twice=42" in out, "the function-like macro did not expand:\n" + out
        assert "cube=27" in out, "the cross-file call did not resolve:\n" + out
        print("  ok: two files + a header compile and run (arrays, macro, prototype)")

        # -E shows the preprocessed unit; the guard must keep the header once.
        out = vm.run("cc -E /proj/main.c", timeout=90)
        assert "#ifndef" not in out, "directives survived preprocessing:\n" + out[-600:]
        assert "int square(int n);" in out, "the header was not included:\n" + out[-600:]
        print("  ok: -E prints a preprocessed unit with the directives resolved")

        # A double include must not produce the header twice.
        vm.run('write /proj/twice.c "#include \\"util.h\\""')
        vm.run('write -a /proj/twice.c "#include \\"util.h\\""')
        vm.run('write -a /proj/twice.c "int main() { printf(\\"guard ok\\\\n\\"); return 0; }"')
        out = vm.run("cc /proj/twice.c", timeout=120)
        assert "guard ok" in out, "the include guard did not hold:\n" + out
        print("  ok: an include guard stops a header being pulled in twice")

        # -o writes the combined unit, and it still compiles.
        out = vm.run("cc -o /proj/all.c /proj/main.c /proj/util.c", timeout=120)
        assert "-> /proj/all.c" in out, "cc -o did not report writing the unit:\n" + out
        out = vm.run("cc /proj/all.c", timeout=120)
        assert "sum=30" in out, "the combined unit does not build:\n" + out
        print("  ok: cc -o writes a combined unit that builds on its own")

        # A missing header is an error with a name in it, not a silent failure.
        vm.run('write /proj/bad.c "#include \\"nope.h\\""')
        out = vm.run("cc /proj/bad.c", timeout=90)
        assert "nope.h" in out, "a missing include was not reported clearly:\n" + out
        print("  ok: a missing include is reported by name")

        vm.expect_absent("KERNEL PANIC")

    print("PASS cc: multi-file C is written, built and run on BoltOS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
