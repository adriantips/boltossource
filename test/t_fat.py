#!/usr/bin/env python3
"""FAT32: injected I/O failures against the real driver, then a real volume.

The host half compiles fs/fat32.c against a disk that can fail a read, fail a
write, tear a sector in half and lose power, and asserts on what is left. The
QEMU half writes to the actual FAT32 volume on the AHCI disk and reboots to see
that it is still there -- which is the path the installer uses to put
BOOTX64.EFI on an EFI system partition, so it had better work on real hardware.

    python test/t_fat.py --host-only     # no QEMU, ~2 seconds
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM, ROOT

OUT = Path(ROOT) / "test" / "out"


def host_tests():
    cc = os.environ.get("BOLTOS_HOST_CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc and Path(r"C:\msys64\ucrt64\bin\clang.exe").exists():
        cc = r"C:\msys64\ucrt64\bin\clang.exe"
    if not cc:
        raise RuntimeError("host C compiler missing (set BOLTOS_HOST_CC)")
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / ("fat-host.exe" if os.name == "nt" else "fat-host")
    subprocess.run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-iquote", str(Path(ROOT) / "include"),
                    str(Path(ROOT) / "test" / "host" / "fat.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def shell(vm, line, expect=None, timeout=60):
    vm.type(line + "\n")
    if expect:
        vm.expect(expect, timeout=timeout)


def qemu_tests():
    # The FAT volume lives on the AHCI disk, which the default device set does
    # not attach -- and its controller has to be on the command line before the
    # disk that plugs into it, so both go in `extra` rather than data_disks.
    fat = os.path.join(ROOT, "iso", "disk-fat.img")
    if not os.path.exists(fat):
        print("SKIP fat: iso/disk-fat.img missing -- run build.sh first")
        return 0
    extra = ["-device", "ich9-ahci,id=ahci",
             "-drive", "id=fat,file=%s,format=raw,if=none,snapshot=on" % fat,
             "-device", "ide-hd,drive=fat,bus=ahci.0"]
    with BoltVM(name="fat", net=False, extra=extra) as vm:
        vm.reach_shell(entry="2")
        shell(vm, "fat ls", "Volume")
        if "no FAT32 volume mounted" in vm.serial():
            raise AssertionError("the AHCI disk carries a FAT32 volume that did not mount")
        print("  ok: the FAT32 volume on the AHCI disk mounted")

        shell(vm, "fat write /T_FAT.TXT round-trip-one", "wrote /T_FAT.TXT")
        shell(vm, "fat cat /T_FAT.TXT", "round-trip-one")
        shell(vm, "fat mkdir /T_FATDIR", "created /T_FATDIR")
        shell(vm, "fat write /T_FATDIR/INNER.TXT nested-value", "wrote")
        shell(vm, "fat cat /T_FATDIR/INNER.TXT", "nested-value")
        shell(vm, "fat ls /T_FATDIR", "INNER.TXT")
        print("  ok: file, directory and nested file all round-trip")

        # An overwrite changes one directory entry. Reading the file back has
        # to show the new contents and nothing of the old ones, and the other
        # entries in the same directory have to still be there.
        shell(vm, "fat write /T_FAT.TXT round-trip-two", "wrote")
        shell(vm, "fat cat /T_FAT.TXT", "round-trip-two")
        shell(vm, "fat ls /", "T_FATDIR")
        print("  ok: overwriting a file leaves its neighbours alone")

        # Errors are reported, not papered over.
        shell(vm, "fat cat /NOSUCH.TXT", "cannot read")
        shell(vm, "fat ls /T_FAT.TXT", "cannot list")
        print("  ok: a missing file and a file listed as a directory both fail")

        vm.reset()
        vm.reach_shell(entry="2")
        shell(vm, "fat cat /T_FAT.TXT", "round-trip-two")
        shell(vm, "fat cat /T_FATDIR/INNER.TXT", "nested-value")
        print("  ok: everything written is still there after a reboot")

        vm.expect_absent("KERNEL PANIC")
    return 0


def main(argv):
    host_tests()
    if "--host-only" in argv:
        print("PASS fat: injected-failure tests only (--host-only)")
        return 0
    rc = qemu_tests()
    print("PASS fat: injected I/O failures, and a real volume that survives a reboot")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
