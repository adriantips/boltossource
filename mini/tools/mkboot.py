#!/usr/bin/env python3
"""Make BoltBoot's firmware image for the Orange Pi Zero3 (mini/boot/).

    mkboot.py OUT --boot0 boot0.bin [--boot1 boot1.bin --boot1-addr 0x40000000]

The H618's boot ROM loads the first part, boot0, into SRAM if its eGON
header checks out: the magic "eGON.BT0", the length (a multiple of 8 KiB
here, as the ROM wants a multiple of 512), and a checksum -- the 32-bit sum of
every word of boot0, counted with the checksum field holding the stamp
0x5F0A6C39. boot0 is 48 KiB at most.

boot1 follows it on the card, right after boot0's last sector, behind a header
of its own that boot0 reads before loading it into DRAM:

    0x00  "BOLTBT1\\0"
    0x08  length of what follows the header, in bytes
    0x0C  sum of its 32-bit words
    0x10  load address (and entry)
    0x14  .. 0x1F  zero
    0x20  boot1

The whole file goes 8 KiB into the card, in front of the first partition
at 1 MiB (mkcard.py), so it must stay under 1016 KiB.
"""
import argparse, struct, sys

STAMP = 0x5F0A6C39
BOOT0_MAX = 0xC000
PAD = 8192
GAP = 1024 * 1024 - 8 * 1024


def word_sum(b):
    if len(b) % 4:
        b = b + bytes(4 - len(b) % 4)
    return sum(struct.unpack("<%dI" % (len(b) // 4), b)) & 0xFFFFFFFF


def egon(boot0):
    b = bytearray(boot0)
    if b[4:12] != b"eGON.BT0":
        raise SystemExit("boot0 has no eGON header at its start")
    n = (len(b) + PAD - 1) // PAD * PAD
    if n > BOOT0_MAX:
        raise SystemExit("boot0 is %d bytes: more than the %d the boot ROM loads" % (len(b), BOOT0_MAX))
    b += bytes(n - len(b))
    struct.pack_into("<II", b, 0x0C, STAMP, n)
    struct.pack_into("<I", b, 0x0C, word_sum(b))
    return bytes(b)


def check(img):
    """The boot ROM's test, for an image read back: True if it would run it."""
    if img[4:12] != b"eGON.BT0":
        return False
    n = struct.unpack_from("<I", img, 0x10)[0]
    if n == 0 or n % 512 or n > BOOT0_MAX or n > len(img):
        return False
    b = bytearray(img[:n])
    want = struct.unpack_from("<I", b, 0x0C)[0]
    struct.pack_into("<I", b, 0x0C, STAMP)
    return word_sum(b) == want


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out")
    ap.add_argument("--boot0", required=True)
    ap.add_argument("--boot1")
    ap.add_argument("--boot1-addr", type=lambda s: int(s, 0), default=0x40000000)
    a = ap.parse_args(argv)

    img = egon(open(a.boot0, "rb").read())
    assert check(img)
    boot0_len = len(img)
    if a.boot1:
        body = open(a.boot1, "rb").read()
        body += bytes((-len(body)) % 4)            # whole words: boot0 sums words
        hdr = b"BOLTBT1\0" + struct.pack("<III", len(body), word_sum(body), a.boot1_addr) + bytes(12)
        tail = hdr + body
        tail += bytes((-len(tail)) % 512)
        img += tail
    if len(img) > GAP:
        raise SystemExit("BoltBoot is %d bytes: more than the %d before the partition" % (len(img), GAP))
    open(a.out, "wb").write(img)
    print("%s: boot0 %d KiB%s" % (a.out, boot0_len >> 10,
          ", boot1 %d KiB" % ((len(img) - boot0_len) >> 10) if a.boot1 else ""))


if __name__ == "__main__":
    main(sys.argv[1:])
