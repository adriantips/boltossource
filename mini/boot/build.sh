#!/usr/bin/env bash
# ===========================================================================
#  Build BoltBoot, BoltOS Mini's own boot firmware for the Orange Pi Zero3.
#
#    bash mini/boot/build.sh [OUTDIR]     -> OUTDIR/boltboot.bin (default build/mini)
#
#  The same clang and ld.lld as the kernel (build/.tools/env, see TV.md).
#  build-mini.sh runs this; mini/tools/mkcard.py puts the result 8 KiB into
#  the card, and `pi.py fel run` hands it to the Pi's boot ROM over USB-C.
# ===========================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
TOOLS="${BOLTOS_TOOLS:-$ROOT/build/.tools/env/bin}"
[ -x "$TOOLS/ld.lld" ] && export PATH="$TOOLS:$PATH"
PYTHON="${BOLTOS_PYTHON:-$TOOLS/python3}"
[ -x "$PYTHON" ] || PYTHON=python3

OUT="${1:-build/mini}"
O="$OUT/boot"
mkdir -p "$O/gen"

ID="$(date '+%Y-%m-%d %H:%M')"
if git rev-parse --short HEAD >/dev/null 2>&1; then
    ID="$ID $(git rev-parse --short HEAD)"
    git diff --quiet 2>/dev/null || ID="$ID+dirty"
fi
printf '#pragma once\n#define BOLT_BUILD_ID "%s"\n' "$ID" > "$O/gen/buildid.h"

# The AArch32 part: the eGON header and the two stubs, a flat 4 KiB.
clang --target=armv7a-none-eabi -mcpu=cortex-a53 -marm -Imini/boot -c mini/boot/start32.S -o "$O/start32.o"
llvm-objcopy -O binary "$O/start32.o" "$O/start32.bin"
[ "$(wc -c < "$O/start32.bin" | tr -d ' ')" = 4096 ] || { echo "ERROR: start32.bin is not 4 KiB"; exit 1; }

# The AArch64 part. No FP/SIMD (nobody has enabled it at EL3) and no
# unaligned accesses (the MMU is off: every access is Device memory).
CF=(--target=aarch64-none-elf -mcpu=cortex-a53 -ffreestanding -fno-pic -fno-pie -fno-stack-protector
    -mgeneral-regs-only -mstrict-align -fno-builtin -Wall -Wextra -Os -std=c11
    -Imini/boot -I"$O/gen" -c)
clang --target=aarch64-none-elf -mcpu=cortex-a53 -Imini/boot -DSTART32_BIN="\"$O/start32.bin\"" \
    -c mini/boot/start0.S -o "$O/start0.o"
OBJS=("$O/start0.o")
for c in mini/boot/boot0.c mini/boot/hw.c mini/boot/clock.c mini/boot/pmic.c mini/boot/dram.c; do
    o="$O/$(basename "${c%.c}").o"
    clang "${CF[@]}" "$c" -o "$o"
    OBJS+=("$o")
done
ld.lld -T mini/boot/boot0.ld -o "$O/boot0.elf" "${OBJS[@]}"
llvm-objcopy -O binary "$O/boot0.elf" "$O/boot0.bin"

"$PYTHON" mini/tools/mkboot.py "$OUT/boltboot.bin" --boot0 "$O/boot0.bin"
