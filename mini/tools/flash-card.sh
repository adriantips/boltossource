#!/usr/bin/env bash
# ===========================================================================
#  Write the BoltOS Mini card image to a microSD card in a reader on this Mac.
#
#    bash mini/tools/flash-card.sh disk4            (see: diskutil list external)
#    bash mini/tools/flash-card.sh disk4 path/to/sdcard.img
#
#  It refuses anything that is not an external disk, shows what it is about to
#  erase, and asks before writing. The write itself needs your Mac password
#  (sudo): macOS lets only root write to a raw disk.
# ===========================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DISK="${1:-}"
IMG="${2:-$ROOT/build/mini/sdcard.img}"

if [ -z "$DISK" ]; then
    echo "usage: bash mini/tools/flash-card.sh diskN [image]"
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
