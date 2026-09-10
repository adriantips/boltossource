#!/usr/bin/env bash
# Boot BoltOS through UEFI (OVMF) instead of the legacy BIOS chain.
# The firmware auto-runs EFI/BOOT/BOOTX64.EFI from the ESP, which build.sh
# stages in esp/ -- QEMU serves that directory straight as a FAT volume, so
# there is no ESP disk image to keep in sync.
# Extra args are passed through (e.g. -display sdl for a real window).
ROOT="$(cd "$(dirname "$0")" && pwd)"

# QEMU + OVMF discovery is shared with run.sh and the test suite.
. "$(dirname "$0")/qemu-env.sh"

[ -n "$OVMF_CODE" ] || {
    echo "ERROR: no UEFI firmware found next to QEMU."
    echo "       install one (e.g. mingw-w64-ucrt-x86_64-qemu ships"
    echo "       edk2-x86_64-code.fd) or set BOLTOS_OVMF_CODE."
    exit 1
}
[ -d "$ROOT/esp/EFI/BOOT" ] || { echo "ERROR: esp/ not staged -- run build.sh first"; exit 1; }

# OVMF wants a writable variable store; keep a per-run copy out of the repo.
VARS="$ROOT/iso/OVMF_VARS.fd"
if [ -n "$OVMF_VARS" ] && [ ! -f "$VARS" ]; then
    cp "$OVMF_VARS" "$VARS"
fi

ARGS=(-drive "if=pflash,format=raw,unit=0,readonly=on,file=$OVMF_CODE")
[ -f "$VARS" ] && ARGS+=(-drive "if=pflash,format=raw,unit=1,file=$VARS")

exec "$QEMU" \
    "${ARGS[@]}" \
    -drive id=esp,file=fat:rw:"$ROOT/esp",format=raw,if=none \
    -device ide-hd,drive=esp,bus=ide.0,unit=0 \
    -drive id=hdd,file="$ROOT/iso/disk-hdd.img",format=raw,if=none \
    -device ide-hd,drive=hdd,bus=ide.1,unit=0,rotation_rate=7200 \
    -drive id=ssd,file="$ROOT/iso/disk-ssd.img",format=raw,if=none \
    -device ide-hd,drive=ssd,bus=ide.1,unit=1,rotation_rate=1 \
    -drive id=nvm,file="$ROOT/iso/disk-nvme.img",format=raw,if=none \
    -device nvme,drive=nvm,serial=BOLTNVME01 \
    -cpu max \
    -m 4G \
    -smp 4 \
    -rtc base=utc \
    -audiodev none,id=snd0 \
    -device AC97,audiodev=snd0 \
    -device intel-hda,id=hda \
    -device hda-duplex,bus=hda.0,audiodev=snd0 \
    -netdev user,id=net0 \
    -device e1000,netdev=net0 \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-mouse,bus=xhci.0 \
    -serial stdio \
    -no-reboot -no-shutdown \
    "$@"
