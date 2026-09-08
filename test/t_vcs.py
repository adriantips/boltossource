#!/usr/bin/env python3
"""BoltVCS: a project's history, kept on the machine that has the project.

Writing software is mostly trying something, finding out it was wrong, and
getting back what worked. Without version control BoltOS could do the first two
and not the third: `snapshot` copies the whole filesystem, which is not a
project's history -- it cannot say what changed, or why, or put one directory
back without putting everything back.

The round trip checked here is the whole of what that costs to be useful:

    init -> add -> commit -> change a file -> status says so -> diff shows the
    line -> commit again -> log has both -> checkout the first one -> the file
    is what it was

plus a branch, and the refusal to record a commit that changes nothing.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

A_C = [
    'int add(int a, int b) { return a + b; }',
]
B_C = [
    'int main() {',
    '  printf(\\"sum=%d\\\\n\\", add(2, 3));',
    '  return 0;',
    '}',
]


def reach_shell(vm, timeout=180):
    for _ in range(16):
        if "boot mode" in vm.serial():
            break
        vm.key("2")                       # safe mode: a text shell
        time.sleep(0.4)
    vm.expect("BoltShell ready", timeout=timeout)
    vm.wait_prompt()


def write_file(vm, path, lines):
    vm.run('write %s "%s"' % (path, lines[0]))
    for l in lines[1:]:
        vm.run('write -a %s "%s"' % (path, l))


def main():
    with BoltVM(name="vcs", extra=["-vga", "none"]) as vm:
        reach_shell(vm)
        vm.run("mkdir /work")
        write_file(vm, "/work/a.c", A_C)
        write_file(vm, "/work/b.c", B_C)

        out = vm.run("cd /work ; vcs init")
        assert "repository in /work" in out, "init did not make a repository:\n" + out

        out = vm.run("vcs status")
        assert "untracked" in out and "a.c" in out, "new files are not untracked:\n" + out
        print("  ok: init, and an untracked working tree")

        out = vm.run("vcs add .")
        assert "staged 2" in out, "`add .` did not stage both files:\n" + out
        out = vm.run('vcs commit -m "first"')
        assert "[main 0] first" in out, "the first commit was not recorded:\n" + out
        print("  ok: add and commit")

        # --- change a file: status and diff have to notice ------------------
        vm.run('write -a /work/a.c "int twice(int n) { return n * 2; }"')
        out = vm.run("vcs status")
        assert "modified" in out and "a.c" in out, "a changed file was not noticed:\n" + out

        out = vm.run("vcs diff")
        assert "+ int twice" in out, "the diff does not show the added line:\n" + out
        assert "- int add" not in out, "the diff calls an untouched line changed:\n" + out
        print("  ok: status sees the change and diff shows the line, only the line")

        out = vm.run("vcs add . ; vcs commit -m \"second\"")
        assert "[main 1] second" in out, "the second commit was not recorded:\n" + out

        out = vm.run("vcs log")
        assert "commit 1" in out and "commit 0" in out, "log is missing a commit:\n" + out
        assert "second" in out and "first" in out, "log is missing a message:\n" + out
        print("  ok: log walks the history")

        # --- a commit that changes nothing is refused -----------------------
        out = vm.run('vcs commit -m "again"')
        assert "nothing to commit" in out, "an empty commit was allowed:\n" + out
        print("  ok: a commit that changes nothing is refused")

        # --- go back ---------------------------------------------------------
        out = vm.run("vcs checkout 0", timeout=90)
        assert "restored 2 file" in out, "checkout did not restore the commit:\n" + out
        out = vm.run("cat /work/a.c")
        assert "int add" in out, "checkout lost the file it was restoring:\n" + out
        assert "twice" not in out, "checkout did not undo the change:\n" + out
        print("  ok: checkout puts an earlier commit back on disk")

        # --- branches --------------------------------------------------------
        out = vm.run("vcs branch spike")
        assert "branch spike" in out, "the branch was not created:\n" + out
        out = vm.run("vcs branch")
        assert "* spike" in out, "the new branch is not checked out:\n" + out
        print("  ok: a branch starts here and is checked out")

        # --- and it all survives a reboot ------------------------------------
        vm.run("sync")
        vm.clear()               # so reach_shell waits for the NEXT boot menu
        vm.reset()
        reach_shell(vm)
        out = vm.run("cd /work ; vcs log")
        assert "commit 1" in out and "second" in out, "the history did not survive a reboot:\n" + out
        print("  ok: the repository survives a reboot")

        vm.expect_absent("KERNEL PANIC")

    print("PASS vcs: a project's history is kept, inspected and restored on BoltOS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
