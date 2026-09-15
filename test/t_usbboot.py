#!/usr/bin/env python3
"""iso/boltusb.img boots off a USB stick, both ways, and the kernel arrives whole.

Everything else in this suite boots iso/os.img off an IDE disk, which is the one
route a physical stick never takes. That left the entire USB path -- an
enumerated controller, Bulk-Only Transport, INT 13h served out of the BIOS's own
USB driver -- untested, and it is where the interesting firmware bugs live: a
machine that got all the way to long mode and then jumped into a kernel that had
never arrived, with nothing on screen to say so.

Three things are checked here that nothing else covers:

  * the image has a partition table real firmware can read, and an ESP that
    really is a mountable FAT32 volume (asserted on the host, before QEMU);
  * legacy BIOS boots it from USB and stage2's own checksum says every byte of
    the kernel made it to 1 MiB;
  * UEFI boots the same image by finding that ESP itself, rather than by being
    handed esp/ as a virtual FAT volume the way t_uefi.py does.

Skips the UEFI half when no OVMF is installed, the way t_uefi.py does.
"""
import os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, find_ovmf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
USB = os.path.join(ROOT, "iso", "boltusb.img")
SECTOR = 512


# ------------------------------------------------------------ host checks ----
def check_image():
    """Read the layout back out of the image the way firmware would."""
    if not os.path.exists(USB):
        raise SystemExit("iso/boltusb.img missing -- run build.sh first")

    with open(USB, "rb") as fh:
        mbr = fh.read(SECTOR)

        assert mbr[510:512] == b"\x55\xAA", \
            "no 0x55AA boot signature; firmware will not treat this as bootable"
        assert mbr[440:444] != b"\0\0\0\0", \
            "MBR disk signature is zero; Windows offers to rewrite these"

        parts = []
        for i in range(4):
            e = mbr[446 + i * 16:446 + (i + 1) * 16]
            active, ptype = e[0], e[4]
            start, count = struct.unpack("<II", e[8:16])
            if ptype:
                parts.append((active, ptype, start, count))
        assert len(parts) == 2, \
            "expected an ESP and a data partition, found %d" % len(parts)

        esp_active, esp_type, esp_lba, esp_sect = parts[0]
        assert esp_type == 0x0C, \
            "partition 1 should be type 0x0C (FAT32 LBA), got 0x%02X" % esp_type
        assert esp_active == 0x80, \
            "partition 1 is not marked active; some BIOSes will not boot a USB-HDD"
        assert esp_lba % 2048 == 0, \
            "the ESP starts at LBA %d, which is not 1 MiB aligned" % esp_lba

        # The boot chain has to live entirely in the gap before partition 1,
        # or writing the ESP would land on top of the kernel.
        hdr = _read_at(fh, 33 * SECTOR, SECTOR)
        assert hdr[:8] == b"BOLTBHDR", "no BoltOS boot header at LBA 33"
        version, klba, ksect, kbytes, _flags, ksum = struct.unpack("<IIIIII", hdr[8:32])
        assert version == 2, "boot header should be v2 (carries kernel_sum), got %d" % version
        assert klba + ksect <= esp_lba, (
            "the raw kernel runs to LBA %d but the ESP starts at %d -- they overlap"
            % (klba + ksect, esp_lba))

        # ...and the sum in the header must describe what is actually on disk,
        # since that is the number stage2 refuses to boot without matching.
        data = _read_at(fh, klba * SECTOR, ksect * SECTOR)
        total = 0
        for (w,) in struct.iter_unpack("<I", data):
            total += w
        assert total & 0xFFFFFFFF == ksum, (
            "boot header says the kernel sums to 0x%08X, the image says 0x%08X"
            % (ksum, total & 0xFFFFFFFF))

        # The ESP must look like FAT32 to firmware that only reads the BPB.
        bpb = _read_at(fh, esp_lba * SECTOR, SECTOR)
        assert bpb[510:512] == b"\x55\xAA", "the ESP has no boot-sector signature"
        assert bpb[82:90] == b"FAT32   ", \
            "the ESP does not identify itself as FAT32: %r" % bpb[82:90]
        bps, spc, reserved, nfats = struct.unpack("<HBHB", bpb[11:17])
        assert (bps, nfats) == (512, 2), "unexpected FAT32 geometry %d/%d" % (bps, nfats)
        hidden, total32 = struct.unpack("<II", bpb[28:36])
        assert hidden == esp_lba, (
            "BPB hidden_sectors is %d but the partition starts at %d; firmware "
            "that trusts the BPB will read the wrong sectors" % (hidden, esp_lba))
        assert total32 == esp_sect, "BPB total sectors %d != partition %d" % (total32, esp_sect)

        fat_size, = struct.unpack("<I", bpb[36:40])
        clusters = (total32 - reserved - nfats * fat_size) // spc
        assert clusters >= 65525, (
            "%d clusters is below the 65525 floor, so this volume is really "
            "FAT16 wearing a FAT32 BPB" % clusters)

        # And BOOTX64.EFI has to be findable at the one path firmware looks at.
        names = _root_listing(fh, esp_lba, bpb)
        assert "EFI" in names, "no /EFI directory on the ESP: %s" % sorted(names)
        assert "KERNEL.BIN" in names, \
            "no /kernel.bin on the ESP -- BOOTX64.EFI would find nothing to load"

    print("  ok: MBR, boot header, and a real FAT32 ESP (%d MiB, %d clusters)"
          % (esp_sect // 2048, clusters))
    return kbytes


def _read_at(fh, off, n):
    fh.seek(off)
    return fh.read(n)


def _root_listing(fh, esp_lba, bpb):
    """8.3 names in the ESP's root directory cluster."""
    spc = bpb[13]
    reserved, nfats = struct.unpack("<HB", bpb[14:17])
    fat_size, = struct.unpack("<I", bpb[36:40])
    root_cluster, = struct.unpack("<I", bpb[44:48])
    data_lba = esp_lba + reserved + nfats * fat_size
    off = (data_lba + (root_cluster - 2) * spc) * SECTOR
    buf = _read_at(fh, off, spc * SECTOR)
    names = set()
    for i in range(0, len(buf), 32):
        e = buf[i:i + 32]
        if not e or e[0] in (0x00, 0xE5):
            continue
        if e[11] in (0x0F, 0x08):          # long-name fragment, or volume label
            continue
        stem = e[0:8].decode("ascii", "replace").strip()
        ext = e[8:11].decode("ascii", "replace").strip()
        names.add(stem + ("." + ext if ext else ""))
    return names


# -------------------------------------------------------------- boot runs ----
def boot_legacy():
    """Legacy BIOS, off an emulated USB mass-storage device."""
    with BoltVM(name="usbboot-bios", boot_img=USB, boot_bus="usb",
                disks=False, net=False, audio=None, memory="2G") as vm:
        # stage2 talks to COM1 now, so its side of the boot is assertable and
        # not just a thing that happened on a screen nobody recorded.
        vm.expect("BoltOS S2: stage 2 loaded", timeout=90)
        vm.expect("BoltOS S2: kernel at 1MiB", timeout=180)
        vm.expect("kernel verified: checksum matches", timeout=60)
        vm.expect("BoltOS is up", timeout=180)
        vm.expect_absent("KERNEL PANIC")
        vm.expect_absent("did not survive the trip")
    print("  ok: legacy BIOS booted it from USB, kernel checksum matched")


def boot_uefi():
    # The 8G is load-bearing, so do not trim it back to save a little host RAM.
    # Firmware allocates top-down, so with only 4 GiB every allocation the
    # loader makes -- and the loader image itself -- is forced below 4 GiB, and
    # a map that covers just 0..4 GiB looks correct. Give the machine more RAM
    # than that and BOOTX64.EFI is loaded above its own identity map, so `mov
    # cr3` faults on the next instruction fetch. That is what a real 8 GiB
    # laptop did on 2026-09-14 while this test sat green at 4G.
    with BoltVM(name="usbboot-efi", uefi=True, boot_img=USB, boot_bus="usb",
                disks=False, net=False, audio=None, memory="8G") as vm:
        # Slow for the reason t_uefi.py explains: uncached memory under OVMF.
        vm.expect("BoltOS kernel", timeout=180)
        vm.expect("BoltOS is up", timeout=300)
        vm.expect_absent("KERNEL PANIC")
        log = vm._buf

    # The E820 map the loader synthesises is written with a 24-byte stride to
    # match mm/pmm.c. When it was 20, every entry after the first was shifted
    # and the PMM read the wreckage as "3007 MiB managed, 0 MiB free" -- which
    # still reaches the desktop, so only the memory figures catch it.
    m = re.search(r"\[pmm\] (\d+) frames managed \((\d+) MiB\), (\d+) free \((\d+) MiB\)", log)
    assert m, "no [pmm] line on a UEFI boot:\n" + log[-1500:]
    managed_mib, free_mib = int(m.group(2)), int(m.group(4))
    assert free_mib > managed_mib // 2, (
        "UEFI boot came up with %d MiB free of %d MiB managed. The memory map "
        "handed to the kernel is being misread -- check that struct e820 in "
        "boot/uefi_boot.c is still 24 bytes wide." % (free_mib, managed_mib))
    print("  ok: UEFI found the ESP itself and booted it (%d MiB free of %d)"
          % (free_mib, managed_mib))


def main():
    kbytes = check_image()
    print("  kernel is %d bytes" % kbytes)
    boot_legacy()
    if find_ovmf()[0]:
        boot_uefi()
    else:
        print("  skip: no OVMF firmware, not testing the UEFI half")
    print("PASS usbboot: boltusb.img boots on legacy BIOS and UEFI from USB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
