#!/usr/bin/env python3
"""Build iso/boltusb.img -- a USB stick that boots on legacy BIOS *and* on UEFI.

iso/os.img is the minimal thing QEMU needs: stage1, stage2, a boot header and a
raw kernel, with no partition table at all. That is fine for `-drive file=...`
and it is the wrong shape for a physical stick. Real firmware decides how to
present a USB device by looking at its MBR, and a disk whose partition table is
64 zero bytes gets treated inconsistently -- some boards emulate it as a floppy,
some refuse it, and the ones that do boot it hand INT 13h a geometry they
invented. And a machine with no CSM at all cannot boot it under any reading.

So this lays out a stick the way the `install` command lays out a hard disk:

    LBA 0           stage1 + a real MBR partition table
    LBA 1..32       stage2
    LBA 33          BoltOS boot header (v2, carries the kernel's sum32)
    LBA 34..        kernel.bin, raw, in the gap before the first partition
    1 MiB-aligned   partition 1: FAT32 EFI system partition, active
                      /EFI/BOOT/BOOTX64.EFI   <- what UEFI firmware runs
                      /kernel.bin             <- what BOOTX64.EFI then loads
    1 MiB-aligned   partition 2: empty BoltFS partition, for `install`

Both paths end at the same kernel: legacy firmware runs stage1 out of the MBR
and stage2 pulls the raw copy in over INT 13h, while UEFI firmware ignores all
of that, finds the ESP, and runs BOOTX64.EFI, which reads its own copy through
the firmware's file protocol. The kernel is stored twice on purpose -- the raw
one cannot be seen by firmware, and the FAT one cannot be seen by stage2.

    mkusb.py OUT stage1.bin stage2.bin kernel.bin BOOTX64.EFI [--esp-mb N]
                                                              [--data-mb N]
"""
import argparse, os, struct, sys

import mkboothdr

SECTOR      = 512
MIB         = 1024 * 1024
MIB_SECTORS = MIB // SECTOR

STAGE2_LBA  = 1
STAGE2_MAX  = 32                 # sectors reserved for stage2, must match stage1
KERNEL_LBA  = 34

PART_ESP    = 0x0C               # FAT32 with LBA addressing
PART_BOLTFS = 0xBF               # matches BLK_PART_BOLTFS in include/blk.h


def align_mib(lba):
    return ((lba + MIB_SECTORS - 1) // MIB_SECTORS) * MIB_SECTORS


# ------------------------------------------------------------------- MBR -----
def part_entry(active, ptype, start, count):
    """One 16-byte MBR partition entry.

    The CHS fields are filled with the 0xFE/0xFF/0xFF "past the end of what CHS
    can say" pattern that every modern installer writes: nothing reads them, but
    leaving them zero makes some firmware decide the entry is malformed.
    """
    return (struct.pack("<B", active) + b"\xFE\xFF\xFF" +
            struct.pack("<B", ptype) + b"\xFE\xFF\xFF" +
            struct.pack("<II", start, min(count, 0xFFFFFFFF)))


def build_mbr(stage1, parts, disk_id):
    """stage1 with a partition table laid over the tail of its own sector.

    The same arrangement every MBR bootloader relies on, and the same one
    kernel/install.c writes: stage1's code and data stop well short of offset
    446, so the table and the 0x55AA signature go in space it does not use.
    That only holds as long as stage1 stays small, so it is checked rather than
    assumed -- growing stage1 into the table would otherwise produce a stick
    that boots and then reports a corrupt partition layout.
    """
    mbr = bytearray(SECTOR)
    if len(stage1) > SECTOR:
        raise SystemExit("stage1.bin is %d bytes; it has to be one sector"
                         % len(stage1))
    mbr[:len(stage1)] = stage1
    if any(mbr[440:510]):
        raise SystemExit(
            "stage1 has content at offset 440..509, where the disk signature "
            "and partition table go. It has outgrown the MBR; move code into "
            "stage2.")
    # Disk signature at 440. Windows in particular wants a non-zero one, and
    # will offer to write one itself if it finds zeros here.
    mbr[440:444] = struct.pack("<I", disk_id)
    for i, p in enumerate(parts):
        mbr[446 + i * 16:446 + (i + 1) * 16] = p
    mbr[510:512] = b"\x55\xAA"
    return bytes(mbr)


# ----------------------------------------------------------------- FAT32 -----
def fat32_geometry(total_sectors, spc=1, reserved=32, nfats=2):
    """Pick a FAT size that is consistent with the space left after it.

    fat_size depends on the cluster count, which depends on the space left after
    the FATs, which depends on fat_size. Converges in two or three passes.
    """
    fat_size = 1
    for _ in range(16):
        data = total_sectors - reserved - nfats * fat_size
        clusters = data // spc
        need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
        if need == fat_size:
            break
        fat_size = need
    data = total_sectors - reserved - nfats * fat_size
    clusters = data // spc
    if clusters < 65525:
        raise SystemExit(
            "an ESP of %d sectors only yields %d clusters; FAT32 is undefined "
            "below 65525. Give it more room (--esp-mb) or use bigger clusters."
            % (total_sectors, clusters))
    return spc, reserved, nfats, fat_size, clusters


def short_name(name):
    """'BOOTX64.EFI' -> the 11-byte padded 8.3 form a directory entry holds."""
    if "." in name:
        stem, ext = name.rsplit(".", 1)
    else:
        stem, ext = name, ""
    if len(stem) > 8 or len(ext) > 3:
        raise SystemExit("%r does not fit 8.3, and this builder writes no long "
                         "names" % name)
    return (stem.upper().ljust(8) + ext.upper().ljust(3)).encode("ascii")


def dir_entry(name, attr, cluster, size):
    return (short_name(name) + struct.pack("<B", attr) + b"\0" * 8 +
            struct.pack("<H", (cluster >> 16) & 0xFFFF) +
            b"\0" * 4 +
            struct.pack("<HI", cluster & 0xFFFF, size))


def raw_entry(name11, attr, cluster, size):
    """Like dir_entry but takes the 11 bytes verbatim, for '.' and '..'."""
    return (name11 + struct.pack("<B", attr) + b"\0" * 8 +
            struct.pack("<H", (cluster >> 16) & 0xFFFF) +
            b"\0" * 4 +
            struct.pack("<HI", cluster & 0xFFFF, size))


def build_esp(total_sectors, part_lba, label, files, volume_id):
    """Return `total_sectors * SECTOR` bytes of FAT32 holding `files`.

    `files` maps a path like "EFI/BOOT/BOOTX64.EFI" to its bytes. Only 8.3 names
    are supported, which is all the UEFI boot path needs.
    """
    spc, reserved, nfats, fat_size, clusters = fat32_geometry(total_sectors)
    cluster_bytes = spc * SECTOR

    # --- allocate clusters -------------------------------------------------
    # The FAT is built as a list of chains; cluster 0 and 1 are the reserved
    # media-descriptor entries, so real allocation starts at 2 with the root.
    fat = [0] * (clusters + 2)
    fat[0], fat[1] = 0x0FFFFFF8, 0x0FFFFFFF
    next_free = [2]
    content = {}                       # cluster -> bytes (one cluster's worth)

    def alloc_chain(data):
        """Copy `data` into a fresh cluster chain; return its first cluster."""
        n = max(1, (len(data) + cluster_bytes - 1) // cluster_bytes)
        if next_free[0] + n > clusters + 2:
            raise SystemExit("the ESP is too small for %d bytes of payload; "
                             "raise --esp-mb" % len(data))
        first = next_free[0]
        for i in range(n):
            c = first + i
            chunk = data[i * cluster_bytes:(i + 1) * cluster_bytes]
            content[c] = chunk.ljust(cluster_bytes, b"\0")
            fat[c] = (first + i + 1) if i < n - 1 else 0x0FFFFFFF
        next_free[0] += n
        return first

    # Build the directory tree depth-first, but the root must be cluster 2, so
    # reserve it before anything else is allocated.
    root_cluster = next_free[0]
    next_free[0] += 1
    fat[root_cluster] = 0x0FFFFFFF

    tree = {}                          # dir path -> {name: (attr, cluster, size)}
    tree[""] = {}
    for path, data in files.items():
        parts = path.split("/")
        parent = ""
        for d in parts[:-1]:
            here = (parent + "/" + d).lstrip("/")
            if here not in tree:
                c = next_free[0]
                next_free[0] += 1
                fat[c] = 0x0FFFFFFF
                tree[here] = {}
                tree[parent][d] = (0x10, c, 0)
            parent = here
        cl = alloc_chain(data)
        tree[parent][parts[-1]] = (0x20, cl, len(data))

    # --- render each directory into its (single) cluster --------------------
    dir_cluster = {"": root_cluster}
    for path in tree:
        if path:
            parent = path.rsplit("/", 1)[0] if "/" in path else ""
            dir_cluster[path] = tree[parent][path.rsplit("/", 1)[-1]][1]

    for path, entries in tree.items():
        buf = bytearray()
        if path == "":
            buf += raw_entry(label.upper().ljust(11).encode("ascii")[:11],
                             0x08, 0, 0)          # volume label
        else:
            parent = path.rsplit("/", 1)[0] if "/" in path else ""
            buf += raw_entry(b".          ", 0x10, dir_cluster[path], 0)
            # A '..' pointing at the root is written as cluster 0 by convention.
            up = dir_cluster[parent]
            buf += raw_entry(b"..         ", 0x10, 0 if up == root_cluster else up, 0)
        for name, (attr, cl, size) in entries.items():
            buf += dir_entry(name, attr, cl, size)
        if len(buf) > cluster_bytes:
            raise SystemExit("directory %r needs more than one cluster, which "
                             "this builder does not chain" % (path or "/"))
        content[dir_cluster[path]] = bytes(buf).ljust(cluster_bytes, b"\0")

    # --- boot sector --------------------------------------------------------
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xEB\x58\x90"                      # jmp short 0x5A; nop
    bs[3:11] = b"BOLTOS  "
    struct.pack_into("<HBHBHHBHHHII", bs, 11,
                     SECTOR,          # bytes per sector
                     spc,             # sectors per cluster
                     reserved,        # reserved sectors
                     nfats,           # number of FATs
                     0,               # root entries (0 on FAT32)
                     0,               # total sectors 16 (0 -> use the 32-bit one)
                     0xF8,            # media descriptor: fixed disk
                     0,               # FAT size 16 (0 on FAT32)
                     63,              # sectors per track
                     255,             # heads
                     part_lba,        # hidden sectors: where this partition starts
                     total_sectors)   # total sectors 32
    struct.pack_into("<IHHI", bs, 36, fat_size, 0, 0, root_cluster)
    struct.pack_into("<HH", bs, 48, 1, 6)          # FSInfo sector, backup boot sector
    bs[64] = 0x80                                  # drive number
    bs[66] = 0x29                                  # extended boot signature
    struct.pack_into("<I", bs, 67, volume_id)
    bs[71:82] = label.upper().ljust(11).encode("ascii")[:11]
    bs[82:90] = b"FAT32   "
    bs[510:512] = b"\x55\xAA"

    # --- FSInfo -------------------------------------------------------------
    fsi = bytearray(SECTOR)
    struct.pack_into("<I", fsi, 0, 0x41615252)
    struct.pack_into("<I", fsi, 484, 0x61417272)
    struct.pack_into("<II", fsi, 488,
                     clusters + 2 - next_free[0],  # free clusters
                     next_free[0])                 # next free hint
    fsi[508:512] = b"\x00\x00\x55\xAA"

    # --- assemble -----------------------------------------------------------
    out = bytearray(total_sectors * SECTOR)
    out[0:SECTOR] = bs
    out[SECTOR:2 * SECTOR] = fsi
    out[6 * SECTOR:7 * SECTOR] = bs                # backup boot sector
    out[7 * SECTOR:8 * SECTOR] = fsi

    fat_bytes = b"".join(struct.pack("<I", e) for e in fat)
    fat_bytes = fat_bytes.ljust(fat_size * SECTOR, b"\0")
    for i in range(nfats):
        off = (reserved + i * fat_size) * SECTOR
        out[off:off + len(fat_bytes)] = fat_bytes

    data_start = (reserved + nfats * fat_size) * SECTOR
    for cl, chunk in content.items():
        off = data_start + (cl - 2) * cluster_bytes
        out[off:off + len(chunk)] = chunk
    return bytes(out)


# ------------------------------------------------------------------ main -----
def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("stage1")
    ap.add_argument("stage2")
    ap.add_argument("kernel")
    ap.add_argument("bootx64")
    ap.add_argument("--esp-mb", type=int, default=64)
    ap.add_argument("--data-mb", type=int, default=32)
    a = ap.parse_args(argv[1:])

    stage1 = open(a.stage1, "rb").read()
    stage2 = open(a.stage2, "rb").read()
    kernel = open(a.kernel, "rb").read()
    efi    = open(a.bootx64, "rb").read()

    if len(stage2) > STAGE2_MAX * SECTOR:
        raise SystemExit("stage2 is %d bytes, over the %d reserved for it"
                         % (len(stage2), STAGE2_MAX * SECTOR))

    ksectors = (len(kernel) + SECTOR - 1) // SECTOR
    esp_lba  = align_mib(KERNEL_LBA + ksectors)
    esp_sect = a.esp_mb * MIB_SECTORS
    fs_lba   = align_mib(esp_lba + esp_sect)
    fs_sect  = a.data_mb * MIB_SECTORS
    total    = fs_lba + fs_sect

    # Derived from the kernel so a rebuild of the same tree gives the same
    # image; firmware only cares that it is not zero.
    disk_id = (0xB01D0000 ^ len(kernel)) & 0xFFFFFFFF

    esp = build_esp(esp_sect, esp_lba, "BOLTESP",
                    {"kernel.bin": kernel, "EFI/BOOT/BOOTX64.EFI": efi},
                    volume_id=disk_id)

    parts = [part_entry(0x80, PART_ESP,    esp_lba, esp_sect),
             part_entry(0x00, PART_BOLTFS, fs_lba,  fs_sect)]

    with open(a.out, "wb") as fh:
        fh.truncate(total * SECTOR)
        fh.write(build_mbr(stage1, parts, disk_id))
        fh.seek(STAGE2_LBA * SECTOR); fh.write(stage2)
        fh.seek(KERNEL_LBA * SECTOR); fh.write(kernel)
        fh.seek(esp_lba * SECTOR);    fh.write(esp)

    # The header goes in last, because it checksums the kernel as written.
    ksum = mkboothdr.kernel_sum32(a.out, KERNEL_LBA, ksectors)
    with open(a.out, "r+b") as fh:
        fh.seek(mkboothdr.HDR_LBA * SECTOR)
        fh.write(mkboothdr.build(KERNEL_LBA, len(kernel), ksum))

    print("      MBR + stage2 + header, kernel @LBA %d (%d sectors, sum32 0x%08X)"
          % (KERNEL_LBA, ksectors, ksum))
    print("      p1 ESP    FAT32 @LBA %-8d %4d MiB  (BOOTX64.EFI + kernel.bin)"
          % (esp_lba, esp_sect // MIB_SECTORS))
    print("      p2 BoltFS       @LBA %-8d %4d MiB  (empty)"
          % (fs_lba, fs_sect // MIB_SECTORS))
    print("OK -> %s (%d bytes)" % (a.out, total * SECTOR))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
