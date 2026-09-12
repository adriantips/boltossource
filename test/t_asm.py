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
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM

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


def main():
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

    print("PASS asm: x86-64 assembled on BoltOS into a program BoltOS runs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
