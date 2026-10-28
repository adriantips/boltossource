#!/usr/bin/env bash
# ===========================================================================
#  Rebuild mini/firmware/u-boot-sunxi-with-spl.bin on a Mac: U-Boot 2026.07
#  for the Orange Pi Zero3, with ARM Trusted Firmware 2.15 inside it and
#  uboot-mini.config's changes on top (the DFU window, the watchdog, the
#  boot command). The result is committed, so this is only needed to change
#  the firmware.
#
#    bash mini/firmware/build-uboot.sh
#
#  It downloads the two source tarballs and Arm's aarch64-none-elf GCC into
#  build/mini-fw/, and wants build/.tools/env (TV.md) plus these conda-forge
#  packages in it: make bison flex dtc swig setuptools pyelftools bash.
#
#  Three things macOS needs that Linux does not, all passed on the command
#  line below rather than patched into U-Boot's tree, except the first:
#    - pylibfdt's swig file still calls two Python 2 C functions
#    - Python extensions link with -undefined dynamic_lookup on macOS
#    - U-Boot's host tools rely on C "common" symbols (-fcommon)
# ===========================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
W="$ROOT/build/mini-fw"
mkdir -p "$W/src"
cd "$W"

UBOOT=u-boot-2026.07
TFA=arm-trusted-firmware-2.15.0
GCC=arm-gnu-toolchain-14.3.rel1-darwin-arm64-aarch64-none-elf
GCC_SHA=8636ad0e42acab8227974c8180916b114040f5c77f8b7f5480fe58af4911d484

[ -f src/$UBOOT.tar.bz2 ] || curl -fsSL -o src/$UBOOT.tar.bz2 https://ftp.denx.de/pub/u-boot/$UBOOT.tar.bz2
[ -f src/tfa-2.15.0.tar.gz ] || curl -fsSL -o src/tfa-2.15.0.tar.gz \
    https://github.com/ARM-software/arm-trusted-firmware/archive/refs/tags/v2.15.0.tar.gz
if [ ! -d tc/$GCC ]; then
    [ -f src/gnu-aarch64.tar.xz ] || curl -fsSL -o src/gnu-aarch64.tar.xz \
        https://developer.arm.com/-/media/Files/downloads/gnu/14.3.rel1/binrel/$GCC.tar.xz
    echo "$GCC_SHA  src/gnu-aarch64.tar.xz" | shasum -a 256 -c -
    mkdir -p tc && tar -xf src/gnu-aarch64.tar.xz -C tc
fi
[ -d $UBOOT ] || tar -xjf src/$UBOOT.tar.bz2
[ -d $TFA ] || tar -xzf src/tfa-2.15.0.tar.gz

export PATH="$W/tc/$GCC/bin:$ROOT/build/.tools/env/bin:$PATH"
export CROSS_COMPILE=aarch64-none-elf-

echo "[1/2] TF-A BL31 for sun50i_h616"
gmake -C $TFA -j8 PLAT=sun50i_h616 DEBUG=0 bl31 > tfa-build.log 2>&1

echo "[2/2] U-Boot for orangepi_zero3"
sed -i '' 's/PyInt_AsLong/PyLong_AsLong/g; s/PyString_FromString/PyUnicode_FromString/g' \
    $UBOOT/scripts/dtc/pylibfdt/libfdt.i_shipped
gmake -C $UBOOT O="$W/ub" orangepi_zero3_defconfig > ub-config.log 2>&1
cat "$ROOT/mini/firmware/uboot-mini.config" >> "$W/ub/.config"
gmake -C $UBOOT O="$W/ub" olddefconfig >> ub-config.log 2>&1
gmake -C $UBOOT O="$W/ub" -j8 CONFIG_SHELL="$ROOT/build/.tools/env/bin/bash" \
    HOSTCFLAGS=-fcommon HOSTLDFLAGS="-undefined dynamic_lookup" \
    BL31="$W/$TFA/build/sun50i_h616/release/bl31.bin" SCP=/dev/null < /dev/null > ub-build.log 2>&1

cp "$W/ub/u-boot-sunxi-with-spl.bin" "$ROOT/mini/firmware/"
echo "OK -> mini/firmware/u-boot-sunxi-with-spl.bin ($(wc -c < "$ROOT/mini/firmware/u-boot-sunxi-with-spl.bin" | tr -d ' ') bytes)"
