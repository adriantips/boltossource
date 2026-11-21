#!/usr/bin/env bash
# ===========================================================================
#  Run BoltOS Mini in QEMU on this Mac: the same kernel the Pi runs, on
#  QEMU's `virt` board. Its screen opens in a window (ramfb), and the VNC
#  server inside is reachable exactly as the Pi's is, on localhost instead:
#
#      bash mini/tools/run-qemu.sh             window + serial log here
#      bash mini/tools/run-qemu.sh --headless  no window; VNC only
#      open vnc://localhost:5901               (password: boltos)
#      http://localhost:8080                   the web page
#
#  -accel hvf runs it on the M-series cores directly; without it (or on an
#  Intel Mac) QEMU emulates the A53, which is slower but fine.
#
#      bash mini/tools/run-qemu.sh --virt      with EL2 and a GICv2 that has
#                                             the virtualisation extensions,
#                                             as on the Pi: Firefox's machine
#                                             (mini/vm.c) runs (emulated, slowly)
# ===========================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/.tools/qemu/bin/qemu-system-aarch64}"
command -v "$QEMU" >/dev/null 2>&1 || [ -x "$QEMU" ] || QEMU=qemu-system-aarch64
KERNEL="$ROOT/build/mini/boltos.bin"
[ -f "$KERNEL" ] || { echo "ERROR: $KERNEL missing -- run: bash build-mini.sh"; exit 1; }

DISPLAY_ARGS=(-display cocoa)
ACCEL=(-accel tcg -cpu cortex-a53)
GIC=2
VIRT=""
EXTRA=()
for a in "$@"; do
    case "$a" in
    --headless) DISPLAY_ARGS=(-display none) ;;
    --hvf)      ACCEL=(-accel hvf -cpu host); GIC=3 ;;   # Apple's hypervisor only does GICv3
    --virt)     VIRT=",virtualization=on"; ACCEL=(-accel tcg,thread=multi -cpu cortex-a53); GIC=2 ;;
    --ohci)     EXTRA+=(-device pci-ohci,id=ohci -device usb-hub,bus=ohci.0,port=1
                        -device usb-kbd,bus=ohci.0,port=1.1 -device usb-mouse,bus=ohci.0,port=1.2) ;;
    *)          EXTRA+=("$a") ;;
    esac
done

# A disk standing in for the Pi's microSD card (sdcard.c, store.c): 2 GiB,
# sparse, with the card image's partition table, so the store lands past it
# as it does on the Pi. It keeps what Packages installs between runs.
CARD="$ROOT/build/mini/qemu-card.img"
# The card's FAT partition (fs.c: /Code, /Notes, /System) comes from the real
# card image, so QEMU's has the same BOLTOS partition the Pi's does; a card
# made before that had an empty partition and gets it now, keeping its store.
if [ -f "$CARD" ] && [ -f "$ROOT/build/mini/sdcard.img" ] && \
   [ "$(dd if="$CARD" bs=512 skip=2048 count=1 2>/dev/null | head -c 90 | tail -c 8)" != "FAT32   " ]; then
    dd if="$ROOT/build/mini/sdcard.img" of="$CARD" bs=1m count=129 conv=notrunc 2>/dev/null
fi
if [ ! -f "$CARD" ] && [ -f "$ROOT/build/mini/sdcard.img" ]; then
    cp "$ROOT/build/mini/sdcard.img" "$CARD"
    python3 -c "import sys; open(sys.argv[1], 'r+b').truncate(2 << 30)" "$CARD"
fi
if [ ! -f "$CARD" ]; then
    python3 - "$CARD" <<'PY'
import struct, sys
with open(sys.argv[1], "wb") as f:
    mbr = bytearray(512)
    mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 0, 0, 0x0C, 0, 0, 0, 2048, 128 * 2048)
    mbr[510:512] = b"\x55\xaa"
    f.write(mbr)
    f.truncate(2 << 30)
PY
fi

exec "$QEMU" -M virt,gic-version=$GIC,highmem=off$VIRT "${ACCEL[@]}" -smp 4 -m 1G \
    -drive if=none,file="$CARD",format=raw,id=card -device virtio-blk-device,drive=card \
    -kernel "$KERNEL" \
    -device ramfb "${DISPLAY_ARGS[@]}" \
    -device virtio-keyboard-device -device virtio-tablet-device \
    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:5901-:5900,hostfwd=tcp:127.0.0.1:8080-:80 \
    -device virtio-net-device,netdev=n0 \
    -serial stdio ${EXTRA[@]+"${EXTRA[@]}"}
