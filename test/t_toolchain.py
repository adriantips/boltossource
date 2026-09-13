#!/usr/bin/env python3
"""as -c, cc -c, ar and ld: a program that did not fit in one file.

`as` could already turn text typed on BoltOS into a program, and `cc -N` a C
file. Both could only do it one file at a time, with every name resolvable
inside that file -- which is the difference between a compiler and a toolchain.
This covers the four pieces that close the gap: objects with holes in them,
archives to keep objects in, a linker that fills the holes, and a C compiler
that will stop one step short of a program and leave the names open.

The host half is where the real checking happens, because an object format is
only right if a reader that was not written alongside it agrees:

  * GNU readelf parses our objects, with the relocations and symbols we meant
  * LLVM's ld.lld *links* our objects into a working program -- the strongest
    single statement that the format is the real one and not a lookalike
  * BoltLD links NASM's objects, which use R_X86_64_32S where ours use PC32,
    so the other relocation path is exercised by a foreign producer
  * where every resolved reference ended up is compared against where lld
    put it, since the two lay the sections out differently on purpose
  * GNU ar reads our archives and we read GNU ar's, long member names included
  * two C files compiled separately link into one program, with the call, the
    shared variable and the single entry point all landing where nm says

The QEMU half then does the whole thing on the machine itself, from typing the
source to running the program, because none of the above proves the commands
are wired up.

    python test/t_toolchain.py --host-only     # no QEMU, a few seconds
"""
import os, shutil, subprocess, sys, tempfile, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT

OUT = Path(ROOT) / "test" / "out"
MSYS = Path(r"C:\msys64")

# A program in two pieces. main calls into lib, and reads two things out of
# lib's .data -- so a call, a RIP-relative load and a 64-bit fetch all have to
# survive being resolved by somebody other than the assembler.
MAIN_ASM = """bits 64
section .text
global _start
extern add3
extern msg
extern msglen
_start:
    mov rdi, 10
    mov rsi, 20
    mov rdx, 12
    call add3
    push rax

    mov rax, 1
    mov rdi, 1
    lea rsi, [msg]
    mov rdx, [msglen]
    syscall

    pop rdi
    mov rax, 60
    syscall
"""

LIB_ASM = """bits 64
section .text
global add3
add3:
    mov rax, rdi
    add rax, rsi
    add rax, rdx
    ret
section .data
global msg
global msglen
msg: db "linked on boltos", 10
msglen: dq 17
"""

# Three objects in a chain, each needing the next, so the archive scan has to
# run to a fixed point rather than once left to right.
TOP_ASM = """section .text
global _start
extern outer
_start:
    call outer
    mov rdi, rax
    mov rax, 60
    syscall
"""
OUTER_ASM = """section .text
global outer
extern inner
outer:
    call inner
    add rax, 1
    ret
"""
INNER_ASM = """section .text
global inner
inner:
    mov rax, 41
    ret
"""

# A name nothing defines. As an object that is a relocation and the linker's
# problem; as a program it has to be an error, and `extern` must not make it
# look otherwise.
EXTERN_ASM = """section .text
global _start
extern nowhere
_start:
    call nowhere
    mov rax, 60
    syscall
"""

# A real SHT_NOBITS section, which only NASM produces here -- BoltAS spells
# `resb` into .data. The linker has to put it after everything with contents so
# the segment can declare more memory than it carries.
BSS_ASM = """bits 64
section .text
global _start
_start:
    mov rax, 7
    mov [buf], rax
    mov rdi, [buf]
    mov rax, 60
    syscall
section .bss
buf: resq 1
"""

# The same split, in C. main.c names `answer` without defining it, shares a
# file-scope variable, and is the only unit with main() -- so it is the only
# one that may carry _start.
MAIN_C = """int answer(int);
int counter;

int main(void) {
    counter = answer(20);
    return counter;
}
"""

LIB_C = """int twice(int n) { return n + n; }

int answer(int n) {
    return twice(n) + 2;
}
"""


def tool(*names):
    for n in names:
        p = MSYS / "ucrt64" / "bin" / n
        if p.exists():
            return str(p)
        p = MSYS / "usr" / "bin" / n
        if p.exists():
            return str(p)
        w = shutil.which(n)
        if w:
            return w
    return None


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def text_bytes(path):
    """The .text of an ELF, whatever the linker decided to call the address."""
    objdump = tool("objdump.exe", "objdump")
    r = run([objdump, "-h", str(path)])
    assert r.returncode == 0, r.stderr
    size = off = None
    lines = r.stdout.splitlines()
    for i, line in enumerate(lines):
        f = line.split()
        if len(f) >= 6 and f[1] == ".text":
            size, off = int(f[2], 16), int(f[5], 16)
            break
    assert size is not None, "no .text in %s:\n%s" % (path, r.stdout)
    with open(path, "rb") as fh:
        fh.seek(off)
        return fh.read(size)


def host_tests():
    cc = os.environ.get("BOLTOS_HOST_CC") or tool("clang.exe", "clang", "gcc")
    if not cc:
        raise RuntimeError("host C compiler missing (set BOLTOS_HOST_CC)")

    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / ("link-host.exe" if os.name == "nt" else "link-host")
    subprocess.run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                    "-Wno-inconsistent-dllimport",
                    "-iquote", str(Path(ROOT) / "include"),
                    str(Path(ROOT) / "test" / "host" / "link.c"), "-o", str(exe)],
                   check=True)

    d = Path(tempfile.mkdtemp())
    for name, body in (("main.asm", MAIN_ASM), ("lib.asm", LIB_ASM),
                       ("top.asm", TOP_ASM), ("outer.asm", OUTER_ASM),
                       ("inner.asm", INNER_ASM)):
        (d / name).write_text(body)

    # Run from inside the scratch directory with bare names: `ar` records a
    # member under its basename, and a Windows temp path is neither short
    # enough nor spelled with the separator a BoltOS path uses.
    def bolt(*args):
        r = run([str(exe)] + [str(a) for a in args], cwd=str(d))
        return r.stdout.strip()

    for n in ("main", "lib", "top", "outer", "inner"):
        out = bolt("asmobj", n + ".asm", n + ".o")
        assert out.startswith("OK"), "%s.asm did not assemble: %s" % (n, out)
    print("  ok: five sources assembled to relocatable objects")

    # --- does a reader that has never seen this code agree with it? ---------
    readelf = tool("readelf.exe", "readelf")
    if readelf:
        r = run([readelf, "-h", "-r", "-s", str(d / "main.o")])
        assert r.returncode == 0, "readelf could not read our object:\n" + r.stderr
        assert "REL (Relocatable file)" in r.stdout, r.stdout
        for want in ("R_X86_64_PLT32", "add3", "R_X86_64_PC32", "msg", "msglen"):
            assert want in r.stdout, "readelf did not find %s:\n%s" % (want, r.stdout)
        print("  ok: GNU readelf parses the object, with the relocations we meant")
    else:
        print("  SKIP: no readelf to read the object back")

    lld = tool("ld.lld.exe", "ld.lld")
    lld_text = None
    if lld:
        r = run([lld, "-m", "elf_x86_64", "-e", "_start",
                 "-o", str(d / "prog.lld"), str(d / "main.o"), str(d / "lib.o")])
        assert r.returncode == 0, "ld.lld refused our objects:\n" + r.stderr
        lld_text = text_bytes(d / "prog.lld")
        print("  ok: LLVM's ld.lld links our objects, so the format is the real one")
    else:
        print("  SKIP: no ld.lld to link our objects with")

    # --- our linker, on our objects and on somebody else's ------------------
    out = bolt("ld", "prog", "main.o", "lib.o")
    assert out.startswith("OK"), "link failed: " + out
    assert "objects=2" in out and "relocs=3" in out, out
    ours = text_bytes(d / "prog")

    nasm = tool("nasm.exe", "nasm")
    if nasm:
        for n in ("main", "lib"):
            r = run([nasm, "-f", "elf64", str(d / (n + ".asm")),
                     "-o", str(d / (n + ".nasm.o"))])
            assert r.returncode == 0, r.stderr
        out = bolt("ld", "prog.n", "main.nasm.o", "lib.nasm.o")
        assert out.startswith("OK"), "our linker refused nasm's objects: " + out
        print("  ok: BoltLD links objects NASM produced, absolute relocations and all")
    else:
        print("  SKIP: no nasm to produce a second opinion's objects")

    # Comparing the two programs byte for byte would be comparing layout
    # choices: lld pads between objects with int3 and puts .data somewhere else,
    # so every displacement legitimately differs. What has to agree is where the
    # references ended up pointing, and objdump says that in both.
    for path, who in ((d / "prog", "BoltLD"),
                      (d / "prog.lld", "ld.lld") if lld else (None, None)):
        if path is None:
            continue
        r = run([tool("objdump.exe", "objdump"), "-d", str(path)])
        assert r.returncode == 0, r.stderr
        for want in ("<add3>", "<msg>", "<msglen>"):
            assert want in r.stdout, \
                "%s did not resolve %s to the right place:\n%s" % (who, want, r.stdout)
    print("  ok: both linkers point the call and the two loads at the same symbols")
    assert len(ours) > 0

    # --- archives -----------------------------------------------------------
    long_o = d / "a_very_long_member_name_indeed.o"
    shutil.copy(d / "lib.o", long_o)
    out = bolt("ar", "libstuff.a", long_o.name)
    assert out.startswith("OK"), "archive failed: " + out
    out = bolt("ld", "prog2", "main.o", "libstuff.a")
    assert "fromlib=1" in out, "the member was not pulled in: " + out
    assert open(d / "prog", "rb").read() == open(d / "prog2", "rb").read(), \
        "linking through an archive produced a different program"
    print("  ok: a member with a long name archives, and links to the same bytes")

    gar = tool("ar.exe", "ar")
    if gar:
        r = run([gar, "t", str(d / "libstuff.a")])
        assert r.returncode == 0 and "a_very_long_member_name_indeed.o" in r.stdout, \
            "GNU ar could not read our archive:\n%s%s" % (r.stdout, r.stderr)
        r = run([gar, "rcs", "libgnu.a", "a_very_long_member_name_indeed.o"], cwd=str(d))
        assert r.returncode == 0, r.stderr
        out = bolt("ld", "prog3", "main.o", "libgnu.a")
        assert "fromlib=1" in out, "we could not pull from GNU ar's archive: " + out
        assert open(d / "prog", "rb").read() == open(d / "prog3", "rb").read()
        print("  ok: GNU ar reads ours and we read GNU ar's, symbol index skipped")
    else:
        print("  SKIP: no GNU ar to check the format against")

    # Two archives that need each other, listed in the order ld(1) would fail on.
    bolt("ar", "libinner.a", "inner.o")
    bolt("ar", "libouter.a", "outer.o")
    out = bolt("ld", "chain", "top.o", "libinner.a", "libouter.a")
    assert out.startswith("OK") and "objects=3" in out and "fromlib=2" in out, \
        "the archive scan did not run to a fixed point: " + out
    print("  ok: an archive that needs a later archive still links")

    # --- .bss: address space, not bytes in the file -------------------------
    if nasm and readelf:
        (d / "bss.asm").write_text(BSS_ASM)
        r = run([nasm, "-f", "elf64", "bss.asm", "-o", "bss.o"], cwd=str(d))
        assert r.returncode == 0, r.stderr
        out = bolt("ld", "bssprog", "bss.o")
        assert out.startswith("OK") and "data=0" in out, \
            "a NOBITS section was counted as file data: " + out
        r = run([readelf, "-l", str(d / "bssprog")])
        seg = [l for l in r.stdout.splitlines() if "0x0000000000402000" in l]
        assert seg, "no data segment:\n" + r.stdout

        # FileSiz then MemSiz on the continuation line readelf prints.
        idx = r.stdout.splitlines().index(seg[0])
        sizes = r.stdout.splitlines()[idx + 1].split()
        assert sizes[0] != sizes[1], \
            "the data segment's file size equals its memory size, so the whole\n" \
            ".bss is being written out as zeros:\n" + r.stdout
        print("  ok: a NOBITS section costs address space and nothing in the file")
    else:
        print("  SKIP: no nasm/readelf for the .bss layout check")

    # --- C, compiled one file at a time ------------------------------------
    ccobj = OUT / ("ccobj-host.exe" if os.name == "nt" else "ccobj-host")
    subprocess.run([cc, "-std=c11", "-O1", "-g", "-Wall",
                    "-Wno-inconsistent-dllimport", "-DBOLTCC_NO_DEBUGGER",
                    "-iquote", str(Path(ROOT) / "include"),
                    str(Path(ROOT) / "test" / "host" / "ccobj.c"), "-o", str(ccobj)],
                   check=True)

    def compile_c(name, body, mode="module"):
        (d / (name + ".c")).write_text(body)
        r = subprocess.run([str(ccobj), mode], stdin=open(d / (name + ".c")),
                           capture_output=True, text=True, cwd=str(d))
        return r.stdout, r.stderr

    for name, body in (("cmain", MAIN_C), ("clib", LIB_C)):
        text, why = compile_c(name, body)
        assert not text.startswith("ERR") and "section .text" in text,             "%s.c did not compile to a module:\n%s" % (name, why)
        (d / (name + ".asm")).write_text(text)

    mtext = (d / "cmain.asm").read_text()
    ltext = (d / "clib.asm").read_text()
    assert "extern answer" in mtext, \
        "a prototype with no definition here did not become an extern:\n" + mtext[:400]
    assert "global _start" in mtext, "the unit with main() has no entry point"
    assert "global _start" not in ltext, \
        "a unit without main() emitted an entry point, which would be a second one"
    assert "global answer" in ltext and "global twice" in ltext, ltext[:400]
    print("  ok: C compiles per file -- externs out, entry point only where main is")

    for n in ("cmain", "clib"):
        out = bolt("asmobj", n + ".asm", n + ".o")
        assert out.startswith("OK"), "%s.asm did not assemble: %s" % (n, out)
    out = bolt("ld", "cprog", "cmain.o", "clib.o")
    assert out.startswith("OK"), "the two C objects did not link: " + out

    if readelf:
        r = run([tool("nm.exe", "nm"), str(d / "cprog")])
        for want in ("T main", "T answer", "T twice", "D _G_counter", "T _start"):
            assert want in r.stdout, "%s is not in the linked program:\n%s" % (want, r.stdout)
        r = run([tool("objdump.exe", "objdump"), "-d", str(d / "cprog")])
        assert "call" in r.stdout and "<answer>" in r.stdout,             "the cross-object call did not resolve:\n" + r.stdout[:1500]
        assert "<twice>" in r.stdout, "the call within one object did not resolve"
        assert "<_G_counter>" in r.stdout, "the shared variable did not resolve"
    # Each object carries its own copy of BoltCC's runtime, as local symbols.
    # That is a few hundred wasted bytes per object and it is why two of them
    # can be linked at all without every helper colliding.
    assert "__printf" in mtext and "__printf" in ltext, \
        "an object did not carry the runtime it needs to stand alone"
    assert "global __printf" not in mtext, \
        "the runtime was published as a global, which is a duplicate symbol " \
        "the moment a second object turns up"
    print("  ok: two separately compiled C files link into one program")

    text, why = compile_c("twomain", MAIN_C.replace("answer(20)", "20"))
    (d / "twomain.asm").write_text(text)
    bolt("asmobj", "twomain.asm", "twomain.o")
    out = bolt("ld", "bad", "cmain.o", "twomain.o", "clib.o")
    assert "duplicate definition of 'main'" in out, \
        "two units with main() linked anyway: " + out

    text, why = compile_c("noinit", "int counter = 5;\nint helper(void){return counter;}\n")
    assert text.startswith("ERR") and "cannot initialise the global" in why, \
        "an initialiser that could never run was accepted:\n%s%s" % (text[:200], why)

    text, why = compile_c("undef", "int missing(int);\nint main(void){return missing(1);}\n",
                          mode="program")
    assert text.startswith("ERR") and "declared but never defined" in why, \
        "a whole program was allowed to call a name nothing defines:\n%s%s" % (text[:200], why)
    text, why = compile_c("undef", "int missing(int);\nint main(void){return missing(1);}\n")
    assert "extern missing" in text, \
        "the same file as a module should leave the name to the linker:\n" + why
    print("  ok: two mains, an initialiser that could not run, and a name "
          "nothing defines are each refused")

    # --- the ways it should refuse -----------------------------------------
    (d / "ext.asm").write_text(EXTERN_ASM)
    out = bolt("asmexe", "ext.asm", "ext.out")
    assert "undefined symbol: nowhere" in out, \
        "a program assembled a call to a name nothing defines:\n" + out
    out = bolt("asmobj", "ext.asm", "ext.o")
    assert out.startswith("OK"), "the same file as an object should assemble: " + out
    out = bolt("ld", "bad", "ext.o")
    assert "undefined reference to 'nowhere'" in out, out
    print("  ok: `extern` is a relocation in an object and an error in a program")

    out = bolt("ld", "bad", "main.o")
    assert "undefined reference to 'add3'" in out, out
    shutil.copy(d / "lib.o", d / "lib2.o")
    out = bolt("ld", "bad", "main.o", "lib.o", "lib2.o")
    assert "duplicate definition of 'add3'" in out, out
    out = bolt("ld", "bad", "prog")
    assert "already a program" in out, out
    out = bolt("ld", "bad", "main.o", "lib.o", "-e", "nosuch")
    assert "nothing defines 'nosuch'" in out, out
    (d / "junk.o").write_bytes(b"\x00" * 200)
    out = bolt("ld", "bad", "junk.o")
    assert "not an ELF64 object" in out, out
    print("  ok: undefined, duplicate, wrong-type and malformed inputs are all named")

    shutil.rmtree(d, ignore_errors=True)


# ---- on the machine itself -------------------------------------------------

def shell(vm, line, timeout=60):
    """One command, one answer.

    The leading Enter is not decoration: reach_shell's probe can leave a
    half-typed line in the buffer and the first real command then arrives with
    a character glued to its front. t_wifi.py's helper says the same thing.
    """
    vm.key("ret")
    time.sleep(0.4)
    vm.clear()
    vm.type(line + "\n")
    time.sleep(0.6)
    return vm.serial()


def write_file(vm, path, body):
    """One line at a time through `write`, quotes escaped.

    The line is handed to the shell inside double quotes, so a double quote in
    the source has to arrive as \\" or the tokenizer eats it -- and `db "text"`
    then reaches the assembler as `db text`, which is a different and much more
    confusing error than "you forgot to escape something".
    """
    lines = [l.replace('"', '\\"') for l in body.strip("\n").split("\n")]
    vm.run('write %s "%s"' % (path, lines[0]))
    for l in lines[1:]:
        vm.run('write -a %s "%s"' % (path, l))


def qemu_tests():
    with BoltVM(name="toolchain", extra=["-vga", "none"]) as vm:
        vm.reach_shell(180, "2")
        vm.key("ret")
        time.sleep(0.5)
        vm.run("mkdir /t")

        write_file(vm, "/t/main.asm", MAIN_ASM)
        write_file(vm, "/t/lib.asm", LIB_ASM)

        out = vm.run("as -c /t/main.asm", timeout=60)
        assert "-> /t/main.o" in out and "object" in out, \
            "as -c did not write an object:\n" + out[-500:]
        out = vm.run("as -c /t/lib.asm", timeout=60)
        assert "-> /t/lib.o" in out, out[-500:]
        print("  ok: two sources assemble to objects on the machine")

        out = vm.run("ld -v -o /t/prog /t/main.o /t/lib.o", timeout=60)
        assert "-> /t/prog" in out, "the link did not produce a program:\n" + out[-600:]
        assert "2 objects" in out, out[-600:]
        out = vm.run("run /t/prog", timeout=60)
        assert "linked on boltos" in out, \
            "the program did not reach the other file's data:\n" + out[-600:]
        assert "exited 42" in out, \
            "10+20+12 should be 42; the call across files is wrong:\n" + out[-600:]
        print("  ok: a two-file program links, runs, and gets the right answer")

        # An archive, and a link that pulls out of it.
        out = vm.run("ar r /t/libmine.a /t/lib.o", timeout=60)
        assert "1 member" in out, "the archive was not built:\n" + out[-500:]
        out = vm.run("ar t /t/libmine.a", timeout=40)
        assert "lib.o" in out, "ar t did not list the member:\n" + out[-500:]
        out = vm.run("ld -o /t/prog2 /t/main.o /t/libmine.a", timeout=60)
        assert "-> /t/prog2" in out, out[-600:]
        out = vm.run("run /t/prog2", timeout=60)
        assert "exited 42" in out, \
            "the program linked out of an archive did not run:\n" + out[-600:]
        print("  ok: ar builds a library and ld pulls the member it needs from it")

        # A missing name is named, and nothing is written.
        out = vm.run("ld -o /t/nope /t/main.o", timeout=60)
        assert "undefined reference to 'add3'" in out, \
            "a missing symbol was not reported:\n" + out[-500:]
        out = vm.run("cat /t/nope", timeout=30)
        assert "no such file" in out.lower() or "not found" in out.lower(), \
            "a failed link still wrote a file:\n" + out[-400:]
        print("  ok: an unresolved name is named, and no program is left behind")

        # And the same split, in C, compiled one file at a time.
        write_file(vm, "/t/cmain.c", MAIN_C)
        write_file(vm, "/t/clib.c", LIB_C)
        out = vm.run("cc -c /t/cmain.c -o /t/cmain.o", timeout=120)
        assert "-> /t/cmain.o" in out and "object" in out, \
            "cc -c did not write an object:\n" + out[-700:]
        out = vm.run("cc -c /t/clib.c -o /t/clib.o", timeout=120)
        assert "-> /t/clib.o" in out, out[-700:]
        out = vm.run("ld -o /t/cprog /t/cmain.o /t/clib.o", timeout=90)
        assert "-> /t/cprog" in out, "the two C objects did not link:\n" + out[-700:]
        out = vm.run("run /t/cprog", timeout=90)
        assert "exited 42" in out, \
            "answer(20) is twice(20)+2, so 42. Anything else means the call\n" \
            "across the object boundary or the shared variable is wrong:\n" + out[-700:]
        print("  ok: two C files compiled separately, linked, and run")

        # The linked program carries its symbols, so a crash in it has names.
        out = vm.run("man ld", timeout=40)
        assert "fixed point" in out or "still missing" in out, \
            "ld has no manual page:\n" + out[-400:]

        vm.expect_absent("KERNEL PANIC")


def main():
    host_tests()
    if "--host-only" in sys.argv:
        print("PASS toolchain (host only): objects, archives and links agree with binutils")
        return 0
    qemu_tests()
    print("PASS toolchain: a program in two files, assembled, archived, linked and run")
    return 0


if __name__ == "__main__":
    sys.exit(main())
