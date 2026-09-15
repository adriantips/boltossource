#!/usr/bin/env python3
"""Write the BoltOS boot header (one sector) into a disk image.

The header lives at LBA 33, between stage2 and the kernel, and tells stage2
where the kernel starts and how many sectors to pull in. Keeping that out of
stage2's own code is what makes stage2 independent of the kernel's size -- so
the kernel can carry a prebuilt copy of it and the `install` command can lay a
working boot chain onto a target disk from inside BoltOS.

The layout must stay byte-identical to `struct boot_header` in include/boot.h
and to the field offsets read by boot/stage2.asm's read_bhdr.

    mkboothdr.py IMAGE KERNEL_LBA KERNEL_BYTES
"""
import struct, sys

SECTOR = 512
HDR_LBA = 33
MAGIC = b"BOLTBHDR"
VERSION = 2


def kernel_sum32(image, kernel_lba, sectors):
    """Sum of every little-endian dword of the kernel as it sits on the disk.

    Taken over the padded sector count rather than the exact byte length, so
    stage2 can compute the same number over whole sectors without caring where
    the image stops inside the last one.
    """
    with open(image, "rb") as fh:
        fh.seek(kernel_lba * SECTOR)
        data = fh.read(sectors * SECTOR)
    if len(data) < sectors * SECTOR:
        data += b"\0" * (sectors * SECTOR - len(data))
    total = 0
    for (word,) in struct.iter_unpack("<I", data):
        total += word
    return total & 0xFFFFFFFF


def build(kernel_lba, kernel_bytes, kernel_sum):
    sectors = (kernel_bytes + SECTOR - 1) // SECTOR
    # magic[8] version kernel_lba kernel_sectors kernel_bytes flags kernel_sum
    # -- 32 bytes, then a sum32 over exactly those, matching include/boot.h.
    body = struct.pack("<8sIIIIII", MAGIC, VERSION, kernel_lba, sectors,
                       kernel_bytes, 0, kernel_sum)
    checksum = sum(body) & 0xFFFFFFFF
    return (body + struct.pack("<I", checksum)).ljust(SECTOR, b"\0")


def main(argv):
    if len(argv) != 4:
        raise SystemExit(__doc__.strip().splitlines()[-1].strip())
    image, kernel_lba, kernel_bytes = argv[1], int(argv[2]), int(argv[3])
    sectors = (kernel_bytes + SECTOR - 1) // SECTOR
    ksum = kernel_sum32(image, kernel_lba, sectors)
    hdr = build(kernel_lba, kernel_bytes, ksum)
    with open(image, "r+b") as fh:
        fh.seek(HDR_LBA * SECTOR)
        fh.write(hdr)
    print("      boot header @LBA %d -> kernel @LBA %d, %d bytes (%d sectors),"
          " sum32 0x%08X"
          % (HDR_LBA, kernel_lba, kernel_bytes, sectors, ksum))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
