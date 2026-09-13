#!/usr/bin/env python3
"""USB mass storage: a stick you plug in and can actually read and write.

The xHCI driver already enumerated mass-storage devices and printed their class
before this, which is a long way from being able to open a file on one. What it
was missing is everything between: two bulk endpoints configured, bulk-only
transport, and SCSI on top.

Four things are asserted, and the last is the only one that cannot be faked:

  1. The device is recognised as bulk-only SCSI and its endpoints configured.
     A device whose two bulk endpoints were never added to the slot context
     answers nothing at all, so the driver's own line naming the disk and its
     size is evidence the whole chain worked -- INQUIRY, TEST UNIT READY and
     READ CAPACITY are three round trips through CBW/data/CSW, and the size it
     prints came back over the wire.

  2. The size is right. READ CAPACITY(10) reports the LAST addressable block,
     not the count. Off by one there and the final sector of every USB disk is
     unreachable, which surfaces months later as a filesystem that breaks when
     it is nearly full. A 64 MiB image must read as 131072 blocks, not 131071.

  3. It joins the block layer like any other disk, so `diskinfo` lists it and
     everything built on blkdev_t -- the installer, fsck, the filesystems --
     can reach it without knowing what a TRB is.

  4. **Bytes written from inside the guest are in the host's file afterwards.**
     A FAT32 volume is created on the stick from the shell, a file is written
     to it, and then QEMU is shut down and the image is opened *here*, with a
     plain Python FAT reader that shares no code with the guest. Anything less
     than that -- reading the file back through the same driver that wrote it
     -- passes just as happily when the driver is writing to a buffer that
     never reaches the device.

The stick is also what proves the driver is not quietly destructive: a disk
with an unreadable filesystem is left alone at boot rather than formatted,
which is asserted by putting garbage on one and checking it survives.
"""
import os, re, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT

LOGIN_USER, LOGIN_PASS = "user", "bolt"
# 64 MiB, not 32: a legal FAT32 volume needs more than 65525 clusters, and a
# 32 MiB disk cannot reach that at any cluster size. The formatter refuses one,
# correctly, which is a limit of the format rather than of the driver.
STICK_MB = 64
STICK = os.path.join(ROOT, "test", "out", "usbstick.img")
JUNK = os.path.join(ROOT, "test", "out", "usbjunk.img")
CONTENT = "usb mass storage reached the host"


def make_stick(path, mb=STICK_MB, fill=b"\0"):
    with open(path, "wb") as fh:
        fh.write(fill * (mb * 1024 * 1024 // len(fill)))
    return path


def run(vm, line, wait_for, timeout=90):
    vm.clear()
    vm.type(line + "\n")
    assert vm.wait_for(wait_for, timeout=timeout), \
        "%r never printed %r:\n%s" % (line, wait_for, vm.serial()[-1200:])
    return vm.serial()


def read_fat_file(image, name):
    """Find a short-named file in the root directory of a FAT32 image and
    return its first cluster's worth of bytes.

    Deliberately a separate implementation from the guest's. It only has to
    handle the one volume the guest just made, so it walks the BPB, computes
    the root directory's first cluster and reads it -- no long names, no chains.
    """
    with open(image, "rb") as fh:
        boot = fh.read(512)
        assert boot[510:512] == b"\x55\xaa", "no boot signature on the stick"
        bps = struct.unpack("<H", boot[11:13])[0]
        spc = boot[13]
        rsvd = struct.unpack("<H", boot[14:16])[0]
        nfat = boot[16]
        fatsz = struct.unpack("<I", boot[36:40])[0]
        root_clus = struct.unpack("<I", boot[44:48])[0]
        assert bps == 512 and spc and fatsz, \
            "not a FAT32 BPB: bps=%s spc=%s fatsz=%s" % (bps, spc, fatsz)
        data_start = rsvd + nfat * fatsz

        def clus_off(c):
            return (data_start + (c - 2) * spc) * bps

        fh.seek(clus_off(root_clus))
        root = fh.read(spc * bps)
        want = name.ljust(11)[:11].upper().encode()
        for i in range(0, len(root), 32):
            e = root[i:i + 32]
            if not e or e[0] == 0:
                break
            if e[11] & 0x0F == 0x0F or e[0] == 0xE5:     # long name / deleted
                continue
            if e[0:11] != want:
                continue
            size = struct.unpack("<I", e[28:32])[0]
            clus = (struct.unpack("<H", e[20:22])[0] << 16) | \
                   struct.unpack("<H", e[26:28])[0]
            fh.seek(clus_off(clus))
            return fh.read(size)
    return None


def enumerate_and_size():
    make_stick(STICK)
    # snapshot=False: the whole point is that what the guest writes lands in
    # this file, where it can be read with something that is not BoltOS.
    with BoltVM(name="usbstor", net=False, snapshot=False,
                usb=["disk:" + STICK]) as vm:
        vm.sign_in(LOGIN_USER, LOGIN_PASS, timeout=200)
        log = vm.serial()
        vm.open_app("Terminal")

        m = re.search(r"\[xhci\] port \d+ slot \d+: mass-storage \S+ class 8", log)
        assert m, "the controller did not classify the stick:\n" + log[-1500:]

        m = re.search(r"\[ok\] USB disk (usb\d): (.+?), (\d+) MiB \((\d+) blocks", log)
        assert m, ("the mass-storage driver never claimed it -- bulk endpoints, "
                   "INQUIRY, TEST UNIT READY or READ CAPACITY failed:\n" + log[-1800:])
        name, model, mib, blocks = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
        print("  ok: %s claimed as %r, %d MiB" % (name, model, mib))

        want = STICK_MB * 1024 * 1024 // 512
        assert blocks == want, (
            "READ CAPACITY made a %d MiB stick %d blocks, not %d. That command "
            "reports the LAST addressable block, not the count -- an off-by-one "
            "here hides the final sector of every USB disk." % (STICK_MB, blocks, want))
        print("  ok: %d blocks, so the last-LBA/count conversion is right" % blocks)

        out = run(vm, "diskinfo", "block devices")
        assert re.search(r"\b" + name + r"\b", out), \
            "%s is not in the block layer:\n%s" % (name, out[-900:])
        print("  ok: `diskinfo` lists it beside the ATA and NVMe disks")

        out = run(vm, "usb", "usb0")
        assert "%d" % STICK_MB in out or "32.0M" in out, out[-600:]
        print("  ok: `usb` shows what is attached")

        # Write something, through FAT32, through the block layer, through BOT.
        # Without --yes it must refuse. This erases somebody's disk and there is
        # no undo, so the confirmation is a word rather than a prompt -- a y/n
        # prompt over SSH or inside a script is a command that hangs.
        out = run(vm, "usb format %s" % name, "usb:", timeout=120)
        assert "--yes" in out and "erase" in out, \
            "`usb format` without --yes should refuse and say why:\n" + out[-700:]
        print("  ok: formatting refuses until --yes is spelled out")

        out = run(vm, "usb format %s --yes BOLTUSB" % name, "usb:", timeout=240)
        assert "formatted" in out and "mounted" in out, \
            "could not format the stick:\n" + out[-800:]
        print("  ok: formatted and mounted a FAT32 volume on the stick")

        run(vm, 'fat write /HELLO.TXT "%s"' % CONTENT, "wrote /HELLO.TXT", timeout=90)
        run(vm, "usb umount", "unmounted", timeout=90)
        run(vm, "sync", "sync", timeout=120)
        time.sleep(1.5)
        vm.expect_absent("KERNEL PANIC")

    # QEMU is gone, so the image is settled. Read it with something that is not
    # BoltOS: if the driver had been writing into a buffer that never reached
    # the device, everything above would still have passed.
    got = read_fat_file(STICK, "HELLO   TXT")
    assert got is not None, \
        "the host's FAT reader cannot find HELLO.TXT on the stick -- nothing " \
        "the guest wrote reached the image"
    text = got.decode("latin-1").rstrip("\0").strip()
    assert CONTENT in text, \
        "the stick holds %r, not what was written" % text[:120]
    print("  ok: the host reads back what the guest wrote (%r)" % text[:48])


def a_disk_we_cannot_read_is_left_alone():
    """The destructive failure mode, checked explicitly.

    A stick with a filesystem this OS does not understand must come back
    untouched. Formatting an unreadable disk is the single worst thing an OS
    can do to somebody, and `blk_is_blank_scratch` reading it as empty would not
    make it any less their disk.
    """
    junk = make_stick(JUNK, mb=8, fill=b"\xa5" * 4096)
    before = open(junk, "rb").read(65536)
    with BoltVM(name="usbstor-junk", net=False, snapshot=False,
                usb=["disk:" + junk]) as vm:
        vm.sign_in(LOGIN_USER, LOGIN_PASS, timeout=200)
        vm.open_app("Terminal")
        out = run(vm, "usb", "usb")
        assert "usb0" in out, "an 8 MiB stick of junk should still be a disk:\n" + out[-600:]
        run(vm, "sync", "sync", timeout=120)
        vm.expect_absent("KERNEL PANIC")

    after = open(junk, "rb").read(65536)
    assert after == before, \
        "a stick with an unreadable filesystem was written to at boot"
    print("  ok: a stick it cannot read is left exactly as it was")


def main():
    enumerate_and_size()
    a_disk_we_cannot_read_is_left_alone()
    print("PASS usbstor: a USB disk enumerates, joins the block layer, and keeps what you write")
    return 0


if __name__ == "__main__":
    sys.exit(main())
