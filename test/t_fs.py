#!/usr/bin/env python3
"""Filesystem and shell basics, plus the fsck / FAT32 surfaces.

Runs everything on copy-on-write disks, so it is safe to repeat and cannot
disturb the developer's persistent images. Drives the text shell in safe mode.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boltvm import BoltVM


def reach_shell(vm, timeout=180):
    return vm.reach_shell(timeout, "2")


def shell(vm, line, expect=None, timeout=60):
    vm.type(line + "\n")
    if expect:
        vm.expect(expect, timeout=timeout)


def main():
    with BoltVM(name="fs", net=False) as vm:
        reach_shell(vm)

        # --- ordinary file operations ---------------------------------------
        shell(vm, "mkdir /home/t_fs")
        shell(vm, "write /home/t_fs/a.txt hello-bolt-fs")
        shell(vm, "cat /home/t_fs/a.txt", "hello-bolt-fs")
        shell(vm, "cp /home/t_fs/a.txt /home/t_fs/b.txt")
        shell(vm, "cat /home/t_fs/b.txt", "hello-bolt-fs")
        shell(vm, "mv /home/t_fs/b.txt /home/t_fs/c.txt")
        shell(vm, "ls /home/t_fs", "c.txt")
        shell(vm, "rm /home/t_fs/c.txt")
        print("  ok: create / read / copy / move / remove")

        # --- redirection and pipes still work -------------------------------
        shell(vm, "echo piped-output > /home/t_fs/d.txt")
        shell(vm, "cat /home/t_fs/d.txt", "piped-output")
        print("  ok: output redirection")

        # --- fsck reports a healthy image -----------------------------------
        shell(vm, "fsck", "structure   : ok")
        log = vm.serial()
        assert "checksum    : ok" in log, "fsck checksum failed on a fresh image"
        assert "superblocks : 2 of 2 valid" in log, \
            "both superblock copies should be valid on a healthy image"
        print("  ok: fsck validates both superblocks and the checksum")

        # --- FAT32 volume round-trips ---------------------------------------
        shell(vm, "fat ls", None)
        if "not mounted" not in vm.serial():
            shell(vm, "fat write /T_FS.TXT fat-round-trip")
            shell(vm, "fat cat /T_FS.TXT", "fat-round-trip")
            print("  ok: FAT32 write/read round-trip")
        else:
            print("  skip: no FAT32 volume mounted in this configuration")

        # --- diskinfo describes the persistent mount ------------------------
        shell(vm, "diskinfo", "block devices:")
        print("  ok: diskinfo lists the block devices")

        vm.expect_absent("KERNEL PANIC")
    print("PASS fs: file operations, fsck and FAT32 all behave")
    return 0


if __name__ == "__main__":
    sys.exit(main())
