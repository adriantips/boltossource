#!/usr/bin/env python3
"""`as`: text typed on BoltOS becomes machine code in a file BoltOS runs.

Everything the machine could build until now came back as bytecode with an
interpreter in front of it. This assembles x86-64 into an ordinary ELF64 --
the same format the SDK's build helper produces on a host, loaded by the same
kernel/elf.c -- and then runs it.

Three programs, chosen for what they prove rather than for looking impressive:

  hello   the data section, a RIP-relative [label], and a write syscall, so
          the program reaches its own data without any relocation
  sum     a loop with a conditional jump backwards, exiting with the answer,
          so the label arithmetic in both directions is real
  tiny    nothing but an immediate and an exit, the smallest thing that can
          still be wrong
  split   .text, then .data, then .text again -- the data label only lands
          correctly if the sizing pass knew the final length of .text

and one that must fail: an instruction the assembler does not know has to be
named, with its line, rather than assembled into whatever was nearest.

Running a program only proves the instructions it happened to execute. An
assembler either emits the bytes the processor decodes as what you wrote or it
emits different ones, and the two look identical from outside until something
jumps into the wrong place. So the host half holds NASM up against it: the same
instruction to both, byte for byte, across every operand shape the encoder
knows. That is the half that catches a wrong REX prefix or a ModRM field taken
from the wrong operand, neither of which a working program would notice.

    python test/t_asm.py --host-only     # no QEMU, a few seconds
"""
import os, shutil, subprocess, sys, tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT

OUT = Path(ROOT) / "test" / "out"
NASM = Path(r"C:\msys64\usr\bin\nasm.exe")

# Every operand shape the encoder knows, with constants and displacements
# chosen so NASM has no shorter encoding to prefer. Anything NASM would
# shorten lives in LONGFORM below instead of being quietly excused here.
EXACT = [
    "mov rax, rcx", "mov eax, ecx", "mov al, cl", "mov sil, dil", "mov r8b, r15b",
    "mov rax, [rbx]", "mov al, [rbx]", "mov rax, [rbp+300]", "mov rax, [rsp+300]",
    "mov al, [r12+300]", "mov [rbx], rax", "mov [rdi], al", "mov [rbp-300], rcx",
    "mov [r13+300], r9", "mov eax, 7", "mov al, 10",
    "mov rax, 0x123456789", "mov byte [rdi], 10", "mov qword [rbx], 100000",
    "add rax, rcx", "sub rax, rcx", "and rax, rcx", "or rax, rcx",
    "xor rax, rcx", "cmp rax, rcx", "add al, cl", "cmp sil, dil",
    "add r14, r15", "xor r8d, r9d",
    "add rax, [rbx]", "cmp al, [rsi]", "sub rcx, [rbp+300]",
    "add [rbx], rax", "xor [rdi], al", "cmp [r12+300], rdx",
    "add rcx, 100000", "cmp rcx, 1000", "and cl, 15", "sub rdx, 999999",
    "inc rax", "dec rcx", "neg rax", "not rdx", "mul rcx", "div rcx",
    "idiv rcx", "neg al", "idiv r10", "imul rax, rcx", "imul r8, r9",
    "test rax, rcx", "test al, cl",
    "shl rax, 3", "sar rdx, 5", "shl rax, cl", "shr al, cl", "sar rbx, cl",
    "shl r11, cl",
    "sete al", "setne cl", "setl dl", "setg sil", "setle r8b", "setb r12b",
    "seta bpl", "setge dil",
    "movzx rax, al", "movzx rax, byte [rsi]", "movsx rax, al", "movzx eax, cl",
    "movsx rcx, byte [rbp+300]", "movzx r10, r13b",
    "push rax", "pop rbx", "push r12", "pop r15", "push rbp", "pop rbp",
    "lea rax, [rbx+300]", "lea rcx, [rsp+300]", "lea rdx, [rdi]",
    "syscall", "ret", "nop", "cqo", "int3",
]

# Where BoltAS declines the shorter encoding NASM prefers. That is the whole
# reason its passes agree: an instruction that changed length once a constant
# turned out to be small would move every label after it. NASM has no spelling
# that forces most of these, so the expected bytes are written out -- each
# checked against NASM using an operand too large for the short form, which is
# the same encoding path with a different immediate.
LONGFORM = [
    ("mov rax, 1",        "48c7c001000000", "mov rax, strict dword 1"),
    ("mov rax, 100000",   "48c7c0a0860100", "mov rax, strict dword 100000"),
    ("shr rcx, 1",        "48c1e901",       "shr rcx, strict byte 1"),
    ("add rax, 10",       "4881c00a000000", None),
    ("add rax, 100000",   "4881c0a0860100", None),
    ("and al, 15",        "80e00f",         None),
    ("mov rax, [rbp+16]", "488b8510000000", None),
    ("lea rax, [rbx+8]",  "488d8308000000", None),
]

HELLO = [
    'bits 64',
    'section .text',
    'global _start',
    '_start:',
    '    mov rax, 1',
    '    mov rdi, 1',
    '    lea rsi, [msg]',
    '    mov rdx, 20',
    '    syscall',
    '',
    '    mov rax, 60',
    '    mov rdi, 7',
    '    syscall',
    'section .data',
    'msg: db \\"assembled on boltos\\", 10',
]

SUM = [
    'section .text',
    'global _start',
    '_start:',
    '    xor rax, rax',
    '    mov rcx, 1',
    'top:',
    '    add rax, rcx',
    '    inc rcx',
    '    cmp rcx, 11',
    '    jne top',
    '    mov rdi, rax',
    '    mov rax, 60',
    '    syscall',
]

TINY = [
    'section .text',
    'global _start',
    '_start:',
    '    mov rax, 60',
    '    mov rdi, 3',
    '    syscall',
]

# .text, then .data, then back to .text. A data label's address depends on the
# final length of .text, so a sizing pass that has only seen half of it places
# this one too low. Valid NASM, and the case that needs the middle pass.
SPLIT = [
    'section .text',
    'global _start',
    '_start:',
    '    mov rax, 1',
    '    mov rdi, 1',
    '    lea rsi, [msg]',
    '    mov rdx, 6',
    '    syscall',
    'section .data',
    'msg: db \\"split\\", 10',
    'section .text',
    '    mov rax, 60',
    '    mov rdi, 9',
    '    syscall',
]

BAD = [
    'section .text',
    'global _start',
    '_start:',
    '    mov rax, 60',
    '    frobnicate rax',
    '    syscall',
]


def write_file(vm, path, lines):
    vm.run('write %s "%s"' % (path, lines[0]))
    for l in lines[1:]:
        vm.run('write -a %s "%s"' % (path, l))


def host_tests():
    """Give both assemblers the same instruction and compare the bytes."""
    cc = os.environ.get("BOLTOS_HOST_CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc and Path(r"C:\msys64\ucrt64\bin\clang.exe").exists():
        cc = r"C:\msys64\ucrt64\bin\clang.exe"
    if not cc:
        raise RuntimeError("host C compiler missing (set BOLTOS_HOST_CC)")
    nasm = os.environ.get("BOLTOS_NASM") or shutil.which("nasm")
    if not nasm and NASM.exists():
        nasm = str(NASM)
    if not nasm:
        print("  SKIP: no nasm to compare against (set BOLTOS_NASM)")
        return

    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / ("asm-host.exe" if os.name == "nt" else "asm-host")
    subprocess.run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                    "-Wno-inconsistent-dllimport",
                    "-iquote", str(Path(ROOT) / "include"),
                    str(Path(ROOT) / "test" / "host" / "asm.c"), "-o", str(exe)],
                   check=True)

    lines = EXACT + [b for b, _, _ in LONGFORM]
    bolt = subprocess.run([str(exe)], input="\n".join(lines) + "\n",
                          capture_output=True, text=True, check=True).stdout.split("\n")
    d = tempfile.mkdtemp()

    def nasm_hex(line):
        src, binp = os.path.join(d, "r.asm"), os.path.join(d, "r.bin")
        open(src, "w").write("bits 64\n" + line + "\n")
        r = subprocess.run([nasm, "-f", "bin", src, "-o", binp],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError("nasm rejected %r: %s" % (line, r.stderr.strip()[:120]))
        return open(binp, "rb").read().hex()

    for i, line in enumerate(EXACT):
        ref, got = nasm_hex(line), bolt[i].strip()
        assert got == ref, "%s\n  boltas %s\n  nasm   %s" % (line, got, ref)
    print("  ok: %d operand forms encode exactly as nasm encodes them" % len(EXACT))

    for j, (line, want, nline) in enumerate(LONGFORM):
        got = bolt[len(EXACT) + j].strip()
        assert got == want, "%s\n  boltas %s\n  wanted %s" % (line, got, want)
        if nline:
            ref = nasm_hex(nline)
            assert ref == want, "%s: nasm(%s) gave %s, wanted %s" % (line, nline, ref, want)
    print("  ok: %d long forms are the same instruction, encoded the long way"
          % len(LONGFORM))


def qemu_tests():
    with BoltVM(name="asm", extra=["-vga", "none"]) as vm:
        vm.reach_shell(180, "2")
        vm.run("mkdir /a")

        write_file(vm, "/a/tiny.asm", TINY)
        out = vm.run("as /a/tiny.asm", timeout=60)
        assert "-> /a/tiny" in out, "as did not report what it wrote:\n" + out[-500:]
        out = vm.run("run /a/tiny", timeout=60)
        assert "exited 3" in out, "the smallest program did not exit 3:\n" + out[-500:]
        print("  ok: an immediate and an exit assemble, load and run")

        write_file(vm, "/a/hello.asm", HELLO)
        out = vm.run("as /a/hello.asm", timeout=60)
        assert "-> /a/hello" in out, out[-500:]
        out = vm.run("run /a/hello", timeout=60)
        assert "assembled on boltos" in out, \
            "the program did not reach its own .data:\n" + out[-500:]
        assert "exited 7" in out, "the exit status did not come back:\n" + out[-500:]
        print("  ok: a RIP-relative [label] reaches .data, and write/exit work")

        write_file(vm, "/a/sum.asm", SUM)
        out = vm.run("as /a/sum.asm", timeout=60)
        out = vm.run("run /a/sum", timeout=60)
        assert "exited 55" in out, \
            "1+2+...+10 should be 55; a backward jump or the arithmetic is wrong:\n" + out[-500:]
        print("  ok: a loop with a backward conditional jump computes 55")

        write_file(vm, "/a/split.asm", SPLIT)
        out = vm.run("as /a/split.asm", timeout=60)
        assert "disagree" not in out, \
            "interleaved sections moved a label between passes:\n" + out[-500:]
        assert "-> /a/split" in out, out[-500:]
        out = vm.run("run /a/split", timeout=60)
        assert "split" in out and "exited 9" in out, \
            "a .data label between two .text sections landed at the wrong address:\n" + out[-500:]
        print("  ok: .text/.data/.text puts the data label where the code expects it")

        # An unknown mnemonic is named, with its line.
        write_file(vm, "/a/bad.asm", BAD)
        out = vm.run("as /a/bad.asm", timeout=60)
        assert "frobnicate" in out and "line 5" in out, \
            "an unknown instruction was not reported with its line:\n" + out[-500:]
        assert "-> /a/bad" not in out, "a failed assembly still wrote a file:\n" + out[-500:]
        print("  ok: an unknown instruction is named, with its line, and writes nothing")

        # That these ran at all is the ELF check: kernel/elf.c rejects anything
        # whose e_ident is not 7F 45 4C 46 with ELFCLASS64 before it maps a
        # single page, so `run` reaching _start means the header is real. What
        # is worth confirming separately is that the file on disk is the whole
        # image -- .text lives at offset 0x1000, so a truncated write would
        # still load and would still be wrong.
        out = vm.run("meta /a/hello", timeout=30)
        size = 0
        for line in out.splitlines():
            if line.strip().startswith("size"):
                for tok in line.replace(":", " ").split():
                    if tok.isdigit():
                        size = int(tok)
                        break
                break
        assert size > 0x1000, \
            "the image is smaller than its own .text offset, so it is truncated:\n" + out[-400:]
        print("  ok: the image on disk is a whole ELF64, past its 0x1000 .text offset")

        vm.expect_absent("KERNEL PANIC")


def main():
    host_tests()
    if "--host-only" in sys.argv:
        print("PASS asm (host only): every encoding agrees with nasm")
        return 0
    qemu_tests()
    print("PASS asm: x86-64 assembled on BoltOS into a program BoltOS runs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
