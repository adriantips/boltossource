#!/usr/bin/env python3
"""make: the decision, not the build.

What is worth checking about a build tool is whether it runs a recipe it should
have run and skips one it should not have. Running it is the easy half.

The host half drives kernel/make.c over a filesystem of 64 files in an array
and a clock that is a variable, so every case is staged exactly and runs in a
second. One of those cases cannot be staged in QEMU at all without rebooting,
and it is the reason this make compares stamps rather than ages: file times on
BoltOS are uptime ticks and they are persisted, so after a reboot a freshly
edited source carries a *smaller* number than the object built from it last
week. Every timestamp-comparing make decides that is up to date and silently
skips it. This one is asked to prove it does not.

The QEMU half then builds a real two-file program with real `as -c` and `ld`
recipes, which is the thing a person would actually type.

    python test/t_make.py --host-only     # no QEMU, a second
"""
import os, shutil, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT

OUT = Path(ROOT) / "test" / "out"
MSYS = Path(r"C:\msys64")


def tool(*names):
    for n in names:
        for sub in ("ucrt64", "usr"):
            p = MSYS / sub / "bin" / n
            if p.exists():
                return str(p)
        w = shutil.which(n)
        if w:
            return w
    return None


def drive(exe, script):
    r = subprocess.run([str(exe)], input=script, capture_output=True, text=True)
    assert r.returncode == 0, "the harness died:\n" + r.stderr
    return r.stdout


def ran(out, needle):
    return any(l.startswith("RUN ") and needle in l for l in out.splitlines())


def host_tests():
    cc = os.environ.get("BOLTOS_HOST_CC") or tool("clang.exe", "clang", "gcc")
    if not cc:
        raise RuntimeError("host C compiler missing (set BOLTOS_HOST_CC)")
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / ("make-host.exe" if os.name == "nt" else "make-host")
    subprocess.run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                    "-Wno-inconsistent-dllimport",
                    "-iquote", str(Path(ROOT) / "include"),
                    str(Path(ROOT) / "test" / "host" / "make.c"), "-o", str(exe)],
                   check=True)

    simple = ("file a.txt alpha\n"
              "file b.txt beta\n"
              "file Makefile CC = gen\\nall: out.txt\\n"
              "out.txt: a.txt b.txt\\n\\t$(CC) $@ $^\\n\n")

    out = drive(exe, simple + "make\ncat out.txt\n")
    assert ran(out, "gen out.txt a.txt b.txt"), out
    assert "CAT out.txt alphabeta" in out, "$@ and $^ did not expand:\n" + out
    print("  ok: a rule builds, with $(VAR), $@ and $^ expanded")

    out = drive(exe, simple + "make\nmake\n")
    assert out.count("RUN gen") == 1, "it built twice with nothing changed:\n" + out
    assert "nothing to be done" in out, out
    print("  ok: a second run with nothing changed runs no recipe")

    out = drive(exe, simple + "make\nfile a.txt ALPHA\nmake\ncat out.txt\n")
    assert out.count("RUN gen") == 2, "an edited prerequisite did not rebuild:\n" + out
    assert "CAT out.txt ALPHAbeta" in out, out
    print("  ok: an edited prerequisite rebuilds what depends on it")

    # A recipe indented with spaces. make(1) says "missing separator" here;
    # this one accepts it, because `write` takes no escapes and the shell reads
    # a typed Tab as a completion request, so a makefile authored on the
    # machine cannot contain a tab at all.
    spaces = ("file a.txt alpha\n"
              "file Makefile all: out\\nout: a.txt\\n    gen $@ $<\\n\n"
              "make\ncat out\n")
    out = drive(exe, spaces)
    assert ran(out, "gen out a.txt"), "a space-indented recipe was not seen:\n" + out
    assert "CAT out alpha" in out, out
    print("  ok: a recipe indented with spaces is a recipe, not a missing separator")

    # The case this design exists for. `tick 5` is a reboot: the clock restarts
    # while the files keep the large mtimes they were persisted with.
    out = drive(exe, simple + "make\ntick 5\nmake\n")
    assert out.count("RUN gen") == 1, \
        "a reboot alone caused a rebuild, so the stamps are not surviving it:\n" + out
    out = drive(exe, simple + "make\ntick 5\nfile a.txt OMEGA\nmake\ncat out.txt\n")
    assert out.count("RUN gen") == 2, \
        "a source edited AFTER a reboot was skipped -- its mtime is now smaller\n" \
        "than the target's, which is exactly the trap:\n" + out
    assert "CAT out.txt OMEGAbeta" in out, out
    print("  ok: across a reboot, an untouched tree rests and an edited one rebuilds")

    proj = ("file main.src MAIN\nfile lib.src LIB\nfile extra.src EXTRA\n"
            "file Makefile OBJS = main.obj lib.obj\\nOBJS += extra.obj\\n"
            ".PHONY: all clean\\nall: prog\\nprog: $(OBJS)\\n\\tgen $@ $^\\n"
            "%.obj: %.src\\n\\tgen $@ $<\\nclean:\\n\\tgen cleaned main.src\\n\n")

    out = drive(exe, proj + "make\ncat prog\n")
    for o in ("main.obj main.src", "lib.obj lib.src", "extra.obj extra.src"):
        assert ran(out, "gen " + o), "the pattern rule did not fire for %s:\n%s" % (o, out)
    assert "CAT prog MAINLIBEXTRA" in out, out
    print("  ok: a pattern rule with a stem, and += on a variable")

    out = drive(exe, proj + "make\nfile lib.src LIBv2\nmake\ncat prog\n")
    tail = out.split("FILE lib.src")[-1]   # after the edit, not after the setup
    assert ran(tail, "gen lib.obj"), "the changed source was not recompiled:\n" + tail
    assert not ran(tail, "gen main.obj"), \
        "an unchanged source was recompiled anyway:\n" + tail
    assert ran(tail, "gen prog"), "the program was not relinked:\n" + tail
    assert "CAT prog MAINLIBv2EXTRA" in out, out
    print("  ok: one changed source recompiles one object and relinks, no more")

    out = drive(exe, proj + "make\nmake clean\nmake clean\n")
    assert out.count("RUN gen cleaned") == 2, \
        "a .PHONY target did not run every time:\n" + out
    print("  ok: a .PHONY target runs every time it is asked for")

    # `a b: dep` really is one recipe for two targets.
    out = drive(exe, "file dep x\n"
                     "file Makefile all: a b\\na b: dep\\n    gen $@ dep\\n\n"
                     "make\ncat a\ncat b\n")
    assert "CAT a x" in out and "CAT b x" in out, \
        "two targets on one line did not both get the recipe:\n" + out

    # And two rules that merely look alike are not. The first version of this
    # worked out the sharing afterwards by comparing prerequisite lists, which
    # cannot tell these apart -- `all` inherited install's recipe and `make`
    # with no arguments installed things.
    out = drive(exe, "file prog x\n"
                     "file Makefile all: prog\\ninstall: prog\\n    gen installed prog\\n\n"
                     "make\ncat installed\n")
    assert "CAT installed <missing>" in out, \
        "the default goal ran a later rule's recipe:\n" + out
    print("  ok: one line means one shared recipe; two lines mean two rules")

    fail = ("file a.txt a\nfile Makefile all: out\\nout: a.txt\\n\\tfail\\n\n"
            "make\ncat out\n")
    out = drive(exe, fail)
    assert "*** [out] Error 2" in out and "DONE -1" in out, out
    assert "CAT out <missing>" in out, "a failed recipe still left a target:\n" + out

    out = drive(exe, "file Makefile all: missing.thing\\n\nmake\n")
    assert "no rule to make target 'missing.thing'" in out, out

    out = drive(exe, "file Makefile a: b\\n\\tgen $@ b\\nb: a\\n\\tgen $@ a\\n\nmake\n")
    assert "circular dependency" in out, \
        "a cycle was not reported as one:\n" + out
    print("  ok: a failing recipe, a missing rule and a cycle are each named")


# ---- and on the machine ----------------------------------------------------

MAIN_ASM = """bits 64
section .text
global _start
extern answer
_start:
    call answer
    mov rdi, rax
    mov rax, 60
    syscall
"""

LIB_ASM = """bits 64
section .text
global answer
answer:
    mov rax, 42
    ret
"""

MAKEFILE = """OBJS = main.o lib.o

prog: $(OBJS)
    ld -o $@ $^

%.o: %.asm
    as -c $< -o $@

.PHONY: clean
clean:
    rm prog
"""


def write_file(vm, path, body):
    lines = body.strip("\n").split("\n")
    vm.run('write %s "%s"' % (path, lines[0]))
    for l in lines[1:]:
        vm.run('write -a %s "%s"' % (path, l))


def qemu_tests():
    with BoltVM(name="make", extra=["-vga", "none"]) as vm:
        vm.reach_shell(180, "2")
        vm.key("ret")
        time.sleep(0.5)
        vm.run("mkdir /p")
        vm.run("cd /p")

        write_file(vm, "/p/main.asm", MAIN_ASM)
        write_file(vm, "/p/lib.asm", LIB_ASM)

        # The recipes are indented with spaces, not a tab: `write` takes no
        # escapes and a typed Tab is a completion request, so a tab cannot get
        # into a file from here at all. kernel/make.c accepts either.
        for line in MAKEFILE.strip("\n").split("\n"):
            vm.run('write -a /p/Makefile "%s"' % line)

        out = vm.run("make", timeout=120)
        assert "as -c main.asm -o main.o" in out, \
            "the pattern rule did not produce the assembler command:\n" + out[-800:]
        assert "ld -o prog main.o lib.o" in out, \
            "the link command did not come out of $@ and $^:\n" + out[-800:]
        out2 = vm.run("run /p/prog", timeout=60)
        assert "exited 42" in out2, \
            "the program make built did not run:\n" + out2[-600:]
        print("  ok: make drives as and ld into a working two-file program")

        out = vm.run("make", timeout=90)
        assert "nothing to be done" in out, \
            "a second make rebuilt something:\n" + out[-600:]
        print("  ok: a second make with nothing changed does nothing")

        vm.run('write /p/lib.asm "bits 64"')
        for l in ["section .text", "global answer", "answer:", "    mov rax, 7", "    ret"]:
            vm.run('write -a /p/lib.asm "%s"' % l)
        out = vm.run("make", timeout=120)
        assert "as -c lib.asm" in out, "the edited source was not reassembled:\n" + out[-800:]
        assert "as -c main.asm" not in out, \
            "an untouched source was reassembled anyway:\n" + out[-800:]
        out2 = vm.run("run /p/prog", timeout=60)
        assert "exited 7" in out2, "the rebuilt program is the old one:\n" + out2[-600:]
        print("  ok: editing one file reassembles that one and relinks")

        out = vm.run("make -n clean", timeout=60)
        assert "rm prog" in out, "-n did not print the recipe:\n" + out[-500:]
        out = vm.run("cat /p/prog | count", timeout=60)
        assert "no such file" not in out.lower(), "-n actually ran the recipe:\n" + out[-500:]
        print("  ok: -n prints a recipe without running it")

        vm.expect_absent("KERNEL PANIC")


def main():
    host_tests()
    if "--host-only" in sys.argv:
        print("PASS make (host only): every build decision is the right one")
        return 0
    qemu_tests()
    print("PASS make: a project built by rules, on the machine, from `make`")
    return 0


if __name__ == "__main__":
    sys.exit(main())
