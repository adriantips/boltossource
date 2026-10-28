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
EXTRA=()
for a in "$@"; do
    case "$a" in
    --headless) DISPLAY_ARGS=(-display none) ;;
    --hvf)      ACCEL=(-accel hvf -cpu host); GIC=3 ;;   # Apple's hypervisor only does GICv3
    --ohci)     EXTRA+=(-device pci-ohci,id=ohci -device usb-hub,bus=ohci.0,port=1
                        -device usb-kbd,bus=ohci.0,port=1.1 -device usb-mouse,bus=ohci.0,port=1.2) ;;
    *)          EXTRA+=("$a") ;;
    esac
done

exec "$QEMU" -M virt,gic-version=$GIC,highmem=off "${ACCEL[@]}" -smp 1 -m 1G \
    -kernel "$KERNEL" \
    -device ramfb "${DISPLAY_ARGS[@]}" \
    -device virtio-keyboard-device -device virtio-tablet-device \
    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:5901-:5900,hostfwd=tcp:127.0.0.1:8080-:80 \
    -device virtio-net-device,netdev=n0 \
    -serial stdio "${EXTRA[@]}"
