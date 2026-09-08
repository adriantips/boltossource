#!/usr/bin/env python3
"""The debugger: a bug in a BoltOS-built program can be found on BoltOS.

"Build it and run it" is only half of writing software. The other half is
finding out why it did the wrong thing, and until now a BoltCC program that
misbehaved gave you nothing at all: a runtime error said what went wrong and
never where, and there was no way to stop a program and look at it.

Two things are checked here, because they are two different claims:

  * a runtime error names the line it happened on and the call stack that got
    there -- which is what you need when a program crashes on its own
  * `cc -g` stops on a line, prints a local by name and shows a backtrace --
    which is what you need when it does not crash but is still wrong
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

# A program with a genuine bug: the loop runs one past the end of the array.
BUGGY = [
    'int at(int a[], int i) { return a[i]; }',
    'int main() {',
    '  int v[3];',
    '  v[0] = 10; v[1] = 20; v[2] = 30;',
    '  int total = 0;',
    '  for (int i = 0; i <= 3; i++) total += at(v, i);',
    '  printf(\\"total=%d\\\\n\\", total);',
    '  return 0;',
    '}',
]

# A correct program, for stepping through.
GOOD = [
    'int twice(int n) { return n * 2; }',
    'int main() {',
    '  int x = 21;',
    '  int y = twice(x);',
    '  printf(\\"y=%d\\\\n\\", y);',
    '  return 0;',
    '}',
]


def reach_shell(vm, timeout=180):
    for _ in range(16):
        if "boot mode" in vm.serial():
            break
        vm.key("2")
        time.sleep(0.4)
    vm.expect("BoltShell ready", timeout=timeout)
    time.sleep(0.6)


def write_file(vm, path, lines):
    vm.run('write %s "%s"' % (path, lines[0]))
    for l in lines[1:]:
        vm.run('write -a %s "%s"' % (path, l))


def main():
    with BoltVM(name="debug", extra=["-vga", "none"]) as vm:
        reach_shell(vm)
        vm.run("mkdir /d")

        # --- a runtime error says where, and how it got there ---------------
        write_file(vm, "/d/bug.c", BUGGY)
        out = vm.run("cc /d/bug.c", timeout=120)
        assert "out of range" in out, "the out-of-bounds read was not caught:\n" + out
        m = re.search(r"line (\d+):", out)
        assert m, "the runtime error carries no line number:\n" + out
        assert "call stack:" in out, "no call stack after a runtime error:\n" + out
        assert "at()" in out, "the failing function is not named in the stack:\n" + out
        print("  ok: a runtime error reports its line and the call stack")

        # --- the debugger stops, prints a local, and continues ---------------
        write_file(vm, "/d/good.c", GOOD)
        vm.clear()
        vm.type("cc -g /d/good.c\n")
        vm.expect("stopped at line", timeout=90)
        print("  ok: cc -g stops before the first line runs")

        # Step until x has been assigned, then read it back by name.
        found = False
        for _ in range(12):
            vm.clear()
            vm.type("s\n")
            time.sleep(0.8)
            vm.clear()
            vm.type("p x\n")
            time.sleep(0.8)
            if "x = 21" in vm.serial():
                found = True
                break
        assert found, "the debugger never showed x = 21:\n" + vm.serial()[-1200:]
        print("  ok: `p NAME` prints a local by name")

        # A backtrace names the function we are in.
        vm.clear()
        vm.type("bt\n")
        time.sleep(1.0)
        assert "main()" in vm.serial(), "backtrace does not name main:\n" + vm.serial()[-600:]
        print("  ok: `bt` shows the call stack")

        # `locals` lists the frame, and `c` runs to completion.
        vm.clear()
        vm.type("locals\n")
        time.sleep(1.0)
        assert "x" in vm.serial(), "`locals` listed nothing:\n" + vm.serial()[-600:]

        vm.clear()
        vm.type("c\n")
        got = False
        for _ in range(20):
            time.sleep(1.0)
            if "y=42" in vm.serial():
                got = True
                break
        assert got, "the program did not finish after `c`:\n" + vm.serial()[-800:]
        print("  ok: `locals` lists the frame and `c` runs to completion")

        vm.expect_absent("KERNEL PANIC")

    print("PASS debug: runtime errors are located, and cc -g debugs a program")
    return 0


if __name__ == "__main__":
    sys.exit(main())
