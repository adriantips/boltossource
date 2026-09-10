#!/usr/bin/env python3
"""`build`: a project, not a file list.

`cc a.c b.c util.c` was already a multi-file build, but the file list lived on
the command line -- which means it lived in the head of whoever typed it. This
checks the thing that makes a directory of files a project: the list written
down beside the sources, in a `build.txt` naming one or more targets, with its
own include path and macros.

The same resolver (include/ccbuild.h) is what the Code app's Run button uses,
so what passes here is what the IDE builds. Checked:

  * two targets in one project, each with its own sources
  * a target's `define` reaching the preprocessor
  * `-l` listing what would be built, without building it
  * the working directory as the default project
  * a directory with no build file at all -- still a project
  * asking for a target that does not exist fails, and says so
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

HEADER = [
    '#ifndef UTIL_H',
    '#define UTIL_H',
    'int square(int n);',
    'int cube(int n);',
    '#endif',
]
UTIL = [
    '#include \\"util.h\\"',
    'int square(int n) { return n * n; }',
    'int cube(int n) { return n * square(n); }',
]
# GREET comes from the target's `define` line, so it only compiles if the
# build file's macros actually reach the preprocessor.
MAIN = [
    '#include \\"util.h\\"',
    'int main() {',
    '  int sum = 0;',
    '  for (int i = 1; i <= 4; i++) sum += square(i);',
    '  printf(\\"app sum=%d greet=%d\\\\n\\", sum, GREET);',
    '  return 0;',
    '}',
]
TOOL = [
    '#include \\"util.h\\"',
    'int main() {',
    '  printf(\\"tool cube=%d\\\\n\\", cube(3));',
    '  return 0;',
    '}',
]
BUILD = [
    '# two programs sharing one utility file',
    'target app',
    '  src main.c util.c',
    '  define GREET=7',
    'target tool',
    '  src tool.c util.c',
]
SOLO = [
    'int main() { printf(\\"solo ok\\\\n\\"); return 0; }',
]


def reach_shell(vm, timeout=180):
    return vm.reach_shell(timeout, "2")


def write_file(vm, path, lines):
    vm.run('write %s "%s"' % (path, lines[0]))
    for l in lines[1:]:
        vm.run('write -a %s "%s"' % (path, l))


def main():
    with BoltVM(name="project", extra=["-vga", "none"]) as vm:
        reach_shell(vm)
        vm.run("mkdir /proj")
        write_file(vm, "/proj/util.h", HEADER)
        write_file(vm, "/proj/util.c", UTIL)
        write_file(vm, "/proj/main.c", MAIN)
        write_file(vm, "/proj/tool.c", TOOL)
        write_file(vm, "/proj/build.txt", BUILD)

        # --- the default target is the first one declared -------------------
        out = vm.run("build /proj", timeout=120)
        assert "app sum=30" in out, "the first target did not build and run:\n" + out
        assert "greet=7" in out, "the target's `define` never reached the preprocessor:\n" + out
        assert "2 files" in out, "the banner does not report both sources:\n" + out
        print("  ok: `build DIR` builds the first target, with its own macros")

        # --- a second target in the same project ----------------------------
        out = vm.run("build -t tool /proj", timeout=120)
        assert "tool cube=27" in out, "the second target did not build:\n" + out
        assert "app sum" not in out, "the second target dragged in the first's main:\n" + out
        print("  ok: `-t NAME` builds a different target from the same project")

        # --- listing builds nothing -----------------------------------------
        out = vm.run("build -l /proj", timeout=60)
        assert "target app" in out and "target tool" in out, "targets not listed:\n" + out
        assert "main.c" in out and "util.c" in out, "sources not listed:\n" + out
        assert "sum=" not in out, "`-l` ran the program instead of listing it:\n" + out
        print("  ok: `-l` lists the targets and their sources without building")

        # --- the working directory is the default project -------------------
        out = vm.run("cd /proj ; build -q", timeout=120)
        assert "app sum=30" in out, "the cwd was not used as the project:\n" + out
        vm.run("cd /")

        # --- a directory with no build file is still a project --------------
        vm.run("mkdir /solo")
        write_file(vm, "/solo/hello.c", SOLO)
        out = vm.run("build /solo", timeout=120)
        assert "solo ok" in out, "a build-file-less directory did not build:\n" + out
        print("  ok: a directory with no build.txt builds every source in it")

        # --- a target that does not exist fails, and says why ----------------
        out = vm.run("build -t nope /proj", timeout=60)
        assert "nope" in out and "no target" in out, "a bad target name was not reported:\n" + out
        print("  ok: an unknown target is refused with a reason")

        # --- and `cc` still builds a plain file list ------------------------
        out = vm.run("cc /proj/main.c /proj/util.c -DGREET=7", timeout=120)
        assert "app sum=30" in out, "cc regressed on an explicit file list:\n" + out
        print("  ok: `cc FILE...` still builds an explicit list")

        vm.expect_absent("KERNEL PANIC")

    print("PASS project: build.txt targets build from the shell and the IDE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
