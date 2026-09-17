#!/usr/bin/env bash
# ===========================================================================
#  sdk/build.sh  -  link a third-party program for BoltOS.
#
#  Same compiler, same flags, same linker script the OS builds its own userland
#  with, so a program built here is a program the kernel can load. See SDK.md.
#
#      sdk/build.sh -o build/myapp myapp.c [more.c ...]
#
#  Set BOLTOS_MSYS, or CLANG / LLD / NASM, if the toolchain lives somewhere
#  this cannot guess.
# ===========================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

OUT=""
SRCS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -o) [ $# -ge 2 ] || { echo "sdk/build.sh: -o needs a path"; exit 1; }
            OUT="$2"; shift 2 ;;
        -h|--help)
            echo "usage: sdk/build.sh -o OUT FILE.c [FILE.c ...]"
            exit 0 ;;
        *)  SRCS+=("$1"); shift ;;
    esac
done
[ -n "$OUT" ] || { echo "sdk/build.sh: -o OUT is required"; exit 1; }
[ "${#SRCS[@]}" -gt 0 ] || { echo "sdk/build.sh: no source files"; exit 1; }

M="${BOLTOS_MSYS-}"
if [ -z "$M" ]; then
    M=/c/msys64; [ -d "$M" ] || M=/mnt/c/msys64
fi
CLANG="${CLANG-$M/ucrt64/bin/clang.exe}"
LLD="${LLD-$M/ucrt64/bin/ld.lld.exe}"
NASM="${NASM-$M/usr/bin/nasm.exe}"
command -v "$CLANG" >/dev/null 2>&1 || [ -x "$CLANG" ] || CLANG=clang
command -v "$LLD"   >/dev/null 2>&1 || [ -x "$LLD" ]   || LLD=ld.lld
command -v "$NASM"  >/dev/null 2>&1 || [ -x "$NASM" ]  || NASM=nasm

# The userland flags, verbatim from build.sh. -fno-omit-frame-pointer is not
# decoration: it is what lets `core show` walk a backtrace out of a crash.
UCFLAGS=(--target=x86_64-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie
         -mno-red-zone -msse -msse2 -mno-mmx -mno-80387
         -fno-omit-frame-pointer
         -Wall -Wextra -O2 -std=c11 -I"$ROOT/user" -I"$ROOT/include" -c)

# Inside the tree rather than /tmp: the toolchain here is a Windows build,
# and a POSIX temp path is not one it can open.
WORK="${BOLTOS_SDK_WORK-$ROOT/build/sdk}"
mkdir -p "$WORK" "$(dirname "$OUT")"

# The C runtime and library the OS links its own programs against. Built here
# rather than reused out of build/, so this works in a fresh checkout.
#
# kernel/font8x8.c is in the list because sdk/bolt.ps1 has it and this did not,
# which meant the bash half of the SDK could not link any program that drew a
# character -- boltcraft, for one, exactly as its own README told you to build
# it. A graphical program needs text and there is no other way to get a glyph
# in ring 3; 3 KiB of rodata in a console program that never asks for one is
# cheaper than a build flag people have to learn about.
"$NASM" -f elf64 "$ROOT/user/crt0.asm" -o "$WORK/crt0.o"
for c in user/ulibc.c user/stdlib_ext.c user/libm.c libc/string.c libc/string_ext.c \
         kernel/font8x8.c; do
    "$CLANG" "${UCFLAGS[@]}" "$ROOT/$c" -o "$WORK/$(basename "${c%.c}").o"
done

OBJS=()
i=0
for s in "${SRCS[@]}"; do
    o="$WORK/app_$i.o"
    i=$((i + 1))
    "$CLANG" "${UCFLAGS[@]}" "$s" -o "$o"
    OBJS+=("$o")
done

"$LLD" -m elf_x86_64 -T "$ROOT/user/user.ld" -no-pie -o "$OUT" \
       "$WORK/crt0.o" "${OBJS[@]}" \
       "$WORK/ulibc.o" "$WORK/stdlib_ext.o" "$WORK/libm.o" \
       "$WORK/string.o" "$WORK/string_ext.o" "$WORK/font8x8.o"

echo "OK -> $OUT  ($(wc -c < "$OUT") bytes)"
echo "   put it on a machine and run it:"
echo "     download http://host/$(basename "$OUT") /bin/$(basename "$OUT")"
echo "     run $(basename "$OUT")"
