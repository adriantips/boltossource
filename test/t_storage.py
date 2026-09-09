#!/usr/bin/env python3
"""Storage failure tests: real C with injected faults, then real QEMU disks.

All images are private files in test/out. Protection is checked by hashing the
whole image before and after a boot with snapshot=False; COW would hide writes.
Run just the injected-failure tests with `python test/t_storage.py --host-only`.
"""
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

from boltvm import BoltVM, ROOT

OUT = Path(ROOT) / "test" / "out"
SECTOR = 512
BASE = 2048
MARKER = b"existing-files-must-win\n"


def host_tests():
    cc = os.environ.get("BOLTOS_HOST_CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc and Path(r"C:\msys64\ucrt64\bin\clang.exe").exists():
        cc = r"C:\msys64\ucrt64\bin\clang.exe"
    if not cc:
        raise RuntimeError("host C compiler missing (set BOLTOS_HOST_CC)")
    exe = OUT / ("storage-host.exe" if os.name == "nt" else "storage-host")
    subprocess.run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-iquote", str(Path(ROOT) / "include"),
                    str(Path(ROOT) / "test" / "host" / "storage.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def fnv(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xffffffff
    return h


def blank(name, mb=8):
    path = OUT / ("storage-" + name + ".img")
    with path.open("wb") as f:
        f.truncate(mb * 1024 * 1024)
    return path


def patch(path, offset, data):
    with path.open("r+b") as f:
        f.seek(offset)
        f.write(data)


def mbr(path, kind):
    data = bytearray(SECTOR)
    data[446 + 4] = kind
    start = 1 if kind == 0xee else BASE
    struct.pack_into("<II", data, 446 + 8, start, path.stat().st_size // SECTOR - start)
    data[510:] = b"\x55\xaa"
    patch(path, 0, data)


def record(name, parent, directory=False, data=b""):
    return struct.pack("<IIIHHHH4xQ32s", parent, int(directory), len(data),
                       0o755 if directory else 0o644, 0, 0, 0, 0, name.encode()) + data


def boltfs(name, malformed=False, mirror=False):
    path = blank(name)
    mbr(path, 0xbf)
    half = (path.stat().st_size // SECTOR - BASE - 2) // 2
    data = b"".join([
        record("/", 0xffffffff, True), record("etc", 0, True),
        record("motd", 1, data=b"storage fixture\n"),
        record("home", 0, True), record("tmp", 0, True),
        record("keep", 0, data=MARKER),
    ])
    if malformed:
        data = bytearray(data)
        struct.pack_into("<I", data, 64, 1)  # parent is itself; checksum still correct
    sb = bytearray(struct.pack("<8sIIQIIQIIQII", b"BOLTFS2\0", 3, 0, 7, 6, 1,
                               len(data), fnv(data), half, 1, 0, 0))
    struct.pack_into("<I", sb, 56, fnv(sb[:56]))
    patch(path, (BASE + 2) * SECTOR, data)
    patch(path, BASE * SECTOR, sb)
    patch(path, (BASE + 1) * SECTOR, sb)
    if mirror:
        patch(path, BASE * SECTOR, b"TORN!!!!")
    return path


def digest(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def drive(tag, path, transport="nvme"):
    devices = {
        "nvme": f"nvme,drive={tag},serial=STORAGESAFE",
        "ata": f"ide-hd,drive={tag},bus=ide.1,unit=0",
        "ahci": f"ide-hd,drive={tag},bus=safeahci.0",
    }
    return tag, str(path), devices[transport]


def reach_shell(vm, recovery=False):
    for _ in range(16):
        if "boot mode" in vm.serial():
            break
        vm.key("3" if recovery else "2")
        time.sleep(0.4)
    vm.expect("BoltShell ready", timeout=180)
    vm.wait_prompt()
    vm.expect_absent("KERNEL PANIC")


def vm_for(name, disks, ahci=False):
    vm = BoltVM(name="storage-" + name, data_disks=disks,
                snapshot=False, net=False, extra=["-vga", "none"])
    if ahci:
        # QEMU must create the controller before the disks that name its bus.
        vm.args[1:1] = ["-device", "ich9-ahci,id=safeahci"]
    return vm


def protected_disks():
    foreign = blank("foreign")
    mbr(foreign, 0x83)
    raw = blank("raw")
    patch(raw, raw.stat().st_size - 19, b"keep-last-sector!!!")
    gpt = blank("gpt", mb=64)
    mbr(gpt, 0xee)
    patch(gpt, SECTOR, b"EFI PART")
    paths = [foreign, raw, gpt]
    before = [digest(p) for p in paths]
    disks = [drive("foreign", foreign), drive("raw", raw, "ata"), drive("gpt", gpt, "ahci")]
    with vm_for("foreign", disks, ahci=True) as vm:
        reach_shell(vm)
        assert "no eligible data disk" in vm.serial(), vm.serial()[-3000:]
        out = vm.run("sync")
        assert "RAM-only" in out, out
        vm.run("write /tmp/volatile safe")
        vm.expect_absent("[fat32] formatted")
    assert before == [digest(p) for p in paths], "boot modified unrelated disk contents"
    print("  ok: foreign MBR, protective GPT on SATA, and raw data remain byte-for-byte intact")

    damaged = boltfs("damaged", malformed=True)
    before = digest(damaged)
    with vm_for("damaged", [drive("damaged", damaged)]) as vm:
        reach_shell(vm)
        assert "cannot load saved filesystem" in vm.serial(), vm.serial()[-3000:]
        out = vm.run("cat /etc/motd")
        assert "Welcome to BoltOS" in out, "failed rebuild lost the seed tree: " + out
        out = vm.run("fsck")
        assert "structure   : FAILED" in out, out
        out = vm.run("fsck -r")
        assert "cannot repair" in out, out
        out = vm.run("sync")
        assert "read-only" in out, out
        out = vm.run("diskinfo")
        assert "changes are RAM-only" in out, out
    assert digest(damaged) == before, "a failed mount overwrote the saved image"
    print("  ok: a checksum-valid corrupt tree is diagnosed and protected from format, repair, and sync")


def existing_disk_and_mirror():
    saved = boltfs("saved", mirror=True)
    empty = blank("faster-empty")
    before = digest(empty)
    with vm_for("selection", [drive("saved", saved, "ata"), drive("empty", empty)]) as vm:
        reach_shell(vm)
        out = vm.run("cat /keep")
        assert MARKER.decode().strip() in out, out
        out = vm.run("fsck")
        assert "superblocks : 2 of 2 valid" in out and "structure   : ok" in out, out
        out = vm.run("sync")
        assert "committed FS" in out, out
    assert digest(empty) == before, "empty NVMe displaced the existing filesystem"
    print("  ok: an existing HDD beats empty NVMe and a torn primary recovers from its mirror")


def scratch_and_recovery():
    fresh = blank("fresh")
    sata = blank("recovery-sata", mb=64)
    disks = [drive("fresh", fresh), drive("sata", sata, "ahci")]
    with vm_for("fresh", disks, ahci=True) as vm:
        reach_shell(vm)
        assert "formatting" in vm.serial(), vm.serial()[-3000:]
        vm.run("write /keep saved-on-first-boot")
        out = vm.run("fat write /KEEP.TXT fat-before-recovery")
        assert "wrote /KEEP.TXT" in out, out
        out = vm.run("sync")
        assert "committed FS" in out, out
    before = digest(fresh)
    sata_before = digest(sata)
    with vm_for("recovery", disks, ahci=True) as vm:
        reach_shell(vm, recovery=True)
        out = vm.run("cat /keep")
        assert "saved-on-first-boot" in out, out
        vm.run("write /keep only-in-memory")
        out = vm.run("sync")
        assert "read-only" in out, out
        out = vm.run("fat write /KEEP.TXT changed")
        assert "write failed" in out, out
        out = vm.run("fat mkdir /DENIED")
        assert "mkdir failed" in out, out
        out = vm.run("fat cat /KEEP.TXT")
        assert "fat-before-recovery" in out, out
        vm.expect_absent("[fat32] formatted")
    assert digest(fresh) == before and digest(sata) == sata_before, "recovery wrote a disk"
    print("  ok: verified-empty scratch storage initialises once; recovery preserves every disk")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    host_tests()
    if "--host-only" not in sys.argv:
        protected_disks()
        existing_disk_and_mirror()
        scratch_and_recovery()
        print("PASS storage: real disk images survive failed mounts and recovery unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
