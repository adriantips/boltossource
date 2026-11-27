#!/usr/bin/env bash
# ===========================================================================
#  Write the BoltOS Mini card image to a microSD card in a reader on this Mac.
#
#    bash mini/tools/flash-card.sh disk4            (see: diskutil list external)
#    bash mini/tools/flash-card.sh disk4 path/to/sdcard.img
#    bash mini/tools/flash-card.sh --boot-only disk4 [boot image]
#
#  --boot-only writes just the boot area -- the firmware 8 KiB into the card,
#  BoltBoot (build/mini/boltboot.bin, the default) or a U-Boot image -- and
#  leaves the FAT partition at 1 MiB, with every file on it, as it is. That
#  is the way back if the Pi stops starting after new firmware. A whole card
#  image works too: only its boot area is taken.
#
#  It refuses anything that is not an external disk, shows what it is about to
#  erase, and asks before writing. The write itself needs your Mac password
#  (sudo): macOS lets only root write to a raw disk.
# ===========================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BOOT_ONLY=0
if [ "${1:-}" = --boot-only ]; then BOOT_ONLY=1; shift; fi
DISK="${1:-}"
IMG="${2:-$ROOT/build/mini/sdcard.img}"
[ "$BOOT_ONLY" = 1 ] && IMG="${2:-$ROOT/build/mini/boltboot.bin}"

if [ -z "$DISK" ]; then
    echo "usage: bash mini/tools/flash-card.sh [--boot-only] diskN [image]"
    echo; diskutil list external; exit 1
fi
DISK="${DISK#/dev/}"
[ -f "$IMG" ] || { echo "ERROR: $IMG not found -- run: bash build-mini.sh"; exit 1; }

# `diskutil list external` names only disks macOS sees as external and
# physical, which is exactly the set this may write to. (`diskutil info` would
# say more, but it can hang for minutes on some card readers.)
LIST="$(diskutil list external)"
HEAD="$(echo "$LIST" | grep -E "^/dev/$DISK \\(external, physical\\):" || true)"
[ -n "$HEAD" ] || { echo "ERROR: $DISK is not an external physical disk:"; echo "$LIST"; exit 1; }
case "$DISK" in disk0|disk1|disk2|disk3) echo "ERROR: $DISK is a system disk. Refusing."; exit 1;; esac

if [ "$BOOT_ONLY" = 1 ]; then
    # A boot image starts with the eGON header; a card image has it 8 KiB in.
    if [ "$(head -c 12 "$IMG" | tail -c 8)" = "eGON.BT0" ]; then SKIP=0
    elif [ "$(dd if="$IMG" bs=512 skip=16 count=1 2>/dev/null | head -c 12 | tail -c 8)" = "eGON.BT0" ]; then SKIP=16
    else echo "ERROR: $IMG has no eGON boot header"; exit 1; fi
    SECS=$(( ($(wc -c < "$IMG") + 511) / 512 - SKIP ))
    [ "$SECS" -le 2032 ] || SECS=2032
    echo "About to write the boot area (8 KiB in, $SECS sectors) of /dev/$DISK; its files stay:"
    echo "$LIST" | sed -n "/^\/dev\/$DISK /,/^$/p" | sed "s/^/    /"
    echo "    from: $IMG"
    if [ "${BOLTOS_YES:-}" != "1" ]; then
        read -r -p "Type yes to continue: " ok
        [ "$ok" = "yes" ] || { echo "Stopped; nothing was written."; exit 1; }
    fi
    diskutil unmountDisk "/dev/$DISK" >/dev/null
    echo "Writing (your Mac password is for sudo, to write the raw disk)..."
    TMP="$(mktemp)"
    dd if="$IMG" of="$TMP" bs=512 skip="$SKIP" count="$SECS" conv=sync 2>/dev/null
    sudo dd if="$TMP" of="/dev/r$DISK" bs=512 seek=16 conv=notrunc
    rm -f "$TMP"
    sync
    diskutil eject "/dev/$DISK" >/dev/null && echo "Done. The card is ejected: put it in the Pi."
    exit 0
fi

echo "About to ERASE /dev/$DISK and write BoltOS Mini to it:"
echo "$LIST" | sed -n "/^\/dev\/$DISK /,/^$/p" | sed "s/^/    /"
echo "    image: $IMG ($(( $(wc -c < "$IMG") >> 20 )) MiB)"
if [ "${BOLTOS_YES:-}" != "1" ]; then
    read -r -p "Type yes to continue: " ok
    [ "$ok" = "yes" ] || { echo "Stopped; nothing was written."; exit 1; }
fi

diskutil unmountDisk "/dev/$DISK" >/dev/null
echo "Writing (your Mac password is for sudo, to write the raw disk)..."
sudo dd if="$IMG" of="/dev/r$DISK" bs=4m
sync
diskutil eject "/dev/$DISK" >/dev/null && echo "Done. The card is ejected: put it in the Pi."
