#!/usr/bin/env python3
"""Build a bootable ISO 9660 image with El Torito hard disk emulation."""

import sys
import struct
import pycdlib


def make_iso(img_path, iso_path, label="BoltOS"):
    with open(img_path, "rb") as f:
        img = bytearray(f.read())

    total_sectors = len(img) // 512

    entry = struct.pack(
        "<B 3s B 3s I I", 0x80, b"\x00\x01\x00", 0x06, b"\xfe\xff\xff", 0, total_sectors
    )

    img[0x1BE : 0x1BE + 16] = entry

    with open(img_path, "wb") as f:
        f.write(img)

    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=3, vol_ident=label)
    iso.add_file(img_path, "/OS.IMG;1")
    iso.add_eltorito("/OS.IMG;1", media_name="hdemul")
    iso.write(iso_path)
    iso.close()


if __name__ == "__main__":
    make_iso(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "BoltOS")
