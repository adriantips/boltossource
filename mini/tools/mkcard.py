#!/usr/bin/env python3
"""Build the microSD card image for BoltOS Mini on an Orange Pi Zero3.

The H618's boot ROM looks for a boot program 8 KiB into the card, before any
partition, so the card is laid out the way every Allwinner card is:

    LBA 0           an MBR with one partition entry, no boot code
    8 KiB           u-boot-sunxi-with-spl.bin: SPL (DRAM), TF-A, U-Boot
    1 MiB           partition 1: FAT32 "BOLTOS"
                      /BOLTOS.BIN   the kernel, which U-Boot loads
                      /README.TXT   what this card is

Only the first part of the card is written; the rest of a 128 GB card is left
alone and unused. The MBR and FAT32 builders are mkusb.py's, the same ones the
desktop's USB stick is made with.

    mkcard.py OUT --uboot u-boot-sunxi-with-spl.bin [--kernel boltos.bin]
                  [--fat-mb 128]
"""
import argparse, os, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
import mkusb  # noqa: E402

SECTOR = 512
UBOOT_OFFSET = 8 * 1024
PART_LBA = 2048                      # 1 MiB

README = b"""BoltOS Mini for the Orange Pi Zero3
===================================

This card boots BoltOS Mini. U-Boot sits in front of this partition (8 KiB
into the card) and loads BOLTOS.BIN from here.

To put a new kernel on the card without taking it out of the Pi, plug the
Pi's USB-C port into the Mac and run:

    python3 mini/tools/pi.py install build/mini/boltos.bin

See MINI.md in the BoltOS repository.
"""


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out")
    ap.add_argument("--uboot", required=True)
    ap.add_argument("--kernel")
    ap.add_argument("--fat-mb", type=int, default=128)
    a = ap.parse_args(argv)

    uboot = open(a.uboot, "rb").read()
    if uboot[4:12] != b"eGON.BT0":
        raise SystemExit("%s has no eGON header: not an Allwinner boot image" % a.uboot)
    if UBOOT_OFFSET + len(uboot) > PART_LBA * SECTOR:
        raise SystemExit("U-Boot is %d bytes and runs into the partition at 1 MiB"
                         % len(uboot))

    files = {"README.TXT": README}
    if a.kernel:
        files["BOLTOS.BIN"] = open(a.kernel, "rb").read()

    fat_sectors = a.fat_mb * 1024 * 1024 // SECTOR
    fat = mkusb.build_esp(fat_sectors, PART_LBA, "BOLTOS", files, 0x424F4C54)

    mbr = bytearray(SECTOR)
    mbr[440:444] = struct.pack("<I", 0x4D494E49)          # "MINI"
    mbr[446:462] = mkusb.part_entry(0x80, mkusb.PART_ESP, PART_LBA, fat_sectors)
    mbr[510:512] = b"\x55\xAA"

    # Written beside the target and renamed into place, so a flash that
    # starts while a build is running reads the old image or the new one,
    # never half of each.
    tmp = a.out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(mbr)
        f.seek(UBOOT_OFFSET)
        f.write(uboot)
        f.seek(PART_LBA * SECTOR)
        f.write(fat)
    os.replace(tmp, a.out)
    size = os.path.getsize(a.out)
    print("%s: %d MiB (U-Boot %d KiB, FAT32 %d MiB%s)" % (
        a.out, size >> 20, len(uboot) >> 10, a.fat_mb,
        ", kernel %d KiB" % (len(files["BOLTOS.BIN"]) >> 10) if a.kernel else ", no kernel yet"))


if __name__ == "__main__":
    main(sys.argv[1:])
